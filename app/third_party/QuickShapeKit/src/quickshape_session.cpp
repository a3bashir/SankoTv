#include "QuickShape/quickshape_session.h"

#include <QEasingCurve>
#include <QLineF>
#include <QtMath>
#include <utility>

namespace quickshape {
namespace {

qreal pointDistance(const QPointF &a, const QPointF &b)
{
    return QLineF(a, b).length();
}

QPointF rotatePoint(const QPointF &point, qreal radians)
{
    const qreal cosine = qCos(radians);
    const qreal sine = qSin(radians);
    return {point.x() * cosine - point.y() * sine,
            point.x() * sine + point.y() * cosine};
}

// Ramer-Douglas-Peucker on an open polyline: keeps only structural vertices.
void rdpSimplify(const QVector<QPointF> &points, qsizetype first, qsizetype last,
                 qreal epsilon, QVector<bool> &keep)
{
    if (last <= first + 1)
        return;
    const QPointF a = points[first];
    const QPointF b = points[last];
    const QPointF ab = b - a;
    const qreal len2 = QPointF::dotProduct(ab, ab);
    qreal worst = -1.0;
    qsizetype worstIndex = -1;
    for (qsizetype i = first + 1; i < last; ++i) {
        qreal d;
        if (len2 < 1e-9) {
            d = pointDistance(points[i], a);
        } else {
            const qreal t = QPointF::dotProduct(points[i] - a, ab) / len2;
            d = pointDistance(points[i], a + ab * qBound<qreal>(0.0, t, 1.0));
        }
        if (d > worst) {
            worst = d;
            worstIndex = i;
        }
    }
    if (worst > epsilon && worstIndex > 0) {
        keep[worstIndex] = true;
        rdpSimplify(points, first, worstIndex, epsilon, keep);
        rdpSimplify(points, worstIndex, last, epsilon, keep);
    }
}

QVector<QPointF> structuralVerticesOf(const QVector<QPointF> &target, bool closed)
{
    if (target.size() < 3)
        return target;
    QVector<QPointF> pts = target;
    if (closed)
        pts.append(target.first()); // wrap so the seam vertex can be found
    QVector<bool> keep(pts.size(), false);
    keep.first() = true;
    keep.last() = true;
    rdpSimplify(pts, 0, pts.size() - 1, 2.0, keep);
    QVector<QPointF> out;
    for (qsizetype i = 0; i < pts.size(); ++i)
        if (keep[i])
            out.append(pts[i]);
    if (closed && out.size() > 1
        && pointDistance(out.first(), out.last()) < 1.0)
        out.removeLast(); // the duplicated wrap vertex
    return out;
}

QPainterPath pathFromPoints(const QVector<QPointF> &points, bool closed)
{
    QPainterPath path;
    if (points.isEmpty())
        return path;
    path.moveTo(points.first());
    for (qsizetype i = 1; i < points.size(); ++i)
        path.lineTo(points[i]);
    if (closed)
        path.closeSubpath();
    return path;
}

} // namespace

QuickShapeSession::QuickShapeSession(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QuickShapeResult>();
    qRegisterMetaType<QuickShapeCommit>();

    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(m_timing.holdDurationMs);
    connect(&m_holdTimer, &QTimer::timeout,
            this, &QuickShapeSession::recognizeHeldStroke);

    m_morphAnimation.setDuration(m_timing.morphDurationMs);
    m_morphAnimation.setStartValue(0.0);
    m_morphAnimation.setEndValue(1.0);
    m_morphAnimation.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&m_morphAnimation, &QVariantAnimation::valueChanged,
            this, [this](const QVariant &value) {
        const qreal progress = value.toReal();
        const qsizetype count = qMin(m_roughPoints.size(), m_targetPoints.size());
        QVector<QPointF> points;
        points.reserve(count);
        for (qsizetype i = 0; i < count; ++i)
            points.append(m_roughPoints[i] * (1.0 - progress)
                          + m_targetPoints[i] * progress);
        updateOverlay(points);
    });
    connect(&m_morphAnimation, &QVariantAnimation::finished, this, [this] {
        updateOverlay(m_targetPoints);
        m_baseTargetPoints = m_targetPoints;
        m_state = m_pointerDown ? State::Transforming : State::Ready;
        emit shapeReady(m_shapeName, m_confidence);
        emit statusChanged("Shape ready", QString("%1 · %2% confidence")
                                             .arg(m_shapeName)
                                             .arg(qRound(m_confidence * 100.0)));
    });
}

