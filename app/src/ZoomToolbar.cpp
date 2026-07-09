#include "ZoomToolbar.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

#include <cmath>

namespace {

// Exact Figma geometry (origin = toolbar 0,0), node 86:32.
constexpr int kW = 528, kH = 46, kRadius = 12;

// Grip: dot matrix inside a 12x20 box at (12,13).
constexpr int kGripX = 12, kGripY = 13, kGripW = 12, kGripH = 20;

// A control group ("zoom" / "rotate") is 160 wide; the label sits at its top,
// the value readout at +138 (right-aligned to the track end), the track 14px
// below the group top.
constexpr double kGroupTopY = 10.5;
constexpr double kTrackY = kGroupTopY + 14.0; // 24.5
constexpr int kTrackW = 160, kTrackH = 10;
constexpr int kDraggerW = 20;
constexpr int kValueDX = 138, kValueW = 22, kValueH = 15;
constexpr double kValueTopY = 12.5;

constexpr int kZoomGroupX = 43;
constexpr int kRotGroupX = 311;

constexpr double kDivY = 11.5;
constexpr int kDiv1X = 222, kDivH = 23;
constexpr int kDiv2X = 291;

// Flip / Reset are 30x30 button boxes (radius 6, bg #212121 = toolbar body, so
// effectively icon-only); the icon sits centred inside at its Figma size
// (flip 21.6x17.55, reset 21x21).
constexpr int kFlipX = 242, kFlipY = 8, kFlipS = 30;
constexpr int kResetX = 490, kResetY = 8, kResetS = 30;

// Vertical band that counts as a hit on a track (label row down through track).
constexpr int kTrackHitY0 = 10, kTrackHitY1 = 40;

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
    setMouseTracking(true); // hover feedback on the Flip / Reset buttons
    m_labelFont = QFont(QStringLiteral("Inter")); // falls back to the UI font
    m_labelFont.setPixelSize(9);
    m_labelFont.setWeight(QFont::DemiBold); // "Semi Bold"
    m_valueFont = m_labelFont;
    m_valueFont.setPixelSize(8);
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

    // Grip: 6 dots (2 cols x 3 rows of r=2 circles, #6a6a6a), matching the
    // Figma grab-dots SVG exactly (circles at relative 2/10 x 2/10/18 in the
    // 12x20 box).
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#6a6a6a"));
    {
        const double gx = kGripX; // 12
        const double gy = kGripY; // 13 (grab-dots frame y in toolbar)
        const double cols[2] = {gx + 2.0, gx + 10.0};
        const double rows[3] = {gy + 2.0, gy + 10.0, gy + 18.0};
        for (double cx : cols)
            for (double cy : rows)
                p.drawEllipse(QPointF(cx, cy), 2.0, 2.0);
    }

    // Labels.
    p.setFont(m_labelFont);
    p.setPen(QColor("#cccccc"));
    const int ascent = QFontMetrics(m_labelFont).ascent();
    p.drawText(QPointF(kZoomGroupX, kGroupTopY + ascent), QStringLiteral("Zoom"));
    p.drawText(QPointF(kRotGroupX, kGroupTopY + ascent), QStringLiteral("Rotate"));

    // Numeric readouts, right-aligned to each track's right edge. The box is
    // widened leftward (right edge pinned at track end) so 4-char values like
    // "200%" / "-180" don't clip.
    p.setFont(m_valueFont);
    const int valueRight = kValueDX + kValueW; // 160 = track right edge
    const int valueBoxW = 44;
    p.drawText(QRectF(kZoomGroupX + valueRight - valueBoxW, kValueTopY, valueBoxW, kValueH),
               Qt::AlignRight | Qt::AlignVCenter,
               QString::number(qRound(m_zoom * 100.0)) + QLatin1Char('%'));
    p.drawText(QRectF(kRotGroupX + valueRight - valueBoxW, kValueTopY, valueBoxW, kValueH),
               Qt::AlignRight | Qt::AlignVCenter,
               QString::number(qRound(m_rotation)) + QChar(0x00B0));

