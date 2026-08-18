#pragma once

#include "PressureCurve.h"
#include "SankoTheme.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QPaintEvent;
class QMouseEvent;
class QKeyEvent;
class QLineEdit;
class QResizeEvent;
class QEnterEvent;

namespace brushlib {

// Shared visual tokens for the Brush Settings studio (Figma 274:23; slider
// anatomy from component 273:30). Every studio control draws from this one
// palette so a token correction lands everywhere at once.
namespace studio {
inline const QColor kWindowBg(0x1e, 0x1e, 0x20);
inline const QColor kSidebarBg(0x25, 0x25, 0x28);
inline const QColor kCanvasBg(0x16, 0x16, 0x17);
inline const QColor kBorder(0x2d, 0x2d, 0x31);
inline const QColor kAccent = SankoTheme::kPurple;
inline const QColor kAccentDark(0x4b, 0x43, 0x97);
inline const QColor kTrack(0x33, 0x33, 0x33);
inline const QColor kDragger(0xb3, 0xb3, 0xb3);
inline const QColor kCapsuleBg(0x11, 0x11, 0x12);
inline const QColor kTextDim(0x96, 0x96, 0x9b);

QFont labelFont();   // Inter Medium 14 (row labels, #96969b)
QFont capsuleFont(); // Inter Medium 12 (value capsules)

// Draw the 55x23 value-capsule pill used on every property row.
void paintCapsule(QPainter &p, const QRect &r, const QString &text);
// Draw a capsule-sized thumbnail of a pressure curve (the design has no
// curve control; this chip reuses the capsule geometry with the curve
// rendered in the accent colour — documented divergence, drawn in the
// file's visual language).
void paintCurveChip(QPainter &p, const QRect &r, const PressureCurve &curve,
                    bool expanded);

// Field tokens (New Project dialog, Figma 350:24) — the input-box language
// shared by StudioTextField and StudioDropdown: #1c1c1c wells with #333
// borders, #ccc values, #999 labels. Hover lightens the border; focus uses
// the accent. Any dialog reusing these components inherits the states.
inline const QColor kFieldBg(0x1c, 0x1c, 0x1c);
inline const QColor kFieldBorder(0x33, 0x33, 0x33);
inline const QColor kFieldBorderHover(0x4a, 0x4a, 0x4a);
inline const QColor kFieldText(0xcc, 0xcc, 0xcc);
inline const QColor kFieldLabel(0x99, 0x99, 0x99);
QFont fieldFont();      // Inter Regular 11 (input values)
QFont fieldLabelFont(); // Inter Medium 10 (field labels)
} // namespace studio

// The custom slider row (Figma 273:30 inside 274:72..104): label + value
// capsule on a 23px line, 12px gap, then the 6px track. NOT a QSlider — the
// design's track/fill/dragger are custom-painted:
//   track  full width, 6px, #333, radius 1
//   fill   left portion, gradient #4b4397 (left) -> #7c6ef6 (right) with a
//          white 10% -> 0 top sheen, radius 1
//   dragger 20x6 #b3b3b3, radius 1, flush at the fill's right end
// The press target is the full band below the label line (the design's
// invisible 40px-tall "Bounds" strip), so the 6px track is not the hit area.
class StudioSlider : public QWidget
{
    Q_OBJECT
public:
    explicit StudioSlider(const QString &label, double min, double max,
                          QWidget *parent = nullptr);

    void setValue(double value); // silent (no signals); clamped to range
    double value() const { return m_value; }
    QString label() const { return m_label; } // programmatic lookup (tests)
    void setStep(double step) { m_step = step; } // 0 = continuous
    // Track response: value = min + (max-min) * t^exponent. Wide ranges
    // (spacing 1%..1000%, size 1..2048) use >1 so the everyday span is not
    // crushed into the first few percent of track. Purely an input/display
    // mapping — the value, range, and capsule text stay honest.
    void setExponent(double exponent) { m_exponent = exponent; }
    // Display text for the value capsule ("12%", "48 px", "4.0x", ...).
    void setFormatter(std::function<QString(double)> f);

