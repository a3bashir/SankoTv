#include "DockController.h"

#include "DockTitleBar.h"

#include <QAction>
#include <QCursor>
#include <QDockWidget>
#include <QEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSettings>
#include <QTimer>

namespace {

// Layout schema version for QMainWindow::saveState()/restoreState(). Starts
// at 1 for the native-docking migration: the previous ADS-format blobs are
// stored under a DIFFERENT settings key and are never fed to restoreState().
constexpr int kDockStateVersion = 1;

Qt::DockWidgetArea areaForZone(DockZone zone)
{
    switch (zone) {
    case DockZone::Left:   return Qt::LeftDockWidgetArea;
    case DockZone::Right:  return Qt::RightDockWidgetArea;
    case DockZone::Top:    return Qt::TopDockWidgetArea;
    case DockZone::Bottom: return Qt::BottomDockWidgetArea;
    default:               return Qt::RightDockWidgetArea;
    }
}

} // namespace

DockController::DockController(QMainWindow *host, const QString &settingsOrg,
                               const QString &settingsApp,
                               const QString &settingsGroup, QObject *parent)
    : QObject(parent ? parent : host), m_host(host),
      m_settingsOrg(settingsOrg), m_settingsApp(settingsApp),
      m_settingsGroup(settingsGroup)
{
    m_overlay = new DockOverlay(host);
    // Sidebars are single COLUMNS: panels stack vertically or tab, never
    // side-by-side within one area. Nesting off keeps every cell of a
    // sidebar at one uniform width, which is what makes capturing and
    // preserving "the sidebar width" well-defined.
    m_host->setDockNestingEnabled(false);
}

QDockWidget *DockController::addPanel(QWidget *content, const QString &title,
                                      const QString &objectName)
{
    QDockWidget *dock = new QDockWidget(title, m_host);
    dock->setObjectName(objectName); // saveState() keys layouts on this
    // The custom title bar replaces Qt's stock one: draggable with a
    // threshold, close button, double-click collapse — and no float icon.
    dock->setTitleBarWidget(new DockTitleBar(dock, this));
    // Panels live in the LEFT/RIGHT sidebars only (vertical stacking happens
    // INSIDE a sidebar); the top/bottom application edges are never dockable
    // and the central canvas can never become a dock area.
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    // Closable is REQUIRED: without it QDockWidget::closeEvent() ignores
    // close(), which would dead-wire the title bar's own Close button and
    // the View menu toggles. Movable stays off — every move goes through the
    // DockTitleBar drag, so Qt never starts its native drag.
    dock->setFeatures(QDockWidget::DockWidgetClosable
                      | QDockWidget::DockWidgetFloatable);
    dock->setWidget(content);
    m_panels.append(dock);
    return dock;
}

// Remember the size the user left a floating, expanded panel at. Called at
// the two moments the value matters — when the panel redocks and when the
// layout is saved — instead of from a resize-event filter, which would race
// Qt's own transient resizes around setFloating() and record sizes the user
// never chose. Every docked->floating transition goes through
// floatDockForDrag()/applyFloatingSize(), so a floating panel's size is
// always either the normalized default or a deliberate manual resize.
void DockController::rememberFloatingSize(QDockWidget *dock)
{
    if (dock->isFloating() && !isDockCollapsed(dock)
        && dock->size().isValid())
        dock->setProperty("userFloatSize", dock->size());
}

void DockController::setDefaultLayout(std::function<void()> defaultLayout)
{
    m_defaultLayout = std::move(defaultLayout);
}

// --- persistence -------------------------------------------------------------

