#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QVector>

// READS BACK WHAT AN MP4 ACTUALLY CONTAINS: which tracks, how many samples
// in each, how long, and the video's frame timing. Not a player and not a
// validator - it walks the file's box structure and reads five small tables.
//
// Why the export needs it: an encoder reporting "done" is a claim. The old
// export never checked a single frame write, and a film with frames missing
// plays without complaint. The export counts the frames IN THE FILE against
// the frames it sent before it tells the artist the export succeeded.
namespace sankoexport {

struct Mp4Track
{
    QByteArray handler; // "vide" or "soun"
    QByteArray codec;   // "avc1", "mp4a", ...
    quint32 timescale = 0;
    quint64 duration = 0; // in timescale units
    quint32 samples = 0;  // frames, for a video track
    // Runs of (sample count, ticks per sample). One run = constant timing.
    QVector<QPair<quint32, quint32>> timing;
    quint64 bytes = 0; // the samples' sizes, summed: what the track weighs

    // A sound track: what kind of AAC, and what it was sampled at.
    int audioObjectType = 0; // 2 = AAC-LC, 5 = HE-AAC
    int sampleRate = 0;
    int channels = 0;
    // A picture track's colour tag, if the file carries one (1 = BT.709).
    int colourPrimaries = 0;
    int colourMatrix = 0;

    double seconds() const { return timescale ? double(duration) / timescale : 0.0; }
    // MEASURED, from the bytes and the length - not what a header claims.
    double bitsPerSecond() const
    {
        return seconds() > 0 ? double(bytes) * 8.0 / seconds() : 0.0;
    }
};

struct Mp4Info
{
    bool readable = false; // the file opened and has a movie box
    QVector<Mp4Track> tracks;

    const Mp4Track *video() const { return find("vide"); }
    const Mp4Track *audio() const { return find("soun"); }

private:
    const Mp4Track *find(const char *handler) const
    {
        for (const Mp4Track &t : tracks)
            if (t.handler == handler)
                return &t;
        return nullptr;
    }
};

Mp4Info readMp4(const QString &path);

} // namespace sankoexport
