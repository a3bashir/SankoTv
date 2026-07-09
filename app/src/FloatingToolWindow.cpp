#include "FloatingToolWindow.h"

#include <QEvent>
#include <QMouseEvent>
#include <QSet>
#include <QSettings>
#include <QVector>


// ONE shared event filter for every FloatingToolWindow (present and future):
// watches each registered window's anchor and the anchor's top-level window,
// repositioning on Move/Resize and re-deriving effective visibility on
// Show/Hide/WindowStateChange (hide on minimize, restore on show, mirror the
// anchor when the user navigates away from the page hosting it).
class FloatingToolWindowManager : public QObject
{
public:
    static FloatingToolWindowManager *instance()
    {
        static FloatingToolWindowManager manager;
        return &manager;
    }

    void registerWindow(FloatingToolWindow *w)
    {
        m_windows.append(w);
        watch(w->m_anchor);
        watchTopLevelOf(w->m_anchor); // may still be a detached page: harmless,
                                      // re-run on the anchor's next Show
    }

    void unregisterWindow(FloatingToolWindow *w) { m_windows.removeAll(w); }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            for (FloatingToolWindow *w : std::as_const(m_windows))
                if (isHostOf(w, object))
                    w->reposition();
            break;
        case QEvent::Show:
            // The anchor may only now sit inside its final top-level window
            // (it is constructed before the page joins the main window).
            for (FloatingToolWindow *w : std::as_const(m_windows))
                if (w->m_anchor == object)
                    watchTopLevelOf(w->m_anchor);
            Q_FALLTHROUGH();
        case QEvent::Hide:
        case QEvent::WindowStateChange:
            for (FloatingToolWindow *w : std::as_const(m_windows))
                if (isHostOf(w, object))
                    w->applyEffectiveVisibility();
            break;
        default:
            break;
        }
        return false; // observe only, never consume
    }

private:
    void watch(QObject *object)
    {
        if (!object || m_watched.contains(object))
            return;
        m_watched.insert(object);
        object->installEventFilter(this);
    }

    void watchTopLevelOf(QWidget *anchor)
    {
        if (QWidget *win = anchor ? anchor->window() : nullptr)
            if (win != anchor)
                watch(win);
    }

    static bool isHostOf(const FloatingToolWindow *w, const QObject *object)
    {
        return w->m_anchor
            && (object == w->m_anchor || object == w->m_anchor->window());
    }

    QVector<FloatingToolWindow *> m_windows;
    QSet<QObject *> m_watched; // never removed: hosts live for the app's life
};

FloatingToolWindow::FloatingToolWindow(QWidget *anchor, const QString &settingsKey,
                                       QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
    , m_anchor(anchor)
    , m_settingsKey(settingsKey)
{
    setAttribute(Qt::WA_TranslucentBackground); // smooth antialiased corners
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus); // never steals canvas focus

    if (!m_settingsKey.isEmpty()) {
        const QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
        if (settings.contains(m_settingsKey)) {
            m_offset = settings.value(m_settingsKey).toPoint();
            m_userPlaced = true;
        }
    }
    FloatingToolWindowManager::instance()->registerWindow(this);
}

FloatingToolWindow::~FloatingToolWindow()
{
    FloatingToolWindowManager::instance()->unregisterWindow(this);
}

void FloatingToolWindow::setVisible(bool visible)
{
    m_wantVisible = visible;
    applyEffectiveVisibility();
}

void FloatingToolWindow::applyEffectiveVisibility()
{
    QWidget *win = m_anchor ? m_anchor->window() : nullptr;
    const bool hostAllows = m_anchor && m_anchor->isVisible() && win && win->isVisible()
        && !(win->windowState() & Qt::WindowMinimized);
    const bool effective = m_wantVisible && hostAllows;
    if (effective && !isVisible()) {
        reposition();
        QWidget::setVisible(true); // bypass our intent-recording override
    } else if (!effective && isVisible()) {
        QWidget::setVisible(false);
    }
}

void FloatingToolWindow::reposition()
{
    if (!m_anchor)
        return;
    if (!m_userPlaced)
        m_offset = defaultOffset(); // defaults track the anchor's size
    move(clampedPos(anchorOrigin() + m_offset));
}

QPoint FloatingToolWindow::anchorOrigin() const
{
    return m_anchor ? m_anchor->mapToGlobal(QPoint(0, 0)) : QPoint();
}

QPoint FloatingToolWindow::clampedPos(const QPoint &pos) const
{
    if (!m_anchor)
        return pos;
    // POSITION-only: x/y are bounded to the anchor's on-screen rect; width and
    // height are never touched — at an edge the window stops at full size.
    const QPoint origin = anchorOrigin();
    const int maxX = origin.x() + qMax(0, m_anchor->width() - width());
    const int maxY = origin.y() + qMax(0, m_anchor->height() - height());
    return QPoint(qBound(origin.x(), pos.x(), maxX), qBound(origin.y(), pos.y(), maxY));
}

QRect FloatingToolWindow::gripRect() const
{
    // A grip child widget ignores mouse events, so presses on it propagate to
    // this window (translated); its geometry is the default drag region.
    return m_gripWidget ? m_gripWidget->geometry() : QRect();
}

QPoint FloatingToolWindow::defaultOffset() const
{
    return m_defaultOffset ? m_defaultOffset() : QPoint();
}

bool FloatingToolWindow::handleGripPress(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton
        || !gripRect().contains(event->position().toPoint()))
        return false;
    m_dragging = true;
    m_dragStartGlobal = event->globalPosition().toPoint();
    m_dragStartPos = pos();
    (m_gripWidget ? m_gripWidget.data() : this)->setCursor(Qt::ClosedHandCursor);
    return true;
}

bool FloatingToolWindow::handleGripMove(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton))
        return false;
    move(clampedPos(m_dragStartPos
                    + (event->globalPosition().toPoint() - m_dragStartGlobal)));
    return true;
}

bool FloatingToolWindow::handleGripRelease(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (!m_dragging)
        return false;
    m_dragging = false;
    (m_gripWidget ? m_gripWidget.data() : this)
        ->setCursor(m_gripWidget ? Qt::OpenHandCursor : Qt::ArrowCursor);
    m_offset = pos() - anchorOrigin();
    m_userPlaced = true;
    persistOffset();
    return true;
}

void FloatingToolWindow::persistOffset()
{
    if (!m_settingsKey.isEmpty())
        QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"))
            .setValue(m_settingsKey, m_offset);
}

void FloatingToolWindow::mousePressEvent(QMouseEvent *event)
{
    if (!handleGripPress(event))
        QWidget::mousePressEvent(event);
}

void FloatingToolWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!handleGripMove(event))
        QWidget::mouseMoveEvent(event);
}

void FloatingToolWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!handleGripRelease(event))
        QWidget::mouseReleaseEvent(event);
}
