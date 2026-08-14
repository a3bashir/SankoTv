#include "StudioControls.h"

#include <QFontMetrics>
#include <QHideEvent>
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
    m_index = std::clamp(index, 0, int(m_options.size()) - 1);
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
// Figma 341:30 tokens, verbatim (kAccent IS the ring's #7C6EF6).
const QColor kRingHandle(0xd9, 0xd9, 0xd9);
const QColor kRingPivot(0xcc, 0xcc, 0xcc);
// Added hover/active states (documented divergence): the palette's
// brighten language.
const QColor kRingHover(0x8f, 0x83, 0xf8);
const QColor kRingElementHover(0xff, 0xff, 0xff);
constexpr double kRingStrokeWidth = 8.0;
constexpr double kPivotStrokeWidth = 2.0;
} // namespace

StudioTipRing::StudioTipRing(QWidget *parent)
    : QWidget(parent)
{
    // The Figma frame, exactly; the layout centres the widget in its row.
    setFixedSize(91, 91);
    setMouseTracking(true); // hover states + cursor feedback
}

QPointF StudioTipRing::ringCentre() const
{
    // TRUE centre. Documented divergence: the design's ring ellipse sits
    // at x 46 while the handles and pivot sit at 45.5 — a half-pixel
    // authoring drift, normalised here so handles land ON the stroke.
    return QPointF(45.5, 45.5);
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

StudioTipRing::Region StudioTipRing::hitTest(const QPointF &pos) const
{
    // Priority: pivot, then handles, then ring — the handles sit on the
    // stroke, so they must win where the regions overlap.
    const double fromCentre = QLineF(ringCentre(), pos).length();
    if (fromCentre <= kPivotGrabRadius)
        return Region::Pivot;
    for (int i = 0; i < 4; ++i)
        if (QLineF(handleCentre(i), pos).length() <= kHandleGrabRadius)
            return Region::Handle;
    if (std::abs(fromCentre - kRingRadius) <= kRingGrabTolerance)
        return Region::Ring;
    return Region::None;
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

    // Ring: Figma stroke, r 40.5 at width 8.
    p.setPen(QPen(ringLive ? kRingHover : kAccent, kRingStrokeWidth));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ringCentre(), kRingRadius, kRingRadius);

    // Handles: r 5 fills, positioned by the current angle and roundness.
    p.setPen(Qt::NoPen);
    p.setBrush(handleLive ? kRingElementHover : kRingHandle);
    for (int i = 0; i < 4; ++i)
        p.drawEllipse(handleCentre(i), kHandleRadius, kHandleRadius);

    // Pivot: hollow r 4 stroke 2.
    p.setPen(QPen(pivotLive ? kRingElementHover : kRingPivot,
                  kPivotStrokeWidth));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ringCentre(), kPivotRadius, kPivotRadius);
}

} // namespace brushlib
