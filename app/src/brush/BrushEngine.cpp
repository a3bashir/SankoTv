#include "BrushEngine.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

BrushEngine::BrushEngine(const QSize &canvasSize) : m_canvas(canvasSize)
{
}

void BrushEngine::setCanvasSize(const QSize &size)
{
    if (size.isValid())
        m_canvas = size;
}

// smoothstep(0,1,t): 3t^2 - 2t^3, clamped. Continuous first derivative, so the
// tip edge fades without a visible ring.
static double smoothstep(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Procedural round tip as an Alpha8 coverage mask.
//
//   d = distance from centre / radius, in [0, 1+]
//   hardness h in [0,1] sets the solid core: coverage is full for d <= h and
//   falls smoothly to 0 at d = 1. h = 1 => crisp disc (a 1px AA rim keeps the
//   edge from stair-stepping); h = 0 => the falloff spans the whole radius,
//   giving a soft, gaussian-like tip.
QImage BrushEngine::makeRoundTip(int diameterPx, double hardness)
{
    const int d = std::max(1, diameterPx);
    QImage tip(d, d, QImage::Format_Alpha8);
    tip.fill(0);
    const double r = d / 2.0;
    const double h = std::clamp(hardness, 0.0, 1.0);
    // A crisp tip still needs a sub-pixel rim to antialias; a soft tip fades
    // across the whole radius. inner = where the falloff starts.
    const double inner = (h >= 1.0) ? std::max(0.0, 1.0 - 1.0 / r) : h;
    const double span = std::max(1e-4, 1.0 - inner);
    for (int y = 0; y < d; ++y) {
        uchar *line = tip.scanLine(y);
        const double dy = (y + 0.5) - r;
        for (int x = 0; x < d; ++x) {
            const double dx = (x + 0.5) - r;
            const double dist = std::hypot(dx, dy) / r; // 0 at centre, 1 at rim
            double a;
            if (dist <= inner)
                a = 1.0;
            else if (dist >= 1.0)
                a = 0.0;
            else
                a = 1.0 - smoothstep((dist - inner) / span);
            line[x] = static_cast<uchar>(std::lround(std::clamp(a, 0.0, 1.0)
                                                     * 255.0));
        }
    }
    return tip;
}

void BrushEngine::ensureTip() const
{
    const int sizeKey = std::max(1, int(std::lround(m_brush.size)));
    const int hardKey = int(std::lround(std::clamp(m_brush.hardness, 0.0, 1.0)
                                        * 1000.0));
    const qint64 shapeKey = m_brush.shape.isNull() ? 0
                                                   : m_brush.shape.cacheKey();
    if (!m_tip.isNull() && sizeKey == m_tipSizeKey && hardKey == m_tipHardKey
        && shapeKey == m_tipShapeKey)
        return;

    if (m_brush.shape.isNull()) {
        m_tip = makeRoundTip(sizeKey, m_brush.hardness);
    } else {
        // Custom tip: scale to the brush size and reduce to an Alpha8 coverage
        // mask. A tip that carries alpha uses it directly; a flat opaque
        // grayscale image uses luminance (white = full, black = none).
        QImage src = m_brush.shape.scaled(sizeKey, sizeKey, Qt::IgnoreAspectRatio,
                                          Qt::SmoothTransformation);
        m_tip = QImage(sizeKey, sizeKey, QImage::Format_Alpha8);
        const bool hasAlpha = src.hasAlphaChannel();
        const QImage rgb = hasAlpha
            ? src.convertToFormat(QImage::Format_ARGB32)
            : src.convertToFormat(QImage::Format_RGB32);
        for (int y = 0; y < sizeKey; ++y) {
            uchar *dst = m_tip.scanLine(y);
            const QRgb *s = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
            for (int x = 0; x < sizeKey; ++x)
                dst[x] = static_cast<uchar>(
                    hasAlpha ? qAlpha(s[x])
                             : qGray(s[x])); // luminance = coverage
        }
    }
    m_tipSizeKey = sizeKey;
    m_tipHardKey = hardKey;
    m_tipShapeKey = shapeKey;
}

const QImage &BrushEngine::tipMask() const
{
    ensureTip();
    return m_tip;
}

// MAX-accumulate the tip's coverage into the stroke buffer at `center`. MAX
// (not additive) means overlapping stamps in one stroke settle at the single
// strongest coverage, so a slow scrub never darkens past one stamp.
void BrushEngine::stampAt(const QPointF &center)
{
    ensureTip();
    const int d = m_tip.width();
    if (d <= 0)
        return;
    const double half = d / 2.0;
    // Top-left of the tip in buffer pixels (rounded so integer-pixel copies
    // stay crisp; sub-pixel jitter is below the spacing resolution).
    const int ox = int(std::lround(center.x() - half));
    const int oy = int(std::lround(center.y() - half));

    const int x0 = std::max(0, ox);
    const int y0 = std::max(0, oy);
    const int x1 = std::min(m_strokeBuf.width(), ox + d);
    const int y1 = std::min(m_strokeBuf.height(), oy + d);
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int y = y0; y < y1; ++y) {
        const uchar *src = m_tip.constScanLine(y - oy);
        uchar *dst = m_strokeBuf.scanLine(y);
        for (int x = x0; x < x1; ++x) {
            const uchar s = src[x - ox];
            if (s > dst[x])
                dst[x] = s;
        }
    }
    m_dirty = m_dirty.united(QRect(x0, y0, x1 - x0, y1 - y0));
    m_stamps.append(center);
}

