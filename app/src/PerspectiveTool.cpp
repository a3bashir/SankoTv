#include "PerspectiveTool.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QtMath>

namespace {
// A tap this close to the existing horizon (canvas px) cannot create VP3 —
// the third point must sit clearly above or below the line.
constexpr qreal kVp3MinHorizonDist = 20.0;
}

void PerspectiveTool::reset()
{
    m_vps.clear();
    m_selected = -1;
    m_snap = false;
    m_guidesVisible = true;
    m_density = 40;
    m_thickness = 1.0;
    m_horizonColor = QColor(0x31, 0x1d, 0xe2);
    m_defaultColor = QColor(0x4d, 0x9f, 0xff);
    m_defaultOpacity = 0.45;
    m_dirLocked = false;
}

QPointF PerspectiveTool::vanishingPoint(int index) const
{
    if (index < 0 || index >= m_vps.size())
        return QPointF();
    return m_vps.at(index).pos;
}

int PerspectiveTool::addVanishingPoint(const QPointF &canvasPos)
{
    if (m_vps.size() >= 3)
        return -1;
    if (m_vps.size() == 2) {
        // VP3 must sit clearly above or below the horizon.
        const QLineF h = horizonLine();
        const QPointF d(h.dx(), h.dy());
        const qreal len = qSqrt(QPointF::dotProduct(d, d));
        if (len > 1e-9) {
            const QPointF rel = canvasPos - h.p1();
            const qreal dist = qAbs(d.x() * rel.y() - d.y() * rel.x()) / len;
            if (dist < kVp3MinHorizonDist)
                return -1;
        }
    }
    VanishingPoint vp;
    vp.pos = canvasPos;
    vp.color = m_defaultColor;
    vp.opacity = m_defaultOpacity;
    m_vps.append(vp);
    m_selected = m_vps.size() - 1;
    return m_selected;
}

void PerspectiveTool::removeVanishingPoint(int index)
{
    if (index < 0 || index >= m_vps.size())
        return;
    m_vps.remove(index);
    if (m_selected >= m_vps.size())
        m_selected = m_vps.size() - 1;
}

void PerspectiveTool::moveVanishingPoint(int index, const QPointF &canvasPos)
{
    if (index < 0 || index >= m_vps.size())
        return;
    m_vps[index].pos = canvasPos; // the horizon is derived: it follows along
}

QColor PerspectiveTool::vpColor(int index) const
{
    if (index < 0 || index >= m_vps.size())
        return m_defaultColor;
    return m_vps.at(index).color;
}

qreal PerspectiveTool::vpOpacity(int index) const
{
    if (index < 0 || index >= m_vps.size())
        return m_defaultOpacity;
    return m_vps.at(index).opacity;
}

void PerspectiveTool::setSelected(int index)
{
    m_selected = (index >= 0 && index < m_vps.size()) ? index : -1;
}

QColor PerspectiveTool::selectedColor() const
{
    if (m_selected >= 0 && m_selected < m_vps.size())
        return m_vps.at(m_selected).color;
    return m_defaultColor;
}

void PerspectiveTool::setSelectedColor(const QColor &color)
{
    if (!color.isValid())
        return;
    if (m_selected >= 0 && m_selected < m_vps.size())
        m_vps[m_selected].color = color;
    else
        m_defaultColor = color; // no VP yet: the next one starts with it
}

qreal PerspectiveTool::selectedOpacity() const
{
    if (m_selected >= 0 && m_selected < m_vps.size())
        return m_vps.at(m_selected).opacity;
    return m_defaultOpacity;
}

void PerspectiveTool::setSelectedOpacity(qreal opacity)
{
    opacity = qBound(0.05, opacity, 1.0);
    if (m_selected >= 0 && m_selected < m_vps.size())
        m_vps[m_selected].opacity = opacity;
    else
        m_defaultOpacity = opacity;
}