void DockController::saveLayout()
{
    QSettings settings(m_settingsOrg, m_settingsApp);
    settings.setValue(m_settingsGroup + QStringLiteral("/state"),
                      m_host->saveState(kDockStateVersion));
    for (QDockWidget *dock : m_panels) {
        rememberFloatingSize(dock); // a floating panel's size is the user's
        const QString base = m_settingsGroup + QStringLiteral("/panels/")
            + dock->objectName() + QLatin1Char('/');
        const bool open = dock->toggleViewAction()->isChecked();
        settings.setValue(base + QStringLiteral("open"), open);
        settings.setValue(base + QStringLiteral("collapsed"),
                          isDockCollapsed(dock));
        settings.setValue(base + QStringLiteral("expandedHeight"),
                          dock->property("expandedHeight").toInt());
        // The active member of a tab group is the one actually painted.
        settings.setValue(base + QStringLiteral("active"),
                          open && dock->isVisible()
                              && !dock->visibleRegion().isEmpty());
        settings.setValue(base + QStringLiteral("floating"),
                          dock->isFloating());
        settings.setValue(base + QStringLiteral("floatGeometry"),
                          dock->geometry());
        // The panel's manually chosen floating size (invalid when the user
        // never resized it while floating -> default applies on restore).
        settings.setValue(base + QStringLiteral("floatSize"),
                          dock->property("userFloatSize").toSize());
    }
    // Manually chosen sidebar widths, so a restart reproduces them exactly.
    settings.setValue(m_settingsGroup + QStringLiteral("/sidebar/left"),
                      sidebarWidth(Qt::LeftDockWidgetArea));
    settings.setValue(m_settingsGroup + QStringLiteral("/sidebar/right"),
                      sidebarWidth(Qt::RightDockWidgetArea));
    settings.sync();
}

bool DockController::restoreLayout()
{
    QSettings settings(m_settingsOrg, m_settingsApp);
    const QByteArray state =
        settings.value(m_settingsGroup + QStringLiteral("/state"))
            .toByteArray();
    if (state.isEmpty())
        return false;
    // Versioned restore; a rejected blob (older schema, corrupt data) leaves
    // the caller to apply the default layout.
    if (!m_host->restoreState(state, kDockStateVersion))
        return false;
    // A saved layout may pre-date the left/right-only rule: pull any
    // top/bottom dock into the right sidebar before per-panel state applies.
    normalizeDockAreas();

    QVector<QDockWidget *> activeTabs;
    for (QDockWidget *dock : m_panels) {
        const QString base = m_settingsGroup + QStringLiteral("/panels/")
            + dock->objectName() + QLatin1Char('/');
        const int expandedHeight =
            settings.value(base + QStringLiteral("expandedHeight")).toInt();
        if (expandedHeight > 0)
            dock->setProperty("expandedHeight", expandedHeight);
        const QSize floatSize =
            settings.value(base + QStringLiteral("floatSize")).toSize();
        if (floatSize.isValid())
            dock->setProperty("userFloatSize", floatSize);
        // Collapse is OUR state, invisible to restoreState(); reapply it
        // without animation so startup shows the final layout directly.
        setDockCollapsed(
            dock,
            settings.value(base + QStringLiteral("collapsed"), false).toBool(),
            false);
        const bool open =
            settings.value(base + QStringLiteral("open"), true).toBool();
        dock->toggleViewAction()->setChecked(open);
        if (settings.value(base + QStringLiteral("floating"), false).toBool()
            && open) {
            const QRect geometry =
                settings.value(base + QStringLiteral("floatGeometry"))
                    .toRect();
            if (geometry.isValid()) {
                dock->setFloating(true);
                // Validate the remembered size against the live screen and
                // the panel's constraints; degenerate/oversized values fall
                // back to the normalized default. Collapsed panels keep the
                // pinned title-bar height.
                applyFloatingSize(dock);
                // A monitor may have been unplugged since the save: clamp
                // the remembered position back onto a live screen.
                dock->move(clampedFloatingPosition(dock, geometry.topLeft()));
            }
        }
        if (settings.value(base + QStringLiteral("active"), false).toBool()
            && open)
            activeTabs.append(dock);
    }
    for (QDockWidget *dock : activeTabs)
        dock->raise(); // restore the front tab of each tab group

    // Reproduce the manually sized sidebars (validated: applySidebarWidth
    // ignores widths below the minimum; -1 means the sidebar was empty).
    // Deferred one event-loop turn: restoreState() finishes dock geometry in
    // a deferred layout pass that would overwrite a synchronous resize.
    const int leftWidth =
        settings.value(m_settingsGroup + QStringLiteral("/sidebar/left"), -1)
            .toInt();
    const int rightWidth =
        settings.value(m_settingsGroup + QStringLiteral("/sidebar/right"), -1)
            .toInt();
    QTimer::singleShot(0, this, [this, leftWidth, rightWidth] {
        if (leftWidth > 0 && leftWidth < m_host->width())
            applySidebarWidth(Qt::LeftDockWidgetArea, leftWidth);
        if (rightWidth > 0 && rightWidth < m_host->width())
            applySidebarWidth(Qt::RightDockWidgetArea, rightWidth);
    });
    return true;
}

