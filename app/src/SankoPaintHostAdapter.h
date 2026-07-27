#pragma once

#include "Brush.h"
#include "BrushEngineIntegration.h"
#include "GpuStampRenderer.h"
#include "StrokeBuilder.h"
#include "TiledImage.h"

#include <QHash>
#include <QMutex>
#include <QString>
#include <memory>

// Narrow host boundary between SankoTV and the standalone engine.  The
// engine sees a stable string key, an RGBA image and canvas-space samples;
// it never sees Scene, Panel, Layer, groups, thumbnails, or QUndoStack.
class SankoPaintHostAdapter
{
public:
    struct StrokeWork {
        QString layerKey;
        QSize canvasSize;
        Brush brush;
        QVector<StrokePoint> rawPoints;
        QVector<StrokeStamp> primaryStamps;
        QVector<StrokeStamp> secondaryStamps;
        QRect affectedRect;
        QImage beforeRegion;
        QImage selectionMask; // optional canvas-sized immutable publication mask
        quint64 seed = 0;
        bool preferGpu = true;
        // Host image identity (QImage::cacheKey) when the stroke began. If it
        // no longer matches at publish time, a host-side edit landed while the
        // render was in flight and the stroke must be rebased over it.
        quint64 hostCacheKeyAtBegin = 0;
    };

    struct StrokeResult {
        bool succeeded = false;
        QString error;
        QString layerKey;
        QRect affectedRect;
        QImage afterRegion;
        BrushStrokeCommit commit;
        QHash<QPoint, QByteArray> afterHashes;
        GpuStampRenderer::Result renderer;
        StrokeWork replay;
    };

    SankoPaintHostAdapter();

    Brush &brush() { return m_brush; }
    const Brush &brush() const { return m_brush; }
    void setBrush(const Brush &brush) { m_brush = brush; }

    void synchronizeLayer(const QString &key, const QImage &hostPixels);
    void forgetLayer(const QString &key);
    // Authority-checked coherence read. If hostPixels changed since the last
    // engine sync (its cacheKey no longer matches), the HOST is authoritative:
    // the mirror is re-seeded from it and hostPixels stays untouched. Only a
    // host that is byte-identical to the last sync may be refreshed from the
    // mirror (a no-op by invariant, so nothing is copied). Returns true when
    // host and mirror are coherent, false when the layer has no mirror.
    bool refreshCoherentImage(const QString &key, QImage &hostPixels,
                              BrushCoherenceTrigger trigger);

    void beginStroke(const QString &key, const QImage &hostPixels,
                     const StrokePoint &point, quint64 seed = 0);
    QRect appendPoint(const StrokePoint &point);
    bool strokeActive() const { return bool(m_primaryStroke); }
    void cancelStroke();
    const TiledImage *previewTiles() const;
    StrokeWork finishStrokeWork(bool preferGpu = true);

    static StrokeResult render(const StrokeWork &work);
    static bool warmUpGpu();
    // QRhi resources must be destroyed on the thread that created them.
    // Hosts call this while their dedicated GPU worker is still alive.
    static void shutdownGpuForCurrentThread();
    // Mutates result when the stroke had to be rebased over a host edit that
    // landed while the GPU render was in flight (commit/hashes re-captured).
    bool publish(StrokeResult &result, QImage &hostPixels);
    bool applyCommit(const QString &key, const BrushStrokeCommit &commit,
                     bool after, QImage &hostPixels,
                     BrushCoherenceTrigger trigger);

    struct ReadbackStats {
        quint64 calls = 0;
        quint64 tiles = 0;
        QHash<BrushCoherenceTrigger, quint64> callsByTrigger;
    };
    ReadbackStats readbackStats() const { return m_readbackStats; }
    void resetReadbackStats() { m_readbackStats = {}; }

private:
    struct LayerState {
        TiledImage pixels{QImage::Format_ARGB32};
        quint64 hostCacheKey = 0;
    };
    LayerState &layer(const QString &key);
    void recordCoherence(BrushCoherenceTrigger trigger, int tiles);

    QHash<QString, LayerState> m_layers;
    Brush m_brush;
    QString m_activeLayerKey;
    quint64 m_strokeHostCacheKey = 0; // host identity at beginStroke
    std::unique_ptr<StrokeBuilder> m_primaryStroke;
    std::unique_ptr<StrokeBuilder> m_secondaryStroke;
    ReadbackStats m_readbackStats;
};