void QuickShapeSession::setTiming(const QuickShapeTiming &timing)
{
    m_timing = timing;
    m_timing.holdDurationMs = qMax(1, m_timing.holdDurationMs);
    m_timing.morphDurationMs = qMax(0, m_timing.morphDurationMs);
    m_timing.dwellRadius = qMax(0.1, m_timing.dwellRadius);
    m_timing.maxDwellVelocity = qMax(0.1, m_timing.maxDwellVelocity);
    m_timing.minimumSamples = qMax(2, m_timing.minimumSamples);
    m_timing.minimumStrokeLength = qMax(0.0, m_timing.minimumStrokeLength);
    m_holdTimer.setInterval(m_timing.holdDurationMs);
    m_morphAnimation.setDuration(m_timing.morphDurationMs);
}

void QuickShapeSession::setHoldDelayMs(int milliseconds)
{
    m_timing.holdDurationMs = qMax(1, milliseconds);
    m_holdTimer.setInterval(m_timing.holdDurationMs);
}

void QuickShapeSession::setMorphDurationMs(int milliseconds)
{
    m_timing.morphDurationMs = qMax(0, milliseconds);
    m_morphAnimation.setDuration(m_timing.morphDurationMs);
}

void QuickShapeSession::setDwellRadius(qreal documentUnits)
{
    m_timing.dwellRadius = qMax(0.1, documentUnits);
}

int QuickShapeSession::holdDelayMs() const
{
    return m_timing.holdDurationMs;
}

int QuickShapeSession::morphDurationMs() const
{
    return m_timing.morphDurationMs;
}

qreal QuickShapeSession::dwellRadius() const
{
    return m_timing.dwellRadius;
}

void QuickShapeSession::pointerPress(const QPointF &documentPoint, qreal pressure)
{
    if (hasActiveShape())
        requestCommit();

    m_morphAnimation.stop();
    m_pointerDown = true;
    m_state = State::Collecting;
    m_sourcePoints = {documentPoint};
    m_sourcePressures = {qBound(0.0, pressure, 1.0)};
    m_strokeClock.start();
    m_sourceTimesMs = {0};
    m_holdAnchor = documentPoint;
    m_lastPoint = documentPoint;
    m_holdTimer.start();
    emit statusChanged("Drawing", "Hold the pen steady to invoke QuickShape");
}

void QuickShapeSession::pointerMove(const QPointF &documentPoint, qreal pressure)
{
    if (!m_pointerDown)
        return;
    if (m_state == State::Transforming) {
        updateTransform(documentPoint);
        return;
    }
    if (m_state == State::Morphing) {
        m_lastPoint = documentPoint;
        return;
    }
    if (m_state != State::Collecting || pointDistance(documentPoint, m_lastPoint) < 1.4)
        return;

    m_sourcePoints.append(documentPoint);
    m_sourcePressures.append(qBound(0.0, pressure, 1.0));
    m_sourceTimesMs.append(m_strokeClock.elapsed());
    m_lastPoint = documentPoint;
    if (pointDistance(documentPoint, m_holdAnchor) > m_timing.dwellRadius) {
        // Drawing resumed beyond the dwell tolerance: restart the hold and
        // keep the ordinary rough stroke going.
        m_holdAnchor = documentPoint;
        m_holdTimer.start();
    }
}

void QuickShapeSession::pointerRelease(const QPointF &documentPoint, qreal pressure)
{
    if (!m_pointerDown)
        return;
    m_pointerDown = false;
    m_holdTimer.stop();

    if (m_state == State::Collecting) {
        if (pointDistance(documentPoint, m_lastPoint) >= 1.4) {
            m_sourcePoints.append(documentPoint);
            m_sourcePressures.append(qBound(0.0, pressure, 1.0));
            m_sourceTimesMs.append(m_strokeClock.elapsed());
        }
        m_state = State::Idle;
        m_sourcePoints.clear();
        m_sourcePressures.clear();
        emit freehandStrokeFinished();
        emit statusChanged("Freehand", "Stroke finished without QuickShape");
    } else if (m_state == State::Transforming) {
        m_state = State::Ready;
    }
}

bool QuickShapeSession::hasActiveShape() const
{
    return m_state == State::Morphing || m_state == State::Transforming
        || m_state == State::Ready;
}

bool QuickShapeSession::isCollectingStroke() const
{
    return m_state == State::Collecting;
}

QPainterPath QuickShapeSession::overlayPath() const
{
    return m_overlayPath;
}

