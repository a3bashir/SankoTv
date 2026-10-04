#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

// THE SEAM BETWEEN "WHAT THE FILM IS" AND "WHO COMPRESSES IT".
//
// The export job (ProjectExport) knows the panels, their durations, the
// project's frame rate and the audio track; it does not know how an MP4 is
// made. An encoder knows how an MP4 is made and nothing about a project.
// This interface is all that passes between them, so the encoder can be
// replaced - by an Apple path on macOS, by a test that drops frames on
// purpose - without the job changing. It has been replaced once already:
// the first implementation drove Qt Multimedia's recorder, and was swapped
// for Media Foundation called directly (MfMovieEncoder) when the recorder
// turned out to write the sound at 16 kbit/s whatever it was asked
// (HANDOFF "Pass 4"). The job did not change.
//
// It is deliberately free of platform and Qt Multimedia types: a frame is a
// QImage and audio is 16-bit PCM bytes.
//
// PULL, NOT PUSH. The encoder says when it can take more (videoWanted /
// audioWanted); the job then offers frames until one is refused. An encoder
// whose calls simply block emits the signals from a queued call instead -
// the job cannot tell the difference, and the window keeps answering.
namespace sankoexport {

struct MovieSpec
{
    QString path;       // the file to write (the job's partial file)
    QSize frameSize;    // every frame offered is exactly this size
    int fps = 24;       // frame N is shown from N/fps to (N+1)/fps
    // Audio, when there is any: signed 16-bit interleaved PCM in, AAC-LC out
    // at audioBitRate. The job READS THE FILE BACK and refuses it if the
    // sound track is anything else.
    bool audio = false;
    int audioSampleRate = 48000;
    int audioChannels = 2;
    int audioBitRate = 192000;
};

class MovieEncoder : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // Open the file and the encoders. False (with a reason a person can
    // read) when that cannot even start; a failure discovered later arrives
    // as finished(false, reason).
    virtual bool begin(const MovieSpec &spec, QString *error) = 0;

    // Frame number `index`, in order, each exactly once. False = not now:
    // offer the SAME frame again after the next videoWanted().
    virtual bool addFrame(const QImage &frame, int index) = 0;
    // No more frames. False = not now, as above.
    virtual bool endVideo() = 0;

    // PCM starting at sample frame `firstSampleFrame` of the track. Same
    // refusal rule, on audioWanted(). Never called when spec.audio is false.
    virtual bool addAudio(const QByteArray &pcm, qint64 firstSampleFrame) = 0;
    virtual bool endAudio() = 0;

    // Stop now. finished(false, ...) still follows; whatever was written is
    // the job's to delete.
    virtual void cancel() = 0;

signals:
    void videoWanted();
    void audioWanted();
    // Exactly once. ok means the encoder believes the file is complete -
    // the job reads the file back before it believes it too.
    void finished(bool ok, const QString &error);
};

// The encoder this platform exports with (the caller owns it), or null
// where there is none yet. Today: Windows only.
MovieEncoder *createMovieEncoder(QObject *parent = nullptr);

// Empty when an H.264 + AAC MP4 can be written on this machine; otherwise
// the sentence to show the user. Asked BEFORE the file dialog, so nobody
// picks a name for a film that cannot be made - and before any encoder
// code runs: on a Windows "N" edition the libraries it needs are not there
// to be called.
QString movieEncoderUnavailableReason();

// What that says on Windows: where the Media Feature Pack is. Separate so
// the wording can be checked on a machine that HAS the encoders - no
// developer machine is an N edition.
QString mediaFeaturePackMessage();

} // namespace sankoexport
