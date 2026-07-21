#include "StoryboardPage.h"

#include "ColorPanel.h"

#include "DrawingCanvas.h"
#include "FloatingToolWindow.h"
#include "SankoSlider.h"
#include "ZoomToolbar.h"
#include "StoryboardModel.h"

#include "docking/DockController.h"

#include <QDockWidget>
#include <QMainWindow>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDropEvent>
#include <QMimeData>

#include <memory>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QPainterPath>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSvgRenderer>
#include <QPlainTextEdit>
#include <QKeySequence>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QStyle>
#include <QTabletEvent>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <Qt>

namespace {

// Dark theme for the native docking chrome (custom title bars, tab bar,
// splitters) to match the app. Applied on the embedded dock host; the
// reusable docking classes carry no styling of their own beyond the accent.
const char *kDockDarkStyle =
    "QMainWindow#storyboardDockHost { background: #0a0a0a; }"
    "QMainWindow#storyboardDockHost::separator { background: #1f1f1f;"
    " width: 3px; height: 3px; }"
    "QDockWidget { background: #111111; }"
    "#dockTitleBar { background: #161616; border-bottom: 1px solid #2a2a2a; }"
    // Figma 7-70 TitleBar: plain text on the bar — no box behind the title.
    "#dockTitleText { color: #cccccc; font-size: 11px; font-weight: 600;"
    " background: transparent; border: none; }"
    "#dockTitleClose { background: transparent; color: #999999; border: none;"
    " border-radius: 4px; font-size: 13px; }"
    "#dockTitleClose:hover { background: #262626; color: #f5a623; }"
    "QMainWindow#storyboardDockHost QTabBar::tab { background: #161616;"
    " color: #cccccc; border: 1px solid #2a2a2a; padding: 3px 8px;"
    " font-size: 11px; }"
    "QMainWindow#storyboardDockHost QTabBar::tab:selected {"
    " background: #1a1a1a; color: #f5a623; }";

constexpr int kThumbW = 160;
constexpr int kThumbH = 90; // 16:9

Panel *makePanel()
{
    return makeBlankPanel(); // one blank raster layer ("Layer 1"), 960x540
}

QPushButton *toolButton(const QString &text, const QString &tip)
{
    QPushButton *button = new QPushButton(text);
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(30); // compact: leaves room for the size slider
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        "  border-radius: 4px; font-size: 11px;"
        "}"
        "QPushButton:hover { background-color: #262626; }"
        "QPushButton:checked { background-color: #f5a623; color: #0a0a0a; border: none; font-weight: 600; }"));
    return button;
}

// Line icons for the floating pill toolbar, painted at 2x for crisp HiDPI.
// All drawn on a 20x20 logical grid with a 1.7px round-capped stroke.
QPixmap toolIconPixmap(const char *kind, const QColor &color)
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(40, 40));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);

    const QLatin1String k(kind);
    if (k == QLatin1String("brush")) {
        p.drawLine(QPointF(4, 16), QPointF(10.5, 9.5)); // handle
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(13.2, 6.8), 3.4, 3.4);     // head
    } else if (k == QLatin1String("erase")) {
        p.save();
        p.translate(10, 10);
        p.rotate(-45);
        p.drawRoundedRect(QRectF(-6.5, -4, 13, 8), 2, 2);
        p.drawLine(QPointF(-1.5, -4), QPointF(-1.5, 4)); // wedge split
        p.restore();
    } else if (k == QLatin1String("shapes")) {
        p.drawRect(QRectF(4, 4, 8.5, 8.5));           // square behind...
        p.drawEllipse(QPointF(13.2, 13.2), 4.3, 4.3); // ...overlapping circle
    } else if (k == QLatin1String("fill")) {
        p.save();
        p.translate(9, 9.5);
        p.rotate(45);
        p.drawRoundedRect(QRectF(-4.2, -4.2, 8.4, 8.4), 1.5, 1.5); // tipped bucket
        p.restore();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(QPointF(16, 15), 1.9, 1.9); // drip
    } else if (k == QLatin1String("camera")) {
        p.drawRoundedRect(QRectF(3, 6.5, 14, 9.5), 2, 2);
        p.drawEllipse(QPointF(10, 11.2), 2.8, 2.8);
        p.drawLine(QPointF(7, 6.5), QPointF(8.2, 4));   // viewfinder bump
        p.drawLine(QPointF(8.2, 4), QPointF(11.8, 4));
        p.drawLine(QPointF(11.8, 4), QPointF(13, 6.5));
    } else if (k == QLatin1String("onion")) {
        p.drawEllipse(QPointF(7.6, 10), 4.6, 4.6);      // two ghost frames
        p.drawEllipse(QPointF(12.4, 10), 4.6, 4.6);
    } else if (k == QLatin1String("selrect")) {
        QPen dashed = p.pen();
        dashed.setDashPattern({2.0, 1.6});
        p.setPen(dashed);
        p.drawRect(QRectF(4, 5, 12, 10));
    } else if (k == QLatin1String("selellipse")) {
        QPen dashed = p.pen();
        dashed.setDashPattern({2.0, 1.6});
        p.setPen(dashed);
        p.drawEllipse(QRectF(3.5, 5, 13, 10));
    } else if (k == QLatin1String("lasso")) {
        QPen dashed = p.pen();
        dashed.setDashPattern({2.0, 1.6});
        p.setPen(dashed);
        QPainterPath loop(QPointF(10, 4)); // irregular closed loop + tail
        loop.cubicTo(QPointF(16.5, 4.5), QPointF(17, 10), QPointF(13, 12.5));
        loop.cubicTo(QPointF(10, 14.5), QPointF(4, 14), QPointF(4, 9.5));
        loop.cubicTo(QPointF(4, 5.5), QPointF(7, 3.7), QPointF(10, 4));
        p.drawPath(loop);
        p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(12, 12.8), QPointF(15, 16.5)); // rope tail
    } else if (k == QLatin1String("move")) {
        p.drawLine(QPointF(10, 3.5), QPointF(10, 16.5));  // 4-way arrows
        p.drawLine(QPointF(3.5, 10), QPointF(16.5, 10));
        p.drawLine(QPointF(8, 5.5), QPointF(10, 3.5));  p.drawLine(QPointF(12, 5.5), QPointF(10, 3.5));
        p.drawLine(QPointF(8, 14.5), QPointF(10, 16.5)); p.drawLine(QPointF(12, 14.5), QPointF(10, 16.5));
        p.drawLine(QPointF(5.5, 8), QPointF(3.5, 10));  p.drawLine(QPointF(5.5, 12), QPointF(3.5, 10));
        p.drawLine(QPointF(14.5, 8), QPointF(16.5, 10)); p.drawLine(QPointF(14.5, 12), QPointF(16.5, 10));
    } else if (k == QLatin1String("undo")) {
        // tabler arrow-back-up: left arrowhead, run right, hook down.
        p.drawLine(QPointF(8.5, 4.5), QPointF(4, 9));
        p.drawLine(QPointF(4, 9), QPointF(8.5, 13.5));
        QPainterPath path(QPointF(4, 9));
        path.lineTo(12, 9);
        path.arcTo(QRectF(8.5, 9, 7, 7), 90, -180);
        path.lineTo(10, 16);
        p.drawPath(path);
    } else if (k == QLatin1String("redo")) {
        // tabler arrow-forward-up: mirrored undo.
        p.drawLine(QPointF(11.5, 4.5), QPointF(16, 9));
        p.drawLine(QPointF(16, 9), QPointF(11.5, 13.5));
        QPainterPath path(QPointF(16, 9));
        path.lineTo(8, 9);
        path.arcTo(QRectF(4.5, 9, 7, 7), 90, 180);
        path.lineTo(10, 16);
        p.drawPath(path);
    }
    return pm;
}

// The tool icons ALWAYS render in this colour, in every state (the button
// background carries the state, never the icon).
const QColor kIconColor(0xcc, 0xcc, 0xcc); // #CCCCCC

// Menu-item icon: a single #cccccc glyph (icons never recolor).
QIcon toolIcon(const char *kind)
{
    return QIcon(toolIconPixmap(kind, kIconColor));
}

// --- Exact Figma tool icons (Floating Toolbar Brush, node 33:110) ----------
// Render the ORIGINAL Figma SVG (its vector art, unmodified) into a 30x30 box
// at the exact Figma icon size, centred, then recolour to #cccccc. Antialiased.
QPixmap figIconPixmap(const QString &svgPath, QSizeF iconSize, bool mirror = false)
{
    constexpr qreal dpr = 2.0;
    constexpr qreal box = 30.0;
    QPixmap pm(QSize(int(box * dpr), int(box * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.save();
    if (mirror) { // horizontal flip about the box centre (redo = undo mirrored)
        p.translate(box, 0);
        p.scale(-1, 1);
    }
    QSvgRenderer r(svgPath);
    r.render(&p, QRectF((box - iconSize.width()) / 2.0, (box - iconSize.height()) / 2.0,
                        iconSize.width(), iconSize.height()));
    p.restore();
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(QRectF(0, 0, box, box), kIconColor);
    p.end();
    return pm;
}

// Plain dashed marquee: a 17x17 rounded-4 dashed rect (1.2px #cccccc) centred
// in the 30x30 box, no cursor arrow (the Selection Shapes popup's Rectangle
// button, Figma node 125:326). Antialiased.
QPixmap selectRectGlyphPixmap()
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(60, 60)); // 30x30 @ 2x
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(kIconColor, 1.2, Qt::CustomDashLine, Qt::FlatCap, Qt::MiterJoin);
    pen.setDashPattern({2.6, 2.0});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(6.5, 6.5, 17.0, 17.0), 4, 4);
    return pm;
}

// Selection Modifier icon (Figma 146:67, updated): render the ORIGINAL SVG at
// its exact node placement inside the 36x36 button — the 24x24 icon frame sits
// at (6,6) and every asset bleeds 0.5px past it (stroke on the 0.5-inset
// marquee), so the SVG's top-left lands at (5.5, 5.5). No recolour: the assets
// carry their literal colours (#cccccc strokes; Inverse also fills #616161).
QPixmap selModIconPixmap(const QString &svgPath, QSizeF svgSize)
{
    constexpr qreal dpr = 2.0, box = 36.0;
    QPixmap pm(QSize(int(box * dpr), int(box * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QSvgRenderer r(svgPath);
    r.render(&p, QRectF(5.5, 5.5, svgSize.width(), svgSize.height()));
    return pm;
}

// Move Modifier icon (Figma 161:39): the asset is the full 36x36 button frame
// with its background stripped (ToolButton paints the state background), so it
// renders 1:1 into the 36 box. Colours are literal (#cccccc strokes).
QPixmap moveModIconPixmap(const QString &svgPath)
{
    constexpr qreal dpr = 2.0, box = 36.0;
    QPixmap pm(QSize(int(box * dpr), int(box * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QSvgRenderer r(svgPath);
    r.render(&p, QRectF(0, 0, box, box));
    return pm;
}

// Selection Modifier "Select All" glyph (Figma 156:43): a 24x24 dashed marquee
// (1px #cccccc, border INSIDE the box like CSS border-box) with a 3x3 grid of
// 4px #d9d9d9 squares at FRAME offsets {4, 10, 16} — the squares are siblings
// of the border in the 24x24 icon frame (grid spans 4..20, perfectly centred),
// NOT children of its padding box. Placed at (6,6) in the 36x36 button box.
QPixmap selectAllGlyphPixmap()
{
    constexpr qreal dpr = 2.0, box = 36.0, ox = 6.0, oy = 6.0;
    constexpr qreal bw = 1.0; // border width, inside the 24 box
    QPixmap pm(QSize(int(box * dpr), int(box * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(kIconColor, bw, Qt::CustomDashLine, Qt::FlatCap, Qt::MiterJoin);
    pen.setDashPattern({3.0, 3.0});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(ox + bw / 2, oy + bw / 2, 24.0 - bw, 24.0 - bw));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xd9, 0xd9, 0xd9));
    const double grid[] = {4.0, 10.0, 16.0}; // frame offsets (Figma), centred
    for (double gy : grid)
        for (double gx : grid)
            p.drawRect(QRectF(ox + gx, oy + gy, 4.0, 4.0));
    return pm;
}

// The Figma "select" glyph (node 107:168): a 17x17 rounded-4 dashed marquee
// (1.2px #cccccc) with the cursor arrow (Figma "Polygon 1", #808080) added at
// the bottom-right, rotated 135 deg. Drawn directly (paint code, no icon file),
// centred/positioned in the 30x30 box. Antialiased.
QPixmap selectGlyphPixmap()
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(60, 60)); // 30x30 @ 2x
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Dashed marquee rect @ (6.5, 6.5), 17x17, radius 4.
    QPen pen(kIconColor, 1.2, Qt::CustomDashLine, Qt::FlatCap, Qt::MiterJoin);
    pen.setDashPattern({2.6, 2.0});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(6.5, 6.5, 17.0, 17.0), 4, 4);
    // Cursor arrow: the exact Figma polygon rendered with its 135-deg rotation.
    static const char kArrowSvg[] =
        "<svg viewBox=\"0 0 21.4155 7.71296\" xmlns=\"http://www.w3.org/2000/svg\">"
        "<path d=\"M10.7078 0L21.4155 7.71296H0L10.7078 0Z\" fill=\"#808080\"/></svg>";
    p.save();
    p.translate(25.379, 28.379); // Figma container centre
    p.rotate(135);
    QSvgRenderer arrow(QByteArray::fromRawData(kArrowSvg, int(sizeof(kArrowSvg) - 1)));
    arrow.render(&p, QRectF(-10.707, -5.142, 21.4155, 7.71296));
    p.restore();
    return pm;
}

QIcon selectGlyphIcon() { return QIcon(selectGlyphPixmap()); }

// Tool button (Figma 33:110 component): 30x30, radius 6, custom-painted so the
// corners are smooth (antialiased) and the background carries the state:
//   Default #212121 · Hover #4C4C4C · Pressed #7C6EF6 · Active(checked) #7C6EF6
// The icon (always #cccccc) is drawn centred on top and never recolours.
class ToolButton : public QPushButton
{
public:
    explicit ToolButton(const QPixmap &icon, QWidget *parent = nullptr)
        : QPushButton(parent), m_icon(icon)
    {
        setFixedSize(30, 30);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true); // hover fires without a button held
    }
    void setIconPixmap(const QPixmap &pm)
    {
        m_icon = pm;
        update();
    }
    // Override the hover / pressed background colours (default: toolbar values;
    // the Selection Shapes popup uses hover #4C4C4C, pressed #737373).
    void setStateColors(const QColor &hover, const QColor &pressed)
    {
        m_hoverBg = hover;
        m_pressedBg = pressed;
        update();
    }

protected:
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
    void mousePressEvent(QMouseEvent *e) override
    {
        QPushButton::mousePressEvent(e);
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        QPushButton::mouseReleaseEvent(e);
        update();
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QColor bg(0x21, 0x21, 0x21); // Default
        if (isDown())
            bg = m_pressedBg; // Pressed
        else if (isChecked())
            bg = QColor(0x7c, 0x6e, 0xf6); // Active tool
        else if (isEnabled() && underMouse())
            bg = m_hoverBg; // Hover
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(0, 0, width(), height()), 6, 6);
        // Icon centred (by its own logical size); dimmed only when disabled.
        p.setOpacity(isEnabled() ? 1.0 : 0.4);
        const qreal dpr = m_icon.devicePixelRatio();
        const QSizeF sz(m_icon.width() / dpr, m_icon.height() / dpr);
        p.drawPixmap(QPointF((width() - sz.width()) / 2.0, (height() - sz.height()) / 2.0),
                     m_icon);
    }

private:
    QPixmap m_icon;
    QColor m_hoverBg{0x4c, 0x4c, 0x4c};   // #4C4C4C
    QColor m_pressedBg{0x7c, 0x6e, 0xf6}; // #7C6EF6
};

// Frameless flyout popup (Figma "Select shapes" 128:374): a Qt::Popup window
// (auto-closes on outside click, grabs input) painting an antialiased rounded-4
// #212121 body. Icon-only buttons are laid out by the caller.
class RoundedPopupFrame : public QWidget
{
public:
    explicit RoundedPopupFrame(QWidget *parent = nullptr)
        // NoDropShadowWindowHint disables the OS drop shadow that Windows adds
        // to popup windows — that native shadow is what left the white/grey
        // artifact at the bottom-right corner of the translucent popup.
        : QWidget(parent,
                  Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground); // no opaque base behind us
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Clear the whole surface to transparent FIRST so the rounded corners
        // (and the bottom-right) never keep a stale/opaque base pixel.
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x21, 0x21, 0x21)); // #212121, radius 4
        p.drawRoundedRect(QRectF(0, 0, width(), height()), 4, 4);
    }
};

// Selection Modifier toolbar body (Figma "Add Select" 146:67): a
// FloatingToolWindow (so the canvas never clips it) painting a translucent
// rgba(33,33,33,0.65) radius-8 body with NO border and NO shadow. NOT
// draggable — it sets no grip widget, so gripRect() is empty and the base
// ignores drags.
class SelModBar : public FloatingToolWindow
{
public:
    SelModBar(QWidget *anchor, QWidget *parent)
        : FloatingToolWindow(anchor, QString(), parent)
    {
        setWindowFlags(windowFlags() | Qt::NoDropShadowWindowHint);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent); // clear -> clean corners, no artifact
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x21, 0x21, 0x21, 166)); // rgba(33,33,33,0.65), r8
        p.drawRoundedRect(QRectF(0, 0, width(), height()), 8, 8);
    }
};

// Like SelModBar, but with a SOLID #212121 body — the Perspective Modifier
// container (Figma 180:121) is full-opacity. Same radius-8, not movable.
class PerspModBar : public FloatingToolWindow
{
public:
    PerspModBar(QWidget *anchor, QWidget *parent)
        : FloatingToolWindow(anchor, QString(), parent)
    {
        setWindowFlags(windowFlags() | Qt::NoDropShadowWindowHint);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent); // clear -> clean corners
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x21, 0x21, 0x21)); // 100% opacity
        p.drawRoundedRect(QRectF(0, 0, width(), height()), 8, 8);
    }
};

// Floating bar body: a FloatingToolWindow (top-level tool window — drag,
// clamp, main-window follow, and persistence come from the base) painting its
// #212121 body + 1px #1a1a1a border with antialiased 12px corners.
class RoundedBar : public FloatingToolWindow
{
public:
    RoundedBar(QWidget *anchor, const QString &settingsKey, QWidget *parent)
        : FloatingToolWindow(anchor, settingsKey, parent)
    {
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const qreal bw = 1.0; // 1px border
        const QRectF r(bw / 2.0, bw / 2.0, width() - bw, height() - bw);
        p.setPen(QPen(QColor(0x1a, 0x1a, 0x1a), bw));
        p.setBrush(QColor(0x21, 0x21, 0x21));
        p.drawRoundedRect(r, 12, 12);
    }
};

// Colour control (Figma node 33:87): a 30x30 rounded-6 box with a 1.5px
// #808080 border and a 24x25 rounded-4 swatch of the current colour, drawn
// INSET so it never bleeds over the border. Custom-painted (antialiased) and
// clipped so the corners stay crisp.
class ColorSwatchButton : public QPushButton
{
public:
    explicit ColorSwatchButton(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setFixedSize(30, 30);
        setCursor(Qt::PointingHandCursor);
    }
    void setSwatchColor(const QColor &c)
    {
        m_color = c;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Border frame (radius 6), inset by half the pen so it sits inside.
        const qreal bw = 1.5;
        const QRectF frame(bw / 2.0, bw / 2.0, width() - bw, height() - bw);
        p.setPen(QPen(underMouse() ? QColor(0xa0, 0xa0, 0xa0) : QColor(0x80, 0x80, 0x80), bw));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 6, 6);
        // Inset swatch (24x25 @ (3, 2.5)), fully inside the frame, rounded 4.
        QPainterPath sw;
        sw.addRoundedRect(QRectF(3.0, 2.5, 24.0, 25.0), 4, 4);
        p.fillPath(sw, m_color);
    }

private:
    QColor m_color{Qt::black};
};

// Six-dot drag grip: 3 columns x 2 rows of small grey dots (horizontal).
QPixmap dragDotsPixmap()
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(40, 28));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x6a, 0x6a, 0x6a));
    for (int row = 0; row < 2; ++row)
        for (int col = 0; col < 3; ++col)
            p.drawEllipse(QPointF(5.0 + col * 5.0, 4.0 + row * 6.0), 1.6, 1.6);
    return pm;
}

// Vertical grip: 2 columns x 3 rows of r=2 dots in a 12x20 box, matching the
// Figma "Grab dots" (for the horizontal Brush toolbar).
QPixmap dragDotsPixmapV()
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(24, 40)); // 12x20 logical @ 2x
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x6a, 0x6a, 0x6a));
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 2; ++col)
            p.drawEllipse(QPointF(2.0 + col * 8.0, 2.0 + row * 8.0), 2.0, 2.0);
    return pm;
}

// Perspective Modifier slider (Figma 180:121): Inter Semi-Bold 9px label
// top-left, 8px value top-right, a 10px-high #333 radius-2 track at y14 with
// a #4B4397->#7C6EF6 gradient fill (white fade on top) reaching the 20x10
// #b3b3b3 radius-1 dragger. The Hue variant paints the full hue spectrum
// across the track (per-VP guide colour) and shows no value text. Plain
// QWidget with an std::function callback — the bar wires it with lambdas.
class PerspSlider : public QWidget
{
public:
    enum Kind { Value, Hue };
    PerspSlider(const QString &label, int min, int max, Kind kind,
                QWidget *parent)
        : QWidget(parent), m_label(label), m_min(min), m_max(max), m_kind(kind)
    {
        setCursor(Qt::PointingHandCursor);
        // Directly on the bar body: no implicit background panel (Figma shows
        // only the track/fill/thumb/label/value on the #212121 toolbar).
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

    std::function<void(int)> onChanged;
    std::function<void()> onBegin; // gesture start (undo snapshot)
    std::function<void()> onEnd;   // gesture end (undo push)

    int value() const { return m_value; }
    void setValue(int v)
    {
        v = qBound(m_min, v, m_max);
        if (v == m_value)
            return;
        m_value = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QFont f(QStringLiteral("Inter"));
        f.setWeight(QFont::DemiBold);
        f.setPixelSize(9);
        p.setFont(f);
        p.setPen(QColor(0xcc, 0xcc, 0xcc));
        if (m_kind == Value) { // the Hue variant is track-only: no label/value
            p.drawText(QRectF(0, 0, width(), 15),
                       Qt::AlignLeft | Qt::AlignVCenter, m_label);
            f.setPixelSize(8);
            p.setFont(f);
            p.drawText(QRectF(width() - 22, 2, 22, 15),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(m_value));
        }

        const QRectF track(0, m_kind == Hue ? 1 : 14, width(), 10);
        QPainterPath tp;
        tp.addRoundedRect(track, 2, 2);
        p.setPen(Qt::NoPen);
        if (m_kind == Hue) {
            QLinearGradient spectrum(track.topLeft(), track.topRight());
            for (int i = 0; i <= 12; ++i)
                spectrum.setColorAt(i / 12.0,
                                    QColor::fromHsvF(qMin(i / 12.0, 0.999), 1.0, 1.0));
            p.fillPath(tp, spectrum);
        } else {
            p.fillPath(tp, QColor(0x33, 0x33, 0x33));
            const qreal cx = centerForValue();
            if (cx > 1.0) {
                const QRectF fill(0, 14, cx, 10);
                QPainterPath fp;
                fp.addRoundedRect(fill, 1, 1);
                QLinearGradient g(fill.topLeft(), fill.topRight());
                g.setColorAt(0.0, QColor(0x4b, 0x43, 0x97));
                g.setColorAt(1.0, QColor(0x7c, 0x6e, 0xf6));
                p.fillPath(fp, g);
                QLinearGradient sheen(fill.topLeft(), fill.bottomLeft());
                sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
                sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
                p.fillPath(fp, sheen);
            }
        }
        QRectF dragger(centerForValue() - 10.0, track.top(), 20, 10);
        dragger.moveLeft(qBound(0.0, dragger.left(), width() - 20.0));
        QPainterPath dp;
        dp.addRoundedRect(dragger, 1, 1);
        p.fillPath(dp, QColor(0xb3, 0xb3, 0xb3));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (onBegin)
            onBegin(); // snapshot BEFORE the first change of the gesture
        setFromX(e->position().x());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            setFromX(e->position().x());
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (onEnd)
            onEnd();
    }

private:
    qreal centerForValue() const
    {
        return (m_value - m_min) * width() / qreal(qMax(1, m_max - m_min));
    }
    void setFromX(qreal x)
    {
        const int v = m_min
            + qRound((m_max - m_min) * qBound(0.0, x / qMax(1, width()), 1.0));
        if (v == m_value)
            return;
        m_value = v;
        update();
        if (onChanged)
            onChanged(m_value);
    }

    QString m_label;
    int m_min = 0;
    int m_max = 100;
    int m_value = 0;
    Kind m_kind = Value;
};

// Support Drawing switch (Figma 180:196/197): a 28x10 radius-5 pill — accent
// #7C6EF6 with the 14x10 #b3b3b3 knob at the right when ON, #4d4d4d with the
// knob at the left when OFF.
class PerspToggle : public QWidget
{
public:
    explicit PerspToggle(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(28, 10);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_TranslucentBackground); // no background panel
        setAutoFillBackground(false);
    }

    std::function<void(bool)> onToggled;

    bool isOn() const { return m_on; }
    void setOn(bool on)
    {
        if (on == m_on)
            return;
        m_on = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        QPainterPath pill;
        pill.addRoundedRect(QRectF(0, 0, 28, 10), 5, 5);
        p.fillPath(pill, m_on ? QColor(0x7c, 0x6e, 0xf6) : QColor(0x4d, 0x4d, 0x4d));
        QPainterPath knob;
        knob.addRoundedRect(QRectF(m_on ? 14.0 : 0.0, 0, 14, 10), 5, 5);
        p.fillPath(knob, QColor(0xb3, 0xb3, 0xb3));
    }
    void mousePressEvent(QMouseEvent *) override
    {
        m_on = !m_on;
        update();
        if (onToggled)
            onToggled(m_on);
    }

private:
    bool m_on = false;
};

