#include "AbrImporter.h"

#include "BrushPresetCodec.h"

#include <QCryptographicHash>
#include <QImage>
#include <QStringDecoder>
#include <QTextStream>
#include <QTransform>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>

// ============================================================================
// Photoshop .abr — format notes and the fidelity contract
// ============================================================================
//
// Two families are supported, detected from the first two big-endian shorts:
//
//   LEGACY v1/v2   short version (1 or 2), short brushCount, then brushCount
//                  records of {short type, long byteLength, payload}. Only
//                  type 2 (sampled) carries a bitmap; type 1 (computed) has
//                  an undocumented payload and is reported as dropped, never
//                  guessed at. v2 sampled records embed a UTF-16BE name.
//
//   v6+ (6/7/9/10, subversion 1 or 2 — the modern format; Photoshop CS
//   through CC all write this layout) —
//                  short version, short subversion, then 8BIM sections:
//                  "8BIM" + 4cc tag + long length + payload. 'samp' holds
//                  the sampled tip bitmaps (one length-prefixed record per
//                  tip, 4-byte aligned); 'desc' holds a standard Photoshop
//                  descriptor (the same structure PSDs use) with one 'Brsh'
//                  list entry per brush carrying name, spacing, dynamics,
//                  scatter and texture settings; 'patt' holds texture
//                  patterns as virtual-memory arrays. The format is
//                  undocumented — this layout follows the Krita and GIMP
//                  reverse engineering (gimp: app/core/gimpbrush-load.c,
//                  krita: kis_abr_brush_collection.cpp), with the descriptor
//                  and pattern structures from the published PSD spec.
//
// Sampled tips are 8- or 16-bit grayscale coverage masks (255 = full paint —
// the same polarity as the engine's tips; verified empirically against
// Photoshop renders of the test files), raw or PackBits-RLE per scanline.
//
// DEFENSIVE PARSING. These files come from the internet: every length,
// offset, count and dimension is treated as hostile. All reads go through a
// bounds-checked big-endian reader that fails sticky instead of running past
// the buffer; declared sizes are capped before any allocation (a tip may not
// exceed Photoshop's own 5000 px limit); RLE can never write outside its
// scanline; a malformed brush is skipped WITH a per-brush error while the
// rest of the file still imports; the descriptor parser caps recursion depth
// and node count. A clear refusal is always acceptable; a crash never is.
//
// THE MAPPING TABLE (also emitted per brush in the import report):
//   mapped        tip bitmap -> customShape UNTRANSFORMED (letterboxed
//                 into the square, white-is-paint convention the roster's
//                 tips use; tips larger than the engine's 2048 px ceiling
//                 are downscaled); the STATIC tip transform (angle,
//                 roundness, flip X/Y) onto the engine's own tip
//                 parameters; name; spacing; diameter; size/angle/roundness
//                 jitter; scatter amount, axes and count; texture pattern
//                 -> custom grain (+ scale, depth, inversion).
//                 Photoshop's control SOURCE per dynamic onto the engine's
//                 ControlSource (off -> None, fade -> Fade, pen pressure ->
//                 Pressure, pen tilt -> Tilt, direction -> Direction), its
//                 minimum percentage onto the engine's per-property
//                 minimum, and the fade STEP count onto the brush's
//                 fadeDistance in canvas pixels.
//   approximated  a computed (non-sampled) v6 tip becomes the engine's
//                 PROCEDURAL tip carrying the descriptor's hardness;
//                 "initial direction" becomes the continuously-tracked
//                 Direction; several fade lengths on one brush collapse to
//                 the longest (the engine has one fade distance per brush).
//   dropped       wet edges; build-up/airbrush accumulation; dual brush
//                 (Photoshop composites differently); colour dynamics;
//                 noise; bristle and erodible tip simulation; and any
//                 control source with no engine equivalent — stylus wheel
//                 and the two rotation sources (recorded, the property left
//                 at its base value, never silently substituted).
//
// A dynamic Photoshop does not drive is imported as source None, not as a
// flat curve on the default Pressure source: both render identically (the
// resolver returns 1.0 either way), but only None states honestly in the
// studio that the brush has no such response.
// ============================================================================

namespace brushlib {
namespace {

// ---- Hostile-input limits --------------------------------------------------
constexpr int kMaxTipDim = 5000;          // Photoshop's own tip ceiling
constexpr int kEngineTipMax = 2048;       // Brush::setSize clamp
constexpr int kMaxBrushesPerFile = 4096;
constexpr int kMaxPatterns = 512;
constexpr int kDescMaxDepth = 32;
constexpr int kDescMaxNodes = 200000;
constexpr qsizetype kMaxPatternDim = 10000;

// ---- Bounded big-endian reader ---------------------------------------------
// Every read is range-checked; a failed read latches ok() false and returns
// zero/empty, so parse loops terminate on the flag instead of running past
// the end of hostile input.
class ByteReader
{
public:
    explicit ByteReader(const QByteArray &bytes)
        : m_d(reinterpret_cast<const uchar *>(bytes.constData()))
        , m_n(bytes.size())
    {
    }

    bool ok() const { return m_ok; }
    void fail() { m_ok = false; }
    qsizetype tell() const { return m_pos; }
    qsizetype size() const { return m_n; }
    qsizetype remaining() const { return m_ok ? m_n - m_pos : 0; }

    bool seek(qsizetype pos)
    {
        if (!m_ok || pos < 0 || pos > m_n) {
            m_ok = false;
            return false;
        }
        m_pos = pos;
        return true;
    }
    bool skip(qsizetype count) { return seek(m_pos + count); }

    quint8 u8()
    {
        if (!require(1))
            return 0;
        return m_d[m_pos++];
    }
    quint16 u16()
    {
        if (!require(2))
            return 0;
        const quint16 v = quint16(m_d[m_pos]) << 8 | m_d[m_pos + 1];
        m_pos += 2;
        return v;
    }
    quint32 u32()
    {
        if (!require(4))
            return 0;
        const quint32 v = quint32(m_d[m_pos]) << 24
            | quint32(m_d[m_pos + 1]) << 16 | quint32(m_d[m_pos + 2]) << 8
            | m_d[m_pos + 3];
        m_pos += 4;
        return v;
    }
    qint64 s64()
    {
        const quint64 hi = u32(), lo = u32();
        return qint64(hi << 32 | lo);
    }
    double f64()
    {
        if (!require(8))
            return 0.0;
        quint64 v = 0;
        for (int i = 0; i < 8; ++i)
            v = v << 8 | m_d[m_pos + i];
        m_pos += 8;
        double out;
        static_assert(sizeof(out) == sizeof(v));
        std::memcpy(&out, &v, sizeof(out));
        return std::isfinite(out) ? out : 0.0;
    }
    QByteArray bytes(qsizetype count)
    {
        if (count < 0 || !require(count))
            return QByteArray();
        QByteArray out(reinterpret_cast<const char *>(m_d + m_pos),
                       count);
        m_pos += count;
        return out;
    }
    const uchar *peek(qsizetype count) const
    {
        if (!m_ok || count < 0 || m_pos + count > m_n)
            return nullptr;
        return m_d + m_pos;
    }

    // Long-prefixed UTF-16BE string (the descriptor/name convention). The
    // declared character count is capped before any allocation.
    QString unicodeString(qsizetype maxChars = 4096)
    {
        const quint32 chars = u32();
        if (!m_ok || qsizetype(chars) > maxChars
            || !require(qsizetype(chars) * 2)) {
            m_ok = false;
            return QString();
        }
        QStringDecoder decode(QStringDecoder::Utf16BE);
        QString s = decode(
            QByteArrayView(reinterpret_cast<const char *>(m_d + m_pos),
                           qsizetype(chars) * 2));
        m_pos += qsizetype(chars) * 2;
        // Photoshop usually includes the terminating NUL in the count.
        while (s.endsWith(QChar(0)))
            s.chop(1);
        return s;
    }

    // Descriptor key: long length, 0 meaning "exactly 4 bytes".
    QString descKey()
    {
        quint32 len = u32();
        if (len == 0)
            len = 4;
        if (!m_ok || len > 1024) {
            m_ok = false;
            return QString();
        }
        return QString::fromLatin1(bytes(qsizetype(len)));
    }

private:
    bool require(qsizetype count)
    {
        if (!m_ok || count < 0 || m_pos + count > m_n) {
            m_ok = false;
            return false;
        }
        return true;
    }

