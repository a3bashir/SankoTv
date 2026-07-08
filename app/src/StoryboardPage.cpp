#include "StoryboardPage.h"

#include "DrawingCanvas.h"
#include "SankoDockOverlay.h"
#include "SankoSlider.h"
#include "StoryboardModel.h"

#include "DockAreaWidget.h"
#include "DockManager.h"
#include "DockWidget.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
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
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QKeySequence>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
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

// Off = idle grey on the pill; On = dark glyph on the amber active square.
QIcon toolIcon(const char *kind)
{
    QIcon icon;
    icon.addPixmap(toolIconPixmap(kind, QColor(0xc8, 0xc8, 0xc8)), QIcon::Normal, QIcon::Off);
    icon.addPixmap(toolIconPixmap(kind, QColor(0x16, 0x16, 0x16)), QIcon::Normal, QIcon::On);
    icon.addPixmap(toolIconPixmap(kind, QColor(0x55, 0x55, 0x55)), QIcon::Disabled, QIcon::Off);
    return icon;
}

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

} // namespace

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

    // Ctrl+Z reverts the last canvas change (drawing or image import).
    QShortcut *undoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), this);
    connect(undoShortcut, &QShortcut::activated, this, [this] {
        if (m_canvas)
            m_canvas->undo();
    });

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
    QWidget *stripBar = new QWidget;
    stripBar->setAttribute(Qt::WA_StyledBackground, true);
    stripBar->setFixedHeight(140);
    // 15% lighter than the old #0d0d0d (15% of the way toward white).
    stripBar->setStyleSheet(QStringLiteral(
        "background-color: #313131; border-bottom: 1px solid #1f1f1f;"));
    QHBoxLayout *stripBarLayout = new QHBoxLayout(stripBar);
    stripBarLayout->setContentsMargins(0, 0, 0, 0);
    stripBarLayout->setSpacing(0);

    stripBarLayout->addWidget(createPanelControls()); // pinned, non-scrolling

    QFrame *divider = new QFrame; // 0.5px vertical divider (1px is the renderable minimum)
    divider->setFrameShape(QFrame::VLine);
    divider->setFixedWidth(1);
    divider->setStyleSheet(QStringLiteral("background-color: #2a2a2a; border: none;"));
    stripBarLayout->addWidget(divider);

    QScrollArea *strip = new QScrollArea;
    strip->setWidgetResizable(true);
    strip->setFrameShape(QFrame::NoFrame);
    strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    strip->setStyleSheet(QStringLiteral("QScrollArea { background-color: #313131; border: none; }"));

    QWidget *stripContainer = new QWidget;
    stripContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_panelStripLayout = new QHBoxLayout(stripContainer);
    m_panelStripLayout->setContentsMargins(14, 12, 14, 12);
    m_panelStripLayout->setSpacing(12);
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
    connect(m_canvas, &DrawingCanvas::layersChanged, this, &StoryboardPage::rebuildLayerPanel);
    m_canvas->installEventFilter(this); // re-clamp the floating overlays on resize

    drawLayout->addWidget(m_canvas, 1);

    createFloatingToolbar(); // child of the canvas, raised above it
    createBrushSettings();   // floating over the canvas, shown with the Brush tool
    createCameraPanel();     // floating over the canvas, shown with the Camera tool
    createShapesPanel();     // floating over the canvas, shown with the Shapes tool

    layout->addWidget(drawRow, 1);

    return column;
}

