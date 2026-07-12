#include "PerspectiveTool.h"

#include <QJsonObject>
#include <QLineF>
#include <QPainter>
#include <QTransform>
#include <QtMath>

void PerspectiveTool::reset(const QSizeF &canvas)
{
    m_mode = TwoPoint;
    m_visible = false;
    m_snap = false;
    m_horizonY = canvas.height() * 0.4;
    m_vpX[0] = canvas.width() * 0.15;
    m_vpX[1] = canvas.width() * 0.85;
    m_zenith = QPointF(canvas.width() / 2.0, canvas.height() * 1.6);
    m_density = 12;
    m_color = QColor(0x4d, 0x9f, 0xff);
    m_opacity = 0.45;
    m_thickness = 1.0;
    m_lockedVp = -1;
}

QPointF PerspectiveTool::vanishingPoint(int index) const
{
    switch (index) {
    case 0: return QPointF(m_vpX[0], m_horizonY);
    case 1: return QPointF(m_vpX[1], m_horizonY);
    default: return m_zenith;
    }
}

void PerspectiveTool::setVanishingPoint(int index, const QPointF &canvasPos)
{
    // The horizon VPs ride the horizon: dragging one horizontally slides it,
    // vertically it carries the horizon (and the other horizon VP) with it.
    // The zenith/nadir VP is free. Positions may be far outside the canvas.
    switch (index) {
    case 0:
        m_vpX[0] = canvasPos.x();
        m_horizonY = canvasPos.y();
        break;
    case 1:
        m_vpX[1] = canvasPos.x();
        m_horizonY = canvasPos.y();
        break;
    default:
        m_zenith = canvasPos;
        break;
    }
}

// Guides: `density` full lines through each active VP, fanned evenly, long
// enough to cross any visible canvas ("infinite"); plus the horizon line.
// Cosmetic pens keep the on-screen thickness constant at any zoom/rotation.
void PerspectiveTool::paintGuides(QPainter &p, const QRectF &canvasRect) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity(m_opacity);
    QPen pen(m_color, m_thickness);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal reach = qMax(canvasRect.width(), canvasRect.height()) * 16.0;
    for (int v = 0; v < vanishingPointCount(); ++v) {
        const QPointF vp = vanishingPoint(v);
        for (int i = 0; i < m_density; ++i) {
            const qreal a = M_PI * i / m_density; // half-turn: distinct lines
            const QPointF dir(qCos(a), qSin(a));
            p.drawLine(vp - dir * reach, vp + dir * reach);
        }
    }

    // Horizon reads slightly stronger than the ray fans.
    pen.setWidthF(m_thickness + 0.8);
    p.setPen(pen);
    p.drawLine(QPointF(canvasRect.left() - reach, m_horizonY),
               QPointF(canvasRect.right() + reach, m_horizonY));
    p.restore();
}

// Editing handles (Perspective tool active): a ringed dot per VP, in WIDGET
// space so they stay grabbable at any zoom — and visible even when a VP sits
// outside the canvas bounds.
void PerspectiveTool::paintHandles(QPainter &p, const QTransform &canvasToWidget) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int v = 0; v < vanishingPointCount(); ++v) {
        const QPointF w = canvasToWidget.map(vanishingPoint(v));
        p.setPen(QPen(QColor(0, 0, 0, 150), 3.5));
        p.setBrush(m_color);
        p.drawEllipse(w, 7.0, 7.0);
        p.setPen(QPen(Qt::white, 1.8));
        p.drawEllipse(w, 7.0, 7.0);
    }
    p.restore();
}

PerspectiveTool::Handle PerspectiveTool::hitTest(const QPointF &widgetPos,
                                                 const QTransform &canvasToWidget) const
{
    for (int v = 0; v < vanishingPointCount(); ++v) {
        const QPointF w = canvasToWidget.map(vanishingPoint(v));
        if (QLineF(w, widgetPos).length() <= 11.0)
            return v == 0 ? HandleVp0 : (v == 1 ? HandleVp1 : HandleVp2);
    }
    // The horizon line itself (any point along it) within 8 screen px. Its
    // widget-space direction honours the view rotation/flip.
    const QPointF a = canvasToWidget.map(QPointF(-100000.0, m_horizonY));
    const QPointF b = canvasToWidget.map(QPointF(100000.0, m_horizonY));
    const QPointF ab = b - a;
    const qreal len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 > 1e-9) {
        const qreal t = QPointF::dotProduct(widgetPos - a, ab) / len2;
        const QPointF proj = a + ab * t;
        if (QLineF(proj, widgetPos).length() <= 8.0)
            return HandleHorizon;
    }
    return HandleNone;
}

