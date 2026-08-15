#include "FloatingToolWindow.h"

#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QVector>

#include <algorithm>

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

    // Registered windows, in registration order (managed-placement priority).
    const QVector<FloatingToolWindow *> &windows() const { return m_windows; }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize: {
            // Unmanaged windows reposition independently; managed ones go
            // through the group pass (fixed priority order, single pass) so
            // collision avoidance stays deterministic.
            QWidget *groupAnchor = nullptr;
            for (FloatingToolWindow *w : std::as_const(m_windows))
                if (isHostOf(w, object)) {
                    if (w->m_managed)
                        groupAnchor = w->m_anchor;
                    else
                        w->reposition();
                }
            if (groupAnchor)
                FloatingToolWindow::repositionManagedGroup(groupAnchor);
            break;
        }
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

namespace {
// Per-anchor suppression capture: (window, its intent at suppress time).
// In MEMORY only — the whole point is that a modal-surface hide is never
// persisted anywhere a user choice is (see the header).
struct SuppressedBar
{
    QPointer<FloatingToolWindow> window;
    bool intent = false;
};
QHash<QWidget *, QVector<SuppressedBar>> &suppressedBars()
{
    static QHash<QWidget *, QVector<SuppressedBar>> store;
    return store;
}
} // namespace

void FloatingToolWindow::suppressFloatingBars(
    QWidget *anchor, const QVector<FloatingToolWindow *> &except)
{
    if (!anchor || suppressedBars().contains(anchor))
        return; // nested suppress keeps the FIRST capture
    QVector<SuppressedBar> captured;
    // The manager's registry, not a widget-tree walk: it covers every
    // floating window over this anchor by construction, including any
    // future bar that inherits this class.
    const QVector<FloatingToolWindow *> all =
        FloatingToolWindowManager::instance()->windows();
    for (FloatingToolWindow *w : all) {
        if (w->m_anchor != anchor || except.contains(w))
            continue;
        captured.append({w, w->m_wantVisible});
        // Base-class hide: intent bookkeeping happens HERE, in memory.
        // Subclass setVisible overrides that persist intent (the library)
        // are excluded by the caller, and none of the bars persist theirs
        // outside a drag release, so nothing is written anywhere.
        w->FloatingToolWindow::setVisible(false);
    }
    suppressedBars().insert(anchor, captured);
}

void FloatingToolWindow::restoreFloatingBars(QWidget *anchor)
{
    if (!anchor)
        return;
    const QVector<SuppressedBar> captured = suppressedBars().take(anchor);
    for (const SuppressedBar &bar : captured)
        if (bar.window)
            bar.window->FloatingToolWindow::setVisible(bar.intent);
}

void FloatingToolWindow::applyEffectiveVisibility()
{
    QWidget *win = m_anchor ? m_anchor->window() : nullptr;
    const bool hostAllows = m_anchor && m_anchor->isVisible() && win && win->isVisible()
        && !(win->windowState() & Qt::WindowMinimized);
    const bool effective = m_wantVisible && hostAllows;
    bool changed = false;
    if (effective && !isVisible()) {
        reposition();
        QWidget::setVisible(true); // bypass our intent-recording override
        changed = true;
    } else if (!effective && isVisible()) {
        QWidget::setVisible(false);
        changed = true;
    }
    // Occupancy changed (e.g. the Selection/Move Modifier bar appeared with
    // a tool, or was dismissed): re-run managed placement so a bar it now
    // overlaps moves aside — and a displaced bar RETURNS to its stored
    // position once the obstacle is gone, because displacement never
    // overwrites the user's saved offset (only a drag release does).
    if (changed && m_anchor)
        repositionManagedGroup(m_anchor);
}

void FloatingToolWindow::reposition()
{
    if (!m_anchor)
        return;
    if (m_managed) {
        repositionManaged(false);
        return;
    }
    if (!m_userPlaced)
        m_offset = defaultOffset(); // defaults track the anchor's size
    move(clampedPos(anchorOrigin() + m_offset));
}