// Floating pill toolbar layered over the canvas: fully-rounded #161616 pill,
// drop shadow, vertical stack of icon tools, colour swatch, brush-size
// slider, undo/redo, and a six-dot drag grip at the bottom. Child of
// m_canvas, so the canvas keeps receiving strokes everywhere the pill is not.
void StoryboardPage::createFloatingToolbar()
{
    m_floatToolbar = new QWidget(m_canvas);
    m_floatToolbar->setObjectName(QStringLiteral("floatToolbar"));
    m_floatToolbar->setAttribute(Qt::WA_StyledBackground, true);
    m_floatToolbar->setFixedWidth(56);
    m_floatToolbar->setStyleSheet(QStringLiteral(
        "QWidget#floatToolbar { background-color: #161616; border-radius: 28px; }"));

    auto *shadow = new QGraphicsDropShadowEffect(m_floatToolbar);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 150));
    m_floatToolbar->setGraphicsEffect(shadow);

    // Swallow every mouse event on the pill body so nothing falls through to
    // the canvas and draws (see eventFilter).
    m_floatToolbar->installEventFilter(this);
    m_floatEventBlockers.insert(m_floatToolbar);

    // Ten tool buttons share the pill now (selection tools + Move joined):
    // compact 28px buttons and tight spacing keep the pill inside the canvas
    // at the minimum window size; the size-slider run absorbs the rest.
    QVBoxLayout *layout = new QVBoxLayout(m_floatToolbar);
    layout->setContentsMargins(12, 10, 12, 6);
    layout->setSpacing(2);

    // Icon-only rounded-square buttons; the active one gets the amber square.
    auto pillButton = [](const char *kind, const QString &tip, bool checkable) {
        QPushButton *b = new QPushButton;
        b->setCheckable(checkable);
        b->setCursor(Qt::PointingHandCursor);
        b->setFixedSize(28, 28);
        b->setIcon(toolIcon(kind));
        b->setIconSize(QSize(16, 16));
        b->setToolTip(tip);
        b->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; border: none; border-radius: 8px; }"
            "QPushButton:hover { background-color: #2a2a2a; }"
            "QPushButton:checked { background-color: #f5a623; }"));
        return b;
    };

    // Tool selection (exclusive).
    QButtonGroup *tools = new QButtonGroup(this);
    tools->setExclusive(true);

    QPushButton *move = pillButton("move",
                                   QStringLiteral("Move \xE2\x80\x94 drag the selected pixels"), true);
    QPushButton *brushTool = pillButton("brush",
                                        QStringLiteral("Brush \xE2\x80\x94 pressure-sensitive; presets include Pen"), true);
    QPushButton *eraser = pillButton("erase", QStringLiteral("Eraser"), true);
    QPushButton *shapes = pillButton("shapes",
                                     QStringLiteral("Shapes \xE2\x80\x94 rectangle, triangle, circle, line, polygon"), true);
    QPushButton *fill = pillButton("fill", QStringLiteral("Flood fill"), true);
    QPushButton *selection = pillButton("selrect",
                                        QStringLiteral("Selection \xE2\x80\x94 click to use; hold or right-click"
                                                       " for Rectangle / Ellipse / Lasso"), true);
    QPushButton *camera = pillButton("camera",
                                     QStringLiteral("Camera overlays \xE2\x80\x94 frame and safe-area guides"), true);
    brushTool->setChecked(true); // Brush is the single drawing tool (default)

    tools->addButton(move);
    tools->addButton(brushTool);
    tools->addButton(eraser);
    tools->addButton(shapes);
    tools->addButton(fill);
    tools->addButton(selection);
    tools->addButton(camera);

    // ONE Selection button for the three modes: a plain click re-activates
    // the last-chosen mode; click-and-hold or right-click opens the mode
    // menu, and the button's icon mirrors the choice.
    QMenu *selectionMenu = new QMenu(m_floatToolbar);
    selectionMenu->setStyleSheet(QStringLiteral(
        "QMenu { background-color: #161616; color: #cccccc; border: 1px solid #2a2a2a;"
        " font-size: 11px; }"
        "QMenu::item { padding: 5px 18px 5px 8px; }"
        "QMenu::item:selected { background-color: #262626; color: #f5a623; }"));
    auto pickSelectionMode = [this, selection](DrawingCanvas::Tool mode, const char *iconKind) {
        m_selectionMode = mode;
        selection->setIcon(toolIcon(iconKind));
        selection->setChecked(true); // the exclusive group unchecks the old tool
        if (m_canvas)
            m_canvas->setTool(mode); // also covers "already checked, mode changed"
    };
    selectionMenu->addAction(toolIcon("selrect"), QStringLiteral("Rectangle"), this,
                             [pickSelectionMode] { pickSelectionMode(DrawingCanvas::SelectRect, "selrect"); });
    selectionMenu->addAction(toolIcon("selellipse"), QStringLiteral("Ellipse"), this,
                             [pickSelectionMode] { pickSelectionMode(DrawingCanvas::SelectEllipse, "selellipse"); });
    selectionMenu->addAction(toolIcon("lasso"), QStringLiteral("Lasso"), this,
                             [pickSelectionMode] { pickSelectionMode(DrawingCanvas::Lasso, "lasso"); });
    auto popupSelectionMenu = [selection, selectionMenu] {
        selectionMenu->popup(selection->mapToGlobal(QPoint(selection->width() + 6, 0)));
    };
    selection->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(selection, &QPushButton::customContextMenuRequested, this,
            [popupSelectionMenu](const QPoint &) { popupSelectionMenu(); });
    connect(selection, &QPushButton::pressed, this, [selection, popupSelectionMenu] {
        QTimer::singleShot(450, selection, [selection, popupSelectionMenu] {
            if (selection->isDown()) // still held: click-and-hold opens the menu
                popupSelectionMenu();
        });
    });

    // Toggled-driven tool selection: the checked state, the canvas tool, and
    // the brush panel's visibility can never disagree — whichever button the
    // exclusive group checks IS the active tool.
    auto bindTool = [this](QPushButton *button, DrawingCanvas::Tool tool) {
        connect(button, &QPushButton::toggled, this, [this, tool](bool on) {
            if (on && m_canvas)
                m_canvas->setTool(tool);
        });
    };
    bindTool(move, DrawingCanvas::Move);
    bindTool(brushTool, DrawingCanvas::Brush);
    bindTool(eraser, DrawingCanvas::Eraser);
    bindTool(shapes, DrawingCanvas::Shapes);
    bindTool(fill, DrawingCanvas::Fill);
    bindTool(camera, DrawingCanvas::Camera);
    // The Selection button activates whichever mode was chosen last.
    connect(selection, &QPushButton::toggled, this, [this](bool on) {
        if (on && m_canvas)
            m_canvas->setTool(m_selectionMode);
    });

    // Each tool's options panel is visible ONLY while that tool is active.
    // The exclusive group unchecks the old tool when a new one is picked,
    // which hides its panel.
    connect(brushTool, &QPushButton::toggled, this, [this](bool on) {
        if (m_brushPanel)
            m_brushPanel->setVisible(on);
    });
    connect(shapes, &QPushButton::toggled, this, [this](bool on) {
        if (m_shapesPanel)
            m_shapesPanel->setVisible(on);
    });
    connect(camera, &QPushButton::toggled, this, [this](bool on) {
        if (m_cameraPanel)
            m_cameraPanel->setVisible(on);
    });

    layout->addWidget(move, 0, Qt::AlignHCenter); // Move sits at the top
    layout->addWidget(brushTool, 0, Qt::AlignHCenter);
    layout->addWidget(eraser, 0, Qt::AlignHCenter);
    layout->addWidget(shapes, 0, Qt::AlignHCenter);
    layout->addWidget(fill, 0, Qt::AlignHCenter);
    layout->addWidget(selection, 0, Qt::AlignHCenter);
    layout->addWidget(camera, 0, Qt::AlignHCenter);

    // Onion skin toggle (independent of the exclusive tool group).
    m_onionButton = pillButton("onion",
                               QStringLiteral("Onion skin (O) \xE2\x80\x94 ghost of previous panel"), true);
    connect(m_onionButton, &QPushButton::toggled, this, [this](bool on) {
        m_canvas->setOnionSkinEnabled(on);
        updateOnionGhost();
    });
    layout->addWidget(m_onionButton, 0, Qt::AlignHCenter);

    // Color swatch.
    QPushButton *color = new QPushButton;
    color->setCursor(Qt::PointingHandCursor);
    color->setToolTip(QStringLiteral("Color"));
    color->setFixedSize(24, 24);
    color->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: #000000; border: 2px solid #3a3a3a; border-radius: 6px; }"));
    connect(color, &QPushButton::clicked, this, [this, color] {
        const QColor chosen = QColorDialog::getColor(Qt::black, this, QStringLiteral("Brush color"));
        if (chosen.isValid()) {
            m_canvas->setColor(chosen);
            color->setStyleSheet(QStringLiteral(
                "QPushButton { background-color: %1; border: 2px solid #3a3a3a; border-radius: 6px; }")
                .arg(chosen.name()));
        }
    });
    layout->addWidget(color, 0, Qt::AlignHCenter);

    // Vertical brush-size slider; range 1-200, value label painted below the
    // track. Always visible, even when the Brush panel is hidden. Fixed run
    // so the pill keeps a stable, compact height.
    m_brushSizeSlider = new SankoSlider;
    m_brushSizeSlider->setToolTip(QStringLiteral("Brush size"));
    m_brushSizeSlider->setOrientation(Qt::Vertical);
    m_brushSizeSlider->setTrackHeight(25); // track WIDTH when vertical
    m_brushSizeSlider->setHandleSize(27);
    m_brushSizeSlider->setRange(1, 200);
    m_brushSizeSlider->setValue(25);
    m_brushSizeSlider->setMinimumHeight(48);  // flexible run: the pill trims the
    m_brushSizeSlider->setMaximumHeight(185); // slider first on short canvases
                                              // (set after the setters above:
                                              // they reset these constraints)
    connect(m_brushSizeSlider, &SankoSlider::valueChanged, this, [this](int v) {
        m_canvas->setBrushToolSize(v);
        // The Eraser and Line widths follow too (clamped to 1-20 inside).
        m_canvas->setBrushSize(v);
    });
    layout->addWidget(m_brushSizeSlider, 0, Qt::AlignHCenter);

    // Undo / redo.
    QPushButton *undo = pillButton("undo", QStringLiteral("Undo (Ctrl+Z)"), false);
    connect(undo, &QPushButton::clicked, this, [this] { m_canvas->undo(); });
    layout->addWidget(undo, 0, Qt::AlignHCenter);

    // The canvas has no redo stack yet; the button ships disabled so the
    // toolbar layout is final. (Flagged in the task report.)
    QPushButton *redo = pillButton("redo", QStringLiteral("Redo \xE2\x80\x94 not available yet"), false);
    redo->setEnabled(false);
    layout->addWidget(redo, 0, Qt::AlignHCenter);

    // Six-dot drag grip: the only place a drag can start.
    QLabel *handle = new QLabel;
    handle->setPixmap(dragDotsPixmap());
    handle->setAlignment(Qt::AlignCenter);
    handle->setFixedHeight(20);
    handle->setCursor(Qt::OpenHandCursor);
    handle->setToolTip(QStringLiteral("Drag to move the toolbar"));
    handle->installEventFilter(this);
    m_floatDragSources.insert(handle, m_floatToolbar); // grip moves the pill
    layout->addWidget(handle);

    m_floatToolbar->adjustSize(); // fixed content -> final pill size now
    m_floatToolbar->raise();      // above the canvas (and its zoom buttons)
}

