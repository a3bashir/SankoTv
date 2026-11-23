#include "SankoPaintHostAdapter.h"

#include "ColorStrokeBuffer.h"
#include "DualBrushCompositor.h"

#include <QCryptographicHash>
#include <QPainter>
#include <QRandomGenerator>

#include <cstring>

namespace {
thread_local GpuStampRenderer *g_workerRenderer = nullptr;

GpuStampRenderer &workerRenderer()
{
    // QRhi is thread-affine. The process-lifetime renderer remains on the
    // serial GPU worker used by the host canvas.
    if (!g_workerRenderer)
        g_workerRenderer = new GpuStampRenderer;
    return *g_workerRenderer;
}

QImage cpuSource(const QRect &region, const Brush &brush,
                 const QVector<StrokePoint> &rawPoints,
                 QVector<StrokeStamp> stamps, quint64 seed, quint32 slot,
                 bool forceMaskPath = false)
{
    QImage transparent(region.size(), QImage::Format_ARGB32);
    transparent.fill(Qt::transparent);
    if (brush.usesColorStrokeBuffer() && !forceMaskPath) {
        for (StrokeStamp &stamp : stamps)
            stamp.point.position -= region.topLeft();
        return ColorStrokeBuffer::composite(transparent, brush, stamps, slot);
    }
    StrokeBuilder rebuilt(region.size(), brush, true, seed, slot);
    for (StrokePoint point : rawPoints) {
        point.position -= region.topLeft();
        rebuilt.addRawPoint(point);
    }
    return rebuilt.compositeOnto(transparent);
}

QImage cpuComposite(const SankoPaintHostAdapter::StrokeWork &work)
{
    if (work.brush.dualBrushEnabled()) {
        const QImage a = cpuSource(work.affectedRect, work.brush, work.rawPoints,
                                   work.primaryStamps, work.seed, 0);
        // In Modulate mode only B's coverage is consumed, so B renders via
        // the mask path even when its preset would promote it to the
        // colour buffer — mirroring the GPU's forced-R16 secondary.
        const QImage b = cpuSource(work.affectedRect, work.brush.secondaryBrush(),
                                   work.rawPoints, work.secondaryStamps, work.seed, 1,
                                   work.brush.dualMode() == Brush::DualMode::Modulate);
        return DualBrushCompositor::composite(
            work.beforeRegion, a, b, work.brush.dualBlendMode(),
            work.brush.dualMasterOpacity(), work.brush.dualMode(),
            work.brush.smudgeActive() ? 0.0 : work.brush.wetEdges(),
            work.brush.opacity());
    }
    if (work.brush.usesColorStrokeBuffer()) {
        QVector<StrokeStamp> stamps = work.primaryStamps;
        for (StrokeStamp &stamp : stamps)
            stamp.point.position -= work.affectedRect.topLeft();
        return ColorStrokeBuffer::composite(work.beforeRegion, work.brush, stamps);
    }
    StrokeBuilder rebuilt(work.affectedRect.size(), work.brush, true, work.seed);
    for (StrokePoint point : work.rawPoints) {
        point.position -= work.affectedRect.topLeft();
        rebuilt.addRawPoint(point);
    }
    return rebuilt.compositeOnto(work.beforeRegion);
}

// ONE PASS of a stroke over work.beforeRegion, which lies at work.affectedRect: the GPU's
// result when it gives one, the CPU's otherwise. (What render() always did; a function of
// its own since batch H, whose mirrored stroke is a second pass.)
QImage renderPass(const SankoPaintHostAdapter::StrokeWork &work,
                  GpuStampRenderer::Result *renderer)
{
    GpuStampRenderer::Result result;
    if (work.preferGpu) {
        result = work.brush.dualBrushEnabled()
            ? workerRenderer().renderDualStrokeForCommit(
                work.canvasSize, work.brush, work.primaryStamps,
                work.secondaryStamps, work.beforeRegion,
                work.affectedRect.topLeft())
            : workerRenderer().renderStrokeForCommit(
                work.canvasSize, work.brush, work.primaryStamps,
                work.beforeRegion, work.affectedRect.topLeft());
    }
    QImage after;
    if (result.succeeded && !result.preparedLayer.isNull())
        after = result.preparedLayer;
    else {
        after = cpuComposite(work);
        result.usedCpuFallback = true;
        result.cpuBackingCoherent = true;
    }
    if (after.size() != work.affectedRect.size())
        after = after.copy(work.affectedRect);
    *renderer = result;
    return after;
}

// Rows copied as they are: both images Format_ARGB32, `from` laid at `at` in `into`.
void blitArgb32(QImage &into, const QPoint &at, const QImage &from)
{
    if (into.format() != QImage::Format_ARGB32 || from.format() != QImage::Format_ARGB32)
        return;
    const QRect target = QRect(at, from.size()).intersected(into.rect());
    if (target.isEmpty())
        return;
    for (int y = 0; y < target.height(); ++y)
        std::memcpy(into.scanLine(target.top() + y) + target.left() * 4,
                    from.constScanLine(target.top() - at.y() + y) + (target.left() - at.x()) * 4,
                    size_t(target.width()) * 4);
}

// SYMMETRY (batch H, 2026-11-22; Figma H-1): THE STROKE, THEN ITS MIRROR IMAGE OVER IT, as
// one after-region of work.affectedRect - one commit, so one Undo takes both sides away.
//   1. The stroke over its own rect (work.strokeRect): the very pass a stroke without
//      Symmetry is, from the same pixels - that side is what it would be without the mirror.
//   2. The mirror image: the picture as it now is under the stroke's REFLECTION is flipped
//      left-right, the same stroke is rendered over it, and the result flipped back. So the
//      second side is the pixel-for-pixel mirror of what the stroke does to the mirrored
//      picture - with every brush (grain, a tilted or textured tip, wet edges, smudge, a
//      dual brush) and with the Eraser, and where the two sides overlap the mirror image
//      lies over the stroke, as a stroke drawn second would. No stamp is rebuilt, no tip is
//      flipped by hand: the engine draws the one stroke twice.
// Only the part of the stroke whose reflection is on the canvas is mirrored (mirrorRects).
// Rows are copied as they are (ARGB32, straight alpha - the engine's own formats convert to
// it without loss), so a pixel neither pass touched keeps its bytes.
QImage renderMirrored(const SankoPaintHostAdapter::StrokeWork &work,
                      GpuStampRenderer::Result *renderer)
{
    const QRect whole = work.affectedRect;
    const QRect canvas(QPoint(0, 0), work.canvasSize);
    QImage current = work.beforeRegion.convertToFormat(QImage::Format_ARGB32);
    SankoPaintHostAdapter::StrokeWork first = work;
    first.mirror = false;
    first.affectedRect = work.strokeRect;
    first.beforeRegion = current.copy(work.strokeRect.translated(-whole.topLeft()));
    const QImage afterStroke = renderPass(first, renderer).convertToFormat(QImage::Format_ARGB32);
    blitArgb32(current, work.strokeRect.topLeft() - whole.topLeft(), afterStroke);

    QRect mirrored, reflection;
    SankoPaintHostAdapter::mirrorRects(work.strokeRect, work.mirrorAxis, canvas, &mirrored, &reflection);
    if (reflection.isEmpty() || !whole.contains(reflection))
        return current;
    SankoPaintHostAdapter::StrokeWork second = first;
    second.affectedRect = mirrored;
    second.beforeRegion = current.copy(reflection.translated(-whole.topLeft())).flipped(Qt::Horizontal);
    GpuStampRenderer::Result secondRenderer;
    const QImage afterMirror = renderPass(second, &secondRenderer)
                                   .convertToFormat(QImage::Format_ARGB32).flipped(Qt::Horizontal);
    blitArgb32(current, reflection.topLeft() - whole.topLeft(), afterMirror);
    if (secondRenderer.usedCpuFallback) // either pass on the CPU: the stroke was not all the GPU's
        renderer->usedCpuFallback = true;
    return current;
}
}