// Vertical slider from the Floating Size CTL (Figma 209:42): a 25x220 #333
// radius-4 track whose gradient fill (#4B4397 -> #7C6EF6 with a white sheen
// across the width) grows from the BOTTOM, capped by the 31px #999 radius-4
// dragger riding at the top of the fill.
class SizeCtlSlider : public QWidget
{
public:
    SizeCtlSlider(int min, int max, int value, QWidget *parent)
        : QWidget(parent), m_min(min), m_max(max), m_value(value)
    {
        setFixedSize(25, 220);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void(int)> onChanged;   // value changed by the user
    std::function<void()> onAdjustBegin;  // press: user started adjusting
    std::function<void()> onAdjustEnd;    // release: user stopped adjusting

    int minimum() const { return m_min; }
    int maximum() const { return m_max; }
    int value() const { return m_value; }
    void setValue(int v) // programmatic sync: no callback
    {
        v = qBound(m_min, v, m_max);
        if (v == m_value)
            return;
        m_value = v;
        update();
    }

    // Saved preset ticks (Procreate-style). Kept sorted + unique.
    const QVector<int> &presets() const { return m_presets; }
    void setPresets(QVector<int> presets)
    {
        for (int &v : presets)
            v = qBound(m_min, v, m_max);
        std::sort(presets.begin(), presets.end());
        presets.erase(std::unique(presets.begin(), presets.end()),
                      presets.end());
        m_presets = presets;
        update();
    }
    bool hasPreset(int v) const { return m_presets.contains(v); }
    void addPreset(int v)
    {
        v = qBound(m_min, v, m_max);
        if (!m_presets.contains(v)) {
            m_presets.append(v);
            std::sort(m_presets.begin(), m_presets.end());
            update();
        }
    }
    void removePreset(int v)
    {
        if (m_presets.removeAll(v) > 0)
            update();
    }

    // Top edge (y) of the 31px handle for value v. The handle travels the
    // FULL track: at min it sits hard against the bottom end, at max against
    // the top, proportional in between — the handle and the value can never
    // desynchronise (no dead zone at either extreme).
    qreal yForValue(int v) const
    {
        const qreal frac = (v - m_min) / qreal(qMax(1, m_max - m_min));
        return (1.0 - frac) * qMax(1, height() - 31);
    }
    // Visual y of the preset marker for value v: the centre of the handle
    // when the slider sits exactly on v, so a snapped marker shows perfectly
    // centred inside the handle. Always within the track (15.5..height-15.5).
    qreal markerY(int v) const
    {
        return yForValue(v) + 15.5;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath track;
        track.addRoundedRect(rect(), 4, 4);
        p.fillPath(track, QColor(0x33, 0x33, 0x33));

        // Fill from the handle's top edge down to the track bottom; the
        // handle spans the full track (yForValue maps min -> bottom end,
        // max -> top end), so fill height is at least the 31px handle.
        const qreal top = yForValue(m_value);
        const QRectF fill(0, top, width(), height() - top);
        QPainterPath fillPath;
        fillPath.addRoundedRect(fill, 4, 4);
        QLinearGradient g(fill.bottomLeft(), fill.topLeft());
        g.setColorAt(0.0, QColor(0x4b, 0x43, 0x97));
        g.setColorAt(1.0, QColor(0x7c, 0x6e, 0xf6));
        p.fillPath(fillPath, g);
        QLinearGradient sheen(fill.topLeft(), fill.topRight());
        sheen.setColorAt(0.0, QColor(255, 255, 255, 26));
        sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillPath(fillPath, sheen);

        QPainterPath dragger;
        dragger.addRoundedRect(QRectF(0, fill.top(), width(), 31), 4, 4);
        p.fillPath(dragger, QColor(0x99, 0x99, 0x99));

        // Saved preset markers (snap points): horizontal bars centred on the
        // track, Sanko accent with a 1px white outline so they read against
        // the track, the gradient fill AND the light dragger. Drawn last so a
        // snapped marker shows centred inside the handle.
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(QColor(0x7c, 0x6e, 0xf6));
        for (int v : m_presets)
            p.drawRoundedRect(QRectF(4.5, markerY(v) - 2.0, width() - 9, 4),
                              2, 2);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (onAdjustBegin)
            onAdjustBegin();
        // Tapping on (or very near) a saved tick snaps to its exact value.
        const int snapV = presetNearY(e->position().y());
        if (snapV >= 0)
            setToValue(snapV);
        else
            setFromY(e->position().y());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            setFromY(e->position().y());
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (onAdjustEnd)
            onAdjustEnd();
    }

private:
    void setToValue(int v)
    {
        v = qBound(m_min, v, m_max);
        if (v == m_value)
            return;
        m_value = v;
        update();
        if (onChanged)
            onChanged(m_value);
    }
    void setFromY(qreal y)
    {
        // Handle-CENTRE mapping over the handle's travel range: the cursor
        // drags the middle of the 31px handle, min lands the handle on the
        // exact bottom end, max on the exact top — every track position maps
        // to a value and vice versa, no dead zone.
        const qreal travel = qMax(1, height() - 31);
        const qreal frac = 1.0 - qBound(0.0, (y - 15.5) / travel, 1.0);
        const int raw = m_min + qRound(frac * (m_max - m_min));
        // Magnet: every saved preset is a snap point. When the handle centre
        // comes within kSnapPx of a marker the value snaps to that preset,
        // landing the marker exactly centred inside the handle (both sit
        // 15.5px below yForValue, so the comparison stays in track space).
        int v = raw;
        qreal bestD = kSnapPx;
        for (int pv : m_presets) {
            const qreal d = qAbs(yForValue(raw) - yForValue(pv));
            if (d <= bestD) {
                bestD = d;
                v = pv;
            }
        }
        setToValue(v);
    }
    // The saved preset whose visible marker is within 6px of y, or -1.
    int presetNearY(qreal y) const
    {
        int best = -1;
        qreal bestD = kSnapPx;
        for (int v : m_presets) {
            const qreal d = qAbs(markerY(v) - y);
            if (d <= bestD) {
                bestD = d;
                best = v;
            }
        }
        return best;
    }

    static constexpr qreal kSnapPx = 6.0; // magnet radius, in track pixels

    int m_min = 0;
    int m_max = 100;
    int m_value = 0;
    QVector<int> m_presets;
};

// Floating Size CTL window (Figma 209:42 + grab 209:53): the 46x574 #212121
// radius-12 bar plus the 8x50 grab pill, separated by a 4px transparent gap
// inside one top-level FloatingToolWindow. The grab appears only while the
// cursor hovers the toolbar and is the only drag region; on release the bar
// snaps to the nearest canvas side (left-handed / right-handed layouts) and
// the grab flips to the INNER side. Side + vertical position persist in
// QSettings and are re-derived on every reposition, so the snap tracks
// window resizes (the base's user-placed offset persistence is bypassed).
class SizeCtlBar : public FloatingToolWindow
{
public:
    static constexpr int kBarW = 46;
    static constexpr int kBarH = 574;
    static constexpr int kGrabW = 8;
    static constexpr int kGrabH = 50;
    static constexpr int kGap = 4;
    static constexpr int kEdgeMargin = 10;

    SizeCtlBar(QWidget *anchor, QWidget *parent)
        : FloatingToolWindow(anchor, QString(), parent)
    {
        setFixedSize(kBarW + kGap + kGrabW, kBarH);
        setMouseTracking(true); // hover shows the grab without a button held
        const QSettings settings(QStringLiteral("SankoTV"),
                                 QStringLiteral("SankoTV"));
        m_side = settings.value(QStringLiteral("storyboard/sizeCtlSide"), 0)
                         .toInt() == 1 ? 1 : 0;
        m_sideY = settings.value(QStringLiteral("storyboard/sizeCtlY"), -1)
                          .toInt();
        setDefaultOffsetProvider([this] { return snappedOffset(); });
    }

    std::function<void()> onSideChanged; // relayout the child controls

    int side() const { return m_side; }
    int barX() const { return m_side == 0 ? 0 : kGrabW + kGap; }
    bool grabVisible() const { return m_grabVisible; }

protected:
    // Draggable only while the grab is shown (hover).
    QRect gripRect() const override
    {
        return m_grabVisible ? grabArea() : QRect();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent); // clean gap + corners
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x21, 0x21, 0x21));
        p.drawRoundedRect(QRectF(barX(), 0, kBarW, kBarH), 12, 12);
        // The grab shows only on hover; when it does it is armed to drag, so
        // it wears the Sanko accent colour (Figma 209:53).
        if (m_grabVisible) {
            p.setBrush(QColor(0x7c, 0x6e, 0xf6));
            p.drawRoundedRect(QRectF(grabArea()), 4, 4);
        }
    }

    void enterEvent(QEnterEvent *) override { setGrabVisible(true); }
    void leaveEvent(QEvent *) override
    {
        // Moving onto a child keeps the cursor inside our rect; only a real
        // exit hides the grab.
        if (!rect().contains(mapFromGlobal(QCursor::pos())))
            setGrabVisible(false);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton
            && gripRect().contains(event->position().toPoint())) {
            m_dragActive = true;
            m_dragStartGlobal = event->globalPosition().toPoint();
            m_dragStartPos = pos();
            setCursor(Qt::ClosedHandCursor);
            return;
        }
        QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragActive && (event->buttons() & Qt::LeftButton)) {
            move(clampToAnchor(m_dragStartPos
                               + (event->globalPosition().toPoint()
                                  - m_dragStartGlobal)));
            return;
        }
        setCursor(gripRect().contains(event->position().toPoint())
                      ? Qt::OpenHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragActive) {
            m_dragActive = false;
            setCursor(Qt::OpenHandCursor);
            snapToNearestSide();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    QRect grabArea() const
    {
        const int gx = m_side == 0 ? kBarW + kGap : 0;
        return QRect(gx, (kBarH - kGrabH) / 2, kGrabW, kGrabH);
    }
    void setGrabVisible(bool visible)
    {
        if (m_grabVisible == visible)
            return;
        m_grabVisible = visible;
        update();
    }
    QPoint snappedOffset() const
    {
        QWidget *anchor = anchorWidget();
        if (!anchor)
            return {};
        const int x = m_side == 0
            ? kEdgeMargin
            : qMax(0, anchor->width() - width() - kEdgeMargin);
        int y = m_sideY >= 0 ? m_sideY
                             : qMax(0, (anchor->height() - height()) / 2);
        y = qBound(0, y, qMax(0, anchor->height() - height()));
        return QPoint(x, y);
    }
    QPoint clampToAnchor(const QPoint &globalPos) const
    {
        QWidget *anchor = anchorWidget();
        if (!anchor)
            return globalPos;
        const QPoint origin = anchor->mapToGlobal(QPoint(0, 0));
        const int maxX = origin.x() + qMax(0, anchor->width() - width());
        const int maxY = origin.y() + qMax(0, anchor->height() - height());
        return QPoint(qBound(origin.x(), globalPos.x(), maxX),
                      qBound(origin.y(), globalPos.y(), maxY));
    }
    void snapToNearestSide()
    {
        QWidget *anchor = anchorWidget();
        if (!anchor)
            return;
        const QPoint rel = pos() - anchor->mapToGlobal(QPoint(0, 0));
        const int newSide =
            rel.x() + width() / 2 <= anchor->width() / 2 ? 0 : 1;
        m_sideY = qBound(0, rel.y(), qMax(0, anchor->height() - height()));
        if (newSide != m_side) {
            m_side = newSide;
            if (onSideChanged)
                onSideChanged(); // the grab flips to the inner side
            update();
        }
        QSettings settings(QStringLiteral("SankoTV"),
                           QStringLiteral("SankoTV"));
        settings.setValue(QStringLiteral("storyboard/sizeCtlSide"), m_side);
        settings.setValue(QStringLiteral("storyboard/sizeCtlY"), m_sideY);
        reposition(); // never user-placed: always lands on snappedOffset()
        // The bar just jumped away from the cursor: the grab is hover-only,
        // so hide it immediately (the platform Leave event can lag a
        // programmatic move).
        setGrabVisible(false);
    }

    int m_side = 0;   // 0 = left canvas edge, 1 = right
    int m_sideY = -1; // canvas-relative y (-1: centre until first drag)
    bool m_grabVisible = false;
    bool m_dragActive = false;
    QPoint m_dragStartGlobal;
    QPoint m_dragStartPos;
};

// Numeric value + preset pill (Figma 213:94 / 213:99): a 64x27 translucent
// #212121 @ 50% rounded pill that floats next to the slider being adjusted.
// It shows the live value and a preset button that is "+" (save the current
// value as a tick) or "-" (the current value already has a tick: remove it).
// A top-level tool window so it is never clipped by the narrow Size CTL bar;
// WA_ShowWithoutActivating keeps focus (and any active brush stroke) intact.
class PresetPill : public QWidget
{
public:
    explicit PresetPill(QWidget *parent)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                              | Qt::WindowStaysOnTopHint
                              | Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(64, 27);

        m_value = new QLabel(this);
        m_value->setGeometry(9, 0, 28, 27);
        m_value->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_value->setStyleSheet(QStringLiteral(
            "color:#ffffff; font-size:12px; font-weight:600;"
            " background:transparent; border:none;"));

        m_button = new QToolButton(this);
        m_button->setGeometry(38, 3, 21, 21);
        m_button->setCursor(Qt::PointingHandCursor);
        m_button->setFocusPolicy(Qt::NoFocus);
        m_button->setStyleSheet(QStringLiteral(
            "QToolButton { color:#ffffff; font-size:15px; font-weight:600;"
            " background:#3a3a3a; border:none; border-radius:5px;"
            " padding-bottom:2px; }"
            "QToolButton:hover { background:#7c6ef6; }"));
        connect(m_button, &QToolButton::clicked, this, [this] {
            if (onPresetToggle)
                onPresetToggle();
        });
    }

    std::function<void()> onPresetToggle;
    std::function<void(bool)> onHoverChange;

    SizeCtlSlider *active = nullptr; // the slider currently being adjusted

    void setValue(int v) { m_value->setText(QString::number(v)); }
    // "-" when the current value already sits on a saved tick, else "+".
    void setHasPreset(bool has)
    {
        m_button->setText(has ? QString::fromUtf8("\xE2\x88\x92")   // U+2212
                              : QStringLiteral("+"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x21, 0x21, 0x21, 128)); // #212121 @ 50%
        p.drawRoundedRect(rect(), 6, 6);
    }
    void enterEvent(QEnterEvent *) override
    {
        if (onHoverChange)
            onHoverChange(true);
    }
    void leaveEvent(QEvent *) override
    {
        if (onHoverChange)
            onHoverChange(false);
    }

private:
    QLabel *m_value = nullptr;
    QToolButton *m_button = nullptr;
};

// Per-tool (Brush / Eraser) Size CTL state: each tool keeps its own size,
// opacity and saved preset ticks. The sliders show and edit whichever tool
// is active; everything persists per tool in QSettings.
struct SizeCtlTool
{
    int size = 25;      // 1..200 canvas px
    int opacity = 100;  // 5..100 %
    QVector<int> sizeTicks;
    QVector<int> opacityTicks;
};
struct SizeCtlToolState
{
    SizeCtlTool brush, eraser;
    bool eraserMode = false; // sliders currently showing the Eraser's state
    SizeCtlTool &active() { return eraserMode ? eraser : brush; }
};


// --- Layers panel widgets (Figma 7-70) ---------------------------------------

// Mime type carried by layer-row drags: comma-separated ascending model
// indices of the dragged (selected) layers.
const char *kLayersMime = "application/x-sankotv-layers";

// Renders one of the layer-panel SVG icons at its native proportions into a
// pixmap of the given LOGICAL size (2x device pixels), optionally tinted
// (SourceIn) and faded — the panel's states (hidden eye, unlocked padlock)
// are all one icon at different tint/opacity.
QPixmap layerIconPm(const QString &svgPath, QSizeF size,
                    const QColor &tint = QColor(), qreal opacity = 1.0)
{
    constexpr qreal dpr = 2.0;
    QPixmap pm(QSize(int(size.width() * dpr), int(size.height() * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QSvgRenderer renderer(svgPath);
    renderer.render(&p, QRectF(0, 0, size.width(), size.height()));
    if (tint.isValid()) {
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(QRectF(0, 0, size.width(), size.height()), tint);
    }
    p.end();
    if (opacity >= 1.0)
        return pm;
    QPixmap faded(pm.size());
    faded.setDevicePixelRatio(dpr);
    faded.fill(Qt::transparent);
    QPainter fp(&faded);
    fp.setOpacity(opacity);
    fp.drawPixmap(0, 0, pm);
    fp.end();
    return faded;
}

// One layer row: Photoshop-style selection on PRESS (immediately for a fresh
// row or with modifiers; deferred for a press inside the current selection,
// so a drag keeps the multi-selection and a plain release collapses to the
// single pressed row), drag-out past QApplication::startDragDistance(),
// double-click = inline rename, right-click = context menu.
class LayerRowFrame : public QFrame
{
public:
    LayerRowFrame(int index, QWidget *parent) : QFrame(parent), m_index(index)
    {
        setObjectName(QStringLiteral("layerRow"));
        setFixedHeight(32);
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void(int, Qt::KeyboardModifiers)> onPressed;
    std::function<void(int)> onCollapseToSingle;
    std::function<void(int)> onDoubleClicked;
    std::function<void(int, const QPoint &)> onContext;
    std::function<void(int)> onDragOut;
    bool selected = false; // set at rebuild; press consults it

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_press = event->globalPosition().toPoint();
            m_pressActive = true;
            m_deferCollapse = selected && event->modifiers() == Qt::NoModifier;
            if (!m_deferCollapse && onPressed)
                onPressed(m_index, event->modifiers());
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_pressActive && (event->buttons() & Qt::LeftButton)
            && (event->globalPosition().toPoint() - m_press).manhattanLength()
                   >= QApplication::startDragDistance()) {
            m_pressActive = false;
            if (onDragOut)
                onDragOut(m_index); // blocking QDrag::exec inside
            return;
        }
        QFrame::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_pressActive) {
            m_pressActive = false;
            if (m_deferCollapse && onCollapseToSingle)
                onCollapseToSingle(m_index);
        }
        QFrame::mouseReleaseEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onDoubleClicked) {
            m_pressActive = false;
            onDoubleClicked(m_index);
            event->accept();
            return;
        }
        QFrame::mouseDoubleClickEvent(event);
    }
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (onContext)
            onContext(m_index, event->globalPos());
        event->accept();
    }

private:
    int m_index;
    QPoint m_press;
    bool m_pressActive = false;
    bool m_deferCollapse = false;
};

// The layer-list container: accepts row drags and paints a thin accent
// insertion line in the gap the drop would land in. Gaps are derived from
// the child rows' live geometry (0 = above the top row).
class LayerListHost : public QWidget
{
public:
    explicit LayerListHost(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAcceptDrops(true);
    }
    std::function<void(int)> onDropAtGap; // VISUAL gap index

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasFormat(QString::fromLatin1(kLayersMime))) {
            m_gap = gapAt(event->position().toPoint());
            update();
            event->acceptProposedAction();
        }
    }
    void dragMoveEvent(QDragMoveEvent *event) override
    {
        m_gap = gapAt(event->position().toPoint());
        update();
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_gap = -1;
        update();
    }
    void dropEvent(QDropEvent *event) override
    {
        const int gap = m_gap;
        m_gap = -1;
        update();
        if (gap >= 0 && onDropAtGap)
            onDropAtGap(gap);
        event->acceptProposedAction();
    }
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        if (m_gap < 0)
            return;
        const QVector<QWidget *> r = rows();
        if (r.isEmpty())
            return;
        int y = 0;
        if (m_gap <= 0)
            y = qMax(1, r.first()->y() - 2);
        else if (m_gap >= r.size())
            y = r.last()->geometry().bottom() + 3;
        else
            y = (r.at(m_gap - 1)->geometry().bottom() + r.at(m_gap)->y()) / 2;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(0x7c, 0x6e, 0xf6), 2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(4, y, width() - 4, y);
    }

private:
    QVector<QWidget *> rows() const
    {
        // Direct children in visual (top -> bottom) order.
        QVector<QWidget *> out;
        for (QObject *child : children())
            if (auto *w = qobject_cast<QFrame *>(child))
                if (w->objectName() == QLatin1String("layerRow") && w->isVisible())
                    out.append(w);
        std::sort(out.begin(), out.end(),
                  [](QWidget *a, QWidget *b) { return a->y() < b->y(); });
        return out;
    }
    int gapAt(const QPoint &pos) const
    {
        const QVector<QWidget *> r = rows();
        for (int i = 0; i < r.size(); ++i)
            if (pos.y() < r.at(i)->geometry().center().y())
                return i;
        return r.size();
    }
    int m_gap = -1;
};

// Footer icon button (Figma Layers Footer Toolbar). Delete and Duplicate
// double as DROP TARGETS for layer-row drags.
class LayerToolButton : public QPushButton
{
public:
    LayerToolButton(const QString &svgPath, QSizeF iconSize,
                    const QString &tip, QWidget *parent)
        : QPushButton(parent)
    {
        setFixedSize(22, 18); // Figma footer button frame
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(tip);
        // Disabled state (e.g. Background layer selected): the glyph fades
        // to gray — the flat button has no frame to gray. Fading (not a
        // solid tint) keeps the glyph silhouette readable.
        QIcon icon(layerIconPm(svgPath, iconSize));
        icon.addPixmap(layerIconPm(svgPath, iconSize, QColor(), 0.35),
                       QIcon::Disabled);
        setIcon(icon);
        setIconSize(iconSize.toSize());
        // Figma shows only the default (flat) footer button; hover/pressed
        // are not defined there, so a minimal rounded highlight is used.
        setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none;"
            " border-radius: 4px; padding: 0; }"
            "QPushButton:hover { background: #262626; }"
            "QPushButton:pressed { background: #2e2e2e; }"
            "QPushButton:disabled { background: transparent; }"));
    }

    std::function<void()> onDropLayers; // set -> becomes a drop target
    void enableLayerDrops() { setAcceptDrops(true); }

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (onDropLayers
            && event->mimeData()->hasFormat(QString::fromLatin1(kLayersMime))) {
            setDown(true);
            event->acceptProposedAction();
        }
    }
    void dragLeaveEvent(QDragLeaveEvent *) override { setDown(false); }
    void dropEvent(QDropEvent *event) override
    {
        setDown(false);
        if (onDropLayers)
            onDropLayers();
        event->acceptProposedAction();
    }
};

// Figma 7:92 Opacity control: a 6px #333 track (radius 1) with a
// left-anchored purple gradient fill (269.99deg #4B4397 -> #7C6EF6 plus a top
// white sheen) and a flat 20x6 #B3B3B3 dragger (radius 1) at the fill's
// leading edge. The "NN%" value is a SEPARATE row label (Figma 7:96), not
// painted here. The track expands to fill the row width (Figma is a fixed
// 166px inside the fixed 280px dock; the dock here is resizable).
class LayerOpacitySlider : public QWidget
{
public:
    explicit LayerOpacitySlider(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(24); // Figma Opacity row height (also the hit area)
        setMinimumWidth(60);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    std::function<void(int)> onChanged;
    std::function<void()> onDragStarted;  // undo snapshot boundary
    std::function<void()> onDragFinished; // undo push boundary
    int value() const { return m_value; }
    void setValueSilent(int v)
    {
        v = qBound(0, v, 100);
        if (v != m_value) {
            m_value = v;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal trackW = width();
        const QRectF track(0, (height() - 6) / 2.0, trackW, 6);
        QPainterPath tp;
        tp.addRoundedRect(track, 1, 1);
        p.fillPath(tp, QColor(0x33, 0x33, 0x33));

        const qreal fillW = m_value / 100.0 * trackW;
        if (fillW > 0.5) {
            const QRectF fill(track.left(), track.top(), fillW, track.height());
            QPainterPath fp;
            fp.addRoundedRect(fill, 1, 1);
            QLinearGradient g(fill.left(), 0, fill.right(), 0);
            g.setColorAt(0.0, QColor(0x4b, 0x43, 0x97));
            g.setColorAt(1.0, QColor(0x7c, 0x6e, 0xf6));
            p.fillPath(fp, g);
            QLinearGradient sheen(0, fill.top(), 0, fill.bottom());
            sheen.setColorAt(0.0, QColor(255, 255, 255, 26)); // 0.1 alpha
            sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.fillPath(fp, sheen);
        }
        // 20x6 dragger at the fill's leading edge (Figma 16:112).
        const qreal dragLeft = qBound(0.0, fillW - 18.0, trackW - 20.0);
        QPainterPath dp;
        dp.addRoundedRect(QRectF(dragLeft, track.top(), 20, 6), 1, 1);
        p.fillPath(dp, QColor(0xb3, 0xb3, 0xb3));
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (onDragStarted)
            onDragStarted();
        setFromX(e->position().x());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton)
            setFromX(e->position().x());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        Q_UNUSED(e);
        if (onDragFinished)
            onDragFinished();
    }

private:
    void setFromX(qreal x)
    {
        const int v = qBound(0, qRound(x / qMax<qreal>(1, width()) * 100.0), 100);
        if (v == m_value)
            return;
        m_value = v;
        update();
        if (onChanged)
            onChanged(v);
    }
    int m_value = 100;
};

// True when the layer carries any artwork the user could lose: imported
// images always count; raster layers count once any pixel has alpha. The
// Background (paper) and group folders never block the empty-delete skip.
bool layerHasContent(const Layer &layer)
{
    if (isGroupLayer(layer))
        return false;
    if (layer.type == QLatin1String("image"))
        return true;
    const QImage &img = layer.image;
    if (img.isNull())
        return false;
    if (layer.type == QLatin1String("background"))
        return false; // plain paper — deleting it is blocked elsewhere anyway
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(line[x]) != 0)
                return true;
    }
    return false;
}

// One layer-STACK edit (add / delete / duplicate / merge / group / reorder /
// rename / colour / visibility / lock / opacity): before/after snapshots of
// the whole vector + active index. QImage's implicit sharing makes the
// snapshots cheap; pixel edits still go through DrawingCommand, so stroke
// data is never forked. The first redo() is skipped because the edit has
// already been applied interactively when the command is pushed.
class LayerStackCommand : public QUndoCommand
{
public:
    LayerStackCommand(StoryboardPage *page, Panel *panel,
                      QVector<Layer> before, int beforeActive,
                      QVector<Layer> after, int afterActive,
                      const QString &text)
        : QUndoCommand(text), m_page(page), m_panel(panel),
          m_before(std::move(before)), m_beforeActive(beforeActive),
          m_after(std::move(after)), m_afterActive(afterActive)
    {
    }
    void redo() override
    {
        if (m_skipFirstRedo) {
            m_skipFirstRedo = false;
            return;
        }
        m_page->applyLayerStackForUndo(m_panel, m_after, m_afterActive);
    }
    void undo() override
    {
        m_page->applyLayerStackForUndo(m_panel, m_before, m_beforeActive);
    }

private:
    StoryboardPage *m_page;
    Panel *m_panel;
    QVector<Layer> m_before;
    int m_beforeActive;
    QVector<Layer> m_after;
    int m_afterActive;
    bool m_skipFirstRedo = true;
};

// --- App-wide undo commands for the panel list --------------------------------
// Insert covers Add / Duplicate / Paste; ownership of the DETACHED Panel
// follows the undo state, so a truncated history never leaks or double-frees.
class InsertPanelCommand : public QUndoCommand
{
public:
    InsertPanelCommand(StoryboardPage *page, Scene *scene, int index,
                       Panel *panel, const QString &text)
        : QUndoCommand(text), m_page(page), m_scene(scene), m_index(index),
          m_panel(panel)
    {
    }
    ~InsertPanelCommand() override
    {
        if (m_owns)
            delete m_panel;
    }
    void redo() override
    {
        m_page->applyPanelInsertForUndo(m_scene, m_index, m_panel);
        m_owns = false;
    }
    void undo() override
    {
        m_page->applyPanelRemoveForUndo(m_scene, m_index);
        m_owns = true;
    }

private:
    StoryboardPage *m_page;
    Scene *m_scene;
    int m_index;
    Panel *m_panel;
    bool m_owns = true;
};

// Delete / Cut a panel. The command captures the panel and owns it while it
// is detached from the scene.
class RemovePanelCommand : public QUndoCommand
{
public:
    RemovePanelCommand(StoryboardPage *page, Scene *scene, int index,
                       const QString &text)
        : QUndoCommand(text), m_page(page), m_scene(scene), m_index(index),
          m_panel(scene->panels.at(index))
    {
    }
    ~RemovePanelCommand() override
    {
        if (m_owns)
            delete m_panel;
    }
    void redo() override
    {
        m_page->applyPanelRemoveForUndo(m_scene, m_index);
        m_owns = true;
    }
    void undo() override
    {
        m_page->applyPanelInsertForUndo(m_scene, m_index, m_panel);
        m_owns = false;
    }

private:
    StoryboardPage *m_page;
    Scene *m_scene;
    int m_index;
    Panel *m_panel;
    bool m_owns = false;
};

// Reorder a panel within its scene (both indices are FINAL positions).
class MovePanelCommand : public QUndoCommand
{
public:
    MovePanelCommand(StoryboardPage *page, Scene *scene, int from, int to)
        : QUndoCommand(QStringLiteral("Reorder Panel")), m_page(page),
          m_scene(scene), m_from(from), m_to(to)
    {
    }
    void redo() override { m_page->applyPanelMoveForUndo(m_scene, m_from, m_to); }
    void undo() override { m_page->applyPanelMoveForUndo(m_scene, m_to, m_from); }

private:
    StoryboardPage *m_page;
    Scene *m_scene;
    int m_from;
    int m_to;
};

} // namespace

