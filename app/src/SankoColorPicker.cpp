#include "SankoColorPicker.h"

#include "ColorPanel.h"
#include "SankoTheme.h"

#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
std::function<void(SankoColorPicker *)> g_openHookForTest;

// The pick's catcher: one pixel kept under the pointer while a pick waits, so a click made over
// another program's window lands here and not on that program (the picker's mouse grab holds
// SankoTV's own windows) - as the Windows picker does.
class PickCatcher : public QWidget
{
public:
    std::function<void(const QPoint &)> onClick;

    explicit PickCatcher(QWidget *picker)
        : QWidget(picker, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                              | Qt::WindowDoesNotAcceptFocus)
    {
        setObjectName(QStringLiteral("colorPickCatcher"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(1, 1);
        setCursor(Qt::CrossCursor);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClick)
            onClick(event->globalPosition().toPoint());
    }
};

void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}
} // namespace

QColor SankoColorPicker::getColor(const QColor &initial, QWidget *parent, const QString &title)
{
    SankoColorPicker picker(initial, title, parent);
    picker.placeBeside(QRect(QCursor::pos(), QSize(1, 1)));
    if (g_openHookForTest) {
        picker.show();
        g_openHookForTest(&picker);
        return picker.result() == QDialog::Accepted ? picker.color() : QColor();
    }
    return picker.exec() == QDialog::Accepted ? picker.color() : QColor();
}

void SankoColorPicker::setOpenHookForTest(std::function<void(SankoColorPicker *)> hook)
{
    g_openHookForTest = std::move(hook);
}