void DockController::resetLayout()
{
    for (QDockWidget *dock : m_panels) {
        setDockCollapsed(dock, false, false);
        dock->setFloating(false);
        clearWidthConstraints(dock);
        dock->show();
    }
    if (m_defaultLayout)
        m_defaultLayout();
}

void DockController::clearSavedLayout()
{
    QSettings settings(m_settingsOrg, m_settingsApp);
    settings.remove(m_settingsGroup);
}

// --- menu integration --------------------------------------------------------

void DockController::populatePanelsMenu(QMenu *menu)
{
    for (QDockWidget *dock : m_panels)
        menu->addAction(dock->toggleViewAction());
    menu->addSeparator();
    QAction *showAll = menu->addAction(QStringLiteral("Show All Panels"));
    connect(showAll, &QAction::triggered, this,
            &DockController::showAllPanels);
    QAction *hideAll = menu->addAction(QStringLiteral("Hide All Panels"));
    connect(hideAll, &QAction::triggered, this,
            &DockController::hideAllPanels);
}

void DockController::showAllPanels()
{
    for (QDockWidget *dock : m_panels)
        dock->show();
}

void DockController::hideAllPanels()
{
    for (QDockWidget *dock : m_panels)
        dock->hide();
}

// --- drag preview / drop -----------------------------------------------------

void DockController::updateDockPreview(QDockWidget *dragged,
                                       const QPoint &globalPos)
{
    const DockHit hit = detectDockHit(dragged, globalPos);
    if (hit.zone == DockZone::None) {
        m_overlay->hidePreview();
        return;
    }
    m_overlay->showPreview(hit.preview, hit.zone, hit.target);
}

void DockController::finishDock(QDockWidget *dragged, const QPoint &globalPos)
{
    m_overlay->hidePreview();
    // The size the user left the floating panel at survives the redock and
    // is reused by the next docked->floating transition.
    rememberFloatingSize(dragged);
    const DockHit hit = detectDockHit(dragged, globalPos);

    if (hit.zone == DockZone::None) {
        // Stays floating where the user dropped it — clamped so a usable
        // strip remains on-screen.
        dragged->move(clampedFloatingPosition(dragged, dragged->pos()));
        return;
    }

    if (hit.target) {
        Qt::DockWidgetArea area = m_host->dockWidgetArea(hit.target);
        if (area != Qt::LeftDockWidgetArea && area != Qt::RightDockWidgetArea)
            area = Qt::RightDockWidgetArea; // sidebars only
        if (hit.zone == DockZone::Center) {
            // Centre drop -> a normal tab on the target's group; the
            // destination sidebar keeps its manually chosen width.
            // addDockWidget() alone does NOT undock a floating dock (it only
            // assigns the area), so clear the floating state explicitly.
            preserveSidebarWidth(area, [&] {
                dragged->setFloating(false);
                clearWidthConstraints(dragged);
                m_host->addDockWidget(area, dragged);
                m_host->tabifyDockWidget(hit.target, dragged);
                dragged->show();
                dragged->raise();
            });
        } else {
            preserveSidebarWidth(area, [&] {
                splitBesideDockGroup(dragged, hit.target, hit.zone);
            });
        }
        return;
    }

    // Host-edge drop (left/right bands only): dock to that sidebar, keeping
    // its width (or the default when the sidebar was empty).
    preserveSidebarWidth(areaForZone(hit.zone), [&] {
        dragged->setFloating(false);
        clearWidthConstraints(dragged);
        m_host->addDockWidget(areaForZone(hit.zone), dragged);
        dragged->show();
    });
}