    const uchar *m_d;
    qsizetype m_n;
    qsizetype m_pos = 0;
    bool m_ok = true;
};

// ---- PackBits --------------------------------------------------------------
// Decodes exactly srcLen source bytes into exactly dstLen output bytes.
// Never writes outside dst, never reads outside the declared source window;
// any mismatch fails the scanline (and with it, that one brush — not the
// file, and never the process).
bool unpackBits(ByteReader &r, qsizetype srcLen, uchar *dst, qsizetype dstLen)
{
    const qsizetype srcEnd = r.tell() + srcLen;
    qsizetype out = 0;
    while (out < dstLen && r.tell() < srcEnd && r.ok()) {
        const qint8 n = qint8(r.u8());
        if (n >= 0) {
            const qsizetype count = qsizetype(n) + 1;
            if (out + count > dstLen || r.tell() + count > srcEnd)
                return false;
            const QByteArray lit = r.bytes(count);
            if (!r.ok())
                return false;
            std::memcpy(dst + out, lit.constData(), size_t(count));
            out += count;
        } else if (n != -128) {
            const qsizetype count = 1 - qsizetype(n);
            if (out + count > dstLen)
                return false;
            const uchar v = r.u8();
            if (!r.ok())
                return false;
            std::memset(dst + out, v, size_t(count));
            out += count;
        }
    }
    if (out != dstLen)
        return false;
    return r.seek(srcEnd); // a short scanline's residue is skipped, bounded
}

// ---- Sampled-bitmap decode (shared by legacy and v6) -----------------------
// Reads {bounds already parsed by the caller} depth/compression/data into a
// Grayscale8 mask. Fails (returning a null image + reason) instead of ever
// allocating what a hostile header declares.
QImage readMaskData(ByteReader &r, int width, int height, int depth,
                    int compression, QString *why)
{
    if (width < 1 || height < 1 || width > kMaxTipDim
        || height > kMaxTipDim) {
        *why = QStringLiteral("tip dimensions %1x%2 out of range (1..%3)")
                   .arg(width).arg(height).arg(kMaxTipDim);
        return QImage();
    }
    if (depth != 8 && depth != 16) {
        *why = QStringLiteral("unsupported tip depth %1 (8/16 only)")
                   .arg(depth);
        return QImage();
    }
    if (compression != 0 && compression != 1) {
        *why = QStringLiteral("unsupported tip compression %1")
                   .arg(compression);
        return QImage();
    }
    const qsizetype rowBytes = qsizetype(width) * (depth / 8);
    QByteArray raw(rowBytes * height, Qt::Uninitialized);
    if (compression == 0) {
        const QByteArray data = r.bytes(raw.size());
        if (!r.ok()) {
            *why = QStringLiteral("unexpected EOF in tip data");
            return QImage();
        }
        raw = data;
    } else {
        // RLE: per-scanline byte counts first, then PackBits per scanline.
        QVector<quint16> lens(height);
        for (int y = 0; y < height; ++y)
            lens[y] = r.u16();
        if (!r.ok()) {
            *why = QStringLiteral("unexpected EOF in RLE scanline table");
            return QImage();
        }
        for (int y = 0; y < height; ++y) {
            if (!unpackBits(r, lens[y],
                            reinterpret_cast<uchar *>(raw.data())
                                + qsizetype(y) * rowBytes,
                            rowBytes)) {
                *why = QStringLiteral("corrupt RLE data at scanline %1")
                           .arg(y);
                return QImage();
            }
        }
    }
    QImage mask(width, height, QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y) {
        uchar *dst = mask.scanLine(y);
        const uchar *src = reinterpret_cast<const uchar *>(raw.constData())
            + qsizetype(y) * rowBytes;
        if (depth == 8)
            std::memcpy(dst, src, size_t(width));
        else // 16-bit big-endian: the high byte carries the coverage
            for (int x = 0; x < width; ++x)
                dst[x] = src[x * 2];
    }
    return mask;
}

// ---- Parsed sample tip -----------------------------------------------------
struct AbrSample
{
    QString uuid;        // v6 pairing key (may be empty)
    QImage mask;         // Grayscale8, white = paint
    int depth = 8;
    int spacingPercent = -1; // legacy carries spacing inline; -1 = absent
    QString legacyName;      // v2 only
    QString error;           // non-empty: this one brush failed
};

// ---- Legacy v1/v2 ----------------------------------------------------------
QVector<AbrSample> parseLegacy(ByteReader &r, int version, int count,
                               QStringList *fileNotes)
{
    QVector<AbrSample> out;
    for (int i = 0; i < count && r.ok(); ++i) {
        const int type = r.u16();
        const quint32 size = r.u32();
        if (!r.ok())
            break;
        const qsizetype end = r.tell() + qsizetype(size);
        if (end > r.size()) {
            AbrSample bad;
            bad.error = QStringLiteral(
                "brush %1 declares %2 bytes but the file ends first")
                    .arg(i + 1).arg(size);
            out.append(bad);
            break;
        }
        AbrSample s;
        if (type == 1) {
            // Computed brush: the legacy payload layout is undocumented —
            // report it dropped rather than guess at a broken import.
            s.error = QStringLiteral(
                "computed (non-sampled) brush in a legacy file — layout is "
                "undocumented, dropped");
        } else if (type != 2) {
            s.error = QStringLiteral("unknown brush type %1 — dropped")
                          .arg(type);
        } else {
            r.u32(); // misc
            s.spacingPercent = r.u16();
            if (version == 2)
                s.legacyName = r.unicodeString();
            r.u8();  // antialiasing
            for (int k = 0; k < 4; ++k)
                r.u16(); // 16-bit bounds (superseded by the long bounds)
            const qint64 top = r.u32(), left = r.u32();
            const qint64 bottom = r.u32(), right = r.u32();
            const int depth = r.u16();
            const int compression = r.u8();
            if (!r.ok()) {
                s.error = QStringLiteral("unexpected EOF in brush header");
            } else {
                QString why;
                s.depth = depth;
                s.mask = readMaskData(r, int(right - left),
                                      int(bottom - top), depth, compression,
                                      &why);
                if (s.mask.isNull())
                    s.error = why;
            }
        }
        out.append(s);
        if (!r.seek(end)) // resynchronise on the declared record length
            break;
    }
    if (!r.ok())
        fileNotes->append(QStringLiteral(
            "file ends before the declared brush count — imported what was "
            "readable"));
    return out;
}

// ---- v6+ 8BIM sections -----------------------------------------------------
struct Section
{
    QByteArray tag;
    qsizetype start = 0;
    qsizetype len = 0;
};

QVector<Section> scanSections(ByteReader &r)
{
    QVector<Section> out;
    while (r.remaining() >= 12) {
        const qsizetype at = r.tell();
        if (std::memcmp(r.peek(4), "8BIM", 4) != 0) {
            // Unknown padding convention between sections: try the next
            // 2-aligned position once, then give up cleanly.
            if (!r.seek(at + 2) || r.remaining() < 12
                || std::memcmp(r.peek(4), "8BIM", 4) != 0)
                break;
        }
        r.skip(4);
        Section s;
        s.tag = r.bytes(4);
        const quint32 len = r.u32();
        if (!r.ok() || qsizetype(len) > r.remaining())
            break;
        s.start = r.tell();
        s.len = qsizetype(len);
        out.append(s);
        // Sections are 4-aligned in every file observed; the scan above
        // resynchronises if a writer chose otherwise.
        r.seek(s.start + ((s.len + 3) & ~qsizetype(3)));
    }
    return out;
}

QVector<AbrSample> parseSampV6(ByteReader &r, const Section &samp,
                               int subversion, QStringList *fileNotes)
{
    QVector<AbrSample> out;
    const qsizetype sectionEnd = samp.start + samp.len;
    r.seek(samp.start);
    while (r.ok() && r.tell() + 4 <= sectionEnd
           && out.size() < kMaxBrushesPerFile) {
        const quint32 len = r.u32();
        if (!r.ok())
            break;
        const qsizetype entryStart = r.tell();
        const qsizetype next =
            entryStart + ((qsizetype(len) + 3) & ~qsizetype(3));
        if (qsizetype(len) < 1 || entryStart + qsizetype(len) > sectionEnd) {
            fileNotes->append(QStringLiteral(
                "samp entry %1 declares %2 bytes past the section end — "
                "stopped there, earlier brushes kept")
                                  .arg(out.size() + 1).arg(len));
            break;
        }
        AbrSample s;
        // The entry begins with a pascal id string (the desc pairing key)
        // inside a fixed-size preamble: 47 bytes (subversion 1) or 301
        // (subversion 2) — the skip counts GIMP and Krita established.
        const uchar *p = r.peek(37);
        if (p && p[0] == 36)
            s.uuid = QString::fromLatin1(
                reinterpret_cast<const char *>(p + 1), 36);
        const qsizetype preamble = subversion == 1 ? 47 : 301;
        if (!r.seek(entryStart + preamble)
            || r.tell() + 19 > entryStart + qsizetype(len)) {
            s.error = QStringLiteral("samp entry too short for its header");
        } else {
            const qint64 top = r.u32(), left = r.u32();
            const qint64 bottom = r.u32(), right = r.u32();
            const int depth = r.u16();
            const int compression = r.u8();
            if (!r.ok()) {
                s.error = QStringLiteral("unexpected EOF in samp header");
            } else {
                QString why;
                s.depth = depth;
                s.mask = readMaskData(r, int(right - left),
                                      int(bottom - top), depth, compression,
                                      &why);
                if (s.mask.isNull())
                    s.error = why;
            }
        }
        out.append(s);
        if (next <= entryStart || !r.seek(qMin(next, sectionEnd)))
            break;
    }
    return out;
}

// ---- Photoshop descriptor parser -------------------------------------------
// The same recursive structure PSD files use. Parsed into a QVariant tree:
// Objc -> QVariantMap, VlLs -> QVariantList, UntF/doub -> double,
// TEXT -> QString, enum -> QString (the enum id), bool/long as themselves.
// Depth- and node-capped; any structural surprise fails the WHOLE descriptor
// cleanly (the file then imports on samp data alone, with a report note).
struct DescParser
{
    ByteReader &r;
    int nodes = 0;

    bool budget()
    {
        if (++nodes > kDescMaxNodes) {
            r.fail();
            return false;
        }
        return true;
    }

    QVariant value(const QByteArray &type, int depth)
    {
        if (depth > kDescMaxDepth || !budget()) {
            r.fail();
            return QVariant();
        }
        if (type == "Objc" || type == "GlbO")
            return descriptor(depth + 1);
        if (type == "VlLs") {
            const quint32 count = r.u32();
            if (!r.ok() || qsizetype(count) > kDescMaxNodes) {
                r.fail();
                return QVariant();
            }
            QVariantList list;
            for (quint32 i = 0; i < count && r.ok(); ++i) {
                const QByteArray t = r.bytes(4);
                list.append(value(t, depth + 1));
            }
            return list;
        }
        if (type == "doub")
            return r.f64();
        if (type == "UntF") {
            r.bytes(4); // unit 4cc ('#Prc', '#Pxl', '#Ang', ...)
            return r.f64();
        }
        if (type == "TEXT")
            return r.unicodeString();
        if (type == "enum") {
            r.descKey(); // enum type id
            return r.descKey();
        }
        if (type == "long")
            return int(qint32(r.u32()));
        if (type == "comp")
            return qlonglong(r.s64());
        if (type == "bool")
            return r.u8() != 0;
        if (type == "type" || type == "GlbC") {
            r.unicodeString();
            r.descKey();
            return QVariant();
        }
        if (type == "alis" || type == "tdta" || type == "Pth ") {
            const quint32 len = r.u32();
            r.skip(qsizetype(len));
            return QVariant();
        }
        if (type == "obj ") {
            // Reference: a structure this importer has no use for. Its
            // items have no self-describing length, so it cannot be
            // skipped — fail the descriptor rather than misparse it.
            r.fail();
            return QVariant();
        }
        r.fail(); // unknown value type: alignment is lost, stop cleanly
        return QVariant();
    }

