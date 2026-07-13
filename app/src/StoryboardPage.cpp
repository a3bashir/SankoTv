#include "StoryboardPage.h"

#include "ColorPanel.h"

#include "DrawingCanvas.h"
#include "FloatingToolWindow.h"
#include "SankoDockOverlay.h"
#include "SankoSlider.h"
#include "ZoomToolbar.h"
#include "StoryboardModel.h"

#include "DockAreaWidget.h"
#include "DockManager.h"
#include "DockWidget.h"

#include <QAction>
#include <QButtonGroup>
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

// Dock-layout schema version. Bumped to 3 for the ADS migration: layouts
// saved by the previous QMainWindow-based builds fail to parse/match and the
// default layout is applied instead.
constexpr int kDockStateVersion = 3;

// Dark theme for the ADS chrome (tabs, title bars, splitters) to match the
// app. Setting a stylesheet on CDockManager replaces ADS's bundled light one.
const char *kAdsDarkStyle =
    "ads--CDockContainerWidget { background: #0a0a0a; }"
    "ads--CDockAreaWidget { background: #111111; }"
    "ads--CDockAreaTitleBar { background: #161616; border-bottom: 1px solid #2a2a2a;"
    " padding: 0; }"
    "ads--CDockWidgetTab { background: #161616; border: 1px solid #2a2a2a;"
    " padding: 2px 6px; }"
    "ads--CDockWidgetTab QLabel { color: #cccccc; font-size: 11px; }"
    "ads--CDockWidgetTab[activeTab=\"true\"] { background: #1a1a1a; }"
    "ads--CDockWidgetTab[activeTab=\"true\"] QLabel { color: #f5a623; }"
    "ads--CDockWidget { background: #111111; border: none; }"
    "ads--CDockSplitter::handle { background: #1f1f1f; }"
    "QToolButton { background: transparent; color: #999999; border: none; }"
    "QToolButton:hover { background: #262626; color: #f5a623; }";

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

    // ADS dock manager hosts everything: native tabbing, drag-to-float,
    // re-docking, and auto-hide come with it — nothing hand-rolled.
    ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
    // Dock headers keep ONLY the Close button: no undock icon, no tabs-menu
    // chevron. (Docks can still be dragged out by their tabs.)
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
    ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);
    SankoDockManager *dockManager = new SankoDockManager(this);
    m_dockManager = dockManager;
    m_dockManager->setStyleSheet(QString::fromLatin1(kAdsDarkStyle));
    root->addWidget(m_dockManager, 1);

    // Photoshop-style drop hints: slim amber edge glow + tab-bar highlight
    // replace ADS's centre arrows and filled preview rect (visuals only;
    // parent-owned by the manager).
    new SankoDockOverlay(dockManager);

    // Central workspace: the canvas area exactly as before (tool column,
    // brush settings, panel strip, canvas) plus the bottom toolbar. The ADS
    // central widget is fixed: no tab, not closable/movable.
    QWidget *central = new QWidget;
    QVBoxLayout *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(centerColumn, 1);
    centralLayout->addWidget(bottomBar);

    ads::CDockWidget *centralDock = new ads::CDockWidget(QStringLiteral("Canvas"));
    centralDock->setObjectName(QStringLiteral("dockCanvas"));
    centralDock->setWidget(central, ads::CDockWidget::ForceNoScrollArea);
    ads::CDockAreaWidget *centralArea =
        m_dockManager->setCentralWidget(centralDock); // must precede other docks

    // Dock widgets re-parent the EXISTING panel instances (never recreated,
    // so all constructor-time connections keep firing). ADS keys saved
    // layouts on the objectName, which must be unique and stable.
    auto makeDock = [](const QString &title, const QString &objectName,
                       QWidget *panel) {
        ads::CDockWidget *dock = new ads::CDockWidget(title);
        dock->setObjectName(objectName);
        dock->setWidget(panel, ads::CDockWidget::ForceNoScrollArea);
        return dock;
    };
    m_layersDock = makeDock(QStringLiteral("Layers"), QStringLiteral("dockLayers"),
                            layersPanel);
    m_scenesDock = makeDock(QStringLiteral("Scenes"), QStringLiteral("dockScenes"),
                            scenesPanel);
    m_shotInfoDock = makeDock(QStringLiteral("Shot Info"), QStringLiteral("dockShotInfo"),
                              shotInfoPanel);

    // Default layout: Layers docked right; Scenes + Shot Info tabbed below
    // it, Scenes tab in front.
    ads::CDockAreaWidget *layersArea =
        m_dockManager->addDockWidget(ads::RightDockWidgetArea, m_layersDock);
    ads::CDockAreaWidget *pairArea =
        m_dockManager->addDockWidget(ads::BottomDockWidgetArea, m_scenesDock, layersArea);
    m_dockManager->addDockWidgetTabToArea(m_shotInfoDock, pairArea);
    m_scenesDock->setAsCurrentTab();
    // Root splitter (contains the central area): canvas | 260px panel column.
    m_dockManager->setSplitterSizes(centralArea, {1030, 260});
    // Right column's vertical splitter: Layers over the tabbed pair.
    m_dockManager->setSplitterSizes(layersArea, {300, 380});

    // Snapshot the pristine default so Reset Layout / failed restores can
    // reproduce it exactly.
    m_defaultDockState = m_dockManager->saveState(kDockStateVersion);

    // Restore ONCE, now that every dock exists. A failed restore (no saved
    // state, or a layout from the pre-ADS builds) keeps the default.
    if (!restoreDockState())
        applyDefaultDockLayout();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
            this, [this] { saveDockState(); });

    // The app's menu bar exists only after this page lands in MainWindow's
    // stack, so hook the View menu once the event loop starts.
    QTimer::singleShot(0, this, [this] { installDockViewActions(); });

    // 'O' toggles onion skin.
    QShortcut *onionShortcut = new QShortcut(QKeySequence(Qt::Key_O), this);
    connect(onionShortcut, &QShortcut::activated, this, [this] {
        if (m_onionButton)
            m_onionButton->toggle(); // emits toggled -> updates canvas + ghost
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

// Reapplies the pristine layout captured at construction (Layers right,
// Scenes + Shot Info tabbed below it). Used by View -> Reset Layout and as
// the fallback when a saved state is rejected.
void StoryboardPage::applyDefaultDockLayout()
{
    if (!m_dockManager || m_defaultDockState.isEmpty())
        return;
    m_dockManager->restoreState(m_defaultDockState, kDockStateVersion);
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
    viewMenu->addAction(m_layersDock->toggleViewAction());
    viewMenu->addAction(m_scenesDock->toggleViewAction());
    viewMenu->addAction(m_shotInfoDock->toggleViewAction());

    // Canvas alignment grid (display-only overlay), default OFF.
    QAction *gridAction = viewMenu->addAction(QStringLiteral("Grid"));
    gridAction->setCheckable(true);
    gridAction->setChecked(false);
    connect(gridAction, &QAction::toggled, this, [this](bool on) {
        if (m_canvas)
            m_canvas->setGridEnabled(on);
    });

    // Escape hatch: wipe the persisted layout and go back to the default.
    viewMenu->addSeparator();
    QAction *resetLayout = viewMenu->addAction(QStringLiteral("Reset Layout"));
    connect(resetLayout, &QAction::triggered, this, [this] {
        QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
        settings.remove(QStringLiteral("storyboard/dockState"));
        applyDefaultDockLayout();
    });

    // LGPL credit for the bundled docking library (license text ships next
    // to the executable).
    QMenu *helpMenu = nullptr;
    const QList<QAction *> topMenus = bar->actions();
    for (QAction *action : topMenus) {
        QString title = action->text();
        title.remove(QLatin1Char('&'));
        if (action->menu() && title == QLatin1String("Help")) {
            helpMenu = action->menu();
            break;
        }
    }
    if (!helpMenu)
        helpMenu = bar->addMenu(QStringLiteral("Help"));
    QAction *adsAbout =
        helpMenu->addAction(QStringLiteral("About Qt Advanced Docking System"));
    connect(adsAbout, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("Qt Advanced Docking System"),
            QStringLiteral(
                "SankoTV uses the Qt Advanced Docking System library\n"
                "(c) Uwe Kindler and contributors, licensed under the\n"
                "GNU LGPL v2.1 and linked as a shared library.\n\n"
                "License text: LICENSE.Qt-Advanced-Docking-System.txt\n"
                "(in the application folder)\n\n"
                "https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System"));
    });
}