QPoint FloatingToolWindow::anchorOrigin() const
{
    return m_anchor ? m_anchor->mapToGlobal(QPoint(0, 0)) : QPoint();
}

QRect FloatingToolWindow::marginRect() const
{
    // THE single place the kMargin window margin is applied, for managed and
    // unmanaged bars alike. The anchor (canvas viewport) IS the application
    // client area minus every docked panel, so deflating it by kMargin on all
    // four edges is the whole margin rule. All logical px: DPR-safe.
    if (!m_anchor)
        return QRect();
    return QRect(anchorOrigin(), m_anchor->size())
        .adjusted(kMargin, kMargin, -kMargin, -kMargin);
}

QPoint FloatingToolWindow::clampedPos(const QPoint &pos) const
{
    if (!m_anchor)
        return pos;
    // POSITION-only: x/y are bounded to the MARGINED anchor rect; width and
    // height are never touched — at an edge the window stops at full size.
    // qMax keeps the bound non-inverted for a bar larger than the margined
    // region: it rests at left/top instead of being pushed back outside.
    const QRect r = marginRect();
    const int maxX = qMax(r.left(), r.right() + 1 - width());
    const int maxY = qMax(r.top(), r.bottom() + 1 - height());
    return QPoint(qBound(r.left(), pos.x(), maxX),
                  qBound(r.top(), pos.y(), maxY));
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
    if (m_managed) {
        // Drag from anywhere on the BACKGROUND. Precedence: a press over
        // ANY child widget (button, slider, input, scrollbar) reaches that
        // child and never starts a drag; only leftover background drags.
        const QPoint at = event->position().toPoint();
        if (event->button() != Qt::LeftButton || !isDragBackgroundAt(at))
            return false;
        m_bgCandidate = true;
        m_bgDragging = false;
        m_dragStartGlobal = event->globalPosition().toPoint();
        m_dragStartPos = pos();
        return true;
    }
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
    if (m_managed) {
        if (!m_bgCandidate || !(event->buttons() & Qt::LeftButton)) {
            // Live affordance, re-derived from the CURRENT hit test on
            // every move: SizeAll over background, the child's own cursor
            // over a child. No latched hover state exists to go stale.
            updateDragCursor(event->position().toPoint());
            return false;
        }
        const QPoint global = event->globalPosition().toPoint();
        if (!m_bgDragging) {
            // Click-vs-drag threshold: a micro-move is still just a click.
            if ((global - m_dragStartGlobal).manhattanLength()
                < QApplication::startDragDistance())
                return true;
            m_bgDragging = true;
        }
        // Free-follow inside the margin-deflated region while dragging;
        // snapping and collision resolve on release.
        const QRect region = placementRegion();
        QPoint p = m_dragStartPos + (global - m_dragStartGlobal);
        p.setX(qBound(region.left(), p.x(),
                      qMax(region.left(), region.right() + 1 - width())));
        p.setY(qBound(region.top(), p.y(),
                      qMax(region.top(), region.bottom() + 1 - height())));
        move(p);
        return true;
    }
    if (!m_dragging || !(event->buttons() & Qt::LeftButton))
        return false;
    move(clampedPos(m_dragStartPos
                    + (event->globalPosition().toPoint() - m_dragStartGlobal)));
    return true;
}

bool FloatingToolWindow::handleGripRelease(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (m_managed) {
        if (!m_bgCandidate)
            return false;
        const bool moved = m_bgDragging;
        m_bgCandidate = false;
        m_bgDragging = false;
        if (moved)
            finishManagedDrag(); // snap detect + collision + persist
        // Sub-threshold press = a click on background: nothing moves.
        updateDragCursor(mapFromGlobal(QCursor::pos()));
        return true;
    }
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

void FloatingToolWindow::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    // Never leave a stale move cursor behind when the pointer exits.
    if (m_managed && !m_bgCandidate && !m_bgDragging)
        unsetCursor();
}

// Live affordance. SizeAll marks "this background drags the toolbar";
// over a child we unset ours so the child's own cursor shows.
void FloatingToolWindow::updateDragCursor(const QPoint &pos)
{
    if (!m_managed || m_bgCandidate || m_bgDragging)
        return;
    if (isDragBackgroundAt(pos))
        setCursor(Qt::SizeAllCursor);
    else
        unsetCursor();
}

