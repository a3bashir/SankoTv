#include "MainWindow.h"
#include "SankoTheme.h"

#ifdef SANKOTV_DEV_RECORDER
#include "devrecorder/DevRecorder.h" // TEMP dev tool; see that header to remove
#include "DrawingCanvas.h"
#include "FloatingToolWindow.h"
#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#endif

#include "AnimaticPage.h"
#include "ConsistencyBoard.h"
#include "DashboardPage.h"
#include "NewProjectDialog.h"
#include "ProjectResize.h"
#include "ProjectSettingsDialog.h"
#include "ResizeProjectDialog.h"
#include "ProjectIO.h"
#include "GenerationPage.h"
#include "ScriptEditorPage.h"
#include "StoryboardModel.h"
#include "StoryboardPage.h"
#include "SankoSlider.h"

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStackedWidget>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>
#include <Qt>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setObjectName(QStringLiteral("SankoTVMainWindow")); // stable saveState key
    setMinimumSize(1280, 720);

    setupMenuBar();

    m_stack = new QStackedWidget(this);
    m_dashboard = new DashboardPage;
    m_scriptEditor = new ScriptEditorPage;
    m_storyboard = new StoryboardPage;
    m_animatic = new AnimaticPage;
    m_consistencyBoard = new ConsistencyBoard;
    m_generation = new GenerationPage;

    // ONE app-wide chronological undo history: drawing, selection, panel,
    // and transform actions all funnel into this shared stack.
    m_undoStack = new QUndoStack(this);
    m_undoStack->setUndoLimit(60);
    m_storyboard->setUndoStack(m_undoStack);

    // UNSAVED-CHANGES BACKSTOP. Every undoable change marks the project
    // dirty BY CONSTRUCTION rather than by someone remembering to wire it:
    // strokes, fills, erases, pastes, transforms, layer stack edits, panel
    // add/remove/move and perspective edits are all undo commands, and so
    // is any command type added in future. Enumerating them by hand would
    // age badly; the risk being guarded against is a change type nobody
    // wired.
    //
    // One command is excluded. Changing what is SELECTED does not change
    // the document, so a marquee drag must not claim there is work to save.
    // The command identifies itself (SelectionCommand::id()) rather than
    // being matched by its text.
    connect(m_undoStack, &QUndoStack::indexChanged, this, [this](int index) {
        if (index <= 0)
            return; // undone back to the start: nothing was just done
        const QUndoCommand *command = m_undoStack->command(index - 1);
        if (command && command->id() == 0x5E1EC7)
            return; // selection: not a document change
        markDirty();
    });

    m_storyboard->setConsistencyEntries(&m_consistencyEntries); // read-only
    m_consistencyBoard->setEntries(&m_consistencyEntries);      // read-write
    m_generation->setConsistencyEntries(&m_consistencyEntries); // read-only

    // Paste / Paste in Place enable once a panel lands on the clipboard.
    // The changes the undo stack cannot see, because they are not undoable:
    // Shot Info fields, panel durations and the scratch audio track, and the
    // consistency entries. Each page says so itself rather than the window
    // guessing from repaints.
    connect(m_storyboard, &StoryboardPage::documentChanged, this,
            &MainWindow::markDirty);
    connect(m_animatic, &AnimaticPage::documentChanged, this,
            &MainWindow::markDirty);
    if (m_consistencyBoard)
        connect(m_consistencyBoard, &ConsistencyBoard::documentChanged, this,
                &MainWindow::markDirty);
    // Pixel edits also arrive here directly. Redundant with the undo-stack
    // backstop (they are undoable), and kept for exactly that reason: a
    // pixel change that somehow never reached the stack still counts.
    if (auto *canvas = m_storyboard->findChild<DrawingCanvas *>()) {
        connect(canvas, &DrawingCanvas::contentChanged, this,
                &MainWindow::markDirty);
        connect(canvas, &DrawingCanvas::layersChanged, this,
                &MainWindow::markDirty);
    }

    connect(m_storyboard, &StoryboardPage::panelClipboardChanged, this, [this](bool available) {
        if (m_pastePanelAct)
            m_pastePanelAct->setEnabled(available);
        if (m_pastePanelInPlaceAct)
            m_pastePanelInPlaceAct->setEnabled(available);
    });

    m_stack->addWidget(m_dashboard);        // index 0
    m_stack->addWidget(m_scriptEditor);     // index 1
    m_stack->addWidget(m_storyboard);       // index 2
    m_stack->addWidget(m_animatic);         // index 3
    m_stack->addWidget(m_consistencyBoard); // index 4
    m_stack->addWidget(m_generation);       // index 5

    // Dashboard -> New Project window (Figma 350:24) -> Script Editor.
    // Create writes <Location>/<Name>/<Name>.sankotv IMMEDIATELY and lands
    // in recents; Open routes into the existing loadFromPath. File > Save /
    // Save As / Open are untouched.
    connect(m_dashboard, &DashboardPage::newProjectRequested, this, [this] {
        NewProjectDialog dialog(this);
        if (dialog.exec() != QDialog::Accepted)
            return;
        if (dialog.mode() == NewProjectDialog::Mode::OpenExisting) {
            loadFromPath(dialog.openPath());
            return;
        }
        onNewProject();
        m_projectName = dialog.projectName();
        m_currentProjectPath = dialog.projectFilePath();
        m_projectFps = dialog.fps();
        m_canvasWidth = dialog.canvasWidth();   // APPLIED: the project's
        m_canvasHeight = dialog.canvasHeight(); // real canvas resolution
        m_storyboard->setProjectCanvasSize(
            QSize(m_canvasWidth, m_canvasHeight));
        m_animatic->setFps(m_projectFps);
        updateSaveActions();
        updateTitle();
        m_stack->setCurrentWidget(m_scriptEditor);
    });

    // Script Editor: parse materializes scenes; Continue navigates.
    connect(m_scriptEditor, &ScriptEditorPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_dashboard);
    });
    connect(m_scriptEditor, &ScriptEditorPage::scenesReady, this,
            [this](const QJsonArray &scenes) {
        buildScenesFromJson(scenes);
        updateSaveActions();
    });
    connect(m_scriptEditor, &ScriptEditorPage::continueRequested, this, [this] {
        if (m_scenes.isEmpty())
            return;
        m_storyboard->loadScenes(m_scenes);
        m_stack->setCurrentWidget(m_storyboard);
    });

    // Storyboard <-> Animatic.
    connect(m_storyboard, &StoryboardPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_scriptEditor);
    });
    connect(m_storyboard, &StoryboardPage::continueToAnimaticRequested, this,
            [this](const QVector<Scene *> &scenes) {
        m_animatic->loadScenes(scenes);
        m_stack->setCurrentWidget(m_animatic);
    });
    connect(m_animatic, &AnimaticPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_storyboard);
    });

    // Animatic -> Generation (AI video clips via fal.ai).
    connect(m_animatic, &AnimaticPage::generationRequested, this, [this] {
        const QString dir = m_currentProjectPath.isEmpty()
            ? QDir::tempPath() + QStringLiteral("/sankotv_generated")
            : QFileInfo(m_currentProjectPath).absolutePath();
        QDir().mkpath(dir);
        m_generation->setProjectDir(dir);
        m_generation->loadScenes(m_scenes);
        m_stack->setCurrentWidget(m_generation);
    });
    connect(m_generation, &GenerationPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_animatic);
    });

    // Storyboard <-> Consistency Board.
    connect(m_storyboard, &StoryboardPage::settingsRequested, this,
            [this] { onPreferences(); });
    connect(m_storyboard, &StoryboardPage::consistencyBoardRequested, this, [this] {
        m_consistencyBoard->refresh();
        m_stack->setCurrentWidget(m_consistencyBoard);
    });
    connect(m_consistencyBoard, &ConsistencyBoard::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_storyboard);
    });

    setCentralWidget(m_stack);

    updateSaveActions();
    updateTitle();