// Qt documents that splitDockWidget() "will add the second dock widget as a
// new tab" when the first is already tabified — so a direct call CANNOT
// satisfy an edge drop beside a tab group. Dissolve, split, rebuild:
void DockController::splitBesideDockGroup(QDockWidget *dragged,
                                          QDockWidget *target, DockZone zone)
{
    Qt::DockWidgetArea area = m_host->dockWidgetArea(target);
    if (area == Qt::NoDockWidgetArea)
        area = Qt::RightDockWidgetArea;
    const Qt::Orientation orientation =
        (zone == DockZone::Left || zone == DockZone::Right) ? Qt::Horizontal
                                                            : Qt::Vertical;
    const bool draggedFirst =
        zone == DockZone::Left || zone == DockZone::Top;

    // Capture the target's tab group (never includes the floating dragged
    // dock, but filter defensively), each member's open state, and which
    // member the user currently sees.
    QList<QDockWidget *> group = m_host->tabifiedDockWidgets(target);
    group.removeAll(dragged);
    QDockWidget *active = activeTabOf(target);

    struct Saved { QDockWidget *dock; bool open; };
    QVector<Saved> saved;
    saved.reserve(group.size());

    // No intermediate layouts on screen while the group is rebuilt.
    m_host->setUpdatesEnabled(false);

    // 1. Temporarily pull every OTHER group member out of the layout, so the
    //    target stands alone and a real split against it is possible. The
    //    members keep their objectName, collapsed state (dynamic properties
    //    + pinned heights live on the widget) and expanded height.
    for (QDockWidget *member : group) {
        saved.append({member, member->toggleViewAction()->isChecked()});
        m_host->removeDockWidget(member);
    }
    if (!group.isEmpty()) {
        // Even with every sibling removed, Qt keeps the target's dock cell
        // flagged as a TAB AREA, and splitDockWidget() against a tabbed cell
        // still degrades to "add as another tab". Re-adding the target puts
        // it in a fresh, un-tabbed cell that CAN be split.
        const bool targetOpen = target->toggleViewAction()->isChecked();
        m_host->removeDockWidget(target);
        m_host->addDockWidget(area, target);
        target->setVisible(targetOpen);
    }

    // 2. Insert the dragged dock and split on the requested side of the
    //    now-lone target. (addDockWidget() alone does not undock a floating
    //    dock — clear the floating state first.)
    if (dragged->isFloating())
        dragged->setFloating(false);
    clearWidthConstraints(dragged);
    m_host->addDockWidget(area, dragged);
    if (draggedFirst)
        m_host->splitDockWidget(dragged, target, orientation);
    else
        m_host->splitDockWidget(target, dragged, orientation);
    dragged->show();

    // 3. Rebuild the original tab group on the target and restore each
    //    member's open/closed state.
    for (const Saved &s : saved) {
        m_host->addDockWidget(area, s.dock);
        m_host->tabifyDockWidget(target, s.dock);
        s.dock->setVisible(s.open);
    }

    // 4. Put the previously active tab back in front.
    if (active && active->toggleViewAction()->isChecked())
        active->raise();

    m_host->setUpdatesEnabled(true);
}

QPoint DockController::clampedFloatingPosition(QDockWidget *dock,
                                               const QPoint &wanted) const
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::screenAt(wanted);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return wanted;
    const QRect available = screen->availableGeometry();
    // At least this much of the panel stays reachable on-screen.
    constexpr int visibleHandle = 60;
    const int minX = available.left() - dock->width() + visibleHandle;
    const int maxX = available.right() - visibleHandle;
    const int minY = available.top();
    const int maxY = available.bottom() - visibleHandle;
    return QPoint(qBound(minX, wanted.x(), maxX),
                  qBound(minY, wanted.y(), maxY));
}

// --- floating-size normalization ---------------------------------------------

