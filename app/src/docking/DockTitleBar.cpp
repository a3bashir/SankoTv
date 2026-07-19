#include "DockTitleBar.h"

#include "DockController.h"

#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>

DockTitleBar::DockTitleBar(QDockWidget *dock, DockController *controller)
    : QFrame(dock), m_dock(dock), m_controller(controller)
{
    setObjectName(QStringLiteral("dockTitleBar")); // app stylesheet hook
    setFixedHeight(DockController::kTitleBarHeight);
    setCursor(Qt::OpenHandCursor);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 0, 6, 0);
    layout->setSpacing(7);

    // Grip glyph (braille dots) signals "draggable".
    QLabel *grip = new QLabel(QString::fromUtf8("\xE2\xA0\xBF")); // U+283F
    grip->setObjectName(QStringLiteral("dockTitleGrip"));
    layout->addWidget(grip);

    QLabel *title = new QLabel(m_dock->windowTitle());
    title->setObjectName(QStringLiteral("dockTitleText"));
    layout->addWidget(title);
    // The dock title can change at runtime; keep the label in step.
    connect(m_dock, &QDockWidget::windowTitleChanged, title,
            &QLabel::setText);

    layout->addStretch(1);

    QPushButton *closeButton =
        new QPushButton(QString::fromUtf8("\xC3\x97")); // multiplication x
    closeButton->setObjectName(QStringLiteral("dockTitleClose"));
    closeButton->setFixedSize(22, 22);
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setFocusPolicy(Qt::NoFocus);
    closeButton->setToolTip(QStringLiteral("Close panel"));
    connect(closeButton, &QPushButton::clicked, m_dock, &QWidget::close);
    layout->addWidget(closeButton);
}

void DockTitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressGlobalPosition = event->globalPosition().toPoint();
        m_dragCandidate = true;
        m_dragging = false;
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void DockTitleBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragCandidate || !(event->buttons() & Qt::LeftButton)) {
        QFrame::mouseMoveEvent(event);
        return;
    }
    const QPoint globalPos = event->globalPosition().toPoint();

    if (!m_dragging) {
        // The drag begins only past the platform threshold, so a
        // double-click never floats the panel by accident.
        if ((globalPos - m_pressGlobalPosition).manhattanLength()
            < QApplication::startDragDistance()) {
            event->accept();
            return;
        }
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        if (!m_dock->isFloating()) {
            // Float in place under the cursor. A collapsed panel keeps its
            // pinned title-bar-only height — floating must NOT expand it.
            const bool collapsed = m_controller->isDockCollapsed(m_dock);
            m_dock->setFloating(true);
            if (!collapsed)
                m_dock->resize(qMax(260, m_dock->width()),
                               qMax(240, m_dock->height()));
        }
        m_dragOffset = globalPos - m_dock->frameGeometry().topLeft();
        if (m_dragOffset.x() < 0 || m_dragOffset.x() > m_dock->width())
            m_dragOffset = QPoint(m_dock->width() / 2,
                                  DockController::kTitleBarHeight / 2);
    }

    m_dock->move(m_controller->clampedFloatingPosition(
        m_dock, globalPos - m_dragOffset));
    m_controller->updateDockPreview(m_dock, globalPos);
    event->accept();
}

void DockTitleBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const bool wasDragging = m_dragging;
        m_dragCandidate = false;
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        if (wasDragging)
            m_controller->finishDock(m_dock,
                                     event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void DockTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragCandidate = false;
        m_dragging = false;
        m_controller->toggleDockCollapse(m_dock);
        event->accept();
        return;
    }
    QFrame::mouseDoubleClickEvent(event);
}
