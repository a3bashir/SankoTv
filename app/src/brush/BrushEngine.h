#pragma once

#include "Brush.h"

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QVector>

// Phase 1 CPU brush engine: procedural/loaded tip, input smoothing, spacing
// resample, and source-over stamping onto a per-stroke buffer.
//
// Lifecycle (mouse-only in Phase 1; pressure arrives in Phase 2):
//   beginStroke(p)  -> reset state, seed the smoother, stamp the first dab
//   extendStroke(p) -> smooth the point, walk the new segment stamping the
//                      tip every (spacing * size) px (residual carried across
//                      calls so density is independent of input rate)
//   compositeOnto(layer) -> multiply the accumulated coverage by opacity,
//                      colourise, and source-over onto the layer ONCE; returns
//                      the affected rect (empty if nothing was drawn)
//
// The stroke buffer accumulates coverage with a MAX rule, so overlapping
// stamps within one stroke never exceed full coverage — no double-darkening;
// the final alpha is capped at brush.opacity.
class BrushEngine
{
public:
    explicit BrushEngine(const QSize &canvasSize = QSize(960, 540));

    Brush &brush() { return m_brush; }
    const Brush &brush() const { return m_brush; }
    void setCanvasSize(const QSize &size);

    // The cached grayscale tip (Alpha8). Rebuilt lazily when size/hardness or
    // a custom shape changes, never per stamp.
    const QImage &tipMask() const;

    // Smoothing strength, 0 (raw input) .. ~0.95 (heavy stabilisation). The
    // default is a light stabiliser that cleans hand jitter without lag.
    void setSmoothing(double factor) { m_smooth = qBound(0.0, factor, 0.95); }

    void beginStroke(const QPointF &canvasPt);
    void extendStroke(const QPointF &canvasPt);
    // Bake the finished stroke onto `layer` (ARGB32_Premultiplied). Returns the
    // dirty rect. Also clears the stroke so the engine is ready for the next.
    QRect compositeOnto(QImage &layer);

    // --- Introspection (used by the verification seam) --------------------
    const QVector<QPointF> &stampPositions() const { return m_stamps; }
    const QImage &strokeBuffer() const { return m_strokeBuf; } // Alpha8 coverage
    bool strokeActive() const { return m_active; }

    // Build a procedural round tip as a standalone helper (also used
    // internally). diameterPx >= 1; hardness 0..1.
    static QImage makeRoundTip(int diameterPx, double hardness);

private:
    void ensureTip() const;
    void stampAt(const QPointF &center); // MAX-accumulate the tip into the buffer

    Brush m_brush;
    QSize m_canvas;

    // Tip cache + the keys that invalidate it.
    mutable QImage m_tip;
    mutable int m_tipSizeKey = -1;
    mutable int m_tipHardKey = -1;
    mutable qint64 m_tipShapeKey = 0;

    // Stroke state.
    QImage m_strokeBuf;          // Alpha8 canvas-sized coverage accumulator
    QRect m_dirty;               // union of every stamped tip rect
    bool m_active = false;
    QPointF m_smoothPt;          // running smoothed position (EMA stabiliser)
    QPointF m_lastStamp;         // last stamped centre (spacing origin)
    double m_residual = 0.0;     // leftover distance carried between segments
    double m_smooth = 0.35;      // stabiliser factor
    QVector<QPointF> m_stamps;   // every stamped centre this stroke (for tests)
};