void PerspectiveTool::setOpacityAll(qreal opacity)
{
    opacity = qBound(0.05, opacity, 1.0);
    for (VanishingPoint &vp : m_vps)
        vp.opacity = opacity;
    m_defaultOpacity = opacity; // new VPs start at the shared value too
}

// Horizontal through VP1 alone; through VP1 and VP2 when both exist (tilting
// with them). Degenerate (coincident VPs) falls back to horizontal.
QLineF PerspectiveTool::horizonLine() const
{
    if (m_vps.isEmpty())
        return QLineF();
    const QPointF a = m_vps.at(0).pos;
    if (m_vps.size() >= 2) {
        const QPointF b = m_vps.at(1).pos;
        if (QLineF(a, b).length() > 1e-6)
            return QLineF(a, b);
    }
    return QLineF(a, a + QPointF(1.0, 0.0));
}

// Guides: full lines through each VP fanned evenly, anchored to the horizon
// angle (tilting the horizon sweeps every fan; the spokes pivot around their
// VP like wheel hubs). The horizon VPs (VP1/VP2) draw a DOUBLE-density fan so
// the grid keeps reading all the way to the horizon, and their lines fade
// with scene depth — strong near the viewer, dissolving as they recede toward
// the horizon line. VP3's vertical-family guides never fade (always fully
// visible). Plus the derived horizon at a fixed slim 1px. Cosmetic pens keep
// the on-screen thickness constant at any zoom/rotation.
void PerspectiveTool::paintGuides(QPainter &p, const QRectF &canvasRect) const
{
    if (m_vps.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);

    const QLineF h = horizonLine();
    const qreal horizonAngle = qAtan2(h.dy(), h.dx());
    const QPointF hDir(qCos(horizonAngle), qSin(horizonAngle));
    const QPointF hNormal(-hDir.y(), hDir.x());
    // Depth-fade gradient across the canvas, perpendicular to the horizon:
    // faint ON the horizon (far away), full strength a canvas-span toward the
    // viewer on either side. Anchored at the horizon point nearest the
    // canvas centre so the falloff tracks the visible area.
    const QPointF c = canvasRect.center();
    const QPointF hPt =
        h.p1() + hDir * QPointF::dotProduct(c - h.p1(), hDir);
    const qreal fadeSpan = qMax(canvasRect.width(), canvasRect.height()) * 0.5;
    const qreal fadeBase = qMax(canvasRect.width(), canvasRect.height()) * 0.9;

    for (int v = 0; v < m_vps.size(); ++v) {
        const VanishingPoint &vp = m_vps.at(v);
        p.setOpacity(vp.opacity);

        // Reach far enough that spokes cross the WHOLE canvas even from a
        // distant VP.
        qreal farCorner = 0.0;
        const QPointF corners[] = {canvasRect.topLeft(), canvasRect.topRight(),
                                   canvasRect.bottomLeft(),
                                   canvasRect.bottomRight()};
        for (const QPointF &corner : corners)
            farCorner = qMax(farCorner, QLineF(vp.pos, corner).length());
        const qreal reach = qMax(fadeBase, farCorner * 1.15);

        QPen pen;
        if (v == 2) {
            // VP3: always fully visible — no depth fade.
            pen = QPen(vp.color, m_thickness);
        } else {
            QLinearGradient depth(hPt - hNormal * fadeSpan,
                                  hPt + hNormal * fadeSpan);
            QColor faint = vp.color;
            faint.setAlpha(26); // ~10%: barely-there at the horizon itself
            depth.setColorAt(0.0, vp.color);
            depth.setColorAt(0.5, faint);
            depth.setColorAt(1.0, vp.color);
            pen = QPen(QBrush(depth), m_thickness);
        }
        pen.setCosmetic(true);
        p.setPen(pen);

        // Double density for the horizon VPs: enough lines that the grid
        // keeps reading right up to the horizon.
        const int rays = (v == 2) ? m_density : m_density * 2;
        for (int i = 0; i < rays; ++i) {
            const qreal a = horizonAngle + M_PI * i / rays;
            const QPointF dir(qCos(a), qSin(a));
            p.drawLine(vp.pos - dir * reach, vp.pos + dir * reach);
        }
    }

    // The horizon (through VP1/VP2) reads solid at a fixed slim 1px.
    QPointF dir(h.dx(), h.dy());
    const qreal len = qSqrt(QPointF::dotProduct(dir, dir));
    if (len > 1e-9) {
        dir /= len;
        const qreal reach = qMax(canvasRect.width(), canvasRect.height()) * 16.0;
        p.setOpacity(1.0);
        QPen pen(m_horizonColor, 1.0);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawLine(h.p1() - dir * reach, h.p1() + dir * reach);
    }
    p.restore();
}

