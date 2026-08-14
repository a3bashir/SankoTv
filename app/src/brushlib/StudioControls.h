#pragma once

#include "PressureCurve.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QPaintEvent;
class QMouseEvent;

namespace brushlib {

// Shared visual tokens for the Brush Settings studio (Figma 274:23; slider
// anatomy from component 273:30). Every studio control draws from this one
// palette so a token correction lands everywhere at once.
namespace studio {
inline const QColor kWindowBg(0x1e, 0x1e, 0x20);
inline const QColor kSidebarBg(0x25, 0x25, 0x28);
inline const QColor kCanvasBg(0x16, 0x16, 0x17);
inline const QColor kBorder(0x2d, 0x2d, 0x31);
inline const QColor kAccent(0x7c, 0x6e, 0xf6);
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

// Direct-manipulation editor for the STATIC tip transform (Figma 341:30,
// "shape control", 91x91): drag the ring to set tipAngle, drag any of the
// four handles to set tipRoundness, click the centre pivot to reset both.
// A second editor for two existing fields — the Tip Angle / Tip Roundness
// sliders remain the precise numeric entry, and both stay synced through
// the studio's syncer pass. The control is also its own preview: the
// handle constellation rotates with the current angle and the squash-axis
// pair pulls inward with the current roundness (the same forward affine
// the tip thumbnail's backward map inverts).
//
// Appearance is the Figma node verbatim — ring stroke #7C6EF6 (kAccent)
// width 8 at r 40.5, four #D9D9D9 r 5 handles, hollow #CCCCCC r 4 pivot at
// stroke 2 — with three DOCUMENTED DIVERGENCES: (1) the design's ring
// centre sits at x 46 while every other element is at 45.5; drawn from the
// true centre (45.5, 45.5) instead. (2) The design carries no hover or
// active states; hovered/dragged elements brighten in the component
// palette's language. (3) Hit targets exceed drawn geometry (5 px handles
// grab at 12 px, the 8 px ring stroke at +/-10 px of its radius, the 4 px
// pivot at 10 px) — the floating-toolbar rule that interactive area
// exceeds drawn area.
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
    QPointF ringCentre() const;                 // true centre, 45.5, 45.5
    QPointF handleCentre(int index) const;      // 0 E, 1 N, 2 W, 3 S
    static constexpr double kRingRadius = 40.5;
    static constexpr double kHandleRadius = 5.0;
    static constexpr double kPivotRadius = 4.0;
    static constexpr double kHandleGrabRadius = 12.0;
    static constexpr double kRingGrabTolerance = 10.0;
    static constexpr double kPivotGrabRadius = 10.0;
    static constexpr double kAngleSnapDegrees = 15.0;  // Shift on the ring
    static constexpr double kRoundnessSnapStep = 0.10; // Shift on a handle
    static constexpr double kRoundnessFloor = 0.01;

    enum class Region { None, Ring, Handle, Pivot };
    Region hitTest(const QPointF &pos) const; // exposed for the seam

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

} // namespace brushlib