QuickShapeCommit QuickShapeSession::currentCommit() const
{
    QuickShapeCommit commit;
    if (!hasActiveShape() || m_targetPoints.isEmpty())
        return commit;
    commit.name = m_shapeName;
    commit.confidence = m_confidence;
    commit.points = m_targetPoints;
    commit.pressures = resampledPressures(m_targetPoints.size());
    return commit;
}

QuickShapeGeometry QuickShapeSession::currentGeometry() const
{
    if (!hasActiveShape() || m_targetPoints.size() < 2)
        return {};
    return QuickShapeGeometry::fromCorrected(m_shapeName, m_targetPoints,
                                             m_shapeCenter);
}

void QuickShapeSession::setEditedGeometry(const QuickShapeGeometry &geometry)
{
    if (!geometry.isValid())
        return;
    const QVector<QPointF> points = geometry.sampled();
    if (points.size() < 2)
        return;
    const bool previouslyActive = hasActiveShape();
    m_holdTimer.stop();
    m_morphAnimation.stop();
    m_pointerDown = false;
    m_shapeName = geometry.name;
    m_targetPoints = points;
    m_baseTargetPoints = points;
    if (geometry.kind == QuickShapeGeometry::Ellipse) {
        m_shapeCenter = geometry.center;
    } else {
        m_shapeCenter = {};
        for (const QPointF &point : points)
            m_shapeCenter += point;
        m_shapeCenter /= points.size();
    }
    m_state = State::Ready;
    updateOverlay(points);
    if (!previouslyActive)
        emit activeShapeChanged(true);
    emit shapeReady(m_shapeName, m_confidence);
}

void QuickShapeSession::setEditedShape(const QString &shapeName,
                                       const QVector<QPointF> &points)
{
    if (points.size() < 2)
        return;
    const bool previouslyActive = hasActiveShape();
    m_holdTimer.stop();
    m_morphAnimation.stop();
    m_pointerDown = false;
    m_shapeName = shapeName;
    m_targetPoints = points;
    m_baseTargetPoints = points;
    m_shapeCenter = {};
    for (const QPointF &point : points)
        m_shapeCenter += point;
    m_shapeCenter /= points.size();
    m_state = State::Ready;
    updateOverlay(points);
    if (!previouslyActive)
        emit activeShapeChanged(true);
    emit shapeReady(m_shapeName, m_confidence);
}

void QuickShapeSession::requestCommit()
{
    if (!hasActiveShape() || m_targetPoints.isEmpty())
        return;
    const QuickShapeCommit commit = currentCommit();
    emit commitRequested(commit);
    clearShapeState();
}

void QuickShapeSession::cancelActiveShape()
{
    if (!hasActiveShape())
        return;
    clearShapeState();
    emit statusChanged("Cancelled", "Temporary QuickShape discarded");
}

void QuickShapeSession::reset()
{
    m_holdTimer.stop();
    m_morphAnimation.stop();
    m_pointerDown = false;
    m_sourcePoints.clear();
    m_sourcePressures.clear();
    m_sourceTimesMs.clear();
    clearShapeState();
}

qreal QuickShapeSession::strokeLength() const
{
    qreal length = 0.0;
    for (qsizetype i = 1; i < m_sourcePoints.size(); ++i)
        length += pointDistance(m_sourcePoints[i - 1], m_sourcePoints[i]);
    return length;
}

// Movement of the stroke endpoint over the trailing ~160 ms window, in
// document units per second. During a true hold no fresh samples arrive, so
// the window is empty and the velocity is zero.
qreal QuickShapeSession::recentEndpointVelocity() const
{
    if (m_sourcePoints.size() < 2
        || m_sourceTimesMs.size() != m_sourcePoints.size())
        return 0.0;
    const qint64 now = m_strokeClock.elapsed();
    const qint64 windowStart = now - 160;
    qreal moved = 0.0;
    qint64 earliest = now;
    for (qsizetype i = m_sourcePoints.size() - 1; i >= 1; --i) {
        if (m_sourceTimesMs[i] < windowStart)
            break;
        moved += pointDistance(m_sourcePoints[i - 1], m_sourcePoints[i]);
        earliest = m_sourceTimesMs[i - 1] > windowStart ? m_sourceTimesMs[i - 1]
                                                        : windowStart;
    }
    const qint64 dt = now - earliest;
    return dt > 0 ? moved * 1000.0 / dt : 0.0;
}