// Keep a floating overlay fully inside the visible canvas.
QPoint StoryboardPage::clampedFloatPos(const QWidget *panel, const QPoint &pos) const
{
    if (!panel || !m_canvas)
        return pos;
    const int maxX = qMax(0, m_canvas->width() - panel->width());
    const int maxY = qMax(0, m_canvas->height() - panel->height());
    return QPoint(qBound(0, pos.x(), maxX), qBound(0, pos.y(), maxY));
}

// First call restores the pill's persisted position (or a sensible default);
// later calls (canvas resizes) re-clamp every floating overlay.
void StoryboardPage::positionFloatingToolbar()
{
    if (!m_floatToolbar || !m_canvas)
        return;
    // Fixed content is 364px (8 tool buttons + swatch + undo/redo + grip +
    // gaps); the size-slider run flexes 48..185 so the grip always stays
    // reachable inside the canvas: floor 412, design height 549.
    m_floatToolbar->setFixedHeight(qBound(412, m_canvas->height() - 12, 549));
    if (!m_toolbarPosRestored) {
        m_toolbarPosRestored = true;
        const QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
        const QPoint fallback(16, qMax(0, (m_canvas->height() - m_floatToolbar->height()) / 2));
        const QPoint saved =
            settings.value(QStringLiteral("storyboard/toolbarPos"), fallback).toPoint();
        m_floatToolbar->move(clampedFloatPos(m_floatToolbar, saved));
    } else {
        m_floatToolbar->move(clampedFloatPos(m_floatToolbar, m_floatToolbar->pos()));
    }
    for (QWidget *panel : {m_brushPanel, m_cameraPanel, m_shapesPanel})
        if (panel)
            panel->move(clampedFloatPos(panel, panel->pos()));
}