bool StoryboardPage::restoreDockState()
{
    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QByteArray state =
        settings.value(QStringLiteral("storyboard/dockState")).toByteArray();
    if (state.isEmpty())
        return false;
    // Versioned restore: layouts written by older builds (pre-ADS QMainWindow
    // blobs, or a different schema version) are rejected here — the caller
    // then re-applies the default layout.
    return m_dockManager && m_dockManager->restoreState(state, kDockStateVersion);
}

void StoryboardPage::saveDockState()
{
    if (!m_dockManager)
        return;
    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    settings.setValue(QStringLiteral("storyboard/dockState"),
                      m_dockManager->saveState(kDockStateVersion));
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
    connect(m_canvas, &DrawingCanvas::contentChanged, this, &StoryboardPage::refreshCurrentThumb);
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
    m_zoomToolbar->setFlipH(m_canvas->viewFlipH());
    connect(m_zoomToolbar, &ZoomToolbar::zoomChanged, m_canvas, &DrawingCanvas::setViewZoom);
    connect(m_zoomToolbar, &ZoomToolbar::rotationChanged, m_canvas, &DrawingCanvas::setViewRotation);
    connect(m_zoomToolbar, &ZoomToolbar::flipToggled, m_canvas, &DrawingCanvas::toggleFlipH);
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
            const QWidget *floats[] = {m_extrasToolbar, m_zoomToolbar, m_brushPanel,
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

    // ---- Vertical extras bar (Shapes / Camera / Onion + brush size) ------
    // Relocated here so the Brush bar matches Figma 33:110 exactly while these
    // controls stay reachable. FloatingToolWindow, same as the Brush bar.
    RoundedBar *extrasBar =
        new RoundedBar(m_canvas, QStringLiteral("storyboard/extrasBarPos"), this);
    m_extrasToolbar = extrasBar;
    m_extrasToolbar->setObjectName(QStringLiteral("extrasToolbar"));

    QVBoxLayout *extras = new QVBoxLayout(m_extrasToolbar);
    extras->setContentsMargins(8, 10, 8, 10);
    extras->setSpacing(10);

    // Grip (horizontal 3x2 dots), top; presses propagate to the bar.
    QLabel *extrasGrip = new QLabel;
    extrasGrip->setPixmap(dragDotsPixmap());
    extrasGrip->setFixedHeight(14);
    extrasGrip->setAlignment(Qt::AlignCenter);
    extrasGrip->setCursor(Qt::OpenHandCursor);
    extrasGrip->setToolTip(QStringLiteral("Drag to move"));
    extras->addWidget(extrasGrip, 0, Qt::AlignHCenter);
    extrasBar->setGripWidget(extrasGrip);

    QPushButton *shapes = toolButton(toolIconPixmap("shapes", kIconColor),
        QStringLiteral("<b>Shapes</b> | Draw rectangles, circles, lines."), true);
    QPushButton *camera = toolButton(toolIconPixmap("camera", kIconColor),
        QStringLiteral("<b>Camera</b> | Frame and safe-area guides."), true);
    tools->addButton(shapes);
    tools->addButton(camera);
    bindTool(shapes, DrawingCanvas::Shapes);
    bindTool(camera, DrawingCanvas::Camera);
    connect(shapes, &QPushButton::toggled, this, [this](bool on) {
        if (m_shapesPanel)
            m_shapesPanel->setVisible(on);
    });
    connect(camera, &QPushButton::toggled, this, [this](bool on) {
        if (m_cameraPanel)
            m_cameraPanel->setVisible(on);
    });
    extras->addWidget(shapes, 0, Qt::AlignHCenter);
    extras->addWidget(camera, 0, Qt::AlignHCenter);

    // Onion skin toggle (independent of the exclusive tool group).
    m_onionButton = toolButton(toolIconPixmap("onion", kIconColor),
        QStringLiteral("<b>Onion Skin</b> | Ghost of the previous panel."), true);
    connect(m_onionButton, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setOnionSkinEnabled(on);
        updateOnionGhost();
    });
    extras->addWidget(m_onionButton, 0, Qt::AlignHCenter);

    // Vertical brush-size slider (range 1-200), fixed run so the bar hugs a
    // stable height.
    m_brushSizeSlider = new SankoSlider;
    m_brushSizeSlider->setToolTip(QStringLiteral("Brush size"));
    m_brushSizeSlider->setOrientation(Qt::Vertical);
    m_brushSizeSlider->setTrackHeight(25); // track WIDTH when vertical
    m_brushSizeSlider->setHandleSize(27);
    m_brushSizeSlider->setRange(1, 200);
    m_brushSizeSlider->setValue(25);
    m_brushSizeSlider->setMinimumHeight(140); // fixed run (set after the setters
    m_brushSizeSlider->setMaximumHeight(140); // above: they reset the constraints)
    connect(m_brushSizeSlider, &SankoSlider::valueChanged, this, [this](int v) {
        m_canvas->setBrushToolSize(v);
        // The Eraser and Line widths follow too (clamped to 1-20 inside).
        m_canvas->setBrushSize(v);
    });
    extras->addWidget(m_brushSizeSlider, 0, Qt::AlignHCenter);

    m_extrasToolbar->adjustSize();
    // Default spot: left edge, vertically centred over the canvas.
    extrasBar->setDefaultOffsetProvider([this, extrasBar] {
        return QPoint(16, qMax(6, (m_canvas->height() - extrasBar->height()) / 2));
    });
    extrasBar->show(); // records intent; effective when the canvas shows

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

    // Camera mirrors the extras-bar Camera button (one source of truth: the
    // extras button owns the tool-group membership and the panel wiring).
    connect(cameraBtn, &QPushButton::toggled, camera, &QPushButton::setChecked);
    connect(camera, &QPushButton::toggled, cameraBtn, &QPushButton::setChecked);

    // Dock toggles wire up once the ADS docks exist (they are created after
    // the canvas column that hosts this bar).
    connect(layersBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_layersDock && m_layersDock->isClosed() == on)
            m_layersDock->toggleView(on);
    });
    connect(scenesBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_scenesDock && m_scenesDock->isClosed() == on)
            m_scenesDock->toggleView(on);
    });
    connect(shotInfoBtn, &QPushButton::toggled, this, [this](bool on) {
        if (m_shotInfoDock && m_shotInfoDock->isClosed() == on)
            m_shotInfoDock->toggleView(on);
    });
    QTimer::singleShot(0, this, [this, layersBtn, scenesBtn, shotInfoBtn] {
        struct Pair { ads::CDockWidget *dock; ToolButton *btn; };
        const Pair pairs[] = {{m_layersDock, layersBtn},
                              {m_scenesDock, scenesBtn},
                              {m_shotInfoDock, shotInfoBtn}};
        for (const Pair &pr : pairs) {
            if (!pr.dock)
                continue;
            pr.btn->setChecked(!pr.dock->isClosed());
            connect(pr.dock, &ads::CDockWidget::viewToggled,
                    pr.btn, &QPushButton::setChecked);
        }
    });


    m_layersToolbar->adjustSize();
    // Default spot: top edge, horizontally centred over the canvas.
    layersBar->setDefaultOffsetProvider([this, layersBar] {
        return QPoint(qMax(6, (m_canvas->width() - layersBar->width()) / 2), 12);
    });
    layersBar->show(); // records intent; effective when the canvas shows
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
            [this](int v) { m_canvas->setBrushOpacity(v); });
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
        persp->setSelectedOpacity(v / 100.0); // per-VP
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
    if (m_brushSizeSlider)
        m_brushSizeSlider->setValue(size);
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
    const bool show = m_onionButton && m_onionButton->isChecked() && scene
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

