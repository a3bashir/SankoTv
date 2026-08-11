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
    // combined with A first (Composite: the original two-stroke blend;
    // Modulate: Photoshop's semantics — B's ALPHA modulates A's coverage
    // through the blend mode, contributes no colour, and never paints
    // outside A). The primary's wet-edges transfer applies to the
    // COMBINED coverage at this publication boundary (the 6a question,
    // resolved in 6d) with the primary's opacity as the ceiling; master
    // opacity is applied once after it; then the source publishes over
    // the frozen layer. Mirrored line for line by dual_publish.frag.
    static QImage composite(const QImage &base, const QImage &strokeA,
                            const QImage &strokeB, Brush::DualBlendMode mode,
                            qreal masterOpacity,
                            Brush::DualMode dualMode = Brush::DualMode::Composite,
                            qreal wetEdges = 0.0, qreal wetCeiling = 1.0);

    static QHash<QPoint, QImage> colorizeCoverageTiles(
        const QHash<QPoint, QImage> &coverageTiles, const QColor &color);
    static QImage tilesToRegion(const QHash<QPoint, QImage> &tiles,
                                const QRect &region);
    static QHash<QPoint, QImage> regionToTiles(const QImage &regionImage,
                                               const QPoint &regionCanvasOrigin,
                                               const QSet<QPoint> &tiles);
};