void DockController::floatDockForDrag(QDockWidget *dock)
{
    // Pulling a panel out changes the sidebar's content and can trigger a
    // narrower relayout of the column it leaves — keep that width.
    releaseSidebarWidthHolds(); // capture real, unconstrained geometry
    const Qt::DockWidgetArea area =
        dock->isFloating() ? Qt::NoDockWidgetArea
                           : m_host->dockWidgetArea(dock);
    const int keepWidth =
        (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea)
            ? sidebarWidth(area)
            : -1;
    dock->setFloating(true);
    // A width hold from an operation earlier in this tick would pin the
    // now-floating window's width — lift it before sizing the float.
    releaseWidthHold(dock);
    if (keepWidth >= kMinSidebarWidth)
        applySidebarWidth(area, keepWidth);
    applyFloatingSize(dock);
    // QMainWindow re-applies dock-derived geometry in a deferred layout pass
    // after setFloating(true); re-assert the normalized size once from the
    // event loop so the panel cannot flash back to its tall docked size.
    // ONE-shot only — later resizes belong to the user and are never reset.
    QPointer<QDockWidget> guard(dock);
    QTimer::singleShot(0, this, [this, guard] {
        if (guard)
            applyFloatingSize(guard);
    });
}

void DockController::applyFloatingSize(QDockWidget *dock)
{
    const QSize size = normalizedFloatingSize(dock);
    if (isDockCollapsed(dock)) {
        // A collapsed panel stays collapsed: width normalizes, the height is
        // pinned to the title bar and must not expand.
        dock->resize(size.width(), kTitleBarHeight);
        return;
    }
    dock->resize(size);
}

QSize DockController::normalizedFloatingSize(QDockWidget *dock) const
{
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    const QSize available = screen ? screen->availableGeometry().size()
                                   : QSize(1280, 800);

    // The panel's remembered manual floating size wins when it is sane;
    // missing, degenerate, or larger-than-screen values fall back to the
    // one central default.
    QSize size = kDefaultFloatingSize;
    const QSize saved = dock->property("userFloatSize").toSize();
    if (saved.isValid() && saved.width() >= kMinSidebarWidth
        && saved.height() >= kTitleBarHeight
        && saved.width() <= available.width()
        && saved.height() <= available.height())
        size = saved;

    size = size.expandedTo(dock->widget() ? dock->widget()->minimumSizeHint()
                                          : QSize());
    return size.boundedTo(available);
}

// --- sidebar-width preservation ----------------------------------------------

QDockWidget *DockController::representativeDock(Qt::DockWidgetArea area) const
{
    // Prefer a painted, non-collapsed member (one representative per tab
    // group, per resizeDocks() docs; a collapsed dock's pinned heights make
    // it a poor resize proxy). Painted-but-collapsed beats tabbed-behind;
    // any open dock in the area is the last resort.
    QDockWidget *painted = nullptr;
    QDockWidget *fallback = nullptr;
    for (QDockWidget *dock : m_panels) {
        if (dock->isFloating() || !dock->toggleViewAction()->isChecked()
            || m_host->dockWidgetArea(dock) != area)
            continue;
        if (dock->isVisible() && !dock->visibleRegion().isEmpty()) {
            if (!isDockCollapsed(dock))
                return dock;
            if (!painted)
                painted = dock;
        } else if (!fallback) {
            fallback = dock;
        }
    }
    return painted ? painted : fallback;
}

int DockController::sidebarWidth(Qt::DockWidgetArea area) const
{
    const QDockWidget *dock = representativeDock(area);
    return dock ? dock->width() : -1;
}

void DockController::applySidebarWidth(Qt::DockWidgetArea area, int width)
{
    if (width < kMinSidebarWidth)
        return;
    // One representative per tab group (resizeDocks() docs), but EVERY
    // distinct cell of the sidebar: vertically stacked cells each need the
    // width or a cell (e.g. a collapsed panel) can keep a stale narrow width
    // after restoreState().
    QList<QDockWidget *> cells;
    for (QDockWidget *dock : m_panels) {
        if (dock->isFloating() || !dock->toggleViewAction()->isChecked()
            || m_host->dockWidgetArea(dock) != area)
            continue;
        bool grouped = false;
        for (QDockWidget *cell : cells)
            if (m_host->tabifiedDockWidgets(cell).contains(dock)) {
                grouped = true;
                break;
            }
        if (!grouped)
            cells.append(dock);
    }
    if (cells.isEmpty())
        return;

    // Qt re-derives dock sizes from size hints in DEFERRED layout passes
    // (LayoutRequest after a collapse, tabify, split or restoreState), which
    // can override a plain resizeDocks() no matter when it runs. A TEMPORARY
    // hard width constraint on every cell wins those passes by definition;
    // it is lifted two event-loop turns later (after the deferred passes
    // have settled) so the user can immediately resize the sidebar again.
    for (QDockWidget *cell : cells) {
        cell->setProperty("widthHoldActive", true);
        cell->setProperty("heldWidth", width);
        cell->setMinimumWidth(width);
        cell->setMaximumWidth(width);
    }
    m_host->resizeDocks(cells, QList<int>(cells.size(), width),
                        Qt::Horizontal);
    QTimer::singleShot(0, this, [this] {
        QTimer::singleShot(0, this,
                           [this] { releaseSidebarWidthHolds(); });
    });
}

