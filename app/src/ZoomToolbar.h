#pragma once

#include "FloatingToolWindow.h"

#include <QFont>
#include <QPoint>

// Custom-painted Canvas View Controls toolbar (fixed 528x46, Figma node
// 86:32). Everything is drawn in paintEvent from exact Figma coordinates so it
// matches pixel-for-pixel; hit-testing uses the same rects. The Fit Screen
// and Reset buttons have hover and pressed states. Emits value-change signals
// that the Storyboard wires to the canvas view transforms (display only).
//
// Floats over the canvas as a FloatingToolWindow (top-level tool window):
// grip drag, clamping, main-window follow, and position persistence all come
// from the base class.
class ZoomToolbar : public FloatingToolWindow
{
    Q_OBJECT

public:
    explicit ZoomToolbar(QWidget *anchor, QWidget *parent = nullptr);

    // Sync the toolbar to the canvas (updates the draggers; no signal).
    void setZoom(double zoom);        // 0.25 .. 4.0
    void setRotation(double degrees); // -180 .. 180

signals:
    void zoomChanged(double zoom);        // dragged the zoom track
    void rotationChanged(double degrees); // dragged the rotate track / reset
    void fitRequested();                  // clicked Fit Screen

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    QRect gripRect() const override;      // the grab-dots band
    QPoint defaultOffset() const override; // bottom-centre of the canvas

private:
    enum Drag { DragNone, DragZoom, DragRotate };
    enum Button { BtnNone, BtnFit, BtnReset };

    double zoomT() const;   // current zoom -> 0..1 along the track (log map)
    double rotationT() const; // current rotation -> 0..1 along the track (linear)
    Button buttonAt(const QPoint &pos) const; // Fit / Reset hit-test

    Drag m_drag = DragNone;
    Button m_hover = BtnNone;   // button under the cursor
    Button m_pressed = BtnNone; // button held down
    double m_zoom = 1.0;
    double m_rotation = 0.0;

    QFont m_labelFont; // "Zoom" / "Rotate" (9px)
    QFont m_valueFont; // numeric readouts (8px)
};
