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
    m_holdTimer.setInterval(420);
    connect(&m_holdTimer, &QTimer::timeout,
            this, &QuickShapeSession::recognizeHeldStroke);

    m_morphAnimation.setDuration(220);
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

void QuickShapeSession::setHoldDelayMs(int milliseconds)
{
    m_holdTimer.setInterval(qMax(1, milliseconds));
}

void QuickShapeSession::setMorphDurationMs(int milliseconds)
{
    m_morphAnimation.setDuration(qMax(0, milliseconds));
}

void QuickShapeSession::setDwellRadius(qreal documentUnits)
{
    m_dwellRadius = qMax(0.1, documentUnits);
}

int QuickShapeSession::holdDelayMs() const
{
    return m_holdTimer.interval();
}

int QuickShapeSession::morphDurationMs() const
{
    return m_morphAnimation.duration();
}

qreal QuickShapeSession::dwellRadius() const
{
    return m_dwellRadius;
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
    m_lastPoint = documentPoint;
    if (pointDistance(documentPoint, m_holdAnchor) > m_dwellRadius) {
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
    clearShapeState();
}

void QuickShapeSession::recognizeHeldStroke()
{
    if (!m_pointerDown || m_state != State::Collecting)
        return;
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

} // namespace quickshape