    QVariant descriptor(int depth)
    {
        if (depth > kDescMaxDepth || !budget()) {
            r.fail();
            return QVariant();
        }
        r.unicodeString(); // class name (unused)
        const QString classId = r.descKey();
        const quint32 count = r.u32();
        if (!r.ok() || qsizetype(count) > kDescMaxNodes) {
            r.fail();
            return QVariant();
        }
        QVariantMap map;
        // The classID distinguishes tip flavours (computedBrush /
        // sampledBrush / dBrush bristle / dTips erodible); stashed under a
        // reserved key no descriptor uses.
        map.insert(QStringLiteral("_classID"), classId);
        for (quint32 i = 0; i < count && r.ok(); ++i) {
            const QString key = r.descKey();
            const QByteArray type = r.bytes(4);
            map.insert(key, value(type, depth + 1));
        }
        return map;
    }
};

QVariantMap parseDescSection(const QByteArray &fileBytes,
                             const Section &desc)
{
    ByteReader r(fileBytes);
    if (!r.seek(desc.start))
        return QVariantMap();
    const quint32 version = r.u32();
    if (version != 16) // the only published descriptor version
        return QVariantMap();
    DescParser p{r};
    const QVariant root = p.descriptor(0);
    if (!r.ok() || r.tell() > desc.start + desc.len)
        return QVariantMap();
    return root.toMap();
}

// ---- 'patt' texture patterns -----------------------------------------------
// PSD pattern format: header + a virtual-memory-array list per channel. Only
// what a grain texture needs is read: the first written channel, raw or
// scanline-RLE, 8-bit. Anything else drops the texture with a named reason.
struct AbrPattern
{
    QString id;
    QString name;
    QImage image; // Grayscale8
};

QVector<AbrPattern> parsePattSection(const QByteArray &fileBytes,
                                     const Section &patt,
                                     QStringList *fileNotes)
{
    QVector<AbrPattern> out;
    ByteReader r(fileBytes);
    if (!r.seek(patt.start))
        return out;
    const qsizetype sectionEnd = patt.start + patt.len;
    while (r.ok() && r.tell() + 4 <= sectionEnd
           && out.size() < kMaxPatterns) {
        const quint32 len = r.u32();
        const qsizetype entryStart = r.tell();
        const qsizetype next =
            entryStart + ((qsizetype(len) + 3) & ~qsizetype(3));
        if (qsizetype(len) < 1 || entryStart + qsizetype(len) > sectionEnd)
            break;
        AbrPattern pat;
        const quint32 version = r.u32();
        const quint32 imageMode = r.u32();
        r.u16(); // point: vertical
        r.u16(); // point: horizontal
        pat.name = r.unicodeString();
        const int idLen = r.u8();
        pat.id = QString::fromLatin1(r.bytes(idLen));
        if (version != 1 || !r.ok()) {
            fileNotes->append(
                QStringLiteral("pattern '%1': unsupported version %2")
                    .arg(pat.name).arg(version));
            r.seek(next);
            continue;
        }
        if (imageMode == 2) // indexed: colour table precedes the VMA list
            r.skip(768 + 4);
        // Virtual memory array list.
        const quint32 vmaVersion = r.u32();
        r.u32(); // vma list length
        const qint64 top = r.u32(), left = r.u32();
        const qint64 bottom = r.u32(), right = r.u32();
        const quint32 channels = r.u32();
        const qsizetype w = qsizetype(right - left);
        const qsizetype h = qsizetype(bottom - top);
        if (vmaVersion != 3 || !r.ok() || w < 1 || h < 1
            || w > kMaxPatternDim || h > kMaxPatternDim
            || channels > 64) {
            fileNotes->append(
                QStringLiteral("pattern '%1': malformed header — dropped")
                    .arg(pat.name));
            r.seek(next);
            continue;
        }
        // Channels: written flag + length, then depth/rect/depth/compression.
        for (quint32 c = 0; c < channels + 2 && r.ok()
             && r.tell() < entryStart + qsizetype(len);
             ++c) {
            const quint32 written = r.u32();
            const quint32 chLen = r.u32();
            if (!written)
                continue;
            const qsizetype chEnd = r.tell() + qsizetype(chLen);
            if (chLen < 23 || chEnd > entryStart + qsizetype(len)) {
                r.fail();
                break;
            }
            const quint32 chDepth32 = r.u32();
            const qint64 cTop = r.u32(), cLeft = r.u32();
            const qint64 cBottom = r.u32(), cRight = r.u32();
            r.u16(); // depth again
            const int compression = r.u8();
            const int cw = int(cRight - cLeft), ch = int(cBottom - cTop);
            QString why;
            QImage img;
            if (chDepth32 == 8 && cw >= 1 && ch >= 1
                && cw <= kMaxPatternDim && ch <= kMaxPatternDim)
                img = readMaskData(r, cw, ch, 8, compression, &why);
            if (!img.isNull()) {
                pat.image = img;
                r.seek(chEnd);
                break; // first decodable channel is the grain source
            }
            if (!r.seek(chEnd))
                break;
        }
        if (pat.image.isNull())
            fileNotes->append(QStringLiteral(
                "pattern '%1': no decodable 8-bit channel — any texture "
                "referencing it is dropped")
                                  .arg(pat.name));
        out.append(pat);
        if (next <= entryStart || !r.seek(qMin(next, sectionEnd)))
            break;
    }
    return out;
}

// ---- Descriptor lookups ----------------------------------------------------
QVariant descGet(const QVariantMap &map, const char *key)
{
    return map.value(QLatin1String(key));
}

double descNumber(const QVariantMap &map, const char *key, double fallback)
{
    const QVariant v = descGet(map, key);
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok && v.isValid() ? d : fallback;
}

// A Photoshop dynamics block: control source + minimum + random jitter.
// Key names verified against ag-psd's abr.ts parseDynamics(): 'bVTy' the
// control enum, 'fStp' the fade step count, 'jitter' and 'Mnm ' percent
// unit-floats.
struct Dynamics
{
    bool present = false;
    int control = 0;    // 'bVTy': 0 off, 1 fade, 2 pressure, 3 tilt, ...
    double jitterPct = 0.0;
    double minimumPct = 0.0;
    double fadeSteps = 0.0; // 'fStp': stamps, not pixels — see fadePixels()
};

Dynamics readDynamics(const QVariantMap &brush, const char *key)
{
    Dynamics d;
    const QVariant v = descGet(brush, key);
    if (v.userType() != QMetaType::QVariantMap)
        return d;
    const QVariantMap m = v.toMap();
    d.present = true;
    d.control = int(descNumber(m, "bVTy", 0));
    d.jitterPct = descNumber(m, "jitter", 0.0);
    d.minimumPct = descNumber(m, "Mnm ", 0.0);
    d.fadeSteps = descNumber(m, "fStp", 0.0);
    return d;
}

// Photoshop's 'bVTy' control enum (order established by ag-psd).
QString controlName(int control)
{
    switch (control) {
    case 0: return QStringLiteral("off");
    case 1: return QStringLiteral("fade");
    case 2: return QStringLiteral("pen pressure");
    case 3: return QStringLiteral("pen tilt");
    case 4: return QStringLiteral("stylus wheel");
    case 5: return QStringLiteral("initial direction");
    case 6: return QStringLiteral("direction");
    case 7: return QStringLiteral("initial rotation");
    case 8: return QStringLiteral("rotation");
    default: return QStringLiteral("source %1").arg(control);
    }
}

// ---- Tip conversion --------------------------------------------------------
// The mask is imported UNTRANSFORMED. Photoshop's static angle, roundness
// and flips map onto the engine's own static tip parameters (tipAngle /
// tipRoundness / tipFlipX / tipFlipY, dynamics Phase 2) instead of being
// flattened into the bitmap — a baked-in rotation would be rotated AGAIN
// once direction-driven angle ships (dynamics Phase 3). This helper only
// letterboxes into the square the engine's IgnoreAspectRatio scaling
// expects, and downscales past the engine's 2048 px tip ceiling.
QImage squareTip(const QImage &mask, QStringList *approximated)
{
    int side = qMax(mask.width(), mask.height());
    QImage square(side, side, QImage::Format_Grayscale8);
    square.fill(0);
    const int offsetX = (side - mask.width()) / 2;
    const int offsetY = (side - mask.height()) / 2;
    for (int y = 0; y < mask.height(); ++y)
        std::memcpy(square.scanLine(y + offsetY) + offsetX,
                    mask.constScanLine(y), size_t(mask.width()));
    if (side > kEngineTipMax) {
        approximated->append(
            QStringLiteral("tip downscaled %1 px -> %2 px (engine ceiling)")
                .arg(side).arg(kEngineTipMax));
        square = square.scaled(kEngineTipMax, kEngineTipMax,
                               Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_Grayscale8);
    }
    return square;
}

// ---- Static-transform convention conversion --------------------------------
// Photoshop's roundness squashes the tip's LOCAL Y (a 50% round brush is a
// wide, short ellipse); the engine's tipRoundness squashes tip-local X
// (Phase 2's documented affine). The two are reconciled EXACTLY by a 90°
// content pre-rotation — lossless for any image:
//     ScaleY(r) = R(+90) * ScaleX(r) * R(-90)
// Rotation signs: Photoshop angles are counter-clockwise positive; the
// engine's tipAngle is clockwise positive in screen space; so without
// roundness the mapping is simply tipAngle = -Angl. With roundness the
// pre-rotation folds in:  tipAngle = 90 - Angl,  and the pre-rotation's
// DIRECTION depends on flip parity, because a reflection conjugates
// rotation (Flip * R(b) = R(-b) * Flip): one flip -> pre-rotate +90,
// zero or two flips -> pre-rotate -90. Flips themselves map directly —
// both Photoshop and the engine apply them to the tip BEFORE rotation.
// Verified over every angle x roundness x flip combination against a
// SEQUENTIALLY composed Photoshop-order reference (flip, then squash local
// Y, then rotate — each its own resample, so no QTransform composition
// order can be assumed). Deliberately NOT against the transform this
// replaced: that one chained QTransform calls, which applies the rotation
// FIRST and therefore squashed in CANVAS space, leaving angle+roundness
// brushes about 20 degrees off Photoshop.
struct TipMapping
{
    QImage mask;        // possibly pre-rotated by an exact 90°
    double engineAngle; // degrees, engine (clockwise-positive) convention
    double roundness;   // 0-1 for setTipRoundness
};

TipMapping mapStaticTransform(const QImage &mask, double angleDeg,
                              double roundnessPct, bool flipX, bool flipY)
{
    TipMapping out;
    out.mask = mask;
    const bool squashed = roundnessPct > 0.5 && roundnessPct < 99.5;
    out.roundness = squashed ? roundnessPct / 100.0 : 1.0;
    if (!squashed) {
        out.engineAngle = -angleDeg;
        return out;
    }
    const bool oddFlips = flipX != flipY;
    out.mask = mask.transformed(
        QTransform().rotate(oddFlips ? 90.0 : -90.0));
    out.engineAngle = 90.0 - angleDeg;
    return out;
}

// ---- Report assembly -------------------------------------------------------
struct BrushReport
{
    QString title;
    QStringList mapped, approximated, dropped;

