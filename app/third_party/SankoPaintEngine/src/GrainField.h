#pragma once

#include "Brush.h"

#include <QImage>
#include <QPointF>
#include <QtMath>

#include <algorithm>
#include <cmath>

// The ONE CPU implementation of the stamp shader's grain term. Both CPU
// consumers — StrokeBuilder::placeStamp (the R16 coverage path) and
// ColorStrokeBuffer::composite (the RGBA16 colour path) — sample grain
// through this class, so the CPU cannot fork against itself; and every
// term matches stamp.frag one for one:
//
//   shader                                here
//   ------                                ----
//   canvasPosition (StaticCanvas:         px,py = x+.5 / y+.5, minus the
//     absolute canvas px; Rolling:          stamp centre in Rolling mode
//     offset from the stamp centre)         (fragment-centre convention)
//   rot(uv) / grainScale                  (gc*px + gs*py) / scale, radians
//   texture(grainTexture, uv), sampler    wrapped() — bilinear over the
//     Linear/Repeat                         ORIGINAL texture, repeat wrap,
//                                           texel centres at (i+.5)/w
//   clamp((g-.5)*contrast+.5, 0, 1)       identical
//   mix(1, g, depth)                      1 - depth + g*depth
//
// depth is the stamp's RESOLVED effectiveGrainDepth — Phase 1's single
// dynamic resolver ran in StrokeBuilder::resolveStamp; there is no second
// resolution site here.
//
// StaticCanvas UVs depend only on absolute canvas coordinates, so the
// pattern is canvas-anchored, camera-invariant, and cannot seam at tile
// boundaries: tiles change where pixels are STORED, never the (x, y) fed
// to this function.
class GrainField
{
public:
    GrainField() = default;

    explicit GrainField(const Brush &brush)
        : m_grain(brush.hasGrain()
                      ? brush.grainTexture().convertToFormat(
                            QImage::Format_Grayscale8)
                      : QImage())
        , m_staticCanvas(brush.grainMode() == Brush::GrainMode::StaticCanvas)
        , m_scale(std::max<qreal>(brush.grainScale(), 1.0))
        , m_contrast(brush.grainContrast())
        , m_cos(std::cos(qDegreesToRadians(brush.grainRotation())))
        , m_sin(std::sin(qDegreesToRadians(brush.grainRotation())))
    {
    }

    bool isNull() const { return m_grain.isNull(); }

    // Grain modulation at canvas pixel (x, y): 1 at depth 0, the
    // contrast-shaped grain value at depth 1. stampCentre feeds Rolling
    // mode only. This is the MULTIPLY texture blend, kept verbatim — the
    // 62 built-ins render through this exact expression.
    qreal modulation(int x, int y, const QPointF &stampCentre,
                     qreal depth) const
    {
        const qreal px = m_staticCanvas ? x + .5 : x + .5 - stampCentre.x();
        const qreal py = m_staticCanvas ? y + .5 : y + .5 - stampCentre.y();
        const qreal u = (m_cos * px + m_sin * py) / m_scale;
        const qreal v = (-m_sin * px + m_cos * py) / m_scale;
        const qreal g = std::clamp(
            (wrapped(u, v) - .5) * m_contrast + .5, 0.0, 1.0);
        return 1.0 - depth + g * depth;
    }

    // The contrast-shaped texture value t itself — the same UVs, the same
    // anchoring, the same shaping as modulation(), WITHOUT the multiply
    // mix. The non-Multiply texture blend modes (TextureBlend.h) consume
    // this; sharing the sampling path is what keeps every mode on the
    // anchoring discipline the grain fix established (Rolling anchored to
    // the quantised raster centre, StaticCanvas to absolute canvas
    // coordinates — camera-invariant and tile-seam-free by construction).
    qreal shapedSample(int x, int y, const QPointF &stampCentre) const
    {
        const qreal px = m_staticCanvas ? x + .5 : x + .5 - stampCentre.x();
        const qreal py = m_staticCanvas ? y + .5 : y + .5 - stampCentre.y();
        const qreal u = (m_cos * px + m_sin * py) / m_scale;
        const qreal v = (-m_sin * px + m_cos * py) / m_scale;
        return std::clamp(
            (wrapped(u, v) - .5) * m_contrast + .5, 0.0, 1.0);
    }

private:
    // Bilinear with repeat addressing and texel centres at (i+.5)/w — the
    // Linear/Repeat sampler, verbatim (this body was ColorStrokeBuffer's
    // wrappedGrain, moved here so it exists once).
    qreal wrapped(qreal u, qreal v) const
    {
        u -= std::floor(u);
        v -= std::floor(v);
        const qreal x = u * m_grain.width() - .5;
        const qreal y = v * m_grain.height() - .5;
        const int x0 = (int(std::floor(x)) % m_grain.width()
                        + m_grain.width()) % m_grain.width();
        const int y0 = (int(std::floor(y)) % m_grain.height()
                        + m_grain.height()) % m_grain.height();
        const int x1 = (x0 + 1) % m_grain.width();
        const int y1 = (y0 + 1) % m_grain.height();
        const qreal fx = x - std::floor(x), fy = y - std::floor(y);
        const auto value = [&](int px, int py) {
            return m_grain.constScanLine(py)[px] / 255.0;
        };
        return (value(x0, y0) * (1.0 - fx) + value(x1, y0) * fx) * (1.0 - fy)
            + (value(x0, y1) * (1.0 - fx) + value(x1, y1) * fx) * fy;
    }

    QImage m_grain;
    bool m_staticCanvas = false;
    qreal m_scale = 1.0;
    qreal m_contrast = 1.0;
    qreal m_cos = 1.0;
    qreal m_sin = 0.0;
};