// ONE reusable tooltip for the Floating Brush Toolbar. Every tooltip appears
// BELOW its button with a constant 4px gap to the toolbar's bottom edge —
// always OUTSIDE the bar, matching the Color/Undo/Redo placement — and never
// covers a button. A short hover delay plus a fade-in keeps the appearance
// smooth; when the pointer chains from one button to the next, the SAME
// widget just moves and re-texts (no hide/show cycle, so no flicker).
class SankoTipPopup : public QWidget
{
public:
    explicit SankoTipPopup(QWidget *parent = nullptr)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint
                              | Qt::NoDropShadowWindowHint)
    {
        setObjectName(QStringLiteral("sankoToolbarTip"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_label = new QLabel(this);
        m_label->setTextFormat(Qt::RichText);
        m_label->setStyleSheet(QStringLiteral(
            "color: #cccccc; font-size: 11px; background: transparent;"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(10, 6, 10, 6);
        layout->addWidget(m_label);

        m_showTimer.setSingleShot(true);
        m_showTimer.setInterval(350); // brief, stable hover delay
        connect(&m_showTimer, &QTimer::timeout, this, [this] { reveal(); });
        m_hideTimer.setSingleShot(true);
        m_hideTimer.setInterval(120); // survives the gap between buttons
        connect(&m_hideTimer, &QTimer::timeout, this, [this] { hide(); });
        m_fade = new QPropertyAnimation(this, "windowOpacity", this);
        m_fade->setDuration(120);
    }

    // Pointer entered `button` (a child of `bar`).
    void scheduleShow(QWidget *button, QWidget *bar)
    {
        m_button = button;
        m_bar = bar;
        m_hideTimer.stop();
        if (isVisible())
            reveal(); // chain-hover: retarget instantly, stay fully opaque
        else if (!m_showTimer.isActive())
            m_showTimer.start();
    }
    // Pointer left a button; cancelled if another button is entered promptly.
    void scheduleHide()
    {
        m_showTimer.stop();
        if (isVisible())
            m_hideTimer.start();
    }
    void hideNow()
    {
        m_showTimer.stop();
        m_hideTimer.stop();
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(0x2a, 0x2a, 0x2a), 1));
        p.setBrush(QColor(0x1a, 0x1a, 0x1a));
        p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 6, 6);
    }

private:
    void reveal()
    {
        if (!m_button || !m_bar || !m_bar->isVisible())
            return;
        m_label->setText(m_button->toolTip());
        adjustSize();
        // Below the TOOLBAR with a constant 4px gap (frame bottom edge is
        // inclusive, hence the +1), centred on the button, kept on-screen.
        const QRect barRect(m_bar->mapToGlobal(QPoint(0, 0)), m_bar->size());
        const int centreX =
            m_button->mapToGlobal(QPoint(m_button->width() / 2, 0)).x();
        int x = centreX - width() / 2;
        if (const QScreen *screen = m_bar->screen()) {
            const QRect avail = screen->availableGeometry();
            x = qBound(avail.left() + 4, x, avail.right() - width() - 3);
        }
        const bool wasVisible = isVisible();
        move(x, barRect.bottom() + 1 + 4);
        if (wasVisible) {
            setWindowOpacity(1.0); // steady while retargeting
            update();
        } else {
            m_fade->stop();
            setWindowOpacity(0.0);
            show();
            m_fade->setStartValue(0.0);
            m_fade->setEndValue(1.0);
            m_fade->start();
        }
    }

    QLabel *m_label = nullptr;
    QPropertyAnimation *m_fade = nullptr;
    QTimer m_showTimer;
    QTimer m_hideTimer;
    QWidget *m_button = nullptr;
    QWidget *m_bar = nullptr;
};


StoryboardPage::StoryboardPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Build the panel widgets in the original order (their signal wiring is
    // established here and must survive the re-parenting into docks below).
    QWidget *scenesPanel = createLeftColumn();
    QWidget *centerColumn = createCenterColumn();
    QWidget *layersPanel = createLayerPanel();
    QWidget *shotInfoPanel = createRightColumn();
    QWidget *bottomBar = createBottomBar();
    m_bottomBar = bottomBar; // the SelMod toolbar keeps a 10px gap above it

    // Native docking host (replaces the ADS manager): an embedded
    // child-widget QMainWindow provides the stock QDockWidget engine; the
    // reusable DockController layers on custom title bars, the animated
    // accent drop preview, tab/split drops, collapse, and persistence. The
    // host fills the page edge to edge (root margins are 0).
    m_dockHost = new QMainWindow;
    m_dockHost->setObjectName(QStringLiteral("storyboardDockHost"));
    m_dockHost->setWindowFlags(Qt::Widget); // child widget, never a window
    m_dockHost->setStyleSheet(QString::fromLatin1(kDockDarkStyle));
    root->addWidget(m_dockHost, 1);

    // Central workspace: the canvas area exactly as before (tool column,
    // brush settings, panel strip, canvas) plus the bottom toolbar — the
    // host's central widget, never a dock, never closable.
    QWidget *central = new QWidget;
    QVBoxLayout *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(centerColumn, 1);
    centralLayout->addWidget(bottomBar);
    m_dockHost->setCentralWidget(central);

    // The controller persists under the app's own QSettings names in a NEW
    // native-docking namespace. The old ADS blob (storyboard/dockState) is a
    // different format and is deliberately never read or overwritten.
    m_dockController = new DockController(
        m_dockHost, QStringLiteral("SankoTV"), QStringLiteral("SankoTV"),
        QStringLiteral("storyboard/nativeDock"), this);

    // Dock widgets wrap the EXISTING panel instances (never recreated, so
    // all constructor-time connections keep firing). saveState() keys saved
    // layouts on the objectName, which must be unique and stable.
    m_layersDock = m_dockController->addPanel(
        layersPanel, QStringLiteral("Layers"), QStringLiteral("dockLayers"));
    m_scenesDock = m_dockController->addPanel(
        scenesPanel, QStringLiteral("Scenes"), QStringLiteral("dockScenes"));
    m_shotInfoDock = m_dockController->addPanel(
        shotInfoPanel, QStringLiteral("Shot Info"),
        QStringLiteral("dockShotInfo"));

    // Default layout: Layers docked right; Scenes + Shot Info tabbed below
    // it, Scenes tab in front. Registered with the controller so Reset
    // Layout / failed restores reproduce it exactly.
    m_dockController->setDefaultLayout([this] {
        m_dockHost->addDockWidget(Qt::RightDockWidgetArea, m_layersDock);
        m_dockHost->addDockWidget(Qt::RightDockWidgetArea, m_scenesDock);
        m_dockHost->splitDockWidget(m_layersDock, m_scenesDock, Qt::Vertical);
        m_dockHost->tabifyDockWidget(m_scenesDock, m_shotInfoDock);
        m_scenesDock->raise();
        // Layers over the tabbed pair; canvas keeps the remaining width.
        m_dockHost->resizeDocks({m_layersDock, m_scenesDock}, {300, 380},
                                Qt::Vertical);
        m_dockHost->resizeDocks({m_layersDock}, {260}, Qt::Horizontal);
    });

    // Establish the default, then let a valid saved layout replace it. A
    // failed restore (nothing saved yet, or a rejected/corrupt blob — the
    // controller checks restoreState()'s return) keeps the default.
    m_dockController->resetLayout();
    m_dockController->restoreLayout();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this] { saveDockState(); });

    // The app's menu bar exists only after this page lands in MainWindow's
    // stack, so hook the View menu once the event loop starts.
    QTimer::singleShot(0, this, [this] { installDockViewActions(); });

    // 'O' toggles onion skin (the extras-bar button is gone; the shortcut
    // drives the canvas state directly).
    QShortcut *onionShortcut = new QShortcut(QKeySequence(Qt::Key_O), this);
    connect(onionShortcut, &QShortcut::activated, this, [this] {
        m_canvas->setOnionSkinEnabled(!m_canvas->isOnionSkinEnabled());
        updateOnionGhost();
    });

    // Ctrl+Left / Ctrl+Right move the current panel within its scene.
    QShortcut *movePanelLeft =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
    connect(movePanelLeft, &QShortcut::activated, this, [this] { movePanelBy(-1); });
    QShortcut *movePanelRight =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
    connect(movePanelRight, &QShortcut::activated, this, [this] { movePanelBy(1); });

    // Ctrl+D DESELECTS (clears the marching-ants selection mask).
    QShortcut *deselectShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_D), this);
    connect(deselectShortcut, &QShortcut::activated, this, [this] {
        if (m_canvas)
            m_canvas->clearSelection();
    });

    // Ctrl+Shift+D duplicates the current panel (moved off Ctrl+D; the
    // Duplicate button in the panel-control column is unchanged).
    QShortcut *duplicateShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D), this);
    connect(duplicateShortcut, &QShortcut::activated, this, [this] { duplicatePanel(); });

    // Ctrl+I imports an image onto the current panel.
    QShortcut *importShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_I), this);
    connect(importShortcut, &QShortcut::activated, this, [this] { importImageToPanel(); });

    // (Ctrl+Z / Ctrl+Y live on the Edit menu's Undo/Redo actions in MainWindow,
    // routed here via editUndo()/editRedo() — a page-level QShortcut would make
    // the sequence ambiguous and silently break both.)





    updateDuplicateButton(); // panel-action buttons disabled until a panel is selected
}

// Layout is persisted ONLY here and on app close (aboutToQuit) — never on
// intermediate dock events, so a mid-session glitch can't be baked in.
StoryboardPage::~StoryboardPage()
{
    saveDockState();
    delete m_panelClipboard; // owned deep copy, independent of any scene
}

// --- Dock plumbing ----------------------------------------------------------

// Reapplies the pristine default layout (Layers right, Scenes + Shot Info
// tabbed below it). Used by View -> Reset Layout and as the fallback when a
// saved state is rejected.
void StoryboardPage::applyDefaultDockLayout()
{
    if (m_dockController)
        m_dockController->resetLayout();
}

// Extend the application's View menu (on the top-level MainWindow) with the
// three dock toggles. toggleViewAction() gives a checkable action that both
// tracks and controls the dock's visibility.
void StoryboardPage::installDockViewActions()
{
    QMainWindow *top = qobject_cast<QMainWindow *>(window());
    if (!top || !m_layersDock)
        return;

    QMenuBar *bar = top->menuBar();
    QMenu *viewMenu = nullptr;
    const QList<QAction *> menus = bar->actions();
    for (QAction *action : menus) {
        QString title = action->text();
        title.remove(QLatin1Char('&'));
        if (action->menu() && title == QLatin1String("View")) {
            viewMenu = action->menu();
            break;
        }
    }
    if (!viewMenu)
        viewMenu = bar->addMenu(QStringLiteral("View"));

    viewMenu->addSeparator();
    // Panels submenu: per-dock toggles (reopen closed panels) plus
    // Show All / Hide All, provided by the docking controller.
    QMenu *panelsMenu = viewMenu->addMenu(QStringLiteral("Panels"));
    m_dockController->populatePanelsMenu(panelsMenu);

    // Canvas alignment grid (display-only overlay), default OFF.
    QAction *gridAction = viewMenu->addAction(QStringLiteral("Grid"));
    gridAction->setCheckable(true);
    gridAction->setChecked(false);
    connect(gridAction, &QAction::toggled, this, [this](bool on) {
        if (m_canvas)
            m_canvas->setGridEnabled(on);
    });

    // Layout commands: explicit save/restore plus the escape hatch that
    // wipes the persisted layout and goes back to the default. (The layout
    // also auto-saves on application close.)
    viewMenu->addSeparator();
    QAction *saveLayout = viewMenu->addAction(QStringLiteral("Save Layout"));
    connect(saveLayout, &QAction::triggered, this,
            [this] { saveDockState(); });
    QAction *restoreLayout =
        viewMenu->addAction(QStringLiteral("Restore Layout"));
    connect(restoreLayout, &QAction::triggered, this, [this] {
        if (!restoreDockState())
            applyDefaultDockLayout(); // nothing saved / rejected blob
    });
    QAction *resetLayout = viewMenu->addAction(QStringLiteral("Reset Layout"));
    connect(resetLayout, &QAction::triggered, this, [this] {
        m_dockController->clearSavedLayout();
        applyDefaultDockLayout();
    });
    // (The old Help > "About Qt Advanced Docking System" entry left with the
    // ADS library: the docking layer is now first-party code.)
}

// Thin wrappers over the docking controller (kept so call sites and the
// save-on-exit hooks read the same as before the ADS migration). The
// controller version-checks restoreState() and reads/writes ONLY the new
// storyboard/nativeDock/* keys — the old ADS blob is never touched.
bool StoryboardPage::restoreDockState()
{
    return m_dockController && m_dockController->restoreLayout();
}

void StoryboardPage::saveDockState()
{
    if (m_dockController)
        m_dockController->saveLayout();
}

// --- Left column ----------------------------------------------------------

QWidget *StoryboardPage::createLeftColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setMinimumWidth(160); // dock-resizable (was fixed 200)
    column->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(12, 14, 12, 12);
    layout->setSpacing(10);

    // (No inner heading: the ADS dock tab already names the panel.)

    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    QWidget *container = new QWidget;
    container->setStyleSheet(QStringLiteral("background: transparent;"));
    m_sceneListLayout = new QVBoxLayout(container);
    m_sceneListLayout->setContentsMargins(0, 0, 0, 0);
    m_sceneListLayout->setSpacing(8);
    m_sceneListLayout->addStretch(1);

    scroll->setWidget(container);
    layout->addWidget(scroll, 1);

    return column;
}

