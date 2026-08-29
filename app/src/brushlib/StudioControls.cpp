#include "SankoTheme.h"
#include "StudioControls.h"

#include "StrokeBuilder.h" // StudioTipShapePreview renders THROUGH the engine

#include <QFontMetrics>
#include <QHideEvent>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLineF>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace brushlib {
namespace studio {

QFont labelFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(14);
    f.setWeight(QFont::Medium);
    return f;
}

QFont capsuleFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(12);
    f.setWeight(QFont::Medium);
    return f;
}

void paintCapsule(QPainter &p, const QRect &r, const QString &text)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kCapsuleBg);
    p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
    p.setFont(capsuleFont());
    p.setPen(kTextDim);
    p.drawText(r, Qt::AlignCenter, text);
}

void paintCurveChip(QPainter &p, const QRect &r, const PressureCurve &curve,
                    bool expanded)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(expanded ? QPen(kAccent, 1) : QPen(Qt::NoPen));
    p.setBrush(kCapsuleBg);
    p.drawRoundedRect(r, r.height() / 2.0, r.height() / 2.0);
    const QRectF g = QRectF(r).adjusted(10, 5, -10, -5);
    QPainterPath path;
    const auto &pts = curve.controlPoints();
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF w(g.left() + pts.at(i).x() * g.width(),
                        g.bottom() - pts.at(i).y() * g.height());
        if (i == 0)
            path.moveTo(w);
        else
            path.lineTo(w);
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kAccent, 1.5));
    p.drawPath(path);
}

} // namespace studio

using namespace studio;

namespace {
constexpr int kLabelRowH = 23; // label + capsule line (Figma 274:73)
constexpr int kRowGap = 12;    // between the label line and the track
constexpr int kTrackH = 6;
constexpr int kDraggerW = 20;
constexpr int kChipW = 55;
constexpr int kChipH = 23;

QString defaultFormat(double v)
{
    return QString::number(v, 'f', 2);
}
} // namespace

// ---------------------------------------------------------------- slider --

StudioSlider::StudioSlider(const QString &label, double min, double max,
                           QWidget *parent)
    : QWidget(parent)
    , m_label(label)
    , m_min(min)
    , m_max(max)
    , m_value(min)
    , m_format(defaultFormat)
{
    setFixedHeight(kLabelRowH + kRowGap + kTrackH);
    setCursor(Qt::PointingHandCursor);
}

void StudioSlider::setValue(double value)
{
    m_value = std::clamp(value, m_min, m_max);
    update();
}

void StudioSlider::setFormatter(std::function<QString(double)> f)
{
    m_format = std::move(f);
    update();
}

void StudioSlider::enableCurveChip()
{
    m_hasChip = true;
    update();
}

void StudioSlider::setChipCurve(const PressureCurve &curve)
{
    m_chipCurve = curve;
    update();
}

void StudioSlider::setChipExpanded(bool expanded)
{
    m_chipExpanded = expanded;
    update();
}

void StudioSlider::setSourceCapsule(const QString &text)
{
    if (m_sourceText == text)
        return;
    m_sourceText = text;
    update();
}

void StudioSlider::setChipDimmed(bool dimmed)
{
    if (m_chipDimmed == dimmed)
        return;
    m_chipDimmed = dimmed;
    update();
}

QRect StudioSlider::trackRect() const
{
    return QRect(0, kLabelRowH + kRowGap, width(), kTrackH);
}

QRect StudioSlider::capsuleRect() const
{
    const QFontMetrics fm(capsuleFont());
    const int w = qMax(kChipW, fm.horizontalAdvance(m_format(m_value)) + 24);
    return QRect(width() - w, 0, w, kChipH);
}

QRect StudioSlider::chipRect() const
{
    if (!m_hasChip)
        return QRect();
    return QRect(capsuleRect().left() - 8 - kChipW, 0, kChipW, kChipH);
}

double StudioSlider::valueAtX(int x) const
{
    const QRect t = trackRect();
    const double span = qMax(1, t.width());
    double u = std::clamp(double(x - t.left()) / span, 0.0, 1.0);
    if (m_exponent != 1.0)
        u = std::pow(u, m_exponent);
    double v = m_min + (m_max - m_min) * u;
    if (m_step > 0.0)
        v = m_min + qRound((v - m_min) / m_step) * m_step;
    return std::clamp(v, m_min, m_max);
}

void StudioSlider::applyDragValue(int x)
{
    const double v = valueAtX(x);
    if (v == m_value)
        return;
    m_value = v;
    update();
    emit valueChanged(m_value);
}

void StudioSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const bool on = isEnabled();
    p.setOpacity(on ? 1.0 : 0.4);

    // Label line.
    p.setFont(labelFont());
    p.setPen(kTextDim);
    p.drawText(QRect(0, 0, width(), kLabelRowH),
               Qt::AlignLeft | Qt::AlignVCenter, m_label);
    paintCapsule(p, capsuleRect(), m_format(m_value));
    if (m_hasChip) {
        // Source = None makes the curve (and the drawer's minimum) inert:
        // the chip cluster dims, but stays clickable so the drawer remains
        // reachable to change the source back.
        if (m_chipDimmed)
            p.setOpacity(on ? 0.4 : 0.25);
        paintCurveChip(p, chipRect(), m_chipCurve, m_chipExpanded);
        if (!m_sourceText.isEmpty()) {
            const QFontMetrics fm(capsuleFont());
            const int w = qMax(44, fm.horizontalAdvance(m_sourceText) + 16);
            paintCapsule(p, QRect(chipRect().left() - 6 - w, 0, w, kChipH),
                         m_sourceText);
        }
        p.setOpacity(on ? 1.0 : 0.4);
    }

    // Track.
    const QRect t = trackRect();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(t, 1, 1);

    // Fill: #4b4397 (left) -> #7c6ef6 (right end of the fill), then the
    // white 10% -> 0 vertical sheen — both from Figma component 273:31.
    double span = (m_max > m_min) ? (m_value - m_min) / (m_max - m_min) : 0.0;
    if (m_exponent != 1.0)
        span = std::pow(span, 1.0 / m_exponent);
    const int fillW = qRound(span * t.width());
    if (fillW > 0) {
        const QRect f(t.left(), t.top(), fillW, t.height());
        QLinearGradient grad(f.topLeft(), f.topRight());
        grad.setColorAt(0.0, kAccentDark);
        grad.setColorAt(1.0, kAccent);
        p.setBrush(grad);
        p.drawRoundedRect(f, 1, 1);
        QLinearGradient sheen(f.topLeft(), f.bottomLeft());
        sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
        sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setBrush(sheen);
        p.drawRoundedRect(f, 1, 1);
    }

    // Dragger: 20x6 flush with the fill's right end.
    const int dx = std::clamp(fillW - kDraggerW, 0, qMax(0, t.width() - kDraggerW));
    p.setBrush(kDragger);
    p.drawRoundedRect(QRect(t.left() + dx, t.top(), kDraggerW, t.height()), 1, 1);
}

void StudioSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_hasChip && chipRect().contains(event->pos())) {
        emit chipClicked();
        return;
    }
    if (event->pos().y() < kLabelRowH && !trackRect().contains(event->pos()))
        return; // the label line is not a drag target (capsule/chip only)
    m_dragging = true;
    m_pressValue = m_value;
    applyDragValue(event->pos().x());
}

void StudioSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging)
        applyDragValue(event->pos().x());
}

void StudioSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_dragging)
        return;
    m_dragging = false;
    if (m_pressValue != m_value)
        emit valueCommitted(m_pressValue, m_value);
}

// ------------------------------------------------------------- curve row --

StudioCurveRow::StudioCurveRow(const QString &label, QWidget *parent)
    : QWidget(parent)
    , m_label(label)
{
    setFixedHeight(kChipH);
    setCursor(Qt::PointingHandCursor);
}

void StudioCurveRow::setCurve(const PressureCurve &curve)
{
    m_curve = curve;
    update();
}

void StudioCurveRow::setExpanded(bool expanded)
{
    m_expanded = expanded;
    update();
}

void StudioCurveRow::setSourceCapsule(const QString &text)
{
    if (m_sourceText == text)
        return;
    m_sourceText = text;
    update();
}

void StudioCurveRow::setChipDimmed(bool dimmed)
{
    if (m_chipDimmed == dimmed)
        return;
    m_chipDimmed = dimmed;
    update();
}

void StudioCurveRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setFont(labelFont());
    p.setPen(kTextDim);
    p.drawText(QRect(0, 0, width(), kChipH),
               Qt::AlignLeft | Qt::AlignVCenter, m_label);
    if (m_chipDimmed)
        p.setOpacity(0.4); // None: inert curve, still clickable (see slider)
    paintCurveChip(p, QRect(width() - kChipW, 0, kChipW, kChipH), m_curve,
                   m_expanded);
    if (!m_sourceText.isEmpty()) {
        const QFontMetrics fm(capsuleFont());
        const int w = qMax(44, fm.horizontalAdvance(m_sourceText) + 16);
        paintCapsule(p, QRect(width() - kChipW - 6 - w, 0, w, kChipH),
                     m_sourceText);
    }
}

void StudioCurveRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit clicked();
}

// ---------------------------------------------------------- curve editor --

StudioCurveEditor::StudioCurveEditor(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(kHeight);
    setMouseTracking(true);
}

void StudioCurveEditor::setCurve(const PressureCurve &curve)
{
    m_curve = curve;
    m_dragIndex = -1;
    m_hoverIndex = -1;
    update();
}

QRect StudioCurveEditor::graphRect() const
{
    return rect().adjusted(14, 14, -14, -14);
}

QPointF StudioCurveEditor::toGraph(const QPointF &curvePt) const
{
    const QRectF g = graphRect();
    return QPointF(g.left() + curvePt.x() * g.width(),
                   g.bottom() - curvePt.y() * g.height());
}

QPointF StudioCurveEditor::fromGraph(const QPointF &widgetPt) const
{
    const QRectF g = graphRect();
    return QPointF(std::clamp((widgetPt.x() - g.left()) / qMax(1.0, g.width()), 0.0, 1.0),
                   std::clamp((g.bottom() - widgetPt.y()) / qMax(1.0, g.height()), 0.0, 1.0));
}

int StudioCurveEditor::pointAt(const QPointF &widgetPt) const
{
    const auto &pts = m_curve.controlPoints();
    int best = -1;
    double bestD = 12.0 * 12.0;
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF w = toGraph(pts.at(i));
        const double d = QPointF::dotProduct(w - widgetPt, w - widgetPt);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void StudioCurveEditor::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kCapsuleBg);
    p.drawRoundedRect(rect(), 6, 6);

    const QRectF g = graphRect();
    p.setPen(QPen(kBorder, 1));
    for (int i = 0; i <= 4; ++i) {
        const qreal x = g.left() + g.width() * i / 4.0;
        const qreal y = g.top() + g.height() * i / 4.0;
        p.drawLine(QPointF(x, g.top()), QPointF(x, g.bottom()));
        p.drawLine(QPointF(g.left(), y), QPointF(g.right(), y));
    }

    const auto &pts = m_curve.controlPoints();
    QPainterPath path;
    for (int i = 0; i < pts.size(); ++i) {
        const QPointF w = toGraph(pts.at(i));
        if (i == 0)
            path.moveTo(w);
        else
            path.lineTo(w);
    }
    p.setPen(QPen(kAccent, 2));
    p.drawPath(path);

    for (int i = 0; i < pts.size(); ++i) {
        const QPointF w = toGraph(pts.at(i));
        const bool hot = (i == m_dragIndex) || (i == m_hoverIndex);
        p.setPen(Qt::NoPen);
        p.setBrush(hot ? QColor(Qt::white) : kDragger);
        p.drawRoundedRect(QRectF(w.x() - 5, w.y() - 5, 10, 10), 2, 2);
    }
}

void StudioCurveEditor::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragIndex = pointAt(event->position());
    if (m_dragIndex < 0 && graphRect().contains(event->position().toPoint())) {
        // Click on empty graph space: add a control point there.
        const QPointF c = fromGraph(event->position());
        QVector<QPointF> pts = m_curve.controlPoints();
        int insert = pts.size();
        for (int i = 0; i < pts.size(); ++i)
            if (pts.at(i).x() > c.x()) {
                insert = i;
                break;
            }
        pts.insert(insert, c);
        m_curve.setControlPoints(pts);
        m_dragIndex = insert;
        update();
        emit curveChanged(m_curve);
    }
}

void StudioCurveEditor::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragIndex < 0) {
        const int hover = pointAt(event->position());
        if (hover != m_hoverIndex) {
            m_hoverIndex = hover;
            update();
        }
        return;
    }
    QVector<QPointF> pts = m_curve.controlPoints();
    if (m_dragIndex >= pts.size())
        return;
    QPointF c = fromGraph(event->position());
    // Endpoints keep their x; interior points stay strictly between their
    // neighbours so the sort order (and this drag's index) cannot change.
    if (m_dragIndex == 0)
        c.setX(0.0);
    else if (m_dragIndex == pts.size() - 1)
        c.setX(1.0);
    else
        c.setX(std::clamp(c.x(), pts.at(m_dragIndex - 1).x() + 0.005,
                          pts.at(m_dragIndex + 1).x() - 0.005));
    pts[m_dragIndex] = c;
    m_curve.setControlPoints(pts);
    update();
    emit curveChanged(m_curve);
}

void StudioCurveEditor::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_dragIndex < 0)
        return;
    m_dragIndex = -1;
    emit curveCommitted();
}

void StudioCurveEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int idx = pointAt(event->position());
    QVector<QPointF> pts = m_curve.controlPoints();
    if (idx <= 0 || idx >= pts.size() - 1)
        return; // endpoints are permanent
    pts.removeAt(idx);
    m_curve.setControlPoints(pts);
    m_dragIndex = -1;
    m_hoverIndex = -1;
    update();
    emit curveChanged(m_curve);
    emit curveCommitted();
}

// -------------------------------------------------------------- toggle ----

StudioToggleRow::StudioToggleRow(const QString &label, QWidget *parent)
    : QWidget(parent)
    , m_label(label)
{
    setFixedHeight(kChipH);
    setCursor(Qt::PointingHandCursor);
}

void StudioToggleRow::setChecked(bool on)
{
    m_on = on;
    update();
}

QRect StudioToggleRow::switchRect() const
{
    return QRect(width() - 36, (height() - 20) / 2, 36, 20);
}

void StudioToggleRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setOpacity(isEnabled() ? 1.0 : 0.4);
    p.setFont(labelFont());
    p.setPen(kTextDim);
    p.drawText(QRect(0, 0, width(), height()),
               Qt::AlignLeft | Qt::AlignVCenter, m_label);

    const QRect s = switchRect();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(m_on ? kAccent : kTrack);
    p.drawRoundedRect(s, 10, 10);
    const int kx = m_on ? s.right() - 17 : s.left() + 2;
    p.setBrush(m_on ? QColor(Qt::white) : kDragger);
    p.drawEllipse(QRect(kx, s.top() + 2, 16, 16));
}

void StudioToggleRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !isEnabled())
        return;
    m_on = !m_on;
    update();
    emit toggled(m_on);
}

// -------------------------------------------------------------- choice ----

StudioChoiceRow::StudioChoiceRow(const QString &label,
                                 const QStringList &options, QWidget *parent)
    : QWidget(parent)
    , m_label(label)
    , m_options(options)
{
    setFixedHeight(kChipH);
    setCursor(Qt::PointingHandCursor);
}

void StudioChoiceRow::choose(int index)
{
    if (index < 0 || index >= m_options.size())
        return;
    m_index = index;
    update();
    emit chosen(index);
}

void StudioChoiceRow::setCurrentIndex(int index)
{
    m_index = std::clamp(index, 0, int(m_options.size()) - 1);
    update();
}

QRect StudioChoiceRow::capsuleRect() const
{
    const QFontMetrics fm(capsuleFont());
    const QString text = m_options.value(m_index);
    const int w = fm.horizontalAdvance(text) + 24 + 12; // pill + chevron
    return QRect(width() - w, 0, w, kChipH);
}

void StudioChoiceRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setOpacity(isEnabled() ? 1.0 : 0.4);
    p.setFont(labelFont());
    p.setPen(kTextDim);
    p.drawText(QRect(0, 0, width(), height()),
               Qt::AlignLeft | Qt::AlignVCenter, m_label);

    const QRect c = capsuleRect();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kCapsuleBg);
    p.drawRoundedRect(c, c.height() / 2.0, c.height() / 2.0);
    p.setFont(capsuleFont());
    p.setPen(kTextDim);
    p.drawText(c.adjusted(12, 0, -18, 0), Qt::AlignLeft | Qt::AlignVCenter,
               m_options.value(m_index));
    // Chevron.
    const QPointF a(c.right() - 14, c.center().y() - 1.5);
    QPainterPath ch;
    ch.moveTo(a);
    ch.lineTo(a + QPointF(4, 4));
    ch.lineTo(a + QPointF(8, 0));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kTextDim, 1.5));
    p.drawPath(ch);
}

void StudioChoiceRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !isEnabled())
        return;
    QMenu menu(this);
    for (int i = 0; i < m_options.size(); ++i) {
        QAction *a = menu.addAction(m_options.at(i));
        a->setCheckable(true);
        a->setChecked(i == m_index);
        a->setData(i);
    }
    const QRect c = capsuleRect();
    if (QAction *picked = menu.exec(mapToGlobal(c.bottomLeft() + QPoint(0, 4)))) {
        const int idx = picked->data().toInt();
        if (idx != m_index) {
            setCurrentIndex(idx);
            emit chosen(idx);
        }
    }
}

// ----------------------------------------------------------- segmented ----

StudioSegmentedRow::StudioSegmentedRow(const QString &label,
                                       const QStringList &options,
                                       QWidget *parent)
    : QWidget(parent)
    , m_label(label)
    , m_options(options)
{
    setFixedHeight(26);
    setCursor(Qt::PointingHandCursor);
}

void StudioSegmentedRow::choose(int index)
{
    if (index < 0 || index >= m_options.size())
        return;
    m_index = index;
    update();
    emit chosen(index);
}

void StudioSegmentedRow::setCurrentIndex(int index)
{
    // -1 = no selection (the grain preset row's Custom state); the paint
    // loop's equality test simply never lights a segment for it.
    m_index = index < 0 ? -1
                        : std::clamp(index, 0, int(m_options.size()) - 1);
    update();
}

QRect StudioSegmentedRow::segmentsRect() const
{
    const QFontMetrics fm(capsuleFont());
    int w = 0;
    for (const QString &o : m_options)
        w += fm.horizontalAdvance(o) + 20;
    return QRect(width() - w, 0, w, height());
}

void StudioSegmentedRow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setOpacity(isEnabled() ? 1.0 : 0.4);
    p.setFont(labelFont());
    p.setPen(kTextDim);
    p.drawText(QRect(0, 0, width(), height()),
               Qt::AlignLeft | Qt::AlignVCenter, m_label);

    const QRect s = segmentsRect();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kCapsuleBg);
    p.drawRoundedRect(s, 6, 6);

    const QFontMetrics fm(capsuleFont());
    int x = s.left();
    for (int i = 0; i < m_options.size(); ++i) {
        const int w = fm.horizontalAdvance(m_options.at(i)) + 20;
        const QRect seg(x, s.top(), w, s.height());
        if (i == m_index) {
            p.setPen(Qt::NoPen);
            p.setBrush(kAccent);
            p.drawRoundedRect(seg, 6, 6);
        }
        QFont f = capsuleFont();
        f.setWeight(i == m_index ? QFont::DemiBold : QFont::Medium);
        p.setFont(f);
        p.setPen(i == m_index ? QColor(Qt::white) : kTextDim);
        p.drawText(seg, Qt::AlignCenter, m_options.at(i));
        x += w;
    }
}

void StudioSegmentedRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !isEnabled())
        return;
    const QRect s = segmentsRect();
    if (!s.contains(event->pos()))
        return;
    const QFontMetrics fm(capsuleFont());
    int x = s.left();
    for (int i = 0; i < m_options.size(); ++i) {
        const int w = fm.horizontalAdvance(m_options.at(i)) + 20;
        if (event->pos().x() < x + w) {
            if (i != m_index) {
                setCurrentIndex(i);
                emit chosen(i);
            }
            return;
        }
        x += w;
    }
}

