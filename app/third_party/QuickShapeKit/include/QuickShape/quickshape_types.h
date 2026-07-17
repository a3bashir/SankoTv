#pragma once

#include <QMetaType>
#include <QPainterPath>
#include <QPointF>
#include <QString>
#include <QVector>

namespace quickshape {

bool isClosedShapeType(const QString &shapeName);

struct QuickShapeResult
{
    bool accepted = false;
    QString name;
    qreal confidence = 0.0;
    QVector<QPointF> rough;
    QVector<QPointF> target;
    QPointF center;

    bool isClosed() const;
    QPainterPath painterPath() const;
};

struct QuickShapeCommit
{
    QString name;
    qreal confidence = 0.0;
    QVector<QPointF> points;
    QVector<qreal> pressures;

    bool isClosed() const;
    QPainterPath painterPath() const;
};

// Editable, parametric description of a corrected shape, for host-side node
// editors. Polyline/Polygon shapes are defined by their structural vertices;
// the ellipse family (circle, ellipse, and open arcs) stays parametric —
// centre, radii, rotation, and an angular span — so editing never degrades
// the curve into a coarse polygon, and an open arc's deliberate gap is
// preserved exactly (span < 2*pi). The host mutates a copy and hands it back
// through QuickShapeSession::setEditedGeometry().
struct QuickShapeGeometry
{
    enum Kind { Polyline, Polygon, Ellipse };

    Kind kind = Polyline;
    QString name;               // recognizer label; decides open vs closed
    QVector<QPointF> nodes;     // Polyline/Polygon structural vertices
    QPointF center;             // Ellipse family
    qreal radiusX = 0.0;
    qreal radiusY = 0.0;
    qreal rotationRad = 0.0;
    qreal startAngleRad = 0.0;
    qreal spanAngleRad = 6.283185307179586; // 2*pi; smaller keeps an arc open

    bool isValid() const;
    bool isClosed() const { return isClosedShapeType(name); }
    // Point on the parametric ellipse at parameter angle t.
    QPointF ellipsePointAt(qreal t) const;
    // Parameter angle of an arbitrary point relative to the ellipse frame.
    qreal ellipseAngleOf(const QPointF &point) const;
    // Dense sample run for the overlay and the brush replay. Ellipse-family
    // shapes are sampled smoothly; vertex shapes return their nodes (the
    // brush engine interpolates its own dab spacing along segments).
    QVector<QPointF> sampled(int ellipseSamples = 96) const;
    // Recover an editable structure from a corrected target polyline.
    static QuickShapeGeometry fromCorrected(const QString &name,
                                            const QVector<QPointF> &target,
                                            const QPointF &center);
};

} // namespace quickshape

Q_DECLARE_METATYPE(quickshape::QuickShapeResult)
Q_DECLARE_METATYPE(quickshape::QuickShapeCommit)
Q_DECLARE_METATYPE(quickshape::QuickShapeGeometry)