void StoryboardPage::rebuildSceneList()
{
    m_sceneCards.clear();
    // Remove everything (including the trailing stretch) and rebuild.
    while (QLayoutItem *item = m_sceneListLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    for (int i = 0; i < m_scenes.size(); ++i) {
        Scene *scene = m_scenes.at(i);

        QFrame *card = new QFrame;
        card->setObjectName(QStringLiteral("sceneCard"));
        card->setProperty("sceneIndex", i);
        card->setCursor(Qt::PointingHandCursor);
        card->installEventFilter(this);

        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(6);

        QLabel *number = new QLabel(QStringLiteral("SCENE %1").arg(scene->number));
        number->setStyleSheet(QStringLiteral(
            "color: #f5a623; font-size: 11px; font-weight: 700; border: none; background: transparent;"));
        cardLayout->addWidget(number);

        QLabel *location = new QLabel(scene->location);
        location->setWordWrap(true);
        location->setStyleSheet(QStringLiteral(
            "color: #ffffff; font-size: 12px; border: none; background: transparent;"));
        cardLayout->addWidget(location);
        // (Add Panel moved to the fixed control column on the panel strip.)

        m_sceneListLayout->addWidget(card);
        m_sceneCards.append(card);
    }

    m_sceneListLayout->addStretch(1);
    updateSceneCardStyles();
}

void StoryboardPage::updateSceneCardStyles()
{
    for (int i = 0; i < m_sceneCards.size(); ++i) {
        const bool selected = (i == m_currentScene);
        m_sceneCards.at(i)->setStyleSheet(
            selected
                ? QStringLiteral("QFrame#sceneCard { background-color: #1b1b1b;"
                                 " border: 1px solid #2a2a2a; border-left: 3px solid #f5a623;"
                                 " border-radius: 6px; }")
                : QStringLiteral("QFrame#sceneCard { background-color: #161616;"
                                 " border: 1px solid #2a2a2a; border-radius: 6px; }"));
    }
}

// --- Center column --------------------------------------------------------

QWidget *StoryboardPage::createCenterColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setStyleSheet(QStringLiteral("background-color: #0a0a0a;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Panel strip row: a FIXED control column (never scrolls), a thin divider,
    // then the horizontally-scrolling thumbnail area — the column sits OUTSIDE
    // the QScrollArea so it stays pinned at the left edge.
    // Panel Strip (Figma tokens): height 159, #1a1a1a, horizontal, 10px gap,
    // 12 top/bottom + 10 left/right padding, items vertically centered.
    QWidget *stripBar = new QWidget;
    stripBar->setAttribute(Qt::WA_StyledBackground, true);
    stripBar->setFixedHeight(159);
    stripBar->setStyleSheet(QStringLiteral("background-color: #1a1a1a;"));
    QHBoxLayout *stripBarLayout = new QHBoxLayout(stripBar);
    stripBarLayout->setContentsMargins(10, 12, 10, 12);
    stripBarLayout->setSpacing(10);

    stripBarLayout->addWidget(createPanelControls(), 0, Qt::AlignVCenter); // pinned, non-scrolling

    QScrollArea *strip = new QScrollArea;
    strip->setWidgetResizable(true);
    strip->setFrameShape(QFrame::NoFrame);
    strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    strip->setStyleSheet(QStringLiteral("QScrollArea { background-color: #1a1a1a; border: none; }"));

    QWidget *stripContainer = new QWidget;
    stripContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_panelStripLayout = new QHBoxLayout(stripContainer);
    m_panelStripLayout->setContentsMargins(0, 0, 0, 0);
    m_panelStripLayout->setSpacing(10);
    m_panelStripLayout->addStretch(1);

    strip->setWidget(stripContainer);
    m_panelScroll = strip; // kept so we can scroll a new panel into view
    stripBarLayout->addWidget(strip, 1); // only the thumbnails scroll

    layout->addWidget(stripBar);

    // Drawing area (settings panels + canvas; the tools live in a floating
    // pill toolbar layered over the canvas).
    QWidget *drawRow = new QWidget;
    QHBoxLayout *drawLayout = new QHBoxLayout(drawRow);
    drawLayout->setContentsMargins(0, 0, 0, 0);
    drawLayout->setSpacing(0);

    m_canvas = new DrawingCanvas;
    connect(m_canvas, &DrawingCanvas::contentChanged, this, [this] {
        refreshCurrentThumb(); // debounced; also refreshes the row preview
    });
    // Undo/redo may rewrite a panel that is NOT current; refresh ITS thumb.
    connect(m_canvas, &DrawingCanvas::panelEdited, this, [this](Panel *panel) {
        Scene *scene = currentScene();
        if (!scene)
            return;
        const int idx = scene->panels.indexOf(panel);
        if (idx < 0 || idx >= m_panelThumbImages.size())
            return;
        m_panelThumbImages.at(idx)->setPixmap(
            panel->flattenedPixmap().scaled(kThumbW, kThumbH, Qt::IgnoreAspectRatio,
                                            Qt::SmoothTransformation));
    });
    connect(m_canvas, &DrawingCanvas::layersChanged, this, &StoryboardPage::rebuildLayerPanel);
    // Ctrl+click canvas pick -> select/highlight that layer's row
    // (Photoshop-style auto-select).
    connect(m_canvas, &DrawingCanvas::layerPickRequested, this,
            [this](const QString &layerId) {
                Panel *panel = currentPanel();
                if (!panel)
                    return;
                for (int i = 0; i < panel->layers.size(); ++i)
                    if (panel->layers.at(i).id == layerId) {
                        layerRowClicked(i, Qt::NoModifier);
                        return;
                    }
            });

    drawLayout->addWidget(m_canvas, 1);

    createFloatingToolbar(); // FloatingToolWindows anchored to the canvas
    createBrushSettings();   // floating over the canvas, shown with the Brush tool
    createCameraPanel();     // floating over the canvas, shown with the Camera tool
    createPerspectiveModifier(); // shown with the Perspective tool
    createShapesPanel();     // floating over the canvas, shown with the Shapes tool

    // Custom-painted Canvas View Controls toolbar (zoom / flip / rotate),
    // floating over the canvas and wired to the canvas view transforms.
    // FloatingToolWindow: anchored to the canvas, owned by the page.
    m_zoomToolbar = new ZoomToolbar(m_canvas, this);
    m_zoomToolbar->setZoom(m_canvas->viewZoom());
    m_zoomToolbar->setRotation(m_canvas->viewRotation());
    connect(m_zoomToolbar, &ZoomToolbar::zoomChanged, m_canvas, &DrawingCanvas::setViewZoom);
    connect(m_zoomToolbar, &ZoomToolbar::rotationChanged, m_canvas, &DrawingCanvas::setViewRotation);
    // Fit Screen: ALWAYS centre the canvas and fit it fully (small margin),
    // clearing pan and rotation whatever the current view state is. The
    // zoom dragger syncs through viewZoomChanged below.
    connect(m_zoomToolbar, &ZoomToolbar::fitRequested, this, [this] {
        m_canvas->fitToScreen();
        m_canvas->resetViewRotation();
        m_zoomToolbar->setRotation(0.0);
    });
    // Wheel/pan zoom on the canvas syncs the zoom dragger back.
    connect(m_canvas, &DrawingCanvas::viewZoomChanged, m_zoomToolbar, &ZoomToolbar::setZoom);
    m_zoomToolbar->show(); // records intent; effective when the canvas shows

    layout->addWidget(drawRow, 1);

    return column;
}

// Floating tool bars layered over the canvas: the horizontal Brush bar and
// the vertical extras bar, both FloatingToolWindows anchored to the canvas.
void StoryboardPage::createFloatingToolbar()
{
    // Shared, exclusive tool group spanning BOTH floating bars (horizontal
    // Brush bar + vertical extras bar): picking any tool unchecks the rest and
    // drives the canvas tool and per-tool options panels.
    QButtonGroup *tools = new QButtonGroup(this);
    tools->setExclusive(true);

    // Custom-painted tool button: the background carries the state (Default
    // #212121 / Hover #4C4C4C / Pressed #7C6EF6 / Active #7C6EF6), the #cccccc
    // icon never recolours. Antialiased corners.
    auto toolButton = [](const QPixmap &icon, const QString &tip, bool checkable) {
        ToolButton *b = new ToolButton(icon);
        b->setCheckable(checkable);
        b->setToolTip(tip);
        return b;
    };
    auto bindTool = [this](QPushButton *button, DrawingCanvas::Tool tool) {
        connect(button, &QPushButton::toggled, this, [this, tool](bool on) {
            if (on && m_canvas)
                m_canvas->setTool(tool);
        });
    };

    // ---- Horizontal Brush bar (Figma node 33:110) ------------------------
    // A RoundedBar FloatingToolWindow anchored to the canvas: grip drag,
    // clamping, main-window follow, and position persistence all come from
    // the base class.
    RoundedBar *brushBar =
        new RoundedBar(m_canvas, QStringLiteral("storyboard/brushBarPos"), this);
    m_floatToolbar = brushBar;
    m_floatToolbar->setObjectName(QStringLiteral("floatToolbar"));
    m_floatToolbar->setFixedHeight(46);

    QHBoxLayout *bar = new QHBoxLayout(m_floatToolbar);
    bar->setContentsMargins(13, 8, 23, 8);
    bar->setSpacing(15);

    // Grip (vertical 2x3 dots), left. The label ignores mouse events, so
    // presses on it propagate to the bar, whose gripRect() covers it.
    QLabel *grip = new QLabel;
    grip->setPixmap(dragDotsPixmapV());
    grip->setFixedSize(12, 20);
    grip->setCursor(Qt::OpenHandCursor);
    grip->setToolTip(QStringLiteral("Drag to move the toolbar"));
    bar->addWidget(grip, 0, Qt::AlignVCenter);
    brushBar->setGripWidget(grip);

    // Exact Figma icons (original SVGs) rendered at their Figma sizes; the
    // "select" glyph is the Figma dashed marquee + cursor arrow. Tooltips use
    // the "<b>Name</b> | description" rich-text format.
    QPushButton *brushTool = toolButton(
        figIconPixmap(QStringLiteral(":/icons/brush.svg"), QSizeF(24.1, 21.18)),
        QStringLiteral("<b>Brush</b> | Paint strokes on the canvas."), true);
    QPushButton *eraser = toolButton(
        figIconPixmap(QStringLiteral(":/icons/erase.svg"), QSizeF(17.83, 20.03)),
        QStringLiteral("<b>Erase</b> | Remove parts of the drawing."), true);
    QPushButton *fill = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fill.svg"), QSizeF(18.67, 17.43)),
        QStringLiteral("<b>Fill</b> | Fill an area with color."), true);
    ToolButton *selection = toolButton(selectGlyphPixmap(),
        QStringLiteral("<b>Select</b> | Select part of the canvas."), true);
    QPushButton *move = toolButton(
        figIconPixmap(QStringLiteral(":/icons/move.svg"), QSizeF(20.4, 20.4)),
        QStringLiteral("<b>Move</b> | Move the selected pixels."), true);
    brushTool->setChecked(true); // Brush is the default drawing tool

    tools->addButton(brushTool);
    tools->addButton(eraser);
    tools->addButton(fill);
    tools->addButton(selection);
    tools->addButton(move);



    bindTool(brushTool, DrawingCanvas::Brush);
    bindTool(eraser, DrawingCanvas::Eraser);
    bindTool(fill, DrawingCanvas::Fill);
    bindTool(move, DrawingCanvas::Move);

    // ONE Selection button, FOUR modes (Figma "Select shapes" 128:374):
    // Rectangle / Ellipse / Freehand / Polygon. A plain click re-activates the
    // last-chosen mode; click-and-hold or right-click opens the Selection
    // Shapes popup, and the button's icon mirrors the choice.
    auto selModeIcon = [](DrawingCanvas::Tool mode) -> QPixmap {
        switch (mode) {
        case DrawingCanvas::SelectEllipse:
            return figIconPixmap(QStringLiteral(":/icons/sel_ellipse.svg"), QSizeF(30, 30));
        case DrawingCanvas::Lasso:
            return figIconPixmap(QStringLiteral(":/icons/sel_freehand.svg"), QSizeF(30, 30));
        case DrawingCanvas::SelectPoly:
            return figIconPixmap(QStringLiteral(":/icons/sel_polygon.svg"), QSizeF(30, 30));
        default: // SelectRect: the marquee + cursor glyph (toolbar identity)
            return selectGlyphPixmap();
        }
    };
    auto pickSelectionMode = [this, selection, selModeIcon](DrawingCanvas::Tool mode) {
        m_selectionMode = mode;
        selection->setIconPixmap(selModeIcon(mode));
        selection->setChecked(true); // the exclusive group unchecks the old tool
        if (m_canvas)
            m_canvas->setTool(mode); // also covers "already checked, mode changed"
    };

    // The popup: an icon-only, 4-button flyout matching the Figma exactly
    // (container #212121 r4, pt3/pr3/pb4/pl6, gap 11; buttons 30x30 r6, hover
    // #4C4C4C, pressed #737373; Rectangle = drawn dashed marquee, the rest are
    // the exact Figma SVGs). Tooltips are the tool name only.
    RoundedPopupFrame *shapesPopup = new RoundedPopupFrame(this);
    QHBoxLayout *popupLayout = new QHBoxLayout(shapesPopup);
    popupLayout->setContentsMargins(6, 3, 3, 4);
    popupLayout->setSpacing(11);
    struct ModeDef { DrawingCanvas::Tool mode; QPixmap icon; const char *name; };
    const ModeDef popupModes[] = {
        {DrawingCanvas::SelectRect, selectRectGlyphPixmap(), "Rectangle"},
        {DrawingCanvas::SelectEllipse,
         figIconPixmap(QStringLiteral(":/icons/sel_ellipse.svg"), QSizeF(30, 30)), "Ellipse"},
        {DrawingCanvas::Lasso,
         figIconPixmap(QStringLiteral(":/icons/sel_freehand.svg"), QSizeF(30, 30)), "Freehand"},
        {DrawingCanvas::SelectPoly,
         figIconPixmap(QStringLiteral(":/icons/sel_polygon.svg"), QSizeF(30, 30)), "Polygon"},
    };
    for (const ModeDef &md : popupModes) {
        ToolButton *b = new ToolButton(md.icon);
        b->setStateColors(QColor(0x4c, 0x4c, 0x4c), QColor(0x73, 0x73, 0x73));
        b->setToolTip(QString::fromLatin1(md.name)); // tool name only, no desc
        const DrawingCanvas::Tool mode = md.mode;
        connect(b, &QPushButton::clicked, this,
                [shapesPopup, pickSelectionMode, mode] {
                    pickSelectionMode(mode);
                    shapesPopup->hide();
                });
        popupLayout->addWidget(b);
    }
    shapesPopup->adjustSize();

    // Automatic placement: keep a 4px gap to the Brush toolbar; prefer above,
    // else below; always fully inside the window and clear of other floats.
    auto showShapesPopup = [this, selection, shapesPopup] {
        shapesPopup->adjustSize();
        const int w = shapesPopup->width(), h = shapesPopup->height();
        const QRect win = window()->frameGeometry();
        const QRect barRect = m_floatToolbar->frameGeometry();
        const QRect selRect(selection->mapToGlobal(QPoint(0, 0)), selection->size());
        int x = qBound(win.left(), selRect.center().x() - w / 2, win.right() - w + 1);
        const int gap = 4;
        // frameGeometry().bottom()/top() are inclusive edges, so "below" needs
        // +1 to leave the SAME 4px empty gap that "above" does.
        const int yAbove = barRect.top() - gap - h;
        const int yBelow = barRect.bottom() + 1 + gap;
        auto clearAt = [&](int y) {
            const QRect r(x, y, w, h);
            if (!win.contains(r))
                return false;
            const QWidget *floats[] = {m_zoomToolbar, m_brushPanel,
                                       m_cameraPanel, m_shapesPanel};
            for (const QWidget *fl : floats)
                if (fl && fl->isVisible() && r.intersects(fl->frameGeometry()))
                    return false;
            // The Panel Strip occupies the band above the canvas; if opening
            // above would overlap it, fall through to "below".
            if (m_panelScroll && m_panelScroll->isVisible()) {
                const QRect strip(m_panelScroll->mapToGlobal(QPoint(0, 0)),
                                  m_panelScroll->size());
                if (r.intersects(strip))
                    return false;
            }
            return true;
        };
        int y = clearAt(yAbove) ? yAbove : (clearAt(yBelow) ? yBelow : yAbove);
        y = qBound(win.top(), y, win.bottom() - h + 1); // stay fully in-window
        shapesPopup->move(x, y);
        shapesPopup->show();
    };
    // Clicking Select opens the Shape Selection popup. `clicked` fires on
    // release — the button's click has fully completed (so it is checked, not
    // left stuck "down") BEFORE the popup grabs the mouse, keeping the active
    // state consistent.
    connect(selection, &QPushButton::clicked, this,
            [showShapesPopup] { showShapesPopup(); });
    // The Select button is active whenever a selection mode is the current
    // tool: becoming checked activates the last-chosen mode; the exclusive tool
    // group unchecks it the moment any non-selection tool is picked.
    connect(selection, &QPushButton::toggled, this, [this](bool on) {
        if (on && m_canvas)
            m_canvas->setTool(m_selectionMode);
    });

    // ---- Selection Modifier toolbar (Figma "Add Select" 146:67) ----------
    // Bottom-centre (above the Brush bar), auto-shown only while a selection
    // tool is active; NOT user-movable. Add/Remove set the combine mode; Move
    // switches to the Move tool; Select All / Inverse / Deselect act at once.
    SelModBar *selBar = new SelModBar(m_canvas, this);
    m_selModToolbar = selBar;
    // Figma 146:67 (updated): a 531x66 rgba(33,33,33,0.65) radius-8 container,
    // px17/py8, holding six 36x36 radius-6 #212121 buttons (24x24 icons) with
    // Inter Semi-Bold 9px #cccccc captions at content y=41. The column x's are
    // the exact node offsets: 0 / 91 / 182 / 276 / 369 / 462.
    selBar->setFixedSize(531, 66);

    auto modButton = [selBar](const QPixmap &icon, const QString &tip, bool checkable,
                              int x) {
        ToolButton *b = new ToolButton(icon, selBar);
        b->setFixedSize(36, 36);
        b->setCheckable(checkable);
        b->setStateColors(QColor(0x4c, 0x4c, 0x4c), QColor(0x73, 0x73, 0x73));
        b->setToolTip(tip);  // name only, no description
        b->move(17 + x, 8);  // px17 / py8 offset into the content area
        return b;
    };
    ToolButton *addBtn = modButton(
        selModIconPixmap(QStringLiteral(":/icons/selmod_add.svg"), QSizeF(25, 25)),
        QStringLiteral("Add"), true, 0);
    ToolButton *removeBtn = modButton(
        selModIconPixmap(QStringLiteral(":/icons/selmod_remove.svg"), QSizeF(25, 25)),
        QStringLiteral("Remove"), true, 91);
    ToolButton *moveBtn = modButton(
        selModIconPixmap(QStringLiteral(":/icons/selmod_move.svg"), QSizeF(27.5, 26.5)),
        QStringLiteral("Move"), true, 182);
    ToolButton *selectAllBtn =
        modButton(selectAllGlyphPixmap(), QStringLiteral("Select All"), false, 276);
    ToolButton *inverseBtn = modButton(
        selModIconPixmap(QStringLiteral(":/icons/selmod_inverse.svg"), QSizeF(25, 25)),
        QStringLiteral("Inverse"), false, 369);
    ToolButton *deselectBtn = modButton(
        selModIconPixmap(QStringLiteral(":/icons/selmod_deselect.svg"), QSizeF(25, 25)),
        QStringLiteral("Deselect"), false, 462);

    // Captions centred under each button (Figma: Inter Semi-Bold 9px #CCCCCC,
    // content y=41 -> 8+41 in widget coords, i.e. 5px below the 36px buttons).
    QFont capFont(QStringLiteral("Inter"));
    capFont.setPixelSize(9);
    capFont.setWeight(QFont::DemiBold);
    const struct { ToolButton *btn; QString text; } captions[] = {
        {addBtn, QStringLiteral("Add")},        {removeBtn, QStringLiteral("Remove")},
        {moveBtn, QStringLiteral("Move")},      {selectAllBtn, QStringLiteral("Select All")},
        {inverseBtn, QStringLiteral("Inverse")}, {deselectBtn, QStringLiteral("Deselect")},
    };
    for (const auto &c : captions) {
        QLabel *cap = new QLabel(c.text, selBar);
        cap->setFont(capFont);
        cap->setStyleSheet(QStringLiteral("color:#cccccc;background:transparent;"));
        cap->adjustSize();
        cap->move(c.btn->x() + c.btn->width() / 2 - cap->width() / 2, 8 + 41);
    }
    // Add / Remove / Move: three mutually exclusive modes (Active = #7C6EF6
    // via the checked state; all off = Replace). Add and Remove set how a
    // freshly-drawn shape combines with the selection; Move drags ONLY the
    // selection outline (marching ants) — never the artwork pixels (the
    // Brush-bar Move TOOL is the one that transforms artwork).
    connect(addBtn, &QPushButton::clicked, this, [this, addBtn, removeBtn, moveBtn] {
        removeBtn->setChecked(false);
        moveBtn->setChecked(false);
        m_canvas->setSelectionOutlineMove(false);
        m_canvas->setSelectionOp(addBtn->isChecked() ? DrawingCanvas::SelAdd
                                                      : DrawingCanvas::SelReplace);
    });
    connect(removeBtn, &QPushButton::clicked, this, [this, addBtn, removeBtn, moveBtn] {
        addBtn->setChecked(false);
        moveBtn->setChecked(false);
        m_canvas->setSelectionOutlineMove(false);
        m_canvas->setSelectionOp(removeBtn->isChecked() ? DrawingCanvas::SelSubtract
                                                        : DrawingCanvas::SelReplace);
    });
    connect(moveBtn, &QPushButton::clicked, this, [this, addBtn, removeBtn, moveBtn] {
        addBtn->setChecked(false);
        removeBtn->setChecked(false);
        m_canvas->setSelectionOp(DrawingCanvas::SelReplace);
        m_canvas->setSelectionOutlineMove(moveBtn->isChecked());
    });
    connect(selectAllBtn, &QPushButton::clicked, this, [this] { m_canvas->selectAll(); });
    connect(inverseBtn, &QPushButton::clicked, this, [this] { m_canvas->invertSelection(); });
    connect(deselectBtn, &QPushButton::clicked, this, [this] { m_canvas->deselect(); });
    selBar->setDefaultOffsetProvider([this, selBar] {
        // Bottom-centre, 10px above the status bar (the bottom toolbar). The
        // gap is measured to the bar's real top edge, mapped into canvas
        // coordinates; if it is hidden, the canvas bottom stands in.
        int statusTop = m_canvas->height();
        if (m_bottomBar && m_bottomBar->isVisible())
            statusTop = m_canvas->mapFromGlobal(
                m_bottomBar->mapToGlobal(QPoint(0, 0))).y();
        return QPoint(qMax(6, (m_canvas->width() - selBar->width()) / 2),
                      qMax(6, statusTop - selBar->height() - 10));
    });
    // Visible only while a selection tool is active (the Select button's checked
    // state tracks exactly that). ADD is the default active mode each time the
    // bar appears; leaving the selection tools drops outline-move mode.
    connect(selection, &QPushButton::toggled, this,
            [this, addBtn, removeBtn, moveBtn](bool on) {
        if (on) {
            addBtn->setChecked(true); // Add = default selection mode
            removeBtn->setChecked(false);
            moveBtn->setChecked(false);
            if (m_canvas) {
                m_canvas->setSelectionOp(DrawingCanvas::SelAdd);
                m_canvas->setSelectionOutlineMove(false);
            }
        } else if (m_canvas) {
            m_canvas->setSelectionOutlineMove(false);
        }
        if (m_selModToolbar)
            m_selModToolbar->setVisible(on);
    });

    // ---- Move Modifier toolbar (Figma "Move" 161:39) ----------------------
    // Same behaviour rules as the Selection Modifier bar: bottom-centre, 10px
    // above the status bar, not movable, auto-shown only while the Move TOOL
    // is active (the exclusive tool group guarantees the two bars are never
    // visible together). Buttons switch the transform box's interaction mode:
    // Pivot Point / Skew / Distort / Perspective (see DrawingCanvas).
    SelModBar *moveBar = new SelModBar(m_canvas, this);
    m_moveModToolbar = moveBar;
    moveBar->setFixedSize(470, 66); // rgba(33,33,33,0.65) r8, px17/py8 content
    auto moveModButton = [moveBar](const QString &svg, const QString &tip, int x) {
        ToolButton *b = new ToolButton(moveModIconPixmap(svg), moveBar);
        b->setFixedSize(36, 36);
        b->setCheckable(true);
        b->setStateColors(QColor(0x4c, 0x4c, 0x4c), QColor(0x73, 0x73, 0x73));
        b->setToolTip(tip);  // name only, no description
        b->move(17 + x, 8);  // px17 / py8 offset into the content area
        return b;
    };
    // Exact Figma column x's within the 325-wide content frame.
    ToolButton *pivotBtn = moveModButton(
        QStringLiteral(":/icons/movemod_pivot.svg"), QStringLiteral("Pivot Point"), 7);
    ToolButton *skewBtn = moveModButton(
        QStringLiteral(":/icons/movemod_skew.svg"), QStringLiteral("Skew"), 104);
    ToolButton *distortBtn = moveModButton(
        QStringLiteral(":/icons/movemod_distort.svg"), QStringLiteral("Distort"), 195);
    ToolButton *perspBtn = moveModButton(
        QStringLiteral(":/icons/movemod_perspective.svg"),
        QStringLiteral("Perspective"), 294);
    ToolButton *warpBtn = moveModButton(
        QStringLiteral(":/icons/movemod_warp.svg"), QStringLiteral("Warp"), 393);

    // Captions centred under each button (Inter Semi-Bold 9px #CCCCCC, y=41).
    const struct { ToolButton *btn; QString text; } moveCaps[] = {
        {pivotBtn, QStringLiteral("Pivot Point")}, {skewBtn, QStringLiteral("Skew")},
        {distortBtn, QStringLiteral("Distort")},
        {perspBtn, QStringLiteral("Perspective")},
        {warpBtn, QStringLiteral("Warp")},
    };
    QFont moveCapFont(QStringLiteral("Inter"));
    moveCapFont.setPixelSize(9);
    moveCapFont.setWeight(QFont::DemiBold);
    for (const auto &c : moveCaps) {
        QLabel *cap = new QLabel(c.text, moveBar);
        cap->setFont(moveCapFont);
        cap->setStyleSheet(QStringLiteral("color:#cccccc;background:transparent;"));
        cap->adjustSize();
        cap->move(c.btn->x() + c.btn->width() / 2 - cap->width() / 2, 8 + 41);
    }

    // One mode at a time (Active = #7C6EF6); clicking the active mode again
    // returns to the default move/scale/rotate box. Switching modes keeps the
    // live transform session — only Enter/Esc end it.
    auto wireMoveMode = [this, pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn](
                            ToolButton *btn, DrawingCanvas::XformUiMode mode) {
        connect(btn, &QPushButton::clicked, this,
                [this, btn, mode, pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn] {
            for (ToolButton *other : {pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn})
                if (other != btn)
                    other->setChecked(false);
            m_canvas->setXformUiMode(btn->isChecked() ? mode
                                                      : DrawingCanvas::XformDefault);
        });
    };
    wireMoveMode(pivotBtn, DrawingCanvas::XformPivot);
    wireMoveMode(skewBtn, DrawingCanvas::XformSkew);
    wireMoveMode(distortBtn, DrawingCanvas::XformDistort);
    wireMoveMode(perspBtn, DrawingCanvas::XformPerspective);
    wireMoveMode(warpBtn, DrawingCanvas::XformWarp);
    // Committing a Warp drops the canvas back to the default box; mirror
    // that here so no mode button stays lit.
    connect(m_canvas, &DrawingCanvas::xformUiModeReset, this,
            [pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn] {
        for (ToolButton *b : {pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn})
            b->setChecked(false);
    });

    moveBar->setDefaultOffsetProvider([this, moveBar] {
        // Bottom-centre, 10px above the status bar — identical to the
        // Selection Modifier bar's placement rule.
        int statusTop = m_canvas->height();
        if (m_bottomBar && m_bottomBar->isVisible())
            statusTop = m_canvas->mapFromGlobal(
                m_bottomBar->mapToGlobal(QPoint(0, 0))).y();
        return QPoint(qMax(6, (m_canvas->width() - moveBar->width()) / 2),
                      qMax(6, statusTop - moveBar->height() - 10));
    });
    // Visible only while the Move tool is active; each appearance starts in
    // the default transform mode.
    connect(move, &QPushButton::toggled, this,
            [this, pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn](bool on) {
        if (on) {
            for (ToolButton *b : {pivotBtn, skewBtn, distortBtn, perspBtn, warpBtn})
                b->setChecked(false);
            if (m_canvas)
                m_canvas->setXformUiMode(DrawingCanvas::XformDefault);
        }
        if (m_moveModToolbar)
            m_moveModToolbar->setVisible(on);
    });

    // Brush options panel visible only while Brush is the active tool.
    connect(brushTool, &QPushButton::toggled, this, [this](bool on) {
        if (m_brushPanel)
            m_brushPanel->setVisible(on);
    });

    bar->addWidget(brushTool, 0, Qt::AlignVCenter);
    bar->addWidget(eraser, 0, Qt::AlignVCenter);
    bar->addWidget(fill, 0, Qt::AlignVCenter);
    bar->addWidget(selection, 0, Qt::AlignVCenter);
    bar->addWidget(move, 0, Qt::AlignVCenter);

    // Color control (Figma node 33:87): custom-painted 30x30 rounded-6 box with
    // a 1.5px #808080 border and an inset 24x25 rounded-4 swatch that never
    // bleeds over the border (antialiased).
    ColorSwatchButton *color = new ColorSwatchButton;
    color->setToolTip(QStringLiteral("<b>Color</b> | Choose the brush color."));
    color->setSwatchColor(Qt::black);
    // Procreate-style Colors panel (ColorPanel body inside the standard
    // floating panel chrome). The swatch toggles it; every live change in the
    // panel drives the brush color and the swatch immediately.
    ColorPanel *colorBody = new ColorPanel;
    colorBody->setColor(Qt::black);
    QWidget *colorsPanel = createFloatingPanel(QStringLiteral("Colors"), colorBody);
    colorsPanel->hide();
    connect(colorBody, &ColorPanel::colorChanged, this,
            [this, color](const QColor &chosen) {
        m_canvas->setColor(chosen);
        color->setSwatchColor(chosen);
    });
    connect(color, &QPushButton::clicked, this, [colorsPanel] {
        colorsPanel->setVisible(!colorsPanel->isVisible());
    });
    bar->addWidget(color, 0, Qt::AlignVCenter);

    // Divider.
    QLabel *divider = new QLabel;
    divider->setFixedSize(1, 20);
    divider->setStyleSheet(QStringLiteral("background-color: #4d4d4d;"));
    bar->addWidget(divider, 0, Qt::AlignVCenter);

    // Undo / redo (exact Figma SVG; redo is the undo arrow mirrored).
    QPushButton *undo = toolButton(
        figIconPixmap(QStringLiteral(":/icons/undo.svg"), QSizeF(19.2, 15.2)),
        QStringLiteral("<b>Undo</b> | Undo the last action."), false);
    connect(undo, &QPushButton::clicked, this, [this] { m_canvas->undo(); });
    bar->addWidget(undo, 0, Qt::AlignVCenter);

    QPushButton *redo = toolButton(
        figIconPixmap(QStringLiteral(":/icons/undo.svg"), QSizeF(19.2, 15.2), /*mirror*/ true),
        QStringLiteral("<b>Redo</b> | Redo the last action."), false);
    connect(redo, &QPushButton::clicked, this, [this] { m_canvas->redo(); });
    bar->addWidget(redo, 0, Qt::AlignVCenter);

    m_floatToolbar->adjustSize(); // fixed content -> final bar width
    // Default spot: bottom-centred, stacked just above the zoom toolbar
    // (46px tall + its 12px margin), re-derived as the canvas resizes.
    // ONE reused tooltip for the whole bar: every tooltip-bearing child
    // (tools, grip, Color, Undo, Redo) routes through eventFilter(), which
    // drives the shared SankoTipPopup — consistent below-the-bar placement.
    if (!m_toolbarTip)
        m_toolbarTip = new SankoTipPopup(this);
    const auto tipChildren = m_floatToolbar->findChildren<QWidget *>();
    for (QWidget *w : tipChildren)
        if (!w->toolTip().isEmpty())
            w->installEventFilter(this);

    brushBar->setDefaultOffsetProvider([this, brushBar] {
        return QPoint(qMax(6, (m_canvas->width() - brushBar->width()) / 2),
                      qMax(6, m_canvas->height() - brushBar->height() - 46 - 12 - 12));
    });
    brushBar->show(); // records intent; effective when the canvas shows


    // ---- Floating Toolbar Layers (Figma node 173:36) ----------------------
    // 381x46 bar: grab dots, Layers / Perspective / Scenes / Camera /
    // Image Reference / Shot Info buttons (30x30, 15px gaps), a 1x20 #4d4d4d
    // divider, then Settings. Exact Figma layout: margins 20/8/18/8, gap 15.
    RoundedBar *layersBar =
        new RoundedBar(m_canvas, QStringLiteral("storyboard/layersBarPos"), this);
    m_layersToolbar = layersBar;
    m_layersToolbar->setObjectName(QStringLiteral("layersToolbar"));
    m_layersToolbar->setFixedHeight(46);

    QHBoxLayout *lay = new QHBoxLayout(m_layersToolbar);
    lay->setContentsMargins(20, 8, 18, 8);
    lay->setSpacing(15);

    // Grab dots (12x20, matching the Figma asset), the drag grip.
    QLabel *layGrip = new QLabel;
    layGrip->setPixmap(dragDotsPixmapV());
    layGrip->setFixedSize(12, 20);
    layGrip->setCursor(Qt::OpenHandCursor);
    layGrip->setToolTip(QStringLiteral("Drag to move the toolbar"));
    lay->addWidget(layGrip, 0, Qt::AlignVCenter);
    layersBar->setGripWidget(layGrip);

    // Dock toggles: checkable buttons mirroring the ADS dock visibility (the
    // docks are wired after construction — see the singleShot below).
    ToolButton *layersBtn = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_layers.svg"), QSizeF(26, 17)),
        QStringLiteral("<b>Layers</b> | Show or hide the Layers panel."), true);
    ToolButton *perspective = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_perspective.svg"), QSizeF(26, 17)),
        QStringLiteral("<b>Perspective</b> | Perspective guides and snap."), true);
    ToolButton *scenesBtn = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_scenes.svg"), QSizeF(24, 24)),
        QStringLiteral("<b>Scenes</b> | Show or hide the Scenes panel."), true);
    ToolButton *cameraBtn = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_camera.svg"), QSizeF(23, 20)),
        QStringLiteral("<b>Camera</b> | Frame and safe-area guides."), true);
    ToolButton *imageRefBtn = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_imageref.svg"), QSizeF(24, 15)),
        QStringLiteral("<b>Image Reference</b> | Reference images (coming soon)."),
        false);
    ToolButton *shotInfoBtn = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_shotinfo.svg"), QSizeF(12, 20)),
        QStringLiteral("<b>Shot Info</b> | Show or hide the Shot Info panel."), true);

    lay->addWidget(layersBtn, 0, Qt::AlignVCenter);
    lay->addWidget(perspective, 0, Qt::AlignVCenter);
    lay->addWidget(scenesBtn, 0, Qt::AlignVCenter);
    lay->addWidget(cameraBtn, 0, Qt::AlignVCenter);
    lay->addWidget(imageRefBtn, 0, Qt::AlignVCenter);
    lay->addWidget(shotInfoBtn, 0, Qt::AlignVCenter);

    // Divider (1x20, #4d4d4d), then Settings.
    QFrame *layDivider = new QFrame;
    layDivider->setFixedSize(1, 20);
    layDivider->setStyleSheet(QStringLiteral("background:#4d4d4d; border:none;"));
    lay->addWidget(layDivider, 0, Qt::AlignVCenter);

    ToolButton *settingBtn = toolButton(
        figIconPixmap(QStringLiteral(":/icons/fig_setting.svg"), QSizeF(24, 24)),
        QStringLiteral("<b>Settings</b> | Open Preferences."), false);
    connect(settingBtn, &QPushButton::clicked, this,
            [this] { emit settingsRequested(); });
    lay->addWidget(settingBtn, 0, Qt::AlignVCenter);

    // Perspective tool: exclusive with the other tools; shows its panel.
    tools->addButton(perspective);
    bindTool(perspective, DrawingCanvas::Perspective);
    connect(perspective, &QPushButton::toggled, this, [this](bool on) {
        if (m_perspModToolbar)
            m_perspModToolbar->setVisible(on);
        if (on && m_syncPerspective)
            m_syncPerspective();
        if (m_canvas)
            m_canvas->update(); // first-tap prompt appears/disappears
    });

    // Camera: this button OWNS the tool (the old extras bar is gone) —
    // exclusive with the other tools and driving the Camera panel.
    tools->addButton(cameraBtn);
    bindTool(cameraBtn, DrawingCanvas::Camera);
    connect(cameraBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_cameraPanel)
            m_cameraPanel->setVisible(on);
    });

    // Dock toggles wire up once the docks exist (they are created after the
    // canvas column that hosts this bar). toggleViewAction() is the "is the
    // panel open" truth: it stays checked while a dock is open even when it
    // sits tabbed behind another panel, exactly like ADS's isClosed().
    connect(layersBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_layersDock
            && m_layersDock->toggleViewAction()->isChecked() != on)
            m_layersDock->toggleViewAction()->trigger();
    });
    connect(scenesBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_scenesDock
            && m_scenesDock->toggleViewAction()->isChecked() != on)
            m_scenesDock->toggleViewAction()->trigger();
    });
    connect(shotInfoBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_shotInfoDock
            && m_shotInfoDock->toggleViewAction()->isChecked() != on)
            m_shotInfoDock->toggleViewAction()->trigger();
    });
    QTimer::singleShot(0, this, [this, layersBtn, scenesBtn, shotInfoBtn] {
        struct Pair { QDockWidget *dock; ToolButton *btn; };
        const Pair pairs[] = {{m_layersDock, layersBtn},
                              {m_scenesDock, scenesBtn},
                              {m_shotInfoDock, shotInfoBtn}};
        for (const Pair &pr : pairs) {
            if (!pr.dock)
                continue;
            pr.btn->setChecked(pr.dock->toggleViewAction()->isChecked());
            connect(pr.dock->toggleViewAction(), &QAction::toggled,
                    pr.btn, &QPushButton::setChecked);
        }
    });


    m_layersToolbar->adjustSize();
    // Default spot: top edge, horizontally centred over the canvas.
    layersBar->setDefaultOffsetProvider([this, layersBar] {
        return QPoint(qMax(6, (m_canvas->width() - layersBar->width()) / 2), 12);
    });
    layersBar->show(); // records intent; effective when the canvas shows

    // ---- Floating Size CTL (Figma 209:42 + grab 209:53) -------------------
    // Replaces the old extras-bar brush-size slider: Brush Size, Fit Screen,
    // and Brush Opacity in one edge-snapping floating toolbar.
    SizeCtlBar *sizeBar = new SizeCtlBar(m_canvas, this);
    m_sizeCtlBar = sizeBar;
    m_sizeCtlBar->setObjectName(QStringLiteral("sizeCtlBar"));

    auto *sizeSlider = new SizeCtlSlider(1, 200, 25, sizeBar);
    sizeSlider->setToolTip(QStringLiteral("Brush size"));
    auto *flipButton = new QPushButton(sizeBar);
    flipButton->setFocusPolicy(Qt::NoFocus);
    flipButton->setCursor(Qt::PointingHandCursor);
    // Exact Figma 209:42 / 213:75: 30x30 button, radius 6, 21.6x17.55 vector.
    flipButton->setFixedSize(30, 30);
    flipButton->setCheckable(true); // Flip is a toggle with an active state
    flipButton->setChecked(m_canvas->viewFlipH());
    // The icon fills the button with a 2px margin: 26px-wide art (aspect
    // 21.6:17.55 kept -> 26x21.13) centred in figIconPixmap's 30x30 box,
    // and the box drawn 1:1 (iconSize 30) so nothing rescales the glyph.
    flipButton->setIcon(QIcon(figIconPixmap(
        QStringLiteral(":/icons/flip.svg"), QSizeF(26.0, 21.13))));
    flipButton->setIconSize(QSize(30, 30));
    flipButton->setToolTip(QStringLiteral("Flip"));
    flipButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:6px; }"
        "QPushButton:hover { background:#2e2e2e; }"
        "QPushButton:checked { background:#7c6ef6; }"));
    auto *opacitySlider = new SizeCtlSlider(5, 100, 100, sizeBar);
    opacitySlider->setToolTip(QStringLiteral("Brush opacity"));

    // Exact Figma column (209:42 metadata, rotated-frame origins decoded):
    // size slider at (10.5, 25), Flip 30x30 at (8, 276), opacity slider at
    // (10.5, 337); shifted by the bar's x when the grab sits on the left
    // (right-edge layout).
    auto placeSizeCtl = [sizeBar, sizeSlider, flipButton, opacitySlider] {
        const int bx = sizeBar->barX();
        sizeSlider->move(bx + 10, 25);
        flipButton->move(bx + 8, 276);
        opacitySlider->move(bx + 10, 337);
    };
    placeSizeCtl();
    sizeBar->onSideChanged = placeSizeCtl;

    // Numeric value + preset pill (Figma 213:94 / 213:99). One shared pill
    // floats next to whichever Size CTL slider is being adjusted; it auto-
    // hides 1s after the last change (kept alive while the cursor is over it
    // so the +/- button stays clickable).
    PresetPill *pill = new PresetPill(this);
    pill->setObjectName(QStringLiteral("sizeCtlPresetPill"));
    QTimer *pillHide = new QTimer(pill);
    pillHide->setSingleShot(true);
    pillHide->setInterval(1000);
    connect(pillHide, &QTimer::timeout, pill, &QWidget::hide);

    // Per-tool state: Brush and Eraser each keep their own size, opacity and
    // preset ticks — all persisted per tool and restored on launch. The
    // sliders display/edit whichever of the two is active.
    auto *tc = new SizeCtlToolState;
    connect(pill, &QObject::destroyed, [tc] { delete tc; });
    auto parseTicks = [](const QString &raw) {
        QVector<int> vals;
        const QStringList parts =
            raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            bool ok = false;
            const int v = part.trimmed().toInt(&ok);
            if (ok)
                vals.append(v);
        }
        return vals;
    };
    {
        const QSettings st(QStringLiteral("SankoTV"),
                           QStringLiteral("SankoTV"));
        auto load = [&](SizeCtlTool &t, const QString &base) {
            t.size = qBound(1, st.value(base + QStringLiteral("size"),
                                        t.size).toInt(), 200);
            t.opacity = qBound(5, st.value(base + QStringLiteral("opacity"),
                                           t.opacity).toInt(), 100);
            t.sizeTicks = parseTicks(
                st.value(base + QStringLiteral("sizeTicks")).toString());
            t.opacityTicks = parseTicks(
                st.value(base + QStringLiteral("opacityTicks")).toString());
        };
        load(tc->brush, QStringLiteral("storyboard/toolCtl/brush/"));
        load(tc->eraser, QStringLiteral("storyboard/toolCtl/eraser/"));
    }
    auto saveToolCtl = [tc] {
        QSettings st(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
        auto join = [](const QVector<int> &ticks) {
            QStringList parts;
            for (int v : ticks)
                parts << QString::number(v);
            return parts.join(QLatin1Char(','));
        };
        auto save = [&](const SizeCtlTool &t, const QString &base) {
            st.setValue(base + QStringLiteral("size"), t.size);
            st.setValue(base + QStringLiteral("opacity"), t.opacity);
            st.setValue(base + QStringLiteral("sizeTicks"),
                        join(t.sizeTicks));
            st.setValue(base + QStringLiteral("opacityTicks"),
                        join(t.opacityTicks));
        };
        save(tc->brush, QStringLiteral("storyboard/toolCtl/brush/"));
        save(tc->eraser, QStringLiteral("storyboard/toolCtl/eraser/"));
    };
    // Restore: canvas gets BOTH tools' stored values (its brush and eraser
    // state are independent members); the sliders start on the Brush.
    m_canvas->setBrushToolSize(tc->brush.size);
    m_canvas->setBrushSize(tc->brush.size);
    m_canvas->setBrushOpacity(tc->brush.opacity);
    m_canvas->setEraserSize(tc->eraser.size);
    m_canvas->setEraserOpacity(tc->eraser.opacity);
    sizeSlider->setValue(tc->brush.size);
    opacitySlider->setValue(tc->brush.opacity);
    sizeSlider->setPresets(tc->brush.sizeTicks);
    opacitySlider->setPresets(tc->brush.opacityTicks);

    // Park the pill on the canvas-facing side of the bar, centred on the
    // slider handle for the current value.
    auto positionPill = [sizeBar, pill](SizeCtlSlider *s) {
        const int handleY = int(s->yForValue(s->value()));
        const int gy = s->mapToGlobal(QPoint(0, handleY)).y();
        const int barLeft = sizeBar->mapToGlobal(QPoint(0, 0)).x();
        const int bx = sizeBar->barX();
        const int px = sizeBar->side() == 0
            ? barLeft + bx + SizeCtlBar::kBarW + 6      // left edge: pill right
            : barLeft + bx - pill->width() - 6;         // right edge: pill left
        pill->move(px, gy - pill->height() / 2);
    };
    auto showPillFor = [pill, pillHide, positionPill](SizeCtlSlider *s) {
        pill->active = s;
        pill->setValue(s->value());
        pill->setHasPreset(s->hasPreset(s->value()));
        positionPill(s);
        pillHide->stop();
        pill->show();
        pill->raise();
    };
    auto refreshPill = [pill, positionPill](SizeCtlSlider *s, int v) {
        if (pill->active != s || !pill->isVisible())
            return;
        pill->setValue(v);
        pill->setHasPreset(s->hasPreset(v));
        positionPill(s);
    };

    sizeSlider->onAdjustBegin = [showPillFor, sizeSlider] {
        showPillFor(sizeSlider);
    };
    // Persist on release (not per move: no per-drag settings churn).
    sizeSlider->onAdjustEnd = [pillHide, saveToolCtl] {
        pillHide->start();
        saveToolCtl();
    };
    // Size drives whichever of Brush / Eraser is active, into that tool's
    // own stored state.
    sizeSlider->onChanged = [this, tc, refreshPill, sizeSlider](int v) {
        if (tc->eraserMode) {
            m_canvas->setEraserSize(v);
        } else {
            m_canvas->setBrushToolSize(v);
            // The classic Line width follows too (clamped to 1-20 inside).
            m_canvas->setBrushSize(v);
        }
        tc->active().size = v;
        refreshPill(sizeSlider, v);
    };

    opacitySlider->onAdjustBegin = [showPillFor, opacitySlider] {
        showPillFor(opacitySlider);
    };
    opacitySlider->onAdjustEnd = [pillHide, saveToolCtl] {
        pillHide->start();
        saveToolCtl();
    };
    // Opacity drives the active tool's engine live; in Brush mode it also
    // keeps the Brush Options panel's Opacity slider in step (that slider
    // writes the same canvas value back — idempotent, no loop).
    opacitySlider->onChanged =
        [this, tc, refreshPill, opacitySlider](int v) {
        if (tc->eraserMode) {
            m_canvas->setEraserOpacity(v);
        } else {
            m_canvas->setBrushOpacity(v);
            if (m_brushOpacitySlider)
                m_brushOpacitySlider->setValue(v);
        }
        tc->active().opacity = v;
        refreshPill(opacitySlider, v);
    };

    // Preset button: "+" saves the current value as a tick, "-" removes the
    // tick the current value sits on. Ticks belong to the ACTIVE tool and
    // persist per tool per slider.
    pill->onPresetToggle = [pill, pillHide, tc, sizeSlider, saveToolCtl] {
        SizeCtlSlider *s = pill->active;
        if (!s)
            return;
        const int v = s->value();
        if (s->hasPreset(v))
            s->removePreset(v);
        else
            s->addPreset(v);
        pill->setHasPreset(s->hasPreset(v));
        (s == sizeSlider ? tc->active().sizeTicks
                         : tc->active().opacityTicks) = s->presets();
        saveToolCtl();
        pillHide->start(); // linger briefly after the tap
    };
    // Hovering the pill keeps it open so the button is reachable.
    pill->onHoverChange = [pillHide](bool over) {
        if (over)
            pillHide->stop();
        else
            pillHide->start();
    };

    connect(flipButton, &QPushButton::toggled, this,
            [this] { m_canvas->toggleFlipH(); });
    // Brush-semantics external setters (brush presets / Brush Options panel):
    // they always update the BRUSH state, and touch the sliders only while
    // the sliders are showing the Brush.
    m_setSizeCtl = [tc, sizeSlider, saveToolCtl](int v) {
        tc->brush.size = qBound(1, v, 200);
        if (!tc->eraserMode)
            sizeSlider->setValue(tc->brush.size);
        saveToolCtl();
    };
    m_setOpacityCtl = [tc, opacitySlider, saveToolCtl](int v) {
        tc->brush.opacity = qBound(5, v, 100);
        if (!tc->eraserMode)
            opacitySlider->setValue(tc->brush.opacity);
        saveToolCtl();
    };

    // Switching between Brush and Eraser swaps the sliders to that tool's
    // stored size/opacity and preset ticks (the canvas keeps both tools'
    // engine values in independent members — nothing to push back). Other
    // tools leave the sliders idle on the last brush-like state.
    connect(m_canvas, &DrawingCanvas::toolChanged, this,
            [tc, sizeSlider, opacitySlider, pill, pillHide](int tool) {
        if (tool != DrawingCanvas::Brush && tool != DrawingCanvas::Eraser)
            return;
        tc->eraserMode = tool == DrawingCanvas::Eraser;
        const SizeCtlTool &t = tc->active();
        sizeSlider->setValue(t.size);        // silent: no engine writeback
        opacitySlider->setValue(t.opacity);
        sizeSlider->setPresets(t.sizeTicks);
        opacitySlider->setPresets(t.opacityTicks);
        sizeSlider->setToolTip(tc->eraserMode
            ? QStringLiteral("Eraser size") : QStringLiteral("Brush size"));
        opacitySlider->setToolTip(tc->eraserMode
            ? QStringLiteral("Eraser opacity")
            : QStringLiteral("Brush opacity"));
        pillHide->stop();
        pill->hide(); // a stale pill must never show the old tool's value
    });


    sizeBar->show(); // records intent; effective when the canvas shows
}

