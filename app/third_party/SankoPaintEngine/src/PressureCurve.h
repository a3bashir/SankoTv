#pragma once

#include <QPointF>
#include <QVector>

// Serializable piecewise-linear pressure remap. Control points are always
// clamped to [0, 1], sorted by input pressure, and include both endpoints.
class PressureCurve
{
public:
    PressureCurve();
    explicit PressureCurve(const QVector<QPointF> &controlPoints);

    const QVector<QPointF> &controlPoints() const { return m_points; }
    void setControlPoints(const QVector<QPointF> &controlPoints);
    qreal valueAt(qreal inputPressure) const;
    void resetLinear();

private:
    QVector<QPointF> m_points;
};