// ------------------------------------------------------- StudioTipRing ---

namespace {
// Figma 345:99 / revised 341:30 tokens, verbatim (kAccent IS the ring's
// #7C6EF6, kCapsuleBg/kTextDim/capsuleFont ARE the capsule's).
const QColor kRingHandle(0xd9, 0xd9, 0xd9);
const QColor kRingPivot(0xcc, 0xcc, 0xcc);
const QColor kPanelBg(0x3c, 0x3c, 0x42);
const QColor kPanelOutline(0x59, 0x59, 0x64);  // inset rect + guide lines
const QColor kGuideCircle(0x55, 0x55, 0x60);
// Added hover/active states (documented divergence): the palette's
// brighten language.
const QColor kRingHover(0x8f, 0x83, 0xf8);
const QColor kRingElementHover(0xff, 0xff, 0xff);
constexpr double kRingStrokeWidth = 2.0;
constexpr double kPivotStrokeWidth = 1.0;
constexpr double kPanelRadius = 8.0;
constexpr int kPanelInset = 8;        // outline rect offset each side
constexpr int kCapsuleX = 16;         // capsule top-left in the panel
constexpr int kCapsuleY = 17;
constexpr int kCapsuleH = 23;
constexpr int kCapsulePadding = 12;   // horizontal, each side
} // namespace

StudioTipRing::StudioTipRing(QWidget *parent)
    : QWidget(parent)
{
    // The Figma panel at the props column's content width (divergence (1)
    // in the header: 320 for the design's 356, internal tokens verbatim).
    setFixedSize(kPanelWidth, kPanelHeight);
    setMouseTracking(true); // hover states + cursor feedback
}

QPointF StudioTipRing::ringCentre() const
{
    // The panel's TRUE centre — divergence (2): the design drifts by
    // half-pixels (ring at 178.5 in a 356 frame, crosshair at 179/105,
    // guide circle at 104.5); every concentric element draws from here.
    return QPointF(kPanelWidth / 2.0, kPanelHeight / 2.0);
}

QString StudioTipRing::capsuleText() const
{
    // The approved scheme: the live value during a gesture; at rest,
    // "None" while both values are default, otherwise only what differs.
    const QString angle = QStringLiteral("%1°").arg(qRound(m_angle));
    const QString round =
        QStringLiteral("%1%").arg(qRound(m_roundness * 100.0));
    if (m_drag == Region::Ring)
        return angle;
    if (m_drag == Region::Handle)
        return round;
    const bool angleDefault = m_angle == 0.0;
    const bool roundDefault = m_roundness == 1.0;
    if (angleDefault && roundDefault)
        return QStringLiteral("None");
    if (roundDefault)
        return angle;
    if (angleDefault)
        return round;
    return angle + QStringLiteral(" · ") + round;
}

QRect StudioTipRing::capsuleRect() const
{
    // Fixed position, width from the text — the design's 55 x 23 "None"
    // pill is exactly this formula (31 px of Inter Medium 12 + 2 * 12).
    const int w = QFontMetrics(capsuleFont()).horizontalAdvance(capsuleText())
        + 2 * kCapsulePadding;
    return QRect(kCapsuleX, kCapsuleY, w, kCapsuleH);
}

QPointF StudioTipRing::handleCentre(int index) const
{
    // The tip's forward affine, matching the thumbnail's backward map
    // inverted: tip-local +X (the axis roundness squashes) maps to the
    // screen direction (cos a, sin a) with y down, scaled by roundness;
    // tip-local +Y is unsquashed. Index 0 E, 1 N, 2 W, 3 S of the
    // UNROTATED design; the whole constellation rotates with the angle.
    const double a = qDegreesToRadians(m_angle);
    const QPointF squash(std::cos(a), std::sin(a));
    const QPointF keep(-std::sin(a), std::cos(a));
    const double squashDistance =
        kRingRadius * std::max(m_roundness, kRoundnessFloor);
    switch (index & 3) {
    case 0: return ringCentre() + squash * squashDistance;
    case 1: return ringCentre() - keep * kRingRadius;
    case 2: return ringCentre() - squash * squashDistance;
    default: return ringCentre() + keep * kRingRadius;
    }
}

void StudioTipRing::setTipValues(double angleDegrees, double roundness)
{
    const double a = wrapAngle(angleDegrees);
    const double r = std::clamp(roundness, kRoundnessFloor, 1.0);
    if (a == m_angle && r == m_roundness)
        return;
    m_angle = a;
    m_roundness = r;
    update();
}

double StudioTipRing::wrapAngle(double degrees)
{
    double a = std::fmod(degrees + 180.0, 360.0);
    if (a <= 0.0)
        a += 360.0;
    return a - 180.0; // (-180, 180]
}

double StudioTipRing::pointerAngle(const QPointF &pos) const
{
    const QPointF d = pos - ringCentre();
    return qRadiansToDegrees(std::atan2(d.y(), d.x())); // y down
}

double StudioTipRing::squashGrabRadius() const
{
    // The squash handles shrink into a tighter and tighter neighbourhood
    // as roundness falls, so their grab radius grows to compensate: 12 px
    // while they sit on the rim, ramping to 20 px by the time they reach
    // the band's inner edge (roundness 34.5/44.5 ~ 0.775). Below that the
    // interior rule covers the whole disc anyway. The ramp stops short of
    // the pivot by construction — at the inner edge the reach is
    // 34.5 - 20 = 14.5 px, well outside the 10 px pivot — so the two
    // never contend.
    const double distance =
        kRingRadius * std::max(m_roundness, kRoundnessFloor);
    const double t = std::clamp(
        (kRingRadius - distance) / (kRingRadius - kRingInner), 0.0, 1.0);
    return kHandleGrabRadius
        + t * (kHandleGrabRadiusMax - kHandleGrabRadius);
}

QPointF StudioTipRing::ringSemiAxes() const
{
    return QPointF(kRingRadius * std::max(m_roundness, kRoundnessFloor),
                   kRingRadius);
}

QPointF StudioTipRing::tipSpace(const QPointF &pos) const
{
    // StrokeBuilder::shapedTipForStamp / stamp.frag, verbatim: rotate the
    // point into tip-local space, then divide the SQUASHED axis. Scaled to
    // pixels, so the tip's edge sits at hypot() == kRingRadius.
    const double a = qDegreesToRadians(m_angle);
    const QPointF v = pos - ringCentre();
    const double squash = std::max(m_roundness, kRoundnessFloor);
    return QPointF(
        (std::cos(a) * v.x() + std::sin(a) * v.y()) / squash,
        -std::sin(a) * v.x() + std::cos(a) * v.y());
}