bool FloatingToolWindow::isDragBackgroundAt(const QPoint &pos) const
{
    return m_managed && rect().contains(pos) && !childAt(pos)
        && !isInteractiveAt(pos);
}

// A drag interrupted by deactivation/focus loss (alt-tab, a dialog)
// cannot rely on a release event: land the bar in a valid position and
// leaves the bar in a valid position, exactly as a completed drag would.
bool FloatingToolWindow::event(QEvent *event)
{
    if (m_managed
        && (event->type() == QEvent::WindowDeactivate
            || event->type() == QEvent::FocusOut)
        && (m_bgCandidate || m_bgDragging)) {
        const bool moved = m_bgDragging;
        m_bgCandidate = false;
        m_bgDragging = false;
        unsetCursor();
        if (moved)
            finishManagedDrag();
    }
    return QWidget::event(event);
}

// --- Managed placement (Figma grab_CTL toolbars) -----------------------------

void FloatingToolWindow::enableManagedPlacement(const QString &name,
                                                DefaultCorner corner)
{
    static int nextOrder = 0;
    m_managed = true;
    m_name = name;
    m_corner = corner;
    m_placeOrder = nextOrder++;
    setMouseTracking(true); // live move-cursor without a button held
    loadManagedState();
}

QRect FloatingToolWindow::placementRegion() const
{
    // Managed placement constrains to exactly the same margined rect as the
    // unmanaged clamp: the anchor keeps bars off the docks, and marginRect()
    // supplies the edge margin.
    return marginRect();
}

QRect FloatingToolWindow::desiredManagedRect() const
{
    const QRect region = placementRegion();
    QPoint p;
    if (!m_hasPlacement) {
        switch (m_corner) { // first run: the Figma default corners
        case DefaultCorner::TopLeft:
            p = region.topLeft();
            break;
        case DefaultCorner::TopRight:
            p = QPoint(region.right() + 1 - width(), region.top());
            break;
        case DefaultCorner::BottomLeft:
            p = QPoint(region.left(), region.bottom() + 1 - height());
            break;
        case DefaultCorner::BottomRight:
            p = QPoint(region.right() + 1 - width(),
                       region.bottom() + 1 - height());
            break;
        }
    } else {
        p = anchorOrigin() + m_freeOffset;
        switch (m_snapEdge) { // a snapped bar sticks to its edge
        case SnapEdge::Left:   p.setX(region.left()); break;
        case SnapEdge::Right:  p.setX(region.right() + 1 - width()); break;
        case SnapEdge::Top:    p.setY(region.top()); break;
        case SnapEdge::Bottom: p.setY(region.bottom() + 1 - height()); break;
        case SnapEdge::None:   break;
        }
    }
    p.setX(qBound(region.left(), p.x(),
                  qMax(region.left(), region.right() + 1 - width())));
    p.setY(qBound(region.top(), p.y(),
                  qMax(region.top(), region.bottom() + 1 - height())));
    return QRect(p, size());
}

QVector<QRect> FloatingToolWindow::managedObstacles(bool yieldToAll) const
{
    // Every visible sibling FloatingToolWindow over the same anchor is an
    // obstacle, inflated by kMargin so the 4px spacing is part of the
    // obstacle itself. Frame rects include the whole layout rect — bar,
    // 4px gap, and pill strip — so the gap counts as OCCUPIED. Unmanaged
    // siblings (the Selection/Move Modifier bars, the Brush Size bar) are
    // ALWAYS obstacles: they place themselves and never yield. Among
    // managed bars, a dropped bar yields to everyone; a group reposition
    // lets later bars yield to earlier ones (fixed priority — no
    // oscillation).
    QVector<QRect> obstacles;
    const auto &all = FloatingToolWindowManager::instance()->windows();
    for (FloatingToolWindow *w : all) {
        if (w == this || w->m_anchor != m_anchor || !w->isVisible())
            continue;
        if (w->m_managed && !yieldToAll && w->m_placeOrder > m_placeOrder)
            continue;
        obstacles.append(w->frameGeometry().adjusted(-kMargin, -kMargin,
                                                     kMargin, kMargin));
    }
    return obstacles;
}