// Off-canvas VP beacon: a triangle with THREE anchor points — the VP itself
// plus the TOP and BOTTOM canvas corners of the side nearest the VP. The two
// corner anchors are FIXED; only the VP vertex moves, so dragging the VP
// stretches/rotates/scales the triangle around them. Filled with the VP's own
// guide colour at 40%. The caller clips strictly to the region OUTSIDE the
// canvas, so no part of it ever covers the artwork; VPs inside the canvas
// draw nothing.
void PerspectiveTool::paintEdgeIndicator(QPainter &p, const QRectF &canvasRect,
                                         int index) const
{
    if (index < 0 || index >= m_vps.size())
        return;
    const VanishingPoint &vp = m_vps.at(index);
    if (canvasRect.contains(vp.pos))
        return; // beacons exist only for off-canvas VPs

    // Fixed anchors, chosen ADAPTIVELY from where the VP escaped: a VP off
    // the left/right gets the top+bottom corners of that side; a VP off the
    // top/bottom gets the left+right corners of that edge. The dominant
    // escape distance decides, so the triangle never breaks (e.g. VP3
    // dragged sideways switches to side anchors automatically).
    const qreal excessX = qMax(0.0, qMax(canvasRect.left() - vp.pos.x(),
                                         vp.pos.x() - canvasRect.right()));
    const qreal excessY = qMax(0.0, qMax(canvasRect.top() - vp.pos.y(),
                                         vp.pos.y() - canvasRect.bottom()));
    QPointF anchorA, anchorB;
    if (excessY > excessX) {
        const bool bottomSide = vp.pos.y() >= canvasRect.center().y();
        anchorA = bottomSide ? canvasRect.bottomLeft() : canvasRect.topLeft();
        anchorB = bottomSide ? canvasRect.bottomRight() : canvasRect.topRight();
    } else {
        const bool rightSide = vp.pos.x() >= canvasRect.center().x();
        anchorA = rightSide ? canvasRect.topRight() : canvasRect.topLeft();
        anchorB = rightSide ? canvasRect.bottomRight() : canvasRect.bottomLeft();
    }

    QPainterPath tri;
    tri.moveTo(vp.pos);
    tri.lineTo(anchorA);
    tri.lineTo(anchorB);
    tri.closeSubpath();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity(0.40);
    p.setPen(Qt::NoPen);
    p.fillPath(tri, vp.color);
    p.restore();
}

