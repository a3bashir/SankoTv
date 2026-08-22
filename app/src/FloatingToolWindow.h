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

    // --- Managed placement (Figma grab_CTL toolbars) ----------------------
    // Opting in replaces the legacy grip-widget drag with the shared system:
    //  * Drag from ANYWHERE on the toolbar background (2026-07: the
    //    Figma grab_CTL pill was REMOVED - see HANDOFF.md; the pill's
    //    hover-visibility state machine was a recurring bug source).
    //    Interactive children always win the press; the drag threshold is
    //    QApplication::startDragDistance().
    //  * Defaults: a corner of the placement region on first run (no saved
    //    state); a saved position always wins.
    //  * Region: the anchor's client rect deflated by kMargin — the canvas
    //    viewport, i.e. the application window's client area MINUS every
    //    docked panel, so a placed bar can never overlap a dock.
    //  * Edge snapping: released within kSnapThreshold of a region edge
    //    snaps to it; a snapped bar keeps its edge across window resizes.
    //  * Collision: bars never overlap each other (4px spacing); a blocked
    //    bar moves to the candidate position with the smallest squared
    //    displacement from the desired spot (finite candidate grid, single
    //    pass — see placeRect()); with NO valid position the bar hides and
    //    re-shows automatically once space frees up.
    //  * Persistence: versioned keys storyboard/floatToolbars/v2/<name>/*
    //    (edge, offset, visible), validated through the same clamp+collision
    //    path on restore.
    enum class SnapEdge { None, Left, Right, Top, Bottom };
    enum class DefaultCorner { TopLeft, TopRight, BottomLeft, BottomRight };
    static constexpr int kMargin = 4;         // to edges AND between bars
    static constexpr int kSnapThreshold = 16; // px from a region edge

    void enableManagedPlacement(const QString &name, DefaultCorner corner);
    bool isManaged() const { return m_managed; }
    SnapEdge snapEdge() const { return m_snapEdge; }
    // True when the point is draggable BACKGROUND: inside the window and
    // NOT over any child widget. Precedence rule: children always win a
    // press; only leftover background starts a drag. Live query - never a
    // latched flag, which is what made the old pill state recur.
    bool isDragBackgroundAt(const QPoint &pos) const;
    // Custom-painted subclasses (e.g. ZoomToolbar) have NO child widgets,
    // so childAt() cannot protect their controls: they declare their own
    // interactive regions here. Anything reported true is NOT background
    // and never starts a drag.
    virtual bool isInteractiveAt(const QPoint &pos) const
    {
        Q_UNUSED(pos);
        return false;
    }
    int lastPlacementTests() const { return m_lastPlaceTests; } // bounded
    // View > Reset Toolbar Positions: clear saved state, back to defaults.
    static void resetAllManagedPlacements();
    // Re-read the persisted state and re-place (the startup restore path,
    // callable explicitly): the saved position is validated through the same
    // clamp+collision pipeline, never applied blindly.
    void restoreManagedState();

    // Records the caller's intent (tool toggles call this / show() / hide());
    // the window is effectively visible only while the anchor and the main
    // window allow it (anchor shown, window shown and not minimized).
    void setVisible(bool visible) override;
    // The caller's intent, independent of whether the host currently allows
    // showing. This is what suppression captures and restores: reading
    // isVisible() instead would mis-capture during a minimize or page switch.
    bool visibleIntent() const { return m_wantVisible; }

    // --- Modal-surface suppression (the Brush Settings studio) ------------
    // Hide every registered floating window over `anchor` except the ones in
    // `except`, remembering each window's visibility INTENT in memory only —
    // nothing is persisted, so this can never be recorded as the user
    // choosing "hidden" (the D1 rule; the BrushLibraryPanel's autoHide() is
    // the same idea for one window, this is the same idea for all of them).
    // restore puts every captured window back to its captured intent — a bar
    // the user had already hidden stays hidden. Calls NEST: captures form a
    // per-anchor stack and each restore pops its own, so holders unwind in
    // reverse order (a modal dialog opening over the Brush Settings studio
    // hides what the studio left visible, and closing it hands the studio
    // back exactly what it had). A capture records INTENT, never effective
    // visibility, so the transient hide/show a page switch or minimize
    // produces can never be re-captured as if the user had chosen it, and
    // restore with no capture is a no-op.
    static void suppressFloatingBars(QWidget *anchor,
                                     const QVector<FloatingToolWindow *> &except = {});
    static void restoreFloatingBars(QWidget *anchor);

    // Place at anchor origin + (persisted or default) offset, clamped.
    // Virtual: the Brush Settings studio clamps against the WINDOW's client
    // area instead of the canvas marginRect — as a centred modal-style
    // surface it is legitimately larger than the canvas viewport at small
    // windows, and the canvas clamp would drag it out of centre.
    virtual void reposition();

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
    // the event was consumed (see ZoomToolbar). In managed mode these SAME
    // entry points implement the background drag (threshold, snap, collision),
    // so subclasses need no changes when they opt in.
    bool handleGripPress(QMouseEvent *event);
    bool handleGripMove(QMouseEvent *event);
    bool handleGripRelease(QMouseEvent *event);

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override; // clears the move cursor
    bool event(QEvent *event) override; // cancels a drag on deactivate

    // THE single place the kMargin window margin is applied. Both the managed
    // placement region and the unmanaged position clamp derive from
    // marginRect(), so no future edge case can leave one edge flush while the
    // others keep their margin. Subclasses that clamp their own drags call
    // clampedPos() rather than re-deriving the bounds.
    QRect marginRect() const;                   // anchor rect deflated by kMargin
    QPoint clampedPos(const QPoint &pos) const; // POSITION-only clamp

private:
    friend class FloatingToolWindowManager;

    QPoint anchorOrigin() const;                // anchor top-left, global coords
    void applyEffectiveVisibility();            // intent && anchor/window state
    void persistOffset();

    // --- managed placement internals --------------------------------------
    QRect placementRegion() const; // anchor global rect deflated by kMargin
    QRect desiredManagedRect() const;
    // Nearest-available placement: desired first; otherwise every (x, y)
    // from the finite candidate grid built from the region bounds and the
    // obstacle edges, keeping the one with the smallest squared displacement
    // (ties: smaller y, then smaller x). Single pass over a bounded set —
    // it cannot loop or oscillate. ok=false when nothing fits.
    QRect placeRect(const QRect &desired, const QVector<QRect> &obstacles,
                    bool preserveSnapAxis, bool *ok);
    QVector<QRect> managedObstacles(bool yieldToAll) const;
    void repositionManaged(bool yieldToAll);
    // Live cursor affordance: SizeAll over background, the child's own
    // cursor over a child. Derived from the current hit test on every move.
    void updateDragCursor(const QPoint &pos);
    void finishManagedDrag();
    void saveManagedState() const;
    void loadManagedState();
    static void repositionManagedGroup(QWidget *anchor);

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

    bool m_managed = false;
    QString m_name;                 // settings sub-key
    DefaultCorner m_corner = DefaultCorner::TopLeft;
    int m_placeOrder = 0;           // priority: earlier bars win space
    SnapEdge m_snapEdge = SnapEdge::None;
    QPoint m_freeOffset;            // anchor-relative desired top-left
    bool m_hasPlacement = false;    // false: use the default corner
    // Background-drag states: idle -> candidate (pressed on background,
    // still a click) -> dragging (threshold crossed). No hover state.
    bool m_bgCandidate = false;
    bool m_bgDragging = false;
    bool m_noSpaceHidden = false;   // fallback: hidden until space frees up
    int m_lastPlaceTests = 0;
};