SankoColorPicker::SankoColorPicker(const QColor &initial, const QString &title, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    , m_title(title)
    , m_previous(initial.isValid() ? initial : QColor(Qt::black))
{
    setObjectName(QStringLiteral("sankoColorPicker"));
    setWindowTitle(title);
    setModal(true);
    setAttribute(Qt::WA_TranslucentBackground); // the container's rounded corners, painted

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1); // inside the 1 px painted border, as the Colors panel
    root->setSpacing(0);

    // The Brush Library's header, as the Colors panel's: 69 px, padding 24 / 20 / 24 / 16, the
    // title alone in Inter Extra Bold 22, white, a 1 px #2D2D35 divider under it. The drag grip.
    auto *header = new QWidget;
    header->setObjectName(QStringLiteral("colorPickerHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    header->setStyleSheet(QStringLiteral(
        "QWidget#colorPickerHeader { background: transparent; border-bottom: 1px solid #2d2d35; }"));
    header->setFixedHeight(69);
    header->setCursor(Qt::OpenHandCursor);
    auto *headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(24, 20, 24, 16);
    headerRow->setSpacing(8);
    auto *titleLabel = new QLabel(title, header);
    titleLabel->setObjectName(QStringLiteral("colorPickerTitle"));
    QFont titleFont(QStringLiteral("Inter"));
    titleFont.setWeight(QFont::ExtraBold);
    // THE PICKER STAYS 254 WIDE FOR EVERY TITLE (the user's word, 2026-11-19: "Background
    // Colour" had made it 263, "Panel Strip Background" 310): 22 px when the title fits the
    // header's room - 252 inside the border less the 24 px padding each side - and a pixel
    // smaller at a time until it does (12 at least). The label never widens the picker.
    constexpr int kTitleRoom = 252 - 24 - 24;
    int titlePx = 22;
    for (; titlePx > 12; --titlePx) {
        titleFont.setPixelSize(titlePx);
        if (QFontMetrics(titleFont).horizontalAdvance(title) <= kTitleRoom)
            break;
    }
    titleFont.setPixelSize(titlePx);
    titleLabel->setFont(titleFont);
    titleLabel->setMaximumWidth(kTitleRoom); // even a title too long at 12 px leaves the width alone
    titleLabel->setStyleSheet(QStringLiteral("color: white; background: transparent;"));
    headerRow->addWidget(titleLabel);
    headerRow->addStretch(1);
    root->addWidget(header);

    // The Colors panel's body, without its History and Palette rows; "previous" held on the
    // colour it opened with. The container's ground shows through it, as in the Colors panel.
    m_body = new ColorPanel;
    m_body->setObjectName(QStringLiteral("colorPickerBody"));
    m_body->setStyleSheet(QStringLiteral("background-color: transparent;"));
    m_body->setHistoryAndPaletteShown(false);
    m_body->setColor(m_previous);
    m_body->holdPrevious(m_previous);
    root->addWidget(m_body);
    connect(m_body, &ColorPanel::colorChanged, this, [this] { m_changed = true; });

    // The Eyedropper, where the Colors panel has it: beside the wheel, a child of the wheel (so it
    // shows with the Wheel), the user's dropper, SankoTV's three button states and the active
    // purple while a pick waits.
    if (QWidget *wheel = m_body->findChild<QWidget *>(QStringLiteral("colorWheel"))) {
        m_pickButton = new QToolButton(wheel);
        m_pickButton->setObjectName(QStringLiteral("colorPickerEyedropper"));
        m_pickButton->setIcon(QIcon(QStringLiteral(":/icons/eyedropper.svg")));
        m_pickButton->setIconSize(QSize(21, 21));
        m_pickButton->setFixedSize(32, 32);
        m_pickButton->move(194, 196);
        m_pickButton->setCursor(Qt::PointingHandCursor);
        m_pickButton->setToolTip(QStringLiteral(
            "<b>Eyedropper</b> | Take one colour from anywhere on screen; Esc keeps the one you had."));
        m_pickButton->setProperty("pickArmed", false);
        m_pickButton->setStyleSheet(QStringLiteral(
            "QToolButton#colorPickerEyedropper { background: transparent; border: none; border-radius: 6px; padding: 0px; }"
            "QToolButton#colorPickerEyedropper:hover { background: #2a2a33; }"
            "QToolButton#colorPickerEyedropper:pressed { background: #3a3a45; }"
            "QToolButton#colorPickerEyedropper[pickArmed=\"true\"] { background: #7c6ef6; }"
            "QToolButton#colorPickerEyedropper[pickArmed=\"true\"]:pressed { background: #3a3a45; }"));
        m_pickButton->raise();
        connect(m_pickButton, &QToolButton::clicked, this, [this] { startScreenPick(); });
    }

    // The divider and the two buttons: 4 px under the body, then Cancel and OK on the right, 8 px
    // apart, 12 px under the divider and above the bottom, OK's edge 12 px in from the picker's
    // (11 inside its 1 px border) - the frame's places.
    root->addSpacing(4);
    auto *divider = new QWidget;
    divider->setObjectName(QStringLiteral("colorPickerDivider"));
    divider->setAttribute(Qt::WA_StyledBackground, true);
    divider->setStyleSheet(QStringLiteral("QWidget#colorPickerDivider { background: #2d2d35; }"));
    divider->setFixedHeight(1);
    root->addWidget(divider);
    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(12, 12, 11, 12);
    footer->setSpacing(8);
    footer->addStretch(1);
    m_cancel = new QPushButton(QStringLiteral("Cancel"));
    m_cancel->setObjectName(QStringLiteral("colorPickerCancel"));
    m_cancel->setFixedSize(76, 28);
    m_cancel->setCursor(Qt::PointingHandCursor);
    m_cancel->setAutoDefault(false);
    m_cancel->setStyleSheet(QStringLiteral(
        "QPushButton#colorPickerCancel { background: #2a2a2e; color: #cccccc; border: none;"
        " border-radius: 6px; font-size: 12px; }"
        "QPushButton#colorPickerCancel:hover { background: #34343a; color: #ffffff; }"
        "QPushButton#colorPickerCancel:pressed { background: #3a3a45; }"));
    m_ok = new QPushButton(QStringLiteral("OK"));
    m_ok->setObjectName(QStringLiteral("colorPickerOk"));
    m_ok->setFixedSize(76, 28);
    m_ok->setCursor(Qt::PointingHandCursor);
    m_ok->setDefault(true); // Enter
    m_ok->setStyleSheet(SankoTheme::themed(
        "QPushButton#colorPickerOk { background: %ACCENT%; color: #ffffff; border: none;"
        " border-radius: 6px; font-size: 12px; font-weight: 600; }"
        "QPushButton#colorPickerOk:hover { background: #8f82f8; }"
        "QPushButton#colorPickerOk:pressed { background: #6a5de0; }"));
    footer->addWidget(m_cancel);
    footer->addWidget(m_ok);
    root->addLayout(footer);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_ok, &QPushButton::clicked, this, &QDialog::accept);

    m_pickTimer = new QTimer(this);
    m_pickTimer->setInterval(30);
    // Over another program's window the pointer's moves reach no SankoTV window: it is
    // followed by its position, as the Windows picker follows it.
    connect(m_pickTimer, &QTimer::timeout, this, [this] { followPointer(QCursor::pos()); });

    // The body's 252 and the 1 px border each side, whatever the title; its height from the layout.
    setFixedSize(m_body->minimumWidth() + 2, sizeHint().height());
}

SankoColorPicker::~SankoColorPicker()
{
    endScreenPick(false);
}

QColor SankoColorPicker::color() const
{
    // Untouched, it gives back exactly the colour it opened on (the panel's HSV round trip
    // could move a channel by one); chosen, an RGB colour like every other in the app.
    return m_changed ? m_body->color().toRgb() : m_previous;
}