// Editing handles (Perspective tool active): a ringed dot per VP in its own
// colour, in WIDGET space so they stay grabbable at any zoom — and visible
// even when a VP sits outside the canvas bounds. The hovered handle grows
// slightly for feedback; the selected VP (the one the Opacity / Hue Colors
// sliders edit) carries an accent outer ring.
void PerspectiveTool::paintHandles(QPainter &p, const QTransform &canvasToWidget,
                                   int hoveredIndex) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int v = 0; v < m_vps.size(); ++v) {
        const QPointF w = canvasToWidget.map(m_vps.at(v).pos);
        const qreal r = (v == hoveredIndex) ? 9.5 : 7.0;
        p.setPen(QPen(QColor(0, 0, 0, 150), 3.5));
        p.setBrush(m_vps.at(v).color);
        p.drawEllipse(w, r, r);
        p.setPen(QPen(Qt::white, 1.8));
        p.drawEllipse(w, r, r);
        if (v == m_selected) {
            p.setPen(QPen(QColor(0x7c, 0x6e, 0xf6), 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(w, r + 4.0, r + 4.0);
        }
    }
    p.restore();
}

int PerspectiveTool::hitTest(const QPointF &widgetPos,
                             const QTransform &canvasToWidget) const
{
    for (int v = 0; v < m_vps.size(); ++v) {
        const QPointF w = canvasToWidget.map(m_vps.at(v).pos);
        if (QLineF(w, widgetPos).length() <= 11.0)
            return v;
    }
    return -1;
}

void PerspectiveTool::beginStroke(const QPointF &anchorCanvas)
{
    m_strokeAnchor = anchorCanvas;
    m_dirLocked = false;
}

QPointF PerspectiveTool::snapPoint(const QPointF &canvasPos)
{
    if (!m_snap || m_vps.isEmpty())
        return canvasPos;
    if (!m_dirLocked) {
        // Lock the direction once the stroke has one (Procreate-style drawing
        // assist): before that, stay put at the anchor.
        if (QLineF(m_strokeAnchor, canvasPos).length() < 6.0)
            return m_strokeAnchor;
        if (!bestDirection(m_strokeAnchor, canvasPos, &m_lockedDir))
            return canvasPos;
        m_dirLocked = true;
    }
    return projectOntoDir(m_strokeAnchor, m_lockedDir, canvasPos);
}

QPointF PerspectiveTool::snapToRay(const QPointF &anchorCanvas,
                                   const QPointF &canvasPos) const
{
    if (!m_snap || m_vps.isEmpty())
        return canvasPos;
    QPointF dir;
    if (!bestDirection(anchorCanvas, canvasPos, &dir))
        return canvasPos;
    return projectOntoDir(anchorCanvas, dir, canvasPos);
}

// Candidates: the ray from `anchor` to every VP, plus PURE VERTICAL — so the
// artist can always drop a perfectly upright line even in 1- and 2-point
// setups. Picks the candidate whose line passes closest to `pos`.
bool PerspectiveTool::bestDirection(const QPointF &anchor, const QPointF &pos,
                                    QPointF *dir) const
{
    bool found = false;
    qreal bestDist = 1e18;
    auto consider = [&](const QPointF &candidate) {
        const qreal len = qSqrt(QPointF::dotProduct(candidate, candidate));
        if (len < 1e-9)
            return;
        const QPointF d = candidate / len;
        const QPointF proj = projectOntoDir(anchor, d, pos);
        const qreal dist = QLineF(proj, pos).length();
        if (dist < bestDist) {
            bestDist = dist;
            *dir = d;
            found = true;
        }
    };
    for (const VanishingPoint &vp : m_vps) {
        if (QLineF(anchor, vp.pos).length() < 4.0)
            continue; // anchor on the VP: the ray is undefined
        consider(vp.pos - anchor);
    }
    consider(QPointF(0.0, 1.0)); // perfectly vertical stays available
    return found;
}

QPointF PerspectiveTool::projectOntoDir(const QPointF &a, const QPointF &dir,
                                        const QPointF &p)
{
    const qreal t = QPointF::dotProduct(p - a, dir);
    return a + dir * t;
}

QJsonObject PerspectiveTool::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("snap")] = m_snap;
    o[QStringLiteral("showGuides")] = m_guidesVisible;
    o[QStringLiteral("density")] = m_density;
    o[QStringLiteral("thickness")] = m_thickness;
    o[QStringLiteral("horizonColor")] = m_horizonColor.name(QColor::HexArgb);
    o[QStringLiteral("defaultColor")] = m_defaultColor.name(QColor::HexArgb);
    o[QStringLiteral("defaultOpacity")] = m_defaultOpacity;
    o[QStringLiteral("selected")] = m_selected;
    QJsonArray vps;
    for (const VanishingPoint &vp : m_vps) {
        QJsonObject v;
        v[QStringLiteral("x")] = vp.pos.x();
        v[QStringLiteral("y")] = vp.pos.y();
        v[QStringLiteral("color")] = vp.color.name(QColor::HexArgb);
        v[QStringLiteral("opacity")] = vp.opacity;
        vps.append(v);
    }
    o[QStringLiteral("vps")] = vps;
    return o;
}