    void write(QString *out) const
    {
        *out += QStringLiteral("• %1\n").arg(title);
        auto section = [out](const char *label, const QStringList &lines) {
            if (lines.isEmpty())
                return;
            *out += QStringLiteral("    %1: %2\n")
                        .arg(QLatin1String(label),
                             lines.join(QStringLiteral("; ")));
        };
        section("mapped", mapped);
        section("approximated", approximated);
        section("dropped", dropped);
    }
};

const QVector<QPointF> kFlatCurve = {QPointF(0.0, 1.0), QPointF(1.0, 1.0)};
// The LINEAR identity response. Photoshop has no per-dynamic response
// curve: a controlled dynamic simply interpolates from the minimum at zero
// drive to the full value at full drive. That is exactly the engine's
//     multiplier = minimum + (1 - minimum) x curve(source)
// with an identity curve — so the minimum belongs in the MINIMUM, and the
// curve stays the honest straight line Photoshop implies. (The old
// two-point curve from min% to 100% with minimum 0 evaluates to the same
// numbers, since both encode the same affine map; what it lost was the
// SLOT: the studio's Minimum control read 0, an edit to the curve silently
// destroyed the floor, and the report had to describe a curve where
// Photoshop had a percentage.)
const QVector<QPointF> kLinearCurve = {QPointF(0.0, 0.0), QPointF(1.0, 1.0)};

// Brush::dynamicCurve() is const-only; the mutable curves are reached
// through their named accessors. This is the importer's own property ->
// curve map so the mapping code below can stay table-driven without
// touching the engine.
PressureCurve &curveFor(::Brush &b, ::Brush::DynamicProperty property)
{
    using P = ::Brush::DynamicProperty;
    switch (property) {
    case P::Size: return b.sizePressureCurve();
    case P::Opacity: return b.opacityPressureCurve();
    case P::Hardness: return b.hardnessPressureCurve();
    case P::Flow: return b.flowPressureCurve();
    case P::Scatter: return b.scatterPressureCurve();
    case P::Smudge: return b.smudgePressureCurve();
    case P::SizeJitter: return b.sizeJitterPressureCurve();
    case P::AngleJitter: return b.angleJitterPressureCurve();
    case P::RoundnessJitter: return b.roundnessJitterPressureCurve();
    case P::SpacingJitter: return b.spacingJitterPressureCurve();
    case P::GrainDepth: return b.grainDepthPressureCurve();
    case P::HueJitter: return b.hueJitterPressureCurve();
    case P::SaturationJitter: return b.saturationJitterPressureCurve();
    case P::BrightnessJitter: return b.brightnessJitterPressureCurve();
    }
    return b.sizePressureCurve(); // unreachable; keeps every path returning
}

// ---- Photoshop control source -> engine ControlSource ----------------------
// The 'bVTy' enum order is ag-psd's dynamicsControl table (abr.ts):
//   0 off  1 fade  2 pen pressure  3 pen tilt  4 stylus wheel
//   5 initial direction  6 direction  7 initial rotation  8 rotation
struct SourceMapping
{
    ::Brush::ControlSource source = ::Brush::ControlSource::None;
    bool supported = false;
    bool approximate = false; // supported, but not the same thing
    QString note;             // why it is approximate
};

SourceMapping mapControlSource(int control)
{
    using CS = ::Brush::ControlSource;
    SourceMapping m;
    switch (control) {
    case 0: // off — the property sits at its base value
        m.source = CS::None;
        m.supported = true;
        return m;
    case 1:
        m.source = CS::Fade;
        m.supported = true;
        return m;
    case 2:
        m.source = CS::Pressure;
        m.supported = true;
        return m;
    case 3:
        m.source = CS::Tilt;
        m.supported = true;
        return m;
    case 6:
        m.source = CS::Direction;
        m.supported = true;
        return m;
    case 5:
        // Initial Direction locks the heading sampled at the FIRST dab for
        // the whole stroke; the engine's Direction source tracks the
        // heading continuously. Same driver, different hold: a straight
        // stroke is identical, a curved one diverges progressively. Mapped
        // to Direction (the closest live behaviour) and reported.
        m.source = CS::Direction;
        m.supported = true;
        m.approximate = true;
        m.note = QStringLiteral(
            "initial direction -> direction (the engine tracks the heading "
            "continuously; identical on straight strokes, diverging as the "
            "stroke curves)");
        return m;
    default: // 4 stylus wheel, 7/8 rotation, anything unknown
        return m; // unsupported: reported dropped, never substituted
    }
}

// Photoshop counts fade in STEPS — brush stamps — while the engine's
// fadeDistance is canvas pixels of arc length. One step advances by the
// stamp spacing, and spacing is a fraction of the tip diameter:
//     pixels = steps x spacing x diameter
// (25 steps at 25% spacing on a 40 px tip = 250 px.) The engine clamps to
// [1, 8192]; the caller reports the conversion with both numbers.
double fadePixels(double steps, double spacingFraction, double sizePx)
{
    return std::clamp(steps * spacingFraction * sizePx, 1.0, 8192.0);
}

// Everything a dynamics block needs to land on an engine property.
struct DynamicTarget
{
    ::Brush *brush = nullptr;
    ::Brush::DynamicProperty property{};
    QString what;        // report noun ("size", "angle", ...)
    double extraMinPct = -1.0; // Photoshop's separate minimum field, if any
};

// Route one Photoshop dynamics block onto one engine property: source,
// minimum, and response curve, with the report line that matches what was
// actually done. Returns the fade step count when the block is fade-driven
// (the caller owns the brush-wide fadeDistance), else 0.
double applyDynamic(const Dynamics &d, const DynamicTarget &t,
                    BrushReport *rep)
{
    ::Brush &b = *t.brush;
    // Photoshop stores a separate "Minimum Diameter" / "Minimum Roundness"
    // / "Minimum Depth" beside some dynamics blocks; honour whichever
    // floor is higher, as the UI does.
    double minimumPct = d.present ? d.minimumPct : 0.0;
    if (t.extraMinPct >= 0.0)
        minimumPct = qMax(minimumPct, t.extraMinPct);
    minimumPct = std::clamp(minimumPct, 0.0, 100.0);

    if (!d.present || d.control == 0) {
        // No dynamic: the property sits at its base value. Source None
        // says exactly that — a flat curve on a Pressure source would
        // render the same but claim a pressure response the brush never
        // had.
        b.setControlSource(t.property, ::Brush::ControlSource::None);
        b.setControlMinimum(t.property, 0.0);
        curveFor(b, t.property).setControlPoints(kFlatCurve);
        return 0.0;
    }

    const SourceMapping m = mapControlSource(d.control);
    if (!m.supported) {
        b.setControlSource(t.property, ::Brush::ControlSource::None);
        b.setControlMinimum(t.property, 0.0);
        curveFor(b, t.property).setControlPoints(kFlatCurve);
        rep->dropped.append(
            QStringLiteral("%1 dynamic (%2 source — no engine equivalent)")
                .arg(t.what, controlName(d.control)));
        return 0.0;
    }

    b.setControlSource(t.property, m.source);
    b.setControlMinimum(t.property, minimumPct / 100.0);
    curveFor(b, t.property).setControlPoints(kLinearCurve);

    QString line = QStringLiteral("%1 %2-controlled").arg(
        t.what, controlName(d.control));
    if (minimumPct > 0.05)
        line += QStringLiteral(" (minimum %1%)").arg(qRound(minimumPct));
    rep->mapped.append(line);
    if (m.approximate)
        rep->approximated.append(
            QStringLiteral("%1 dynamic: %2").arg(t.what, m.note));
    return m.source == ::Brush::ControlSource::Fade ? d.fadeSteps : 0.0;
}

// ---- Per-brush assembly ----------------------------------------------------
struct MappedBrush
{
    BrushPreset preset;
    BrushReport report;
    bool valid = false;
};

// Every dynamic starts SILENT: source None with a flat curve. The engine's
// defaults are Pressure + linear, which would give every imported brush a
// full pressure response Photoshop never had; before this phase the curves
// were flattened but the SOURCE was left at Pressure, so the studio showed
// a pressure-driven row for a brush with no dynamics at all. Renders
// identically either way (a flat curve and a None source both resolve to
// 1.0) — this states it truthfully. Each Photoshop dynamic that IS present
// overwrites its own property below.
void silenceAllDynamics(::Brush &b)
{
    for (int i = 0; i < ::Brush::kDynamicPropertyCount; ++i) {
        const auto property = ::Brush::DynamicProperty(i);
        b.setControlSource(property, ::Brush::ControlSource::None);
        b.setControlMinimum(property, 0.0);
    }
    b.sizePressureCurve().setControlPoints(kFlatCurve);
    b.opacityPressureCurve().setControlPoints(kFlatCurve);
    b.hardnessPressureCurve().setControlPoints(kFlatCurve);
    b.flowPressureCurve().setControlPoints(kFlatCurve);
    b.scatterPressureCurve().setControlPoints(kFlatCurve);
    b.smudgePressureCurve().setControlPoints(kFlatCurve);
    b.sizeJitterPressureCurve().setControlPoints(kFlatCurve);
    b.angleJitterPressureCurve().setControlPoints(kFlatCurve);
    b.roundnessJitterPressureCurve().setControlPoints(kFlatCurve);
    b.spacingJitterPressureCurve().setControlPoints(kFlatCurve);
    b.grainDepthPressureCurve().setControlPoints(kFlatCurve);
    b.hueJitterPressureCurve().setControlPoints(kFlatCurve);
    b.saturationJitterPressureCurve().setControlPoints(kFlatCurve);
    b.brightnessJitterPressureCurve().setControlPoints(kFlatCurve);
}

MappedBrush buildBrush(const AbrSample *sample, const QVariantMap &desc,
                       const QVector<AbrPattern> &patterns,
                       const QString &fallbackName,
                       const AbrSample *dualSample = nullptr)
{
    MappedBrush out;
    BrushReport &rep = out.report;
    ::Brush &b = out.preset.brush;
    silenceAllDynamics(b);

    // --- Tip descriptor (nested 'Brsh' object inside the brush entry) ---
    const QVariantMap tip = descGet(desc, "Brsh").toMap();
    const double diameter = descNumber(tip, "Dmtr", 0.0);
    const double angle = descNumber(tip, "Angl", 0.0);
    const double roundness = descNumber(tip, "Rndn", 100.0);
    const bool flipX = descGet(tip, "flipX").toBool();
    const bool flipY = descGet(tip, "flipY").toBool();

    // --- Name ---
    QString name = descGet(desc, "Nm  ").toString().trimmed();
    if (name.isEmpty() && sample)
        name = sample->legacyName.trimmed();
    if (name.isEmpty())
        name = fallbackName;
    out.preset.name = name;

    // --- Tip image + static transform ---
    // The mask stays UNTRANSFORMED; angle, roundness and flips land on the
    // engine's static tip parameters (see mapStaticTransform for the
    // convention conversion). Baking is gone — dynamics Phase 3's
    // direction-driven rotation would have rotated a baked tip twice.
    const QString tipClass =
        tip.value(QStringLiteral("_classID")).toString();
    if (sample && !sample->mask.isNull()) {
        const TipMapping mapping = mapStaticTransform(
            sample->mask, angle, roundness, flipX, flipY);
        b.setCustomShape(squareTip(mapping.mask, &rep.approximated));
        b.setTipAngle(mapping.engineAngle);
        b.setTipRoundness(mapping.roundness);
        b.setTipFlipX(flipX);
        b.setTipFlipY(flipY);
        rep.mapped.append(
            QStringLiteral("sampled tip %1x%2 (%3-bit) -> custom shape, "
                           "untransformed")
                .arg(sample->mask.width())
                .arg(sample->mask.height())
                .arg(sample->depth));
        if (std::abs(angle) > 0.05)
            rep.mapped.append(QStringLiteral("tip angle %1°")
                                  .arg(angle, 0, 'f', 1));
        if (mapping.roundness < 1.0)
            rep.mapped.append(QStringLiteral("tip roundness %1%")
                                  .arg(qRound(roundness)));
        if (flipX || flipY)
            rep.mapped.append(QStringLiteral("tip flip %1%2")
                                  .arg(flipX ? QStringLiteral("X") : QString(),
                                       flipY ? QStringLiteral("Y") : QString()));
    } else if (!desc.isEmpty() && !tip.isEmpty()) {
        // Non-sampled tip flavours use the engine's PROCEDURAL tip — its
        // hardness falloff is the same formula the old synthesized image
        // copied, now evaluated at stamp resolution instead of resampled
        // from a 128 px bitmap. computedBrush carries a real hardness; the
        // bristle (dBrush) and erodible/airbrush (dTips) simulations have
        // no engine counterpart — approximated as a simple round, and the
        // report says so.
        if (tipClass == QLatin1String("dBrush"))
            rep.dropped.append(QStringLiteral(
                "bristle simulation (density/length/stiffness) — tip "
                "approximated as a simple round"));
        else if (tipClass == QLatin1String("dTips"))
            rep.dropped.append(QStringLiteral(
                "erodible/airbrush tip simulation — tip approximated as a "
                "simple round"));
        const double hardnessPct = descNumber(tip, "Hrdn", 100.0);
        b.setHardness(std::clamp(hardnessPct / 100.0, 0.0, 1.0));
        // Procedural discs are isotropic, so the X/Y squash-axis
        // difference vanishes: roundness and angle map directly.
        b.setTipRoundness(roundness > 0.5 && roundness < 99.5
                              ? roundness / 100.0 : 1.0);
        b.setTipAngle(-angle);
        rep.mapped.append(
            QStringLiteral("computed tip -> procedural (hardness %1%)")
                .arg(qRound(hardnessPct)));
        if (roundness > 0.5 && roundness < 99.5)
            rep.mapped.append(QStringLiteral("tip roundness %1%")
                                  .arg(qRound(roundness)));
        if (std::abs(angle) > 0.05)
            rep.mapped.append(QStringLiteral("tip angle %1°")
                                  .arg(angle, 0, 'f', 1));
    } else {
        rep.dropped.append(QStringLiteral("no tip data"));
        return out; // not importable
    }

    // --- Size ---
    double sizePx = diameter;
    if (sizePx < 1.0 && sample)
        sizePx = qMax(sample->mask.width(), sample->mask.height());
    if (sizePx > kEngineTipMax) {
        rep.approximated.append(
            QStringLiteral("size %1 px clamped to the engine maximum %2")
                .arg(qRound(sizePx)).arg(kEngineTipMax));
        sizePx = kEngineTipMax;
    }
    b.setSize(qMax(1, qRound(sizePx)));
    rep.mapped.append(QStringLiteral("size %1 px").arg(b.size()));

    // --- Spacing ---
    // Tip-level Spcn first, brush-level as fallback; Intr false means
    // Photoshop's "spacing off" (speed-driven stamping) which has no
    // engine counterpart — a tight 2% reads closest to continuous.
    double spacingPct = descNumber(tip, "Spcn", -1.0);
    if (spacingPct < 0.0)
        spacingPct = descNumber(desc, "Spcn", -1.0);
    if (spacingPct < 0.0 && sample && sample->spacingPercent >= 0)
        spacingPct = sample->spacingPercent;
    const QVariant spacingOn = descGet(tip, "Intr");
    if (spacingOn.isValid() && !spacingOn.toBool()) {
        b.setSpacing(0.02);
        rep.approximated.append(QStringLiteral(
            "Photoshop spacing disabled (speed-based) -> 2% used"));
    } else if (spacingPct >= 1.0) {
        b.setSpacing(spacingPct / 100.0);
        rep.mapped.append(
            QStringLiteral("spacing %1%").arg(qRound(spacingPct)));
    } else {
        b.setSpacing(0.25);
        rep.approximated.append(QStringLiteral(
            "no usable spacing in the file — Photoshop's default 25% used"));
    }

    if (desc.isEmpty()) {
        out.valid = true; // legacy: nothing further to read
        return out;
    }

    // Fade is a BRUSH-wide distance in the engine but a PER-DYNAMIC step
    // count in Photoshop. Collect every fade-driven block's step count and
    // reconcile once, after the dynamics are read.
    QVector<QPair<QString, double>> fadeSteps;
    auto noteFade = [&fadeSteps](const QString &what, double steps) {
        if (steps > 0.0)
            fadeSteps.append({what, steps});
    };

    // --- Tip dynamics (gated on useTipDynamics, as Photoshop stores it) ---
    if (descGet(desc, "useTipDynamics").toBool()) {
        const Dynamics szVr = readDynamics(desc, "szVr");
        if (szVr.present && szVr.jitterPct > 0.0) {
            b.setSizeJitter(szVr.jitterPct / 100.0);
            rep.mapped.append(QStringLiteral("size jitter %1%")
                                  .arg(qRound(szVr.jitterPct)));
        }
        noteFade(QStringLiteral("size"),
                 applyDynamic(szVr,
                              {&b, ::Brush::DynamicProperty::Size,
                               QStringLiteral("size"),
                               descNumber(desc, "minimumDiameter", -1.0)},
                              &rep));

        // ANGLE: the mapping this overhaul exists for. Photoshop's "Angle
        // Jitter -> Control: Direction" is what makes calligraphic and
        // ribbon brushes work; the engine's Direction source on the angle
        // property injects the smoothed heading into the stamp rotation,
        // where the static tipAngle mapped above acts as an OFFSET from
        // it — the same composition Photoshop uses.
        const Dynamics angDyn = readDynamics(desc, "angleDynamics");
        if (angDyn.present && angDyn.jitterPct > 0.0) {
            b.setAngleJitter(angDyn.jitterPct / 100.0);
            rep.mapped.append(QStringLiteral("angle jitter %1%")
                                  .arg(qRound(angDyn.jitterPct)));
        }
        noteFade(QStringLiteral("angle"),
                 applyDynamic(angDyn,
                              {&b, ::Brush::DynamicProperty::AngleJitter,
                               QStringLiteral("angle")},
                              &rep));

        const Dynamics rndDyn = readDynamics(desc, "roundnessDynamics");
        if (rndDyn.present && rndDyn.jitterPct > 0.0) {
            b.setRoundnessJitter(rndDyn.jitterPct / 100.0);
            rep.mapped.append(QStringLiteral("roundness jitter %1%")
                                  .arg(qRound(rndDyn.jitterPct)));
        }
        noteFade(QStringLiteral("roundness"),
                 applyDynamic(rndDyn,
                              {&b, ::Brush::DynamicProperty::RoundnessJitter,
                               QStringLiteral("roundness"),
                               descNumber(desc, "minimumRoundness", -1.0)},
                              &rep));

        // Random flip jitters (brush-level flipX/flipY) have no slot.
        if (descGet(desc, "flipX").toBool()
            || descGet(desc, "flipY").toBool())
            rep.dropped.append(
                QStringLiteral("flip jitter (no counterpart)"));
    }

    // --- Scatter (amount lives in scatterDynamics.jitter) ---
    if (descGet(desc, "useScatter").toBool()) {
        const Dynamics sctr = readDynamics(desc, "scatterDynamics");
        const bool bothAxes = descGet(desc, "bothAxes").toBool();
        const double amount = std::clamp(sctr.jitterPct / 100.0, 0.0, 10.0);
        b.setScatterPerpendicular(amount);
        if (bothAxes)
            b.setScatterAlong(amount);
        const int count = int(descNumber(desc, "Cnt ", 1));
        b.setScatterCount(std::clamp(count, 1, 16));
        rep.mapped.append(
            QStringLiteral("scatter %1% (%2, count %3)")
                .arg(qRound(sctr.jitterPct))
                .arg(bothAxes ? QStringLiteral("both axes")
                              : QStringLiteral("one axis"))
                .arg(b.scatterCount()));
        noteFade(QStringLiteral("scatter"),
                 applyDynamic(sctr,
                              {&b, ::Brush::DynamicProperty::Scatter,
                               QStringLiteral("scatter")},
                              &rep));
        const Dynamics cntDyn = readDynamics(desc, "countDynamics");
        if (cntDyn.present
            && (cntDyn.jitterPct > 0.0 || cntDyn.control != 0))
            rep.dropped.append(
                QStringLiteral("scatter count dynamic (no counterpart)"));
    }

    // --- Texture -> custom grain ---
    if (descGet(desc, "useTexture").toBool()) {
        const QVariantMap txtr = descGet(desc, "Txtr").toMap();
        const QString patId = descGet(txtr, "Idnt").toString();
        const QString patName = descGet(txtr, "Nm  ").toString();
        const AbrPattern *found = nullptr;
        for (const AbrPattern &p : patterns)
            if (!p.image.isNull()
                && (p.id == patId || (!patName.isEmpty()
                                      && p.name == patName))) {
                found = &p;
                break;
            }
        if (found) {
            b.setCustomGrain(found->image);
            const double scalePct = descNumber(desc, "textureScale", 100.0);
            b.setGrainScale(std::clamp(
                found->image.width() * scalePct / 100.0, 1.0, 2048.0));
            const double depthPct = descNumber(desc, "textureDepth", 100.0);
            b.setGrainDepth(std::clamp(depthPct / 100.0, 0.0, 1.0));
            b.setGrainMode(::Brush::GrainMode::StaticCanvas);
            // Texture depth carries its own dynamics block ('textureDepth
            // Dynamics' + 'minimumDepth', ag-psd's texture.depthDynamics /
            // depthMinimum). Before this phase it was read by nothing at
            // all — neither mapped nor reported.
            noteFade(QStringLiteral("grain depth"),
                     applyDynamic(readDynamics(desc, "textureDepthDynamics"),
                                  {&b, ::Brush::DynamicProperty::GrainDepth,
                                   QStringLiteral("grain depth"),
                                   descNumber(desc, "minimumDepth", -1.0)},
                                  &rep));
            rep.mapped.append(
                QStringLiteral("texture '%1' %2x%3 -> custom grain "
                               "(scale %4%, depth %5%)")
                    .arg(found->name)
                    .arg(found->image.width())
                    .arg(found->image.height())
                    .arg(qRound(scalePct))
                    .arg(qRound(depthPct)));
            if (descGet(desc, "InvT").toBool())
                rep.dropped.append(
                    QStringLiteral("texture inversion (no counterpart)"));
            // Texture blend mode -> the native TextureBlendMode enum.
            // Files carry either the 4cc or the long enum key (this
            // machine's Sinbad file writes the long form "height"), so
            // both spellings are accepted per mode. The mapping is exact
            // at this layer — the old "approximated" line is gone; the
            // formula-vs-Photoshop confidence grading is an ENGINE
            // property and lives in TextureBlend.h, not in the import
            // report, because mapped/approximated/dropped describes what
            // the importer did with the file, and the importer now lands
            // every mode on its own enum value. An unknown string is the
            // one remaining approximation and is reported by name.
            const QString blend =
                descGet(desc, "textureBlendMode").toString();
            using TB = ::Brush::TextureBlendMode;
            TB texMode = TB::Multiply;
            bool known = true;
            if (blend.isEmpty() || blend == QLatin1String("Mltp")
                || blend == QLatin1String("multiply")) {
                texMode = TB::Multiply;
            } else if (blend == QLatin1String("Sbtr")
                       || blend == QLatin1String("blendSubtraction")
                       || blend == QLatin1String("subtraction")
                       || blend == QLatin1String("subtract")) {
                texMode = TB::Subtract;
            } else if (blend == QLatin1String("Drkn")
                       || blend == QLatin1String("darken")) {
                texMode = TB::Darken;
            } else if (blend == QLatin1String("Ovrl")
                       || blend == QLatin1String("overlay")) {
                texMode = TB::Overlay;
            } else if (blend == QLatin1String("CBrn")
                       || blend == QLatin1String("colorBurn")) {
                texMode = TB::ColorBurn;
            } else if (blend == QLatin1String("linearBurn")) {
                texMode = TB::LinearBurn;
            } else if (blend == QLatin1String("hardMix")) {
                texMode = TB::HardMix;
            } else if (blend == QLatin1String("Hght")
                       || blend == QLatin1String("height")) {
                texMode = TB::Height;
            } else if (blend == QLatin1String("linearHeight")) {
                texMode = TB::LinearHeight;
            } else {
                known = false;
            }
            b.setTextureBlendMode(texMode);
            if (known) {
                if (texMode != TB::Multiply)
                    rep.mapped.append(QStringLiteral(
                        "texture blend mode '%1' -> native").arg(blend));
            } else {
                rep.approximated.append(QStringLiteral(
                    "texture blend mode '%1' unrecognised -> Multiply "
                    "used").arg(blend));
            }
        } else {
            rep.dropped.append(
                QStringLiteral("texture '%1' (pattern data missing or "
                               "undecodable)")
                    .arg(patName.isEmpty() ? patId : patName));
        }
    }

    // --- Transfer: 'opVr' is opacity, 'prVr' is FLOW (ag-psd's mapping) ---
    if (descGet(desc, "usePaintDynamics").toBool()) {
        const Dynamics opVr = readDynamics(desc, "opVr");
        noteFade(QStringLiteral("opacity"),
                 applyDynamic(opVr,
                              {&b, ::Brush::DynamicProperty::Opacity,
                               QStringLiteral("opacity")},
                              &rep));
        if (opVr.present && opVr.jitterPct > 0.0)
            rep.dropped.append(QStringLiteral(
                "opacity jitter (engine has no random opacity)"));
        const Dynamics prVr = readDynamics(desc, "prVr");
        noteFade(QStringLiteral("flow"),
                 applyDynamic(prVr,
                              {&b, ::Brush::DynamicProperty::Flow,
                               QStringLiteral("flow")},
                              &rep));
        const Dynamics wtVr = readDynamics(desc, "wtVr");
        const Dynamics mxVr = readDynamics(desc, "mxVr");
        if ((wtVr.present && wtVr.control != 0)
            || (mxVr.present && mxVr.control != 0))
            rep.dropped.append(QStringLiteral(
                "wetness/mix dynamics (mixer brush — no counterpart)"));
    }

    // --- Colour dynamics -> the 6c parameters -----------------------------
    // clVr is a full BrushDynamics block driving the FOREGROUND/BACKGROUND
    // interpolation (the 15th dynamic); 'H   '/Strt/Brgh/purity are plain
    // percent amounts (no per-amount dynamics in Photoshop). Semantics
    // match what 6c verified: HSV space, hue wraps +/- jitter (100% = the
    // whole wheel), saturation/brightness are absolute offsets from base,
    // purity is a -100..100 push toward grey or full chroma. The engine's
    // per-brush backgroundColor stays at its default (white): Photoshop
    // interpolates toward the APP-LEVEL background colour at paint time
    // and the ABR file carries no colour to adopt — inventing one here
    // would bake today's palette into the preset. (6c's identity-colour
    // rule is untouched: backgroundColor is never adopted from or into
    // the app colour on selection.)
    if (descGet(desc, "useColorDynamics").toBool()) {
        const Dynamics clvr = readDynamics(desc, "clVr");
        b.setFgBgJitter(std::clamp(clvr.jitterPct / 100.0, 0.0, 1.0));
        noteFade(QStringLiteral("foreground/background"),
                 applyDynamic(clvr,
                              {&b,
                               ::Brush::DynamicProperty::ForegroundBackground,
                               QStringLiteral("foreground/background")},
                              &rep));
        const double huePct = descNumber(desc, "H   ", 0.0);
        const double satPct = descNumber(desc, "Strt", 0.0);
        const double brgPct = descNumber(desc, "Brgh", 0.0);
        const double purityPct = descNumber(desc, "purity", 0.0);
        b.setHueJitter(std::clamp(huePct / 100.0, 0.0, 1.0));
        b.setSaturationJitter(std::clamp(satPct / 100.0, 0.0, 1.0));
        b.setBrightnessJitter(std::clamp(brgPct / 100.0, 0.0, 1.0));
        b.setPurity(std::clamp(purityPct / 100.0, -1.0, 1.0));
        b.setColorDynamicsPerTip(
            descGet(desc, "colorDynamicsPerTip").toBool());
        rep.mapped.append(
            QStringLiteral("colour dynamics: fg/bg %1%, hue %2%, "
                           "saturation %3%, brightness %4%, purity %5%%6 "
                           "(background colour stays the engine default — "
                           "the file carries none)")
                .arg(qRound(clvr.jitterPct))
                .arg(qRound(huePct))
                .arg(qRound(satPct))
                .arg(qRound(brgPct))
                .arg(qRound(purityPct))
                .arg(b.colorDynamicsPerTip()
                         ? QStringLiteral(", per tip")
                         : QString()));
    }

    // --- Fade distance: per-dynamic STEPS -> one brush-wide distance -----
    // Photoshop gives every fade-driven dynamic its own step count; the
    // engine has a single fadeDistance shared by all of them. One fading
    // dynamic converts exactly; several with different counts cannot, so
    // the LONGEST wins (the shorter dynamics would otherwise be cut off
    // mid-fade, which reads as a bug rather than a brush) and the report
    // names every count that was reconciled.
    if (!fadeSteps.isEmpty()) {
        auto longest = fadeSteps.first();
        bool conflict = false;
        for (const auto &entry : fadeSteps) {
            if (entry.second > longest.second)
                longest = entry;
            if (qAbs(entry.second - fadeSteps.first().second) > 0.5)
                conflict = true;
        }
        const double px = fadePixels(longest.second, b.spacing(), b.size());
        b.setFadeDistance(px);
        rep.mapped.append(
            QStringLiteral("fade distance %1 steps x %2% spacing x %3 px "
                           "tip -> %4 px")
                .arg(qRound(longest.second))
                .arg(qRound(b.spacing() * 100.0))
                .arg(b.size())
                .arg(qRound(px)));
        if (conflict) {
            QStringList all;
            for (const auto &entry : fadeSteps)
                all.append(QStringLiteral("%1 %2")
                               .arg(entry.first)
                               .arg(qRound(entry.second)));
            rep.approximated.append(
                QStringLiteral("per-dynamic fade lengths differ (%1 steps) "
                               "— the engine has one fade distance per "
                               "brush; the longest was used")
                    .arg(all.join(QStringLiteral(", "))));
        }
    }

    // --- Dual brush -> the engine's Modulate mode -------------------------
    // Photoshop's dual block carries EXACTLY: Flip, the tip shape, BlnM,
    // Spcn, useScatter/scatterDynamics/bothAxes, and Cnt/countDynamics —
    // no colour, opacity, flow, paint dynamics or texture (validated
    // against ag-psd's abr.ts), so there is nothing else to map. B is a
    // coverage PATTERN in Modulate (6d forces it to the mask buffer), and
    // the importer sets no colour parameters on it.
    const QVariantMap dual = descGet(desc, "dualBrush").toMap();
    if (descGet(dual, "useDualBrush").toBool()) {
        b.setDualBrushEnabled(true);
        b.setDualMode(::Brush::DualMode::Modulate);
        ::Brush &sec = b.secondaryBrush();
        // setDualBrushEnabled clones the mid-build primary; B starts from
        // a clean default instead, then takes only what the block carries.
        sec = ::Brush();
        silenceAllDynamics(sec);
        const QVariantMap btip = descGet(dual, "Brsh").toMap();
        const double bDiameter = descNumber(btip, "Dmtr", 0.0);
        const double bAngle = descNumber(btip, "Angl", 0.0);
        const double bRoundness = descNumber(btip, "Rndn", 100.0);
        QString bTipNote;
        if (dualSample && !dualSample->mask.isNull()) {
            const TipMapping mapping = mapStaticTransform(
                dualSample->mask, bAngle, bRoundness, false, false);
            sec.setCustomShape(squareTip(mapping.mask, &rep.approximated));
            sec.setTipAngle(mapping.engineAngle);
            sec.setTipRoundness(mapping.roundness);
            bTipNote = QStringLiteral("sampled %1x%2 donor tip")
                           .arg(dualSample->mask.width())
                           .arg(dualSample->mask.height());
        } else {
            const double bHardness = descNumber(btip, "Hrdn", 100.0);
            sec.setHardness(std::clamp(bHardness / 100.0, 0.0, 1.0));
            sec.setTipRoundness(bRoundness > 0.5 && bRoundness < 99.5
                                    ? bRoundness / 100.0 : 1.0);
            sec.setTipAngle(-bAngle);
            bTipNote = QStringLiteral("computed tip (hardness %1%)")
                           .arg(qRound(bHardness));
        }
        double bSize = bDiameter;
        if (bSize < 1.0 && dualSample)
            bSize = qMax(dualSample->mask.width(),
                         dualSample->mask.height());
        if (bSize < 1.0)
            bSize = b.size();
        sec.setSize(qMax(1, qRound(qMin(bSize, double(kEngineTipMax)))));
        const double bSpacingPct = descNumber(dual, "Spcn", 25.0);
        sec.setSpacing(std::clamp(bSpacingPct / 100.0, 0.01, 10.0));
        if (descGet(dual, "useScatter").toBool()) {
            const Dynamics sctr = readDynamics(dual, "scatterDynamics");
            const double amount =
                std::clamp(sctr.jitterPct / 100.0, 0.0, 10.0);
            sec.setScatterPerpendicular(amount);
            if (descGet(dual, "bothAxes").toBool())
                sec.setScatterAlong(amount);
            if (sctr.present && sctr.control != 0)
                rep.approximated.append(QStringLiteral(
                    "dual scatter control (%1) -> static scatter")
                                            .arg(controlName(sctr.control)));
        }
        sec.setScatterCount(
            std::clamp(int(descNumber(dual, "Cnt ", 1)), 1, 16));
        const Dynamics cntDyn = readDynamics(dual, "countDynamics");
        if (cntDyn.present && (cntDyn.jitterPct > 0.0 || cntDyn.control != 0))
            rep.dropped.append(QStringLiteral(
                "dual scatter-count dynamic (no counterpart)"));
        if (descGet(dual, "Flip").toBool())
            rep.dropped.append(QStringLiteral(
                "dual tip random flip (flip jitter — no counterpart)"));
        // BlnM -> the Modulate coverage functions 6d defined. Multiply and
        // Mask are a*b; Subtract a*(1-b); Linear Burn max(0, a+b-1);
        // Overlay the two-branch curve. NormalOver and Screen have no
        // confining coverage meaning and fall back to Multiply (the 6d
        // rule); Darken (min) falls to Multiply, Color Burn and Hard Mix
        // to Linear Burn, Color Dodge to Overlay — nearest confined
        // shapes, reported as approximations.
        const QString blnm = descGet(dual, "BlnM").toString();
        ::Brush::DualBlendMode mode = ::Brush::DualBlendMode::Multiply;
        QString blendNote;
        if (blnm == QLatin1String("Mltp")) {
            mode = ::Brush::DualBlendMode::Multiply;
        } else if (blnm == QLatin1String("Sbtr")) {
            mode = ::Brush::DualBlendMode::Subtract;
        } else if (blnm == QLatin1String("Ovrl")) {
            mode = ::Brush::DualBlendMode::Overlay;
        } else if (blnm == QLatin1String("linearBurn")) {
            mode = ::Brush::DualBlendMode::LinearBurn;
        } else if (blnm == QLatin1String("Nrml")
                   || blnm == QLatin1String("Scrn")) {
            mode = ::Brush::DualBlendMode::Multiply;
            blendNote = QStringLiteral(
                "dual blend '%1' has no confining coverage meaning -> "
                "Multiply (the 6d fallback)").arg(blnm);
        } else if (blnm == QLatin1String("Drkn")) {
            mode = ::Brush::DualBlendMode::Multiply;
            blendNote = QStringLiteral(
                "dual blend Darken (min) -> Multiply (nearest confined)");
        } else if (blnm == QLatin1String("CBrn")
                   || blnm == QLatin1String("HrdM")) {
            mode = ::Brush::DualBlendMode::LinearBurn;
            blendNote = QStringLiteral(
                "dual blend '%1' -> Linear Burn (nearest confined)")
                            .arg(blnm);
        } else if (blnm == QLatin1String("CDdg")) {
            mode = ::Brush::DualBlendMode::Overlay;
            blendNote = QStringLiteral(
                "dual blend Color Dodge -> Overlay (nearest confined "
                "brightening)");
        } else if (!blnm.isEmpty()) {
            blendNote = QStringLiteral(
                "dual blend '%1' has no mapping -> Multiply used")
                            .arg(blnm);
        }
        b.setDualBlendMode(mode);
        if (!blendNote.isEmpty())
            rep.approximated.append(blendNote);
        rep.mapped.append(
            QStringLiteral("dual brush -> Modulate: %1, %2 px, spacing "
                           "%3%, scatter %4%%5, count %6, blend %7")
                .arg(bTipNote)
                .arg(sec.size())
                .arg(qRound(bSpacingPct))
                .arg(qRound(sec.scatterPerpendicular() * 100.0))
                .arg(sec.scatterAlong() > 0.0
                         ? QStringLiteral(" both axes") : QString())
                .arg(sec.scatterCount())
                .arg(blnm));
    }

    // --- Noise -> the 6e amount -------------------------------------------
    // Photoshop's Noise is a checkbox with no strength; 1.0 is the full
    // effect, the value the checkbox means (6e's mapping note).
    if (descGet(desc, "Nose").toBool()) {
        b.setNoise(1.0);
        rep.mapped.append(QStringLiteral("noise -> amount 100%"));
    }

    // --- Out-of-scope features: report, never approximate silently ---
    if (descGet(desc, "Wtdg").toBool())
        rep.dropped.append(QStringLiteral(
            "wet edges (engine composites differently)"));
    if (descGet(desc, "Rpt ").toBool())
        rep.dropped.append(QStringLiteral(
            "build-up / airbrush accumulation ('Rpt ' is a boolean toggle; "
            "6b's buildUp is an amount — mapping deferred)"));

    out.valid = true;
    return out;
}

// ---- File-level orchestration ----------------------------------------------

// Deterministic identity: the id is derived from the mapped content via a
// CANONICAL text serialisation plus the raw tip/grain pixels — deliberately
// NOT from BrushPresetCodec::saveBrush. The codec's bytes change with every
// wire-format bump (v1 -> v2 -> v3 already), and an id derived from them
// silently broke re-import idempotence at each bump: the same ABR hashed to
// a new id against the same stored brush. This fingerprint only moves when
// the MAPPING itself changes; when it does (as in the unbake that
// introduced it), BrushLibraryModel::importFile's upgrade path recognises
// the same source by name and upgrades in place, keeping the stored id.
void assignIdentity(BrushPreset &preset)
{
    preset.category = QStringLiteral("Imported");
    preset.builtin = false;
    preset.id = AbrImporter::contentId(preset);
}

} // namespace

QString AbrImporter::contentId(const BrushPreset &preset)
{
    const ::Brush &b = preset.brush;
    QString canon;
    QTextStream ts(&canon);
    ts.setRealNumberPrecision(9);
    ts << b.size() << ';' << b.spacing() << ';' << b.opacity() << ';'
       << b.flow() << ';' << b.hardness() << ';' << int(b.toolMode()) << ';'
       << b.tipAngle() << ';' << b.tipRoundness() << ';' << b.tipFlipX()
       << ';' << b.tipFlipY() << ';' << b.sizeJitter() << ';'
       << b.angleJitter() << ';' << b.roundnessJitter() << ';'
       << b.spacingJitter() << ';' << b.scatterAlong() << ';'
       << b.scatterPerpendicular() << ';' << b.scatterCount() << ';'
       << b.grainScale() << ';' << b.grainDepth() << ';'
       << int(b.grainMode()) << ';' << int(b.grainPreset()) << ';'
       // Generation 4: the texture blend mode is mapped content (two
       // brushes differing only here must not collide into one id).
       << int(b.textureBlendMode()) << ';';
    // The colour, noise and dual parameters joined the mapping in
    // generation 3 — they are fingerprint material for the same reason
    // everything above is: two brushes differing only here must not
    // collide into one id (the second would be silently skipped as a
    // duplicate on import).
    ts << b.hueJitter() << ';' << b.saturationJitter() << ';'
       << b.brightnessJitter() << ';' << b.fgBgJitter() << ';'
       << b.purity() << ';' << b.colorDynamicsPerTip() << ';'
       << b.noise() << ';' << b.dualBrushEnabled() << ';';
    if (b.dualBrushEnabled()) {
        const ::Brush &sec =
            const_cast<::Brush &>(b).secondaryBrush();
        ts << int(b.dualMode()) << ',' << int(b.dualBlendMode()) << ','
           << sec.size() << ',' << sec.spacing() << ',' << sec.hardness()
           << ',' << sec.tipAngle() << ',' << sec.tipRoundness() << ','
           << sec.scatterAlong() << ',' << sec.scatterPerpendicular()
           << ',' << sec.scatterCount() << ';';
    }
    bool fades = false;
    // WIDENED to 15 with the generation/3 bump IN THE SAME COMMIT — the
    // importer now maps colour dynamics, so ForegroundBackground (the
    // 15th property) participates in identity. The codec's v2 dynamics
    // block is a different surface and STAYS PINNED at 14: every v2-v6
    // file on disk carries exactly 14 entries. Widening here without the
    // generation bump (or bumping without widening) produces the
    // duplicate-beside-every-brush failure the tag exists to prevent.
    for (int i = 0; i < 15; ++i) {
        const auto property = ::Brush::DynamicProperty(i);
        ts << int(b.controlSource(property)) << ','
           << b.controlMinimum(property) << ',';
        for (const QPointF &pt :
             b.dynamicCurve(property).controlPoints())
            ts << pt.x() << ':' << pt.y() << ' ';
        ts << ';';
        fades = fades
            || b.controlSource(property) == ::Brush::ControlSource::Fade;
    }
    // fadeDistance joins the fingerprint ONLY for a brush that actually
    // fades. It is a mapped parameter now, so two brushes differing only
    // in fade length must not collide into one id (the second would be
    // silently skipped as a duplicate on import) — but appending it
    // unconditionally would move the id of every non-fading brush for a
    // value none of them read.
    if (fades)
        ts << "fade=" << b.fadeDistance() << ';';
    QCryptographicHash h(QCryptographicHash::Sha256);
    // MAPPING GENERATION. The library's upgrade-on-reimport path finds
    // old-generation presets by asking whether a stored preset's content
    // still fingerprints to its stored id. That test only fires when the
    // fingerprint FUNCTION changes — which is why it worked for the unbake
    // (it replaced codec-byte ids with this content hash) and why, without
    // this tag, it would silently fail for a MAPPING change: a preset
    // written by the previous importer is perfectly self-consistent, so it
    // would not be recognised, and re-importing the same ABR would add a
    // duplicate beside every brush instead of upgrading it. Measured: 1
    // stored preset became 13 on re-import before this tag existed.
    // Bump this string whenever the mapping changes what a given ABR
    // produces; nothing else needs to know. Generation 3: colour dynamics
    // (clVr/H/Strt/Brgh/purity/perTip), dual brush -> Modulate, and noise
    // joined the mapping. Generation 4: texture blend modes map natively
    // (previously every mode collapsed onto Multiply), and the mode joined
    // the fingerprint above IN THE SAME COMMIT.
    h.addData("abr-mapping-generation/4\n");
    h.addData(preset.name.toUtf8());
    h.addData("\n", 1);
    h.addData(canon.toUtf8());
    auto addImage = [&h](const QImage &img) {
        const QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
        h.addData(QByteArray::number(gray.width()));
        h.addData("x", 1);
        h.addData(QByteArray::number(gray.height()));
        for (int y = 0; y < gray.height(); ++y)
            h.addData(QByteArrayView(
                reinterpret_cast<const char *>(gray.constScanLine(y)),
                gray.width()));
    };
    if (b.hasCustomShape())
        addImage(b.customShape());
    if (b.grainPreset() == ::Brush::GrainPreset::Custom)
        addImage(b.grainTexture());
    // A sampled dual DONOR tip is mapped content too (generation 3): two
    // duals differing only in B's bitmap must not collide.
    if (b.dualBrushEnabled()) {
        const ::Brush &sec = const_cast<::Brush &>(b).secondaryBrush();
        if (sec.hasCustomShape())
            addImage(sec.customShape());
    }
    return QStringLiteral("user/abr-")
        + QString::fromLatin1(h.result().toHex().left(32));
}

namespace {

struct AbrParseResult
{
    QVector<BrushPreset> presets;
    QString report;
};

AbrParseResult parseAbr(const QByteArray &bytes)
{
    AbrParseResult result;
    QString &report = result.report;
    ByteReader r(bytes);
    const int version = r.u16();
    const int second = r.u16(); // brush count (v1/2) or subversion (v6+)
    if (!r.ok()) {
        report = QStringLiteral("Not an ABR file (too short).");
        return result;
    }

    QStringList fileNotes;
    QVector<AbrSample> samples;
    QVariantMap descRoot;
    QVector<AbrPattern> patterns;

    if (version == 1 || version == 2) {
        const int count = std::clamp(second, 0, kMaxBrushesPerFile);
        if (second > kMaxBrushesPerFile)
            fileNotes.append(
                QStringLiteral("declared brush count %1 capped at %2")
                    .arg(second).arg(kMaxBrushesPerFile));
        report += QStringLiteral(
            "ABR version %1 (legacy sampled-brush format), %2 brushes "
            "declared.\n").arg(version).arg(second);
        samples = parseLegacy(r, version, count, &fileNotes);
    } else if ((version == 6 || version == 7 || version == 9
                || version == 10)
               && (second == 1 || second == 2)) {
        report += QStringLiteral(
            "ABR version %1.%2 (8BIM samp/desc format).\n")
                      .arg(version).arg(second);
        QVector<Section> sections = scanSections(r);
        const Section *samp = nullptr, *desc = nullptr, *patt = nullptr;
        for (const Section &s : sections) {
            if (s.tag == "samp" && !samp)
                samp = &s;
            else if (s.tag == "desc" && !desc)
                desc = &s;
            else if (s.tag == "patt" && !patt)
                patt = &s;
        }
        if (samp) {
            ByteReader sr(bytes);
            samples = parseSampV6(sr, *samp, second, &fileNotes);
        }
        if (desc) {
            descRoot = parseDescSection(bytes, *desc);
            if (descRoot.isEmpty())
                fileNotes.append(QStringLiteral(
                    "descriptor section unreadable — names, spacing and "
                    "dynamics fall back to defaults"));
        }
        if (patt)
            patterns = parsePattSection(bytes, *patt, &fileNotes);
        if (!samp && descRoot.isEmpty()) {
            report += QStringLiteral(
                "No 'samp' or readable 'desc' section — nothing to "
                "import.\n");
            return result;
        }
    } else {
        // Recognizably ABR, but not a layout this importer understands:
        // refuse clearly by name rather than import a broken brush.
        report += QStringLiteral(
            "SankoTV cannot import this file: ABR version %1 (second "
            "field %2) is not a supported layout. Supported: versions 1 "
            "and 2 (legacy), and 6/7/9/10 with subversion 1 or 2 (the "
            "8BIM samp/desc format Photoshop CS through CC write).\n")
                      .arg(version).arg(second);
        return result;
    }

    // Pair descriptor brush entries with samp bitmaps. The desc 'Brsh'
    // list holds sampled AND computed brushes; sampled entries pair with
    // samp records by uuid where both sides carry one, positionally
    // otherwise (the order Photoshop writes is the samp order).
    QVariantList descBrushes =
        descGet(descRoot, "Brsh").toList();
    QVector<MappedBrush> mapped;
    int fallbackCounter = 0;
    auto fallbackName = [&fallbackCounter] {
        return QStringLiteral("Imported Brush %1").arg(++fallbackCounter);
    };

    if (!descBrushes.isEmpty()) {
        QVector<bool> claimed(samples.size(), false);
        auto claimNextUnclaimed = [&]() -> int {
            for (int i = 0; i < claimed.size(); ++i)
                if (!claimed[i])
                    return i;
            return -1;
        };
        // Dual-brush DONOR tips also live in 'samp'. Dual brush is dropped
        // by design, so claim those records up front: a donor must never
        // leak into the library as a standalone "Imported Brush N". Since
        // the dual mapping (6d Modulate) the claimed record is KEPT — it
        // becomes B's tip via donorByUuid rather than being discarded.
        QHash<QString, int> donorByUuid;
        for (const QVariant &entryVar : descBrushes) {
            const QVariantMap dual =
                descGet(entryVar.toMap(), "dualBrush").toMap();
            if (!descGet(dual, "useDualBrush").toBool())
                continue;
            const QString dualUuid =
                descGet(descGet(dual, "Brsh").toMap(), "sampledData")
                    .toString();
            if (dualUuid.isEmpty())
                continue;
            for (int i = 0; i < samples.size(); ++i)
                if (!claimed[i] && samples[i].uuid == dualUuid) {
                    claimed[i] = true;
                    donorByUuid.insert(dualUuid, i);
                }
        }
        for (const QVariant &entryVar : descBrushes) {
            const QVariantMap entry = entryVar.toMap();
            const QVariantMap tip = descGet(entry, "Brsh").toMap();
            const QString uuid = descGet(tip, "sampledData").toString();
            const AbrSample *sample = nullptr;
            if (!uuid.isEmpty()) {
                // Pair by uuid where the samp preamble carried one;
                // otherwise the first unclaimed record in file order (the
                // order Photoshop writes the descriptor in).
                int idx = -1;
                for (int i = 0; i < samples.size(); ++i)
                    if (!claimed[i] && samples[i].uuid == uuid) {
                        idx = i;
                        break;
                    }
                if (idx < 0)
                    idx = claimNextUnclaimed();
                if (idx >= 0) {
                    claimed[idx] = true;
                    sample = &samples[idx];
                }
            }
            if (sample && !sample->error.isEmpty()) {
                MappedBrush bad;
                bad.report.title = QStringLiteral("\"%1\" — could not be parsed: %2")
                                       .arg(descGet(entry, "Nm  ")
                                                .toString()
                                                .trimmed(),
                                            sample->error);
                mapped.append(bad);
                continue;
            }
            const AbrSample *dualSample = nullptr;
            {
                const QVariantMap dual =
                    descGet(entry, "dualBrush").toMap();
                if (descGet(dual, "useDualBrush").toBool()) {
                    const QString dualUuid =
                        descGet(descGet(dual, "Brsh").toMap(),
                                "sampledData").toString();
                    const int di = donorByUuid.value(dualUuid, -1);
                    if (di >= 0 && samples[di].error.isEmpty())
                        dualSample = &samples[di];
                }
            }
            mapped.append(buildBrush(sample, entry, patterns,
                                     fallbackName(), dualSample));
        }
        // samp records the descriptor never claimed still hold real tips.
        for (int i = 0; i < samples.size(); ++i) {
            if (claimed[i])
                continue;
            if (!samples[i].error.isEmpty()) {
                MappedBrush bad;
                bad.report.title = QStringLiteral(
                    "unnamed brush — could not be parsed: %1").arg(samples[i].error);
                mapped.append(bad);
                continue;
            }
            mapped.append(buildBrush(&samples[i], QVariantMap(), patterns,
                                     fallbackName()));
        }
    } else {
        for (const AbrSample &s : samples) {
            if (!s.error.isEmpty()) {
                MappedBrush bad;
                bad.report.title = s.legacyName.isEmpty()
                    ? QStringLiteral("brush — could not be parsed: %1").arg(s.error)
                    : QStringLiteral("\"%1\" — could not be parsed: %2")
                          .arg(s.legacyName, s.error);
                mapped.append(bad);
                continue;
            }
            mapped.append(buildBrush(&s, QVariantMap(), patterns,
                                     fallbackName()));
        }
    }

    int imported = 0;
    for (MappedBrush &m : mapped) {
        if (!m.valid)
            continue;
        assignIdentity(m.preset);
        if (m.report.title.isEmpty())
            m.report.title = QStringLiteral("\"%1\"").arg(m.preset.name);
        result.presets.append(m.preset);
        ++imported;
    }
    // PARSE-stage count only. Whether a parsed brush is then ADDED, found
    // ALREADY PRESENT, or UPGRADED in place is the library's decision —
    // BrushLibraryModel::importFile appends that application account below
    // this report. The old wording ("imported into the Imported category")
    // claimed an application outcome this stage cannot know, which is how
    // one report came to say both "1 of 1 imported" and "no new brushes".
    report += QStringLiteral("%1 of %2 brushes parsed from this file.\n\n")
                  .arg(imported).arg(mapped.size());
    for (const MappedBrush &m : mapped)
        m.report.write(&report);
    if (!fileNotes.isEmpty())
        report += QStringLiteral("\nFile notes:\n  - ")
            + fileNotes.join(QStringLiteral("\n  - ")) + QLatin1Char('\n');
    return result;
}

} // namespace

// ---- BrushImporter interface -----------------------------------------------

QString AbrImporter::name() const
{
    return QStringLiteral("Photoshop ABR");
}

bool AbrImporter::probe(const QByteArray &bytes) const
{
    // Claim only bytes that are recognizably ABR; never another format.
    // An ABR whose exact layout is unsupported is still claimed — import()
    // then refuses it BY NAME in the report, which beats a mute failure.
    if (bytes.size() < 10)
        return false;
    const uchar *d = reinterpret_cast<const uchar *>(bytes.constData());
    const int version = d[0] << 8 | d[1];
    const int second = d[2] << 8 | d[3];
    if (version == 1 || version == 2) {
        // Legacy: a sane count and a plausible first brush type.
        const int firstType = d[4] << 8 | d[5];
        return second >= 1 && second <= kMaxBrushesPerFile && firstType >= 1
            && firstType <= 5;
    }
    if (version >= 3 && version <= 10) // v6-family (and near-versions):
        return std::memcmp(d + 4, "8BIM", 4) == 0; // structure must confirm
    return false;
}

QVector<BrushPreset> AbrImporter::import(const QByteArray &bytes) const
{
    return importWithReport(bytes, nullptr);
}

QVector<BrushPreset>
AbrImporter::importWithReport(const QByteArray &bytes, QString *report) const
{
    const AbrParseResult result = parseAbr(bytes);
    if (report)
        *report = result.report;
    return result.presets;
}

} // namespace brushlib