QRect FloatingToolWindow::placeRect(const QRect &desired,
                                    const QVector<QRect> &obstacles,
                                    bool preserveSnapAxis, bool *ok)
{
    const QRect region = placementRegion();
    m_lastPlaceTests = 0;
    *ok = false;
    if (region.width() < desired.width() || region.height() < desired.height())
        return desired; // cannot fit at all -> caller hides (fallback)

    auto fits = [&](const QRect &r) {
        ++m_lastPlaceTests;
        if (!region.contains(r))
            return false;
        for (const QRect &o : obstacles)
            if (r.intersects(o))
                return false;
        return true;
    };
    if (fits(desired)) {
        *ok = true;
        return desired;
    }

    // Finite candidate grid: the desired coordinates, the region bounds, and
    // every obstacle edge (obstacles are pre-inflated, so touching = 4px
    // apart). Nearest available = the fitting candidate with the smallest
    // squared displacement from the desired top-left.
    QVector<int> xs = {desired.x(), region.left(),
                       region.right() + 1 - desired.width()};
    QVector<int> ys = {desired.y(), region.top(),
                       region.bottom() + 1 - desired.height()};
    for (const QRect &o : obstacles) {
        xs.append(o.left() - desired.width());
        xs.append(o.right() + 1);
        ys.append(o.top() - desired.height());
        ys.append(o.bottom() + 1);
    }
    // A snapped bar first slides only along its edge (the snapped coordinate
    // is fixed); the caller falls back to the free search if nothing fits.
    if (preserveSnapAxis) {
        if (m_snapEdge == SnapEdge::Left || m_snapEdge == SnapEdge::Right)
            xs = {desired.x()};
        else if (m_snapEdge == SnapEdge::Top || m_snapEdge == SnapEdge::Bottom)
            ys = {desired.y()};
    }

    QRect best;
    qint64 bestScore = -1;
    for (int x : xs)
        for (int y : ys) {
            const QRect r(QPoint(x, y), desired.size());
            if (!fits(r))
                continue;
            const qint64 dx = x - desired.x();
            const qint64 dy = y - desired.y();
            const qint64 score = dx * dx + dy * dy;
            if (bestScore < 0 || score < bestScore
                || (score == bestScore
                    && (y < best.y() || (y == best.y() && x < best.x())))) {
                bestScore = score;
                best = r;
            }
        }
    if (bestScore >= 0) {
        *ok = true;
        return best;
    }
    return desired;
}

void FloatingToolWindow::repositionManaged(bool yieldToAll)
{
    if (!m_anchor)
        return;
    const QRect desired = desiredManagedRect();
    const QVector<QRect> obstacles = managedObstacles(yieldToAll);
    bool ok = false;
    QRect placed;
    int tests = 0;
    if (m_snapEdge != SnapEdge::None) {
        placed = placeRect(desired, obstacles, true, &ok); // slide on edge
        tests = m_lastPlaceTests;
    }
    if (!ok) {
        placed = placeRect(desired, obstacles, false, &ok);
        tests += m_lastPlaceTests;
    }
    m_lastPlaceTests = tests;
    if (!ok) {
        // Fallback (stated): no non-overlapping in-region position exists —
        // the bar hides rather than overlap or leave the window, and every
        // later reposition retries so it returns as soon as space frees up.
        m_noSpaceHidden = true;
        QWidget::setVisible(false);
        return;
    }
    if (m_noSpaceHidden) {
        m_noSpaceHidden = false;
        if (m_wantVisible)
            QWidget::setVisible(true);
    }
    move(placed.topLeft());
    // Placement may have moved the window under (or out from under) the
    // cursor: re-derive the live affordance from the CURRENT hit test.
    updateDragCursor(mapFromGlobal(QCursor::pos()));
}