void StoryboardPage::refreshCurrentThumb()
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
    // Layer rows: click = make that layer active, double-click = rename.
    const QVariant layerIdx = object->property("layerIndex");
    if (layerIdx.isValid()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                setActiveLayer(layerIdx.toInt());
            return false;
        }
        if (event->type() == QEvent::MouseButtonDblClick) {
            renameLayer(layerIdx.toInt());
            return true;
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

// 40x22 layer thumbnail over a dark checker-free backdrop (transparent areas
// read as the panel background, not white).
QPixmap layerThumb(const Layer &layer)
{
    QImage out(40, 22, QImage::Format_ARGB32_Premultiplied);
    out.fill(QColor(0x1b, 0x1b, 0x1b));
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!layer.image.isNull())
        painter.drawImage(QRect(0, 0, 40, 22), layer.image);
    painter.end();
    return QPixmap::fromImage(out);
}

} // namespace

QWidget *StoryboardPage::createLayerPanel()
{
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setMinimumWidth(170); // dock-resizable (was fixed 200)
    column->setStyleSheet(QStringLiteral(
        "background-color: #111111; border-left: 1px solid #1f1f1f;"));

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(10, 12, 10, 12);
    layout->setSpacing(8);

    // (No inner heading: the ADS dock tab already names the panel.)

    // Opacity of the ACTIVE layer.
    QHBoxLayout *opacityRow = new QHBoxLayout;
    opacityRow->setSpacing(6);
    QLabel *opacityLabel = new QLabel(QStringLiteral("Opacity"));
    opacityLabel->setStyleSheet(QStringLiteral("color: #999999; font-size: 10px; border: none;"));
    opacityRow->addWidget(opacityLabel);
    opacityRow->addStretch(1);
    layout->addLayout(opacityRow);

    // Custom glowing slider, opacity preset (track 14 / handle 16, 0-100).
    // Drop-in for the previous QSlider: same range/value/valueChanged
    // surface, so the opacity wiring is unchanged. The live "NN%" label is
    // painted inside the widget, right of the track.
    m_layerOpacity = new SankoSlider;
    m_layerOpacity->setTrackHeight(10);
    m_layerOpacity->setHandleSize(13);
    m_layerOpacity->setRange(0, 100);
    m_layerOpacity->setValueSuffix(QStringLiteral("%"));
    m_layerOpacity->setValue(100);
    connect(m_layerOpacity, &SankoSlider::valueChanged, this, [this](int v) {
        if (m_updatingLayerUi)
            return;
        Panel *panel = currentPanel();
        Layer *layer = panel ? panel->activeLayer() : nullptr;
        if (!layer)
            return;
        layer->opacity = v / 100.0;
        refreshLayerCanvas(); // live: canvas composite + panel thumbnail
    });
    layout->addWidget(m_layerOpacity);

    // Layer rows (top = frontmost), scrollable.
    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    QWidget *listHost = new QWidget;
    listHost->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    m_layerListLayout = new QVBoxLayout(listHost);
    m_layerListLayout->setContentsMargins(0, 0, 0, 0);
    m_layerListLayout->setSpacing(4);
    m_layerListLayout->addStretch(1);
    scroll->setWidget(listHost);
    layout->addWidget(scroll, 1);

    // Actions.
    QHBoxLayout *row1 = new QHBoxLayout;
    row1->setSpacing(6);
    QPushButton *add = layerActionButton(QStringLiteral("+ Layer"),
                                         QStringLiteral("Add a blank layer above the active one"));
    QPushButton *addImage = layerActionButton(QStringLiteral("+ Image"),
                                              QStringLiteral("Import an image as a new layer"));
    connect(add, &QPushButton::clicked, this, [this] { layerAdd(); });
    connect(addImage, &QPushButton::clicked, this, [this] { layerAddImage(); });
    row1->addWidget(add);
    row1->addWidget(addImage);
    layout->addLayout(row1);

    QHBoxLayout *row2 = new QHBoxLayout;
    row2->setSpacing(6);
    m_layerMergeButton = layerActionButton(QStringLiteral("Merge Down"),
                                           QStringLiteral("Flatten the active layer into the one below"));
    m_layerDeleteButton = layerActionButton(QStringLiteral("Delete"),
                                            QStringLiteral("Remove the active layer"));
    connect(m_layerMergeButton, &QPushButton::clicked, this, [this] { layerMergeDown(); });
    connect(m_layerDeleteButton, &QPushButton::clicked, this, [this] { layerDelete(); });
    row2->addWidget(m_layerMergeButton);
    row2->addWidget(m_layerDeleteButton);
    layout->addLayout(row2);

    QHBoxLayout *row3 = new QHBoxLayout;
    row3->setSpacing(6);
    QPushButton *up = layerActionButton(QStringLiteral("▲"),
                                        QStringLiteral("Move the active layer up (toward the front)"));
    QPushButton *down = layerActionButton(QStringLiteral("▼"),
                                          QStringLiteral("Move the active layer down (toward the back)"));
    connect(up, &QPushButton::clicked, this, [this] { layerMove(+1); });   // front = higher index
    connect(down, &QPushButton::clicked, this, [this] { layerMove(-1); });
    row3->addWidget(up);
    row3->addWidget(down);
    layout->addLayout(row3);

    return column;
}

