#pragma once

#include "quickshape_recognizer.h"

#include <QObject>
#include <QTimer>
#include <QVariantAnimation>

namespace quickshape {

class QuickShapeSession final : public QObject
{
    Q_OBJECT

public:
    explicit QuickShapeSession(QObject *parent = nullptr);

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

    State m_state = State::Idle;
    bool m_pointerDown = false;
    QTimer m_holdTimer;
    QVariantAnimation m_morphAnimation;
    qreal m_dwellRadius = 8.0;
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

