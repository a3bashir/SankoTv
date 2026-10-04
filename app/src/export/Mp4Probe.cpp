#include "Mp4Probe.h"

#include <QFile>
#include <QtEndian>

namespace sankoexport {

namespace {

// Boxes that only contain other boxes, on the way down to a track's tables.
bool isContainer(const QByteArray &type)
{
    return type == "moov" || type == "trak" || type == "mdia" || type == "minf"
        || type == "stbl";
}

// A descriptor's length: up to four bytes, seven bits each.
quint32 descriptorLength(const QByteArray &d, qsizetype &at)
{
    quint32 length = 0;
    for (int i = 0; i < 4 && at < d.size(); ++i) {
        const quint8 byte = quint8(d[at++]);
        length = (length << 7) | (byte & 0x7f);
        if (!(byte & 0x80))
            break;
    }
    return length;
}

// What the sample description says about the stream itself. `stsd` is the
// box's body: 8 bytes, then the first entry (size, type, fields, children).
void readSampleEntry(const QByteArray &stsd, Mp4Track &track)
{
    const qsizetype entry = 8;
    if (stsd.size() < entry + 8)
        return;
    const qsizetype entryEnd =
        qMin<qsizetype>(stsd.size(), entry + qFromBigEndian<quint32>(stsd.constData() + entry));
    const bool sound = track.codec == "mp4a";
    // Fixed fields after the entry's own 8-byte header: 28 bytes of sound
    // description, 78 of picture description. Child boxes follow.
    qsizetype child = entry + 8 + (sound ? 28 : 78);
    if (sound && stsd.size() >= entry + 36) {
        track.channels = qFromBigEndian<quint16>(stsd.constData() + entry + 24);
        track.sampleRate = qFromBigEndian<quint32>(stsd.constData() + entry + 32) >> 16;
    }
    while (child + 8 <= entryEnd) {
        const quint32 size = qFromBigEndian<quint32>(stsd.constData() + child);
        if (size < 8 || child + qsizetype(size) > entryEnd)
            break;
        const QByteArray type = stsd.mid(child + 4, 4);
        if (type == "colr" && size >= 18 && stsd.mid(child + 8, 4) == "nclx") {
            // primaries, transfer, matrix: 1 / 1 / 1 is BT.709
            track.colourPrimaries = qFromBigEndian<quint16>(stsd.constData() + child + 12);
            track.colourMatrix = qFromBigEndian<quint16>(stsd.constData() + child + 16);
        } else if (type == "esds") {
            // ES descriptor > decoder config (object type, bit rates) >
            // decoder-specific info, whose first five bits are the AAC
            // audio object type: 2 is AAC-LC, 5 is HE-AAC.
            qsizetype at = child + 12;
            if (at < entryEnd && quint8(stsd[at++]) == 0x03) {
                descriptorLength(stsd, at);
                at += 3;
                if (at < entryEnd && quint8(stsd[at++]) == 0x04) {
                    descriptorLength(stsd, at);
                    at += 13; // type, stream, buffer size, max and average rate
                    if (at < entryEnd && quint8(stsd[at++]) == 0x05) {
                        descriptorLength(stsd, at);
                        if (at < entryEnd)
                            track.audioObjectType = quint8(stsd[at]) >> 3;
                    }
                }
            }
        }
        child += size;
    }
}

// The file is SEEKED through, never read whole: the picture data (mdat) is
// nearly all of it and none of our business.
void walk(QFile &file, qint64 from, qint64 to, Mp4Info &info, int depth)
{
    if (depth > 8)
        return;
    qint64 at = from;
    while (at + 8 <= to) {
        if (!file.seek(at))
            return;
        const QByteArray head = file.read(16);
        if (head.size() < 8)
            return;
        quint64 size = qFromBigEndian<quint32>(head.constData());
        const QByteArray type = head.mid(4, 4);
        qint64 header = 8;
        if (size == 1) { // 64-bit size follows the type
            if (head.size() < 16)
                return;
            size = qFromBigEndian<quint64>(head.constData() + 8);
            header = 16;
        } else if (size == 0) { // "to the end of the file"
            size = quint64(to - at);
        }
        if (size < quint64(header) || at + qint64(size) > to)
            return; // a box that claims more than there is: stop, do not guess
        const qint64 body = at + header, end = at + qint64(size);

        if (type == "moov")
            info.readable = true;
        if (type == "trak")
            info.tracks.append(Mp4Track());

        if (isContainer(type)) {
            walk(file, body, end, info, depth + 1);
        } else if (!info.tracks.isEmpty()
                   && (type == "mdhd" || type == "hdlr" || type == "stsd"
                       || type == "stsz" || type == "stts")) {
            Mp4Track &track = info.tracks.last();
            // The tables are small; the sample-size table is four bytes a
            // sample (an hour of sound is 0.7 MB) and is read whole, because
            // its SUM is the track's real bit rate. Caps keep a damaged size
            // field from turning into a huge read.
            qint64 cap = 64;
            if (type == "stts")
                cap = 8 + 8 * 4096;
            else if (type == "stsd")
                cap = 4096;
            else if (type == "stsz")
                cap = 16 * 1024 * 1024;
            const qint64 want = qMin<qint64>(end - body, cap);
            file.seek(body);
            const QByteArray data = file.read(want);
            const char *b = data.constData();
            const qsizetype n = data.size();
            if (type == "mdhd" && n >= 24) {
                if (quint8(b[0]) == 1 && n >= 32) {
                    track.timescale = qFromBigEndian<quint32>(b + 20);
                    track.duration = qFromBigEndian<quint64>(b + 24);
                } else {
                    track.timescale = qFromBigEndian<quint32>(b + 12);
                    track.duration = qFromBigEndian<quint32>(b + 16);
                }
            } else if (type == "hdlr" && n >= 12) {
                track.handler = data.mid(8, 4);
            } else if (type == "stsd" && n >= 16) {
                track.codec = data.mid(12, 4);
                readSampleEntry(data, track);
            } else if (type == "stsz" && n >= 12) {
                const quint32 fixed = qFromBigEndian<quint32>(b + 4);
                track.samples = qFromBigEndian<quint32>(b + 8);
                if (fixed) {
                    track.bytes = quint64(fixed) * track.samples;
                } else {
                    for (quint32 i = 0; i < track.samples && 16 + qsizetype(i) * 4 <= n; ++i)
                        track.bytes += qFromBigEndian<quint32>(b + 12 + i * 4);
                }
            } else if (type == "stts" && n >= 8) {
                const quint32 runs = qFromBigEndian<quint32>(b + 4);
                for (quint32 i = 0; i < runs && 16 + qsizetype(i) * 8 <= n; ++i)
                    track.timing.append(
                        {qFromBigEndian<quint32>(b + 8 + i * 8),
                         qFromBigEndian<quint32>(b + 12 + i * 8)});
            }
        }
        at = end;
    }
}

} // namespace

Mp4Info readMp4(const QString &path)
{
    Mp4Info info;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return info;
    walk(file, 0, file.size(), info, 0);
    return info;
}

} // namespace sankoexport
