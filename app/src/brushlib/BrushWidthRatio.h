#pragma once

#include "Brush.h"

#include <QByteArray>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QWaitCondition>

namespace brushlib {

// THE DISPLAY-SIZE LAYER (2026-09-29, user-approved; HANDOFF "THE SIZE
// NUMBER"). The engine's size is the tip FRAME's diameter; what the artist
// sees is narrower (a scan's empty border, a soft falloff, a tip whose
// short axis lies across the path). The UI therefore shows and accepts the
// VISIBLE WIDTH, and converts at the boundary:
//
//     display = engine x ratio          engine = display / ratio
//
// The engine, the shaders and the stamp assets are untouched: no pixel
// moves, and every approved brush still renders at its own engine size.
//
// DEFINITION (approved): the width ACROSS THE PATH of a straight,
// FULL-PRESSURE stroke, at the tip's BROAD heading, where the stroke's mean
// coverage is at least 10% OF THAT STROKE'S OWN PEAK.
//
// NOTHING IS REMEMBERED. The ratio is a pure function of the brush. It is
// not a preset field and is never written into a preset file, so there is
// no stored number to go stale when a brush is retuned, promoted,
// overridden, re-scanned or imported. Both caches are CONTENT-ADDRESSED by
// key(): a changed brush is a different key, and nothing is ever
// "invalidated" - the old entry is simply never asked for again.
// display = ratio x engine + offset. The offset is the antialiased fringe
// a stroke carries at every size (about a pixel on a hard edge): without
// it, one proportion measured at the reference read a pixel LOW at the
// pencils' 25 px defaults while being right at 120. Both come from two
// measurements (kReferenceSizeLow and kReferenceSize); nothing else.
struct WidthRatio
{
    double ratio = 1.0;     // slope the UI uses (1.0 when excluded)
    double offset = 0.0;    // px, the fringe (0 when excluded)
    double measured = 1.0;  // widthHigh / kReferenceSize: the raw proportion
    double widthLow = 0.0;  // the two measurements, in px
    double widthHigh = 0.0;
    bool excluded = false;  // scatter-envelope brush: the number is the
                            // DROPLET size, not the stroke width
};

class BrushWidthRatio
{
public:
    // Two reference sizes for every brush, 64 apart: the slope between
    // them is exact to 1/64 (widths are whole pixels) and the intercept is
    // the fringe. The low one sits where the defaults live; the high one
    // is still tens of milliseconds. One model is applied at EVERY size,
    // which is why a label can be 1-2 px off under ~8 px and on a sparse
    // stamp drawn small - see HANDOFF; accepted, not special-cased.
    static constexpr int kReferenceSize = 88;
    static constexpr int kReferenceSizeLow = 24;
    // Bump when measure() changes what it returns: it versions the disk
    // tier's directory, so old entries are never read under new rules.
    // r3: two sizes (24, 88) and the affine fit (r1 was one proportion at
    // 64; r2 was consumed by the two intermediate models tried on the
    // way). Entries also name their sizes, so a mismatch is refused even
    // when the revision is not bumped.
    static constexpr quint32 kRevision = 3;
    // Excluded = a brush WITH scatter whose measured ratio exceeds this.
    // "Has scatter" alone is the wrong test: the Drawing pastels and chalks
    // scatter too and measure 0.92-1.13.
    static constexpr double kScatterExclusion = 1.25;

    // SHA-256 over the codec's serialisation of the brush after normalising
    // ONLY what the number must not depend on: size -> kReferenceSize,
    // colour -> black, eraseMode -> off (the erase variant of a preset has
    // the same footprint). Every other field, tip and grain bytes included,
    // is in the key; a codec version bump re-keys everything.
    static QByteArray key(const ::Brush &brush);

    // The pure measurement. Deterministic; tens of milliseconds.
    static WidthRatio measure(const ::Brush &brush);

    // memory -> disk (when diskDir is given) -> measure. Thread-safe.
    // diskDir is the PREVIEW RENDERER's cache root, so tests inherit its
    // scratch override and never write the real cache.
    static WidthRatio ratioFor(const ::Brush &brush,
                               const QString &diskDir = QString());
    // Memory tier only - never measures. False when not yet known.
    static bool cached(const ::Brush &brush, WidthRatio *out);

    static int displaySize(const ::Brush &brush, int engineSize);
    // presetDefaultEngine: the active preset's own engine size. When the
    // requested display number is that preset's label, the engine gets
    // EXACTLY the preset's size back - so an approved brush's default is
    // always reachable, byte for byte, from its own label.
    static int engineSize(const ::Brush &brush, int displaySize,
                          int presetDefaultEngine = 0);
    // The same conversions on a ratio already in hand (the studio holds
    // the last one reported by its measurer rather than asking, so it
    // never measures on the UI thread).
    static int displaySizeWith(const WidthRatio &r, int engineSize);
    static int engineSizeWith(const WidthRatio &r, int displaySize,
                              int presetDefaultEngine = 0);

    // Test hooks.
    static void clearMemoryCacheForTest();
    static int measureCountForTest();
    static QThread *lastMeasureThreadForTest();
};

// OFF-THREAD measurement for a live editor (the studio): request() the
// brush as it now stands and ready(key, ratio) comes back on the caller's
// thread when the measurement is done - the UI keeps its last number until
// then and never stalls. Requests coalesce: only the newest queued brush is
// measured (an older state's answer would be stale on arrival); the one in
// flight completes and reports. The result also lands in the shared caches,
// so the same brush is never measured twice.
class BrushWidthMeasurer : public QObject
{
    Q_OBJECT
public:
    explicit BrushWidthMeasurer(QObject *parent = nullptr);
    ~BrushWidthMeasurer() override; // joins the worker; no hang

    void request(const ::Brush &brush, const QString &diskDir = QString());

signals:
    void ready(const QByteArray &key, const brushlib::WidthRatio &ratio);

private:
    struct Job
    {
        QByteArray key;
        ::Brush brush;
        QString diskDir;
    };
    void loop();

    QThread *m_thread = nullptr;
    QMutex m_mutex;
    QWaitCondition m_wake;
    QQueue<Job> m_queue;
    bool m_quit = false;
};

} // namespace brushlib

Q_DECLARE_METATYPE(brushlib::WidthRatio)
