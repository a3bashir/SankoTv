#include "BrushPreviewRenderer.h"

#include "BrushPresetCodec.h"
#include "SankoPaintHostAdapter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QStandardPaths>
#include <QtMath>

#include <memory>

namespace brushlib {
namespace {

constexpr quint64 kPreviewSeed = 4242;
constexpr int kSampleCount = 48;
constexpr int kSuper = 2; // render 2x, smooth-downscale for clean AA

// The swatch fixture path in 1x swatch coordinates.
struct SamplePoint
{
    QPointF pos;
    qreal pressure;
    qreal tiltX;
};

QVector<SamplePoint> samplePath()
{
    QVector<SamplePoint> pts;
    pts.reserve(kSampleCount);
    for (int i = 0; i < kSampleCount; ++i) {
        const qreal t = i / qreal(kSampleCount - 1);
        // Gentle two-crest wave across the swatch: direction changes reveal
        // angle-following tips and roundness.
        const qreal x = 10.0 + t * 202.0;
        const qreal y = 13.0 + 5.0 * qSin(t * 2.0 * M_PI * 1.25);
        // Pressure ramp 0.15 -> 1.0 (at 45%) -> 0.55: entry taper, full
        // body, relaxed exit. Flat-curve pens stay uniform.
        const qreal pressure = t < 0.45
            ? 0.15 + (t / 0.45) * 0.85
            : 1.0 - ((t - 0.45) / 0.55) * 0.45;
        // Tilt ramp 0 -> 40 degrees: tilt-responsive brushes broaden toward
        // the right end; tilt-disabled brushes stay constant, so one swatch
        // shows both characters.
        const qreal tiltX = t * 40.0;
        pts.append({QPointF(x, y), pressure, tiltX});
    }
    return pts;
}

// Preview brushes are rendered at a size that fits the 26px row; softness,
// grain, scatter and curves still convey the character. Deterministic.
int previewBrushSize(int size)
{
    return qBound(3, size, 16);
}

QImage smudgeBackground(const QSize &size)
{
    // Three opaque colour bands (the Phase 1 fixture colours): a smudge
    // stroke crossing them visibly DRAGS colour instead of depositing it.
    QImage bg(size, QImage::Format_ARGB32);
    QPainter p(&bg);
    const int w = size.width();
    p.fillRect(0, 0, w / 3, size.height(), QColor(0xc0, 0x30, 0x30));
    p.fillRect(w / 3, 0, w / 3, size.height(), QColor(0x30, 0xc0, 0x50));
    p.fillRect(2 * (w / 3), 0, w - 2 * (w / 3), size.height(),
               QColor(0x30, 0x50, 0xc0));
    return bg;
}

} // namespace

QImage BrushPreviewRenderer::renderPreviewImage(const ::Brush &brush)
{
    const QSize canvas(kSwatchW * kSuper, kSwatchH * kSuper);
    ::Brush b = brush;
    b.setSize(previewBrushSize(brush.size()) * kSuper);
    if (b.dualBrushEnabled())
        b.secondaryBrush().setSize(
            previewBrushSize(brush.secondaryBrush().size()) * kSuper);
    // Neutral ink for legibility on the row background; colour-jitter
    // brushes still show their hue spread because jitter modulates this
    // base. (Blue Pencil etc. keep their identity colour.)
    // The engine's Brush defaults to BLACK, and presets that never chose an
    // identity colour carried that default straight into the preview — a
    // black stroke on the dark rows, which is why previews were near
    // invisible. The comment above stated this neutral-ink intent for two
    // phases while no code implemented it. Default black = "no identity
    // colour" -> white ink; explicit identity colours are untouched.
    // (Fixture change: kSwatchRevision bumped so cached swatches re-key.)
    if (b.color() == QColor(Qt::black))
        b.setColor(Qt::white);
    if (b.dualBrushEnabled()
        && b.secondaryBrush().color() == QColor(Qt::black))
        b.secondaryBrush().setColor(Qt::white);

    StrokeBuilder sb(canvas, b, false, kPreviewSeed, 0);
    std::unique_ptr<StrokeBuilder> sb2;
    if (b.dualBrushEnabled())
        sb2 = std::make_unique<StrokeBuilder>(canvas, b.secondaryBrush(),
                                              false, kPreviewSeed, 1);
    const QVector<SamplePoint> path = samplePath();
    for (const SamplePoint &s : path) {
        StrokePoint p;
        p.position = s.pos * kSuper;
        p.pressure = s.pressure;
        p.tiltX = s.tiltX;
        sb.addRawPoint(p);
        if (sb2)
            sb2->addRawPoint(p);
    }

    SankoPaintHostAdapter::StrokeWork w;
    w.layerKey = QStringLiteral("brushlib-preview");
    w.canvasSize = canvas;
    w.brush = b;
    w.rawPoints = sb.rawPoints();
    w.primaryStamps = sb.stamps();
    if (sb2)
        w.secondaryStamps = sb2->stamps();
    w.affectedRect =
        sb.affectedRect().intersected(QRect(QPoint(0, 0), canvas));
    if (w.affectedRect.isEmpty())
        return QImage();
    const bool smudge = b.smudgeActive();
    QImage before(w.affectedRect.size(), QImage::Format_ARGB32);
    before.fill(Qt::transparent);
    if (smudge) {
        const QImage full = smudgeBackground(canvas);
        before = full.copy(w.affectedRect);
    }
    w.beforeRegion = before;
    w.seed = kPreviewSeed;
    w.preferGpu = false; // CPU: deterministic, and never touches the serial
                         // GPU commit worker the canvas strokes run on
    StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush, before,
                                        w.affectedRect.topLeft());
    if (sb2)
        StrokeBuilder::resolveColorDynamics(w.secondaryStamps,
                                            w.brush.secondaryBrush(), before,
                                            w.affectedRect.topLeft());
    const auto result = SankoPaintHostAdapter::render(w);
    if (!result.succeeded)
        return QImage();