    // Optional pressure-curve chip between the label and the value capsule.
    // The chip is display + click-target only; the owning section places the
    // expandable response drawer underneath the row.
    void enableCurveChip();
    void setChipCurve(const PressureCurve &curve);
    void setChipExpanded(bool expanded);
    // Control-source capsule beside the chip — ALWAYS visible on dynamic
    // rows ("Pressure" by default), so a row states its source instead of
    // relying on an absent-capsule-means-Pressure convention. Documented
    // divergence, drawn with the shared capsule painter.
    void setSourceCapsule(const QString &text);
    // Dim the chip + source capsule (source = None: curve and minimum are
    // inert) without disabling the row — the chip stays clickable so the
    // drawer remains reachable to change the source back.
    void setChipDimmed(bool dimmed);

signals:
    void valueChanged(double value);                 // live, during drag
    void valueCommitted(double before, double after); // on release / click
    void chipClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QRect trackRect() const;
    QRect capsuleRect() const;
    QRect chipRect() const;
    double valueAtX(int x) const;
    void applyDragValue(int x);

    QString m_label;
    double m_min, m_max;
    double m_value;
    double m_step = 0.0;
    double m_exponent = 1.0;
    std::function<QString(double)> m_format;
    bool m_dragging = false;
    double m_pressValue = 0.0;
    bool m_hasChip = false;
    bool m_chipExpanded = false;
    bool m_chipDimmed = false;
    QString m_sourceText;
    PressureCurve m_chipCurve;
};

// A curve-only property row (the Dynamics section): label + curve chip in
// the capsule position. Clicking toggles the editor the section places
// below it.
class StudioCurveRow : public QWidget
{
    Q_OBJECT
public:
    explicit StudioCurveRow(const QString &label, QWidget *parent = nullptr);
    QString label() const { return m_label; }
    void setCurve(const PressureCurve &curve);
    void setExpanded(bool expanded);
    // Same always-visible source capsule + dim rules as StudioSlider's.
    void setSourceCapsule(const QString &text);
    void setChipDimmed(bool dimmed);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QString m_label;
    PressureCurve m_curve;
    bool m_expanded = false;
    bool m_chipDimmed = false;
    QString m_sourceText;
};

// The expandable pressure-curve editor. PressureCurve is piecewise-LINEAR
// control points clamped to [0,1] including both endpoints, so the editor
// is exactly that: draggable points joined by straight segments.
//   drag a point to move it (endpoints keep x = 0 / x = 1)
//   click empty graph space to add a point
//   double-click an interior point to remove it
class StudioCurveEditor : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kHeight = 172;
    explicit StudioCurveEditor(QWidget *parent = nullptr);
    void setCurve(const PressureCurve &curve); // silent
    PressureCurve curve() const { return m_curve; }

signals:
    void curveChanged(const PressureCurve &curve); // live, during drag
    void curveCommitted(); // gesture finished (release / add / remove)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QRect graphRect() const;
    QPointF toGraph(const QPointF &curvePt) const;  // curve -> widget
    QPointF fromGraph(const QPointF &widgetPt) const;
    int pointAt(const QPointF &widgetPt) const;

    PressureCurve m_curve;
    int m_dragIndex = -1;
    int m_hoverIndex = -1;
};

// Label + on/off switch, in the slider row's label style. The design has no
// toggle control; the switch is drawn from the same tokens (track #333, knob
// #b3b3b3, accent fill when on) — documented divergence.
class StudioToggleRow : public QWidget
{
    Q_OBJECT
public:
    explicit StudioToggleRow(const QString &label, QWidget *parent = nullptr);
    QString label() const { return m_label; }
    void setChecked(bool on); // silent
    bool isChecked() const { return m_on; }

signals:
    void toggled(bool on); // user action only

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRect switchRect() const;
    QString m_label;
    bool m_on = false;
};

// Label + capsule dropdown (current value + chevron; opens a QMenu). Used
// for enum parameters (grain preset, dual blend mode). Same capsule
// geometry as the value pill — documented divergence, drawn in-language.
class StudioChoiceRow : public QWidget
{
    Q_OBJECT
public:
    StudioChoiceRow(const QString &label, const QStringList &options,
                    QWidget *parent = nullptr);
    QString label() const { return m_label; }
    void setCurrentIndex(int index); // silent
    int currentIndex() const { return m_index; }
    // The programmatic counterpart of picking from the menu: sets the
    // index AND emits chosen(), exactly like a user selection.
    void choose(int index);

signals:
    void chosen(int index); // user action only

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRect capsuleRect() const;
    QString m_label;
    QStringList m_options;
    int m_index = 0;
};

