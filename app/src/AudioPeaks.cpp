#include "AudioPeaks.h"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <memory>

namespace audiopeaks {

void fold(PeakData &data, const qint16 *samples, int frames, int sampleRate,
          qint64 *sampleIndex)
{
    if (sampleRate <= 0 || frames <= 0)
        return;
    const int perPeak = qMax(1, sampleRate / kPerSecond);
    data.sampleRate = sampleRate;
    for (int i = 0; i < frames; ++i) {
        const int n = int(*sampleIndex / perPeak);
        if (n >= data.maxes.size()) {
            data.maxes.resize(n + 1);
            data.mins.resize(n + 1);
            data.maxes[n] = samples[i];
            data.mins[n] = samples[i];
        } else {
            if (samples[i] > data.maxes[n])
                data.maxes[n] = samples[i];
            if (samples[i] < data.mins[n])
                data.mins[n] = samples[i];
        }
        ++*sampleIndex;
    }
}

} // namespace audiopeaks

namespace {
std::atomic<int> g_foldedOffUi{0};

// A buffer in any format the decoder hands over, as mono Int16: channels
// averaged, floats scaled. The decoder is asked for mono Int16 and gives it
// for every file the gate reads; this is the guard for one that does not.
QVector<qint16> monoInt16(const QAudioBuffer &buffer)
{
    const QAudioFormat f = buffer.format();
    const int ch = qMax(1, f.channelCount());
    const int frames = buffer.frameCount();
    QVector<qint16> out(frames);
    if (f.sampleFormat() == QAudioFormat::Int16) {
        const qint16 *s = buffer.constData<qint16>();
        for (int i = 0; i < frames; ++i) {
            int sum = 0;
            for (int c = 0; c < ch; ++c)
                sum += s[i * ch + c];
            out[i] = qint16(sum / ch);
        }
    } else if (f.sampleFormat() == QAudioFormat::Float) {
        const float *s = buffer.constData<float>();
        for (int i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < ch; ++c)
                sum += s[i * ch + c];
            out[i] = qint16(qBound(-32768.0f, sum / ch * 32767.0f, 32767.0f));
        }
    } else if (f.sampleFormat() == QAudioFormat::Int32) {
        const qint32 *s = buffer.constData<qint32>();
        for (int i = 0; i < frames; ++i) {
            qint64 sum = 0;
            for (int c = 0; c < ch; ++c)
                sum += s[i * ch + c];
            out[i] = qint16((sum / ch) >> 16);
        }
    } else { // UInt8
        const quint8 *s = buffer.constData<quint8>();
        for (int i = 0; i < frames; ++i) {
            int sum = 0;
            for (int c = 0; c < ch; ++c)
                sum += int(s[i * ch + c]) - 128;
            out[i] = qint16((sum / ch) << 8);
        }
    }
    return out;
}
} // namespace

