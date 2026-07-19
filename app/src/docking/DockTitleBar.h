#pragma once

#include <QFrame>
#include <QPoint>

class DockController;
class QDockWidget;

// Custom title bar for DockController-managed QDockWidgets: grip glyph +
// panel title + Close button (deliberately NO float icon — floating happens
// by dragging). Behaviour:
//   * dragging starts only after the cursor travels
//     QApplication::startDragDistance() — a double-click never flashes into
//     a drag;
//   * dragging a docked panel floats it under the cursor and drives the
//     controller's docking preview; release drops it (tab / split / edge /
//     stays floating);
//   * double-click collapses/expands the panel via the controller — a
//     collapsed panel stays collapsed when moved, floated or redocked.
// Styled by the application through the "dockTitleBar" objectName.
class DockTitleBar : public QFrame
{
    Q_OBJECT

public:
    DockTitleBar(QDockWidget *dock, DockController *controller);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QDockWidget *m_dock = nullptr;
    DockController *m_controller = nullptr;
    QPoint m_pressGlobalPosition;
    QPoint m_dragOffset;
    bool m_dragCandidate = false;
    bool m_dragging = false;
};