#ifdef SANKOTV_DEV_RECORDER
    // TEMP developer recorder — remove with app/src/devrecorder/ and the
    // CMake block (option SANKOTV_DEV_RECORDER). Nothing else depends on it.
    {
        auto *rec = devrec::Recorder::instance();
        rec->initialize(this,
                        {QStringLiteral("SankoTV"),
                         QCoreApplication::applicationVersion(),
#ifdef QT_NO_DEBUG
                         QStringLiteral("Release"),
#else
                         QStringLiteral("Debug"),
#endif
#ifdef SANKOTV_GIT_HEAD
                         // Captured at BUILD time, and marked "+dirty" when
                         // built from uncommitted work — a build from a
                         // dirty tree is not any commit, and a bare hash
                         // cannot say so.
                         QStringLiteral(SANKOTV_GIT_HEAD),
#else
                         QString(),
#endif
                         // When the binary was built, which is what tells
                         // you whether it predates the work you are looking
                         // at. The hash alone never can.
                         QStringLiteral(__DATE__ " " __TIME__)});
        rec->setStateProvider([this]() -> QVariantMap {
            auto *canvas = m_storyboard
                ? m_storyboard->findChild<DrawingCanvas *>() : nullptr;
            if (!canvas)
                return {};
            const ::Brush &b = canvas->paintBrush();
            QVariantMap m{{QStringLiteral("tool"), int(canvas->tool())},
                          {QStringLiteral("brushSize"), b.size()},
                          {QStringLiteral("brushOpacity"), b.opacity()},
                          {QStringLiteral("brushHardness"), b.hardness()},
                          {QStringLiteral("color"), b.color().name()},
                          {QStringLiteral("zoom"), canvas->viewZoom()},
                          {QStringLiteral("rotation"),
                           canvas->viewRotation()}};
            // Grab Control instrumentation: every floating toolbar's pill
            // state lands in each snapshot, so a stuck pill is visible in
            // the LOG even without pixels.
            for (QWidget *tl : QApplication::topLevelWidgets())
                if (auto *bar = qobject_cast<FloatingToolWindow *>(tl))
                    m.insert(QStringLiteral("bar.")
                                 + (bar->objectName().isEmpty()
                                        ? QString::fromLatin1(
                                              bar->metaObject()->className())
                                        : bar->objectName()),
                             QStringLiteral("%1,%2 vis=%3 pill=%4")
                                 .arg(bar->x())
                                 .arg(bar->y())
                                 .arg(int(bar->isVisible()))
                                 .arg(0));
            return m;
        });
        rec->setCameraProvider([this]() -> QVariantMap {
            auto *canvas = m_storyboard
                ? m_storyboard->findChild<DrawingCanvas *>() : nullptr;
            return canvas ? canvas->devCameraState() : QVariantMap();
        });
        rec->setPressClassifier(
            [](QWidget *w, const QPoint &at) -> QVariantMap {
                auto *bar = qobject_cast<FloatingToolWindow *>(w);
                if (!bar)
                    return {};
                QWidget *child = bar->childAt(at);
                // Managed bars use the base helper; the unmanaged Size CTL
                // bar drags from background too, via the same rule.
                const bool background = bar->isManaged()
                    ? bar->isDragBackgroundAt(at)
                    : (bar->rect().contains(at) && !child);
                return {{QStringLiteral("grabAccepted"), background},
                        {QStringLiteral("hitRegion"),
                         background ? QStringLiteral("background")
                                    : QStringLiteral("child")},
                        {QStringLiteral("consumedBy"),
                         child ? QString::fromLatin1(
                                     child->metaObject()->className())
                               : QStringLiteral("toolbar")},
                        {QStringLiteral("dragThresholdPx"),
                         QApplication::startDragDistance()}};
            });
        QMenu *dev = menuBar()->addMenu(QStringLiteral("Developer"));
        dev->addAction(rec->toggleAction());
        dev->addAction(rec->markAction());
        menuBar()->setCornerWidget(rec->indicatorWidget(),
                                   Qt::TopRightCorner);
        // Autostart (env-guarded): record from process start — startup
        // sequencing (camera writes, dock restores) happens before any
        // hotkey can arm the recorder. SANKOTV_DEVREC_AUTOSTART=<ms>
        // optionally stops and quits after that many milliseconds, so a
        // scripted startup-trail capture ends with a clean session summary.
        if (qEnvironmentVariableIsSet("SANKOTV_DEVREC_AUTOSTART")) {
            rec->startRecording();
            bool ok = false;
            const int ms =
                qEnvironmentVariable("SANKOTV_DEVREC_AUTOSTART").toInt(&ok);
            if (ok && ms > 0)
                QTimer::singleShot(ms, this, [rec] {
                    rec->stopRecording();
                    QCoreApplication::quit();
                });
        }
    }