// Floating overlay panel over the canvas, styled after the dock headers:
// dark title bar with ONLY a Close button, draggable by that title bar.
QWidget *StoryboardPage::createFloatingPanel(const QString &title, QWidget *body)
{
    // FloatingToolWindow anchored to the canvas: header drag, clamping,
    // main-window follow, and position persistence come from the base class.
    // (The old QGraphicsDropShadowEffect is gone: a top-level window has no
    // room outside its rect, so the shadow would be clipped invisible.)
    FloatingToolWindow *panel = new FloatingToolWindow(
        m_canvas, QStringLiteral("storyboard/panelPos/") + title, this);
    panel->setObjectName(QStringLiteral("floatPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(QStringLiteral(
        "QWidget#floatPanel { background-color: #111111; border: 1px solid #2a2a2a; }"));

    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(1, 1, 1, 1); // inside the 1px border
    layout->setSpacing(0);

    QWidget *header = new QWidget;
    header->setObjectName(QStringLiteral("floatPanelHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setFixedHeight(26);
    header->setCursor(Qt::OpenHandCursor);
    header->setStyleSheet(QStringLiteral(
        "QWidget#floatPanelHeader { background-color: #161616; border-bottom: 1px solid #2a2a2a; }"));
    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(8, 0, 4, 0);
    headerLayout->setSpacing(4);
    QLabel *titleLabel = new QLabel(title);
    titleLabel->setStyleSheet(QStringLiteral(
        "color: #cccccc; font-size: 11px; border: none; background: transparent;"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);
    QToolButton *closeButton = new QToolButton;
    closeButton->setText(QString::fromUtf8("\xE2\x9C\x95")); // ✕
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setToolTip(QStringLiteral("Close"));
    closeButton->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; color: #999999; border: none;"
        " font-size: 11px; padding: 2px 6px; }"
        "QToolButton:hover { background: #262626; color: #f5a623; }"));
    connect(closeButton, &QToolButton::clicked, panel, &QWidget::hide);
    headerLayout->addWidget(closeButton);

    layout->addWidget(header);
    layout->addWidget(body);

    // Drag by the header: its unhandled mouse events (title label included)
    // propagate to the panel, whose gripRect() covers the header; the Close
    // button consumes its own clicks first. Default spot: right of the pill.
    panel->setGripWidget(header);
    panel->setDefaultOffsetProvider([] { return QPoint(84, 12); });
    return panel;
}

// Narrow settings column between the toolbar and the canvas; visible only
// while the Brush tool is active. Initial values mirror DrawingCanvas's
// brush defaults (size 25, opacity 100%, hardness 80%, P->size on).
QWidget *StoryboardPage::createBrushSettings()
{
    QWidget *body = new QWidget;
    body->setStyleSheet(QStringLiteral("background: transparent;"));

    QVBoxLayout *layout = new QVBoxLayout(body);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    const QString captionStyle = QStringLiteral("color: #777777; font-size: 10px; border: none;");
    const QString checkStyle = QStringLiteral(
        "QCheckBox { color: #cccccc; font-size: 11px; border: none; }"
        "QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #2a2a2a;"
        " border-radius: 2px; background: #1c1c1c; }"
        "QCheckBox::indicator:checked { background: #f5a623; border-color: #f5a623; }");

    // SankoSliders (slim 10/13 size); each paints its own value label, so
    // the caption is just the static name.
    auto addSlider = [&](const QString &name, int min, int max, int value,
                         SankoSlider *&outSlider) {
        QLabel *caption = new QLabel(name);
        caption->setStyleSheet(captionStyle);
        layout->addWidget(caption);
        outSlider = new SankoSlider;
        outSlider->setTrackHeight(10);
        outSlider->setHandleSize(13);
        outSlider->setRange(min, max);
        outSlider->setValue(value);
        layout->addWidget(outSlider);
    };

    // (Brush size moved to the vertical SankoSlider in the tool column; the
    // panel keeps Opacity, Hardness, pressure toggles, and the presets.)
    addSlider(QStringLiteral("Opacity"), 0, 100, 100, m_brushOpacitySlider);
    addSlider(QStringLiteral("Hardness"), 0, 100, 80, m_brushHardnessSlider);
    connect(m_brushOpacitySlider, &SankoSlider::valueChanged, this,
            [this](int v) {
        m_canvas->setBrushOpacity(v);
        if (m_setOpacityCtl)
            m_setOpacityCtl(v); // silent sync: no signal loop
    });
    connect(m_brushHardnessSlider, &SankoSlider::valueChanged, this,
            [this](int v) { m_canvas->setBrushHardness(v); });

    m_pressureSizeCheck = new QCheckBox(QString::fromUtf8("Pressure \xE2\x86\x92 Size"));
    m_pressureSizeCheck->setStyleSheet(checkStyle);
    m_pressureSizeCheck->setChecked(true); // default ON (before connect: no null-canvas call)
    connect(m_pressureSizeCheck, &QCheckBox::toggled, this,
            [this](bool on) { m_canvas->setPressureToSize(on); });
    layout->addWidget(m_pressureSizeCheck);

    m_pressureOpacityCheck = new QCheckBox(QString::fromUtf8("Pressure \xE2\x86\x92 Opacity"));
    m_pressureOpacityCheck->setStyleSheet(checkStyle);
    m_pressureOpacityCheck->setChecked(false); // default OFF
    connect(m_pressureOpacityCheck, &QCheckBox::toggled, this,
            [this](bool on) { m_canvas->setPressureToOpacity(on); });
    layout->addWidget(m_pressureOpacityCheck);

    QLabel *presetHeader = new QLabel(QStringLiteral("PRESETS"));
    presetHeader->setStyleSheet(QStringLiteral(
        "color: #888888; font-size: 10px; font-weight: 600; letter-spacing: 1px;"
        " border: none; margin-top: 6px;"));
    layout->addWidget(presetHeader);

    const QString presetStyle = QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 11px; padding: 4px; }"
        "QPushButton:hover { border-color: #f5a623; color: #f5a623; }");
    struct Preset { const char *name; int size, opacity, hardness; bool pSize, pOpacity; };
    const Preset presets[] = {
        // "Pen" replaces the old Pen tool: fixed-width, hard-edged, opaque.
        {"Pen", 4, 100, 100, false, false},
        {"Hard Pencil", 3, 100, 95, true, false},
        {"Soft Brush", 40, 80, 20, true, true},
        {"Marker", 25, 60, 70, false, false},
        {"Ink", 6, 100, 90, true, false},
    };
    for (const Preset &p : presets) {
        QPushButton *button = new QPushButton(QString::fromLatin1(p.name));
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(presetStyle);
        const Preset preset = p;
        connect(button, &QPushButton::clicked, this, [this, preset] {
            applyBrushPreset(preset.size, preset.opacity, preset.hardness,
                             preset.pSize, preset.pOpacity);
        });
        layout->addWidget(button);
    }

    m_brushPanel = createFloatingPanel(QStringLiteral("Brush Options"), body);
    m_brushPanel->setFixedWidth(170);
    m_brushPanel->adjustSize();
    m_brushPanel->setVisible(true); // Brush is the default tool; the pill's
                                    // toggled connection hides it otherwise
    return m_brushPanel;
}

// Floating overlay shown only while the Camera tool is active. Hosts the
// display-only viewport overlay toggles.
// Perspective Modifier toolbar (Figma 180:121): a 578x92 rgba(33,33,33,0.65)
// radius-8 bar holding the Density / Opacity / Thickness sliders (exact node
// offsets, 1x23 #4d4d4d dividers), the full-width Hue Colors spectrum slider,
// and the Support Drawing snap toggle. Opacity and Hue edit the SELECTED
// vanishing point (each VP keeps independent colour/opacity); Density and
// Thickness are shared. Bottom-centre, 10px above the status bar, like the
// Selection / Move Modifier bars.
QWidget *StoryboardPage::createPerspectiveModifier()
{
    PerspectiveTool *persp = m_canvas->perspective();
    PerspModBar *bar = new PerspModBar(m_canvas, this); // solid #212121 body
    m_perspModToolbar = bar;
    bar->setFixedSize(578, 92);

    PerspSlider *density = new PerspSlider(QStringLiteral("Density"), 2, 40,
                                           PerspSlider::Value, bar);
    density->setFixedSize(160, 25);
    density->move(10, 29);
    PerspSlider *opacity = new PerspSlider(QStringLiteral("Opacity"), 5, 100,
                                           PerspSlider::Value, bar);
    opacity->setFixedSize(160, 25);
    opacity->move(209, 29);
    PerspSlider *thickness = new PerspSlider(QStringLiteral("Thickness"), 1, 6,
                                             PerspSlider::Value, bar);
    thickness->setFixedSize(160, 25);
    thickness->move(408, 29);
    PerspSlider *hue = new PerspSlider(QString(), 0, 359, // track-only
                                       PerspSlider::Hue, bar);
    hue->setFixedSize(558, 12);
    hue->move(10, 67);

    for (int x : {189, 388}) {
        QFrame *divider = new QFrame(bar);
        divider->setStyleSheet(QStringLiteral("background:#4d4d4d;border:none;"));
        divider->setFixedSize(1, 23);
        divider->move(x, 30);
    }

    QFont toggleFont(QStringLiteral("Inter"));
    toggleFont.setPixelSize(8);
    toggleFont.setWeight(QFont::DemiBold);
    QLabel *guidesLabel = new QLabel(QStringLiteral("Show Guides"), bar);
    guidesLabel->setFont(toggleFont);
    guidesLabel->setStyleSheet(
        QStringLiteral("color:#cccccc;background:transparent;"));
    guidesLabel->setFixedSize(54, 11);
    guidesLabel->move(332, 10);
    PerspToggle *guides = new PerspToggle(bar);
    guides->move(392, 11);

    QLabel *supportLabel = new QLabel(QStringLiteral("Support Drawing"), bar);
    supportLabel->setFont(toggleFont);
    supportLabel->setStyleSheet(
        QStringLiteral("color:#cccccc;background:transparent;"));
    supportLabel->setFixedSize(67, 11);
    supportLabel->move(468, 10);
    PerspToggle *support = new PerspToggle(bar);
    support->move(540, 11);

    // Undo/redo: every slider drag or toggle click is ONE command on the
    // shared app stack (snapshot at gesture start, push at gesture end).
    auto wireUndo = [this](PerspSlider *slider, const QString &text) {
        slider->onBegin = [this] { m_canvas->beginPerspectiveEdit(); };
        slider->onEnd = [this, text] { m_canvas->endPerspectiveEdit(text); };
    };
    wireUndo(density, QStringLiteral("Guide Density"));
    wireUndo(opacity, QStringLiteral("Guide Opacity"));
    wireUndo(thickness, QStringLiteral("Guide Thickness"));
    wireUndo(hue, QStringLiteral("Guide Color"));

    density->onChanged = [this, persp](int v) {
        persp->setDensity(v);
        m_canvas->update();
    };
    opacity->onChanged = [this, persp](int v) {
        persp->setOpacityAll(v / 100.0); // shared across every VP
        m_canvas->update();
    };
    thickness->onChanged = [this, persp](int v) {
        persp->setThickness(v);
        m_canvas->update();
    };
    hue->onChanged = [this, persp](int v) {
        persp->setSelectedColor(QColor::fromHsv(v, 255, 255)); // per-VP
        m_canvas->update();
    };
    support->onToggled = [this, persp](bool on) {
        m_canvas->beginPerspectiveEdit();
        persp->setSnapEnabled(on);
        m_canvas->endPerspectiveEdit(QStringLiteral("Support Drawing"));
    };
    guides->onToggled = [this, persp](bool on) {
        m_canvas->beginPerspectiveEdit();
        persp->setGuidesVisible(on);
        m_canvas->endPerspectiveEdit(QStringLiteral("Show Guides"));
        m_canvas->update();
    };

    // Toolbar <- model refresh: tool activation, VP create/select (canvas
    // signal), and project load all re-sync the sliders to the selected VP.
    m_syncPerspective = [persp, density, opacity, thickness, hue, support,
                         guides] {
        density->setValue(persp->density());
        opacity->setValue(qRound(persp->selectedOpacity() * 100.0));
        thickness->setValue(qRound(persp->thickness()));
        const int h = persp->selectedColor().hsvHue();
        hue->setValue(h < 0 ? 0 : h);
        support->setOn(persp->snapEnabled());
        guides->setOn(persp->guidesVisible());
    };
    m_syncPerspective();
    connect(m_canvas, &DrawingCanvas::perspectiveEdited, this, [this] {
        if (m_syncPerspective)
            m_syncPerspective();
    });

    bar->setDefaultOffsetProvider([this, bar] {
        int statusTop = m_canvas->height();
        if (m_bottomBar && m_bottomBar->isVisible())
            statusTop = m_canvas->mapFromGlobal(
                m_bottomBar->mapToGlobal(QPoint(0, 0))).y();
        return QPoint(qMax(6, (m_canvas->width() - bar->width()) / 2),
                      qMax(6, statusTop - bar->height() - 10));
    });







    bar->setVisible(false); // shown while the Perspective tool is active
    return bar;
}

QWidget *StoryboardPage::createCameraPanel()
{
    QWidget *body = new QWidget;
    body->setStyleSheet(QStringLiteral("background: transparent;"));

    QVBoxLayout *layout = new QVBoxLayout(body);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    // Overlay toggles (display-only; never saved into the artwork).
    QPushButton *cameraFrame = toolButton(QStringLiteral("Camera Frame"),
                                          QStringLiteral("Camera frame \xE2\x80\x94 16:9 framing, dims outside"));
    cameraFrame->setChecked(true); // camera frame is ON by default
    connect(cameraFrame, &QPushButton::toggled, this,
            [this](bool on) { m_canvas->setCameraFrameEnabled(on); });
    layout->addWidget(cameraFrame);

    QPushButton *safeArea = toolButton(QStringLiteral("Safe Area"),
                                       QStringLiteral("Action-safe guide \xE2\x80\x94 5% inset"));
    connect(safeArea, &QPushButton::toggled, this,
            [this](bool on) { m_canvas->setSafeAreaEnabled(on); });
    layout->addWidget(safeArea);

    QPushButton *titleSafe = toolButton(QStringLiteral("Title Safe"),
                                        QStringLiteral("Title-safe guide \xE2\x80\x94 10% inset"));
    connect(titleSafe, &QPushButton::toggled, this,
            [this](bool on) { m_canvas->setTitleSafeEnabled(on); });
    layout->addWidget(titleSafe);

    m_cameraPanel = createFloatingPanel(QStringLiteral("Camera"), body);
    m_cameraPanel->setFixedWidth(170);
    m_cameraPanel->adjustSize();
    m_cameraPanel->setVisible(false); // Brush is the default tool
    return m_cameraPanel;
}

// Floating overlay shown only while the Shapes tool is active: shape
// selector, stroke width, and the fill toggle.
QWidget *StoryboardPage::createShapesPanel()
{
    QWidget *body = new QWidget;
    body->setStyleSheet(QStringLiteral("background: transparent;"));

    QVBoxLayout *layout = new QVBoxLayout(body);
    layout->setContentsMargins(10, 8, 10, 10);
    layout->setSpacing(6);

    // Shape selector (exclusive), Rectangle default.
    QButtonGroup *kinds = new QButtonGroup(this);
    kinds->setExclusive(true);
    auto addKind = [&](const QString &text, const QString &tip,
                       DrawingCanvas::ShapeKind kind, bool checked) {
        QPushButton *button = toolButton(text, tip);
        button->setChecked(checked);
        kinds->addButton(button);
        connect(button, &QPushButton::toggled, this, [this, kind](bool on) {
            if (on && m_canvas)
                m_canvas->setShapeKind(kind);
        });
        layout->addWidget(button);
    };
    addKind(QStringLiteral("Rectangle"), QStringLiteral("Click-drag corner to corner"),
            DrawingCanvas::ShapeRectangle, true);
    addKind(QStringLiteral("Triangle"),
            QStringLiteral("Click-drag \xE2\x80\x94 isosceles triangle in the box"),
            DrawingCanvas::ShapeTriangle, false);
    addKind(QStringLiteral("Circle"), QStringLiteral("Click-drag \xE2\x80\x94 ellipse in the box"),
            DrawingCanvas::ShapeCircle, false);
    addKind(QStringLiteral("Line"), QStringLiteral("Click-drag start to end"),
            DrawingCanvas::ShapeLine, false);
    addKind(QStringLiteral("Polygon"),
            QStringLiteral("Click to place vertices; double-click or Enter closes, Esc cancels"),
            DrawingCanvas::ShapePolygon, false);

    QLabel *strokeCaption = new QLabel(QStringLiteral("Stroke"));
    strokeCaption->setStyleSheet(QStringLiteral("color: #777777; font-size: 10px; border: none;"));
    layout->addWidget(strokeCaption);
    SankoSlider *stroke = new SankoSlider;
    stroke->setTrackHeight(10);
    stroke->setHandleSize(13);
    stroke->setRange(1, 100);
    stroke->setValue(4); // mirrors the canvas default
    connect(stroke, &SankoSlider::valueChanged, this, [this](int v) {
        if (m_canvas)
            m_canvas->setShapeStrokeWidth(v);
    });
    layout->addWidget(stroke);

    QCheckBox *fillCheck = new QCheckBox(QStringLiteral("Fill"));
    fillCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { color: #cccccc; font-size: 11px; border: none; }"
        "QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid #2a2a2a;"
        " border-radius: 2px; background: #1c1c1c; }"
        "QCheckBox::indicator:checked { background: #f5a623; border-color: #f5a623; }"));
    fillCheck->setChecked(false); // default OFF = outline only
    connect(fillCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_canvas)
            m_canvas->setShapeFill(on);
    });
    layout->addWidget(fillCheck);

    m_shapesPanel = createFloatingPanel(QStringLiteral("Shapes"), body);
    m_shapesPanel->setFixedWidth(170);
    m_shapesPanel->adjustSize();
    m_shapesPanel->setVisible(false); // Brush is the default tool
    return m_shapesPanel;
}

void StoryboardPage::setActionSafeMaskOpacity(int percent)
{
    if (m_canvas)
        m_canvas->setActionSafeMaskOpacity(percent);
}

void StoryboardPage::setTitleSafeMaskOpacity(int percent)
{
    if (m_canvas)
        m_canvas->setTitleSafeMaskOpacity(percent);
}

// Presets drive the UI controls; their change signals push into the canvas,
// so the sliders, checkboxes, and brush engine always agree.
void StoryboardPage::applyBrushPreset(int size, int opacityPct, int hardnessPct,
                                      bool pressureSize, bool pressureOpacity)
{
    if (m_setSizeCtl)
        m_setSizeCtl(size);
    if (m_setOpacityCtl)
        m_setOpacityCtl(opacityPct);
    if (m_brushOpacitySlider)
        m_brushOpacitySlider->setValue(opacityPct);
    if (m_brushHardnessSlider)
        m_brushHardnessSlider->setValue(hardnessPct);
    if (m_pressureSizeCheck)
        m_pressureSizeCheck->setChecked(pressureSize);
    if (m_pressureOpacityCheck)
        m_pressureOpacityCheck->setChecked(pressureOpacity);
}

void StoryboardPage::rebuildPanelStrip()
{
    m_panelThumbs.clear();
    m_panelThumbImages.clear();
    while (QLayoutItem *item = m_panelStripLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    Scene *scene = currentScene();
    if (scene) {
        for (int i = 0; i < scene->panels.size(); ++i) {
            Panel *panel = scene->panels.at(i);

            QLabel *thumb = new QLabel;
            thumb->setObjectName(QStringLiteral("panelThumb"));
            thumb->setProperty("panelIndex", i);
            thumb->setFixedSize(kThumbW, kThumbH);
            thumb->setCursor(Qt::PointingHandCursor);
            thumb->setScaledContents(true);
            thumb->setPixmap(panel->flattenedPixmap().scaled(kThumbW, kThumbH,
                                                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            thumb->installEventFilter(this);

            // Panel number overlay, bottom-left.
            QLabel *num = new QLabel(QStringLiteral("P%1").arg(i + 1), thumb);
            num->setStyleSheet(QStringLiteral(
                "color: #f5a623; font-size: 11px; font-weight: 700;"
                " background: rgba(0,0,0,140); padding: 1px 4px; border-radius: 3px;"));
            num->move(5, kThumbH - 20);

            m_panelStripLayout->addWidget(thumb, 0, Qt::AlignVCenter);
            m_panelThumbs.append(thumb);
            m_panelThumbImages.append(thumb);
        }
    }

    // (Add Panel now lives ONLY in the fixed control column at the left of the strip.)
    m_panelStripLayout->addStretch(1);
    updatePanelThumbStyles();
}

void StoryboardPage::updatePanelThumbStyles()
{
    for (int i = 0; i < m_panelThumbs.size(); ++i) {
        const bool selected = (i == m_currentPanel);
        m_panelThumbs.at(i)->setStyleSheet(
            selected
                ? QStringLiteral("QLabel#panelThumb { border: 3px solid #f5a623; border-radius: 4px;"
                                 " background-color: #1a1a1a; }")
                : QStringLiteral("QLabel#panelThumb { border: 1px solid #2a2a2a; border-radius: 4px;"
                                 " background-color: #1a1a1a; }"));
    }
}

// --- Right column ---------------------------------------------------------

QWidget *StoryboardPage::createRightColumn()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setMinimumWidth(180); // dock-resizable (was fixed 220)
    column->setStyleSheet(QStringLiteral("background-color: #111111;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    // (No inner heading: the ADS dock tab already names the panel.)

    const QString labelStyle = QStringLiteral("color: #888888; font-size: 11px;");
    const QString fieldStyle = QStringLiteral(
        "QComboBox, QLineEdit, QPlainTextEdit {"
        "  background-color: #1a1a1a; color: #ffffff; border: 1px solid #2a2a2a;"
        "  border-radius: 4px; padding: 5px; font-size: 12px;"
        "}"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background-color: #1a1a1a; color: #ffffff;"
        "  selection-background-color: #f5a623; selection-color: #0a0a0a; }");

    auto addLabel = [&](const QString &text) {
        QLabel *l = new QLabel(text);
        l->setStyleSheet(labelStyle);
        layout->addWidget(l);
    };

    addLabel(QStringLiteral("Shot type"));
    m_shotType = new QComboBox;
    m_shotType->addItems({QStringLiteral("Close-up"), QStringLiteral("Medium"),
                          QStringLiteral("Wide"), QStringLiteral("Extreme Close-up"),
                          QStringLiteral("Extreme Wide"), QStringLiteral("Over-the-shoulder")});
    m_shotType->setStyleSheet(fieldStyle);
    layout->addWidget(m_shotType);

    addLabel(QStringLiteral("Camera angle"));
    m_camera = new QComboBox;
    m_camera->addItems({QStringLiteral("Eye level"), QStringLiteral("Low angle"),
                        QStringLiteral("High angle"), QStringLiteral("Bird's eye"),
                        QStringLiteral("Dutch angle")});
    m_camera->setStyleSheet(fieldStyle);
    layout->addWidget(m_camera);

    addLabel(QStringLiteral("Lens"));
    m_lens = new QComboBox;
    m_lens->addItems({QStringLiteral("Wide (16-24mm)"), QStringLiteral("Normal (35-50mm)"),
                      QStringLiteral("Telephoto (85mm+)")});
    m_lens->setStyleSheet(fieldStyle);
    layout->addWidget(m_lens);

    addLabel(QStringLiteral("Mood"));
    m_mood = new QLineEdit;
    m_mood->setPlaceholderText(QStringLiteral("e.g. tense, calm, mysterious"));
    m_mood->setStyleSheet(fieldStyle);
    layout->addWidget(m_mood);

    addLabel(QStringLiteral("Notes"));
    m_notes = new QPlainTextEdit;
    m_notes->setPlaceholderText(QStringLiteral("Director notes for this panel..."));
    m_notes->setStyleSheet(fieldStyle);
    m_notes->setFixedHeight(90);
    layout->addWidget(m_notes);

    // Parent scene action.
    QLabel *actionTitle = new QLabel(QStringLiteral("Action"));
    actionTitle->setStyleSheet(labelStyle);
    layout->addWidget(actionTitle);

    m_actionLabel = new QLabel;
    m_actionLabel->setWordWrap(true);
    m_actionLabel->setStyleSheet(QStringLiteral(
        "color: #777777; font-size: 12px; font-style: italic;"));
    layout->addWidget(m_actionLabel);

    layout->addStretch(1);

    // Auto-save on any change.
    connect(m_shotType, &QComboBox::currentTextChanged, this, [this] { saveShotInfo(); });
    connect(m_camera, &QComboBox::currentTextChanged, this, [this] { saveShotInfo(); });
    connect(m_lens, &QComboBox::currentTextChanged, this, [this] { saveShotInfo(); });
    connect(m_mood, &QLineEdit::textChanged, this, [this] { saveShotInfo(); });
    connect(m_notes, &QPlainTextEdit::textChanged, this, [this] { saveShotInfo(); });

    return column;
}

// --- Bottom bar -----------------------------------------------------------

QWidget *StoryboardPage::createBottomBar()
{
    QWidget *bar = new QWidget;
    bar->setAttribute(Qt::WA_StyledBackground, true);
    bar->setFixedHeight(56);
    bar->setStyleSheet(QStringLiteral(
        "background-color: #111111; border-top: 1px solid #2a2a2a;"));

    QHBoxLayout *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(16, 0, 16, 0);

    QPushButton *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90  Script Editor"));
    back->setCursor(Qt::PointingHandCursor);
    back->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #999999; border: none; font-size: 14px;"
        " padding: 8px 6px; } QPushButton:hover { color: #ffffff; }"));
    connect(back, &QPushButton::clicked, this, &StoryboardPage::backRequested);
    layout->addWidget(back);

    QPushButton *consistency = new QPushButton(QStringLiteral("Consistency Board"));
    consistency->setCursor(Qt::PointingHandCursor);
    consistency->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 7px 14px; font-size: 13px; }"
        "QPushButton:hover { color: #f5a623; border-color: #f5a623; }"));
    connect(consistency, &QPushButton::clicked, this, &StoryboardPage::consistencyBoardRequested);
    layout->addWidget(consistency);

    // (Duplicate Panel moved to the fixed control column on the panel strip.)

    m_importButton = new QPushButton(QStringLiteral("Import Image"));
    m_importButton->setCursor(Qt::PointingHandCursor);
    m_importButton->setToolTip(QStringLiteral("Import a reference image onto this panel (Ctrl+I)"));
    m_importButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 6px; padding: 7px 14px; font-size: 13px; }"
        "QPushButton:hover { color: #f5a623; border-color: #f5a623; }"
        "QPushButton:disabled { color: #555555; border-color: #1f1f1f; }"));
    connect(m_importButton, &QPushButton::clicked, this, &StoryboardPage::importImageToPanel);
    layout->addWidget(m_importButton);

    layout->addStretch(1);

    QPushButton *animatic = new QPushButton(QStringLiteral("Continue to Animatic"));
    animatic->setCursor(Qt::PointingHandCursor);
    animatic->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #ffffff; color: #0a0a0a; border: none;"
        " border-radius: 6px; padding: 9px 22px; font-size: 14px; font-weight: 600; }"
        "QPushButton:hover { background-color: #e6e6e6; }"));
    connect(animatic, &QPushButton::clicked, this,
            [this] { emit continueToAnimaticRequested(m_scenes); });
    layout->addWidget(animatic);

    return bar;
}

