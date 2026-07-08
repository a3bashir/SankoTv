#pragma once

#include <QFont>
#include <QPoint>
#include <QWidget>

// Custom-painted Canvas View Controls toolbar (fixed 477x42). Everything is
// drawn in paintEvent from exact Figma coordinates so it matches pixel-for-
// pixel; hit-testing uses the same rects. Emits value-change signals that the
// Storyboard wires to the canvas view transforms (display only).
class ZoomToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit ZoomToolbar(QWidget *parent = nullptr);

    // Sync the toolbar to the canvas (updates the draggers; no signal).
    void setZoom(double zoom);        // 0.25 .. 4.0
    void setRotation(double degrees); // -180 .. 180
    void setFlipH(bool on);

signals:
    void zoomChanged(double zoom);        // dragged the zoom track
    void rotationChanged(double degrees); // dragged the rotate track / reset
    void flipToggled();                   // clicked Flip

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    enum Drag { DragNone, DragZoom, DragRotate, DragGrip };

    double zoomT() const;   // current zoom -> 0..1 along the track (log map)
    double rotationT() const; // current rotation -> 0..1 along the track (linear)

    Drag m_drag = DragNone;
    double m_zoom = 1.0;
    double m_rotation = 0.0;
    bool m_flipH = false;

    QPoint m_dragStartGlobal; // grip drag: cursor at press
    QPoint m_toolbarStartPos; // grip drag: toolbar pos at press

    QFont m_labelFont;
};