// Label + segmented pill (2-3 options; Paint|Smudge, Rolling|Static). The
// active segment uses the sidebar's active style (#7c6ef6 pill, white
// text); idle segments the dim text on the capsule background.
class StudioSegmentedRow : public QWidget
{
    Q_OBJECT
public:
    StudioSegmentedRow(const QString &label, const QStringList &options,
                       QWidget *parent = nullptr);
    QString label() const { return m_label; }
    void setCurrentIndex(int index); // silent
    int currentIndex() const { return m_index; }
    void choose(int index); // programmatic pick: sets index + emits chosen()

signals:
    void chosen(int index); // user action only

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QRect segmentsRect() const;
    QString m_label;
    QStringList m_options;
    int m_index = 0;
};

// The Shape Control panel (Figma 345:99): direct-manipulation editor for
// the STATIC tip transform — drag the ring to set tipAngle, drag any of
// the four handles to set tipRoundness, click the centre pivot to reset
// both. A second editor for two existing fields — the Tip Angle / Tip
// Roundness sliders remain the precise numeric entry, and both stay synced
// through the studio's syncer pass. The control is also its own preview:
// the ring IS the tip's ellipse (see below), the handle constellation
// rotates with the angle, and the squash pair pulls inward with roundness.
//
// The panel wraps the revised ring node (341:30) in chrome: a #3C3C42
// rounded panel with a #595964 inset outline, a static #555560 guide
// circle at r 58 marking the unsquashed rim, #595964 crosshair guide
// lines from the outline to the guide circle, and a DISPLAY-ONLY value
// capsule (top-left, the studio's shared paintCapsule). The capsule shows
// the live angle ("34°") during a ring drag, the live roundness ("62%")
// during a handle drag, "None" at rest while both values are default, and
// a compact combination at rest otherwise — only the non-default value
// when just one differs, "34° · 62%" when both do. It is not a hit
// region: like all panel chrome it resolves to None, so a press on it can
// never start a drag. The sliders keep numeric entry; the tip thumbnail
// stays the only surface showing flips and the actual mask.
//
// Ring tokens per the revised node: stroke #7C6EF6 (kAccent) width 2 at
// r 44.5; four two-tone handles (r 4 #D9D9D9 disc, r 2 #7C6EF6 dot); the
// pivot is an r 2.5 #CCCCCC circle plus four 4 px crosshair ticks.
//
// DOCUMENTED DIVERGENCES from the design:
// (1) WIDTH. The frame is 356 wide but the props column offers
//     384 - 2*32 = 320 of content width, so the panel adapts to 320 x 210
//     with every internal token verbatim: the ring, guide circle, capsule
//     and vertical guides are untouched and centred; only the horizontal
//     guide lines — which self-define as outline-to-circle — shorten.
// (2) CENTRES. The design drifts by half-pixels (ring centre 178.5 vs
//     panel centre 178, crosshair at x 179 / y 105 vs ring centre 104.5).
//     Everything is drawn from the panel's true centre instead.
// (3) NAMING ONLY. The design labels the top/bottom handles
//     "shape_control" and left/right "rotation_control", implying a
//     vertical squash pair at rest. The engine's squash axis is tip-local
//     X — HORIZONTAL at angle 0 — and the ring must not lie about the
//     affine, so behaviour stays engine-true: all four handles drive the
//     one roundness, the ring band rotates. All four handles are visually
//     identical in the design, so nothing on screen diverges.
// (4) The design carries no hover or active states; hovered/dragged
//     elements brighten in the component palette's language.
// (5) Hit targets exceed drawn geometry (4 px handles grab at 12 px, the
//     2 px ring stroke at +/-10 px of its ellipse, the pivot at 10 px) —
//     the floating-toolbar rule that interactive area exceeds drawn area.
class StudioTipRing : public QWidget
{
    Q_OBJECT
public:
    explicit StudioTipRing(QWidget *parent = nullptr);

    // Silent (no signals) — the syncer path, mirroring StudioSlider.
    void setTipValues(double angleDegrees, double roundness);
    double tipAngle() const { return m_angle; }
    double tipRoundness() const { return m_roundness; }

