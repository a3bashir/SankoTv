#include "MfMovieEncoder.h"

#include <QDir>
#include <QImage>
#include <QVector>

#include <cmath>
#include <cstdlib>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace sankoexport {

struct MfMovieEncoder::Writer
{
    ComPtr<IMFSinkWriter> sink;
    DWORD video = 0;
    DWORD audio = 0;
    bool hasAudio = false;
    qint64 audioTime = 0; // end of the last sound written, 100 ns units
};

namespace {

// A turn of the event loop may spend this long handing frames to Windows
// before the window gets to answer (repaint the progress, hear Cancel).
constexpr int kTurnMs = 15;

QString failure(const char *what, HRESULT hr)
{
    return QStringLiteral("Windows could not %1 (code 0x%2).")
        .arg(QLatin1String(what)).arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// Frame N starts at N / fps, in Media Foundation's 100 ns units - computed
// from the NUMBER every time. 1/24 s is not a whole number of those units;
// adding a rounded duration 86,400 times would drift, and this cannot.
// Measured: Windows writes each frame as exactly 1000 ticks of a timescale
// of fps x 1000, with zero error over an hour at 24 fps.
LONGLONG frameTime(qint64 index, int fps)
{
    return (index * 10000000LL + fps / 2) / fps;
}

LONGLONG sampleTime(qint64 frame, int rate)
{
    return (frame * 10000000LL + rate / 2) / rate;
}

// RGBX8888 -> NV12, BT.709, limited range (16-235 / 16-240): what a player
// assumes of an HD file, and what the file's colour tag then says it is.
// Chroma is the mean of each 2x2 block. Done once per panel, not per frame.
void toNv12(const QImage &rgb, uchar *dst)
{
    const int w = rgb.width(), h = rgb.height();
    uchar *luma = dst, *chroma = dst + qsizetype(w) * h;
    QVector<float> cb(qsizetype(w) * h), cr(qsizetype(w) * h);
    for (int y = 0; y < h; ++y) {
        const uchar *s = rgb.constScanLine(y);
        uchar *row = luma + qsizetype(y) * w;
        float *cbRow = cb.data() + qsizetype(y) * w, *crRow = cr.data() + qsizetype(y) * w;
        for (int x = 0; x < w; ++x) {
            const float r = s[x * 4], g = s[x * 4 + 1], b = s[x * 4 + 2];
            const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            row[x] = uchar(std::lround(16.0f + l * (219.0f / 255.0f)));
            cbRow[x] = 128.0f + (b - l) * (224.0f / 255.0f / 1.8556f);
            crRow[x] = 128.0f + (r - l) * (224.0f / 255.0f / 1.5748f);
        }
    }
    for (int y = 0; y < h / 2; ++y) {
        uchar *row = chroma + qsizetype(y) * w;
        for (int x = 0; x < w / 2; ++x) {
            const qsizetype i = qsizetype(2 * y) * w + 2 * x;
            row[2 * x] = uchar(std::lround((cb[i] + cb[i + 1] + cb[i + w] + cb[i + w + 1]) / 4));
            row[2 * x + 1] =
                uchar(std::lround((cr[i] + cr[i + 1] + cr[i + w] + cr[i + w + 1]) / 4));
        }
    }
}

void tagBt709(IMFMediaType *type)
{
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

// Windows' AAC encoder takes exactly these (bytes per second): 96, 128, 160
// and 192 kbit/s.
UINT32 aacBytesPerSecond(int bitRate)
{
    UINT32 best = 24000;
    for (UINT32 candidate : {12000u, 16000u, 20000u, 24000u})
        if (std::abs(int(candidate) * 8 - bitRate) < std::abs(int(best) * 8 - bitRate))
            best = candidate;
    return best;
}

} // namespace

QString MfMovieEncoder::unavailableReason()
{
    // By NAME, as data: nothing in these libraries runs, and none of the
    // delay-loaded imports is touched. An N edition without the Media
    // Feature Pack has none of the four.
    for (const wchar_t *name : {L"mfplat.dll", L"mfreadwrite.dll", L"mfh264enc.dll",
                                L"mfAACEnc.dll"}) {
        const HMODULE module = LoadLibraryExW(
            name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_AS_DATAFILE);
        if (!module)
            return mediaFeaturePackMessage();
        FreeLibrary(module);
    }
    return QString();
}

MfMovieEncoder::MfMovieEncoder(QObject *parent)
    : MovieEncoder(parent)
{
}

MfMovieEncoder::~MfMovieEncoder()
{
    release();
    if (m_started)
        MFShutdown();
}

void MfMovieEncoder::release()
{
    delete m_writer; // releases the sink writer, which closes the file
    m_writer = nullptr;
}

bool MfMovieEncoder::begin(const MovieSpec &spec, QString *error)
{
    auto fail = [&](const QString &text) {
        if (error)
            *error = text;
        release();
        m_finished = true; // nothing further will be reported
        return false;
    };
    m_spec = spec;
    const QString unavailable = unavailableReason();
    if (!unavailable.isEmpty())
        return fail(unavailable);
    const UINT32 w = UINT32(spec.frameSize.width()), h = UINT32(spec.frameSize.height());
    if (w == 0 || h == 0 || (w & 1) || (h & 1) || spec.fps <= 0)
        return fail(QStringLiteral("The film's frame size or rate cannot be encoded."));
    if (spec.audio
        && ((spec.audioSampleRate != 44100 && spec.audioSampleRate != 48000)
            || (spec.audioChannels != 1 && spec.audioChannels != 2)))
        return fail(QStringLiteral("The sound is in a form Windows' encoder does not "
                                   "take (%1 Hz, %2 channels).")
                        .arg(spec.audioSampleRate).arg(spec.audioChannels));

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
        return fail(failure("start its media components", hr));
    m_started = true;
    m_writer = new Writer;

#define SANKO_MF(call, what) \
    hr = (call); \
    if (FAILED(hr)) \
        return fail(failure(what, hr));

    // The software encoder, on purpose (see the header). Throttling is left
    // at its default: WriteSample waits when Windows has enough queued, so a
    // long film cannot pile up in memory.
    ComPtr<IMFAttributes> attributes;
    SANKO_MF(MFCreateAttributes(&attributes, 2), "prepare the export");
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    const QString native = QDir::toNativeSeparators(spec.path);
    SANKO_MF(MFCreateSinkWriterFromURL(reinterpret_cast<LPCWSTR>(native.utf16()), nullptr,
                                       attributes.Get(), &m_writer->sink),
             "create the file");

    // The picture, as it is written...
    ComPtr<IMFMediaType> out;
    SANKO_MF(MFCreateMediaType(&out), "prepare the export");
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    // A nominal figure the format requires; in quality mode the encoder
    // spends what the picture needs, not this.
    out->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, UINT32(spec.fps), 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    tagBt709(out.Get());
    SANKO_MF(m_writer->sink->AddStream(out.Get(), &m_writer->video), "set up the video");

    // ...and as it is handed over.
    ComPtr<IMFMediaType> in;
    SANKO_MF(MFCreateMediaType(&in), "prepare the export");
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, w);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, UINT32(spec.fps), 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    tagBt709(in.Get());
    // THE QUALITY SETTING. Without these Windows encodes at a constant bit
    // rate, and the first second of every panel is soft (measured: 41 dB
    // against 45 on line work, 34 against 40 on paint).
    ComPtr<IMFAttributes> settings;
    SANKO_MF(MFCreateAttributes(&settings, 4), "prepare the export");
    settings->SetUINT32(CODECAPI_AVEncCommonRateControlMode,
                        eAVEncCommonRateControlMode_Quality);
    settings->SetUINT32(CODECAPI_AVEncCommonQuality, 100);
    settings->SetUINT32(CODECAPI_AVEncMPVGOPSize, UINT32(spec.fps)); // a keyframe a second
    settings->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SANKO_MF(m_writer->sink->SetInputMediaType(m_writer->video, in.Get(), settings.Get()),
             "set up the video encoder");

    if (spec.audio) {
        const UINT32 rate = UINT32(spec.audioSampleRate), channels = UINT32(spec.audioChannels);
        ComPtr<IMFMediaType> aacType, pcmType;
        SANKO_MF(MFCreateMediaType(&aacType), "prepare the export");
        aacType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aacType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aacType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aacType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        aacType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        aacType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                           aacBytesPerSecond(spec.audioBitRate));
        aacType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        aacType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29); // AAC-LC
        SANKO_MF(m_writer->sink->AddStream(aacType.Get(), &m_writer->audio),
                 "set up the sound");
        SANKO_MF(MFCreateMediaType(&pcmType), "prepare the export");
        pcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        pcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        pcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        pcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
        SANKO_MF(m_writer->sink->SetInputMediaType(m_writer->audio, pcmType.Get(), nullptr),
                 "set up the sound encoder");
        m_writer->hasAudio = true;
    }
    SANKO_MF(m_writer->sink->BeginWriting(), "start the encoders");