void DockController::releaseWidthHold(QDockWidget *dock)
{
    if (!dock->property("widthHoldActive").toBool())
        return;
    // Back to Qt's defaults, NOT to a remembered pair: the dock widgets
    // never carry width constraints of their own (the panel content's
    // minimumSizeHint governs), and restoring a remembered pair could bake
    // in a constraint captured while another hold was active. The collapse
    // feature's HEIGHT pins are untouched.
    dock->setMinimumWidth(0);
    dock->setMaximumWidth(QWIDGETSIZE_MAX);
    dock->setProperty("widthHoldActive", false);
}

// Lift every temporary width constraint (see applySidebarWidth). Scans ALL
// panels — a held cell may have floated or switched areas in the meantime
// and must still get its original min/max width back.
void DockController::releaseSidebarWidthHolds()
{
    QList<QDockWidget *> held;
    QList<int> widths;
    for (QDockWidget *dock : m_panels) {
        if (!dock->property("widthHoldActive").toBool())
            continue;
        const int width = dock->property("heldWidth").toInt();
        releaseWidthHold(dock);
        if (!dock->isFloating() && width >= kMinSidebarWidth
            && dock->toggleViewAction()->isChecked()) {
            held.append(dock);
            widths.append(width);
        }
    }
    // Lifting a constraint itself triggers a relayout that can re-derive
    // the sidebar from size hints (shrinking it to the panels' minimums);
    // immediately re-asserting the explicit sizes makes that relayout keep
    // the held widths — explicit dock sizes stick as long as no further
    // hint change follows.
    if (!held.isEmpty())
        m_host->resizeDocks(held, widths, Qt::Horizontal);
}

// QDockWidget syncs a FLOATING dock's window min/max size to its content
// hints (a collapsed float gets pinned to the bare title bar, e.g. 96px
// wide) and those pins survive setFloating(false). Every path that returns
// a dock to the layout clears them, or the redocked panel refuses to track
// its sidebar's width. Never touches an active width hold or the collapse
// feature's height pins.
void DockController::clearWidthConstraints(QDockWidget *dock)
{
    if (dock->property("widthHoldActive").toBool())
        return;
    dock->setMinimumWidth(0);
    dock->setMaximumWidth(QWIDGETSIZE_MAX);
}

void DockController::preserveSidebarWidth(
    Qt::DockWidgetArea area, const std::function<void()> &operation)
{
    // Capture BEFORE the drop mutates the layout (the dragged dock is
    // floating, so it never serves as the measuring representative). An
    // empty sidebar has no width yet and starts at the default. Any hold
    // still pending from an operation earlier in this tick is lifted first
    // so the capture reads real, unconstrained geometry.
    releaseSidebarWidthHolds();
    const int width = sidebarWidth(area);
    operation();
    applySidebarWidth(area, width > 0 ? width : kDefaultSidebarWidth);
}

// Saved layouts that pre-date the left/right-only rule may still place a
// dock in a top/bottom area; move any such dock into the right sidebar.
void DockController::normalizeDockAreas()
{
    for (QDockWidget *dock : m_panels) {
        if (dock->isFloating())
            continue;
        const Qt::DockWidgetArea area = m_host->dockWidgetArea(dock);
        if (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea)
            m_host->addDockWidget(Qt::RightDockWidgetArea, dock);
    }
}

// --- collapse ----------------------------------------------------------------

bool DockController::isDockCollapsed(const QDockWidget *dock) const
{
    return dock->property("collapsed").toBool();
}