#endif
}

MainWindow::~MainWindow()
{
    freeScenes();
}

// --- Menu -----------------------------------------------------------------

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("File"));

    // New Project reaches the same dialog the Dashboard offers, so there is
    // one way to create a project rather than two that must agree.
    QAction *newAct = fileMenu->addAction(QStringLiteral("New Project..."));
    newAct->setShortcut(QKeySequence::New);
    connect(newAct, &QAction::triggered, this, [this] {
        if (!confirmDiscardChanges(QStringLiteral("starting a new project")))
            return;
        emit m_dashboard->newProjectRequested();
    });

    QAction *openAct = fileMenu->addAction(QStringLiteral("Open Project..."));
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::onOpenProject);

    m_recentMenu = fileMenu->addMenu(QStringLiteral("Open Recent"));
    // Rebuilt each time it opens: the list changes as projects are opened
    // and saved, including from the New Project window.
    connect(m_recentMenu, &QMenu::aboutToShow, this,
            &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();

    m_closeProjectAct = fileMenu->addAction(QStringLiteral("Close Project"));
    m_closeProjectAct->setShortcut(QKeySequence::Close); // Ctrl+W
    connect(m_closeProjectAct, &QAction::triggered, this,
            &MainWindow::onCloseProject);

    fileMenu->addSeparator();

    m_saveAct = fileMenu->addAction(QStringLiteral("Save Project"));
    m_saveAct->setShortcut(QKeySequence::Save);
    connect(m_saveAct, &QAction::triggered, this, &MainWindow::onSaveProject);

    m_saveAsAct = fileMenu->addAction(QStringLiteral("Save Project As..."));
    m_saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAct, &QAction::triggered, this, &MainWindow::onSaveProjectAs);

    fileMenu->addSeparator();
    // Project Settings lives under File, NOT under Edit > Preferences:
    // Preferences are application-wide; these belong to the open project.
    m_projectSettingsAct = fileMenu->addAction(QStringLiteral("Project Settings..."));
    connect(m_projectSettingsAct, &QAction::triggered, this,
            &MainWindow::onProjectSettings);

    fileMenu->addSeparator();

    // Exit does NOTHING but close the window, so the unsaved-changes prompt
    // has exactly one implementation (closeEvent) and the menu cannot drift
    // from what the X button does.
    //
    // Alt+F4 is shown as TEXT and deliberately not bound: Windows already
    // delivers it as a close request, and binding it would put a second
    // close path in front of the one that must stay correct.
    QAction *exitAct = fileMenu->addAction(QStringLiteral("Exit"));
    exitAct->setShortcutVisibleInContextMenu(false);
    exitAct->setText(QStringLiteral("Exit\tAlt+F4"));
    connect(exitAct, &QAction::triggered, this, [this] { close(); });

    QMenu *editMenu = menuBar()->addMenu(QStringLiteral("Edit"));

    // Clipboard actions (Storyboard page only). They act on the CANVAS
    // selection when one exists, falling back to the panel clipboard —
    // StoryboardPage::editCopy/editCut/editPaste route it. Focused text
    // fields keep their native Ctrl+C/X/V: Qt's ShortcutOverride lets
    // editors claim the keys before these window-level actions fire.
    auto editAction = [this, editMenu](const QString &text, const QKeySequence &shortcut,
                                       void (StoryboardPage::*slot)()) {
        QAction *action = editMenu->addAction(text);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, [this, slot] {
            if (m_stack && m_stack->currentWidget() == m_storyboard && m_storyboard)
                (m_storyboard->*slot)();
        });
        return action;
    };
    // App-wide undo/redo (Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z): ONE chronological
    // history shared with the Brush-bar Undo/Redo buttons — drawing,
    // selection, panel, and transform actions in strict order.
    editAction(QStringLiteral("Undo"), QKeySequence(Qt::CTRL | Qt::Key_Z),
               &StoryboardPage::editUndo);
    QAction *redoAct = editAction(QStringLiteral("Redo"),
                                  QKeySequence(Qt::CTRL | Qt::Key_Y),
                                  &StoryboardPage::editRedo);
    redoAct->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y),
                           QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    editMenu->addSeparator();

    editAction(QStringLiteral("Copy"), QKeySequence::Copy, &StoryboardPage::editCopy);
    editAction(QStringLiteral("Cut"), QKeySequence::Cut, &StoryboardPage::editCut);
    m_pastePanelAct = editAction(QStringLiteral("Paste"), QKeySequence::Paste,
                                 &StoryboardPage::editPaste);
    m_pastePanelInPlaceAct = editAction(QStringLiteral("Paste in Place"),
                                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V),
                                        &StoryboardPage::editPasteInPlace);
    m_pastePanelAct->setEnabled(false);        // until something is copied
    m_pastePanelInPlaceAct->setEnabled(false); // (wired after m_storyboard exists)


    editMenu->addSeparator();
    QAction *prefsAct = editMenu->addAction(QStringLiteral("Preferences..."));
    prefsAct->setShortcut(QKeySequence::Preferences);
    connect(prefsAct, &QAction::triggered, this, &MainWindow::onPreferences);

    menuBar()->addMenu(QStringLiteral("View"));
}