    // Compose the full swatch: smudge swatches keep their colour-band
    // background (that IS the point); others stay transparent around the
    // stroke.
    QImage full(canvas, QImage::Format_ARGB32_Premultiplied);
    full.fill(Qt::transparent);
    QPainter p(&full);
    if (smudge)
        p.drawImage(0, 0, smudgeBackground(canvas));
    p.drawImage(w.affectedRect.topLeft(), result.afterRegion);
    p.end();
    return full.scaled(kSwatchW, kSwatchH, Qt::IgnoreAspectRatio,
                       Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_ARGB32);
}

BrushPreviewRenderer::BrushPreviewRenderer(const QString &cacheRootOverride,
                                           QObject *parent)
    : QObject(parent)
{
    m_cacheRoot = cacheRootOverride.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/brushPreviews")
        : cacheRootOverride;
    QDir().mkpath(cacheDir());
    prune(cacheDir());
    m_thread = QThread::create([this] { workerLoop(); });
    m_thread->setObjectName(QStringLiteral("BrushPreviewRenderer"));
    m_thread->start(QThread::LowPriority); // canvas strokes win the cores
}

BrushPreviewRenderer::~BrushPreviewRenderer()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        m_queue.clear();
        m_pendingIds.clear();
        m_wake.wakeAll();
    }
    m_thread->wait(); // one in-flight render at most (~tens of ms)
    delete m_thread;
}

QString BrushPreviewRenderer::cacheDir() const
{
    // The settingsHash key already covers the codec wire version (the hash
    // is over the serialised bytes, version header included), so a codec
    // bump invalidates every key by itself. The revision directory versions
    // THIS renderer's fixture: path, seed, sizing, background.
    return m_cacheRoot + QStringLiteral("/r%1").arg(kSwatchRevision);
}

void BrushPreviewRenderer::requestPreview(const QString &presetId,
                                          const ::Brush &brush)
{
    QMutexLocker lock(&m_mutex);
    if (m_quit || m_pendingIds.contains(presetId))
        return; // deduplicated: already queued or in flight
    m_pendingIds.insert(presetId);
    m_queue.enqueue({presetId, brush, m_epoch});
    m_wake.wakeOne();
}

void BrushPreviewRenderer::cancelAll()
{
    QMutexLocker lock(&m_mutex);
    ++m_epoch; // the in-flight job's result is dropped on this epoch check
    m_queue.clear();
    m_pendingIds.clear();
}

void BrushPreviewRenderer::cancelPreset(const QString &presetId)
{
    QMutexLocker lock(&m_mutex);
    m_pendingIds.remove(presetId); // in-flight result will not be emitted
    for (int i = m_queue.size() - 1; i >= 0; --i)
        if (m_queue.at(i).presetId == presetId)
            m_queue.removeAt(i);
}

void BrushPreviewRenderer::workerLoop()
{
    for (;;) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_queue.isEmpty())
                m_wake.wait(&m_mutex);
            if (m_quit)
                return;
            job = m_queue.dequeue();
        }
        const QImage image = loadOrRender(job.brush);
        {
            QMutexLocker lock(&m_mutex);
            // Dropped if cancelAll() bumped the epoch or cancelPreset()
            // removed the id (preset deleted mid-render): no dangling result.
            const bool stale =
                m_quit || job.epoch != m_epoch
                || !m_pendingIds.remove(job.presetId);
            if (stale)
                continue;
        }
        m_lastRenderThread = QThread::currentThread();
        emit previewReady(job.presetId, image);
    }
}

QImage BrushPreviewRenderer::loadOrRender(const ::Brush &brush)
{
    const QString key = QString::fromLatin1(
        BrushPresetCodec::settingsHash(brush).toHex());
    const QString file =
        cacheDir() + QLatin1Char('/') + key + QStringLiteral(".png");
    QImage cached(file);
    if (!cached.isNull())
        return cached.convertToFormat(QImage::Format_ARGB32);
    const QImage fresh = renderPreviewImage(brush);
    if (!fresh.isNull())
        fresh.save(file, "PNG");
    return fresh;
}

void BrushPreviewRenderer::prune(const QString &dir)
{
    // Previews are a few KB each; the cap only matters after many edited
    // variations accumulate. Oldest-first beyond 512 entries.
    QDir d(dir);
    QFileInfoList files =
        d.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::Time);
    for (int i = files.size() - 1; i >= 512; --i)
        QFile::remove(files.at(i).absoluteFilePath());
}

} // namespace brushlib
