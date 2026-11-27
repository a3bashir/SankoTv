#include "SankoStandardSlider.h"
#include "SankoTheme.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>

namespace {

// The value's box: 44 px wide, its right edge on the track's, 2 px below the top, 15 px high
// (the toolbar's, widened to the left so "100%" and "-180" are not cut).
constexpr int kValueBoxW = 44, kValueBoxTop = 2, kValueBoxH = 15;
// (Hue) The swatch in the value's place.
constexpr int kSwatchW = 16, kSwatchH = 8, kSwatchTop = 3;

} // namespace

SankoStandardSlider::SankoStandardSlider(const QString &label, Kind kind, QWidget *parent)
    : QWidget(parent), m_label(label), m_kind(kind)
{
    if (kind == Kind::Hue)
        m_max = 359;
    setCursor(Qt::PointingHandCursor);
    // On whatever it is laid on: only the label, the value, the track and the dragger are drawn.
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setFixedHeight(kHeight);
    setMinimumWidth(kDraggerWidth * 3);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_labelFont = QFont(QStringLiteral("Inter")); // falls back to the UI font
    m_labelFont.setPixelSize(9);
    m_labelFont.setWeight(QFont::DemiBold);
    m_valueFont = m_labelFont;
    m_valueFont.setPixelSize(8);
}

QSize SankoStandardSlider::sizeHint() const
{
    return QSize(kStandardWidth, kHeight);
}

QSize SankoStandardSlider::minimumSizeHint() const
{
    return QSize(kDraggerWidth * 3, kHeight);
}

void SankoStandardSlider::setRange(int min, int max)
{
    m_min = min;
    m_max = qMax(min + 1, max);
    const int kept = qBound(m_min, m_value, m_max);
    if (kept != m_value) {
        m_value = kept;
        emit valueChanged(m_value);
    }
    update();
}

void SankoStandardSlider::setValueSuffix(const QString &suffix)
{
    m_suffix = suffix;
    update();
}

QString SankoStandardSlider::valueText() const
{
    return m_kind == Kind::Hue ? QString() : QString::number(m_value) + m_suffix;
}

void SankoStandardSlider::setSwatch(const QColor &color)
{
    if (color == m_swatch)
        return;
    m_swatch = color;
    update();
}

void SankoStandardSlider::setValue(int value)
{
    value = qBound(m_min, value, m_max);
    if (value == m_value)
        return;
    m_value = value;
    update();
    emit valueChanged(m_value);
}

double SankoStandardSlider::fraction() const
{
    return double(m_value - m_min) / double(qMax(1, m_max - m_min));
}

QRectF SankoStandardSlider::trackRect() const
{
    return QRectF(0, kTrackTop, width(), kTrackHeight);
}

QRectF SankoStandardSlider::draggerRect() const
{
    return QRectF(fraction() * (width() - kDraggerWidth), kTrackTop, kDraggerWidth, kTrackHeight);
}

QRectF SankoStandardSlider::fillRect() const
{
    if (m_kind == Kind::Hue)
        return QRectF();
    const double fillW = draggerRect().left() + kDraggerWidth / 2.0; // up to the dragger's middle
    return fillW > 1.0 ? QRectF(0, kTrackTop, fillW, kTrackHeight) : QRectF();
}

int SankoStandardSlider::xForValue(int value) const
{
    const double t = double(qBound(m_min, value, m_max) - m_min) / double(qMax(1, m_max - m_min));
    return qBound(0, qRound(t * width()), qMax(0, width() - 1));
}

void SankoStandardSlider::setFromX(double x)
{
    const double t = qBound(0.0, x / double(qMax(1, width())), 1.0);
    setValue(m_min + qRound(t * (m_max - m_min)));
}

void SankoStandardSlider::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The label, and the value at the track's right end.
    if (!m_label.isEmpty()) {
        p.setFont(m_labelFont);
        p.setPen(QColor(0xcc, 0xcc, 0xcc));
        p.drawText(QPointF(0, QFontMetrics(m_labelFont).ascent()), m_label);
    }
    if (m_kind == Kind::Value) {
        p.setFont(m_valueFont);
        p.setPen(QColor(0xcc, 0xcc, 0xcc));
        p.drawText(QRectF(width() - kValueBoxW, kValueBoxTop, kValueBoxW, kValueBoxH),
                   Qt::AlignRight | Qt::AlignVCenter, valueText());
    } else if (m_swatch.isValid()) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_swatch);
        p.drawRoundedRect(QRectF(width() - kSwatchW, kSwatchTop, kSwatchW, kSwatchH), 2, 2);
    }

    // The track.
    const QRectF track = trackRect();
    p.setPen(Qt::NoPen);
    if (m_kind == Kind::Hue) {
        QLinearGradient spectrum(track.topLeft(), track.topRight());
        for (int i = 0; i <= 12; ++i)
            spectrum.setColorAt(i / 12.0, QColor::fromHsvF(float(qMin(i / 12.0, 0.999)), 1.0f, 1.0f));
        p.setBrush(spectrum);
        p.drawRoundedRect(track, 2, 2);
    } else {
        p.setBrush(QColor(0x33, 0x33, 0x33));
        p.drawRoundedRect(track, 2, 2);
        const QRectF fill = fillRect();
        if (!fill.isEmpty()) {
            QLinearGradient purple(fill.left(), 0, fill.right(), 0); // dark at the left, light at the right
            purple.setColorAt(0.0, QColor(75, 67, 151)); // #4b4397
            purple.setColorAt(1.0, SankoTheme::kPurple);
            p.setBrush(purple);
            p.drawRoundedRect(fill, 1, 1);
            QLinearGradient sheen(0, fill.top(), 0, fill.bottom()); // white, 10 % to none
            sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
            sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.setBrush(sheen);
            p.drawRoundedRect(fill, 1, 1);
        }
    }

    // The dragger.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xb3, 0xb3, 0xb3));
    p.drawRoundedRect(draggerRect(), 1, 1);
}

void SankoStandardSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging = true;
    emit dragStarted();
    setFromX(event->position().x());
    event->accept();
}

void SankoStandardSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton))
        setFromX(event->position().x());
    event->accept();
}

void SankoStandardSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        emit dragFinished();
    }
    event->accept();
}