void FloatingToolWindow::finishManagedDrag()
{
    const QRect region = placementRegion();
    const QRect r = frameGeometry();
    // Edge snapping: released within kSnapThreshold (16 logical px) of a
    // region edge snaps the bar to that edge (nearest edge wins).
    struct EdgeDist { SnapEdge edge; int dist; };
    const EdgeDist dists[4] = {
        {SnapEdge::Left, r.left() - region.left()},
        {SnapEdge::Right, region.right() - r.right()},
        {SnapEdge::Top, r.top() - region.top()},
        {SnapEdge::Bottom, region.bottom() - r.bottom()},
    };
    SnapEdge nearest = SnapEdge::None;
    int nearestDist = kSnapThreshold + 1;
    for (const EdgeDist &d : dists)
        if (d.dist >= 0 && d.dist < nearestDist) {
            nearestDist = d.dist;
            nearest = d.edge;
        }
    m_snapEdge = nearest;
    m_freeOffset = pos() - anchorOrigin();
    m_hasPlacement = true;
    repositionManaged(true); // dropped bar yields to every other bar
    m_freeOffset = pos() - anchorOrigin(); // keep what collision decided
    saveManagedState();
}

void FloatingToolWindow::saveManagedState() const
{
    if (m_name.isEmpty())
        return;
    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QString base =
        QStringLiteral("storyboard/floatToolbars/v2/") + m_name + QLatin1Char('/');
    settings.setValue(base + QStringLiteral("edge"), int(m_snapEdge));
    settings.setValue(base + QStringLiteral("offset"), m_freeOffset);
    settings.setValue(base + QStringLiteral("visible"), m_wantVisible);
}

void FloatingToolWindow::loadManagedState()
{
    if (m_name.isEmpty())
        return;
    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QString base =
        QStringLiteral("storyboard/floatToolbars/v2/") + m_name + QLatin1Char('/');
    if (!settings.contains(base + QStringLiteral("offset")))
        return; // first run: the default corner applies
    const int edge = settings.value(base + QStringLiteral("edge")).toInt();
    m_snapEdge = (edge >= int(SnapEdge::None) && edge <= int(SnapEdge::Bottom))
        ? SnapEdge(edge)
        : SnapEdge::None;
    m_freeOffset = settings.value(base + QStringLiteral("offset")).toPoint();
    m_hasPlacement = true;
    // A stale "hidden" is recoverable via the same toggle that hid it; the
    // restored position is validated by the clamp+collision reposition path.
    if (!settings.value(base + QStringLiteral("visible"), true).toBool())
        m_wantVisible = false;
}

void FloatingToolWindow::repositionManagedGroup(QWidget *anchor)
{
    // Deterministic single pass in priority order: each bar avoids only the
    // bars placed before it, so the pass can never oscillate.
    QVector<FloatingToolWindow *> managed;
    const auto &all = FloatingToolWindowManager::instance()->windows();
    for (FloatingToolWindow *w : all)
        if (w->m_managed && w->m_anchor == anchor)
            managed.append(w);
    std::sort(managed.begin(), managed.end(),
              [](const FloatingToolWindow *a, const FloatingToolWindow *b) {
                  return a->m_placeOrder < b->m_placeOrder;
              });
    for (FloatingToolWindow *w : managed)
        w->repositionManaged(false);
}

void FloatingToolWindow::restoreManagedState()
{
    if (!m_managed)
        return;
    m_wantVisible = isVisible() || m_wantVisible;
    loadManagedState();
    repositionManaged(false);
    applyEffectiveVisibility(); // a saved "hidden" restores hidden
}

void FloatingToolWindow::resetAllManagedPlacements()
{
    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    settings.remove(QStringLiteral("storyboard/floatToolbars"));
    QWidget *anchor = nullptr;
    const auto &all = FloatingToolWindowManager::instance()->windows();
    for (FloatingToolWindow *w : all)
        if (w->m_managed) {
            w->m_snapEdge = SnapEdge::None;
            w->m_hasPlacement = false;
            w->m_freeOffset = QPoint();
            anchor = w->m_anchor;
        }
    if (anchor)
        repositionManagedGroup(anchor);
}
