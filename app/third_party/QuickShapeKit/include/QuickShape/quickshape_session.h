#pragma once

#include "quickshape_recognizer.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <QVariantAnimation>

namespace quickshape {

// One clear configuration point for the hold-to-recognize behaviour. All
// distance values are DOCUMENT units — the host converts its screen-space
// tolerances (about 8 screen px, about 20 screen px/s) using the current
// canvas scale at stroke start, so behaviour is zoom-independent.
struct QuickShapeTiming
{
    int holdDurationMs = 550;         // stylus rest before recognition begins
    int morphDurationMs = 220;        // rough -> corrected ease-in-out
    qreal dwellRadius = 8.0;          // endpoint jitter tolerance
    qreal maxDwellVelocity = 20.0;    // endpoint velocity ceiling, units/sec
    int minimumSamples = 8;           // shorter strokes stay freehand
    qreal minimumStrokeLength = 24.0; // shorter strokes stay freehand
};

class QuickShapeSession final : public QObject
{
    Q_OBJECT

public:
    explicit QuickShapeSession(QObject *parent = nullptr);

    void setTiming(const QuickShapeTiming &timing);
    QuickShapeTiming timing() const { return m_timing; }

    // Legacy single-value accessors (kept for compatibility; they read and
    // write the same QuickShapeTiming fields).
    void setHoldDelayMs(int milliseconds);
    void setMorphDurationMs(int milliseconds);
    void setDwellRadius(qreal documentUnits);
    int holdDelayMs() const;
    int morphDurationMs() const;
    qreal dwellRadius() const;

    void pointerPress(const QPointF &documentPoint, qreal pressure = 1.0);
    void pointerMove(const QPointF &documentPoint, qreal pressure = 1.0);
    void pointerRelease(const QPointF &documentPoint, qreal pressure = 1.0);

    bool hasActiveShape() const;
    bool isCollectingStroke() const;
    QPainterPath overlayPath() const;
    QuickShapeCommit currentCommit() const;

    // Host-side node editing: the current shape as an editable parametric
    // structure, and its replacement after edits. setEditedGeometry keeps the
    // original resampled pressure profile for the eventual commit.
    QuickShapeGeometry currentGeometry() const;
    void setEditedGeometry(const QuickShapeGeometry &geometry);

    // Call after host-side node editing to replace the temporary vector.
    void setEditedShape(const QString &shapeName, const QVector<QPointF> &points);

public slots:
    // Emits commitRequested synchronously, then clears the temporary overlay.
    void requestCommit();
    void cancelActiveShape();
    void reset();

signals:
    void overlayPathChanged(const QPainterPath &path);
    void activeShapeChanged(bool available);
    void shapeRecognized(const quickshape::QuickShapeResult &result);
    void shapeReady(const QString &shapeName, qreal confidence);
    void commitRequested(const quickshape::QuickShapeCommit &commit);
    void freehandStrokeFinished();
    void recognitionRejected();
    void statusChanged(const QString &state, const QString &detail);

private slots:
    void recognizeHeldStroke();

private:
    enum class State { Idle, Collecting, Morphing, Transforming, Ready };

    void beginMorph(const QuickShapeResult &result);
    void updateTransform(const QPointF &documentPoint);
    void updateOverlay(const QVector<QPointF> &points);
    void clearShapeState();
    QVector<qreal> resampledPressures(int count) const;
    qreal strokeLength() const;
    qreal recentEndpointVelocity() const; // document units per second

    State m_state = State::Idle;
    bool m_pointerDown = false;
    QTimer m_holdTimer;
    QVariantAnimation m_morphAnimation;
    QuickShapeTiming m_timing;
    QElapsedTimer m_strokeClock;      // per-stroke sample timestamps
    QVector<qint64> m_sourceTimesMs;  // parallel to m_sourcePoints
    QPointF m_holdAnchor;
    QPointF m_lastPoint;
    QPointF m_transformAnchor;
    QPointF m_shapeCenter;
    QVector<QPointF> m_sourcePoints;
    QVector<qreal> m_sourcePressures;
    QVector<QPointF> m_roughPoints;
    QVector<QPointF> m_targetPoints;
    QVector<QPointF> m_baseTargetPoints;
    QPainterPath m_overlayPath;
    QString m_shapeName;
    qreal m_confidence = 0.0;
};

} // namespace quickshape