#undef SANKO_MF

    nextTurn();
    return true;
}

// The calls into Windows block for as long as they take, so the job is
// offered work in TURNS: sound first (it may only go as far as the picture
// has), then frames for a few milliseconds, then back to the event loop.
// That is also what keeps picture and sound side by side in the file.
void MfMovieEncoder::nextTurn()
{
    if (m_turnQueued || m_finished)
        return;
    m_turnQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_turnQueued = false;
        if (m_finished || !m_writer)
            return;
        if (m_writer->hasAudio && !m_audioEnded)
            emit audioWanted();
        if (m_finished || !m_writer)
            return;
        m_framesThisTurn = 0;
        m_turn.restart();
        if (!m_videoEnded)
            emit videoWanted();
    }, Qt::QueuedConnection);
}

bool MfMovieEncoder::addFrame(const QImage &image, int index)
{
    if (m_finished || !m_writer || m_videoEnded)
        return false;
    if (m_framesThisTurn > 0 && m_turn.elapsed() >= kTurnMs) {
        nextTurn();
        return false;
    }
    if (image.size() != m_spec.frameSize || image.format() != QImage::Format_RGBX8888) {
        finish(false, QStringLiteral("A frame was not in the form the encoder takes."));
        return false;
    }
    const DWORD bytes = DWORD(image.width()) * DWORD(image.height()) * 3 / 2;
    if (image.cacheKey() != m_convertedKey) {
        m_nv12.resize(bytes);
        toNv12(image, reinterpret_cast<uchar *>(m_nv12.data()));
        m_convertedKey = image.cacheKey();
    }
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE *data = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buffer);
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(data, m_nv12.constData(), bytes);
        buffer->Unlock();
        hr = buffer->SetCurrentLength(bytes);
    }
    if (SUCCEEDED(hr))
        hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(buffer.Get());
    const LONGLONG start = frameTime(index, m_spec.fps);
    const LONGLONG end = frameTime(qint64(index) + 1, m_spec.fps);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleTime(start);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleDuration(end - start);
    if (SUCCEEDED(hr))
        hr = m_writer->sink->WriteSample(m_writer->video, sample.Get());
    if (FAILED(hr)) {
        finish(false, failure("write a frame of the film", hr));
        return false;
    }
    m_videoTime = end;
    ++m_framesThisTurn;
    return true;
}