// Modal Preferences dialog: category list on the left, settings pane on the
// right. Currently one category (Camera) with the safe-area guide opacities;
// values persist in QSettings and push into the canvas live while dragging.
void MainWindow::onPreferences()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Preferences"));
    dialog.setFixedSize(520, 300);
    dialog.setStyleSheet(SankoTheme::themed("QDialog { background-color: #161616; }"
        "QLabel { color: #cccccc; font-size: 11px; }"
        "QListWidget { background-color: #111111; color: #cccccc; font-size: 12px;"
        " border: none; border-right: 1px solid #1f1f1f; outline: none; }"
        "QListWidget::item { padding: 8px 12px; }"
        "QListWidget::item:selected { background-color: #262626; color: %ACCENT%; }"
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 4px; font-size: 11px; padding: 6px 16px; }"
        "QPushButton:hover { background-color: #262626; }"));

    QHBoxLayout *split = new QHBoxLayout(&dialog);
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);

    QListWidget *categories = new QListWidget;
    categories->setFixedWidth(130);
    categories->addItem(QStringLiteral("Camera"));
    categories->setCurrentRow(0);
    split->addWidget(categories);

    QStackedWidget *pages = new QStackedWidget;
    split->addWidget(pages, 1);
    connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

    // --- Camera page -------------------------------------------------------
    QWidget *cameraPage = new QWidget;
    QVBoxLayout *cameraLayout = new QVBoxLayout(cameraPage);
    cameraLayout->setContentsMargins(16, 16, 16, 12);
    cameraLayout->setSpacing(6);

    QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));

    // Each row: caption + SankoSlider (0-100, "%"), seeded from QSettings.
    // Changes persist immediately and update the canvas overlays live.
    auto addOpacityRow = [&](const QString &caption, const QString &key,
                             void (StoryboardPage::*apply)(int)) {
        QLabel *label = new QLabel(caption);
        cameraLayout->addWidget(label);
        SankoSlider *slider = new SankoSlider;
        slider->setRange(0, 100);
        slider->setValueSuffix(QStringLiteral("%"));
        slider->setValue(qBound(0, settings.value(key, 50).toInt(), 100));
        connect(slider, &SankoSlider::valueChanged, this, [this, key, apply](int v) {
            QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV")).setValue(key, v);
            if (m_storyboard)
                (m_storyboard->*apply)(v);
        });
        cameraLayout->addWidget(slider);
    };
    addOpacityRow(QStringLiteral("Action Safe Area Opacity"),
                  QStringLiteral("camera/actionSafeOpacity"),
                  &StoryboardPage::setActionSafeMaskOpacity);
    addOpacityRow(QStringLiteral("Title Safe Area Opacity"),
                  QStringLiteral("camera/titleSafeOpacity"),
                  &StoryboardPage::setTitleSafeMaskOpacity);

    cameraLayout->addStretch(1);

    QHBoxLayout *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    QPushButton *closeButton = new QPushButton(QStringLiteral("Close"));
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttonRow->addWidget(closeButton);
    cameraLayout->addLayout(buttonRow);

    pages->addWidget(cameraPage);

    dialog.exec();
}

void MainWindow::updateSaveActions()
{
    const bool hasScenes = !m_scenes.isEmpty();
    if (m_saveAct)
        m_saveAct->setEnabled(hasScenes);
    if (m_saveAsAct)
        m_saveAsAct->setEnabled(hasScenes);
    // A project is "open" once it has a file (New Project creates one
    // immediately) or scenes; Project Settings has nothing to edit before.
    if (m_projectSettingsAct)
        m_projectSettingsAct->setEnabled(hasScenes
                                         || !m_currentProjectPath.isEmpty());
    // Close Project follows the same definition of "a project is open".
    if (m_closeProjectAct)
        m_closeProjectAct->setEnabled(hasScenes
                                      || !m_currentProjectPath.isEmpty());
}

// --- Project Settings -----------------------------------------------------

void MainWindow::onProjectSettings()
{
    // Canvas facts come from the RUNTIME authority when a panel exists
    // (Panel::canvasSize() — pixels are the truth since the resolution
    // epic), else from the project's stored size.
    QSize canvas(m_canvasWidth, m_canvasHeight);
    for (Scene *scene : m_scenes) {
        if (!scene->panels.isEmpty() && scene->panels.first()->canvasSize().isValid()) {
            canvas = scene->panels.first()->canvasSize();
            break;
        }
    }
    ProjectSettingsDialog dialog(m_projectName, m_projectFps, canvas, this);
    // Apply and OK both route here; Cancel emits nothing, so the project is
    // untouched until the artist commits — the dialog holds the pending
    // values, the window holds the truth.
    connect(&dialog, &ProjectSettingsDialog::applied, this,
            &MainWindow::applyProjectSettings);
    // Resize is its own confirmed, non-undoable workflow rather than a
    // pending edit: close the settings window first so the size prompt and
    // the confirm are not stacked three dialogs deep.
    bool resizeAsked = false;
    connect(&dialog, &ProjectSettingsDialog::resizeRequested, this,
            [&dialog, &resizeAsked] {
                resizeAsked = true;
                dialog.reject();
            });
    dialog.exec();
    if (resizeAsked)
        onResizeProject(canvas);
}