void StoryboardPage::rebuildLayerPanel()
{
    if (!m_layerListLayout)
        return;

    m_layerRows.clear();
    while (QLayoutItem *item = m_layerListLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    Panel *panel = currentPanel();
    const bool hasPanel = (panel != nullptr) && !panel->layers.isEmpty();

    // Sync the opacity slider to the active layer (guarded — no feedback loop).
    m_updatingLayerUi = true;
    const Layer *active = hasPanel ? panel->activeLayer() : nullptr;
    const int pct = active ? qRound(qBound(0.0, active->opacity, 1.0) * 100.0) : 100;
    if (m_layerOpacity) {
        m_layerOpacity->setEnabled(active != nullptr);
        m_layerOpacity->setValue(pct); // slider paints its own "NN%" label
    }
    m_updatingLayerUi = false;

    if (m_layerDeleteButton)
        m_layerDeleteButton->setEnabled(hasPanel && panel->layers.size() > 1);
    if (m_layerMergeButton)
        m_layerMergeButton->setEnabled(hasPanel && panel->activeLayerIndex > 0);

    if (!hasPanel) {
        m_layerListLayout->addStretch(1);
        return;
    }

    // Top row = frontmost layer = highest vector index.
    for (int i = panel->layers.size() - 1; i >= 0; --i) {
        const Layer &layer = panel->layers.at(i);
        const bool isActive = (i == panel->activeLayerIndex);

        QFrame *row = new QFrame;
        row->setObjectName(QStringLiteral("layerRow"));
        row->setProperty("layerIndex", i);
        row->setFixedHeight(32);
        row->setCursor(Qt::PointingHandCursor);
        row->installEventFilter(this); // click = activate, double-click = rename
        row->setStyleSheet(
            isActive
                ? QStringLiteral("QFrame#layerRow { background-color: #1b1b1b;"
                                 " border: 1px solid #2a2a2a; border-left: 3px solid #f5a623;"
                                 " border-radius: 4px; }")
                : QStringLiteral("QFrame#layerRow { background-color: #161616;"
                                 " border: 1px solid #232323; border-radius: 4px; }"));

        QHBoxLayout *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 2, 4, 2);
        rowLayout->setSpacing(5);

        // Visibility (eye) toggle.
        QPushButton *eye = new QPushButton(layer.visible ? QStringLiteral("\U0001F441")
                                                         : QStringLiteral("–"));
        eye->setToolTip(QStringLiteral("Show / hide layer"));
        eye->setCursor(Qt::PointingHandCursor);
        eye->setFixedSize(20, 20);
        eye->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; color: %1; font-size: 11px; }")
            .arg(layer.visible ? QStringLiteral("#cccccc") : QStringLiteral("#555555")));
        connect(eye, &QPushButton::clicked, this, [this, i] {
            Panel *p = currentPanel();
            if (!p || i < 0 || i >= p->layers.size())
                return;
            p->layers[i].visible = !p->layers[i].visible;
            refreshLayerCanvas();
            rebuildLayerPanel();
        });
        rowLayout->addWidget(eye);

        // Layer thumbnail.
        QLabel *thumb = new QLabel;
        thumb->setFixedSize(40, 22);
        thumb->setStyleSheet(QStringLiteral("border: 1px solid #2a2a2a; border-radius: 2px;"));
        thumb->setPixmap(layerThumb(layer));
        rowLayout->addWidget(thumb);

        // Name (double-click the row to rename).
        QLabel *name = new QLabel(layer.name);
        name->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 11px; border: none; background: transparent;")
            .arg(layer.visible ? QStringLiteral("#cccccc") : QStringLiteral("#666666")));
        rowLayout->addWidget(name, 1);

        // Lock toggle (padlock).
        QPushButton *lock = new QPushButton(layer.locked ? QStringLiteral("\U0001F512")
                                                         : QStringLiteral("\U0001F513"));
        lock->setToolTip(QStringLiteral("Lock / unlock layer"));
        lock->setCursor(Qt::PointingHandCursor);
        lock->setFixedSize(20, 20);
        lock->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; color: %1; font-size: 10px; }")
            .arg(layer.locked ? QStringLiteral("#f5a623") : QStringLiteral("#555555")));
        connect(lock, &QPushButton::clicked, this, [this, i] {
            Panel *p = currentPanel();
            if (!p || i < 0 || i >= p->layers.size())
                return;
            p->layers[i].locked = !p->layers[i].locked;
            rebuildLayerPanel();
        });
        rowLayout->addWidget(lock);

        m_layerListLayout->addWidget(row);
        m_layerRows.append(row);
    }
    m_layerListLayout->addStretch(1);
}

