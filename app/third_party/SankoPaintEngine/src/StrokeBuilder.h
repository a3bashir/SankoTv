#pragma once

#include "Brush.h"
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
    QImage shapedTipForStamp(const StrokeStamp &stamp) const;

    static QVector<StrokePoint> smoothPath(const QVector<StrokePoint> &points, int radius = 2);
    static QVector<StrokeStamp> resamplePath(const QVector<StrokePoint> &points,
                                             const Brush &brush, quint64 seed = 0,
                                             quint32 subBrushSlot = 0);
    static void resolveColorDynamics(QVector<StrokeStamp> &stamps, const Brush &brush,
                                     const QImage &preStrokeRegion,
                                     const QPoint &regionCanvasOrigin);

private:
    static StrokePoint interpolate(const StrokePoint &a, const StrokePoint &b, qreal t);
    static StrokeStamp resolveStamp(const StrokePoint &point, const Brush &brush,
                                    quint64 seed, quint64 index, const QPointF &direction,
                                    quint32 subBrushSlot);
    static qreal localSpacing(const StrokeStamp &stamp, const Brush &brush);
    qreal appendStampGroup(const StrokePoint &point, const QPointF &direction);
    void appendSmoothedPoint(const StrokePoint &point);
    void appendSegment(const StrokePoint &a, const StrokePoint &b);
    void placeStamp(const StrokeStamp &stamp);

    QSize m_canvasSize;
    Brush m_brush;
    QVector<StrokePoint> m_rawPoints;
    QVector<StrokePoint> m_smoothedPoints;
    QVector<StrokeStamp> m_stamps;
    QVector<QPointF> m_stampPositions;
    TiledImage m_strokeMask{QImage::Format_Grayscale16};
    TiledImage m_previewTiles{QImage::Format_ARGB32_Premultiplied};
    QRect m_affectedRect;
    qreal m_distanceToNextStamp = 0.0;
    quint64 m_seed = 0;
    quint64 m_nextStampIndex = 0;
    quint32 m_subBrushSlot = 0;
    bool m_rasterizePreview = true;
};