void SankoPaintHostAdapter::mirrorRects(const QRect &strokeRect, int axis, const QRect &canvas,
                                        QRect *mirrored, QRect *reflection)
{
    // What both sides of the axis have room for: a column further from the axis than that
    // has no place on the canvas for its reflection.
    const int room = qMin(axis - canvas.left(), canvas.left() + canvas.width() - axis);
    const QRect part = room > 0
        ? strokeRect.intersected(QRect(axis - room, canvas.top(), 2 * room, canvas.height()))
        : QRect();
    *mirrored = part;
    *reflection = part.isEmpty()
        ? QRect()
        : QRect(2 * axis - (part.left() + part.width()), part.top(), part.width(), part.height());
}

SankoPaintHostAdapter::SankoPaintHostAdapter() = default;

SankoPaintHostAdapter::LayerState &SankoPaintHostAdapter::layer(const QString &key)
{
    return m_layers[key];
}

void SankoPaintHostAdapter::synchronizeLayer(const QString &key,
                                             const QImage &hostPixels)
{
    if (key.isEmpty() || hostPixels.isNull())
        return;
    LayerState &state = layer(key);
    if (state.pixels.size() == hostPixels.size()
        && state.hostCacheKey == hostPixels.cacheKey())
        return;
    state.pixels.reset(hostPixels.size());
    state.pixels.load(hostPixels.convertToFormat(QImage::Format_ARGB32));
    state.pixels.takeDirtyTiles();
    state.hostCacheKey = hostPixels.cacheKey();
}

