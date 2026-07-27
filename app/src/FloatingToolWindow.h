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
    //  * Grab Control: the Figma grab_CTL pill (50x8 #212121 capsule,
    //    nodes 213:79/81/83) centred in a 12px-gap strip BELOW the content;
    //    hover-shown (like the Brush Size bar's grab), the SOLE drag region,
    //    with a click-vs-drag threshold of QApplication::startDragDistance().
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
    //  * Persistence: versioned keys storyboard/floatToolbars/v1/<name>/*
    //    (edge, offset, visible), validated through the same clamp+collision
    //    path on restore.
    enum class SnapEdge { None, Left, Right, Top, Bottom };
    enum class DefaultCorner { TopLeft, TopRight, BottomLeft, BottomRight };
    // Which side of the bar the grab pill sits on. Decided ONLY at placement
    // time (drop, snap, reposition — never per mouse-move, so it cannot
    // flicker mid-drag): snapped Top => Below, snapped Bottom => Above;
    // free bars AND Left/Right-snapped bars use the region half their
    // centre occupies (top half => Below, bottom half => Above).
    enum class PillSide { Below, Above };
    static constexpr int kMargin = 4;         // to edges AND between bars
    static constexpr int kSnapThreshold = 16; // px from a region edge
    static constexpr int kPillW = 50;         // Figma grab_CTL
    static constexpr int kPillH = 8;
    // Bar <-> pill gap: 4px, matching the Floating Brush Size bar's grab
    // (SizeCtlBar::kGap). The pill strip (gap + pill) is reserved in the
    // LAYOUT RECT permanently — margins, snapping, and collision all operate
    // on the full window rect whether or not the pill is currently drawn.
    static constexpr int kPillGap = 4;
    static constexpr int kPillStrip = kPillGap + kPillH; // reserved footprint
    // The Sanko accent from the existing design language (the same value the
    // Brush Size bar's armed grab already uses) — never a new colour.
    static QColor accentColor() { return QColor(0x7c, 0x6e, 0xf6); }

    void enableManagedPlacement(const QString &name, DefaultCorner corner,
                                int contentHeight);
    bool isManaged() const { return m_managed; }
    int contentHeight() const { return m_contentH; }
    SnapEdge snapEdge() const { return m_snapEdge; }
    PillSide pillSide() const { return m_pillSide; }
    // Content y-offset inside the window: the pill strip sits above the bar
    // when the pill side is Above (bottom-positioned toolbars).
    int contentOffsetY() const
    {
        return m_pillSide == PillSide::Above ? kPillStrip : 0;
    }
    QRect grabPillRect() const; // own coords (empty when not managed)
    bool grabPillVisible() const { return m_pillVisible; }
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
    // the event was consumed (see ZoomToolbar). In managed mode these SAME
    // entry points implement the grab-pill drag (threshold, snap, collision),
    // so subclasses need no changes when they opt in.
    bool handleGripPress(QMouseEvent *event);
    bool handleGripMove(QMouseEvent *event);
    bool handleGripRelease(QMouseEvent *event);

    // Managed subclasses call this at the end of their paintEvent: draws the
    // hover-shown grab pill (accent-tinted while armed/dragging).
    void paintGrabPill(QPainter &painter) const;
    // The pill flipped sides (placement moved the bar across the region's
    // midline): subclasses relayout/repaint their content at contentOffsetY().
    virtual void pillSideChanged() {}

    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    friend class FloatingToolWindowManager;

    QPoint anchorOrigin() const;                // anchor top-left, global coords
    QPoint clampedPos(const QPoint &pos) const; // POSITION-only clamp
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
    void updatePillSide(); // placement-time only: deterministic, no flicker
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
    int m_contentH = 0;             // bar content height above the pill strip
    int m_placeOrder = 0;           // priority: earlier bars win space
    SnapEdge m_snapEdge = SnapEdge::None;
    PillSide m_pillSide = PillSide::Below;
    QPoint m_freeOffset;            // anchor-relative desired top-left
    bool m_hasPlacement = false;    // false: use the default corner
    bool m_pillVisible = false;     // hover-shown grab pill
    bool m_pillCandidate = false;   // pressed on the pill (click until moved)
    bool m_pillDragging = false;    // moved past the threshold
    bool m_noSpaceHidden = false;   // fallback: hidden until space frees up
    int m_lastPlaceTests = 0;
};