void PerspectiveTool::dragHandle(Handle handle, const QPointF &canvasPos)
{
    switch (handle) {
    case HandleVp0: setVanishingPoint(0, canvasPos); break;
    case HandleVp1: setVanishingPoint(1, canvasPos); break;
    case HandleVp2: setVanishingPoint(2, canvasPos); break;
    case HandleHorizon: m_horizonY = canvasPos.y(); break;
    default: break;
    }
}

void PerspectiveTool::beginStroke(const QPointF &anchorCanvas)
{
    m_strokeAnchor = anchorCanvas;
    m_lockedVp = -1;
}

QPointF PerspectiveTool::snapPoint(const QPointF &canvasPos)
{
    if (!m_snap)
        return canvasPos;
    if (m_lockedVp < 0) {
        // Lock the ray once the stroke has a direction (Procreate-style
        // drawing assist): before that, stay put at the anchor.
        if (QLineF(m_strokeAnchor, canvasPos).length() < 6.0)
            return m_strokeAnchor;
        m_lockedVp = nearestRayVp(m_strokeAnchor, canvasPos);
        if (m_lockedVp < 0)
            return canvasPos;
    }
    return projectOntoLine(m_strokeAnchor, vanishingPoint(m_lockedVp), canvasPos);
}

QPointF PerspectiveTool::snapToRay(const QPointF &anchorCanvas,
                                   const QPointF &canvasPos) const
{
    if (!m_snap)
        return canvasPos;
    const int vp = nearestRayVp(anchorCanvas, canvasPos);
    if (vp < 0)
        return canvasPos;
    return projectOntoLine(anchorCanvas, vanishingPoint(vp), canvasPos);
}

// The active VP whose ray through `anchor` passes closest to `pos`.
int PerspectiveTool::nearestRayVp(const QPointF &anchor, const QPointF &pos) const
{
    int best = -1;
    qreal bestDist = 1e18;
    for (int v = 0; v < vanishingPointCount(); ++v) {
        const QPointF vp = vanishingPoint(v);
        if (QLineF(anchor, vp).length() < 4.0)
            continue; // anchor on the VP: the ray is undefined
        const QPointF proj = projectOntoLine(anchor, vp, pos);
        const qreal dist = QLineF(proj, pos).length();
        if (dist < bestDist) {
            bestDist = dist;
            best = v;
        }
    }
    return best;
}

QPointF PerspectiveTool::projectOntoLine(const QPointF &a, const QPointF &b,
                                         const QPointF &p)
{
    const QPointF ab = b - a;
    const qreal len2 = ab.x() * ab.x() + ab.y() * ab.y();
    if (len2 < 1e-12)
        return p;
    const qreal t = QPointF::dotProduct(p - a, ab) / len2;
    return a + ab * t;
}

QJsonObject PerspectiveTool::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("mode")] = int(m_mode);
    o[QStringLiteral("visible")] = m_visible;
    o[QStringLiteral("snap")] = m_snap;
    o[QStringLiteral("horizonY")] = m_horizonY;
    o[QStringLiteral("vp0x")] = m_vpX[0];
    o[QStringLiteral("vp1x")] = m_vpX[1];
    o[QStringLiteral("zenithX")] = m_zenith.x();
    o[QStringLiteral("zenithY")] = m_zenith.y();
    o[QStringLiteral("density")] = m_density;
    o[QStringLiteral("color")] = m_color.name(QColor::HexArgb);
    o[QStringLiteral("opacity")] = m_opacity;
    o[QStringLiteral("thickness")] = m_thickness;
    return o;
}

void PerspectiveTool::fromJson(const QJsonObject &o)
{
    if (o.isEmpty())
        return;
    const int mode = o.value(QStringLiteral("mode")).toInt(int(TwoPoint));
    m_mode = mode == 1 ? OnePoint : (mode == 3 ? ThreePoint : TwoPoint);
    m_visible = o.value(QStringLiteral("visible")).toBool(false);
    m_snap = o.value(QStringLiteral("snap")).toBool(false);
    m_horizonY = o.value(QStringLiteral("horizonY")).toDouble(m_horizonY);
    m_vpX[0] = o.value(QStringLiteral("vp0x")).toDouble(m_vpX[0]);
    m_vpX[1] = o.value(QStringLiteral("vp1x")).toDouble(m_vpX[1]);
    m_zenith.setX(o.value(QStringLiteral("zenithX")).toDouble(m_zenith.x()));
    m_zenith.setY(o.value(QStringLiteral("zenithY")).toDouble(m_zenith.y()));
    setDensity(o.value(QStringLiteral("density")).toInt(m_density));
    const QColor c(o.value(QStringLiteral("color")).toString());
    if (c.isValid())
        m_color = c;
    setOpacity(o.value(QStringLiteral("opacity")).toDouble(m_opacity));
    setThickness(o.value(QStringLiteral("thickness")).toDouble(m_thickness));
    m_lockedVp = -1;
}
