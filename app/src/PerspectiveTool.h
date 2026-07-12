#pragma once

#include <QColor>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QVector>

class QJsonObject;
class QPainter;
class QTransform;

// Tap-created 1/2/3-point perspective guides with optional stroke snapping
// ("Support Drawing").
//
// Pure model + geometry: DrawingCanvas owns one instance, renders the guides
// as a DISPLAY-ONLY overlay in canvas space (never into any layer, never in
// flattenedPixmap or exports), routes taps/drags to it while the Perspective
// tool is active, and asks it to constrain stroke points while Support
// Drawing is on. The Perspective Modifier toolbar (StoryboardPage) and
// project save/load (MainWindow) talk to the same instance.
//
// Workflow: the first tap creates VP1 and the horizon line (#311DE2 by
// default) — anywhere in the workspace, inside or outside the canvas; a
// second tap creates VP2; with two VPs a tap clearly off the
// horizon creates VP3. Each VP moves independently — the horizon is DERIVED,
// always passing through VP1 (and VP2 when it exists), so it tilts when the
// VPs sit at different heights. Every VP carries its own guide color and
// opacity; density and thickness are shared.
//
// Geometry lives in CANVAS coordinates; vanishing points may sit far outside
// the canvas bounds. Snapping receives canvas-space points (already through
// the inverse view transform), so it is correct at any zoom/rotation/flip.
class PerspectiveTool
{
public:
    struct VanishingPoint {
        QPointF pos;
        QColor color;
        qreal opacity = 0.45;
    };

    void reset(); // removes every VP; keeps density/thickness/snap defaults

    // --- Support Drawing (stroke snapping) ------------------------------------
    bool snapEnabled() const { return m_snap; }
    void setSnapEnabled(bool on) { m_snap = on; }

    // --- shared guide settings -------------------------------------------------
    int density() const { return m_density; }
    void setDensity(int lines) { m_density = qBound(2, lines, 72); }
    qreal thickness() const { return m_thickness; }
    void setThickness(qreal px) { m_thickness = qBound(0.5, px, 8.0); }

    // --- vanishing points --------------------------------------------------------
    int count() const { return m_vps.size(); }
    QPointF vanishingPoint(int index) const;
    // Creates a VP at the tap position and returns its index; returns -1 when
    // three already exist, or when two exist and the tap is too close to the
    // horizon to define a third point cleanly.
    int addVanishingPoint(const QPointF &canvasPos);
    void removeVanishingPoint(int index);
    void moveVanishingPoint(int index, const QPointF &canvasPos);

    // Selected VP: the one the Opacity and Hue Colors sliders edit.
    QColor vpColor(int index) const;
    qreal vpOpacity(int index) const;

    int selected() const { return m_selected; }
    void setSelected(int index);
    QColor selectedColor() const;    // falls back to the new-VP default
    void setSelectedColor(const QColor &color);
    qreal selectedOpacity() const;
    void setSelectedOpacity(qreal opacity);

    // Show/Hide Guides (the Modifier-bar toggle); guides render only while
    // this is on AND at least one VP exists.
    bool guidesVisible() const { return m_guidesVisible; }
    void setGuidesVisible(bool on) { m_guidesVisible = on; }
    bool isVisible() const { return m_guidesVisible && !m_vps.isEmpty(); }
    // The derived horizon: horizontal through VP1 alone, through VP1 and VP2
    // when both exist (so it tilts with them). Null when no VPs exist.
    QLineF horizonLine() const;
    QColor horizonColor() const { return m_horizonColor; }
    void setHorizonColor(const QColor &color) { m_horizonColor = color; }

    // --- rendering (painter already in CANVAS space) --------------------------
    void paintGuides(QPainter &p, const QRectF &canvasRect) const;
    // Editing handles, WIDGET space (constant screen size; may be off-canvas).
    void paintHandles(QPainter &p, const QTransform &canvasToWidget) const;

    // --- interaction -----------------------------------------------------------
    // VP handle under the cursor, or -1.
    int hitTest(const QPointF &widgetPos, const QTransform &canvasToWidget) const;

    // --- snapping ---------------------------------------------------------------
    // Stroke assist: beginStroke() anchors at the stroke start and clears the
    // ray lock; snapPoint() locks onto the best guide direction once the
    // stroke has direction — the rays to every VP plus PURE VERTICAL, so
    // upright lines stay drawable — and projects every later point onto it.
    // Returns the input unchanged while Support Drawing is off or no VPs exist.
    void beginStroke(const QPointF &anchorCanvas);
    QPointF snapPoint(const QPointF &canvasPos);
    // Stateless variant for two-point tools (shape Line): projects onto the
    // best guide direction through the given anchor.
    QPointF snapToRay(const QPointF &anchorCanvas, const QPointF &canvasPos) const;

    // --- persistence -------------------------------------------------------------
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &object); // accepts the legacy fixed-mode schema

private:
    // Best guide direction through `anchor` for a stroke heading toward
    // `pos`; false when none applies (no VPs / anchor on a VP with no
    // alternative).
    bool bestDirection(const QPointF &anchor, const QPointF &pos, QPointF *dir) const;
    static QPointF projectOntoDir(const QPointF &a, const QPointF &dir,
                                  const QPointF &p);

    QVector<VanishingPoint> m_vps; // 0..3, in creation order
    int m_selected = -1;
    bool m_snap = false;
    bool m_guidesVisible = true;
    int m_density = 12;
    qreal m_thickness = 1.0;
    QColor m_horizonColor{0x31, 0x1d, 0xe2}; // #311DE2 by default
    QColor m_defaultColor{0x4d, 0x9f, 0xff}; // colour given to new VPs
    qreal m_defaultOpacity = 0.45;

    // Stroke-assist state.
    QPointF m_strokeAnchor;
    bool m_dirLocked = false;
    QPointF m_lockedDir;
};