void SankoPaintHostAdapter::forgetLayer(const QString &key)
{
    m_layers.remove(key);
}

void SankoPaintHostAdapter::recordCoherence(BrushCoherenceTrigger trigger, int tiles)
{
    ++m_readbackStats.calls;
    m_readbackStats.tiles += quint64(qMax(0, tiles));
    ++m_readbackStats.callsByTrigger[trigger];
}

bool SankoPaintHostAdapter::refreshCoherentImage(const QString &key,
                                                 QImage &hostPixels,
                                                 BrushCoherenceTrigger trigger)
{
    const auto it = m_layers.find(key);
    if (it == m_layers.end())
        return false;
    // AUTHORITY CHECK. The mirror is only written together with the host
    // (publish/applyCommit/synchronizeLayer), each of which records the host
    // image's cacheKey afterwards. A differing key therefore means the HOST
    // changed since the last sync — a classic edit (erase, transform, fill,
    // shapes, paste bake, classic undo/redo) — and the host is authoritative:
    // re-seed the mirror from it and leave the host pixels alone. cacheKey is
    // (ser_no << 32 | detach_no): QPainter::begin, bits()/scanLine() and
    // whole-image assignment all change it, so classic edits cannot be missed.
    if (it->hostCacheKey != hostPixels.cacheKey()
        || it->pixels.size() != hostPixels.size()) {
        synchronizeLayer(key, hostPixels);
        return false;
    }
    // Keys match: host and mirror were last written together and the host is
    // untouched since, so their pixels are identical — nothing to copy. The
    // commit readback already populated the tiled CPU backing store; explicit
    // consumers are counted, but cause no GPU transfer while clean.
    recordCoherence(trigger, 0);
    return true;
}