    // Geometry the seam asserts against; also used by the hit tests.
    QPointF ringCentre() const;                 // panel's true centre
    // Handle index parity is load-bearing: EVEN indices (0, 2) are the
    // SQUASH pair — the ones roundness pulls inward along tip-local X —
    // and odd indices (1, 3) are the unsquashed pair, always on the rim.
    QPointF handleCentre(int index) const;      // 0 +squash, 1 -keep,
                                                // 2 -squash, 3 +keep
    // What the display-only capsule currently reads; the rect it paints
    // in. Exposed for the seam (text scheme + press-on-capsule-is-None).
    QString capsuleText() const;
    QRect capsuleRect() const;
    static constexpr int kPanelWidth = 320;  // design 356; divergence (1)
    static constexpr int kPanelHeight = 210;
    static constexpr double kRingRadius = 44.5;
    static constexpr double kGuideRadius = 58.0; // static unsquashed rim
    static constexpr double kHandleRadius = 4.0;
    static constexpr double kHandleDotRadius = 2.0;
    static constexpr double kPivotRadius = 2.5;
    // The pivot's crosshair ticks: 4 px long, from 2.5 to 6.5 px out.
    static constexpr double kPivotTickInner = 2.5;
    static constexpr double kPivotTickOuter = 6.5;
    static constexpr double kHandleGrabRadius = 12.0;    // at the rim
    static constexpr double kHandleGrabRadiusMax = 20.0; // at the inner edge
    static constexpr double kRingGrabTolerance = 10.0;
    static constexpr double kPivotGrabRadius = 10.0;
    static constexpr double kAngleSnapDegrees = 15.0;  // Shift on the ring
    static constexpr double kRoundnessSnapStep = 0.10; // Shift on a handle
    static constexpr double kRoundnessFloor = 0.01;
    // The ring band's edges; the interior is (kPivotGrabRadius, inner).
    static constexpr double kRingInner = kRingRadius - kRingGrabTolerance;
    static constexpr double kRingOuter = kRingRadius + kRingGrabTolerance;

    // THE RING IS THE TIP. It is drawn as the ellipse the engine's own
    // affine produces — semi-axis kRingRadius * roundness along the SQUASH
    // direction (cos angle, sin angle) and kRingRadius along the
    // unsquashed direction (-sin angle, cos angle), y down. That is the
    // boundary of StrokeBuilder::shapedTipForStamp's keep test (and
    // stamp.frag's, which is identical in form):
    //
    //     tx = ( cos a * lx + sin a * ly) / roundness
    //     ty = (-sin a * lx + cos a * ly)
    //     the tip keeps hypot(tx, ty) < 1
    //
    // so inverting the boundary gives exactly the ellipse above. tipSpace()
    // below IS that transform, which is what lets the seam assert the drawn
    // geometry against the renderer's formula rather than against a
    // picture. A circular ring was a lie about a squashed tip.
    //
    // THE HIT-TEST PARTITION — evaluated in this order, exhaustive and
    // mutually exclusive (every press resolves to exactly one region at
    // every roundness and angle; the seam proves it over a sampled grid):
    //
    //   1 PIVOT   distance from centre <= 10 px
    //   2 HANDLE  within a handle's own grab radius (scaled for the
    //             squash pair, fixed for the unsquashed pair)
    //   3 RING    within 10 px of the ELLIPSE, measured as a true pixel
    //             distance (signedRingDistance) rather than a distance
    //             from the centre — the old |d - 40.5| <= 10 band assumed
    //             a circle and is meaningless once the ring deforms
    //   4 HANDLE  anything else still inside the control's reach
    //             (<= kRingRadius + 10 from the centre): the interior
    //             rule, which grabs the nearer SQUASH handle
    //   5 NONE    beyond that
    //
    // At roundness 1.0 the ellipse is the old circle and rules 1-5 reduce
    // to the previous distance bands exactly, so nothing about the
    // unsquashed control changed.
    //
    // The interior rule (4) closes the dead annulus a dev recording found:
    // a press between the pivot and the band used to resolve to NONE and
    // do nothing at all. It also carries the whole control at heavy
    // squash: once the ellipse collapses toward a sliver the band is a
    // thin capsule and almost the entire disc becomes interior, so
    // roundness stays editable from anywhere while the sliver itself
    // stays available for rotation.
    //
    // The pivot owns the innermost disc outright. Below roundness
    // 10/44.5 = 0.225 the squash handles pass UNDER it, so a press at a
    // squash handle's own centre resets instead of grabbing — deliberate,
    // because the reset must stay reachable. The threshold is
    // kPivotGrabRadius / kRingRadius: the handles sit at
    // kRingRadius * roundness along the squash axis, which is the
    // ellipse's cardinal point (it moved from 0.247 when the revised
    // design grew the ring from r 40.5 to 44.5).
    //
    // The panel chrome — background, outline, guide circle and lines, and
    // the capsule — is all rule 5: NONE. Nothing outside the ring's reach
    // is interactive, so the capsule can never start a drag.
    enum class Region { None, Ring, Handle, Pivot };
    Region hitTest(const QPointF &pos) const; // exposed for the seam
    // Which handle a press would grab, or -1 when the press is not a
    // handle grab. In the interior the nearer SQUASH handle wins, with
    // ties — a press exactly on the unsquashed axis, equidistant from
    // both — going to index 0 deterministically.
    int handleForPress(const QPointF &pos) const;
    // The squash pair's grab radius, which GROWS as roundness pulls the
    // handles inward: kHandleGrabRadius at the rim up to
    // kHandleGrabRadiusMax once the handle reaches the band's inner edge.
    // The unsquashed pair never moves, so it keeps the fixed radius.
    double squashGrabRadius() const;
    // The engine's tip transform applied to a widget point, in pixels:
    // the ellipse's edge is where hypot() equals kRingRadius. Exposed so
    // the seam can check the drawn geometry against the renderer's own
    // formula.
    QPointF tipSpace(const QPointF &pos) const;
    // Signed PIXEL distance from a widget point to the ring's ellipse,
    // negative inside. Uses the implicit-form gradient, which is exact on
    // both axes and close elsewhere; a plain distance-from-centre test
    // cannot describe a deformed ring.
    double signedRingDistance(const QPointF &pos) const;
    // The ellipse's semi-axes in pixels: x along the squash direction,
    // y along the unsquashed one.
    QPointF ringSemiAxes() const;

signals:
    void angleEdited(double degrees);     // live, during a ring drag
    void roundnessEdited(double value);   // live, during a handle drag
    void editCommitted();                 // on release, one per gesture
    void resetRequested();                // pivot click (no drag)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    static double wrapAngle(double degrees); // to (-180, 180]
    double pointerAngle(const QPointF &pos) const;
    void applyRingDrag(const QPointF &pos, Qt::KeyboardModifiers modifiers);
    void applyHandleDrag(const QPointF &pos,
                         Qt::KeyboardModifiers modifiers);
    void updateCursorFor(Region region);

