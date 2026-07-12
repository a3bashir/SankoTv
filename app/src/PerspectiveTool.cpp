#include "PerspectiveTool.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
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
    m_density = 12;
    m_thickness = 1.0;
    m_horizonColor = QColor(0xff, 0xd4, 0x00);
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

// Guides: `density` full lines through each VP, fanned evenly, long enough to
// cross any visible canvas ("infinite"); plus the derived horizon line. Each
// VP paints with ITS OWN colour and opacity. Cosmetic pens keep the on-screen
// thickness constant at any zoom/rotation.
void PerspectiveTool::paintGuides(QPainter &p, const QRectF &canvasRect) const
{
    if (m_vps.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);

    const qreal reach = qMax(canvasRect.width(), canvasRect.height()) * 16.0;
    for (const VanishingPoint &vp : m_vps) {
        p.setOpacity(vp.opacity);
        QPen pen(vp.color, m_thickness);
        pen.setCosmetic(true);
        p.setPen(pen);
        for (int i = 0; i < m_density; ++i) {
            const qreal a = M_PI * i / m_density; // half-turn: distinct lines
            const QPointF dir(qCos(a), qSin(a));
            p.drawLine(vp.pos - dir * reach, vp.pos + dir * reach);
        }
    }

    // The horizon (through VP1/VP2) reads slightly stronger than the fans.
    const QLineF h = horizonLine();
    QPointF dir(h.dx(), h.dy());
    const qreal len = qSqrt(QPointF::dotProduct(dir, dir));
    if (len > 1e-9) {
        dir /= len;
        p.setOpacity(1.0);
        QPen pen(m_horizonColor, m_thickness + 0.8);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawLine(h.p1() - dir * reach, h.p1() + dir * reach);
    }
    p.restore();
}

// Editing handles (Perspective tool active): a ringed dot per VP in its own
// colour, in WIDGET space so they stay grabbable at any zoom — and visible
// even when a VP sits outside the canvas bounds. The selected VP (the one the
// Opacity / Hue Colors sliders edit) carries an accent outer ring.
void PerspectiveTool::paintHandles(QPainter &p, const QTransform &canvasToWidget) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int v = 0; v < m_vps.size(); ++v) {
        const QPointF w = canvasToWidget.map(m_vps.at(v).pos);
        p.setPen(QPen(QColor(0, 0, 0, 150), 3.5));
        p.setBrush(m_vps.at(v).color);
        p.drawEllipse(w, 7.0, 7.0);
        p.setPen(QPen(Qt::white, 1.8));
        p.drawEllipse(w, 7.0, 7.0);
        if (v == m_selected) {
            p.setPen(QPen(QColor(0x7c, 0x6e, 0xf6), 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(w, 11.0, 11.0);
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
    setDensity(o.value(QStringLiteral("density")).toInt(m_density));
    setThickness(o.value(QStringLiteral("thickness")).toDouble(m_thickness));
    const QColor hc(o.value(QStringLiteral("horizonColor")).toString());
    if (hc.isValid())
        m_horizonColor = hc;
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
