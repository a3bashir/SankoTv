#pragma once

#include "Brush.h"
#include "StrokeBuilder.h"
#include "TiledImage.h"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QSet>
#include <QString>
#include <memory>

class QRhi;
class QRhiTexture;
class QRhiSampler;

class GpuStampRenderer
{
public:
    struct Result {
        bool succeeded = false;
        QString error;
        QHash<QPoint, QImage> masks;
        QHash<QPoint, QImage> previewTiles;
        QImage preparedLayer; // Worker-composited coherent CPU backing store.
        int instancedDrawCalls = 0;
        int logicalStamps = 0;
        int stampInstances = 0;
        int instanceBufferBytes = 0;
        int strokeTargetBytesPerTile = 0;
        int baseTipUploads = 0;
        int grainTextureUploads = 0;
        qint64 grainTextureBytes = 0;
        QSet<QPoint> dirtyTiles;
        int uploadedLayerTiles = 0;
        qint64 elapsedMs = 0;
        double totalPreciseMs = 0.0;
        double resourcePipelineMs = 0.0;
        double instanceBuildUploadMs = 0.0;
        double renderSubmissionMs = 0.0;
        double publicationSubmissionMs = 0.0;
        double readbackSubmissionMs = 0.0;
        double readbackConversionMs = 0.0;
        double finalGpuWaitMs = 0.0;
        double gpuFrameMs = 0.0; // Whole QRhi offscreen frame, when timestamps are enabled.
        double unattributedCpuMs = 0.0;
        int publicationDrawCalls = 0;
        int primaryLogicalStamps = 0;
        int secondaryLogicalStamps = 0;
        int primaryStampInstances = 0;
        int secondaryStampInstances = 0;
        int primaryStampDrawCalls = 0;
        int secondaryStampDrawCalls = 0;
        int primaryInstanceStride = 0;
        int secondaryInstanceStride = 0;
        qint64 primaryStrokeBufferBytes = 0;
        qint64 secondaryStrokeBufferBytes = 0;
        bool dualBrush = false;
        int readbackBatches = 0;
        qint64 readbackBytes = 0;
        qint64 strokeBufferReadbackBytes = 0;
        qint64 publicationReadbackBytes = 0;
        bool compositedOnGpu = false;
        bool cpuBackingCoherent = true;
        bool usedCpuFallback = false;
        QString strokeBufferFormat;
        // Outer worker-envelope timing. Renderer phase timers remain disjoint;
        // these fields attribute queue/callback latency around them.
        double workerQueueMs = 0.0;
        double workerEnvelopeMs = 0.0;
        qint64 workerFinishedMonotonicNs = 0;
    };

    GpuStampRenderer();
    ~GpuStampRenderer();

    bool isAvailable() const { return m_rhi != nullptr; }
    QString backendName() const;
    Result renderStroke(const QSize &layerSize, const Brush &brush,
                        const QVector<StrokeStamp> &stamps);
    Result renderStrokeForCommit(const QSize &layerSize, const Brush &brush,
                                 const QVector<StrokeStamp> &stamps,
                                 const QImage &baseRegion, const QPoint &regionCanvasOrigin,
                                 bool deferredReadback = false);
    Result renderDualStrokeForCommit(const QSize &layerSize, const Brush &brush,
                                     const QVector<StrokeStamp> &primaryStamps,
                                     const QVector<StrokeStamp> &secondaryStamps,
                                     const QImage &baseRegion,
                                     const QPoint &regionCanvasOrigin);
    int uploadLayerTiles(const TiledImage &layer, const QSet<QPoint> &dirtyTiles);
    void resetLayerTextures();

private:
    bool createRhi();
    Result renderStrokeInternal(const QSize &layerSize, const Brush &brush,
                                const QVector<StrokeStamp> &stamps,
                                const QImage *baseRegion, const QPoint &regionCanvasOrigin,
                                bool deferredReadback);
    Result renderColorStrokeInternal(const QSize &layerSize, const Brush &brush,
                                    const QVector<StrokeStamp> &stamps,
                                    const QImage *baseRegion,
                                    const QPoint &regionCanvasOrigin,
                                    bool deferredReadback);
    Result publishDualTiles(const Brush &brush,
                            const QHash<QPoint, QImage> &primaryTiles,
                            const QHash<QPoint, QImage> &secondaryTiles,
                            const QImage &baseRegion,
                            const QPoint &regionCanvasOrigin);
    QRhiSampler *linearClampSampler();
    QRhiSampler *linearRepeatSampler();
    QRhiSampler *nearestClampSampler();

    std::unique_ptr<QRhi> m_rhi;
    std::unique_ptr<QObject> m_fallbackSurface;
    QHash<QPoint, QRhiTexture *> m_layerTextures;
    // Phase 5 keeps A and B resources resident independently. Alternating
    // custom tips/grains must not cause two texture uploads on every preview.
    QHash<quint64, QRhiTexture *> m_grainTexturesGpu;
    QHash<quint64, QRhiTexture *> m_tipTexturesGpu;
    QRhiSampler *m_linearClampSampler = nullptr;
    QRhiSampler *m_linearRepeatSampler = nullptr;
    QRhiSampler *m_nearestClampSampler = nullptr;
};