// --- Data / selection -----------------------------------------------------

void StoryboardPage::loadScenes(const QVector<Scene *> &scenes)
{
    if (m_undoStack)
        m_undoStack->clear(); // a fresh document starts with a fresh history
    m_scenes = scenes; // non-owning; MainWindow owns the Scene/Panel objects
    m_currentScene = -1;
    m_currentPanel = -1;

    rebuildSceneList();
    if (!m_scenes.isEmpty())
        selectScene(0);
    else
        rebuildPanelStrip();
}

void StoryboardPage::selectScene(int index)
{
    if (index < 0 || index >= m_scenes.size())
        return;
    m_currentScene = index;
    updateSceneCardStyles();

    Scene *scene = m_scenes.at(index);
    m_actionLabel->setText(scene->action);

    rebuildPanelStrip();
    if (!scene->panels.isEmpty())
        selectPanel(0);
    else {
        m_currentPanel = -1;
        m_canvas->setActivePanel(nullptr);
        updateDuplicateButton();
    }
}

void StoryboardPage::selectPanel(int index)
{
    Scene *scene = currentScene();
    if (!scene || index < 0 || index >= scene->panels.size())
        return;
    flushThumbRefresh(); // finalize the OLD panel's debounced thumbnail
    m_currentPanel = index;
    m_canvas->setActivePanel(scene->panels.at(index));
    updatePanelThumbStyles();
    loadShotInfo();
    updateOnionGhost();
    updateLightTable();
    updateDuplicateButton();
    rebuildLayerPanel();
}

void StoryboardPage::updateOnionGhost()
{
    if (!m_canvas)
        return;
    Scene *scene = currentScene();
    const bool show = m_canvas->isOnionSkinEnabled() && scene
        && m_currentPanel > 0 && m_currentPanel < scene->panels.size();
    if (show)
        m_canvas->setPreviousPixmap(scene->panels.at(m_currentPanel - 1)->flattenedPixmap());
    else
        m_canvas->setPreviousPixmap(QPixmap()); // clears the ghost
}

// --- App-wide undo plumbing ---------------------------------------------------

void StoryboardPage::setUndoStack(QUndoStack *stack)
{
    m_undoStack = stack;
    if (m_canvas)
        m_canvas->setUndoStack(stack);
}

// Command callbacks: mutate the scene's panel list and refresh, jumping to
// the affected scene first so undo lands where the user can see it.
void StoryboardPage::applyPanelInsertForUndo(Scene *scene, int index, Panel *panel)
{
    const int sceneIdx = m_scenes.indexOf(scene);
    if (sceneIdx < 0 || !panel)
        return;
    if (sceneIdx != m_currentScene)
        selectScene(sceneIdx);
    index = qBound(0, index, int(scene->panels.size()));
    scene->panels.insert(index, panel);
    rebuildPanelStrip();
    selectPanel(index);
    updateDuplicateButton();
    if (m_panelScroll && index < m_panelThumbs.size())
        m_panelScroll->ensureWidgetVisible(m_panelThumbs.at(index));
}

Panel *StoryboardPage::applyPanelRemoveForUndo(Scene *scene, int index)
{
    const int sceneIdx = m_scenes.indexOf(scene);
    if (sceneIdx < 0 || index < 0 || index >= scene->panels.size())
        return nullptr;
    if (sceneIdx != m_currentScene)
        selectScene(sceneIdx);
    Panel *panel = scene->panels.takeAt(index);
    rebuildPanelStrip();
    if (!scene->panels.isEmpty())
        selectPanel(qBound(0, index, int(scene->panels.size()) - 1));
    updateDuplicateButton();
    return panel;
}

void StoryboardPage::applyPanelMoveForUndo(Scene *scene, int from, int to)
{
    const int sceneIdx = m_scenes.indexOf(scene);
    if (sceneIdx < 0 || from < 0 || from >= scene->panels.size() || to < 0
        || to >= scene->panels.size())
        return;
    if (sceneIdx != m_currentScene)
        selectScene(sceneIdx);
    scene->panels.move(from, to);
    rebuildPanelStrip();
    selectPanel(to);
}

void StoryboardPage::addPanelToScene(int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= m_scenes.size())
        return;
    if (sceneIndex != m_currentScene)
        selectScene(sceneIndex);

    Scene *scene = m_scenes.at(sceneIndex);
    if (!m_undoStack)
        return;
    m_undoStack->push(new InsertPanelCommand(this, scene, scene->panels.size(),
                                             makePanel(),
                                             QStringLiteral("Add Panel")));
}

// --- Shot info ------------------------------------------------------------

void StoryboardPage::loadShotInfo()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    m_loadingShotInfo = true;
    m_shotType->setCurrentText(panel->shotType);
    m_camera->setCurrentText(panel->cameraAngle);
    m_lens->setCurrentText(panel->lens);
    m_mood->setText(panel->mood);
    m_notes->setPlainText(panel->notes);
    m_loadingShotInfo = false;
}

void StoryboardPage::saveShotInfo()
{
    if (m_loadingShotInfo)
        return;
    Panel *panel = currentPanel();
    if (!panel)
        return;
    panel->shotType = m_shotType->currentText();
    panel->cameraAngle = m_camera->currentText();
    panel->lens = m_lens->currentText();
    panel->mood = m_mood->text();
    panel->notes = m_notes->toPlainText();
}

// Debounced: every stroke commit / opacity tick used to re-flatten the WHOLE
// stack and smooth-scale it — with many layers that alone made drawing lag.
// The pixel-perfect refresh now runs once, shortly after changes settle.
void StoryboardPage::refreshCurrentThumb()
{
    if (!m_thumbTimer) {
        m_thumbTimer = new QTimer(this);
        m_thumbTimer->setSingleShot(true);
        m_thumbTimer->setInterval(120);
        connect(m_thumbTimer, &QTimer::timeout, this, [this] {
            refreshCurrentThumbNow();
            updateActiveLayerThumb(); // the changed layer's row preview
        });
    }
    m_thumbTimer->start(); // restarts while edits keep coming
}

// A pending debounced refresh must land on the panel it was queued for —
// run it NOW before the current panel changes.
void StoryboardPage::flushThumbRefresh()
{
    if (m_thumbTimer && m_thumbTimer->isActive()) {
        m_thumbTimer->stop();
        refreshCurrentThumbNow();
        updateActiveLayerThumb();
    }
}

void StoryboardPage::refreshCurrentThumbNow()
{
    Panel *panel = currentPanel();
    if (!panel || m_currentPanel < 0 || m_currentPanel >= m_panelThumbImages.size())
        return;
    m_panelThumbImages.at(m_currentPanel)
        ->setPixmap(panel->flattenedPixmap().scaled(kThumbW, kThumbH,
                                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// --- Helpers --------------------------------------------------------------

Scene *StoryboardPage::currentScene() const
{
    if (m_currentScene < 0 || m_currentScene >= m_scenes.size())
        return nullptr;
    return m_scenes.at(m_currentScene);
}

Panel *StoryboardPage::currentPanel() const
{
    Scene *scene = currentScene();
    if (!scene || m_currentPanel < 0 || m_currentPanel >= scene->panels.size())
        return nullptr;
    return scene->panels.at(m_currentPanel);
}

bool StoryboardPage::eventFilter(QObject *object, QEvent *event)
{
    // Floating toolbars/panels manage themselves now: FloatingToolWindow's
    // shared manager watches the canvas and the main window, handling drag,
    // clamping, follow, and show/hide for every registered instance.

    // Floating Brush bar tooltips: one reused popup BELOW the bar (4px gap,
    // never covering a button), driven by Enter/Leave so chain-hovering
    // retargets the same widget without any hide/show flicker.
    if (QWidget *w = qobject_cast<QWidget *>(object);
        w && m_floatToolbar && m_toolbarTip && w->window() == m_floatToolbar
        && !w->toolTip().isEmpty()) {
        switch (event->type()) {
        case QEvent::ToolTip:
            return true; // the popup pipeline replaces Qt's at-cursor tip
        case QEvent::Enter:
            m_toolbarTip->scheduleShow(w, m_floatToolbar);
            break;
        case QEvent::Leave:
            m_toolbarTip->scheduleHide();
            break;
        case QEvent::MouseButtonPress:
            m_toolbarTip->hideNow(); // a click dismisses the tip
            break;
        default:
            break;
        }
    }

    // Panel thumbnails: select on click, drag to reorder.
    const QVariant panelIdx = object->property("panelIndex");
    if (panelIdx.isValid()) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                selectPanel(panelIdx.toInt());
                m_dragSourceIndex = panelIdx.toInt();
                m_dragStartGlobal = me->globalPosition().toPoint();
                m_panelPressActive = true;
                m_panelDragging = false;
            }
            return false; // let the label keep the implicit mouse grab
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (!m_panelPressActive || !(me->buttons() & Qt::LeftButton))
                return false;
            const QPoint g = me->globalPosition().toPoint();
            if (!m_panelDragging
                && (g - m_dragStartGlobal).manhattanLength() >= 8) {
                beginPanelDrag();
            }
            if (m_panelDragging) {
                updatePanelDrag(g);
                return true;
            }
            return false;
        }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && m_panelDragging) {
                finishPanelDrag();
                return true;
            }
            m_panelPressActive = false;
            return false;
        }
        default:
            break;
        }
    }

    // Scene cards: select on click.
    if (event->type() == QEvent::MouseButtonPress
        && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        const QVariant sceneIdx = object->property("sceneIndex");
        if (sceneIdx.isValid()) {
            selectScene(sceneIdx.toInt());
            return false;
        }
    }
    return QWidget::eventFilter(object, event);
}

// --- Panel reordering -----------------------------------------------------

void StoryboardPage::beginPanelDrag()
{
    Scene *scene = currentScene();
    if (!scene || m_dragSourceIndex < 0 || m_dragSourceIndex >= m_panelThumbImages.size())
        return;
    m_panelDragging = true;

    // Semi-transparent ghost that follows the cursor.
    m_dragGhost = new QLabel(nullptr, Qt::FramelessWindowHint | Qt::Tool
                                          | Qt::WindowStaysOnTopHint
                                          | Qt::WindowTransparentForInput);
    m_dragGhost->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_dragGhost->setAttribute(Qt::WA_TranslucentBackground, true);
    m_dragGhost->setWindowOpacity(0.6);
    m_dragGhost->setFixedSize(kThumbW, kThumbH);
    m_dragGhost->setScaledContents(true);
    m_dragGhost->setPixmap(m_panelThumbImages.at(m_dragSourceIndex)->pixmap());
    m_dragGhost->show();

    // Amber drop-position indicator, drawn inside the strip container.
    QWidget *container = m_panelStripLayout->parentWidget();
    m_dropIndicator = new QWidget(container);
    m_dropIndicator->setStyleSheet(QStringLiteral("background-color: #f5a623;"));
    m_dropIndicator->hide();
}

void StoryboardPage::updatePanelDrag(const QPoint &globalPos)
{
    if (m_dragGhost)
        m_dragGhost->move(globalPos.x() - kThumbW / 2, globalPos.y() - kThumbH / 2);

    m_dropTarget = dropTargetForX(globalPos);

    if (m_dropIndicator && !m_panelThumbs.isEmpty()) {
        const QRect first = m_panelThumbs.first()->geometry();
        int x;
        if (m_dropTarget < m_panelThumbs.size())
            x = m_panelThumbs.at(m_dropTarget)->geometry().left() - 7;
        else
            x = m_panelThumbs.last()->geometry().right() + 5;
        m_dropIndicator->setGeometry(x, first.top(), 2, first.height());
        m_dropIndicator->raise();
        m_dropIndicator->show();
    }
}

int StoryboardPage::dropTargetForX(const QPoint &globalPos) const
{
    QWidget *container = m_panelStripLayout->parentWidget();
    if (!container)
        return 0;
    const int cx = container->mapFromGlobal(globalPos).x();
    int target = 0;
    for (int i = 0; i < m_panelThumbs.size(); ++i) {
        if (cx > m_panelThumbs.at(i)->geometry().center().x())
            target = i + 1;
        else
            break;
    }
    return target;
}

void StoryboardPage::finishPanelDrag()
{
    const int src = m_dragSourceIndex;
    const int target = m_dropTarget;
    cancelPanelDrag(); // tears down ghost/indicator, resets flags
    if (src >= 0)
        movePanel(src, target);
}

void StoryboardPage::cancelPanelDrag()
{
    if (m_dragGhost) {
        m_dragGhost->deleteLater();
        m_dragGhost = nullptr;
    }
    if (m_dropIndicator) {
        m_dropIndicator->deleteLater();
        m_dropIndicator = nullptr;
    }
    m_panelDragging = false;
    m_panelPressActive = false;
    m_dragSourceIndex = -1;
    m_dropTarget = -1;
}

void StoryboardPage::movePanel(int from, int target)
{
    Scene *scene = currentScene();
    if (!scene || from < 0 || from >= scene->panels.size())
        return;
    // Dropping right before or right after itself is a no-op.
    if (target == from || target == from + 1)
        return;
    target = qBound(0, target, scene->panels.size());

    const int dest = (target > from) ? target - 1 : target;
    if (dest == from || !m_undoStack)
        return;
    m_undoStack->push(new MovePanelCommand(this, scene, from, dest));
}

void StoryboardPage::movePanelBy(int delta)
{
    Scene *scene = currentScene();
    if (!scene || m_currentPanel < 0)
        return;
    const int dst = m_currentPanel + delta;
    if (dst < 0 || dst >= scene->panels.size())
        return; // clamp at boundaries, no wrap-around
    if (!m_undoStack)
        return;
    m_undoStack->push(new MovePanelCommand(this, scene, m_currentPanel, dst));
}

// Deep copy shared by Duplicate and the Edit-menu panel clipboard: layers
// (QImage is copy-on-write: painting detaches, so assignment is a safe deep
// copy) with fresh UUIDs so undo/UI never cross panels, plus shot metadata
// and duration. Undo history and generation state start fresh.
Panel *StoryboardPage::clonePanel(const Panel *source)
{
    Panel *copy = new Panel;
    copy->layers = source->layers;
    for (Layer &layer : copy->layers)
        layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    copy->activeLayerIndex = source->activeLayerIndex;
    copy->duration = source->duration;
    copy->shotType = source->shotType;
    copy->cameraAngle = source->cameraAngle;
    copy->lens = source->lens;
    copy->mood = source->mood;
    copy->notes = source->notes;
    return copy;
}

// Insert a clone of `panel` into the CURRENT scene at `insertAt`, then
// select it and scroll it into view. Shared by paste and duplicate.
void StoryboardPage::insertPanelClone(const Panel *panel, int insertAt,
                                      const QString &text)
{
    Scene *scene = currentScene();
    if (!scene || !panel || !m_undoStack)
        return;
    insertAt = qBound(0, insertAt, int(scene->panels.size()));
    m_undoStack->push(
        new InsertPanelCommand(this, scene, insertAt, clonePanel(panel), text));
}

void StoryboardPage::duplicatePanel()
{
    Panel *source = currentPanel();
    if (!source || m_currentPanel < 0)
        return;
    insertPanelClone(source, m_currentPanel + 1, QStringLiteral("Duplicate Panel"));
}

// --- Perspective persistence (project save/load) --------------------------------

void StoryboardPage::commitQuickShape()
{
    if (m_canvas)
        m_canvas->commitQuickShape();
}

QJsonObject StoryboardPage::perspectiveToJson() const
{
    return m_canvas->perspective()->toJson();
}

void StoryboardPage::perspectiveFromJson(const QJsonObject &object)
{
    m_canvas->perspective()->fromJson(object);
    if (m_syncPerspective)
        m_syncPerspective();
    m_canvas->update();
}

// --- Edit-menu undo/redo routing -----------------------------------------------

// Undo/Redo drive the DRAWING history (same as the Brush-bar buttons);
// Undo/Redo Selection drive the canvas's separate SELECTION history.

void StoryboardPage::editUndo()
{
    if (m_canvas)
        m_canvas->undo();
}

void StoryboardPage::editRedo()
{
    if (m_canvas)
        m_canvas->redo();
}

// --- Edit-menu clipboard routing ----------------------------------------------

// Canvas selection first, panel-level clipboard as the fallback. Paste goes
// wherever the most recent copy/cut came from.

void StoryboardPage::editCopy()
{
    if (m_canvas && m_canvas->hasSelection()) {
        m_canvas->copySelection();
        m_lastClipSource = ClipSource::Canvas;
        emit panelClipboardChanged(true); // enables the paste actions
        return;
    }
    if (currentPanel()) {
        copySelectedPanel();
        m_lastClipSource = ClipSource::PanelLevel;
    }
}

void StoryboardPage::editCut()
{
    if (m_canvas && m_canvas->hasSelection()) {
        m_canvas->cutSelection();
        if (m_canvas->hasCanvasClipboard()) { // locked layers block the cut
            m_lastClipSource = ClipSource::Canvas;
            emit panelClipboardChanged(true);
        }
        return;
    }
    if (currentPanel()) {
        cutSelectedPanel();
        if (hasPanelClipboard())
            m_lastClipSource = ClipSource::PanelLevel;
    }
}

void StoryboardPage::editPaste()
{
    if (m_lastClipSource == ClipSource::Canvas && m_canvas)
        m_canvas->pasteClipboard(false); // floating, view centre
    else if (m_lastClipSource == ClipSource::PanelLevel)
        pastePanelAfterSelected();
}

void StoryboardPage::editPasteInPlace()
{
    if (m_lastClipSource == ClipSource::Canvas && m_canvas)
        m_canvas->pasteClipboard(true); // exact copied coordinates
    else if (m_lastClipSource == ClipSource::PanelLevel)
        pastePanelInPlace();
}

// --- Panel-level clipboard ----------------------------------------------------

void StoryboardPage::copySelectedPanel()
{
    Panel *source = currentPanel();
    if (!source)
        return;
    delete m_panelClipboard;
    m_panelClipboard = clonePanel(source);
    m_clipboardSceneIndex = m_currentScene; // source position, for Paste in Place
    m_clipboardPanelIndex = m_currentPanel;
    emit panelClipboardChanged(true);
}

void StoryboardPage::cutSelectedPanel()
{
    Scene *scene = currentScene();
    Panel *source = currentPanel();
    if (!scene || !source || m_currentPanel < 0)
        return;
    if (scene->panels.size() <= 1) { // same rule as Delete
        QMessageBox::information(this, QStringLiteral("Cut Panel"),
                                 QStringLiteral("A scene must keep at least one panel."));
        return;
    }
    copySelectedPanel();
    // No confirmation (unlike Delete): the clipboard copy AND undo are the nets.
    if (!m_undoStack)
        return;
    m_undoStack->push(new RemovePanelCommand(this, scene, m_currentPanel,
                                             QStringLiteral("Cut Panel")));
}

void StoryboardPage::pastePanelAfterSelected()
{
    Scene *scene = currentScene();
    if (!scene || !m_panelClipboard)
        return;
    const int insertAt = (m_currentPanel >= 0 && m_currentPanel < scene->panels.size())
        ? m_currentPanel + 1
        : scene->panels.size();
    insertPanelClone(m_panelClipboard, insertAt, QStringLiteral("Paste Panel"));
}

void StoryboardPage::pastePanelInPlace()
{
    if (!m_panelClipboard)
        return;
    // Back into the scene/position the copy was taken from (clamped if the
    // scene shrank); no repositioning afterwards. No-op if that scene is gone.
    if (m_clipboardSceneIndex < 0 || m_clipboardSceneIndex >= m_scenes.size())
        return;
    if (m_clipboardSceneIndex != m_currentScene)
        selectScene(m_clipboardSceneIndex);
    insertPanelClone(m_panelClipboard, m_clipboardPanelIndex,
                     QStringLiteral("Paste Panel"));
}

void StoryboardPage::importImageToPanel()
{
    if (!currentPanel() || !m_canvas)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Image"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    m_canvas->importImage(path); // handles blank check, warning, undo, refresh
}

// --- Fixed panel-control column ------------------------------------------

namespace {

enum class CtrlIcon { Add, Duplicate, Clear, Delete, LightTable };

// Crisp painted glyphs (no font/emoji dependency). knockout = the colour drawn
// behind the front square of the Duplicate icon so the two squares read as
// stacked rather than as overlapping outlines.
QIcon paintCtrlIcon(CtrlIcon kind, const QColor &color, const QColor &knockout)
{
    QPixmap pm(40, 40);          // 2x for a crisp downscale to the icon size
    pm.setDevicePixelRatio(2.0); // logical 20x20 drawing grid, as before
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color);
    pen.setWidthF(1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    if (kind == CtrlIcon::Add) {
        p.drawLine(QPointF(10, 4), QPointF(10, 16));
        p.drawLine(QPointF(4, 10), QPointF(16, 10));
    } else if (kind == CtrlIcon::Duplicate) {
        p.drawRoundedRect(QRectF(7.5, 3.5, 9, 9), 1.5, 1.5);       // back square
        p.setBrush(knockout);                                     // hide the overlap
        p.drawRoundedRect(QRectF(3.5, 7.5, 9, 9), 1.5, 1.5);      // front square
        p.setBrush(Qt::NoBrush);
    } else if (kind == CtrlIcon::Clear) {
        // Panel frame with an X through the drawing area.
        p.drawRoundedRect(QRectF(3.5, 4.5, 13, 11), 1.5, 1.5);
        p.drawLine(QPointF(7, 7.5), QPointF(13, 12.5));
        p.drawLine(QPointF(13, 7.5), QPointF(7, 12.5));
    } else if (kind == CtrlIcon::LightTable) {
        // Three stacked/offset panel frames (neighbours behind the current).
        p.drawRoundedRect(QRectF(3, 6, 9, 7), 1.2, 1.2);   // back-left (prev)
        p.setBrush(knockout);
        p.drawRoundedRect(QRectF(8, 6, 9, 7), 1.2, 1.2);   // back-right (next)
        p.drawRoundedRect(QRectF(5.5, 8.5, 9, 7), 1.2, 1.2); // front (current)
        p.setBrush(Qt::NoBrush);
    } else { // Delete: trash can
        p.drawLine(QPointF(4, 6), QPointF(16, 6));                 // lid
        p.drawLine(QPointF(8, 6), QPointF(8.6, 4)); p.drawLine(QPointF(12, 6), QPointF(11.4, 4)); // handle
        p.drawLine(QPointF(6, 6.5), QPointF(6.9, 16.5));          // body left
        p.drawLine(QPointF(14, 6.5), QPointF(13.1, 16.5));       // body right
        p.drawLine(QPointF(6.9, 16.5), QPointF(13.1, 16.5));     // body bottom
        p.drawLine(QPointF(10, 8.5), QPointF(10, 14.5));         // centre rib
    }
    p.end();
    return QIcon(pm);
}

QPushButton *makeCtrlButton(CtrlIcon kind, const QColor &iconColor,
                            const QString &tooltipHtml, const QString &styleSheet)
{
    QPushButton *button = new QPushButton;
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedSize(28, 28);       // 30% down from the original 40x40
    button->setIcon(paintCtrlIcon(kind, iconColor, QColor(0x31, 0x31, 0x31))); // knockout = column bg
    button->setIconSize(QSize(12, 12)); // glyph +20%; box stays 28x28
    button->setToolTip(tooltipHtml); // Qt renders HTML tooltips automatically
    button->setStyleSheet(styleSheet);
    return button;
}

} // namespace