double StudioTipRing::signedRingDistance(const QPointF &pos) const
{
    // Implicit form f = |tipSpace| - kRingRadius, converted from tip units
    // to PIXELS by dividing through the gradient magnitude. Exact on both
    // axes (the only places the ellipse's curvature vanishes) and a close
    // approximation between them — which is all a 10 px grab band needs.
    // Without this the band would be measured in squashed units and would
    // collapse to nothing along the narrow axis.
    const QPointF t = tipSpace(pos);
    const double m = std::hypot(t.x(), t.y());
    if (m <= 1e-9)
        return -kRingRadius; // the centre is inside by the full radius
    const double squash = std::max(m_roundness, kRoundnessFloor);
    const double gradient = std::hypot(t.x() / squash, t.y()) / m;
    if (gradient <= 1e-9)
        return -kRingRadius;
    return (m - kRingRadius) / gradient;
}

StudioTipRing::Region StudioTipRing::hitTest(const QPointF &pos) const
{
    // The partition documented in the header, in priority order. Each test
    // claims its points outright and the last one catches everything still
    // inside the control, so no press falls through and none is claimed
    // twice — at any roundness, including a fully collapsed ring.
    const double fromCentre = QLineF(ringCentre(), pos).length();
    if (fromCentre <= kPivotGrabRadius)
        return Region::Pivot;
    for (int i = 0; i < 4; ++i) {
        const double grab =
            (i % 2 == 0) ? squashGrabRadius() : kHandleGrabRadius;
        if (QLineF(handleCentre(i), pos).length() <= grab)
            return Region::Handle;
    }
    if (std::abs(signedRingDistance(pos)) <= kRingGrabTolerance)
        return Region::Ring;
    if (fromCentre <= kRingRadius + kRingGrabTolerance)
        return Region::Handle; // the interior: nearer squash handle
    return Region::None;
}

int StudioTipRing::handleForPress(const QPointF &pos) const
{
    if (hitTest(pos) != Region::Handle)
        return -1;
    // A press inside a handle's own grab radius takes that handle; the
    // nearest wins where two overlap, which they do once roundness has
    // pulled the squash pair close together.
    {
        int best = -1;
        double bestDistance = 0.0;
        for (int i = 0; i < 4; ++i) {
            const double grab =
                (i % 2 == 0) ? squashGrabRadius() : kHandleGrabRadius;
            const double distance = QLineF(handleCentre(i), pos).length();
            if (distance <= grab && (best < 0 || distance < bestDistance)) {
                best = i;
                bestDistance = distance;
            }
        }
        if (best >= 0)
            return best;
    }
    {
        // Interior: the NEARER squash handle, decided by which side of the
        // unsquashed axis the press falls on. On the axis itself the two
        // are exactly equidistant; that tie goes to index 0 (+squash),
        // which keeps the choice deterministic and stateless. Either way
        // both handles drive the same roundness, so the tie-break decides
        // presentation, never the value.
        const double a = qDegreesToRadians(m_angle);
        const QPointF squash(std::cos(a), std::sin(a));
        const QPointF v = pos - ringCentre();
        const double projection = v.x() * squash.x() + v.y() * squash.y();
        return projection >= 0.0 ? 0 : 2;
    }
}

void StudioTipRing::updateCursorFor(Region region)
{
    switch (region) {
    case Region::Ring:
        setCursor(m_drag == Region::Ring ? Qt::ClosedHandCursor
                                         : Qt::OpenHandCursor);
        break;
    case Region::Handle:
    case Region::Pivot:
        setCursor(Qt::PointingHandCursor);
        break;
    case Region::None:
        unsetCursor();
        break;
    }
}

void StudioTipRing::applyRingDrag(const QPointF &pos,
                                  Qt::KeyboardModifiers modifiers)
{
    double a = wrapAngle(pointerAngle(pos) + m_grabOffset);
    if (modifiers & Qt::ShiftModifier)
        a = wrapAngle(std::round(a / kAngleSnapDegrees) * kAngleSnapDegrees);
    if (a != m_angle) {
        m_angle = a;
        update();
        emit angleEdited(m_angle);
    }
}

void StudioTipRing::applyHandleDrag(const QPointF &pos,
                                    Qt::KeyboardModifiers modifiers)
{
    // All four handles drive the ONE roundness: the value is the pointer's
    // distance from the centre as a fraction of the ring radius. Photoshop
    // floor 1% — 0 divides by zero in the backward-mapped sampler.
    double r = QLineF(ringCentre(), pos).length() / kRingRadius;
    if (modifiers & Qt::ShiftModifier)
        r = std::round(r / kRoundnessSnapStep) * kRoundnessSnapStep;
    r = std::clamp(r, kRoundnessFloor, 1.0);
    if (r != m_roundness) {
        m_roundness = r;
        update();
        emit roundnessEdited(m_roundness);
    }
}

void StudioTipRing::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_drag = hitTest(event->position());
    m_moved = false;
    if (m_drag == Region::Ring)
        m_grabOffset = m_angle - pointerAngle(event->position());
    updateCursorFor(m_drag);
    update();
    event->accept();
}

void StudioTipRing::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drag == Region::None) {
        const Region hover = hitTest(event->position());
        if (hover != m_hover) {
            m_hover = hover;
            updateCursorFor(hover);
            update();
        }
        return;
    }
    // Qt's implicit mouse grab keeps delivering moves after the pointer
    // leaves the widget; no clamping — the angle is a direction and the
    // roundness projection clamps itself.
    m_moved = true;
    if (m_drag == Region::Ring)
        applyRingDrag(event->position(), event->modifiers());
    else if (m_drag == Region::Handle)
        applyHandleDrag(event->position(), event->modifiers());
    event->accept();
}

void StudioTipRing::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || m_drag == Region::None) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const Region was = m_drag;
    m_drag = Region::None;
    if (was == Region::Pivot && !m_moved
        && hitTest(event->position()) == Region::Pivot) {
        emit resetRequested();
    } else if (was != Region::Pivot) {
        // A click without drag emitted nothing (the grab offset preserves
        // the value), so this commit closes an empty gesture harmlessly.
        emit editCommitted();
    }
    m_hover = hitTest(event->position());
    updateCursorFor(m_hover);
    update();
    event->accept();
}

