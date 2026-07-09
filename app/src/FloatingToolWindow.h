#pragma once

#include <QPoint>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

// Frameless, OS-composited floating tool window: the proven architecture for
// every element that floats over the drawing canvas. Because it is a real
// top-level window (Qt::Tool), nothing in the widget tree — most importantly
// the constantly-repainting canvas — can ever clip it, so it always renders
// complete during fast drags.
//
//  - Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus
//    (WS_EX_NOACTIVATE): tied to the main window, never activates or steals
//    canvas focus. WA_TranslucentBackground keeps antialiased corners smooth;
//    WA_ShowWithoutActivating keeps showing it from grabbing activation.
//  - Subclasses paint/lay out their content at (0,0) in their own window.
//  - GLOBAL-coordinate grip drag: gripRect() (or a designated grip widget)
//    declares where a drag may start; the position-only clamp keeps the
//    window inside the anchor's on-screen rect at FULL size (never shrinks).
//  - Follows the main window: ONE shared event filter (see the manager in the
//    .cpp) repositions every registered instance on main-window Move/Resize,
//    hides them on minimize, restores them on show, and mirrors the anchor's
//    own visibility (so they vanish when the user leaves the Storyboard).
//  - Per-window persistence: the canvas-relative offset is stored under the
//    QSettings key the subclass provides (empty key = not persisted).
//  - Registry: every instance registers itself on construction, so the shared
//    filter automatically covers any FUTURE floating panel that inherits this.
class FloatingToolWindow : public QWidget
{
    Q_OBJECT

public:
    // anchor: the widget the window floats over (the drawing canvas). Clamp
    // bounds, the offset origin, and visibility all derive from it.
    explicit FloatingToolWindow(QWidget *anchor,
                                const QString &settingsKey = QString(),
                                QWidget *parent = nullptr);
    ~FloatingToolWindow() override;

    // Records the caller's intent (tool toggles call this / show() / hide());
    // the window is effectively visible only while the anchor and the main
    // window allow it (anchor shown, window shown and not minimized).
    void setVisible(bool visible) override;

    // Place at anchor origin + (persisted or default) offset, clamped.
    void reposition();

    QWidget *anchorWidget() const { return m_anchor; }

    // Convenience wiring so plain instances don't need a subclass: the grip
    // widget's geometry becomes the default gripRect() (its mouse events fall
    // through to this window), and the provider supplies defaultOffset().
    void setGripWidget(QWidget *grip) { m_gripWidget = grip; }
    void setDefaultOffsetProvider(std::function<QPoint()> provider)
    {
        m_defaultOffset = std::move(provider);
    }

protected:
    // Region (own coords) where a drag may start. Default: the grip widget's
    // geometry if set, else empty (not draggable).
    virtual QRect gripRect() const;
    // Canvas-relative spot used until the user drags the window (re-derived
    // on every reposition so defaults can track the anchor's size).
    virtual QPoint defaultOffset() const;

    // Grip drag in GLOBAL screen coords with the position-only clamp.
    // Subclasses that override the mouse events call these first and stop if
    // the event was consumed (see ZoomToolbar).
    bool handleGripPress(QMouseEvent *event);
    bool handleGripMove(QMouseEvent *event);
    bool handleGripRelease(QMouseEvent *event);

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    friend class FloatingToolWindowManager;

    QPoint anchorOrigin() const;                // anchor top-left, global coords
    QPoint clampedPos(const QPoint &pos) const; // POSITION-only clamp
    void applyEffectiveVisibility();            // intent && anchor/window state
    void persistOffset();

    QPointer<QWidget> m_anchor;
    QString m_settingsKey;
    QPoint m_offset;            // canvas-relative
    bool m_userPlaced = false;  // false: keep following defaultOffset()
    bool m_wantVisible = false; // the caller's intent (tool toggles etc.)
    bool m_dragging = false;
    QPoint m_dragStartGlobal;   // cursor at press
    QPoint m_dragStartPos;      // window pos at press
    QPointer<QWidget> m_gripWidget;
    std::function<QPoint()> m_defaultOffset;
};
