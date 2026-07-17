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

} // namespace quickshape

Q_DECLARE_METATYPE(quickshape::QuickShapeResult)
Q_DECLARE_METATYPE(quickshape::QuickShapeCommit)