// Floating overlay panel over the canvas, styled after the dock headers:
// dark title bar with ONLY a Close button, draggable by that title bar.
QWidget *StoryboardPage::createFloatingPanel(const QString &title, QWidget *body)
{
    QWidget *panel = new QWidget(m_canvas);
    panel->setObjectName(QStringLiteral("floatPanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setStyleSheet(QStringLiteral(
        "QWidget#floatPanel { background-color: #111111; border: 1px solid #2a2a2a; }"));

    auto *shadow = new QGraphicsDropShadowEffect(panel);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 150));
    panel->setGraphicsEffect(shadow);

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

    // Drag by the header; swallow stray clicks on both header and body so
    // nothing falls through to the canvas (see eventFilter).
    header->installEventFilter(this);
    panel->installEventFilter(this);
    m_floatDragSources.insert(header, panel);
    m_floatEventBlockers.insert(panel);
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
    m_brushPanel->move(clampedFloatPos(m_brushPanel, QPoint(84, 12))); // right of the pill
    m_brushPanel->setVisible(true); // Brush is the default tool; the pill's
                                    // toggled connection hides it otherwise
    m_brushPanel->raise();
    return m_brushPanel;
}

// Floating overlay shown only while the Camera tool is active. Hosts the
// display-only viewport overlay toggles.
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
    m_cameraPanel->move(clampedFloatPos(m_cameraPanel, QPoint(84, 12))); // right of the pill
    m_cameraPanel->setVisible(false); // Brush is the default tool
    m_cameraPanel->raise();
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
    m_shapesPanel->move(clampedFloatPos(m_shapesPanel, QPoint(84, 12))); // right of the pill
    m_shapesPanel->setVisible(false); // Brush is the default tool
    m_shapesPanel->raise();
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

            m_panelStripLayout->addWidget(thumb);
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
                ? QStringLiteral("QLabel#panelThumb { border: 2px solid #f5a623; border-radius: 4px; }")
                : QStringLiteral("QLabel#panelThumb { border: 1px solid #2a2a2a; border-radius: 4px;"
                                 " background-color: #161616; }"));
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

