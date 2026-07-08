#include "ZoomToolbar.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

#include <cmath>

namespace {

// Exact Figma geometry (origin = toolbar 0,0).
constexpr int kW = 477, kH = 42, kRadius = 12;

constexpr int kGripX0 = 21, kGripX1 = 28; // dot columns
const int kGripRows[3] = {14, 20, 26};    // dot rows

constexpr int kZoomTextX = 42, kLabelY = 15;
constexpr int kTrackY = 18, kTrackW = 112, kTrackH = 6;
constexpr int kZoomTrackX = 84;
constexpr int kDiv1X = 207, kDivY = 11, kDivH = 20;
constexpr int kFlipX = 219, kFlipY = 9, kFlipS = 24;
constexpr int kDiv2X = 254;
constexpr int kRotTextX = 266;
constexpr int kRotTrackX = 312;
constexpr int kResetX = 435, kResetY = 11, kResetS = 21;

constexpr int kDraggerW = 20;

constexpr double kZoomMin = 0.25, kZoomMax = 4.0;

// Value <-> normalized position along a track (0..1).
double zoomToT(double zoom)
{
    zoom = qBound(kZoomMin, zoom, kZoomMax);
    return std::log(zoom / kZoomMin) / std::log(kZoomMax / kZoomMin);
}
double tToZoom(double t)
{
    return kZoomMin * std::pow(kZoomMax / kZoomMin, qBound(0.0, t, 1.0));
}
double rotToT(double deg) { return qBound(0.0, (deg + 180.0) / 360.0, 1.0); }
double tToRot(double t) { return -180.0 + qBound(0.0, t, 1.0) * 360.0; }

} // namespace

ZoomToolbar::ZoomToolbar(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(kW, kH);
    setMouseTracking(false);
    m_labelFont = QFont(QStringLiteral("Inter")); // falls back to the UI font
    m_labelFont.setPixelSize(11);
    m_labelFont.setWeight(QFont::DemiBold); // "Semi Bold"
}

double ZoomToolbar::zoomT() const { return zoomToT(m_zoom); }
double ZoomToolbar::rotationT() const { return rotToT(m_rotation); }

void ZoomToolbar::setZoom(double zoom)
{
    zoom = qBound(kZoomMin, zoom, kZoomMax);
    if (qFuzzyCompare(zoom, m_zoom))
        return;
    m_zoom = zoom;
    update();
}

void ZoomToolbar::setRotation(double degrees)
{
    degrees = qBound(-180.0, degrees, 180.0);
    if (qFuzzyCompare(degrees, m_rotation))
        return;
    m_rotation = degrees;
    update();
}

void ZoomToolbar::setFlipH(bool on)
{
    if (on == m_flipH)
        return;
    m_flipH = on;
    update();
}

void ZoomToolbar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Toolbar body: #212121 fill, 1px #1a1a1a border, radius 12.
    QRectF body(0.5, 0.5, kW - 1, kH - 1);
    p.setPen(QPen(QColor("#1a1a1a"), 1));
    p.setBrush(QColor("#212121"));
    p.drawRoundedRect(body, kRadius, kRadius);

    // Grip: 6 dots (3x3, #6a6a6a).
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#6a6a6a"));
    for (int row : kGripRows) {
        p.drawRect(QRect(kGripX0, row, 3, 3));
        p.drawRect(QRect(kGripX1, row, 3, 3));
    }

    // Labels.
    p.setFont(m_labelFont);
    p.setPen(QColor("#cccccc"));
    const int ascent = QFontMetrics(m_labelFont).ascent();
    p.drawText(QPointF(kZoomTextX, kLabelY + ascent), QStringLiteral("Zoom"));
    p.drawText(QPointF(kRotTextX, kLabelY + ascent), QStringLiteral("Rotate"));

    // A track: #333 base, purple-gradient filled "Control" (#7c6ef6 -> #3725d3
    // with a faint white top overlay, per Figma), #b3b3b3 dragger.
    auto paintTrack = [&](int trackX, double t) {
        const QRectF track(trackX, kTrackY, kTrackW, kTrackH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#333333"));
        p.drawRoundedRect(track, 1, 1);

        const double draggerLeft = trackX + t * (kTrackW - kDraggerW);
        const double fillW = draggerLeft + kDraggerW / 2.0 - trackX; // up to dragger centre
        if (fillW > 1.0) {
            const QRectF fillR(trackX, kTrackY, fillW, kTrackH);
            QLinearGradient purple(fillR.left(), 0, fillR.right(), 0); // 270deg: dark left -> light right
            purple.setColorAt(0.0, QColor("#3725d3"));
            purple.setColorAt(1.0, QColor("#7c6ef6"));
            p.setPen(Qt::NoPen);
            p.setBrush(purple);
            p.drawRoundedRect(fillR, 1, 1);
            QLinearGradient sheen(0, fillR.top(), 0, fillR.bottom()); // 10% -> 0% white overlay
            sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
            sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.setBrush(sheen);
            p.drawRoundedRect(fillR, 1, 1);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#b3b3b3"));
        p.drawRoundedRect(QRectF(draggerLeft, kTrackY, kDraggerW, kTrackH), 1, 1);
    };
    paintTrack(kZoomTrackX, zoomT());
    paintTrack(kRotTrackX, rotationT());

    // Dividers.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#4d4d4d"));
    p.drawRect(QRect(kDiv1X, kDivY, 1, kDivH));
    p.drawRect(QRect(kDiv2X, kDivY, 1, kDivH));

    // Flip button: just the icon (no background, per Figma); a faint accent
    // panel appears only while flip is active, for feedback.
    if (m_flipH) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(124, 110, 246, 60)); // #7c6ef6 @ ~24%
        p.drawRoundedRect(QRectF(kFlipX, kFlipY, kFlipS, kFlipS), 5, 5);
    }
    QSvgRenderer flipSvg(QStringLiteral(":/icons/flip.svg"));
    {
        const double sw = 21.0, sh = 17.0; // flip.svg native aspect
        const double s = qMin(kFlipS / sw, kFlipS / sh);
        const double fw = sw * s, fh = sh * s;
        flipSvg.render(&p, QRectF(kFlipX + (kFlipS - fw) / 2.0,
                                  kFlipY + (kFlipS - fh) / 2.0, fw, fh));
    }

    // Reset Rotation button icon.
    QSvgRenderer resetSvg(QStringLiteral(":/icons/resetrotation.svg"));
    resetSvg.render(&p, QRectF(kResetX, kResetY, kResetS, kResetS));
}