QWidget *StoryboardPage::createPanelControls()
{
    // Controls column (Figma tokens): 44px wide, vertical, 6px gap. Four
    // 44x30 buttons (radius 4, 1.5px #2a2a2a border), SVG icons from :/icons.
    // Order: Add, Duplicate, Light Table, Delete.
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(44);
    column->setStyleSheet(QStringLiteral("background: transparent;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Base style: 44x30, radius 4, 1.5px #2a2a2a border; `bg` fills the face.
    // `extra` appends per-button rules (hover / checked).
    auto ctrlButton = [](const QString &iconPath, const QSize &iconSize,
                         const QString &bg, const QString &tip, const QString &extra) {
        QPushButton *b = new QPushButton;
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(44, 30);
        b->setIcon(QIcon(iconPath));
        b->setIconSize(iconSize);
        b->setToolTip(tip);
        b->setStyleSheet(QStringLiteral(
            "QPushButton { background-color: %1; border: 1.5px solid #2a2a2a; border-radius: 4px; }"
            "QPushButton:disabled { border-color: #242424; }%2").arg(bg, extra));
        return b;
    };

    m_addPanelButton = ctrlButton(
        QStringLiteral(":/icons/add.svg"), QSize(13, 13), QStringLiteral("#7c6ef6"),
        QStringLiteral("<b>Add Panel</b> | Creates a new storyboard panel."),
        QStringLiteral("QPushButton:hover { background-color: #8f82f8; }"));
    connect(m_addPanelButton, &QPushButton::clicked, this, [this] { addPanelAfterSelected(); });
    layout->addWidget(m_addPanelButton);

    m_dupPanelButton = ctrlButton(
        QStringLiteral(":/icons/duplicate.svg"), QSize(17, 17), QStringLiteral("#111111"),
        QStringLiteral("<b>Duplicate Panel</b> | Copies the selected panel."),
        QStringLiteral("QPushButton:hover { border-color: #3a3a3a; }"));
    connect(m_dupPanelButton, &QPushButton::clicked, this, [this] { duplicatePanel(); });
    layout->addWidget(m_dupPanelButton);

    // Light Table — checkable toggle; amber border marks the ON state.
    m_lightTableButton = ctrlButton(
        QStringLiteral(":/icons/lighttable.svg"), QSize(14, 21), QStringLiteral("#111111"),
        QStringLiteral("<b>Light Table</b> | Ghost neighbouring panels behind the current "
                       "one (previous red, next green)."),
        QStringLiteral("QPushButton:hover { border-color: #3a3a3a; }"
                       "QPushButton:checked { border: 1.5px solid #f5a623; }"));
    m_lightTableButton->setCheckable(true);
    connect(m_lightTableButton, &QPushButton::toggled, this, [this](bool on) {
        if (m_canvas)
            m_canvas->setLightTableEnabled(on);
        updateLightTable();
    });
    layout->addWidget(m_lightTableButton);

    m_deletePanelButton = ctrlButton(
        QStringLiteral(":/icons/delete.svg"), QSize(14, 15), QStringLiteral("#111111"),
        QStringLiteral("<b>Delete Panel</b> | Removes the selected panel. Asks first."),
        QStringLiteral("QPushButton:hover { border-color: #3a3a3a; }"));
    connect(m_deletePanelButton, &QPushButton::clicked, this, [this] { deleteSelectedPanel(); });
    layout->addWidget(m_deletePanelButton);

    return column;
}

// Feed the current panel's neighbour pixmaps (within the same scene) to the
// canvas: previous tinted red, next tinted green. Display-only; recomputed
// whenever the panel changes or panels are added/removed/reordered.
void StoryboardPage::updateLightTable()
{
    if (!m_canvas)
        return;
    Scene *scene = currentScene();
    const bool on = m_lightTableButton && m_lightTableButton->isChecked();
    QPixmap prev, next;
    if (on && scene) {
        if (m_currentPanel > 0 && m_currentPanel < scene->panels.size())
            prev = scene->panels.at(m_currentPanel - 1)->flattenedPixmap();
        if (m_currentPanel >= 0 && m_currentPanel + 1 < scene->panels.size())
            next = scene->panels.at(m_currentPanel + 1)->flattenedPixmap();
    }
    m_canvas->setLightTablePixmaps(prev, next); // null pixmaps clear a side
}

void StoryboardPage::addPanelAfterSelected()
{
    Scene *scene = currentScene();
    if (!scene)
        return;
    // Insert immediately AFTER the selected panel (or append when the scene is empty).
    const int insertAt = (m_currentPanel >= 0 && m_currentPanel < scene->panels.size())
        ? m_currentPanel + 1
        : scene->panels.size();
    if (!m_undoStack)
        return;
    m_undoStack->push(new InsertPanelCommand(this, scene, insertAt, makePanel(),
                                             QStringLiteral("Add Panel")));
}

void StoryboardPage::deleteSelectedPanel()
{
    Scene *scene = currentScene();
    Panel *panel = currentPanel();
    if (!scene || !panel || m_currentPanel < 0)
        return;

    if (scene->panels.size() <= 1) {
        QMessageBox::information(this, QStringLiteral("Delete Panel"),
                                 QStringLiteral("A scene must keep at least one panel."));
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("Delete this panel?"),
        QStringLiteral("Delete the selected panel? (Undo restores it.)"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (!m_undoStack)
        return;
    m_undoStack->push(new RemovePanelCommand(this, scene, m_currentPanel,
                                             QStringLiteral("Delete Panel")));
}

void StoryboardPage::updateDuplicateButton()
{
    Scene *scene = currentScene();
    const bool hasScene = (scene != nullptr);
    const bool hasPanel = (currentPanel() != nullptr);
    if (m_addPanelButton)
        m_addPanelButton->setEnabled(hasScene);                 // needs a scene to add into
    if (m_dupPanelButton)
        m_dupPanelButton->setEnabled(hasPanel);
    if (m_clearPanelButton)
        m_clearPanelButton->setEnabled(hasPanel);
    if (m_deletePanelButton)                                     // blocked on the last remaining panel
        m_deletePanelButton->setEnabled(hasPanel && scene && scene->panels.size() > 1);
    if (m_importButton)
        m_importButton->setEnabled(hasPanel);
}

// --- Layer panel ------------------------------------------------------------

namespace {

QPushButton *layerActionButton(const QString &text, const QString &tip)
{
    QPushButton *button = new QPushButton(text);
    button->setToolTip(tip);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(26);
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 10px; }"
        "QPushButton:hover { background-color: #262626; }"
        "QPushButton:disabled { color: #555555; }"));
    return button;
}

// Row chrome for one layer entry — shared by the full rebuild and the
// in-place selection restyle so both render identically.
// Figma layerRow: default bg #161616 / border #232323; selected bg #1b1b1b /
// Sanko accent border #7C6EF6. Radius 4. The layer's colour tag shows as a
// thicker left edge stripe.
QString layerRowStyle(const Layer &layer, bool selected)
{
    QString style = selected
        ? QStringLiteral("QFrame#layerRow { background-color: #1b1b1b;"
                         " border: 1px solid #7c6ef6; border-radius: 4px; }")
        : QStringLiteral("QFrame#layerRow { background-color: #161616;"
                         " border: 1px solid #232323; border-radius: 4px; }");
    if (!layer.colorTag.isEmpty())
        style += QStringLiteral(
            "QFrame#layerRow { border-left: 3px solid %1; }")
            .arg(layer.colorTag);
    return style;
}

// 40x22 layer thumbnail over a Photoshop-style transparency checkerboard:
// transparent areas show the checker, painted pixels (pure black included)
// draw clearly on top, an empty layer shows only the checker.
QPixmap layerThumb(const Layer &layer)
{
    constexpr int kW = 40, kH = 22, kCell = 4; // subtle small squares
    QImage out(kW, kH, QImage::Format_ARGB32_Premultiplied);
    const QColor light(0x3a, 0x3a, 0x3a), dark(0x2a, 0x2a, 0x2a);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            out.setPixelColor(x, y,
                              ((x / kCell + y / kCell) & 1) ? dark : light);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!layer.image.isNull())
        painter.drawImage(QRect(0, 0, kW, kH), layer.image);
    painter.end();
    return QPixmap::fromImage(out);
}

} // namespace

QWidget *StoryboardPage::createLayerPanel()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setMinimumWidth(170); // dock-resizable (Figma is a fixed 280)
    column->setStyleSheet(QStringLiteral("background-color: #111111;"));

    // Figma 7-70: LayerList (opacity row + rows, p8 gap6) over the footer
    // toolbar strip. The dock's own title bar plays the Figma TitleBar.
    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QWidget *body = new QWidget;
    body->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    QVBoxLayout *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(8, 8, 8, 8); // Figma LayerList padding 8
    bodyLayout->setSpacing(6);                   // Figma LayerList gap 6
    layout->addWidget(body, 1);

    // Opacity row (Figma 7:92, 24px tall, gap 14): label | track | value.
    QHBoxLayout *opacityRow = new QHBoxLayout;
    opacityRow->setContentsMargins(0, 0, 0, 0);
    opacityRow->setSpacing(0);
    QLabel *opacityLabel = new QLabel(QStringLiteral("Opacity"));
    opacityLabel->setStyleSheet(QStringLiteral(
        "color: #999999; font-family: 'Inter'; font-size: 10px; border: none;"
        " background: transparent;"));
    opacityRow->addWidget(opacityLabel);
    opacityRow->addSpacing(14); // Figma gap 14

    LayerOpacitySlider *slider = new LayerOpacitySlider;
    opacityRow->addWidget(slider, 1);
    opacityRow->addSpacing(14); // Figma gap 14

    QLabel *opacityValue = new QLabel(QStringLiteral("100%"));
    opacityValue->setStyleSheet(QStringLiteral(
        "color: #cccccc; font-family: 'Inter'; font-size: 10px; border: none;"
        " background: transparent;"));
    opacityRow->addWidget(opacityValue);
    bodyLayout->addLayout(opacityRow);

    slider->onChanged = [this, opacityValue](int v) {
        opacityValue->setText(QString::number(v) + QLatin1Char('%'));
        if (m_updatingLayerUi)
            return;
        Panel *panel = currentPanel();
        Layer *layer = panel ? panel->activeLayer() : nullptr;
        if (!layer)
            return;
        layer->opacity = v / 100.0;
        refreshLayerCanvas(); // live: canvas composite + panel thumbnail
    };
    // One undo entry per opacity DRAG (snapshot at press, push at release).
    auto opacityBefore = std::make_shared<QVector<Layer>>();
    auto opacityBeforeActive = std::make_shared<int>(0);
    auto opacityStartValue = std::make_shared<int>(-1);
    slider->onDragStarted = [this, opacityBefore, opacityBeforeActive,
                             opacityStartValue, slider] {
        if (Panel *p = currentPanel()) {
            *opacityBefore = p->layers;
            *opacityBeforeActive = p->activeLayerIndex;
            *opacityStartValue = slider->value();
        }
    };
    slider->onDragFinished = [this, opacityBefore, opacityBeforeActive,
                              opacityStartValue, slider] {
        Panel *p = currentPanel();
        if (!p || *opacityStartValue < 0
            || slider->value() == *opacityStartValue)
            return;
        pushLayerCommand(p, *opacityBefore, *opacityBeforeActive,
                         QStringLiteral("Layer Opacity"));
        *opacityStartValue = -1;
    };

    // rebuildLayerPanel() syncs the active layer's opacity through here
    // (pct < 0 => no active layer => disabled).
    m_syncLayerOpacity = [slider, opacityValue](int pct) {
        const bool enabled = pct >= 0;
        const int shown = enabled ? pct : 100;
        slider->setEnabled(enabled);
        opacityValue->setEnabled(enabled);
        slider->setValueSilent(shown);
        opacityValue->setText(QString::number(shown) + QLatin1Char('%'));
    };

    // Layer rows (top = frontmost), scrollable; the host accepts row drags
    // and paints the accent insertion line at the drop gap.
    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    LayerListHost *listHost = new LayerListHost;
    m_layerListHost = listHost;
    listHost->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_layerListLayout = new QVBoxLayout(listHost);
    m_layerListLayout->setContentsMargins(0, 0, 0, 0);
    m_layerListLayout->setSpacing(6); // Figma LayerList gap
    m_layerListLayout->addStretch(1);
    listHost->onDropAtGap = [this](int visualGap) {
        Panel *panel = currentPanel();
        if (!panel)
            return;
        // Visual gap 0 = above the top (frontmost) row -> model gap = size.
        layerMoveTo(selectedLayers(), panel->layers.size() - visualGap);
    };
    scroll->setWidget(listHost);
    bodyLayout->addWidget(scroll, 1);

    // Footer toolbar (Figma "Layers Footer Toolbar"), left to right: Delete,
    // Clear, Merge, Group, Duplicate, Import Image, New Layer.
    QWidget *footer = new QWidget;
    footer->setAttribute(Qt::WA_StyledBackground, true);
    footer->setFixedHeight(27);
    footer->setStyleSheet(QStringLiteral(
        "background-color: #161616; border: none; border-top: 1px solid #2a2a2a;"));
    QHBoxLayout *footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(12, 0, 12, 0); // Figma px 12
    footerLayout->setSpacing(4);                     // Figma gap 4
    footerLayout->addStretch(1);                     // Figma justify-end

    const QSizeF iconSize(22, 18);
    LayerToolButton *deleteButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_delete.svg"), iconSize,
        QStringLiteral("Delete selected layer(s) — layers can also be dragged here"),
        footer);
    connect(deleteButton, &QPushButton::clicked, this,
            [this] { layerDeleteSelected(); });
    deleteButton->onDropLayers = [this] { layerDeleteSelected(); };
    deleteButton->enableLayerDrops();
    m_layerDeleteButton = deleteButton;
    footerLayout->addWidget(deleteButton);

    LayerToolButton *clearButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_clear.svg"), iconSize,
        QStringLiteral("Clear the selected layer(s)"), footer);
    connect(clearButton, &QPushButton::clicked, this,
            [this] { layerClearSelected(); });
    footerLayout->addWidget(clearButton);

    LayerToolButton *mergeButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_merge.svg"), iconSize,
        QStringLiteral("Merge the selected layers (single layer: merge down)"),
        footer);
    connect(mergeButton, &QPushButton::clicked, this,
            [this] { layerMergeSelected(); });
    m_layerMergeButton = mergeButton;
    footerLayout->addWidget(mergeButton);

    LayerToolButton *groupButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_group.svg"), iconSize,
        QStringLiteral("Group: flatten the selected layers into one"), footer);
    connect(groupButton, &QPushButton::clicked, this,
            [this] { layerGroupSelected(); });
    footerLayout->addWidget(groupButton);

    LayerToolButton *duplicateButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_duplicate.svg"), iconSize,
        QStringLiteral("Duplicate selected layer(s) — layers can also be dragged here"),
        footer);
    connect(duplicateButton, &QPushButton::clicked, this,
            [this] { layerDuplicateSelected(); });
    duplicateButton->onDropLayers = [this] { layerDuplicateSelected(); };
    duplicateButton->enableLayerDrops();
    footerLayout->addWidget(duplicateButton);

    LayerToolButton *importButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_import.svg"), QSizeF(16, 16),
        QStringLiteral("Import an image as a new layer"), footer);
    connect(importButton, &QPushButton::clicked, this,
            [this] { layerAddImage(); });
    footerLayout->addWidget(importButton);

    LayerToolButton *newButton = new LayerToolButton(
        QStringLiteral(":/icons/layer_newlayer.svg"), iconSize,
        QStringLiteral("New layer above the active one"), footer);
    connect(newButton, &QPushButton::clicked, this, [this] { layerAdd(); });
    footerLayout->addWidget(newButton);

    layout->addWidget(footer);
    return column;
}

void StoryboardPage::rebuildLayerPanel()
{
    if (!m_layerListLayout)
        return;
    if (m_canvas)
        m_canvas->invalidateComposite(); // stack structure/state changed

    m_layerRows.clear();
    while (QLayoutItem *item = m_layerListLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    Panel *panel = currentPanel();
    const bool hasPanel = (panel != nullptr) && !panel->layers.isEmpty();

    // Selection follows the panel: a different panel resets it to the active
    // layer; indices left stale by stack changes are dropped.
    if (panel != m_layerSelPanel) {
        m_layerSelPanel = panel;
        m_layerSelection.clear();
        m_layerAnchor = hasPanel ? panel->activeLayerIndex : -1;
    }
    if (hasPanel) {
        for (auto it = m_layerSelection.begin(); it != m_layerSelection.end();) {
            if (*it < 0 || *it >= panel->layers.size())
                it = m_layerSelection.erase(it);
            else
                ++it;
        }
        if (m_layerSelection.isEmpty())
            m_layerSelection.insert(
                qBound(0, panel->activeLayerIndex, panel->layers.size() - 1));
    } else {
        m_layerSelection.clear();
    }

    // Sync the opacity slider to the active layer (guarded — no feedback loop).
    m_updatingLayerUi = true;
    const Layer *active = hasPanel ? panel->activeLayer() : nullptr;
    const int pct = active ? qRound(qBound(0.0, active->opacity, 1.0) * 100.0) : 100;
    if (m_syncLayerOpacity)
        m_syncLayerOpacity(active ? pct : -1); // <0 => disabled
    m_updatingLayerUi = false;

    const QVector<int> sel = selectedLayers();
    if (m_layerDeleteButton)
        m_layerDeleteButton->setEnabled(!sel.isEmpty());
    if (m_layerMergeButton)
        m_layerMergeButton->setEnabled(!sel.isEmpty());

    // Mirror the row multi-selection into the canvas: with several rows
    // selected the Move tool lifts them ALL behind one box.
    if (m_canvas) {
        QStringList selIds;
        for (int i : sel)
            selIds.append(panel->layers.at(i).id);
        m_canvas->setSelectedLayerIds(selIds);
    }

    if (!hasPanel) {
        m_layerListLayout->addStretch(1);
        return;
    }

    // Collapsed folders hide their member rows.
    QSet<QString> collapsed;
    for (const Layer &layer : panel->layers)
        if (isGroupLayer(layer) && !layer.groupExpanded)
            collapsed.insert(layer.id);

    // Top row = frontmost layer = highest vector index (Figma layerRow).
    for (int i = panel->layers.size() - 1; i >= 0; --i) {
        const Layer &layer = panel->layers.at(i);
        if (!layer.groupId.isEmpty() && collapsed.contains(layer.groupId))
            continue; // inside a collapsed folder
        const bool isGroup = isGroupLayer(layer);
        const bool isSelected = m_layerSelection.contains(i);

        LayerRowFrame *row = new LayerRowFrame(i, nullptr);
        row->selected = isSelected;
        row->setProperty("layerIndex", i); // live-thumb + drag-ghost lookup
        row->setStyleSheet(layerRowStyle(layer, isSelected));

        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        // Figma pl 6 / pr 7, but the row's own 1px border sits inside the
        // frame, so the layout margins are reduced by 1px each side to land
        // the children at the exact Figma x-offsets (eye 6, thumb 25, ...).
        rowLayout->setContentsMargins(5, 0, 6, 0);
        rowLayout->setSpacing(6); // Figma gap 6

        // Folder members indent beneath their folder row.
        if (!layer.groupId.isEmpty())
            rowLayout->addSpacing(14);


        // Visibility (eye) toggle — Figma eye 13x13 (7:74 eye), native #CCC.
        QPushButton *eye = new QPushButton;
        eye->setToolTip(QStringLiteral("Show / hide layer"));
        eye->setCursor(Qt::PointingHandCursor);
        eye->setFocusPolicy(Qt::NoFocus);
        eye->setFixedSize(13, 13);
        eye->setIcon(QIcon(layerIconPm(QStringLiteral(":/icons/layer_eye.svg"),
                                       QSizeF(13, 13), QColor(),
                                       layer.visible ? 1.0 : 0.3)));
        eye->setIconSize(QSize(13, 13));
        eye->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; padding: 0; }"));
        connect(eye, &QPushButton::clicked, this, [this, i] {
            Panel *p = currentPanel();
            if (!p || i < 0 || i >= p->layers.size())
                return;
            const QVector<Layer> before = p->layers;
            const int beforeActive = p->activeLayerIndex;
            p->layers[i].visible = !p->layers[i].visible;
            pushLayerCommand(p, before, beforeActive,
                             QStringLiteral("Toggle Layer Visibility"));
            refreshLayerCanvas();
            rebuildLayerPanel();
            // A Move box lifted over now-hidden layers (or a hidden group's
            // members) is stale — drop and re-lift around what IS visible.
            m_canvas->resetTransformBox();
        });
        rowLayout->addWidget(eye, 0, Qt::AlignVCenter);

        // Folder rows: expand/collapse chevron AFTER the eye (eye first),
        // ~10% larger for visibility.
        if (isGroup) {
            QPushButton *chevron =
                new QPushButton(layer.groupExpanded
                                    ? QString::fromUtf8("\xE2\x96\xBE")   // U+25BE
                                    : QString::fromUtf8("\xE2\x96\xB8")); // U+25B8
            chevron->setToolTip(QStringLiteral("Expand / collapse group"));
            chevron->setCursor(Qt::PointingHandCursor);
            chevron->setFocusPolicy(Qt::NoFocus);
            chevron->setFixedSize(14, 14);
            chevron->setStyleSheet(QStringLiteral(
                "QPushButton { background: transparent; border: none;"
                " color: #999999; font-size: 10px; padding: 0; }"));
            connect(chevron, &QPushButton::clicked, this, [this, i] {
                Panel *p = currentPanel();
                if (!p || i < 0 || i >= p->layers.size())
                    return;
                p->layers[i].groupExpanded = !p->layers[i].groupExpanded;
                rebuildLayerPanel(); // pure UI state: not an undo entry
            });
            rowLayout->addWidget(chevron, 0, Qt::AlignVCenter);
        }

        // Layer thumbnail (Figma th 40x22, bg #1A1A1A / border #2A2A2A / r2).
        // Group rows drop the thumbnail box entirely and show ONLY the folder
        // icon (no square behind it), sized up to Photoshop-folder proportions.
        QLabel *thumb = new QLabel;
        thumb->setObjectName(QStringLiteral("layerThumb")); // live updates
        thumb->setFixedSize(40, 22);
        thumb->setAlignment(Qt::AlignCenter);
        if (isGroup) {
            thumb->setStyleSheet(
                QStringLiteral("background: transparent; border: none;"));
            thumb->setPixmap(layerIconPm(
                QStringLiteral(":/icons/layer_group.svg"), QSizeF(22, 18)));
        } else {
            thumb->setStyleSheet(QStringLiteral(
                "background-color: #1a1a1a; border: 1px solid #2a2a2a;"
                " border-radius: 2px;"));
            thumb->setPixmap(layerThumb(layer));
        }
        rowLayout->addWidget(thumb, 0, Qt::AlignVCenter);

        // Name (Figma 7:77, Inter Regular 11px #CCC). Double-click to rename.
        QLabel *name = new QLabel(layer.name);
        name->setObjectName(QStringLiteral("layerName"));
        name->setStyleSheet(QStringLiteral(
            "color: %1; font-family: 'Inter'; font-size: 11px; border: none;"
            " background: transparent;")
            .arg(layer.visible ? QStringLiteral("#cccccc") : QStringLiteral("#666666")));
        rowLayout->addWidget(name, 0, Qt::AlignVCenter);
        rowLayout->addStretch(1); // Figma 'sp' spacer

        // Lock toggle — Figma Lock Layer 7x8 (7:74 lock), native colour.
        QPushButton *lock = new QPushButton;
        lock->setToolTip(QStringLiteral("Lock / unlock layer"));
        lock->setCursor(Qt::PointingHandCursor);
        lock->setFocusPolicy(Qt::NoFocus);
        lock->setFixedSize(7, 8);
        lock->setIcon(QIcon(layerIconPm(QStringLiteral(":/icons/layer_lock.svg"),
                                        QSizeF(7, 8),
                                        layer.locked ? QColor(0xf5, 0xa6, 0x23)
                                                     : QColor(),
                                        layer.locked ? 1.0 : 0.55)));
        lock->setIconSize(QSize(7, 8));
        lock->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; padding: 0; }"));
        connect(lock, &QPushButton::clicked, this, [this, i] {
            Panel *p = currentPanel();
            if (!p || i < 0 || i >= p->layers.size())
                return;
            const QVector<Layer> before = p->layers;
            const int beforeActive = p->activeLayerIndex;
            p->layers[i].locked = !p->layers[i].locked;
            pushLayerCommand(p, before, beforeActive,
                             QStringLiteral("Toggle Layer Lock"));
            rebuildLayerPanel();
        });
        rowLayout->addWidget(lock, 0, Qt::AlignVCenter);

        row->onPressed = [this](int idx, Qt::KeyboardModifiers mods) {
            layerRowClicked(idx, mods);
        };
        row->onCollapseToSingle = [this](int idx) {
            layerRowClicked(idx, Qt::NoModifier);
        };
        row->onDoubleClicked = [this](int idx) { layerBeginRename(idx); };
        row->onContext = [this](int idx, const QPoint &globalPos) {
            layerContextMenu(idx, globalPos);
        };
        row->onDragOut = [this](int idx) { startLayerDrag(idx); };

        m_layerListLayout->addWidget(row);
        m_layerRows.append(row);
    }
    m_layerListLayout->addStretch(1);
}

// Selection-only refresh: the stack structure is unchanged, so restyle the
// EXISTING rows and re-sync the slider/buttons/canvas mirror — with many
// layers the old full rebuild (teardown + N re-scaled thumbnails) on every
// click made the panel flicker and lag.
void StoryboardPage::updateLayerSelectionUi()
{
    Panel *panel = currentPanel();
    if (!panel || panel->layers.isEmpty() || panel != m_layerSelPanel
        || m_layerRows.isEmpty()) {
        rebuildLayerPanel(); // panel/structure changed: full rebuild
        return;
    }
    for (auto it = m_layerSelection.begin(); it != m_layerSelection.end();) {
        if (*it < 0 || *it >= panel->layers.size())
            it = m_layerSelection.erase(it);
        else
            ++it;
    }
    if (m_layerSelection.isEmpty())
        m_layerSelection.insert(
            qBound(0, panel->activeLayerIndex, panel->layers.size() - 1));

    // Same side-state as rebuildLayerPanel: opacity slider (guarded),
    // footer button enables, and the canvas's multi-selection mirror.
    m_updatingLayerUi = true;
    const Layer *active = panel->activeLayer();
    const int pct =
        active ? qRound(qBound(0.0, active->opacity, 1.0) * 100.0) : 100;
    if (m_syncLayerOpacity)
        m_syncLayerOpacity(active ? pct : -1);
    m_updatingLayerUi = false;

    const QVector<int> sel = selectedLayers();
    if (m_layerDeleteButton)
        m_layerDeleteButton->setEnabled(!sel.isEmpty());
    if (m_layerMergeButton)
        m_layerMergeButton->setEnabled(!sel.isEmpty());
    if (m_canvas) {
        QStringList selIds;
        for (int i : sel)
            selIds.append(panel->layers.at(i).id);
        m_canvas->setSelectedLayerIds(selIds);
    }

    for (QWidget *row : m_layerRows) {
        const int i = row->property("layerIndex").toInt();
        if (i < 0 || i >= panel->layers.size()) {
            rebuildLayerPanel(); // stale rows: fall back to the full rebuild
            return;
        }
        auto *frame = static_cast<LayerRowFrame *>(row);
        frame->selected = m_layerSelection.contains(i);
        row->setStyleSheet(layerRowStyle(panel->layers.at(i),
                                         frame->selected));
    }
}

void StoryboardPage::setActiveLayer(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    m_canvas->commitQuickShape(); // bake a pending shape into the OLD layer
    panel->activeLayerIndex = index;
    m_layerSelection.clear();
    m_layerSelection.insert(index);
    m_layerAnchor = index;
    updateLayerSelectionUi(); // highlight + slider follow the new active layer
    m_canvas->refreshTransformBox(); // Move box follows the new target
}

// Photoshop selection rules: plain click = single select + activate, Shift =
// contiguous range from the anchor, Ctrl = toggle in/out. The pressed row
// always becomes the ACTIVE (drawing) layer.
void StoryboardPage::layerRowClicked(int index, Qt::KeyboardModifiers mods)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    m_canvas->commitQuickShape();

    if (mods & Qt::ControlModifier) {
        if (m_layerSelection.contains(index)) {
            if (m_layerSelection.size() > 1)
                m_layerSelection.remove(index);
        } else {
            m_layerSelection.insert(index);
        }
        m_layerAnchor = index;
    } else if ((mods & Qt::ShiftModifier) && m_layerAnchor >= 0
               && m_layerAnchor < panel->layers.size()) {
        m_layerSelection.clear();
        const int lo = qMin(m_layerAnchor, index);
        const int hi = qMax(m_layerAnchor, index);
        for (int i = lo; i <= hi; ++i)
            m_layerSelection.insert(i);
    } else {
        m_layerSelection.clear();
        m_layerSelection.insert(index);
        m_layerAnchor = index;
    }

    panel->activeLayerIndex = index;
    updateLayerSelectionUi(); // in place: structure unchanged by a click
    m_canvas->refreshTransformBox(); // Move box follows the new target
}