void StoryboardPage::addPanelToScene(int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= m_scenes.size())
        return;
    if (sceneIndex != m_currentScene)
        selectScene(sceneIndex);

    Scene *scene = m_scenes.at(sceneIndex);
    scene->panels.append(makePanel());
    rebuildPanelStrip();
    selectPanel(scene->panels.size() - 1);
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
    // Floating overlays (pill toolbar + Brush/Camera panels): reposition on
    // canvas resize; drag via their registered grip/header.
    if (object == m_canvas && event->type() == QEvent::Resize) {
        positionFloatingToolbar();
        return false;
    }
    // Grips/headers are QWidgets/QLabels that would IGNORE mouse events;
    // ignored events propagate to the parent — the canvas — and draw. Every
    // mouse AND tablet event on them is therefore CONSUMED here (return
    // true), whether or not it moved anything. Subsequent move/release
    // events still arrive: Qt targets them at the widget under the press.
    if (QWidget *dragTarget = m_floatDragSources.value(object)) {
        QWidget *source = static_cast<QWidget *>(object);
        const auto beginDrag = [this, dragTarget, source](const QPoint &globalPos) {
            m_floatDragPanel = dragTarget;
            m_floatDragStart = globalPos;
            m_floatStartPos = dragTarget->pos();
            source->setCursor(Qt::ClosedHandCursor);
        };
        const auto moveDrag = [this, dragTarget](const QPoint &globalPos) {
            if (m_floatDragPanel == dragTarget)
                dragTarget->move(clampedFloatPos(dragTarget,
                                                 m_floatStartPos + (globalPos - m_floatDragStart)));
        };
        const auto endDrag = [this, dragTarget, source] {
            if (m_floatDragPanel != dragTarget)
                return;
            m_floatDragPanel = nullptr;
            source->setCursor(Qt::OpenHandCursor);
            if (dragTarget == m_floatToolbar) // only the pill persists its spot
                QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"))
                    .setValue(QStringLiteral("storyboard/toolbarPos"), dragTarget->pos());
        };
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton)
                beginDrag(me->globalPosition().toPoint());
            return true;
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->buttons() & Qt::LeftButton)
                moveDrag(me->globalPosition().toPoint());
            return true;
        }
        case QEvent::MouseButtonRelease: {
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
                endDrag();
            return true;
        }
        case QEvent::MouseButtonDblClick:
            return true;
        case QEvent::TabletPress:
            beginDrag(static_cast<QTabletEvent *>(event)->globalPosition().toPoint());
            event->accept();
            return true;
        case QEvent::TabletMove:
            moveDrag(static_cast<QTabletEvent *>(event)->globalPosition().toPoint());
            event->accept();
            return true;
        case QEvent::TabletRelease:
            endDrag();
            event->accept();
            return true;
        default:
            break;
        }
        return false;
    }
    // Overlay bodies (margins/gaps between controls): swallow mouse AND
    // tablet events so they never reach the canvas underneath. Child
    // buttons/sliders get their events first and are unaffected.
    if (m_floatEventBlockers.contains(object)) {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
            return true;
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
            event->accept(); // stop mouse-event synthesis as well
            return true;
        default:
            return false;
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

    Panel *selected = currentPanel(); // keep selection by pointer
    Panel *moved = scene->panels.takeAt(from);
    const int dest = (target > from) ? target - 1 : target;
    scene->panels.insert(dest, moved);

    rebuildPanelStrip();
    int newSel = scene->panels.indexOf(selected);
    if (newSel < 0)
        newSel = scene->panels.indexOf(moved);
    selectPanel(newSel >= 0 ? newSel : 0);
}