void SankoColorPicker::setColor(const QColor &color)
{
    if (!color.isValid())
        return;
    m_body->setColor(color);
    m_changed = true;
}

void SankoColorPicker::placeBeside(const QRect &anchor)
{
    const QSize s = size();
    QScreen *screen = QGuiApplication::screenAt(anchor.center());
    if (!screen)
        screen = this->screen() ? this->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect free = screen->availableGeometry();
    constexpr int kGap = 8;
    int x = anchor.right() + 1 + kGap;
    if (x + s.width() - 1 > free.right())
        x = anchor.left() - kGap - s.width();
    x = qBound(free.left(), x, qMax(free.left(), free.right() - s.width() + 1));
    // Level with it: its header's middle at the anchor's.
    int y = anchor.center().y() - 35;
    y = qBound(free.top(), y, qMax(free.top(), free.bottom() - s.height() + 1));
    move(x, y);
}

void SankoColorPicker::paintEvent(QPaintEvent *)
{
    // The Brush Library's container, as the Colors panel paints it.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setPen(QPen(QColor(0x2d, 0x2d, 0x35), 1));
    p.setBrush(QColor(0x1e, 0x1e, 0x20));
    p.drawRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1), 10, 10);
}

// --- The Eyedropper -----------------------------------------------------------------------

QColor SankoColorPicker::screenColorAt(const QPoint &global)
{
    QScreen *screen = QGuiApplication::screenAt(global);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QColor();
    const QRect g = screen->geometry();
    const QImage px = screen->grabWindow(0, global.x() - g.x(), global.y() - g.y(), 1, 1).toImage();
    return px.isNull() ? QColor() : QColor(px.pixel(0, 0));
}

void SankoColorPicker::startScreenPick()
{
    if (m_picking)
        return;
    m_picking = true;
    m_beforePick = color();
    m_changedBeforePick = m_changed;
    m_lastPointer = QPoint(-100000, -100000);
    if (!m_pickCatcher) {
        auto *catcher = new PickCatcher(this);
        catcher->onClick = [this](const QPoint &global) {
            if (!m_picking)
                return;
            if (global != m_lastPointer)
                setColor(screenColorAt(global));
            endScreenPick(true);
        };
        m_pickCatcher = catcher;
    }
    if (m_pickButton) {
        m_pickButton->setProperty("pickArmed", true);
        repolish(m_pickButton);
    }
    grabMouse(Qt::CrossCursor);
    grabKeyboard();
    m_pickTimer->start();
}

void SankoColorPicker::followPointer(const QPoint &global)
{
    if (!m_picking || global == m_lastPointer)
        return;
    m_lastPointer = global;
    // Sampled before the catcher moves there: what is read is what the screen shows.
    const QColor seen = screenColorAt(global);
    if (seen.isValid())
        setColor(seen);
    if (m_pickCatcher) {
        m_pickCatcher->move(global);
        if (!m_pickCatcher->isVisible())
            m_pickCatcher->show();
    }
}

void SankoColorPicker::endScreenPick(bool keep)
{
    if (!m_picking)
        return;
    m_picking = false;
    m_pickTimer->stop();
    if (m_pickCatcher)
        m_pickCatcher->hide();
    releaseKeyboard();
    releaseMouse();
    if (!keep) { // exactly as it was: untouched before the pick, untouched again
        m_body->setColor(m_beforePick);
        m_changed = m_changedBeforePick;
    }
    if (m_pickButton) {
        m_pickButton->setProperty("pickArmed", false);
        repolish(m_pickButton);
    }
}

void SankoColorPicker::done(int result)
{
    endScreenPick(false);
    QDialog::done(result);
}

void SankoColorPicker::keyPressEvent(QKeyEvent *event)
{
    if (m_picking) {
        if (event->key() == Qt::Key_Escape)
            endScreenPick(false); // the pick only: the picker stays open
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event); // Esc: Cancel; Enter: OK
}

void SankoColorPicker::mousePressEvent(QMouseEvent *event)
{
    if (m_picking) {
        // The grab brings every click here, wherever it is on SankoTV's windows.
        if (event->button() == Qt::LeftButton) {
            const QPoint global = event->globalPosition().toPoint();
            if (global != m_lastPointer) // there, the colour is already the one under it
                setColor(screenColorAt(global));
            endScreenPick(true);
        }
        event->accept();
        return;
    }
    if (m_drag.press(this, event, QRect(0, 0, width(), 70))) {
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void SankoColorPicker::mouseMoveEvent(QMouseEvent *event)
{
    if (m_picking) {
        followPointer(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (m_drag.move(this, event)) {
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void SankoColorPicker::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_picking) {
        event->accept();
        return;
    }
    if (m_drag.release()) {
        event->accept();
        return;
    }
    QDialog::mouseReleaseEvent(event);
}
