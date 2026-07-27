#pragma once

#include "Brush.h"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QSet>

class DualBrushCompositor
{
public:
    // A and B are straight-alpha stroke images over transparency. B is
    // combined with A first; master opacity is applied to that result once,
    // then the combined source is published over the frozen layer.
    static QImage composite(const QImage &base, const QImage &strokeA,
                            const QImage &strokeB, Brush::DualBlendMode mode,
                            qreal masterOpacity);

    static QHash<QPoint, QImage> colorizeCoverageTiles(
        const QHash<QPoint, QImage> &coverageTiles, const QColor &color);
    static QImage tilesToRegion(const QHash<QPoint, QImage> &tiles,
                                const QRect &region);
    static QHash<QPoint, QImage> regionToTiles(const QImage &regionImage,
                                               const QPoint &regionCanvasOrigin,
                                               const QSet<QPoint> &tiles);
};