void SankoPaintHostAdapter::beginStroke(const QString &key,
                                        const QImage &hostPixels,
                                        const StrokePoint &point, quint64 seed,
                                        bool rasterizePreviewWanted)
{
    synchronizeLayer(key, hostPixels);
    m_activeLayerKey = key;
    // Either synchronizeLayer just recorded this, or it matched already;
    // publish() compares against it to detect mid-flight host edits.
    m_strokeHostCacheKey = hostPixels.cacheKey();
    m_strokeMirror = m_mirror; // Symmetry as it is now, for the whole stroke (batch H)
    m_strokeMirrorAxis = m_mirrorAxis;
    if (!seed)
        seed = QRandomGenerator::global()->generate64();
    // DECIMATED LIVE PREVIEW. The per-stamp CPU preview raster costs
    // ~size^2 on the GUI thread (measured ~10 ms per pointer move at size
    // 500 on a 4K canvas), and the old fix was a gate: brushes over 512 px
    // simply had NO preview — the artist drew blind until the async publish
    // landed. Both problems are the same problem, and one mechanism ends
    // both: brushes over kPreviewFullResLimit rasterize their preview in a
    // SEPARATE builder at 1/k scale (k = ceil(size/256)), and the canvas
    // scales it back up when compositing. Cost per move is capped at a
    // ~256 px brush's (~2.6 ms at 4K); the edge blur of the upscale is
    // ~one preview texel = ~0.4% of the brush diameter at every size.
    //
    // THE PREVIEW BUILDER'S OUTPUT CANNOT REACH PUBLISHED PIXELS, and that
    // is the entire safety case: finishStrokeWork() reads stamps, points,
    // seed and affectedRect from m_primaryStroke ONLY — which stays at
    // full resolution, bounds-only — and render() re-rasterizes from
    // those. m_previewStroke is reachable solely through previewTiles(),
    // which only the canvas paint path consumes. Its stamps are never
    // read. (Pinned by the preview-invariance checks in the gate: same
    // stroke with preview on and off publishes byte-identical tiles.)
    const int kPreviewFullResLimit = 256;
    m_previewScale = rasterizePreviewWanted
        ? qMax(1, (m_brush.size() + kPreviewFullResLimit - 1)
                      / kPreviewFullResLimit)
        : 1;
    const bool fullResPreview = rasterizePreviewWanted && m_previewScale == 1;
    m_primaryStroke = std::make_unique<StrokeBuilder>(
        hostPixels.size(), m_brush, fullResPreview, seed, 0);
    m_primaryStroke->addRawPoint(point);
    if (rasterizePreviewWanted && m_previewScale > 1) {
        const int k = m_previewScale;
        ::Brush previewBrush = m_brush;
        previewBrush.setSize(qMax(1, m_brush.size() / k));
        // Grain pattern scale follows the decimation so textured presets
        // preview at roughly the right grain frequency. The NOISE field has
        // no such knob: noise-textured presets preview with noise at the
        // wrong spatial scale and settle at publish (recorded in HANDOFF as
        // a known in-flight divergence).
        previewBrush.setGrainScale(qMax<qreal>(1.0, m_brush.grainScale() / k));
        const QSize scaled((hostPixels.width() + k - 1) / k,
                           (hostPixels.height() + k - 1) / k);
        m_previewStroke = std::make_unique<StrokeBuilder>(
            scaled, previewBrush, true, seed, 0);
        StrokePoint scaledPoint = point;
        scaledPoint.position = point.position / qreal(k);
        m_previewStroke->addRawPoint(scaledPoint);
    } else {
        m_previewStroke.reset();
    }
    if (m_brush.dualBrushEnabled()) {
        // The secondary builder's raster is never displayed (previewTiles()
        // returns the primary's or the decimated builder's tiles), so it
        // runs bounds-only: it exists for its stamp list and affectedRect.
        m_secondaryStroke = std::make_unique<StrokeBuilder>(
            hostPixels.size(), m_brush.secondaryBrush(), false, seed, 1);
        m_secondaryStroke->addRawPoint(point);
    } else {
        m_secondaryStroke.reset();
    }
}

QRect SankoPaintHostAdapter::appendPoint(const StrokePoint &point)
{
    if (!m_primaryStroke)
        return {};
    const QRect before = m_primaryStroke->affectedRect();
    m_primaryStroke->addRawPoint(point);
    if (m_secondaryStroke)
        m_secondaryStroke->addRawPoint(point);
    if (m_previewStroke) {
        StrokePoint scaledPoint = point;
        scaledPoint.position = point.position / qreal(m_previewScale);
        m_previewStroke->addRawPoint(scaledPoint);
    }
    // The dirty rect is in CANVAS coordinates from the full-res builder,
    // whichever builder rasterized the pixels.
    const bool hasPreview = m_previewStroke
        ? m_previewStroke->previewTiles().allocatedTileCount() > 0
        : (m_primaryStroke->previewTiles().allocatedTileCount() > 0
           || (m_secondaryStroke
               && m_secondaryStroke->previewTiles().allocatedTileCount() > 0));
    if (!hasPreview)
        return {};
    QRect after = m_primaryStroke->affectedRect();
    if (m_secondaryStroke)
        after = after.united(m_secondaryStroke->affectedRect());
    return before.united(after);
}

const TiledImage *SankoPaintHostAdapter::previewTiles() const
{
    if (m_previewStroke)
        return &m_previewStroke->previewTiles();
    return m_primaryStroke ? &m_primaryStroke->previewTiles() : nullptr;
}

void SankoPaintHostAdapter::cancelStroke()
{
    m_primaryStroke.reset();
    m_secondaryStroke.reset();
    m_previewStroke.reset();
    m_previewScale = 1;
    m_activeLayerKey.clear();
    m_strokeMirror = false;
}

