#include "SankoSlider.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

namespace {

constexpr int kBleed = 3;      // extra cross-size padding around the handle
constexpr int kLabelGap = 6;   // space between the track run and the label
constexpr int kMinRunH = 90;   // minimum run, horizontal
constexpr int kMinRunV = 60;   // minimum run, vertical (tight tool column;
                               // the layout stretch grows it with the window)

const QColor kPurple(0x7c, 0x6e, 0xf6);

QFont labelFont()
{
    QFont font(QStringLiteral("Consolas"));
    font.setPixelSize(12);
    return font;
}

} // namespace

SankoSlider::SankoSlider(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus); // arrow keys work when focused
    setCursor(Qt::PointingHandCursor);
    refreshSizeConstraints();
}

void SankoSlider::setRange(int min, int max)
{
    m_min = min;
    m_max = qMax(min + 1, max);
    setValue(m_value); // re-clamp into the new range
    update();
}

void SankoSlider::setOrientation(Qt::Orientation orientation)
{
    m_orientation = orientation;
    refreshSizeConstraints();
    update();
}

void SankoSlider::setTrackHeight(int px)
{
    m_trackH = qMax(4, px);
    refreshSizeConstraints();
    update();
}

void SankoSlider::setHandleSize(int px)
{
    m_handle = qMax(8, px);
    refreshSizeConstraints();
    update();
}

void SankoSlider::setValueSuffix(const QString &suffix)
{
    m_suffix = suffix;
    update();
}

void SankoSlider::setValue(int value)
{
    value = qBound(m_min, value, m_max);
    if (value == m_value)
        return;
    m_value = value;
    update();
    emit valueChanged(m_value);
}

void SankoSlider::refreshSizeConstraints()
{
    const int cross = qMax(m_trackH, m_handle) + 2 * kBleed;
    if (m_orientation == Qt::Horizontal) {
        setMinimumSize(kMinRunH, cross);
        setMaximumSize(QWIDGETSIZE_MAX, cross);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        setMinimumSize(cross, kMinRunV);
        setMaximumSize(cross, QWIDGETSIZE_MAX);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    updateGeometry();
}

int SankoSlider::labelExtent() const
{
    // Reserve stable space for the widest possible value text so the track
    // never jitters as digits change.
    const QFontMetrics fm(labelFont());
    if (m_orientation == Qt::Horizontal)
        return fm.horizontalAdvance(QString::number(m_max) + m_suffix) + 4;
    return fm.height() + 2; // label sits BELOW a vertical slider
}

qreal SankoSlider::trackLength() const
{
    const qreal run = (m_orientation == Qt::Horizontal) ? width() : height();
    return qMax<qreal>(m_handle * 2.0, run - labelExtent() - kLabelGap);
}

qreal SankoSlider::handleCenterPos() const
{
    const qreal margin = m_handle / 2.0;
    const qreal span = trackLength() - 2.0 * margin;
    const qreal t = qreal(m_value - m_min) / qreal(m_max - m_min);
    if (m_orientation == Qt::Horizontal)
        return margin + t * span;             // min at left, max at right
    return trackLength() - margin - t * span; // min at BOTTOM, max at top
}

int SankoSlider::valueFromPos(qreal pos) const
{
    const qreal margin = m_handle / 2.0;
    const qreal span = trackLength() - 2.0 * margin;
    if (span <= 0.0)
        return m_value;
    const qreal t = (m_orientation == Qt::Horizontal)
        ? (pos - margin) / span
        : (trackLength() - margin - pos) / span;
    return qBound(m_min, m_min + qRound(t * (m_max - m_min)), m_max);
}

void SankoSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool enabled = isEnabled();
    if (!enabled)
        p.setOpacity(0.4); // whole widget dimmed when disabled

    const bool horizontal = (m_orientation == Qt::Horizontal);
    const qreal trackR = m_trackH * 0.35; // squared-ish ends, matches handle

    const QRectF track = horizontal
        ? QRectF(0.5, (height() - m_trackH) / 2.0, trackLength() - 1.0, m_trackH)
        : QRectF((width() - m_trackH) / 2.0, 0.5, m_trackH, trackLength() - 1.0);

    // Track.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x23, 0x23, 0x23));
    p.drawRoundedRect(track, trackR, trackR);
    if (m_hovered && enabled) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0x4a, 0x4a, 0x4a), 0.5));
        p.drawRoundedRect(track, trackR, trackR);
    }

    const qreal c = handleCenterPos();

    // Flat solid fill (no glow/bloom in any state): left->handle when
    // horizontal, BOTTOM->handle when vertical. The fill's rounding toward
    // the handle hides beneath it.
    const QRectF fill = horizontal
        ? QRectF(track.left(), track.top(), c - track.left(), m_trackH)
        : QRectF(track.left(), c, m_trackH, track.bottom() - c);
    const qreal fillRun = horizontal ? fill.width() : fill.height();
    if (fillRun > 0.5 && enabled) {
        p.setPen(Qt::NoPen);
        p.setBrush(kPurple);
        p.drawRoundedRect(fill, trackR, trackR);
    }

    // Handle, coloured by state.
    QColor handleColor;
    if (!enabled)
        handleColor = QColor(0x3a, 0x3a, 0x3a);
    else if (m_dragging)
        handleColor = QColor(0xff, 0xff, 0xff);
    else if (m_hovered)
        handleColor = QColor(0xf0, 0xf0, 0xf0);
    else
        handleColor = QColor(0xe8, 0xe8, 0xe8);

    const QRectF handle = horizontal
        ? QRectF(c - m_handle / 2.0, (height() - m_handle) / 2.0, m_handle, m_handle)
        : QRectF((width() - m_handle) / 2.0, c - m_handle / 2.0, m_handle, m_handle);
    p.setBrush(handleColor);
    p.setPen(m_dragging && enabled ? QPen(kPurple, 1) : QPen(Qt::NoPen));
    p.drawRoundedRect(handle, m_handle * 0.3, m_handle * 0.3);

    // Live value label: right of a horizontal slider, BELOW a vertical one.
    p.setPen(kPurple);
    p.setFont(labelFont());
    const QString text = QString::number(m_value) + m_suffix;
    if (horizontal) {
        const QRectF labelRect(trackLength() + kLabelGap, 0,
                               width() - trackLength() - kLabelGap, height());
        p.drawText(labelRect, Qt::AlignVCenter | Qt::AlignRight, text);
    } else {
        const QRectF labelRect(0, trackLength() + kLabelGap, width(),
                               height() - trackLength() - kLabelGap);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, text);
    }
}

void SankoSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus(Qt::MouseFocusReason);
    m_dragging = true; // click on the track jumps AND starts dragging
    const QPointF pos = event->position();
    setValue(valueFromPos(m_orientation == Qt::Horizontal ? pos.x() : pos.y()));
    update(); // repaint even when the value didn't change (drag glow)
}

void SankoSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const QPointF pos = event->position();
    setValue(valueFromPos(m_orientation == Qt::Horizontal ? pos.x() : pos.y()));
}

void SankoSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        update();
    }
}

void SankoSlider::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Down:
        setValue(m_value - 1);
        break;
    case Qt::Key_Right:
    case Qt::Key_Up:
        setValue(m_value + 1);
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

void SankoSlider::enterEvent(QEnterEvent *)
{
    m_hovered = true;
    update();
}

void SankoSlider::leaveEvent(QEvent *)
{
    m_hovered = false;
    update();
}

void SankoSlider::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::EnabledChange)
        update();
    QWidget::changeEvent(event);
}