    // A track: #333 base (radius 2), purple-gradient filled "Control" (270deg
    // #4b4397 -> #7c6ef6 with a faint white top sheen, per Figma), #b3b3b3
    // dragger (radius 1).
    auto paintTrack = [&](int trackX, double t) {
        const QRectF track(trackX, kTrackY, kTrackW, kTrackH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#333333"));
        p.drawRoundedRect(track, 2, 2);

        const double draggerLeft = trackX + t * (kTrackW - kDraggerW);
        const double fillW = draggerLeft + kDraggerW / 2.0 - trackX; // up to dragger centre
        if (fillW > 1.0) {
            const QRectF fillR(trackX, kTrackY, fillW, kTrackH);
            QLinearGradient purple(fillR.left(), 0, fillR.right(), 0); // 270deg: dark left -> light right
            purple.setColorAt(0.0, QColor(75, 67, 151));   // #4b4397
            purple.setColorAt(1.0, QColor(124, 110, 246)); // #7c6ef6
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
    paintTrack(kZoomGroupX, zoomT());
    paintTrack(kRotGroupX, rotationT());

    // Dividers.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#4d4d4d"));
    p.drawRect(QRectF(kDiv1X, kDivY, 1, kDivH));
    p.drawRect(QRectF(kDiv2X, kDivY, 1, kDivH));

    // Flip / Reset buttons (30x30 boxes, radius 6). The Figma default bg is
    // #212121 = the toolbar body (so the default reads as icon-only); hover and
    // pressed lift it to the app's standard #262626 / #303030. Flip also shows
    // a purple accent while toggled on. Icons render at their Figma size,
    // centred.
    auto paintButtonBg = [&](const QRectF &box, Button which, bool active) {
        QColor bg(Qt::transparent);
        if (m_pressed == which)
            bg = QColor("#303030"); // pressed
        else if (m_hover == which)
            bg = QColor("#262626"); // hover
        p.setPen(Qt::NoPen);
        if (bg.alpha() > 0) {
            p.setBrush(bg);
            p.drawRoundedRect(box, 6, 6);
        }
        if (active) { // Flip toggled on: purple accent over any hover/press bg
            p.setBrush(QColor(124, 110, 246, 60)); // #7c6ef6 @ ~24%
            p.drawRoundedRect(box, 6, 6);
        }
    };

    const QRectF flipBox(kFlipX, kFlipY, kFlipS, kFlipS);
    paintButtonBg(flipBox, BtnFlip, m_flipH);
    QSvgRenderer flipSvg(QStringLiteral(":/icons/flip.svg"));
    {
        const double fw = 21.6, fh = 17.55; // Figma flip icon size
        flipSvg.render(&p, QRectF(kFlipX + (kFlipS - fw) / 2.0,
                                  kFlipY + (kFlipS - fh) / 2.0, fw, fh));
    }

    const QRectF resetBox(kResetX, kResetY, kResetS, kResetS);
    paintButtonBg(resetBox, BtnReset, false);
    QSvgRenderer resetSvg(QStringLiteral(":/icons/resetrotation.svg"));
    {
        const double rw = 21.0, rh = 21.0;
        resetSvg.render(&p, QRectF(kResetX + (kResetS - rw) / 2.0,
                                   kResetY + (kResetS - rh) / 2.0, rw, rh));
    }
}

void ZoomToolbar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->pos();
    const bool inTrackBand = pos.y() >= kTrackHitY0 && pos.y() <= kTrackHitY1;

    // Grip drag (reposition the floating toolbar).
    if (pos.x() >= kGripX - 2 && pos.x() <= kGripX + kGripW + 3) {
        m_drag = DragGrip;
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_toolbarStartPos = this->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    // Flip / Reset buttons: show the pressed state now; the action fires on
    // release if the cursor is still over the same button.
    const Button btn = buttonAt(pos);
    if (btn != BtnNone) {
        m_pressed = btn;
        m_hover = BtnNone;
        update();
        return;
    }
    // Zoom track.
    if (pos.x() >= kZoomGroupX && pos.x() <= kZoomGroupX + kTrackW && inTrackBand) {
        m_drag = DragZoom;
        m_zoom = tToZoom((pos.x() - kZoomGroupX) / double(kTrackW));
        update();
        emit zoomChanged(m_zoom);
        return;
    }
    // Rotate track.
    if (pos.x() >= kRotGroupX && pos.x() <= kRotGroupX + kTrackW && inTrackBand) {
        m_drag = DragRotate;
        m_rotation = tToRot((pos.x() - kRotGroupX) / double(kTrackW));
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
        m_zoom = tToZoom((event->pos().x() - kZoomGroupX) / double(kTrackW));
        update();
        emit zoomChanged(m_zoom);
        break;
    case DragRotate:
        m_rotation = tToRot((event->pos().x() - kRotGroupX) / double(kTrackW));
        update();
        emit rotationChanged(m_rotation);
        break;
    default: {
        // Not dragging a slider/grip: track hover over the Flip / Reset buttons
        // (skip while a button is held down so the pressed state stays put).
        if (m_pressed != BtnNone)
            break;
        const Button h = buttonAt(event->pos());
        if (h != m_hover) {
            m_hover = h;
            setCursor(h == BtnNone ? Qt::ArrowCursor : Qt::PointingHandCursor);
            update();
        }
        break;
    }
    }
}

void ZoomToolbar::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drag == DragGrip)
        setCursor(Qt::ArrowCursor);
    m_drag = DragNone;

    // Fire the button action only if released over the same button.
    if (m_pressed != BtnNone) {
        const QPoint pos = event->position().toPoint();
        const Button released = buttonAt(pos);
        if (released == m_pressed) {
            if (m_pressed == BtnFlip) {
                m_flipH = !m_flipH;
                emit flipToggled();
            } else if (m_pressed == BtnReset) {
                m_rotation = 0.0;
                emit rotationChanged(0.0);
            }
        }
        m_pressed = BtnNone;
        m_hover = released; // resume hover if still over a button
        setCursor(released == BtnNone ? Qt::ArrowCursor : Qt::PointingHandCursor);
        update();
    }
}

void ZoomToolbar::leaveEvent(QEvent *)
{
    if (m_hover != BtnNone) {
        m_hover = BtnNone;
        setCursor(Qt::ArrowCursor);
        update();
    }
}

ZoomToolbar::Button ZoomToolbar::buttonAt(const QPoint &pos) const
{
    if (QRect(kFlipX, kFlipY, kFlipS, kFlipS).contains(pos))
        return BtnFlip;
    if (QRect(kResetX, kResetY, kResetS, kResetS).contains(pos))
        return BtnReset;
    return BtnNone;
}