void DockController::toggleDockCollapse(QDockWidget *dock)
{
    setDockCollapsed(dock, !isDockCollapsed(dock), true);
}

// Collapse state lives in dynamic properties ON THE DOCK plus its pinned
// min/max heights, so it survives moving, floating and redocking without any
// bookkeeping: nothing resets it on drop. The content widget is hidden while
// collapsed; only the title bar remains.
void DockController::setDockCollapsed(QDockWidget *dock, bool collapsed,
                                      bool animate)
{
    if (isDockCollapsed(dock) == collapsed
        || dock->property("collapseAnimating").toBool())
        return;

    constexpr int animationDuration = 170;

    // Collapsing/expanding changes the dock's minimum size hints, which can
    // make QMainWindow relayout the whole sidebar column narrower — capture
    // the width up front and re-assert it once the state change lands.
    releaseSidebarWidthHolds(); // capture real, unconstrained geometry
    const Qt::DockWidgetArea area = dock->isFloating()
        ? Qt::NoDockWidgetArea
        : m_host->dockWidgetArea(dock);
    const int keepWidth =
        (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea)
            ? sidebarWidth(area)
            : -1;
    const auto keepSidebar = [this, area, keepWidth] {
        if (keepWidth >= kMinSidebarWidth)
            applySidebarWidth(area, keepWidth);
    };

    if (collapsed) {
        dock->setProperty("expandedHeight", qMax(dock->height(), 120));
        dock->setProperty("savedMinimumHeight", dock->minimumHeight());
        dock->setProperty("savedMaximumHeight", dock->maximumHeight());
        // The content widget stays VISIBLE: the height pin alone clips it
        // behind the title bar. Hiding it would drop the dock's size hints
        // to bare-title-bar dimensions, and every later relayout would
        // re-derive (shrink) the whole sidebar from those hints.
        const auto finish = [dock, keepSidebar] {
            dock->setMinimumHeight(kTitleBarHeight);
            dock->setMaximumHeight(kTitleBarHeight);
            dock->setProperty("collapsed", true);
            dock->setProperty("collapseAnimating", false);
            keepSidebar();
        };
        dock->setMinimumHeight(kTitleBarHeight);
        if (!animate) {
            finish();
            return;
        }
        dock->setProperty("collapseAnimating", true);
        auto *animation = new QPropertyAnimation(dock, "maximumHeight", dock);
        animation->setDuration(animationDuration);
        animation->setStartValue(dock->height());
        animation->setEndValue(kTitleBarHeight);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QPropertyAnimation::finished, dock, finish);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
        return;
    }

    const int expandedHeight =
        qMax(dock->property("expandedHeight").toInt(), 120);
    const auto finish = [this, dock, keepSidebar] {
        const int savedMin = dock->property("savedMinimumHeight").toInt();
        const int savedMax = dock->property("savedMaximumHeight").toInt();
        dock->setMinimumHeight(qMax(0, savedMin));
        dock->setMaximumHeight(savedMax > 0 ? savedMax : QWIDGETSIZE_MAX);
        dock->setProperty("collapsed", false);
        dock->setProperty("collapseAnimating", false);
        if (!dock->isFloating())
            clearWidthConstraints(dock); // stale float pins block resizing
        dock->updateGeometry();
        keepSidebar();
    };
    if (!animate) {
        finish();
        if (!dock->isFloating())
            m_host->resizeDocks({dock}, {expandedHeight}, Qt::Vertical);
        else
            dock->resize(dock->width(), expandedHeight);
        return;
    }
    dock->setProperty("collapseAnimating", true);
    auto *animation = new QPropertyAnimation(dock, "maximumHeight", dock);
    animation->setDuration(animationDuration);
    animation->setStartValue(kTitleBarHeight);
    animation->setEndValue(expandedHeight);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, dock, finish);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

// --- hit testing -------------------------------------------------------------

QDockWidget *DockController::activeTabOf(QDockWidget *member) const
{
    // The painted member of a tab group; `member` itself when unpainted
    // (tabbed behind) but no sibling qualifies either.
    if (member->isVisible() && !member->visibleRegion().isEmpty())
        return member;
    const QList<QDockWidget *> siblings = m_host->tabifiedDockWidgets(member);
    for (QDockWidget *sibling : siblings)
        if (sibling->isVisible() && !sibling->visibleRegion().isEmpty())
            return sibling;
    return member;
}