void QuickShapeSession::recognizeHeldStroke()
{
    if (!m_pointerDown || m_state != State::Collecting)
        return;
    // Movement-stability gates: recognition needs a usable stroke AND a
    // genuinely resting endpoint — not merely "no sample for a while". Slow
    // curved drawing, careful corners, and retracing keep the endpoint
    // moving, so they re-arm the timer instead of snapping prematurely.
    if (m_sourcePoints.size() < m_timing.minimumSamples
        || strokeLength() < m_timing.minimumStrokeLength
        || recentEndpointVelocity() > m_timing.maxDwellVelocity) {
        m_holdTimer.start();
        return;
    }
    const QuickShapeResult result = QuickShapeRecognizer::recognize(m_sourcePoints);
    if (!result.accepted) {
        emit recognitionRejected();
        emit statusChanged("Not recognized", "Continue drawing or try a clearer shape");
        if (m_pointerDown)
            m_holdTimer.start();
        return;
    }
    beginMorph(result);
}

void QuickShapeSession::beginMorph(const QuickShapeResult &result)
{
    m_holdTimer.stop();
    m_state = State::Morphing;
    m_roughPoints = result.rough;
    m_targetPoints = result.target;
    m_baseTargetPoints = result.target;
    m_shapeCenter = result.center;
    m_shapeName = result.name;
    m_confidence = result.confidence;
    m_transformAnchor = m_lastPoint;
    emit shapeRecognized(result);
    emit activeShapeChanged(true);
    emit statusChanged("Recognized", QString("%1 · correcting").arg(m_shapeName));
    m_morphAnimation.stop();
    m_morphAnimation.start();
}

void QuickShapeSession::updateTransform(const QPointF &documentPoint)
{
    const QPointF start = m_transformAnchor - m_shapeCenter;
    const QPointF current = documentPoint - m_shapeCenter;
    const qreal startLength = qMax(QLineF(QPointF(), start).length(), 18.0);
    const qreal scale = qBound(0.35,
        QLineF(QPointF(), current).length() / startLength, 3.5);
    const qreal rotation = qAtan2(current.y(), current.x())
        - qAtan2(start.y(), start.x());

    m_targetPoints.clear();
    m_targetPoints.reserve(m_baseTargetPoints.size());
    for (const QPointF &point : std::as_const(m_baseTargetPoints))
        m_targetPoints.append(m_shapeCenter
                              + rotatePoint((point - m_shapeCenter) * scale, rotation));
    updateOverlay(m_targetPoints);
}

void QuickShapeSession::updateOverlay(const QVector<QPointF> &points)
{
    m_overlayPath = pathFromPoints(points, isClosedShapeType(m_shapeName));
    emit overlayPathChanged(m_overlayPath);
}

void QuickShapeSession::clearShapeState()
{
    const bool wasActive = hasActiveShape() || !m_overlayPath.isEmpty();
    m_morphAnimation.stop();
    m_state = State::Idle;
    m_shapeName.clear();
    m_confidence = 0.0;
    m_roughPoints.clear();
    m_targetPoints.clear();
    m_baseTargetPoints.clear();
    m_overlayPath = {};
    m_sourcePoints.clear();
    m_sourcePressures.clear();
    emit overlayPathChanged(m_overlayPath);
    if (wasActive)
        emit activeShapeChanged(false);
}

QVector<qreal> QuickShapeSession::resampledPressures(int count) const
{
    QVector<qreal> result;
    if (count <= 0 || m_sourcePoints.isEmpty()
        || m_sourcePoints.size() != m_sourcePressures.size())
        return result;
    if (m_sourcePoints.size() == 1) {
        result.fill(m_sourcePressures.first(), count);
        return result;
    }

    QVector<qreal> lengths(m_sourcePoints.size(), 0.0);
    for (qsizetype i = 1; i < m_sourcePoints.size(); ++i)
        lengths[i] = lengths[i - 1] + pointDistance(m_sourcePoints[i - 1], m_sourcePoints[i]);
    const qreal totalLength = lengths.last();
    if (totalLength < 0.001) {
        result.fill(m_sourcePressures.first(), count);
        return result;
    }

    qsizetype segment = 1;
    for (int i = 0; i < count; ++i) {
        const qreal wanted = count == 1 ? 0.0 : totalLength * i / qreal(count - 1);
        while (segment < lengths.size() - 1 && lengths[segment] < wanted)
            ++segment;
        const qreal span = lengths[segment] - lengths[segment - 1];
        const qreal mix = span > 0.0 ? (wanted - lengths[segment - 1]) / span : 0.0;
        result.append(m_sourcePressures[segment - 1] * (1.0 - mix)
                      + m_sourcePressures[segment] * mix);
    }
    return result;
}

// --- QuickShapeGeometry -----------------------------------------------------