// Canvas-only resize. The order here is the whole safety argument: refuse
// while an edit is in flight, ask for the size, PRECHECK the memory before
// anything is touched, offer to save (that file is the rollback if a panel
// still fails), confirm the irreversible part in the user's terms, and only
// then swap panel by panel.
void MainWindow::onResizeProject(const QSize &currentSize)
{
    // 1. In-flight work: refuse rather than commit it. Committing an
    // unfinished stroke or a half-placed transform writes pixels the user
    // never chose to keep, into an undo stack this operation then clears.
    QString active;
    if (m_storyboard && m_storyboard->hasActiveEdit(&active)) {
        QMessageBox::information(
            this, QStringLiteral("Resize Project"),
            QStringLiteral("Finish %1 before resizing the project.\n\nNothing "
                           "has been changed.")
                .arg(active));
        return;
    }

    ResizeProjectDialog sizeDialog(currentSize, this);
    if (sizeDialog.exec() != QDialog::Accepted)
        return;
    const QSize newSize = sizeDialog.chosenSize();

    // 2. PRECHECK, before a single pixel moves. A refusal at this point is
    // perfectly atomic: nothing has been touched.
    const ProjectResize::Plan plan =
        ProjectResize::plan(m_scenes, currentSize, newSize);
    if (!plan.fits) {
        QMessageBox::warning(this, QStringLiteral("Resize Project"),
                             plan.refusal);
        return;
    }

    // 3. Offer to save FIRST. This file is the rollback: if a panel fails
    // to allocate despite the precheck, reopening it restores the project
    // exactly as it was.
    if (!m_currentProjectPath.isEmpty() || !m_scenes.isEmpty()) {
        const QMessageBox::StandardButton save = QMessageBox::question(
            this, QStringLiteral("Resize Project"),
            QStringLiteral("Save the project before resizing?\n\nResizing "
                           "cannot be undone. A saved copy is the only way "
                           "back to the current canvas."),
            QMessageBox::Save | QMessageBox::No | QMessageBox::Cancel,
            QMessageBox::Save);
        if (save == QMessageBox::Cancel)
            return;
        if (save == QMessageBox::Save) {
            onSaveProject();
            if (m_currentProjectPath.isEmpty())
                return; // the save was cancelled: treat it as cancelling
        }
    }

    // 4. Confirm the irreversible part, in the user's terms. The crop
    // sentence appears ONLY when something is actually cropped.
    const bool grows = !plan.crops;
    QString body =
        QStringLiteral("The canvas changes from %1 \xC3\x97 %2 to %3 \xC3\x97 %4.\n\n")
            .arg(currentSize.width())
            .arg(currentSize.height())
            .arg(newSize.width())
            .arg(newSize.height());
    body += grows
        ? QStringLiteral("Artwork keeps its current size and position, "
                         "centred on the new canvas. Nothing is scaled.\n\n")
        : QStringLiteral("Artwork keeps its current size and is centred on "
                         "the new canvas; nothing is scaled. Artwork outside "
                         "the new canvas will be cropped and cannot be "
                         "recovered.\n\n");
    body += QStringLiteral("This cannot be undone. Your undo history will be "
                           "cleared.");
    QMessageBox confirm(QMessageBox::Question,
                        QStringLiteral("Resize project to %1 \xC3\x97 %2?")
                            .arg(newSize.width())
                            .arg(newSize.height()),
                        body, QMessageBox::NoButton, this);
    QPushButton *go =
        confirm.addButton(QStringLiteral("Resize Project"),
                          QMessageBox::AcceptRole);
    confirm.addButton(QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != go)
        return;

    // 5-6. Swap and settle. One definition, shared with the test hook.
    const ProjectResize::Outcome outcome = applyCanvasResizeInternal(newSize);

    if (!outcome.ok) {
        // Should be unreachable after the precheck. Never leave this
        // quiet: a partially resized project is exactly what the loader's
        // panel census now reports, and the saved file is the way back.
        QMessageBox::critical(
            this, QStringLiteral("Resize Project"),
            QStringLiteral(
                "The resize ran out of memory at scene %1, panel %2, after "
                "%3 panel%4.\n\nThe project now holds panels of two sizes. "
                "Close it WITHOUT saving and reopen the version you saved a "
                "moment ago to get back to %5 \xC3\x97 %6.")
                .arg(outcome.failedSceneNumber)
                .arg(outcome.failedPanelIndex)
                .arg(outcome.panelsResized)
                .arg(outcome.panelsResized == 1 ? "" : "s")
                .arg(currentSize.width())
                .arg(currentSize.height()));
    }
}

// The resize proper: swap every panel, then put the project's state right.
// No dialogs, so this is also what the gate drives.
ProjectResize::Outcome MainWindow::applyCanvasResizeInternal(const QSize &newSize)
{
    const QSize currentSize(m_canvasWidth, m_canvasHeight);
    const QPoint offset = ProjectResize::centreOffset(currentSize, newSize);
    const ProjectResize::Outcome outcome =
        ProjectResize::apply(m_scenes, newSize, offset);

    // ORDER MATTERS: the members must be updated before anything can save,
    // or a save would write the old manifest against new pixels — a
    // mismatch of our own making.
    m_canvasWidth = newSize.width();
    m_canvasHeight = newSize.height();
    if (m_storyboard)
        m_storyboard->applyProjectResize(newSize, offset);
    if (m_undoStack)
        m_undoStack->clear(); // every canvas-space command is now invalid
    // A resize changes every panel AND clears the history that could undo
    // it, so the unsaved-changes flag cannot come from the undo stack here.
    markDirty();
    updateSaveActions();
    updateTitle();
    return outcome;
}

void MainWindow::applyCanvasResize(const QSize &newSize)
{
    applyCanvasResizeInternal(newSize);
}

void MainWindow::applyProjectSettings(const QString &projectName, int fps)
{
    m_projectName = projectName;
    m_projectFps = fps;
    // FPS drives TIMING only: the animatic's per-block frame counts
    // re-derive from it (AnimaticTimeline::setFps -> rebuildBlocks). No
    // layer pixel is touched — the frame rate is metadata about playback,
    // not about the artwork.
    if (m_animatic)
        m_animatic->setFps(m_projectFps);
    markDirty(); // name and frame rate are saved, and neither is undoable
    updateTitle();
}

