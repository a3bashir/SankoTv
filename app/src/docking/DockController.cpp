#include "DockController.h"

#include "DockTitleBar.h"

#include <QAction>
#include <QCursor>
#include <QDockWidget>
#include <QGuiApplication>
#include <QMainWindow>
#include <QMenu>
#include <QPropertyAnimation>
#include <QScreen>
#include <QSettings>

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
    // Edge drops beside an existing dock need nested layouts to express
    // arbitrary split arrangements.
    m_host->setDockNestingEnabled(true);
}

QDockWidget *DockController::addPanel(QWidget *content, const QString &title,
                                      const QString &objectName)
{
    QDockWidget *dock = new QDockWidget(title, m_host);
    dock->setObjectName(objectName); // saveState() keys layouts on this
    // The custom title bar replaces Qt's stock one: draggable with a
    // threshold, close button, double-click collapse — and no float icon.
    dock->setTitleBarWidget(new DockTitleBar(dock, this));
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
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
    }
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

    QVector<QDockWidget *> activeTabs;
    for (QDockWidget *dock : m_panels) {
        const QString base = m_settingsGroup + QStringLiteral("/panels/")
            + dock->objectName() + QLatin1Char('/');
        const int expandedHeight =
            settings.value(base + QStringLiteral("expandedHeight")).toInt();
        if (expandedHeight > 0)
            dock->setProperty("expandedHeight", expandedHeight);
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
                dock->resize(geometry.size());
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
    return true;
}

void DockController::resetLayout()
{
    for (QDockWidget *dock : m_panels) {
        setDockCollapsed(dock, false, false);
        dock->setFloating(false);
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
    const DockHit hit = detectDockHit(dragged, globalPos);

    if (hit.zone == DockZone::None) {
        // Stays floating where the user dropped it — clamped so a usable
        // strip remains on-screen.
        dragged->move(clampedFloatingPosition(dragged, dragged->pos()));
        return;
    }

    if (hit.target) {
        if (hit.zone == DockZone::Center) {
            // Centre drop -> a normal tab on the target's group.
            // addDockWidget() alone does NOT undock a floating dock (it only
            // assigns the area), so clear the floating state explicitly.
            const Qt::DockWidgetArea area =
                m_host->dockWidgetArea(hit.target) == Qt::NoDockWidgetArea
                    ? Qt::RightDockWidgetArea
                    : m_host->dockWidgetArea(hit.target);
            dragged->setFloating(false);
            m_host->addDockWidget(area, dragged);
            m_host->tabifyDockWidget(hit.target, dragged);
            dragged->show();
            dragged->raise();
        } else {
            splitBesideDockGroup(dragged, hit.target, hit.zone);
        }
        return;
    }

    // Host-edge drop: dock to that side of the main window.
    dragged->setFloating(false);
    m_host->addDockWidget(areaForZone(hit.zone), dragged);
    dragged->show();
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

    if (collapsed) {
        dock->setProperty("expandedHeight", qMax(dock->height(), 120));
        dock->setProperty("savedMinimumHeight", dock->minimumHeight());
        dock->setProperty("savedMaximumHeight", dock->maximumHeight());
        const auto finish = [dock] {
            if (dock->widget())
                dock->widget()->hide();
            dock->setMinimumHeight(kTitleBarHeight);
            dock->setMaximumHeight(kTitleBarHeight);
            dock->setProperty("collapsed", true);
            dock->setProperty("collapseAnimating", false);
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
    const auto finish = [dock] {
        const int savedMin = dock->property("savedMinimumHeight").toInt();
        const int savedMax = dock->property("savedMaximumHeight").toInt();
        dock->setMinimumHeight(qMax(0, savedMin));
        dock->setMaximumHeight(savedMax > 0 ? savedMax : QWIDGETSIZE_MAX);
        dock->setProperty("collapsed", false);
        dock->setProperty("collapseAnimating", false);
        dock->updateGeometry();
    };
    if (dock->widget())
        dock->widget()->show();
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
        const QRect rect(target->mapTo(m_host, QPoint(0, 0)), target->size());
        const QPoint local = hostPos - rect.topLeft();
        const int edgeX = qMax(46, rect.width() / 4);
        const int edgeY = qMax(46, rect.height() / 4);
        DockZone zone = DockZone::Center;
        if (local.x() < edgeX)
            zone = DockZone::Left;
        else if (local.x() > rect.width() - edgeX)
            zone = DockZone::Right;
        else if (local.y() < edgeY)
            zone = DockZone::Top;
        else if (local.y() > rect.height() - edgeY)
            zone = DockZone::Bottom;
        hit.zone = zone;
        hit.target = target;
        hit.preview = targetPreview(target, zone);
        return hit;
    }

    // Not over a dock: host-edge bands dock to the main window sides; the
    // central area means "keep floating" (never steal the central widget).
    constexpr int edge = 120;
    const QRect r = m_host->rect();
    DockZone zone = DockZone::None;
    if (hostPos.x() < edge)
        zone = DockZone::Left;
    else if (hostPos.x() > r.width() - edge)
        zone = DockZone::Right;
    else if (hostPos.y() < edge)
        zone = DockZone::Top;
    else if (hostPos.y() > r.height() - edge)
        zone = DockZone::Bottom;
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