bool QuickShapeGeometry::isValid() const
{
    if (kind == Ellipse)
        return radiusX > 0.4 && radiusY > 0.4 && qAbs(spanAngleRad) > 0.05;
    return nodes.size() >= (kind == Polygon ? 3 : 2);
}

QPointF QuickShapeGeometry::ellipsePointAt(qreal t) const
{
    const qreal cosine = qCos(rotationRad);
    const qreal sine = qSin(rotationRad);
    const qreal x = radiusX * qCos(t);
    const qreal y = radiusY * qSin(t);
    return center + QPointF(x * cosine - y * sine, x * sine + y * cosine);
}

qreal QuickShapeGeometry::ellipseAngleOf(const QPointF &point) const
{
    const qreal cosine = qCos(rotationRad);
    const qreal sine = qSin(rotationRad);
    const QPointF d = point - center;
    const qreal localX = d.x() * cosine + d.y() * sine;
    const qreal localY = -d.x() * sine + d.y() * cosine;
    return qAtan2(localY / qMax<qreal>(0.4, radiusY),
                  localX / qMax<qreal>(0.4, radiusX));
}

QVector<QPointF> QuickShapeGeometry::sampled(int ellipseSamples) const
{
    if (kind != Ellipse)
        return nodes;
    QVector<QPointF> out;
    const int count = qMax(16, ellipseSamples);
    out.reserve(count);
    const bool fullTurn = qAbs(spanAngleRad) >= 2.0 * M_PI - 0.02;
    if (fullTurn) {
        for (int i = 0; i < count; ++i) // path closes via the shape name
            out.append(ellipsePointAt(startAngleRad + 2.0 * M_PI * i / count));
    } else {
        for (int i = 0; i < count; ++i) // inclusive endpoints keep the gap
            out.append(ellipsePointAt(startAngleRad
                                      + spanAngleRad * i / qreal(count - 1)));
    }
    return out;
}

QuickShapeGeometry QuickShapeGeometry::fromCorrected(
    const QString &name, const QVector<QPointF> &target, const QPointF &center)
{
    QuickShapeGeometry geometry;
    geometry.name = name;
    const bool closed = isClosedShapeType(name);
    const bool ellipseFamily = name == QLatin1String("Circle")
        || name == QLatin1String("Ellipse") || name == QLatin1String("Arc")
        || name == QLatin1String("Elliptical Arc");

    if (ellipseFamily && target.size() >= 8) {
        geometry.kind = Ellipse;
        geometry.center = center;
        // Orientation from the samples' second moments, radii from the
        // extents in the rotated frame — exact for targets generated from
        // the parametric form, and stable under the session's rotate/scale.
        qreal sxx = 0.0, syy = 0.0, sxy = 0.0;
        for (const QPointF &point : target) {
            const QPointF d = point - center;
            sxx += d.x() * d.x();
            syy += d.y() * d.y();
            sxy += d.x() * d.y();
        }
        geometry.rotationRad = 0.5 * qAtan2(2.0 * sxy, sxx - syy);
        const qreal cosine = qCos(geometry.rotationRad);
        const qreal sine = qSin(geometry.rotationRad);
        qreal rx = 0.0, ry = 0.0;
        for (const QPointF &point : target) {
            const QPointF d = point - center;
            rx = qMax(rx, qAbs(d.x() * cosine + d.y() * sine));
            ry = qMax(ry, qAbs(-d.x() * sine + d.y() * cosine));
        }
        geometry.radiusX = qMax(0.5, rx);
        geometry.radiusY = qMax(0.5, ry);
        if (closed) {
            geometry.startAngleRad = geometry.ellipseAngleOf(target.first());
            geometry.spanAngleRad = 2.0 * M_PI;
        } else {
            // Unwrap the signed angular travel so the arc's deliberate gap
            // (and winding direction) survives round-trips exactly.
            qreal previous = geometry.ellipseAngleOf(target.first());
            geometry.startAngleRad = previous;
            qreal span = 0.0;
            for (qsizetype i = 1; i < target.size(); ++i) {
                qreal a = geometry.ellipseAngleOf(target[i]);
                qreal delta = a - previous;
                while (delta > M_PI)
                    delta -= 2.0 * M_PI;
                while (delta < -M_PI)
                    delta += 2.0 * M_PI;
                span += delta;
                previous = a;
            }
            geometry.spanAngleRad = span;
        }
        return geometry;
    }

    geometry.kind = closed ? Polygon : Polyline;
    geometry.nodes = structuralVerticesOf(target, closed);
    return geometry;
}

} // namespace quickshape