void MainWindow::updateTitle()
{
    // [*] is Qt's modified marker: it renders as an asterisk while
    // windowModified is true and disappears when it is not.
    setWindowTitle(
        QString::fromUtf8("SANKO TV \xE2\x80\x94 %1[*]").arg(m_projectName));
    setWindowModified(m_dirty);
}

void MainWindow::markDirty()
{
    if (m_dirty)
        return; // idempotent: this is called from very many places
    m_dirty = true;
    setWindowModified(true);
}

void MainWindow::setClean()
{
    m_dirty = false;
    setWindowModified(false);
}

// --- Scene ownership ------------------------------------------------------

QSize MainWindow::activePanelSizeForTest() const
{
    auto *canvas =
        m_storyboard ? m_storyboard->findChild<DrawingCanvas *>() : nullptr;
    return canvas ? canvas->canvasSize() : QSize();
}

void MainWindow::freeScenes()
{
    // DETACH BEFORE DESTROYING. Several pages hold NON-OWNING Scene*/Panel*
    // into m_scenes, and deleting the scenes underneath them leaves those
    // members dangling — not harmlessly, but until something dereferences
    // them:
    //   * DrawingCanvas::m_panel — the crash a user reported as "File >
    //     Open closes the app": the canvas kept pointing at a deleted panel
    //     for the whole of the following load, and setActivePanel then read
    //     it through invalidateComposite().
    //   * AnimaticTimeline::m_scenes — loadFromPath calls setFps() AFTER
    //     this, and a CHANGED rate rebuilds the timeline by walking the
    //     scene list. The animatic otherwise only reloads when the user
    //     navigates to it, so the stale list survives until then.
    //   * GenerationPage::m_scenes and its per-row Panel* — same lazy
    //     refresh, same exposure.
    // The detach lives HERE rather than at the four call sites (this one,
    // the destructor, buildScenesFromJson and onNewProject) because a fifth
    // caller added later would have to remember, and this is the last
    // moment where every holder is still consistent.
    // The camera and the per-drawing view aids belong to the project being
    // left, not the one arriving: one canvas serves them all, so without
    // this the zoom, rotation, pan, flip, onion skin and light table simply
    // carry over. It lives HERE rather than in resetProjectState because
    // this is the teardown all three transitions actually share — Open goes
    // through loadFromPath, which calls freeScenes() directly and never
    // touches resetProjectState.
    if (auto *canvas =
            m_storyboard ? m_storyboard->findChild<DrawingCanvas *>() : nullptr)
        canvas->resetViewForNewProject();
    if (m_storyboard)
        m_storyboard->detachScenes(); // NOT loadScenes({}): see detachScenes
    if (m_animatic)
        m_animatic->loadScenes({}); // clears its rows AND its timeline's list
    if (m_generation)
        m_generation->loadScenes({});
    for (Scene *scene : m_scenes)
        delete scene; // Scene destructor deletes its panels
    m_scenes.clear();
}

void MainWindow::buildScenesFromJson(const QJsonArray &scenes)
{
    // A Script Editor re-parse REPLACES every scene in the project. That is
    // as large a change as there is, and none of it is undoable.
    markDirty();
    freeScenes();
    for (const QJsonValue &value : scenes) {
        const QJsonObject obj = value.toObject();
        Scene *scene = new Scene;
        scene->number = obj.value(QStringLiteral("scene_number")).toInt();
        scene->location = obj.value(QStringLiteral("location")).toString();
        scene->timeOfDay = obj.value(QStringLiteral("time_of_day")).toString();
        scene->action = obj.value(QStringLiteral("action")).toString();
        // One blank panel per scene, at the PROJECT's canvas size (set by
        // the New Project dialog before the Script Editor can run).
        scene->panels.append(
            makeBlankPanel(QSize(m_canvasWidth, m_canvasHeight)));
        m_scenes.append(scene);
    }
}

// --- Project lifecycle ----------------------------------------------------

// THE teardown, shared by New, Open and Close. They differ only in what they
// do AFTERWARDS — New applies the dialog's values and goes to the Script
// Editor, Open applies the file's and goes to the Storyboard, Close applies
// nothing and goes to the Dashboard — so there is no second reset to keep
// correct.
//
// This deliberately clears more than the old onNewProject did. That function
// left the previous project's undo history, panel clipboard and perspective
// vanishing points in place, so starting a new project inherited the last
// one's construction lines and an undo stack describing panels that no
// longer existed. Open happened to escape it (perspectiveFromJson overwrites
// the VPs, loadScenes clears the stack), which is why only New showed it.
void MainWindow::resetProjectState(ClipboardPolicy clipboards)
{
    freeScenes(); // detaches every page BEFORE deleting the scenes

    m_currentProjectPath.clear();
    m_projectName = QStringLiteral("Untitled Project");
    m_projectFps = 24;
    // The 960x540 members are the documented PRE-PROJECT IDLE values, not a
    // guess: going back to them beats an invalid size that any panel-making
    // path would build garbage from.
    m_canvasWidth = 960;
    m_canvasHeight = 540;
    if (m_storyboard)
        m_storyboard->setProjectCanvasSize(QSize(m_canvasWidth, m_canvasHeight));

    m_consistencyEntries.clear();
    if (m_consistencyBoard)
        m_consistencyBoard->refresh();
    if (m_animatic) {
        m_animatic->setAudioPath(QString()); // stops the player too
        m_animatic->setFps(m_projectFps);
    }
    if (m_undoStack)
        m_undoStack->clear(); // its commands describe panels that are gone

    if (auto *canvas =
            m_storyboard ? m_storyboard->findChild<DrawingCanvas *>() : nullptr) {
        // Vanishing points are per project: reset() removes every VP and
        // keeps the density/thickness/snap defaults.
        canvas->perspective()->reset();
        if (clipboards == ClipboardPolicy::Clear)
            canvas->clearCanvasClipboard();
        canvas->update();
    }
    if (clipboards == ClipboardPolicy::Clear && m_storyboard)
        m_storyboard->clearPanelClipboard();

    updateSaveActions();
    updateTitle();
    setClean(); // LAST: tearing the old project down marks dirty on the way
}