void StudioTipRing::leaveEvent(QEvent *event)
{
    if (m_drag == Region::None && m_hover != Region::None) {
        m_hover = Region::None;
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

void StudioTipRing::hideEvent(QHideEvent *event)
{
    // Safety: a hide mid-drag (section switch, window close) must not
    // leave a stuck grab state.
    if (m_drag != Region::None) {
        m_drag = Region::None;
        emit editCommitted();
    }
    m_hover = Region::None;
    QWidget::hideEvent(event);
}

void StudioTipRing::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool ringLive =
        m_drag == Region::Ring
        || (m_drag == Region::None && m_hover == Region::Ring);
    const bool handleLive =
        m_drag == Region::Handle
        || (m_drag == Region::None && m_hover == Region::Handle);
    const bool pivotLive =
        m_drag == Region::Pivot
        || (m_drag == Region::None && m_hover == Region::Pivot);

    const QPointF c = ringCentre();

    // --- Panel chrome (all Region::None) --------------------------------
    // Background and inset outline; the 1 px outline pen is centred half a
    // pixel inside its box so the stroke stays inside like the design's.
    p.setPen(Qt::NoPen);
    p.setBrush(kPanelBg);
    p.drawRoundedRect(QRectF(0, 0, kPanelWidth, kPanelHeight),
                      kPanelRadius, kPanelRadius);
    p.setPen(QPen(kPanelOutline, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(kPanelInset + 0.5, kPanelInset + 0.5,
                             kPanelWidth - 2 * kPanelInset - 1,
                             kPanelHeight - 2 * kPanelInset - 1),
                      kPanelRadius - 0.5, kPanelRadius - 0.5);

    // Static guide circle: the unsquashed rim the tip returns to at full
    // roundness — chrome, not the tip; the purple ellipse is the tip.
    p.setPen(QPen(kGuideCircle, 1.0));
    p.drawEllipse(c, kGuideRadius, kGuideRadius);

    // Crosshair guide lines, outline box edge to guide circle edge. Drawn
    // half a pixel off the true centre so the 1 px stroke fills one crisp
    // pixel row/column instead of AA-blurring across two — the same side
    // the design's own inside-stroke rounding lands on (divergence (2)).
    p.setPen(QPen(kPanelOutline, 1.0));
    const double gy = c.y() + 0.5, gx = c.x() + 0.5;
    p.drawLine(QPointF(kPanelInset, gy),
               QPointF(c.x() - kGuideRadius, gy));
    p.drawLine(QPointF(c.x() + kGuideRadius, gy),
               QPointF(kPanelWidth - kPanelInset, gy));
    p.drawLine(QPointF(gx, kPanelInset),
               QPointF(gx, c.y() - kGuideRadius));
    p.drawLine(QPointF(gx, c.y() + kGuideRadius),
               QPointF(gx, kPanelHeight - kPanelInset));

    // The display-only value capsule — the studio's shared painting.
    paintCapsule(p, capsuleRect(), capsuleText());

    // --- The control itself ---------------------------------------------
    // Ring: the Figma stroke on the TIP's own ellipse. QTransform::rotate
    // maps local +X to (cos a, sin a) and local +Y to (-sin a, cos a) —
    // the engine's squash and unsquashed directions exactly — so drawing
    // an axis-aligned ellipse in the rotated frame reproduces the affine
    // without duplicating it. The geometry is NOT clamped to a legible
    // minimum: at roundness 0.01 the ellipse is 0.9 px across and the
    // control says so. The revised design's 2 px stroke only closes over
    // itself below roundness 2/(2*44.5) = 0.022, so the ellipse stays a
    // legible outline nearly to the floor — do not "fix" the stroke
    // weight; the token is 2 px and the legibility comes with it.
    p.save();
    p.translate(c);
    p.rotate(m_angle);
    p.setPen(QPen(ringLive ? kRingHover : kAccent, kRingStrokeWidth));
    p.setBrush(Qt::NoBrush);
    const QPointF semi = ringSemiAxes();
    p.drawEllipse(QPointF(0.0, 0.0), semi.x(), semi.y());
    p.restore();

    // Handles: two-tone — r 4 disc with an r 2 accent dot — positioned by
    // the current angle and roundness. The disc brightens on hover/drag;
    // the dot stays accent (it reads as the ring showing through).
    for (int i = 0; i < 4; ++i) {
        p.setPen(Qt::NoPen);
        p.setBrush(handleLive ? kRingElementHover : kRingHandle);
        p.drawEllipse(handleCentre(i), kHandleRadius, kHandleRadius);
        p.setBrush(kAccent);
        p.drawEllipse(handleCentre(i), kHandleDotRadius, kHandleDotRadius);
    }

    // Pivot: r 2.5 circle plus the four 4 px crosshair ticks, 1 px.
    const QColor pivotColor = pivotLive ? kRingElementHover : kRingPivot;
    p.setPen(QPen(pivotColor, kPivotStrokeWidth));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, kPivotRadius, kPivotRadius);
    for (const QPointF &d : {QPointF(1, 0), QPointF(-1, 0),
                             QPointF(0, 1), QPointF(0, -1)})
        p.drawLine(c + d * kPivotTickInner, c + d * kPivotTickOuter);
}

// ---------------------------------------------------------------------------
// StudioTipShapePreview
// ---------------------------------------------------------------------------

StudioTipShapePreview::StudioTipShapePreview(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kPanelWidth, kPanelHeight);
}

void StudioTipShapePreview::setBrush(const ::Brush &brush)
{
    // ONE neutral stamp through the engine's own per-stamp tip function.
    // The StrokeBuilder copies the brush, so the render can never touch the
    // session brush's tip cache, let alone the canvas brush's. Everything
    // the stamp leaves at defaults — tilt, barrel rotation, viewport
    // rotation, angle jitter — is a per-input dynamic; everything the tip
    // ACTUALLY IS (hardness falloff, custom image, static angle/roundness/
    // flips) is read from the brush inside shapedTipForStamp itself.
    const qreal dpr = devicePixelRatioF();
    const int px = qMax(1, qRound(kTipPx * dpr));
    ::Brush copy = brush;
    StrokeBuilder shaper(QSize(px, px), copy, /*rasterizePreview=*/false);
    StrokeStamp stamp;
    stamp.effectiveSize = px; // fixed preview resolution, NOT brush size
    stamp.effectiveHardness = copy.hardness();
    stamp.roundness = 1.0;    // jitter-neutral; the static roundness is
                              // the brush's and composes inside the engine
    QImage tip = shaper.shapedTipForStamp(stamp);
    if (tip == m_tip)
        return; // byte-identical render: the edit was not a tip parameter
    m_tip = tip;

    // Colorize coverage as the component ink — depiction only; the engine
    // bytes above are what the seam reads.
    QImage inked(m_tip.size(), QImage::Format_ARGB32_Premultiplied);
    const QColor ink = kDragger;
    for (int y = 0; y < m_tip.height(); ++y) {
        const uchar *src = m_tip.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(inked.scanLine(y));
        for (int x = 0; x < m_tip.width(); ++x)
            dst[x] = qPremultiply(
                qRgba(ink.red(), ink.green(), ink.blue(), src[x]));
    }
    inked.setDevicePixelRatio(dpr);
    m_display = QPixmap::fromImage(inked);
    update();
}

void StudioTipShapePreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Panel chrome (358:23 background, 358:24 inset outline well) — the
    // TipRing's exact tokens; the two panels are siblings in the design.
    p.setPen(Qt::NoPen);
    p.setBrush(kPanelBg);
    p.drawRoundedRect(QRectF(0, 0, kPanelWidth, kPanelHeight),
                      kCornerRadius, kCornerRadius);
    p.setPen(QPen(kPanelOutline, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(kWellInset + 0.5, kWellInset + 0.5,
                             kPanelWidth - 2 * kWellInset - 1,
                             kPanelHeight - 2 * kWellInset - 1),
                      kCornerRadius - 0.5, kCornerRadius - 0.5);
    // The tip, centered (requirement 2). The render is square with the
    // affine baked in, so drawing it 1:1 preserves the real aspect ratio.
    if (!m_display.isNull()) {
        const QSizeF s =
            QSizeF(m_display.size()) / m_display.devicePixelRatio();
        p.drawPixmap(QPointF((kPanelWidth - s.width()) / 2.0,
                             (kPanelHeight - s.height()) / 2.0),
                     m_display);
    }
}

// ---------------------------------------------------------------------------
// StudioGrainPreview
// ---------------------------------------------------------------------------

StudioGrainPreview::StudioGrainPreview(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kPanelWidth, kPanelHeight);
}

void StudioGrainPreview::setBrush(const ::Brush &brush)
{
    // ONE neutral stamp through the real stroke path. The render copy
    // keeps every GRAIN field verbatim (preset or custom texture, scale,
    // depth, contrast, rotation, motion mode, blend mode) and neutralizes
    // the TIP half — the erase-swatch pattern: the panels stay
    // orthogonal, so each Texture control visibly moves exactly this well
    // and each Tip control exactly the one below. Noise and wet edges are
    // zeroed too: noise is the Tip's falloff roughening and wet edges
    // would rim the patch with something that is not grain.
    //
    // The disc is SOFT (hardness 0.6), not hard, and that is load-bearing:
    // at full coverage most texture blend modes collapse to the same
    // value (Multiply, Subtract and Darken are identical at tip = 1), so
    // a hardness-1 patch is blend-mode-BLIND — the (p) gate proved it on
    // first run. The soft falloff band gives the nine modes coverage
    // values they actually differ over, while the solid core still shows
    // depth and contrast plainly.
    ::Brush copy = brush;
    copy.clearCustomShape();
    copy.setHardness(0.6);
    copy.setSize(kPatchPx);
    copy.setTipAngle(0.0);
    copy.setTipRoundness(1.0);
    copy.setTipFlipX(false);
    copy.setTipFlipY(false);
    copy.setSizeJitter(0.0);
    copy.setAngleJitter(0.0);
    copy.setRoundnessJitter(0.0);
    copy.setSpacingJitter(0.0);
    copy.setScatterAlong(0.0);
    copy.setScatterPerpendicular(0.0);
    copy.setScatterCount(1);
    copy.setNoise(0.0);
    copy.setWetEdges(0.0);
    copy.setOpacity(1.0);
    copy.setFlow(1.0);
    copy.setDualBrushEnabled(false);

    // rasterizePreview must be TRUE: false makes placeStamp record only
    // the affected rect and rasterize nothing (BrushPreviewRenderer's
    // mode — it rasterizes from the stamp list via the host adapter),
    // which leaves strokeMask() empty. Caught by the (p) gate's flatness
    // control on first run.
    StrokeBuilder sb(QSize(kPatchPx, kPatchPx), copy,
                     /*rasterizePreview=*/true);
    // A full-pressure press stamps immediately (appendSmoothedPoint's
    // first-point branch), so one point is one stamp — no overlap, no
    // Rolling-anchor smear between stamps.
    sb.addRawPoint(QPointF(kPatchPx * 0.5, kPatchPx * 0.5));
    QImage patch = sb.strokeMask();
    if (patch == m_patch)
        return; // byte-identical render: the edit was not a grain parameter
    m_patch = patch;

    QImage inked(m_patch.size(), QImage::Format_ARGB32_Premultiplied);
    const QColor ink = kDragger;
    for (int y = 0; y < m_patch.height(); ++y) {
        const uchar *src = m_patch.constScanLine(y);
        QRgb *dst = reinterpret_cast<QRgb *>(inked.scanLine(y));
        for (int x = 0; x < m_patch.width(); ++x)
            dst[x] = qPremultiply(
                qRgba(ink.red(), ink.green(), ink.blue(), src[x]));
    }
    m_display = QPixmap::fromImage(inked);
    update();
}

void StudioGrainPreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Panel chrome (359:48 background, 359:49 inset outline well) — the
    // Tip Shape panel's exact tokens; the two are siblings in the design.
    p.setPen(Qt::NoPen);
    p.setBrush(kPanelBg);
    p.drawRoundedRect(QRectF(0, 0, kPanelWidth, kPanelHeight),
                      kCornerRadius, kCornerRadius);
    p.setPen(QPen(kPanelOutline, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(kWellInset + 0.5, kWellInset + 0.5,
                             kPanelWidth - 2 * kWellInset - 1,
                             kPanelHeight - 2 * kWellInset - 1),
                      kCornerRadius - 0.5, kCornerRadius - 0.5);
    if (!m_display.isNull()) {
        const QSizeF s = QSizeF(m_display.size());
        p.drawPixmap(QPointF((kPanelWidth - s.width()) / 2.0,
                             (kPanelHeight - s.height()) / 2.0),
                     m_display);
    }
}

// ---------------------------------------------------------------------------
// StudioTextField
// ---------------------------------------------------------------------------

namespace studio {
QFont fieldFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(11);
    return f;
}
QFont fieldLabelFont()
{
    QFont f(QStringLiteral("Inter"));
    f.setPixelSize(10);
    f.setWeight(QFont::Medium);
    return f;
}
} // namespace studio

StudioTextField::StudioTextField(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(25);
    setCursor(Qt::IBeamCursor);
    setAttribute(Qt::WA_Hover);
    m_edit = new QLineEdit(this);
    m_edit->setFrame(false);
    m_edit->setFont(studio::fieldFont());
    // The embedded editor paints NOTHING but text + caret: transparent
    // background, our field colours. The well, border, hover and focus
    // states are painted by this widget.
    QPalette pal = m_edit->palette();
    pal.setColor(QPalette::Base, Qt::transparent);
    pal.setColor(QPalette::Text, studio::kFieldText);
    pal.setColor(QPalette::PlaceholderText, QColor(0x66, 0x66, 0x66));
    pal.setColor(QPalette::Highlight, studio::kAccent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    m_edit->setPalette(pal);
    m_edit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: transparent; border: none; padding: 0; }"));
    connect(m_edit, &QLineEdit::textEdited, this,
            &StudioTextField::textEdited);
    connect(m_edit, &QLineEdit::returnPressed, this,
            &StudioTextField::submitted);
    // Repaint on focus transitions so the accent focus ring tracks the
    // editor's real focus, which is the only focus that exists here.
    m_edit->installEventFilter(this);
    setFocusProxy(m_edit);
}

