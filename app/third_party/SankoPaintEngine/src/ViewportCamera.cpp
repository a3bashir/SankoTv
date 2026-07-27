#include "ViewportCamera.h"

#include <algorithm>

void ViewportCamera::setZoom(qreal zoom)
{
    m_zoom = std::clamp(zoom, 0.01, 64.0);
}

QTransform ViewportCamera::canvasToViewport() const
{
    QTransform transform;
    transform.translate(m_viewportSize.width() * .5 + m_pan.x(),
                        m_viewportSize.height() * .5 + m_pan.y());
    transform.rotate(m_rotation);
    transform.scale(m_zoom, m_zoom);
    transform.translate(-m_canvasSize.width() * .5, -m_canvasSize.height() * .5);
    return transform;
}

QPointF ViewportCamera::mapToCanvas(const QPointF &viewportPoint) const
{
    return viewportToCanvas().map(viewportPoint);
}

QPointF ViewportCamera::mapToViewport(const QPointF &canvasPoint) const
{
    return canvasToViewport().map(canvasPoint);
}

void ViewportCamera::zoomAt(qreal factor, const QPointF &viewportAnchor)
{
    const QPointF canvasAnchor = mapToCanvas(viewportAnchor);
    setZoom(m_zoom * factor);
    m_pan += viewportAnchor - mapToViewport(canvasAnchor);
}