void MainWindow::onNewProject()
{
    // New and Open keep the clipboards: pasting a panel into the next
    // project is a real workflow, and the paste guard refuses a mismatched
    // size. Close does not, because there is nothing to paste into.
    resetProjectState(ClipboardPolicy::Keep);
}

void MainWindow::onCloseProject()
{
    if (!confirmDiscardChanges(QStringLiteral("closing the project")))
        return;
    resetProjectState(ClipboardPolicy::Clear);
    if (m_stack && m_dashboard)
        m_stack->setCurrentWidget(m_dashboard);
}

bool MainWindow::onDashboardForTest() const
{
    return m_stack && m_dashboard && m_stack->currentWidget() == m_dashboard;
}

// Save, reporting whether it ACTUALLY HAPPENED. onSaveProject returns void:
// a Save As that the artist cancels, or a write that fails, is
// indistinguishable from success to its caller. That is exactly the hole
// this closes, because the prompt's whole purpose is to not lose work.
bool MainWindow::saveForPrompt()
{
    if (m_currentProjectPath.isEmpty()) {
        onSaveProjectAs(); // modal; may be cancelled, leaving no path
        return !m_currentProjectPath.isEmpty() && !isDirty();
    }
    return saveToPath(m_currentProjectPath) && !isDirty();
}

bool MainWindow::mayDiscardAfterAnswer(DiscardAnswer answer)
{
    switch (answer) {
    case DiscardAnswer::Save:
        // NOT an unconditional yes. If the save did not happen, the
        // transition must not proceed: otherwise the prompt that exists to
        // prevent data loss becomes the thing that causes it.
        return saveForPrompt();
    case DiscardAnswer::Discard:
        return true;
    case DiscardAnswer::Cancel:
        return false;
    }
    return false;
}

bool MainWindow::confirmDiscardChanges(const QString &actionDescription)
{
    if (!shouldPromptToSave())
        return true; // nothing to lose
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved Changes"));
    box.setText(QStringLiteral("Save changes to “%1” before %2?")
                    .arg(m_projectName, actionDescription));
    box.setInformativeText(
        QStringLiteral("Your changes will be lost if you don't save them."));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard
                           | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:
        return mayDiscardAfterAnswer(DiscardAnswer::Save);
    case QMessageBox::Discard:
        return mayDiscardAfterAnswer(DiscardAnswer::Discard);
    default:
        return mayDiscardAfterAnswer(DiscardAnswer::Cancel);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmDiscardChanges(QStringLiteral("exiting"))) {
        event->ignore(); // Cancel must actually cancel, X included
        return;
    }
    event->accept();
}

void MainWindow::onOpenProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Project"), QDir::homePath(),
        QStringLiteral("SankoTV Project (*.sankotv)"));
    if (path.isEmpty())
        return;
    openProject(path);
}

// THE open path. File > Open reaches it after its file dialog, Open Recent
// reaches it directly — so the unsaved-changes prompt, the teardown and the
// load exist once rather than once per entry point.
void MainWindow::openProject(const QString &path)
{
    if (!confirmDiscardChanges(QStringLiteral("opening another project")))
        return;
    if (!QFileInfo::exists(path)) {
        // A recent entry whose file has been moved or deleted: say so and
        // forget it, rather than failing to load something invisible.
        NewProjectDialog::removeRecentProject(path);
        rebuildRecentMenu();
        QMessageBox::warning(
            this, QStringLiteral("Open Project"),
            QStringLiteral("This project could not be found:\n\n%1\n\nIt has "
                           "been removed from the recent list.")
                .arg(path));
        return;
    }
    loadFromPath(path);
    rebuildRecentMenu(); // the just-opened project moves to the top
}

// Rebuilt from the shared recents store. Every entry routes through
// openProject(), so nothing about loading is duplicated here.
void MainWindow::rebuildRecentMenu()
{
    if (!m_recentMenu)
        return;
    m_recentMenu->clear();
    const QVector<NewProjectDialog::RecentEntry> recents =
        NewProjectDialog::recentProjects();
    if (recents.isEmpty()) {
        QAction *empty =
            m_recentMenu->addAction(QStringLiteral("No Recent Projects"));
        empty->setEnabled(false);
        return;
    }
    for (const NewProjectDialog::RecentEntry &entry : recents) {
        const QString name = QFileInfo(entry.path).completeBaseName();
        QAction *action = m_recentMenu->addAction(name);
        action->setToolTip(entry.path);
        const QString path = entry.path;
        connect(action, &QAction::triggered, this,
                [this, path] { openProject(path); });
    }
}

void MainWindow::onSaveProject()
{
    if (m_scenes.isEmpty())
        return;
    if (m_currentProjectPath.isEmpty()) {
        onSaveProjectAs();
        return;
    }
    if (saveToPath(m_currentProjectPath))
        updateTitle();
}

// True when it is safe to go ahead: either nothing else lives in that
// folder, or the artist has been told what will happen and insisted.
bool MainWindow::confirmSaveAsLocation(const QString &path)
{
    const QFileInfo target(path);
    QStringList others;
    for (const QFileInfo &fi : target.absoluteDir().entryInfoList(
             {QStringLiteral("*.sankotv")}, QDir::Files, QDir::Name)) {
        if (fi.absoluteFilePath() != target.absoluteFilePath())
            others << fi.fileName();
    }
    if (others.isEmpty())
        return true;

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Save Project As"));
    box.setText(QStringLiteral("This folder already contains another "
                               "project.\n\nSaving here will make the two "
                               "share their artwork files."));
    box.setInformativeText(
        QStringLiteral(
            "Already here: %1\n\n"
            "SankoTV stores every panel and layer as image files named by "
            "position (panel_s0_p0_layer0.png and so on), and those names do "
            "not identify the project. Two projects in one folder write the "
            "SAME image files, so the next time either one is saved it "
            "OVERWRITES THE OTHER'S ARTWORK. That loss is permanent and "
            "there is no undo for it.\n\n"
            "To keep this copy independent, save it into a folder of its "
            "own.")
            .arg(others.join(QStringLiteral(", "))));
    QPushButton *anyway =
        box.addButton(QStringLiteral("Save Here Anyway"),
                      QMessageBox::DestructiveRole);
    QPushButton *choose =
        box.addButton(QStringLiteral("Choose Another Folder"),
                      QMessageBox::RejectRole);
    box.setDefaultButton(choose); // never proceed on a reflexive Enter
    box.exec();
    Q_UNUSED(choose);
    return box.clickedButton() == anyway;
}