QDockWidget *DockController::dockAt(QDockWidget *dragged,
                                    const QPoint &hostPos) const
{
    for (QDockWidget *dock : m_panels) {
        if (dock == dragged || dock->isFloating() || !dock->isVisible())
            continue;
        const QRect rect(dock->mapTo(m_host, QPoint(0, 0)), dock->size());
        if (rect.contains(hostPos))
            return dock;
    }
    return nullptr;
}

DockHit DockController::detectDockHit(QDockWidget *dragged,
                                      const QPoint &globalPos) const
{
    DockHit hit;
    const QPoint hostPos = m_host->mapFromGlobal(globalPos);
    if (!m_host->rect().contains(hostPos))
        return hit; // outside the host: stays floating

    if (QDockWidget *target = dockAt(dragged, hostPos)) {
        // Sidebars are single columns, so a target offers only VERTICAL
        // stacking (top/bottom edges -> split above/below) or tabbing
        // (everywhere else, its left/right edges included). Docking beside a
        // sidebar goes through the host-edge bands below.
        const QRect rect(target->mapTo(m_host, QPoint(0, 0)), target->size());
        const QPoint local = hostPos - rect.topLeft();
        const int edgeY = qMax(46, rect.height() / 4);
        DockZone zone = DockZone::Center;
        if (local.y() < edgeY)
            zone = DockZone::Top;
        else if (local.y() > rect.height() - edgeY)
            zone = DockZone::Bottom;
        hit.zone = zone;
        hit.target = target;
        hit.preview = targetPreview(target, zone);
        return hit;
    }

    // Not over a dock: only the LEFT/RIGHT host-edge bands are docking
    // targets — panels live in the sidebars, so the top/bottom application
    // edges never dock, never preview, and the central canvas can never
    // become a top/bottom dock area. (Vertical stacking still happens INSIDE
    // a sidebar via the target-edge zones above.) Anywhere else keeps the
    // panel floating with no indicator.
    constexpr int edge = 120;
    const QRect r = m_host->rect();
    DockZone zone = DockZone::None;
    if (hostPos.x() < edge)
        zone = DockZone::Left;
    else if (hostPos.x() > r.width() - edge)
        zone = DockZone::Right;
    if (zone == DockZone::None)
        return hit;
    hit.zone = zone;
    hit.preview = hostPreview(zone);
    return hit;
}

QRect DockController::hostPreview(DockZone zone) const
{
    const QRect r = m_host->rect();
    switch (zone) {
    case DockZone::Left:
        return QRect(r.left(), r.top(), r.width() / 3, r.height());
    case DockZone::Right:
        return QRect(r.right() - r.width() / 3, r.top(), r.width() / 3,
                     r.height());
    case DockZone::Top:
        return QRect(r.left(), r.top(), r.width(), r.height() / 3);
    case DockZone::Bottom:
        return QRect(r.left(), r.bottom() - r.height() / 3, r.width(),
                     r.height() / 3);
    default:
        return r.adjusted(r.width() / 5, r.height() / 5, -r.width() / 5,
                          -r.height() / 5);
    }
}

QRect DockController::targetPreview(QDockWidget *target, DockZone zone) const
{
    const QRect r(target->mapTo(m_host, QPoint(0, 0)), target->size());
    // Edge zones show the thin insertion line (the overlay derives the line
    // from the rect's centreline); Centre shows the tab preview fill.
    constexpr int indicatorThickness = 4;
    switch (zone) {
    case DockZone::Left:
        return QRect(r.left(), r.top(), indicatorThickness, r.height());
    case DockZone::Right:
        return QRect(r.right() - indicatorThickness, r.top(),
                     indicatorThickness, r.height());
    case DockZone::Top:
        return QRect(r.left(), r.top(), r.width(), indicatorThickness);
    case DockZone::Bottom:
        return QRect(r.left(), r.bottom() - indicatorThickness, r.width(),
                     indicatorThickness);
    default:
        return r.adjusted(18, 18, -18, -18);
    }
}