SankoPaintHostAdapter::StrokeWork
SankoPaintHostAdapter::finishStrokeWork(bool preferGpu)
{
    StrokeWork work;
    if (!m_primaryStroke)
        return work;
    work.layerKey = m_activeLayerKey;
    work.canvasSize = m_primaryStroke->previewTiles().size();
    work.brush = m_brush;
    work.rawPoints = m_primaryStroke->rawPoints();
    work.primaryStamps = m_primaryStroke->stamps();
    work.secondaryStamps = m_secondaryStroke
        ? m_secondaryStroke->stamps() : QVector<StrokeStamp>{};
    work.affectedRect = m_primaryStroke->affectedRect();
    if (m_secondaryStroke)
        work.affectedRect = work.affectedRect.united(m_secondaryStroke->affectedRect());
    const auto it = m_layers.constFind(work.layerKey);
    if (it != m_layers.cend()) {
        work.affectedRect = work.affectedRect.intersected(it->pixels.rect());
        if (m_strokeMirror) {
            // SYMMETRY (batch H): the commit's region also holds the stroke's mirror image
            // (render). A stroke with no part to mirror - none of it on the canvas, or its
            // reflection all off it - is an ordinary stroke.
            QRect mirrored, reflection;
            mirrorRects(work.affectedRect, m_strokeMirrorAxis, it->pixels.rect(), &mirrored, &reflection);
            if (!reflection.isEmpty()) {
                work.mirror = true;
                work.mirrorAxis = m_strokeMirrorAxis;
                work.strokeRect = work.affectedRect;
                work.affectedRect = work.affectedRect.united(reflection);
            }
        }
        work.beforeRegion = it->pixels.read(work.affectedRect);
    }
    work.seed = m_primaryStroke->seed();
    work.preferGpu = preferGpu;
    work.hostCacheKeyAtBegin = m_strokeHostCacheKey;
    StrokeBuilder::resolveColorDynamics(work.primaryStamps, work.brush,
                                        work.beforeRegion, work.affectedRect.topLeft());
    if (m_secondaryStroke)
        StrokeBuilder::resolveColorDynamics(work.secondaryStamps,
                                            work.brush.secondaryBrush(),
                                            work.beforeRegion,
                                            work.affectedRect.topLeft());
    m_primaryStroke.reset();
    m_secondaryStroke.reset();
    m_previewStroke.reset();
    m_previewScale = 1;
    m_strokeMirror = false;
    return work;
}

SankoPaintHostAdapter::StrokeResult
SankoPaintHostAdapter::render(const StrokeWork &work)
{
    StrokeResult out;
    out.layerKey = work.layerKey;
    out.affectedRect = work.affectedRect;
    out.replay = work;
    if (work.affectedRect.isEmpty() || work.beforeRegion.isNull()
        || work.primaryStamps.isEmpty())
        return out;

    // The stroke - and, with Symmetry, its mirror image over it (batch H) - as one region.
    const bool mirrored = work.mirror && !work.strokeRect.isEmpty()
        && work.affectedRect.contains(work.strokeRect);
    out.afterRegion = mirrored ? renderMirrored(work, &out.renderer) : renderPass(work, &out.renderer);
    if (!work.selectionMask.isNull()) {
        const QImage coverage = work.selectionMask.copy(work.affectedRect)
            .convertToFormat(QImage::Format_Grayscale8);
        QImage masked = out.afterRegion.convertToFormat(
            QImage::Format_ARGB32_Premultiplied);
        const QImage before = work.beforeRegion.convertToFormat(
            QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < masked.height(); ++y) {
            QRgb *dst = reinterpret_cast<QRgb *>(masked.scanLine(y));
            const QRgb *src = reinterpret_cast<const QRgb *>(before.constScanLine(y));
            const uchar *alpha = coverage.constScanLine(y);
            for (int x = 0; x < masked.width(); ++x) {
                const int a = alpha[x], inv = 255 - a;
                dst[x] = qRgba((qRed(dst[x]) * a + qRed(src[x]) * inv + 127) / 255,
                               (qGreen(dst[x]) * a + qGreen(src[x]) * inv + 127) / 255,
                               (qBlue(dst[x]) * a + qBlue(src[x]) * inv + 127) / 255,
                               (qAlpha(dst[x]) * a + qAlpha(src[x]) * inv + 127) / 255);
            }
        }
        out.afterRegion = masked;
    }

    TiledImage before(QImage::Format_ARGB32);
    before.reset(work.canvasSize);
    before.writeCoherent(work.affectedRect, work.beforeRegion);
    out.commit = BrushStrokeCommit::capture(
        before, work.affectedRect, out.afterRegion, work.seed);
    for (const BrushTilePatch &patch : out.commit.tilePatches)
        out.afterHashes.insert(patch.coordinate, QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(patch.after.constBits()),
                           patch.after.sizeInBytes()),
            QCryptographicHash::Sha256));
    out.succeeded = out.commit.isValid();
    out.error = out.succeeded ? QString() : out.renderer.error;
    return out;
}

