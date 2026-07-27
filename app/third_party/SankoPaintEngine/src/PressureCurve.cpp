#include "PressureCurve.h"

#include <algorithm>

PressureCurve::PressureCurve()
{
    resetLinear();
}

PressureCurve::PressureCurve(const QVector<QPointF> &controlPoints)
{
    setControlPoints(controlPoints);
}

void PressureCurve::resetLinear()
{
    m_points = {{0.0, 0.0}, {1.0, 1.0}};
}

void PressureCurve::setControlPoints(const QVector<QPointF> &controlPoints)
{
    QVector<QPointF> points;
    points.reserve(controlPoints.size() + 2);
    for (QPointF point : controlPoints) {
        point.setX(std::clamp(point.x(), 0.0, 1.0));
        point.setY(std::clamp(point.y(), 0.0, 1.0));
        points.append(point);
    }
    std::sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) {
        return a.x() < b.x();
    });

    QVector<QPointF> unique;
    for (const QPointF &point : points) {
        if (!unique.isEmpty() && qAbs(unique.last().x() - point.x()) < 0.0001)
            unique.last().setY(point.y());
        else
            unique.append(point);
    }
    if (unique.isEmpty() || unique.first().x() > 0.0001)
        unique.prepend(QPointF(0.0, unique.isEmpty() ? 0.0 : unique.first().y()));
    else
        unique.first().setX(0.0);
    if (unique.last().x() < 0.9999)
        unique.append(QPointF(1.0, unique.last().y()));
    else
        unique.last().setX(1.0);
    m_points = unique;
}

qreal PressureCurve::valueAt(qreal inputPressure) const
{
    const qreal input = std::clamp(inputPressure, 0.0, 1.0);
    for (int i = 1; i < m_points.size(); ++i) {
        if (input <= m_points.at(i).x()) {
            const QPointF a = m_points.at(i - 1);
            const QPointF b = m_points.at(i);
            const qreal width = b.x() - a.x();
            if (width <= 0.000001)
                return b.y();
            return std::clamp(a.y() + (b.y() - a.y()) * ((input - a.x()) / width), 0.0, 1.0);
        }
    }
    return m_points.constLast().y();
}