// THE READER: a QObject on the worker thread (or the caller's, serial).
// One decoder per request, deleted when it is done; the folded peaks go
// back by value in one signal.
class PeakReader : public QObject
{
    Q_OBJECT

public slots:
    void read(const QString &path, const QString &key)
    {
        auto *decoder = new QAudioDecoder(this);
        QAudioFormat wanted;
        wanted.setSampleRate(48000);
        wanted.setChannelCount(1);
        wanted.setSampleFormat(QAudioFormat::Int16);
        decoder->setAudioFormat(wanted);
        decoder->setSource(QUrl::fromLocalFile(path));
        auto data = std::make_shared<audiopeaks::PeakData>();
        auto index = std::make_shared<qint64>(0);
        auto done = std::make_shared<bool>(false);
        connect(decoder, &QAudioDecoder::bufferReady, this, [=] {
            while (decoder->bufferAvailable()) {
                const QAudioBuffer buffer = decoder->read();
                if (!buffer.isValid() || buffer.frameCount() <= 0)
                    continue;
                const QVector<qint16> mono = monoInt16(buffer);
                data->sourceFormat = int(buffer.format().sampleFormat());
                data->sourceChannels = buffer.format().channelCount();
                audiopeaks::fold(*data, mono.constData(), mono.size(),
                                 buffer.format().sampleRate(), index.get());
                if (QThread::currentThread() != QCoreApplication::instance()->thread())
                    g_foldedOffUi.fetch_add(1);
            }
        });
        connect(decoder, &QAudioDecoder::finished, this, [=] {
            if (*done)
                return;
            *done = true;
            if (data->count() > 0)
                emit finished(path, key, *data);
            else
                emit failed(path, key, QStringLiteral("no sound was decoded"));
            decoder->deleteLater();
        });
        connect(decoder, QOverload<QAudioDecoder::Error>::of(&QAudioDecoder::error), this,
                [=](QAudioDecoder::Error) {
                    if (*done)
                        return;
                    *done = true;
                    emit failed(path, key, decoder->errorString());
                    decoder->deleteLater();
                });
        decoder->start();
    }

signals:
    void finished(const QString &path, const QString &key, const audiopeaks::PeakData &data);
    void failed(const QString &path, const QString &key, const QString &why);
};

AudioPeaks::AudioPeaks(QObject *parent, bool serialForTest)
    : QObject(parent), m_serial(serialForTest)
{
    qRegisterMetaType<audiopeaks::PeakData>("audiopeaks::PeakData");
    m_reader = new PeakReader;
    if (!m_serial) {
        m_thread = new QThread(this);
        m_thread->setObjectName(QStringLiteral("SankoAudioPeaks"));
        m_reader->moveToThread(m_thread);
        m_thread->start();
    }
    connect(this, &AudioPeaks::readRequested, m_reader, &PeakReader::read);
    connect(m_reader, &PeakReader::finished, this,
            [this](const QString &path, const QString &key, const audiopeaks::PeakData &data) {
                auto it = m_entries.find(path);
                if (it == m_entries.end() || it->key != key)
                    return; // forgotten, or replaced, while it was read
                it->state = Ready;
                it->data = data;
                emit ready(path);
            });
    connect(m_reader, &PeakReader::failed, this,
            [this](const QString &path, const QString &key, const QString &why) {
                auto it = m_entries.find(path);
                if (it == m_entries.end() || it->key != key)
                    return;
                it->state = Plain;
                qWarning().noquote() << "AudioPeaks: no waveform for" << path << "-" << why;
                emit failed(path);
            });
}

AudioPeaks::~AudioPeaks()
{
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
    }
    delete m_reader;
}

QString AudioPeaks::keyFor(const QString &path)
{
    const QFileInfo info(path);
    return QStringLiteral("%1|%2|%3").arg(info.absoluteFilePath()).arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

void AudioPeaks::request(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    const QString key = keyFor(path);
    auto it = m_entries.find(path);
    if (it != m_entries.end() && it->key == key && it->state != None)
        return; // read, being read, or found unreadable: as it is
    Entry entry;
    entry.key = key;
    entry.state = Reading;
    m_entries.insert(path, entry);
    emit readRequested(path, key);
}

void AudioPeaks::forget(const QString &path)
{
    m_entries.remove(path);
}

AudioPeaks::State AudioPeaks::state(const QString &path) const
{
    auto it = m_entries.constFind(path);
    return it == m_entries.constEnd() ? None : it->state;
}

const audiopeaks::PeakData *AudioPeaks::peaks(const QString &path) const
{
    auto it = m_entries.constFind(path);
    return (it != m_entries.constEnd() && it->state == Ready) ? &it->data : nullptr;
}

int AudioPeaks::foldedOffUiForTest()
{
    return g_foldedOffUi.load();
}

void AudioPeaks::resetFoldedOffUiForTest()
{
    g_foldedOffUi = 0;
}

#include "AudioPeaks.moc"