void StoryboardPage::movePanelBy(int delta)
{
    Scene *scene = currentScene();
    if (!scene || m_currentPanel < 0)
        return;
    const int dst = m_currentPanel + delta;
    if (dst < 0 || dst >= scene->panels.size())
        return; // clamp at boundaries, no wrap-around
    scene->panels.move(m_currentPanel, dst);
    rebuildPanelStrip();
    selectPanel(dst);
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
void StoryboardPage::insertPanelClone(const Panel *panel, int insertAt)
{
    Scene *scene = currentScene();
    if (!scene || !panel)
        return;
    insertAt = qBound(0, insertAt, int(scene->panels.size()));
    scene->panels.insert(insertAt, clonePanel(panel));

    rebuildPanelStrip();
    selectPanel(insertAt);
    if (m_panelScroll && insertAt < m_panelThumbs.size())
        m_panelScroll->ensureWidgetVisible(m_panelThumbs.at(insertAt));
}

void StoryboardPage::duplicatePanel()
{
    Panel *source = currentPanel();
    if (!source || m_currentPanel < 0)
        return;
    insertPanelClone(source, m_currentPanel + 1);
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
    // No confirmation (unlike Delete): the clipboard copy is the safety net.
    const int removeAt = m_currentPanel;
    delete scene->panels.at(removeAt); // Scene owns its Panel objects
    scene->panels.removeAt(removeAt);
    rebuildPanelStrip();
    selectPanel(qBound(0, removeAt, scene->panels.size() - 1)); // nearest remaining
}

void StoryboardPage::pastePanelAfterSelected()
{
    Scene *scene = currentScene();
    if (!scene || !m_panelClipboard)
        return;
    const int insertAt = (m_currentPanel >= 0 && m_currentPanel < scene->panels.size())
        ? m_currentPanel + 1
        : scene->panels.size();
    insertPanelClone(m_panelClipboard, insertAt);
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
    insertPanelClone(m_panelClipboard, m_clipboardPanelIndex);
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
    QWidget *column = new QWidget;
    column->setAttribute(Qt::WA_StyledBackground, true);
    column->setFixedWidth(56);
    column->setStyleSheet(QStringLiteral("background-color: #313131;")); // matches the strip

    QVBoxLayout *layout = new QVBoxLayout(column);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(4); // four buttons now share the strip-bar height

    // Add — purple fill, white icon.
    m_addPanelButton = makeCtrlButton(
        CtrlIcon::Add, QColor(0xff, 0xff, 0xff),
        QStringLiteral("<b>Add Panel</b> | Creates a new storyboard panel."),
        QStringLiteral(
            "QPushButton { background-color: #7c6ef6; border: none; border-radius: 8px; }"
            "QPushButton:hover { background-color: #8f82f8; }"
            "QPushButton:disabled { background-color: #3a3550; }"));
    connect(m_addPanelButton, &QPushButton::clicked, this, [this] { addPanelAfterSelected(); });
    layout->addWidget(m_addPanelButton, 0, Qt::AlignHCenter);

    // Duplicate — transparent, light grey icon, grey border.
    m_dupPanelButton = makeCtrlButton(
        CtrlIcon::Duplicate, QColor(0xcc, 0xcc, 0xcc),
        QStringLiteral("<b>Duplicate Panel</b> | Copies the selected panel."),
        QStringLiteral(
            "QPushButton { background-color: transparent; border: 1px solid #3a3a3a; border-radius: 8px; }"
            "QPushButton:hover { border-color: #5a5a5a; background-color: #161616; }"
            "QPushButton:disabled { border-color: #242424; }"));
    connect(m_dupPanelButton, &QPushButton::clicked, this, [this] { duplicatePanel(); });
    layout->addWidget(m_dupPanelButton, 0, Qt::AlignHCenter);

    // Clear — wipes the current drawing (destructive: confirms first).
    m_clearPanelButton = makeCtrlButton(
        CtrlIcon::Clear, QColor(0xcc, 0xcc, 0xcc),
        QStringLiteral("<b>Clear Drawing</b> | Clears the selected panel's drawing. Asks first."),
        QStringLiteral(
            "QPushButton { background-color: transparent; border: 1px solid #3a3a3a; border-radius: 8px; }"
            "QPushButton:hover { border-color: #5a5a5a; background-color: #161616; }"
            "QPushButton:disabled { border-color: #242424; }"));
    connect(m_clearPanelButton, &QPushButton::clicked, this, [this] {
        if (!currentPanel())
            return;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Clear this drawing?"),
            QStringLiteral("Clear this drawing?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes)
            m_canvas->clearCanvas();
    });
    layout->addWidget(m_clearPanelButton, 0, Qt::AlignHCenter);

    // Delete — transparent, red icon, dark-red border.
    m_deletePanelButton = makeCtrlButton(
        CtrlIcon::Delete, QColor(0xe0, 0x65, 0x5f),
        QStringLiteral("<b>Delete Panel</b> | Removes the selected panel. Asks first."),
        QStringLiteral(
            "QPushButton { background-color: transparent; border: 1px solid #5a2a2a; border-radius: 8px; }"
            "QPushButton:hover { border-color: #e0655f; background-color: #1a0f0f; }"
            "QPushButton:disabled { border-color: #2a1a1a; }"));
    connect(m_deletePanelButton, &QPushButton::clicked, this, [this] { deleteSelectedPanel(); });
    layout->addWidget(m_deletePanelButton, 0, Qt::AlignHCenter);

    // Light Table — checkable toggle (styling is a later restyle pass). Shows
    // ghosts of the neighbouring panels behind the current drawing.
    m_lightTableButton = makeCtrlButton(
        CtrlIcon::LightTable, QColor(0xcc, 0xcc, 0xcc),
        QStringLiteral("<b>Light Table</b> | Ghost neighbouring panels behind the current "
                       "one (previous red, next green)."),
        QStringLiteral(
            "QPushButton { background-color: transparent; border: 1px solid #3a3a3a; border-radius: 8px; }"
            "QPushButton:hover { border-color: #5a5a5a; background-color: #161616; }"
            "QPushButton:checked { background-color: #f5a623; border: none; }"));
    m_lightTableButton->setCheckable(true);
    connect(m_lightTableButton, &QPushButton::toggled, this, [this](bool on) {
        if (m_canvas)
            m_canvas->setLightTableEnabled(on);
        updateLightTable();
    });
    layout->addWidget(m_lightTableButton, 0, Qt::AlignHCenter);

    layout->addStretch(1); // buttons pinned to the top
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
    scene->panels.insert(insertAt, makePanel());
    rebuildPanelStrip();
    selectPanel(insertAt);
    if (m_panelScroll && insertAt < m_panelThumbs.size())
        m_panelScroll->ensureWidgetVisible(m_panelThumbs.at(insertAt));
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
        this, QStringLiteral("Delete this panel?"), QStringLiteral("This cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    const int removeAt = m_currentPanel;
    delete scene->panels.at(removeAt); // Scene owns its Panel objects; free the removed one
    scene->panels.removeAt(removeAt);

    rebuildPanelStrip();
    selectPanel(qBound(0, removeAt, scene->panels.size() - 1)); // nearest remaining panel
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