void BrushEngine::beginStroke(const QPointF &canvasPt)
{
    if (m_strokeBuf.size() != m_canvas
        || m_strokeBuf.format() != QImage::Format_Alpha8) {
        m_strokeBuf = QImage(m_canvas, QImage::Format_Alpha8);
    }
    m_strokeBuf.fill(0);
    m_dirty = QRect();
    m_stamps.clear();
    m_active = true;
    m_smoothPt = canvasPt;   // the stabiliser starts exactly on the first point
    m_lastStamp = canvasPt;
    m_residual = 0.0;
    stampAt(canvasPt);       // a dab on the initial press (tap = one dot)
}

void BrushEngine::extendStroke(const QPointF &canvasPt)
{
    if (!m_active)
        return;
    // Stabilise BEFORE stamping: an exponential moving average toward the raw
    // point cleans hand jitter (heavier m_smooth = calmer path, slight lag).
    m_smoothPt = m_smoothPt + (1.0 - m_smooth) * (canvasPt - m_smoothPt);

    // Walk from the last stamp to the smoothed point, dropping a stamp every
    // `step` px. The residual carries the leftover distance so density is
    // exact and independent of how far apart the input points arrive.
    const double step = std::max(0.5, m_brush.spacing * m_brush.size);
    QPointF from = m_lastStamp;
    QPointF to = m_smoothPt;
    double segLen = std::hypot(to.x() - from.x(), to.y() - from.y());
    if (segLen <= 0.0)
        return;

    double travelled = -m_residual; // negative => still owe distance to next stamp
    while (travelled + step <= segLen) {
        travelled += step;
        const double t = travelled / segLen;
        stampAt(from + (to - from) * t);
    }
    m_residual = segLen - travelled; // distance past the last stamp
    m_lastStamp = to;
}

QRect BrushEngine::compositeOnto(QImage &layer)
{
    const QRect region = m_dirty.intersected(layer.rect());
    m_active = false;
    if (region.isEmpty() || m_strokeBuf.isNull()) {
        m_strokeBuf = QImage();
        m_dirty = QRect();
        return QRect();
    }

    // Build the coloured stroke over the dirty rect: alpha = coverage *
    // opacity (capped at opacity, so overlaps never exceed the stroke's
    // opacity), premultiplied by the brush colour. One source-over onto the
    // layer composites the whole stroke as a single flat layer.
    const double op = std::clamp(m_brush.opacity, 0.0, 1.0);
    const int cr = m_brush.color.red();
    const int cg = m_brush.color.green();
    const int cb = m_brush.color.blue();
    QImage colored(region.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < region.height(); ++y) {
        const uchar *cov = m_strokeBuf.constScanLine(region.y() + y)
                         + region.x();
        QRgb *out = reinterpret_cast<QRgb *>(colored.scanLine(y));
        for (int x = 0; x < region.width(); ++x) {
            const int a = int(std::lround(cov[x] * op)); // 0..255, <= opacity*255
            // Premultiplied: colour channels scaled by the same alpha.
            out[x] = qRgba((cr * a + 127) / 255, (cg * a + 127) / 255,
                           (cb * a + 127) / 255, a);
        }
    }

    QPainter p(&layer);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.drawImage(region.topLeft(), colored);
    p.end();

    m_strokeBuf = QImage();
    m_dirty = QRect();
    return region;
}