    double m_angle = 0.0;      // degrees, (-180, 180]
    double m_roundness = 1.0;  // 0.01 - 1.0
    Region m_drag = Region::None;
    Region m_hover = Region::None;
    double m_grabOffset = 0.0; // ring drag: angle - pointerAngle at press
    bool m_moved = false;      // distinguishes pivot click from drag
};

// Shared single-line text input (first needed by the New Project dialog;
// built as a library component because other dialogs will want it). A
// custom-painted well in the field language hosting an embedded FRAMELESS
// QLineEdit — caret, selection, IME and clipboard come from Qt, every
// painted pixel is ours. Re-implementing text editing loses IME silently.
// numericMode() restricts input to digits and reports intValue().
class StudioTextField : public QWidget
{
    Q_OBJECT
public:
    explicit StudioTextField(QWidget *parent = nullptr);
    QString text() const;
    void setText(const QString &text);
    void setPlaceholder(const QString &text);
    // Digits only (an int validator makes non-numeric input unenterable).
    void setNumericMode(int minValue, int maxValue);
    int intValue() const;
    // Disabled-but-legible: dimmed value, no focus ring, input refused —
    // used by Width/Height while a non-Custom preset is active, so blocked
    // editing is VISIBLE, never silently ignored.
    void setFieldEnabled(bool enabled);
    bool fieldEnabled() const { return m_fieldEnabled; }
    QLineEdit *edit() const { return m_edit; } // for tab order / tests

signals:
    void textEdited(const QString &text);
    void submitted(); // Enter pressed inside the field

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QLineEdit *m_edit = nullptr;
    bool m_hover = false;
    bool m_fieldEnabled = true;
};

// Shared dropdown (New Project dialog): a painted field-language box with a
// chevron; clicking opens a styled popup list (Qt::Popup widget, hover
// highlight, click selects). Deliberately NOT a QComboBox and NOT the
// studio's cycle-style StudioChoiceRow — the design shows a popup menu.
class StudioDropdown : public QWidget
{
    Q_OBJECT
public:
    StudioDropdown(const QStringList &options, QWidget *parent = nullptr);
    int currentIndex() const { return m_index; }
    QString currentText() const;
    void setCurrentIndex(int index); // silent
    void choose(int index);          // sets AND emits, like a user pick

signals:
    void chosen(int index); // user action (or choose())

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void openPopup();
    QStringList m_options;
    int m_index = 0;
    bool m_hover = false;
};

} // namespace brushlib