bool MfMovieEncoder::endVideo()
{
    if (m_finished || !m_writer)
        return false;
    if (!m_videoEnded) {
        m_videoEnded = true;
        m_nv12.clear();
        m_writer->sink->NotifyEndOfSegment(m_writer->video);
        nextTurn(); // whatever sound is left may go now
        finalizeIfComplete();
    }
    return true;
}

bool MfMovieEncoder::addAudio(const QByteArray &pcm, qint64 firstSampleFrame)
{
    if (m_finished || !m_writer || !m_writer->hasAudio || m_audioEnded)
        return false;
    const int rate = m_spec.audioSampleRate;
    const LONGLONG start = sampleTime(firstSampleFrame, rate);
    // Never ahead of the picture: this is what interleaves the file. (The
    // next turn asks again.)
    if (!m_videoEnded && start >= m_videoTime)
        return false;
    const qint64 frames = pcm.size() / (m_spec.audioChannels * 2);
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE *data = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(DWORD(pcm.size()), &buffer);
    if (SUCCEEDED(hr))
        hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(data, pcm.constData(), size_t(pcm.size()));
        buffer->Unlock();
        hr = buffer->SetCurrentLength(DWORD(pcm.size()));
    }
    if (SUCCEEDED(hr))
        hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr))
        hr = sample->AddBuffer(buffer.Get());
    const LONGLONG end = sampleTime(firstSampleFrame + frames, rate);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleTime(start);
    if (SUCCEEDED(hr))
        hr = sample->SetSampleDuration(end - start);
    if (SUCCEEDED(hr))
        hr = m_writer->sink->WriteSample(m_writer->audio, sample.Get());
    if (FAILED(hr)) {
        finish(false, failure("write the film's sound", hr));
        return false;
    }
    m_writer->audioTime = end;
    return true;
}

bool MfMovieEncoder::endAudio()
{
    if (m_finished || !m_writer || !m_writer->hasAudio)
        return false;
    if (!m_audioEnded) {
        m_audioEnded = true;
        m_writer->sink->NotifyEndOfSegment(m_writer->audio);
        finalizeIfComplete();
    }
    return true;
}

void MfMovieEncoder::finalizeIfComplete()
{
    if (m_finished || !m_writer || !m_videoEnded
        || (m_writer->hasAudio && !m_audioEnded))
        return;
    const HRESULT hr = m_writer->sink->Finalize();
    release();
    if (FAILED(hr))
        finish(false, failure("finish the file", hr));
    else
        finish(true, QString());
}

void MfMovieEncoder::cancel()
{
    if (m_finished)
        return;
    release(); // no Finalize: the file is abandoned as it stands
    finish(false, QString());
}

void MfMovieEncoder::finish(bool ok, const QString &error)
{
    if (m_finished)
        return;
    m_finished = true;
    if (!ok)
        release();
    // Queued: this may be called from inside the job's own offer of a frame.
    QMetaObject::invokeMethod(this, [this, ok, error] { emit finished(ok, error); },
                              Qt::QueuedConnection);
}

} // namespace sankoexport
