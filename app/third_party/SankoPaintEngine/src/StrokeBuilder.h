#pragma once

#include "Brush.h"
#include "GrainField.h"
#include "TiledImage.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QVector>

struct StrokePoint
{
    QPointF position;
    qreal pressure = 1.0;
    quint64 timestamp = 0;
    qreal tiltX = 0.0;
    qreal tiltY = 0.0;
    qreal rotation = 0.0;
    // Camera rotation at capture time when tablet orientation is active.
    // Subtracting this in stamp space keeps the physical pen orientation
    // stable on screen while the canvas view is rotated.
    qreal viewportRotation = 0.0;
};

struct StrokeStamp
{
    StrokePoint point;
    quint64 index = 0;
    qreal effectiveSize = 0.0;
    qreal effectiveOpacity = 0.0;
    qreal effectiveHardness = 0.0;
    qreal effectiveFlow = 1.0;
    qreal angleJitterDegrees = 0.0;
    qreal roundness = 1.0;
    qreal spacingMultiplier = 1.0;
    QPointF scatterOffset;
    qreal effectiveGrainDepth = 0.0;
    qreal effectiveSmudge = 0.0;
    qreal effectiveHueJitter = 0.0;
    qreal effectiveSaturationJitter = 0.0;
    qreal effectiveBrightnessJitter = 0.0;
    QColor resolvedColor = Qt::black; // Straight-alpha HSV result; premultiplied in the shader.
    // Per-stamp noise base seed (Phase 6e): indexedHash(strokeSeed, index,
    // property 105, slot), resolved once in resolveStamp so the GPU path —
    // which never sees the stroke seed — hashes the identical value. The
    // per-pixel key extends it inside NoiseField.
    quint32 noiseSeed = 0;
};

class StrokeBuilder
{
public:
    StrokeBuilder() = default;
    StrokeBuilder(const QSize &canvasSize, const Brush &brush, bool rasterizePreview = true,
                  quint64 seed = 0, quint32 subBrushSlot = 0);

    void reset(const QSize &canvasSize, const Brush &brush, bool rasterizePreview = true,
               quint64 seed = 0, quint32 subBrushSlot = 0);
    void addRawPoint(const StrokePoint &point);
    void addRawPoint(const QPointF &point); // Phase-1/mouse compatibility: full pressure.

    const QVector<StrokePoint> &rawPoints() const { return m_rawPoints; }
    const QVector<StrokePoint> &smoothedPoints() const { return m_smoothedPoints; }
    const QVector<StrokeStamp> &stamps() const { return m_stamps; }
    const QVector<QPointF> &stampPositions() const { return m_stampPositions; }
    // The stroke accumulator is 16-bit. This is its single 8-bit publication
    // boundary for layer compositing, tests, thumbnails, and save/load users.
    QImage strokeMask() const;
    int allocatedStrokeTileCount() const { return m_strokeMask.allocatedTileCount(); }
    QRect affectedRect() const { return m_affectedRect; }
    int tipCacheRegenerationCount() const { return m_brush.tipCacheRegenerationCount(); }
    int tipCacheEntryCount() const { return m_brush.tipCacheEntryCount(); }
    quint64 seed() const { return m_seed; }

    QImage previewImage() const { return m_previewTiles.toImage(); }
    const TiledImage &previewTiles() const { return m_previewTiles; }
    QImage compositeOnto(const QImage &destination) const;
    // UNORM16 stroke mask -> published 8-bit tile, applying the wet-edges
    // transfer at this single publication boundary (see WetEdges.h).
    QImage publishMask8(const QImage &source) const;
    QImage shapedTipForStamp(const StrokeStamp &stamp) const;

    static QVector<StrokePoint> smoothPath(const QVector<StrokePoint> &points, int radius = 2);
    static QVector<StrokeStamp> resamplePath(const QVector<StrokePoint> &points,
                                             const Brush &brush, quint64 seed = 0,
                                             quint32 subBrushSlot = 0);
    static void resolveColorDynamics(QVector<StrokeStamp> &stamps, const Brush &brush,
                                     const QImage &preStrokeRegion,
                                     const QPoint &regionCanvasOrigin);

private:
    // Per-stroke state for the dynamics SOURCES (Phase 3): accumulated arc
    // length for Fade and the smoothed heading for Direction. Advanced once
    // per stamp GROUP during the path walk — CPU-side, from the shared
    // point stream, so both render paths resolve identically. The heading
    // accumulator is distance-constant exponential smoothing of the
    // heading VECTOR (tau = 8 canvas px of arc; see advanceWalk): it
    // stays UNNORMALIZED so splitting a straight run into sub-steps
    // composes exactly — the same path at any sample rate produces the
    // same heading (the rate-independence the app-level stabilizer EMA
    // lacks, recorded in HANDOFF).
    struct DynamicWalkState
    {
        qreal arcLength = 0.0;           // canvas px along the walked path
        QPointF lastPosition;
        bool hasLast = false;
        QPointF headingAccum{1.0, 0.0};  // reads as +X before any movement
        bool headingSeeded = false;      // first real segment SNAPS, no blend
    };
    static StrokePoint interpolate(const StrokePoint &a, const StrokePoint &b, qreal t);
    static void advanceWalk(DynamicWalkState &walk, const QPointF &position,
                            const QPointF &direction);
    static Brush::DynamicSource sourceSample(const StrokePoint &point,
                                             const Brush &brush,
                                             const DynamicWalkState &walk,
                                             qreal *headingDegrees);
    static StrokeStamp resolveStamp(const StrokePoint &point, const Brush &brush,
                                    quint64 seed, quint64 index, const QPointF &direction,
                                    quint32 subBrushSlot,
                                    const Brush::DynamicSource &source,
                                    qreal headingDegrees);
    static qreal localSpacing(const StrokeStamp &stamp, const Brush &brush);
    qreal appendStampGroup(const StrokePoint &point, const QPointF &direction);
    void appendTimeStamps(const StrokePoint &a, const StrokePoint &b);
    void appendSmoothedPoint(const StrokePoint &point);
    void appendSegment(const StrokePoint &a, const StrokePoint &b);
    void placeStamp(const StrokeStamp &stamp);

    QSize m_canvasSize;
    Brush m_brush;
    GrainField m_grainField; // the shader's grain term, CPU side
    QVector<StrokePoint> m_rawPoints;
    QVector<StrokePoint> m_smoothedPoints;
    QVector<StrokeStamp> m_stamps;
    QVector<QPointF> m_stampPositions;
    TiledImage m_strokeMask{QImage::Format_Grayscale16};
    TiledImage m_previewTiles{QImage::Format_ARGB32_Premultiplied};
    QRect m_affectedRect;
    qreal m_distanceToNextStamp = 0.0;
    // Build-up: the timestamp (ms, qreal for sub-ms carry) of the most
    // recent emitted stamp of ANY kind — the time-walk's twin of
    // m_distanceToNextStamp. Time stamps fire at lastStamp + k*period, a
    // sequence independent of how events subdivide the hold.
    qreal m_lastStampTimeMs = -1.0;
    DynamicWalkState m_walk;
    quint64 m_seed = 0;
    quint64 m_nextStampIndex = 0;
    quint32 m_subBrushSlot = 0;
    bool m_rasterizePreview = true;
};