void PerspectiveTool::fromJson(const QJsonObject &o)
{
    if (o.isEmpty())
        return;
    reset();
    m_snap = o.value(QStringLiteral("snap")).toBool(false);
    m_guidesVisible = o.value(QStringLiteral("showGuides")).toBool(true);
    setDensity(o.value(QStringLiteral("density")).toInt(m_density));
    setThickness(o.value(QStringLiteral("thickness")).toDouble(m_thickness));
    // The pre-#311DE2 builds stored the old yellow default (never
    // user-editable), so migrate it to the current default on load.
    const QColor hc(o.value(QStringLiteral("horizonColor")).toString());
    if (hc.isValid())
        m_horizonColor = (hc == QColor(0xff, 0xd4, 0x00)) ? QColor(0x31, 0x1d, 0xe2)
                                                          : hc;
    const QColor dc(o.value(QStringLiteral("defaultColor")).toString());
    if (dc.isValid())
        m_defaultColor = dc;
    m_defaultOpacity =
        qBound(0.05, o.value(QStringLiteral("defaultOpacity")).toDouble(m_defaultOpacity), 1.0);

    if (o.contains(QStringLiteral("vps"))) {
        const QJsonArray vps = o.value(QStringLiteral("vps")).toArray();
        for (const QJsonValue &val : vps) {
            if (m_vps.size() >= 3)
                break;
            const QJsonObject v = val.toObject();
            VanishingPoint vp;
            vp.pos = QPointF(v.value(QStringLiteral("x")).toDouble(),
                             v.value(QStringLiteral("y")).toDouble());
            const QColor c(v.value(QStringLiteral("color")).toString());
            vp.color = c.isValid() ? c : m_defaultColor;
            vp.opacity =
                qBound(0.05, v.value(QStringLiteral("opacity")).toDouble(m_defaultOpacity), 1.0);
            m_vps.append(vp);
        }
    } else if (o.contains(QStringLiteral("mode"))) {
        // Legacy fixed-mode schema (mode/horizonY/vp0x/vp1x/zenith*): rebuild
        // the equivalent VPs so old projects keep their guides.
        const int mode = qBound(1, o.value(QStringLiteral("mode")).toInt(2), 3);
        const qreal horizonY = o.value(QStringLiteral("horizonY")).toDouble(216.0);
        const QColor c(o.value(QStringLiteral("color")).toString());
        if (c.isValid())
            m_defaultColor = c;
        m_defaultOpacity = qBound(
            0.05, o.value(QStringLiteral("opacity")).toDouble(m_defaultOpacity), 1.0);
        const qreal xs[2] = {o.value(QStringLiteral("vp0x")).toDouble(144.0),
                             o.value(QStringLiteral("vp1x")).toDouble(816.0)};
        for (int i = 0; i < qMin(mode, 2); ++i) {
            VanishingPoint vp;
            vp.pos = QPointF(xs[i], horizonY);
            vp.color = m_defaultColor;
            vp.opacity = m_defaultOpacity;
            m_vps.append(vp);
        }
        if (mode == 3) {
            VanishingPoint vp;
            vp.pos = QPointF(o.value(QStringLiteral("zenithX")).toDouble(480.0),
                             o.value(QStringLiteral("zenithY")).toDouble(864.0));
            vp.color = m_defaultColor;
            vp.opacity = m_defaultOpacity;
            m_vps.append(vp);
        }
    }
    const int sel = o.value(QStringLiteral("selected")).toInt(m_vps.size() - 1);
    setSelected(sel);
}