bool StudioTextField::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit
        && (event->type() == QEvent::FocusIn
            || event->type() == QEvent::FocusOut))
        update(); // the accent focus ring follows the editor's focus
    return QWidget::eventFilter(watched, event);
}

QString StudioTextField::text() const { return m_edit->text(); }
void StudioTextField::setText(const QString &text) { m_edit->setText(text); }
void StudioTextField::setPlaceholder(const QString &text)
{
    m_edit->setPlaceholderText(text);
}

void StudioTextField::setNumericMode(int minValue, int maxValue)
{
    // QIntValidator alone still allows out-of-range intermediates (typing
    // "9" toward "96"), which is correct for editing; range enforcement is
    // the caller's validation step via intValue().
    m_edit->setValidator(new QIntValidator(minValue, maxValue, m_edit));
}

int StudioTextField::intValue() const { return m_edit->text().toInt(); }

void StudioTextField::setFieldEnabled(bool enabled)
{
    if (m_fieldEnabled == enabled)
        return;
    m_fieldEnabled = enabled;
    // Read-only rather than disabled: the value stays selectable/copyable,
    // input is refused, and the DIMMED rendering below makes the refusal
    // visible — never a live-looking field that ignores keystrokes.
    m_edit->setReadOnly(!enabled);
    QPalette pal = m_edit->palette();
    pal.setColor(QPalette::Text, enabled ? studio::kFieldText
                                         : QColor(0x77, 0x77, 0x77));
    m_edit->setPalette(pal);
    setCursor(enabled ? Qt::IBeamCursor : Qt::ArrowCursor);
    update();
}

void StudioTextField::resizeEvent(QResizeEvent *)
{
    m_edit->setGeometry(8, 0, width() - 16, height());
}

void StudioTextField::mousePressEvent(QMouseEvent *)
{
    if (m_fieldEnabled)
        m_edit->setFocus(Qt::MouseFocusReason);
}

void StudioTextField::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}
void StudioTextField::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void StudioTextField::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor border = studio::kFieldBorder;
    if (m_fieldEnabled) {
        if (m_edit->hasFocus())
            border = studio::kAccent; // focus ring
        else if (m_hover)
            border = studio::kFieldBorderHover;
    }
    p.setPen(QPen(border, 1.0));
    p.setBrush(studio::kFieldBg);
    p.drawRoundedRect(r, 3, 3);
}

// ---------------------------------------------------------------------------
// StudioDropdown
// ---------------------------------------------------------------------------

namespace {
// The popup list: a frameless Qt::Popup child-less widget painting option
// rows in the field language. Closes on pick or outside click (Qt::Popup
// semantics). Kept file-local — the dropdown is its only client.
class DropdownPopup : public QWidget
{
public:
    static constexpr int kRowH = 24;
    DropdownPopup(StudioDropdown *owner, const QStringList &options,
                  int current)
        : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint),
          m_owner(owner), m_options(options), m_hover(current)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setMouseTracking(true);
    }
    QSize sizeHint() const override
    {
        return QSize(m_owner->width(), int(m_options.size()) * kRowH + 8);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(studio::kFieldBorder, 1.0));
        p.setBrush(studio::kFieldBg);
        p.drawRoundedRect(r, 3, 3);
        p.setFont(studio::fieldFont());
        for (int i = 0; i < m_options.size(); ++i) {
            const QRect row(1, 4 + i * kRowH, width() - 2, kRowH);
            if (i == m_hover) {
                p.setPen(Qt::NoPen);
                p.setBrush(studio::kAccent);
                p.drawRect(row);
            }
            p.setPen(i == m_hover ? QColor(Qt::white) : studio::kFieldText);
            p.drawText(row.adjusted(8, 0, -8, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, m_options.at(i));
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int i = (int(event->position().y()) - 4) / kRowH;
        if (i >= 0 && i < m_options.size() && i != m_hover) {
            m_hover = i;
            update();
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        const int i = (int(event->position().y()) - 4) / kRowH;
        if (i >= 0 && i < m_options.size())
            m_owner->choose(i);
        close();
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
            close();
        else if (event->key() == Qt::Key_Return
                 || event->key() == Qt::Key_Enter) {
            if (m_hover >= 0)
                m_owner->choose(m_hover);
            close();
        } else if (event->key() == Qt::Key_Down
                   || event->key() == Qt::Key_Up) {
            const int step = event->key() == Qt::Key_Down ? 1 : -1;
            m_hover = qBound(0, m_hover + step,
                             int(m_options.size()) - 1);
            update();
        }
    }

private:
    StudioDropdown *m_owner;
    QStringList m_options;
    int m_hover;
};
} // namespace

StudioDropdown::StudioDropdown(const QStringList &options, QWidget *parent)
    : QWidget(parent), m_options(options)
{
    setFixedHeight(25);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus); // tab stop; Space/Enter opens
}

QString StudioDropdown::currentText() const
{
    return m_options.value(m_index);
}

void StudioDropdown::setCurrentIndex(int index)
{
    m_index = qBound(0, index, int(m_options.size()) - 1);
    update();
}

void StudioDropdown::choose(int index)
{
    setCurrentIndex(index);
    emit chosen(m_index);
}

void StudioDropdown::openPopup()
{
    auto *popup = new DropdownPopup(this, m_options, m_index);
    popup->resize(popup->sizeHint());
    popup->move(mapToGlobal(QPoint(0, height() + 2)));
    popup->show();
    popup->setFocus();
}

void StudioDropdown::mousePressEvent(QMouseEvent *)
{
    openPopup();
}

void StudioDropdown::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter)
        openPopup();
    else
        QWidget::keyPressEvent(event);
}

void StudioDropdown::enterEvent(QEnterEvent *)
{
    m_hover = true;
    update();
}
void StudioDropdown::leaveEvent(QEvent *)
{
    m_hover = false;
    update();
}

void StudioDropdown::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor border = studio::kFieldBorder;
    if (hasFocus())
        border = studio::kAccent;
    else if (m_hover)
        border = studio::kFieldBorderHover;
    p.setPen(QPen(border, 1.0));
    p.setBrush(studio::kFieldBg);
    p.drawRoundedRect(r, 3, 3);
    p.setFont(studio::fieldFont());
    p.setPen(studio::kFieldText);
    p.drawText(rect().adjusted(8, 0, -24, 0),
               Qt::AlignVCenter | Qt::AlignLeft, currentText());
    // 10px chevron, right-aligned (design 350:103): two 1px strokes.
    const QPointF c(width() - 16.0, height() / 2.0);
    p.setPen(QPen(studio::kFieldLabel, 1.2));
    p.drawLine(c + QPointF(-3.5, -1.5), c + QPointF(0, 2));
    p.drawLine(c + QPointF(0, 2), c + QPointF(3.5, -1.5));
}

} // namespace brushlib
