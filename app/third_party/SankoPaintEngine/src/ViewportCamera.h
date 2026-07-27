#pragma once

#include <QPointF>
#include <QSizeF>
#include <QTransform>

class ViewportCamera
{
public:
    void setViewportSize(const QSizeF &size) { m_viewportSize = size; }
    void setCanvasSize(const QSizeF &size) { m_canvasSize = size; }
    QSizeF viewportSize() const { return m_viewportSize; }
    QSizeF canvasSize() const { return m_canvasSize; }

    qreal zoom() const { return m_zoom; }
    void setZoom(qreal zoom);
    qreal rotation() const { return m_rotation; }
    void setRotation(qreal degrees) { m_rotation = degrees; }
    QPointF pan() const { return m_pan; }
    void setPan(const QPointF &pan) { m_pan = pan; }
    void panBy(const QPointF &delta) { m_pan += delta; }
    void reset(qreal zoom = 1.0) { m_zoom = zoom; m_rotation = 0; m_pan = {}; }
    void zoomAt(qreal factor, const QPointF &viewportAnchor);

    QTransform canvasToViewport() const;
    QTransform viewportToCanvas() const { return canvasToViewport().inverted(); }
    QPointF mapToCanvas(const QPointF &viewportPoint) const;
    QPointF mapToViewport(const QPointF &canvasPoint) const;

private:
    QSizeF m_viewportSize;
    QSizeF m_canvasSize;
    qreal m_zoom = 1.0;
    qreal m_rotation = 0.0;
    QPointF m_pan;
};