void ZoomToolbar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->pos();
    const QRect trackBand(0, kDivY, 0, kDivH); // vertical band for the tracks
    Q_UNUSED(trackBand);

    // Grip drag (reposition the floating toolbar).
    if (pos.x() >= kGripX0 - 2 && pos.x() <= kGripX1 + 5) {
        m_drag = DragGrip;
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_toolbarStartPos = this->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    // Flip button.
    if (QRect(kFlipX, kFlipY, kFlipS, kFlipS).contains(pos)) {
        m_flipH = !m_flipH;
        update();
        emit flipToggled();
        return;
    }
    // Reset Rotation button.
    if (QRect(kResetX, kResetY, kResetS, kResetS).contains(pos)) {
        m_rotation = 0.0;
        update();
        emit rotationChanged(0.0);
        return;
    }
    // Zoom track (x 84..196).
    if (pos.x() >= kZoomTrackX && pos.x() <= kZoomTrackX + kTrackW
        && pos.y() >= kDivY && pos.y() <= kDivY + kDivH) {
        m_drag = DragZoom;
        m_zoom = tToZoom((pos.x() - kZoomTrackX) / double(kTrackW));
        update();
        emit zoomChanged(m_zoom);
        return;
    }
    // Rotate track (x 312..424).
    if (pos.x() >= kRotTrackX && pos.x() <= kRotTrackX + kTrackW
        && pos.y() >= kDivY && pos.y() <= kDivY + kDivH) {
        m_drag = DragRotate;
        m_rotation = tToRot((pos.x() - kRotTrackX) / double(kTrackW));
        update();
        emit rotationChanged(m_rotation);
        return;
    }
}

void ZoomToolbar::mouseMoveEvent(QMouseEvent *event)
{
    switch (m_drag) {
    case DragGrip: {
        const QPoint delta = event->globalPosition().toPoint() - m_dragStartGlobal;
        QPoint np = m_toolbarStartPos + delta;
        if (parentWidget()) {
            const int maxX = qMax(0, parentWidget()->width() - width());
            const int maxY = qMax(0, parentWidget()->height() - height());
            np.setX(qBound(0, np.x(), maxX));
            np.setY(qBound(0, np.y(), maxY));
        }
        move(np);
        break;
    }
    case DragZoom:
        m_zoom = tToZoom((event->pos().x() - kZoomTrackX) / double(kTrackW));
        update();
        emit zoomChanged(m_zoom);
        break;
    case DragRotate:
        m_rotation = tToRot((event->pos().x() - kRotTrackX) / double(kTrackW));
        update();
        emit rotationChanged(m_rotation);
        break;
    default:
        break;
    }
}

void ZoomToolbar::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (m_drag == DragGrip)
        setCursor(Qt::ArrowCursor);
    m_drag = DragNone;
}