bool SankoPaintHostAdapter::warmUpGpu()
{
    ::Brush brush;
    brush.setSize(8);
    brush.setSpacing(0.25);
    StrokeBuilder stroke(QSize(32, 32), brush, false, 1);
    stroke.addRawPoint(QPointF(16, 16));
    QImage base(32, 32, QImage::Format_ARGB32);
    base.fill(Qt::transparent);
    const auto result = workerRenderer().renderStrokeForCommit(
        base.size(), brush, stroke.stamps(), base, QPoint());
    return result.succeeded;
}

void SankoPaintHostAdapter::shutdownGpuForCurrentThread()
{
    delete g_workerRenderer;
    g_workerRenderer = nullptr;
}

bool SankoPaintHostAdapter::publish(StrokeResult &result, QImage &hostPixels)
{
    if (!result.succeeded || result.layerKey.isEmpty())
        return false;
    // MID-FLIGHT AUTHORITY CHECK. The commit rendered the stroke over the
    // pixels captured at stroke begin. If the host image changed while the
    // render was in flight (a classic edit landed, or a coherence pass
    // re-synced), re-composite the stroke over the CURRENT pixels on this
    // thread (CPU path — same resolved stamps and seed, deterministic)
    // instead of clobbering the newer edit with a stale beforeRegion.
    if (hostPixels.cacheKey() != result.replay.hostCacheKeyAtBegin) {
        StrokeWork work = result.replay;
        work.affectedRect = work.affectedRect.intersected(hostPixels.rect());
        if (work.affectedRect.isEmpty())
            return false;
        work.beforeRegion = hostPixels.copy(work.affectedRect)
                                .convertToFormat(QImage::Format_ARGB32);
        work.preferGpu = false; // synchronous CPU rebase, no GPU on this thread
        work.hostCacheKeyAtBegin = hostPixels.cacheKey();
        StrokeResult rebased = render(work);
        if (!rebased.succeeded)
            return false;
        result = rebased; // commit + hashes re-captured over the current host
        // The mirror may not have seen the mid-flight host edit either:
        // re-seed it from the current pixels before layering the stroke
        // region on top (no-op if a coherence pass already re-synced).
        synchronizeLayer(result.layerKey, hostPixels);
    }
    LayerState &state = layer(result.layerKey);
    if (state.pixels.size() != hostPixels.size())
        synchronizeLayer(result.layerKey, hostPixels);
    state.pixels.writeCoherent(result.affectedRect, result.afterRegion);
    QPainter painter(&hostPixels);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(result.affectedRect.topLeft(), result.afterRegion);
    painter.end();
    state.hostCacheKey = hostPixels.cacheKey();
    recordCoherence(BrushCoherenceTrigger::StrokeCommit,
                    result.commit.dirtyTiles.size());
    return true;
}

bool SankoPaintHostAdapter::applyCommit(const QString &key,
                                        const BrushStrokeCommit &commit,
                                        bool after, QImage &hostPixels,
                                        BrushCoherenceTrigger trigger)
{
    if (commit.tilePatches.isEmpty() || commit.affectedRect.isEmpty())
        return false;
    synchronizeLayer(key, hostPixels);
    LayerState &state = layer(key);
    if (after)
        commit.applyAfter(state.pixels);
    else
        commit.applyBefore(state.pixels);
    const QImage region = state.pixels.read(commit.affectedRect);
    QPainter painter(&hostPixels);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(commit.affectedRect.topLeft(), region);
    painter.end();
    state.hostCacheKey = hostPixels.cacheKey();
    recordCoherence(trigger, commit.dirtyTiles.size());
    return true;
}
