#pragma once

#include "MovieEncoder.h"

#include <QElapsedTimer>

// THE MovieEncoder THAT SHIPS ON WINDOWS: Media Foundation's Sink Writer,
// called directly. Windows compresses the picture (H.264) and the sound
// (AAC-LC) with its own encoders and writes the MP4 container itself -
// nothing of ours and nothing of FFmpeg's is in the file's making.
//
// WHY NOT QT'S RECORDER, which was built first and measured (HANDOFF
// "Pass 4"): it reaches the same Windows encoders through FFmpeg's
// wrappers, and in the one mode that gives the approved picture it passes
// the AAC encoder no bit rate - the sound came out as HE-AAC at 16 kbit/s.
// It also stored 1080p in the BT.601 matrix with no colour tag, which every
// player reads as BT.709. Here both are set.
//
// THE SETTINGS, each measured in the probe before this was written
// (tests/_backups/export_probe_mf_20261003*):
// - H.264, quality mode at 100: the same encoder, mode and value the
//   approved picture was made with - same sizes to within 3 %, same
//   keyframes, equal or better fidelity at every step.
// - Baseline profile, a keyframe every second, no B-frames: as approved.
//   (High profile was accepted and changed neither size nor fidelity.)
// - The SOFTWARE encoder. The GPU's was measured (equal or better on one
//   card) and is deliberately not used: every vendor's differs, and only
//   one could be measured.
// - Colour: our own RGB -> NV12 conversion, BT.709, limited range, and the
//   file SAYS so (a colour tag in the container).
// - AAC-LC at the bit rate asked for (192 kbit/s), 16-bit PCM in.
//
// THE MEDIA FOUNDATION LIBRARIES ARE DELAY-LOADED (CMakeLists): a Windows
// "N" edition does not have them, and an import the loader cannot satisfy
// would stop the whole application from starting. Nothing here may be
// called unless unavailableReason() is empty.
namespace sankoexport {

class MfMovieEncoder : public MovieEncoder
{
    Q_OBJECT

public:
    explicit MfMovieEncoder(QObject *parent = nullptr);
    ~MfMovieEncoder() override;

    // Looks for the system libraries by NAME, without calling into them.
    static QString unavailableReason();

    bool begin(const MovieSpec &spec, QString *error) override;
    bool addFrame(const QImage &frame, int index) override;
    bool endVideo() override;
    bool addAudio(const QByteArray &pcm, qint64 firstSampleFrame) override;
    bool endAudio() override;
    void cancel() override;

private:
    struct Writer; // the COM objects, kept out of this header
    void nextTurn();
    void finalizeIfComplete();
    void finish(bool ok, const QString &error);
    void release();

    MovieSpec m_spec;
    Writer *m_writer = nullptr;
    bool m_started = false; // MFStartup succeeded
    bool m_finished = false;
    bool m_turnQueued = false;
    bool m_videoEnded = false;
    bool m_audioEnded = false;
    int m_framesThisTurn = 0;
    QElapsedTimer m_turn;
    qint64 m_videoTime = 0; // end of the last frame written, 100 ns units
    qint64 m_convertedKey = -1;
    QByteArray m_nv12; // the current panel, converted once
};

} // namespace sankoexport
