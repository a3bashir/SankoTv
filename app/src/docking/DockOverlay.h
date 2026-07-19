#pragma once

#include <QColor>
#include <QPointer>
#include <QRect>
#include <QWidget>

class QDockWidget;
class QPropertyAnimation;

// Reusable native-QDockWidget docking layer (no third-party docking library).
// DockOverlay + DockTitleBar + DockController are application-agnostic: they
// depend only on a host QMainWindow and are styled from the application layer
// (objectName selectors), never from hard-coded app colours beyond the accent.

// Where a dragged dock would land, relative to a target dock (or the host
// window when no target is under the cursor).
enum class DockZone { None, Left, Right, Top, Bottom, Center };

// One drag hit-test result: the zone, the preview rectangle to paint (host
// coordinates), and the dock under the cursor (null = host-edge docking).
struct DockHit
{
    DockZone zone = DockZone::None;
    QRect preview;
    QPointer<QDockWidget> target;
};

// Translucent, mouse-transparent overlay covering the host window during a
// title-bar drag. Paints the animated docking preview in the accent colour:
// a soft looping glow, a filled rounded preview for area/tab drops, a thin
// insertion LINE when a drop would split beside another panel, and a dashed
// outline around the target dock.
class DockOverlay : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal glowOpacity READ glowOpacity WRITE setGlowOpacity)

public:
    explicit DockOverlay(QWidget *host,
                         const QColor &accent = QColor(0x7c, 0x6e, 0xf6));

    void showPreview(const QRect &previewRect, DockZone zone,
                     QDockWidget *target);
    void hidePreview();

    qreal glowOpacity() const { return m_glowOpacity; }
    void setGlowOpacity(qreal opacity);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QRect m_previewRect;
    DockZone m_zone = DockZone::None;
    QPointer<QDockWidget> m_targetDock;
    QPropertyAnimation *m_animation = nullptr;
    qreal m_glowOpacity = 0.25;
    QColor m_accent;
};