// Ascending, in-range, non-Background selection — what every layer
// operation acts on.
QVector<int> StoryboardPage::selectedLayers() const
{
    const Panel *panel = currentPanel();
    QVector<int> out;
    if (!panel)
        return out;
    for (int i : m_layerSelection) {
        if (i < 0 || i >= panel->layers.size())
            continue;
        if (panel->layers.at(i).type == QLatin1String("background"))
            continue;
        out.append(i);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void StoryboardPage::layerBeginRename(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    const int visual = panel->layers.size() - 1 - index;
    QWidget *row = m_layerRows.value(visual);
    if (!row)
        return;
    QLabel *name = row->findChild<QLabel *>(QStringLiteral("layerName"));
    if (!name)
        return;

    // Inline editor over the name label (no dialog). Enter/focus-out
    // commits, Escape cancels; the rebuild replaces the row afterwards.
    QLineEdit *edit = new QLineEdit(panel->layers.at(index).name, row);
    edit->setStyleSheet(QStringLiteral(
        "QLineEdit { background: #0f0f0f; color: #ffffff; font-size: 11px;"
        " border: 1px solid #7c6ef6; border-radius: 2px; padding: 0 2px; }"));
    edit->setGeometry(name->geometry().adjusted(-2, -3, 40, 3));
    edit->show();
    edit->setFocus();
    edit->selectAll();

    auto cancelled = std::make_shared<bool>(false);
    QShortcut *escape = new QShortcut(QKeySequence(Qt::Key_Escape), edit);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, edit, [edit, cancelled] {
        *cancelled = true;
        edit->clearFocus(); // editingFinished fires; commit is skipped
    });
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, index, cancelled] {
        const QString text = edit->text().trimmed();
        edit->deleteLater();
        Panel *p = currentPanel();
        if (!*cancelled && p && index >= 0 && index < p->layers.size()
            && !text.isEmpty() && p->layers.at(index).name != text) {
            const QVector<Layer> before = p->layers;
            const int beforeActive = p->activeLayerIndex;
            p->layers[index].name = text;
            pushLayerCommand(p, before, beforeActive,
                             QStringLiteral("Rename Layer"));
        }
        rebuildLayerPanel();
    });
}

void StoryboardPage::layerContextMenu(int index, const QPoint &globalPos)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    // Right-clicking outside the selection re-targets it (Photoshop rule).
    if (!m_layerSelection.contains(index))
        layerRowClicked(index, Qt::NoModifier);

    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background: #161616; color: #cccccc; border: 1px solid #2a2a2a; }"
        "QMenu::item { padding: 4px 18px; font-size: 11px; }"
        "QMenu::item:selected { background: #262626; color: #ffffff; }"));

    QAction *rename = menu.addAction(QStringLiteral("Rename Layer"));
    connect(rename, &QAction::triggered, this,
            [this, index] { layerBeginRename(index); });

    QMenu *colorMenu = menu.addMenu(QStringLiteral("Change Layer Color"));
    struct Tag { const char *name; const char *hex; };
    const Tag tags[] = {{"None", ""},          {"Amber", "#f5a623"},
                        {"Violet", "#7c6ef6"}, {"Red", "#e74c3c"},
                        {"Green", "#2ecc71"},  {"Blue", "#3498db"}};
    for (const Tag &tag : tags) {
        QAction *action = colorMenu->addAction(QString::fromLatin1(tag.name));
        const QString hex = QString::fromLatin1(tag.hex);
        if (!hex.isEmpty()) {
            QPixmap chip(12, 12);
            chip.fill(QColor(hex));
            action->setIcon(QIcon(chip));
        }
        connect(action, &QAction::triggered, this, [this, index, hex] {
            Panel *p = currentPanel();
            if (!p || index < 0 || index >= p->layers.size())
                return;
            const QVector<Layer> before = p->layers;
            const int beforeActive = p->activeLayerIndex;
            p->layers[index].colorTag = hex;
            pushLayerCommand(p, before, beforeActive,
                             QStringLiteral("Change Layer Color"));
            rebuildLayerPanel();
        });
    }
    QAction *custom = colorMenu->addAction(QStringLiteral("Custom..."));
    connect(custom, &QAction::triggered, this,
            [this, index] { layerSetColorTag(index); });

    QAction *duplicate = menu.addAction(QStringLiteral("Duplicate Layer"));
    connect(duplicate, &QAction::triggered, this,
            [this] { layerDuplicateSelected(); });

    QAction *duplicateTo =
        menu.addAction(QStringLiteral("Duplicate to Another Panel"));
    duplicateTo->setEnabled(
        panel->layers.at(index).type != QLatin1String("background"));
    connect(duplicateTo, &QAction::triggered, this,
            [this, index] { layerDuplicateToPanel(index); });

    if (isGroupLayer(panel->layers.at(index))) {
        menu.addSeparator();
        QAction *ungroup = menu.addAction(QStringLiteral("Ungroup"));
        connect(ungroup, &QAction::triggered, this,
                [this, index] { layerUngroup(index); });
    }

    menu.exec(globalPos);
}

void StoryboardPage::layerSetColorTag(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    const QColor start = panel->layers.at(index).colorTag.isEmpty()
        ? QColor(0xf5, 0xa6, 0x23)
        : QColor(panel->layers.at(index).colorTag);
    const QColor color =
        QColorDialog::getColor(start, this, QStringLiteral("Layer Color"));
    if (!color.isValid())
        return;
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;
    panel->layers[index].colorTag = color.name();
    pushLayerCommand(panel, before, beforeActive,
                     QStringLiteral("Change Layer Color"));
    rebuildLayerPanel();
}

void StoryboardPage::layerAdd()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;
    // Next free "Layer N": one past the highest number in use — Background +
    // "Layer 1" yields "Layer 2" (the plain size+1 skipped numbers).
    int highest = 0;
    for (const Layer &existing : panel->layers)
        if (existing.name.startsWith(QLatin1String("Layer "))) {
            bool numbered = false;
            const int n = existing.name.mid(6).toInt(&numbered);
            if (numbered)
                highest = qMax(highest, n);
        }
    Layer layer = makeRasterLayer(QStringLiteral("Layer %1").arg(highest + 1));
    int insertAt = qBound(0, panel->activeLayerIndex + 1, panel->layers.size());
    // Inserting from inside a group joins that group (block stays whole).
    if (panel->activeLayerIndex >= 0
        && panel->activeLayerIndex < panel->layers.size()) {
        const Layer &active = panel->layers.at(panel->activeLayerIndex);
        if (!active.groupId.isEmpty())
            layer.groupId = active.groupId;
    }
    panel->layers.insert(insertAt, layer);
    panel->activeLayerIndex = insertAt;
    m_layerSelection.clear();
    m_layerSelection.insert(insertAt);
    m_layerAnchor = insertAt;
    pushLayerCommand(panel, before, beforeActive, QStringLiteral("Add Layer"));
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerAddImage()
{
    Panel *panel = currentPanel();
    if (!panel || !m_canvas)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Image Layer"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;
    if (m_canvas->importImage(path)) // inserts + emits layersChanged -> rebuild
        pushLayerCommand(panel, before, beforeActive,
                         QStringLiteral("Import Image Layer"));
}

void StoryboardPage::layerDeleteSelected()
{
    Panel *panel = currentPanel();
    // A selected group row deletes its whole folder (members included);
    // ONE coherent operation and ONE undo entry regardless of selection
    // order or direction.
    const QVector<int> sel = expandGroupBlocks(selectedLayers());
    if (!panel || sel.isEmpty())
        return;
    if (!confirmLayerDelete(sel))
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;

    int removedBelowActive = 0;
    bool activeRemoved = false;
    // Descending removal keeps every remaining index in `sel` valid, so a
    // bottom-up selection deletes exactly what was selected.
    for (int k = sel.size() - 1; k >= 0; --k) {
        const int idx = sel.at(k);
        if (panel->layers.size() <= 1)
            break; // never delete the last remaining layer
        panel->layers.removeAt(idx);
        if (idx < panel->activeLayerIndex)
            ++removedBelowActive;
        else if (idx == panel->activeLayerIndex)
            activeRemoved = true;
    }
    panel->activeLayerIndex -= removedBelowActive;
    panel->activeLayerIndex =
        qBound(0, panel->activeLayerIndex, panel->layers.size() - 1);
    if (activeRemoved && panel->activeLayerIndex == 0 && panel->layers.size() > 1
        && panel->layers.first().type == QLatin1String("background"))
        panel->activeLayerIndex = 1; // land on a drawing layer, not the paper

    m_layerSelection.clear();
    m_layerSelection.insert(panel->activeLayerIndex);
    m_layerAnchor = panel->activeLayerIndex;
    pushLayerCommand(panel, before, beforeActive,
                     sel.size() == 1 ? QStringLiteral("Delete Layer")
                                     : QStringLiteral("Delete Layers"));
    refreshLayerCanvas();
    rebuildLayerPanel();
    // A Move session holding the deleted layer is stale: drop it (never
    // commit it) and re-lift around the new active layer, so the deleted
    // pixels vanish from the canvas at once.
    m_canvas->resetTransformBox();
}

void StoryboardPage::layerDuplicateSelected()
{
    Panel *panel = currentPanel();
    // Group rows duplicate their whole folder (fresh folder id, members
    // remapped into it).
    const QVector<int> sel = expandGroupBlocks(selectedLayers());
    if (!panel || sel.isEmpty())
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;

    // Copies (deep image copy, fresh id) land as one block
    // directly above the topmost selected item, keeping relative order.
    // Copies stay inside a parent group only when the whole selection lives
    // in that one group (keeps member blocks contiguous under their folder).
    QString commonGroup =
        panel->layers.at(sel.first()).groupId; // may be empty
    bool sameGroup = true;
    for (int idx : sel) {
        const Layer &src = panel->layers.at(idx);
        if (isGroupLayer(src) || src.groupId != commonGroup) {
            sameGroup = false;
            break;
        }
    }
    const int insertAt = sel.last() + 1;
    QHash<QString, QString> groupRemap; // old folder id -> new folder id
    QVector<Layer> copies;
    copies.reserve(sel.size());
    for (int idx : sel) {
        Layer copy = panel->layers.at(idx);
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!copy.image.isNull())
            copy.image = copy.image.copy();
        copy.name = copy.name + QStringLiteral(" copy");
        if (isGroupLayer(panel->layers.at(idx)))
            groupRemap.insert(panel->layers.at(idx).id, copy.id);
        copies.append(copy);
    }
    for (Layer &copy : copies) {
        if (groupRemap.contains(copy.groupId))
            copy.groupId = groupRemap.value(copy.groupId); // copied folder
        else if (!sameGroup)
            copy.groupId.clear(); // mixed selection -> copies land at root
    }
    for (int k = 0; k < copies.size(); ++k)
        panel->layers.insert(insertAt + k, copies.at(k));

    m_layerSelection.clear();
    for (int k = 0; k < copies.size(); ++k)
        m_layerSelection.insert(insertAt + k);
    panel->activeLayerIndex = insertAt + copies.size() - 1;
    m_layerAnchor = panel->activeLayerIndex;
    pushLayerCommand(panel, before, beforeActive,
                     QStringLiteral("Duplicate Layer"));
    refreshLayerCanvas();
    rebuildLayerPanel();
}

// Real deep copy into another panel. A group row copies its whole folder —
// members included — as independent layers.
void StoryboardPage::layerDuplicateToPanel(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    const Layer &source = panel->layers.at(index);
    if (source.type == QLatin1String("background"))
        return;

    QStringList labels;
    QVector<Panel *> targets;
    for (Scene *scene : m_scenes)
        for (int p = 0; p < scene->panels.size(); ++p) {
            Panel *target = scene->panels.at(p);
            if (target == panel)
                continue;
            labels << QStringLiteral("Scene %1 — Panel %2")
                          .arg(scene->number)
                          .arg(p + 1);
            targets.append(target);
        }
    if (targets.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Duplicate Layer"),
            QStringLiteral("There is no other panel to duplicate this layer into."));
        return;
    }
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, QStringLiteral("Duplicate to Another Panel"),
        QStringLiteral("Copy \"%1\" into:").arg(source.name), labels, 0,
        false, &ok);
    if (!ok)
        return;
    duplicateLayerToPanelCore(index, targets.at(labels.indexOf(choice)));
}

bool StoryboardPage::duplicateLayerToPanelCore(int index, Panel *target)
{
    Panel *panel = currentPanel();
    if (!panel || !target || target == panel || index < 0
        || index >= panel->layers.size())
        return false;
    const Layer &source = panel->layers.at(index);
    if (source.type == QLatin1String("background"))
        return false;

    const QVector<Layer> before = target->layers;
    const int beforeActive = target->activeLayerIndex;
    if (isGroupLayer(source)) {
        // Whole folder: every member copied (fresh ids, independent pixels)
        // in relative order, remapped under a fresh folder id; the folder row
        // lands directly above its members, frontmost in the target panel.
        Layer folder = source;
        folder.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (const Layer &member : panel->layers) {
            if (member.groupId != source.id)
                continue;
            Layer copy = member;
            copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            copy.groupId = folder.id;
            copy.image = copy.image.copy(); // REAL copy, independent pixels
            target->layers.append(copy);
        }
        target->layers.append(folder);
        pushLayerCommand(target, before, beforeActive,
                         QStringLiteral("Duplicate Group to Panel"));
    } else {
        Layer copy = source;
        copy.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        copy.groupId.clear();
        copy.image = copy.image.copy(); // REAL copy, independent pixels
        target->layers.append(copy);    // frontmost in the target panel
        pushLayerCommand(target, before, beforeActive,
                         QStringLiteral("Duplicate Layer to Panel"));
    }
    if (Scene *scene = currentScene()) {
        const int idx = scene->panels.indexOf(target);
        if (idx >= 0 && idx < m_panelThumbImages.size())
            m_panelThumbImages.at(idx)->setPixmap(
                target->flattenedPixmap().scaled(kThumbW, kThumbH,
                                                 Qt::IgnoreAspectRatio,
                                                 Qt::SmoothTransformation));
    }
    return true;
}

// Shared core of Merge and Group: flatten the given ascending indices into
// the LOWEST one (respecting per-layer opacity), optionally renaming it.
void StoryboardPage::mergeLayerIndices(const QVector<int> &rawIndices,
                                       const QString &newName)
{
    Panel *panel = currentPanel();
    // Merging flattens PIXELS: group folder rows can never take part.
    QVector<int> indices;
    for (int idx : rawIndices)
        if (idx >= 0 && idx < (panel ? panel->layers.size() : 0)
            && !isGroupLayer(panel->layers.at(idx)))
            indices.append(idx);
    if (!panel || indices.size() < 2)
        return;
    m_canvas->commitQuickShape();
    // An in-flight Move drag lands in the pre-merge layers BEFORE the merge
    // snapshot; the session left lifted here goes stale below and is dropped.
    m_canvas->refreshTransformBox();
    const QVector<Layer> beforeStack = panel->layers;
    const int beforeActive = panel->activeLayerIndex;

    const int targetIdx = indices.first();
    Layer &target = panel->layers[targetIdx];
    target.image.detach(); // never bake into the undo snapshot's shared handle
    QPainter painter(&target.image);
    for (int k = 1; k < indices.size(); ++k) {
        const Layer &top = panel->layers.at(indices.at(k));
        painter.setOpacity(qBound(0.0, top.opacity, 1.0));
        painter.drawImage(0, 0, top.image);
    }
    painter.end();
    if (!newName.isEmpty())
        target.name = newName;

    for (int k = indices.size() - 1; k >= 1; --k)
        panel->layers.removeAt(indices.at(k));

    panel->activeLayerIndex = targetIdx;
    m_layerSelection.clear();
    m_layerSelection.insert(targetIdx);
    m_layerAnchor = targetIdx;
    pushLayerCommand(panel, beforeStack, beforeActive,
                     QStringLiteral("Merge Layers"));
    refreshLayerCanvas();
    rebuildLayerPanel();
    m_canvas->resetTransformBox(); // the box re-wraps the merged layer
}

void StoryboardPage::layerMergeSelected()
{
    Panel *panel = currentPanel();
    const QVector<int> sel = selectedLayers();
    if (!panel || sel.isEmpty())
        return;

    if (sel.size() >= 2) {
        mergeLayerIndices(sel, QString());
        return;
    }

    // Single selection: merge down into the layer directly beneath.
    const int idx = sel.first();
    if (idx <= 0
        || panel->layers.at(idx - 1).type == QLatin1String("background"))
        return; // nothing beneath / don't bake art into the paper
    mergeLayerIndices(QVector<int>{idx - 1, idx}, QString());
}

// Group = FOLDER: the selected layers move into a new expandable group
// entry, each keeping its own pixels/settings. One level deep (a folder
// cannot hold another folder); layers already inside a folder move into the
// new one.
void StoryboardPage::layerGroupSelected()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    QVector<int> sel;
    for (int idx : selectedLayers())
        if (!isGroupLayer(panel->layers.at(idx)))
            sel.append(idx);
    if (sel.isEmpty())
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;

    // Pull the selected layers out (descending keeps indices valid),
    // preserving their relative order.
    QVector<Layer> members;
    for (int k = sel.size() - 1; k >= 0; --k)
        members.prepend(panel->layers.takeAt(sel.at(k)));

    Layer group;
    group.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    group.type = QStringLiteral("group");
    group.image = QImage(); // folders own no pixels
    int groupNumber = 1;
    for (const Layer &layer : panel->layers)
        if (isGroupLayer(layer))
            ++groupNumber;
    group.name = QStringLiteral("Group %1").arg(groupNumber);
    for (Layer &member : members)
        member.groupId = group.id;

    // The block reassembles where the topmost selected layer used to be,
    // members first, folder row directly above them.
    const int insertAt =
        qBound(1, sel.last() - int(sel.size()) + 1, panel->layers.size());
    for (int k = 0; k < members.size(); ++k)
        panel->layers.insert(insertAt + k, members.at(k));
    const int groupIdx = insertAt + members.size();
    panel->layers.insert(groupIdx, group);

    panel->activeLayerIndex = groupIdx;
    m_layerSelection.clear();
    m_layerSelection.insert(groupIdx);
    m_layerAnchor = groupIdx;
    pushLayerCommand(panel, before, beforeActive,
                     QStringLiteral("Group Layers"));
    refreshLayerCanvas();
    rebuildLayerPanel();
}

// Dissolve a folder: members stay in place, back at root.
void StoryboardPage::layerUngroup(int groupIndex)
{
    Panel *panel = currentPanel();
    if (!panel || groupIndex < 0 || groupIndex >= panel->layers.size()
        || !isGroupLayer(panel->layers.at(groupIndex)))
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;

    const QString groupId = panel->layers.at(groupIndex).id;
    panel->layers.removeAt(groupIndex);
    for (Layer &layer : panel->layers)
        if (layer.groupId == groupId)
            layer.groupId.clear();

    panel->activeLayerIndex =
        qBound(0, panel->activeLayerIndex, panel->layers.size() - 1);
    m_layerSelection.clear();
    m_layerSelection.insert(panel->activeLayerIndex);
    m_layerAnchor = panel->activeLayerIndex;
    pushLayerCommand(panel, before, beforeActive, QStringLiteral("Ungroup"));
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerClearSelected()
{
    Panel *panel = currentPanel();
    const QVector<int> sel = selectedLayers();
    if (!panel || sel.isEmpty())
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;
    bool cleared = false;
    for (int idx : sel) {
        Layer &layer = panel->layers[idx];
        if (layer.locked || isGroupLayer(layer))
            continue; // locked layers keep their pixels; folders have none
        layer.image = makeLayerImage(); // fresh transparent (detached) image
        cleared = true;
    }
    if (!cleared)
        return;
    pushLayerCommand(panel, before, beforeActive,
                     QStringLiteral("Clear Layer"));
    refreshLayerCanvas();
    rebuildLayerPanel();
}

// Drag-reorder: move the ascending source indices as ONE block whose first
// layer lands at `insertAt` (a gap in the PRE-move list, ascending model
// coordinates); relative order inside the block is preserved. The block can
// never land beneath the Background layer.
void StoryboardPage::layerMoveTo(const QVector<int> &rawSources, int insertAt)
{
    Panel *panel = currentPanel();
    // Dragging a folder row moves the whole block (members + folder).
    const QVector<int> sources = expandGroupBlocks(rawSources);
    if (!panel || sources.isEmpty())
        return;
    m_canvas->commitQuickShape();
    const QVector<Layer> before = panel->layers;
    const int beforeActive = panel->activeLayerIndex;

    const QString activeId = panel->activeLayer() ? panel->activeLayer()->id
                                                  : QString();
    bool movingGroupRow = false;
    for (int src : sources)
        if (src >= 0 && src < panel->layers.size()
            && isGroupLayer(panel->layers.at(src)))
            movingGroupRow = true;

    QVector<Layer> moved;
    moved.reserve(sources.size());
    int adjustedInsert = insertAt;
    for (int k = sources.size() - 1; k >= 0; --k) {
        const int src = sources.at(k);
        if (src < 0 || src >= panel->layers.size())
            continue;
        moved.prepend(panel->layers.takeAt(src));
        if (src < insertAt)
            --adjustedInsert;
    }
    if (moved.isEmpty())
        return;
    const bool hasBackground = !panel->layers.isEmpty()
        && panel->layers.first().type == QLatin1String("background");
    adjustedInsert = qBound(hasBackground ? 1 : 0, adjustedInsert,
                            panel->layers.size());

    // Which folder does the landing gap belong to? A gap directly above a
    // member (or between a folder's top member and its folder row) is inside
    // that folder; anywhere else is root. Folders never nest, so a dragged
    // folder row escapes to just above the target folder's row instead.
    QString targetGroup;
    if (adjustedInsert < panel->layers.size()) {
        const Layer &above = panel->layers.at(adjustedInsert);
        if (!above.groupId.isEmpty() && !isGroupLayer(above))
            targetGroup = above.groupId;
        else if (isGroupLayer(above) && adjustedInsert > 0
                 && panel->layers.at(adjustedInsert - 1).groupId == above.id)
            targetGroup = above.id;
    }
    if (movingGroupRow && !targetGroup.isEmpty()) {
        for (int i = 0; i < panel->layers.size(); ++i)
            if (panel->layers.at(i).id == targetGroup) {
                adjustedInsert = i + 1;
                break;
            }
        targetGroup.clear();
    }
    QSet<QString> movedFolderIds;
    for (const Layer &layer : moved)
        if (isGroupLayer(layer))
            movedFolderIds.insert(layer.id);
    for (Layer &layer : moved) {
        if (isGroupLayer(layer))
            continue; // folder rows always live at root
        if (movedFolderIds.contains(layer.groupId))
            continue; // whole folder moved together: membership unchanged
        layer.groupId = targetGroup; // join/leave the landing folder
    }
    for (int k = 0; k < moved.size(); ++k)
        panel->layers.insert(adjustedInsert + k, moved.at(k));

    // Selection = the moved block; the active layer keeps its identity.
    m_layerSelection.clear();
    for (int k = 0; k < moved.size(); ++k)
        m_layerSelection.insert(adjustedInsert + k);
    for (int i = 0; i < panel->layers.size(); ++i)
        if (panel->layers.at(i).id == activeId) {
            panel->activeLayerIndex = i;
            break;
        }
    panel->activeLayerIndex =
        qBound(0, panel->activeLayerIndex, panel->layers.size() - 1);
    m_layerAnchor = panel->activeLayerIndex;
    pushLayerCommand(panel, before, beforeActive,
                     QStringLiteral("Reorder Layers"));
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::startLayerDrag(int index)
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    if (!m_layerSelection.contains(index))
        layerRowClicked(index, Qt::NoModifier);
    const QVector<int> sel = selectedLayers();
    if (sel.isEmpty())
        return;

    QStringList parts;
    for (int idx : sel)
        parts << QString::number(idx);
    QMimeData *mime = new QMimeData;
    mime->setData(QString::fromLatin1(kLayersMime), parts.join(QLatin1Char(',')).toUtf8());

    QDrag *drag = new QDrag(this);
    drag->setMimeData(mime);
    // Semi-transparent ghost so the list underneath stays readable while
    // dragging (rows are looked up by their layerIndex property — collapsed
    // folders make the visual order sparse).
    for (QWidget *row : m_layerRows)
        if (row->property("layerIndex").toInt() == index) {
            const QPixmap grab = row->grab();
            QPixmap ghost(grab.size());
            ghost.setDevicePixelRatio(grab.devicePixelRatio());
            ghost.fill(Qt::transparent);
            QPainter ghostPainter(&ghost);
            ghostPainter.setOpacity(0.55);
            ghostPainter.drawPixmap(0, 0, grab);
            ghostPainter.end();
            drag->setPixmap(ghost);
            drag->setHotSpot(QPoint(grab.width() / 2, grab.height() / 2));
            break;
        }
    drag->exec(Qt::MoveAction); // drops: list gap / Delete / Duplicate
}

void StoryboardPage::refreshLayerCanvas()
{
    if (m_canvas) {
        m_canvas->invalidateComposite(); // layer state changed
        m_canvas->update();
    }
    refreshCurrentThumb();
}

// Refresh only the ACTIVE layer's row thumbnail (called on every stroke
// commit — no full list rebuild, so drawing stays fluid).
void StoryboardPage::updateActiveLayerThumb()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    const int idx = panel->activeLayerIndex;
    if (idx < 0 || idx >= panel->layers.size())
        return;
    if (isGroupLayer(panel->layers.at(idx)))
        return; // folder rows own no pixels — keep their folder icon
    for (QWidget *row : m_layerRows)
        if (row->property("layerIndex").toInt() == idx) {
            if (QLabel *thumb =
                    row->findChild<QLabel *>(QStringLiteral("layerThumb")))
                thumb->setPixmap(layerThumb(panel->layers.at(idx)));
            return;
        }
}

void StoryboardPage::applyLayerStackForUndo(Panel *panel,
                                            const QVector<Layer> &layers,
                                            int activeIndex)
{
    if (!panel)
        return;
    if (m_canvas)
        m_canvas->commitQuickShape();
    panel->layers = layers;
    panel->activeLayerIndex =
        qBound(0, activeIndex, qMax(0, panel->layers.size() - 1));
    if (panel == currentPanel()) {
        m_layerSelection.clear();
        m_layerSelection.insert(panel->activeLayerIndex);
        m_layerAnchor = panel->activeLayerIndex;
    }
    if (panel == currentPanel()) {
        refreshLayerCanvas();
        rebuildLayerPanel();
    } else if (Scene *scene = currentScene()) {
        const int idx = scene->panels.indexOf(panel);
        if (idx >= 0 && idx < m_panelThumbImages.size())
            m_panelThumbImages.at(idx)->setPixmap(
                panel->flattenedPixmap().scaled(kThumbW, kThumbH,
                                                Qt::IgnoreAspectRatio,
                                                Qt::SmoothTransformation));
    }
}

void StoryboardPage::pushLayerCommand(Panel *panel,
                                      const QVector<Layer> &before,
                                      int beforeActive, const QString &text)
{
    if (!panel || !m_undoStack)
        return;
    m_undoStack->push(new LayerStackCommand(this, panel, before, beforeActive,
                                            panel->layers,
                                            panel->activeLayerIndex, text));
}

// Any selected group row pulls its whole member block into the operation
// (delete / duplicate / move act on blocks). Ascending, deduplicated.
QVector<int> StoryboardPage::expandGroupBlocks(const QVector<int> &indices) const
{
    const Panel *panel = currentPanel();
    if (!panel)
        return indices;
    QSet<int> out;
    for (int idx : indices) {
        if (idx < 0 || idx >= panel->layers.size())
            continue;
        out.insert(idx);
        const Layer &layer = panel->layers.at(idx);
        if (isGroupLayer(layer))
            for (int i = 0; i < panel->layers.size(); ++i)
                if (panel->layers.at(i).groupId == layer.id)
                    out.insert(i);
    }
    QVector<int> sorted(out.begin(), out.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

// Delete / Cancel confirmation. EMPTY layers (no artwork anywhere in the
// selection) offer a "don't ask again" checkbox persisted in QSettings; any
// layer WITH content always asks.
bool StoryboardPage::confirmLayerDelete(const QVector<int> &indices)
{
    Panel *panel = currentPanel();
    if (!panel || indices.isEmpty())
        return false;
    bool allEmpty = true;
    for (int idx : indices)
        if (idx >= 0 && idx < panel->layers.size()
            && layerHasContent(panel->layers.at(idx))) {
            allEmpty = false;
            break;
        }

    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const bool skipEmpty =
        settings.value(QStringLiteral("storyboard/skipEmptyLayerDeleteConfirm"),
                       false).toBool();
    if (allEmpty && skipEmpty)
        return true;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Delete Layer(s)"));
    box.setText(indices.size() == 1
                    ? QStringLiteral("Permanently delete the selected layer?")
                    : QStringLiteral("Permanently delete the %1 selected layers?")
                          .arg(indices.size()));
    if (!allEmpty)
        box.setInformativeText(
            QStringLiteral("The selection contains artwork."));
    QPushButton *deleteButton =
        box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    QCheckBox *dontAsk = nullptr;
    if (allEmpty) {
        // The skip only ever applies to EMPTY layers.
        dontAsk = new QCheckBox(
            QStringLiteral("Don't ask again for empty layers"), &box);
        box.setCheckBox(dontAsk);
    }
    box.exec();
    if (box.clickedButton() != deleteButton)
        return false;
    if (dontAsk && dontAsk->isChecked())
        settings.setValue(
            QStringLiteral("storyboard/skipEmptyLayerDeleteConfirm"), true);
    return true;
}