void MainWindow::onSaveProjectAs()
{
    if (m_scenes.isEmpty())
        return;

    const QString start = m_currentProjectPath.isEmpty()
        ? QDir::homePath() + QStringLiteral("/") + m_projectName + QStringLiteral(".sankotv")
        : m_currentProjectPath;

    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save Project As"), start,
        QStringLiteral("SankoTV Project (*.sankotv)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".sankotv"), Qt::CaseInsensitive))
        path += QStringLiteral(".sankotv");

    // GUARD: a project's panels and layers are written as image files named
    // by POSITION (panel_s0_p0_layer0.png and so on), with nothing in the
    // name identifying the project. Two projects in one folder therefore
    // write the SAME image files, and whichever is saved next overwrites the
    // other's artwork permanently. The Save As itself is harmless — both
    // hold identical pixels at that instant — which is exactly why this is
    // worth saying out loud before it happens rather than after.
    if (!confirmSaveAsLocation(path))
        return;

    m_projectName = QFileInfo(path).completeBaseName();
    if (saveToPath(path))
        updateTitle();
}

// --- Save / Load ----------------------------------------------------------

bool MainWindow::saveToPath(const QString &path)
{
    m_storyboard->commitQuickShape(); // temporary vectors are not serialized
    m_storyboard->ensureBrushPixelsForSave();

    // The serialization core lives in ProjectIO (extracted with the
    // resolution epic so the seam can drive it without this window); this
    // shell owns the page prep above, the dialogs, and the recents.
    ProjectIO::SaveData data;
    data.projectName = m_projectName;
    data.fps = m_projectFps;
    data.canvasSize = QSize(m_canvasWidth, m_canvasHeight);
    data.scenes = m_scenes;
    data.consistency = m_consistencyEntries;
    data.audioPath = m_animatic->audioPath(); // scratch track (path only)
    data.perspective = m_storyboard->perspectiveToJson();
    const QJsonObject root =
        ProjectIO::projectToJson(data, QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Save Project"),
                             QStringLiteral("Could not write to:\n%1").arg(path));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    m_currentProjectPath = path;
    NewProjectDialog::recordRecentProject(path);
    setClean(); // what is on disk now matches what is in memory
    return true;
}

bool MainWindow::loadFromPath(const QString &path)
{
    m_storyboard->commitQuickShape(); // resolve before panels are replaced
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Open Project"),
                             QStringLiteral("Could not open:\n%1").arg(path));
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this, QStringLiteral("Open Project"),
                             QStringLiteral("Not a valid SankoTV project file."));
        return false;
    }

    const QJsonObject root = doc.object();
    const QString folder = QFileInfo(path).absolutePath();

    freeScenes();

    // The model rebuild + every migration + the pixels-win reconciliation
    // live in ProjectIO (extracted with the resolution epic); this shell
    // owns dialogs, member assignment, and page updates.
    ProjectIO::LoadedProject loaded = ProjectIO::projectFromJson(root, folder);

    m_projectName = loaded.projectName;
    if (m_projectName.isEmpty())
        m_projectName = QFileInfo(path).completeBaseName();
    m_projectFps = loaded.fps;
    m_scenes = loaded.scenes; // ownership transfers to MainWindow
    m_consistencyEntries = loaded.consistency;
    if (m_consistencyBoard)
        m_consistencyBoard->refresh();

    // Scratch audio track (loaded only if the file still exists at that path).
    m_animatic->setAudioPath(loaded.audioPath);

    m_currentProjectPath = path;
    updateSaveActions();
    updateTitle();

    // PIXELS WIN (reconciled inside ProjectIO): the project opens at the
    // artwork's real size; a lying manifest is reported here — never
    // silently — and corrected by the next save. Artwork is untouched.
    // A MIXED project takes precedence and gets its own message: the plain
    // mismatch text would claim the artwork is one size when only some of
    // it is. Neither case alters a pixel.
    if (loaded.mixedSizes)
        QMessageBox::information(
            this, QStringLiteral("Project Size"),
            ProjectIO::mixedCanvasSizesDialogText(
                loaded.manifestSize, loaded.pixelSize,
                loaded.majorityPanelCount, loaded.offSizePanels));
    else if (loaded.mismatch)
        QMessageBox::information(
            this, QStringLiteral("Project Size"),
            canvasMismatchDialogText(loaded.manifestSize, loaded.pixelSize));
    m_canvasWidth = loaded.pixelSize.width();
    m_canvasHeight = loaded.pixelSize.height();
    m_storyboard->setProjectCanvasSize(QSize(m_canvasWidth, m_canvasHeight));
    m_animatic->setFps(m_projectFps);
    NewProjectDialog::recordRecentProject(path);

    // Skip the Script Editor: go straight to the Storyboard.
    m_storyboard->loadScenes(m_scenes);
    m_storyboard->perspectiveFromJson(loaded.perspective);
    m_stack->setCurrentWidget(m_storyboard);
    // LAST, and it must stay last. Opening a project fires the very signals
    // that mark it dirty — panels are selected, the canvas repaints and
    // publishes, pages rebuild — so a freshly opened project would otherwise
    // be born modified and prompt to save work nobody did. Anything added
    // below this line that touches the model must clear the flag again.
    setClean();
    return true;
}
