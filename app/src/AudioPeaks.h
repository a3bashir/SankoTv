#pragma once

// THE SOUND'S PEAKS (Phase 2D, 2026-11-01; the Phase 2 spec's 5.1 step 1,
// Figma 2D-1 to 2D-5). The AUDIO row draws the track's waveform from a
// cache of 200 min / max peaks a second, read ONCE per file, OFF the UI
// thread, and kept in memory for the session - never in the project.
//
// No new library (CLAUDE.md rule 13): the samples come from Qt Multimedia's
// QAudioDecoder, already linked for playback and the MP4's audio, under the
// Qt licence the app has. The decoder lives on a worker thread of its own
// (PeakReader) and folds each buffer into peaks as it arrives, so the UI
// never waits; the finished peaks come back to the UI thread in one signal.
//
// A file is keyed by its path, size and modified time: a track replaced in
// its folder is read again; a track removed is forgotten.

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

class QThread;
class PeakReader;

namespace audiopeaks {

constexpr int kPerSecond = 200; // 5 ms a peak: one a column at zoom 800

struct PeakData
{
    QVector<qint16> maxes;
    QVector<qint16> mins;
    int sampleRate = 0;     // of the samples the peaks were folded from
    int sourceFormat = 0;   // QAudioFormat::SampleFormat the decoder gave (test evidence)
    int sourceChannels = 0;
    int count() const { return maxes.size(); }
    double seconds() const { return maxes.size() / double(kPerSecond); }
};

// THE ONE BUCKET ARITHMETIC: fold `frames` mono samples into `data`,
// continuing from `sampleIndex` (the running count of samples folded so
// far, advanced here); peak n holds samples [n * rate / 200, (n + 1) * rate
// / 200). The reader uses it on the decoder's buffers; the checks use it on
// the samples they generated, which is wf1's closed form.
void fold(PeakData &data, const qint16 *samples, int frames, int sampleRate,
          qint64 *sampleIndex);

} // namespace audiopeaks

Q_DECLARE_METATYPE(audiopeaks::PeakData)

class AudioPeaks : public QObject
{
    Q_OBJECT

public:
    // None: no track or not asked; Reading: the decoder is at it; Ready:
    // the peaks are in; Plain: the decoder could not read what the player
    // plays (a codec it lacks) - the row stays the plain bar, logged once.
    enum State { None, Reading, Ready, Plain };

    // serialForTest: the reader stays on the calling thread (the control of
    // the off-the-UI-thread check, wf3).
    explicit AudioPeaks(QObject *parent = nullptr, bool serialForTest = false);
    ~AudioPeaks() override;

    void request(const QString &path); // reads it unless already read
    void forget(const QString &path);
    State state(const QString &path) const;
    const audiopeaks::PeakData *peaks(const QString &path) const; // null unless Ready

    // Test hooks: buffers folded off the UI thread (the counter of wf3).
    static int foldedOffUiForTest();
    static void resetFoldedOffUiForTest();
    bool serialForTest() const { return m_serial; }

signals:
    void ready(const QString &path);
    void failed(const QString &path);
    void readRequested(const QString &path, const QString &key);

private:
    static QString keyFor(const QString &path);
    struct Entry
    {
        QString key;
        State state = None;
        audiopeaks::PeakData data;
    };
    QHash<QString, Entry> m_entries; // by path
    QThread *m_thread = nullptr;
    PeakReader *m_reader = nullptr;
    bool m_serial = false;
};