void StoryboardPage::setActiveLayer(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    panel->activeLayerIndex = index;
    rebuildLayerPanel(); // amber highlight + slider follow the new active layer
}

void StoryboardPage::renameLayer(int index)
{
    Panel *panel = currentPanel();
    if (!panel || index < 0 || index >= panel->layers.size())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename Layer"), QStringLiteral("Layer name:"),
        QLineEdit::Normal, panel->layers.at(index).name, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    panel->layers[index].name = name.trimmed();
    rebuildLayerPanel();
}

void StoryboardPage::layerAdd()
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    Layer layer = makeRasterLayer(QStringLiteral("Layer %1").arg(panel->layers.size() + 1));
    const int insertAt = qBound(0, panel->activeLayerIndex + 1, panel->layers.size());
    panel->layers.insert(insertAt, layer);
    panel->activeLayerIndex = insertAt;
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerAddImage()
{
    if (!currentPanel() || !m_canvas)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Image Layer"), QString(),
        QStringLiteral("Images (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty())
        return;
    m_canvas->importImage(path); // inserts the layer + emits layersChanged -> rebuild
}

void StoryboardPage::layerDelete()
{
    Panel *panel = currentPanel();
    if (!panel || panel->layers.size() <= 1)
        return; // the last remaining layer can never be deleted
    if (panel->layers.at(panel->activeLayerIndex).type == QLatin1String("background"))
        return; // the Background layer is permanent
    panel->layers.removeAt(panel->activeLayerIndex);
    panel->activeLayerIndex = qBound(0, panel->activeLayerIndex, panel->layers.size() - 1);
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerMergeDown()
{
    Panel *panel = currentPanel();
    if (!panel || panel->activeLayerIndex <= 0
        || panel->activeLayerIndex >= panel->layers.size())
        return;

    const int idx = panel->activeLayerIndex;
    if (panel->layers.at(idx - 1).type == QLatin1String("background"))
        return; // don't bake art into the Background (keeps it a clean paper layer)
    Layer &below = panel->layers[idx - 1];
    const Layer &top = panel->layers.at(idx);

    QPainter painter(&below.image); // bake the active layer (with its opacity) into the one below
    painter.setOpacity(qBound(0.0, top.opacity, 1.0));
    painter.drawImage(0, 0, top.image);
    painter.end();

    panel->layers.removeAt(idx);
    panel->activeLayerIndex = idx - 1;
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::layerMove(int delta)
{
    Panel *panel = currentPanel();
    if (!panel)
        return;
    const int from = panel->activeLayerIndex;
    const int to = from + delta;
    if (from < 0 || from >= panel->layers.size() || to < 0 || to >= panel->layers.size())
        return;
    panel->layers.swapItemsAt(from, to);
    panel->activeLayerIndex = to;
    refreshLayerCanvas();
    rebuildLayerPanel();
}

void StoryboardPage::refreshLayerCanvas()
{
    if (m_canvas)
        m_canvas->update();
    refreshCurrentThumb();
}
