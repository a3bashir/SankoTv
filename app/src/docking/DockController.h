#pragma once

#include "DockOverlay.h"

#include <QObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include <functional>

class QDockWidget;
class QMainWindow;
class QMenu;

// Reusable docking controller for a host QMainWindow, built on native
// QDockWidget (no third-party docking library). Owns the drop overlay and
// implements every behaviour the DockTitleBar delegates: drag hit-testing and
// previews, tab/split drops (including REAL splits beside tabbed groups),
// multi-monitor-safe floating, animated collapse/expand that survives
// moving/floating/redocking, a Panels menu, and versioned layout persistence
// with per-panel visibility/collapse/active-tab/floating-geometry keys and a
// safe fallback to the application-provided default layout.
//
// The application layer stays in charge of WHAT is docked: it registers its
// existing panel widgets through addPanel() (they are wrapped, never
// recreated) and provides the default layout as a callback.
class DockController : public QObject
{
    Q_OBJECT

public:
    // The collapsed panel height == the title bar height (kept in sync with
    // DockTitleBar's fixed height).
    static constexpr int kTitleBarHeight = 30;

    // settingsOrg/App name the QSettings store (the application's own values,
    // never demo defaults); settingsGroup namespaces every key this
    // controller writes (e.g. "storyboard/nativeDock").
    DockController(QMainWindow *host, const QString &settingsOrg,
                   const QString &settingsApp, const QString &settingsGroup,
                   QObject *parent = nullptr);

    // Wraps an EXISTING content widget in a QDockWidget with a DockTitleBar.
    // The content keeps its behaviour, signals and models; objectName must be
    // unique and stable (QMainWindow::saveState() keys layouts on it).
    QDockWidget *addPanel(QWidget *content, const QString &title,
                          const QString &objectName);
    const QVector<QDockWidget *> &panels() const { return m_panels; }

    // The application's pristine default placement (addDockWidget /
    // splitDockWidget / tabifyDockWidget calls). Used by resetLayout() and as
    // the fallback whenever a saved layout cannot be restored.
    void setDefaultLayout(std::function<void()> defaultLayout);

    // --- layout persistence --------------------------------------------------
    void saveLayout();
    bool restoreLayout(); // false: nothing saved / rejected -> caller default
    void resetLayout();   // expand + show everything, reapply default layout
    void clearSavedLayout();

    // --- menu integration ----------------------------------------------------
    // Fills a "Panels" menu with the per-dock toggle actions plus
    // Show All Panels / Hide All Panels.
    void populatePanelsMenu(QMenu *menu);
    void showAllPanels();
    void hideAllPanels();

    // --- drag support (called by DockTitleBar) -------------------------------
    void updateDockPreview(QDockWidget *dragged, const QPoint &globalPos);
    void finishDock(QDockWidget *dragged, const QPoint &globalPos);
    // Clamp a floating position so a usable strip of the panel stays on
    // whichever screen the cursor is on (multi-monitor safe).
    QPoint clampedFloatingPosition(QDockWidget *dock,
                                   const QPoint &wanted) const;

    // --- collapse ------------------------------------------------------------
    void toggleDockCollapse(QDockWidget *dock);
    // animate=false is the restore path (no visible transition on startup).
    void setDockCollapsed(QDockWidget *dock, bool collapsed, bool animate);
    bool isDockCollapsed(const QDockWidget *dock) const;

    QMainWindow *host() const { return m_host; }

private:
    // A drop on a target's edge must create a REAL split even when the
    // target sits inside a tabbed group. Qt documents that splitDockWidget()
    // degrades to "add as another tab" for tabified docks, so this helper
    // dissolves the group, splits beside the lone target, rebuilds the tabs
    // and restores the previously active one.
    void splitBesideDockGroup(QDockWidget *dragged, QDockWidget *target,
                              DockZone zone);

    DockHit detectDockHit(QDockWidget *dragged, const QPoint &globalPos) const;
    QDockWidget *dockAt(QDockWidget *dragged, const QPoint &hostPos) const;
    QRect hostPreview(DockZone zone) const;   // host-edge zones
    QRect targetPreview(QDockWidget *target, DockZone zone) const;
    QDockWidget *activeTabOf(QDockWidget *member) const;

    QMainWindow *m_host = nullptr;
    DockOverlay *m_overlay = nullptr;
    QVector<QDockWidget *> m_panels;
    std::function<void()> m_defaultLayout;
    QString m_settingsOrg;
    QString m_settingsApp;
    QString m_settingsGroup;
};
