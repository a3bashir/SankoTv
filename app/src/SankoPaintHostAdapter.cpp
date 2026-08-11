#include "SankoPaintHostAdapter.h"

#include "ColorStrokeBuffer.h"
#include "DualBrushCompositor.h"

#include <QCryptographicHash>
#include <QPainter>
#include <QRandomGenerator>

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
    if (!seed)
        seed = QRandomGenerator::global()->generate64();
    // Very large CPU preview masks are the one operation that can still block
    // pointer delivery. The GPU commit remains identical; normal brushes keep
    // the immediate tiled preview, while >512 px strokes publish asynchronously.
    const bool rasterizePreview = rasterizePreviewWanted
                                  && m_brush.size() <= 512;
    m_primaryStroke = std::make_unique<StrokeBuilder>(
        hostPixels.size(), m_brush, rasterizePreview, seed, 0);
    m_primaryStroke->addRawPoint(point);
    if (m_brush.dualBrushEnabled()) {
        m_secondaryStroke = std::make_unique<StrokeBuilder>(
            hostPixels.size(), m_brush.secondaryBrush(),
            rasterizePreview && m_brush.secondaryBrush().size() <= 512,
            seed, 1);
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
    if (m_primaryStroke->previewTiles().allocatedTileCount() == 0
        && (!m_secondaryStroke
            || m_secondaryStroke->previewTiles().allocatedTileCount() == 0))
        return {};
    QRect after = m_primaryStroke->affectedRect();
    if (m_secondaryStroke)
        after = after.united(m_secondaryStroke->affectedRect());
    return before.united(after);
}

const TiledImage *SankoPaintHostAdapter::previewTiles() const
{
    return m_primaryStroke ? &m_primaryStroke->previewTiles() : nullptr;
}

void SankoPaintHostAdapter::cancelStroke()
{
    m_primaryStroke.reset();
    m_secondaryStroke.reset();
    m_activeLayerKey.clear();
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

    if (work.preferGpu) {
        out.renderer = work.brush.dualBrushEnabled()
            ? workerRenderer().renderDualStrokeForCommit(
                work.canvasSize, work.brush, work.primaryStamps,
                work.secondaryStamps, work.beforeRegion,
                work.affectedRect.topLeft())
            : workerRenderer().renderStrokeForCommit(
                work.canvasSize, work.brush, work.primaryStamps,
                work.beforeRegion, work.affectedRect.topLeft());
    }
    if (out.renderer.succeeded && !out.renderer.preparedLayer.isNull())
        out.afterRegion = out.renderer.preparedLayer;
    else {
        out.afterRegion = cpuComposite(work);
        out.renderer.usedCpuFallback = true;
        out.renderer.cpuBackingCoherent = true;
    }
    if (out.afterRegion.size() != work.affectedRect.size())
        out.afterRegion = out.afterRegion.copy(work.affectedRect);
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
