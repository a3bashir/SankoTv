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

    m_storyboard->setConsistencyEntries(&m_consistencyEntries); // read-only
    m_consistencyBoard->setEntries(&m_consistencyEntries);      // read-write
    m_generation->setConsistencyEntries(&m_consistencyEntries); // read-only

    // Paste / Paste in Place enable once a panel lands on the clipboard.
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
                         QStringLiteral(SANKOTV_GIT_HEAD)});
#else
                         QString()});
#endif
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

    QAction *openAct = fileMenu->addAction(QStringLiteral("Open Project..."));
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::onOpenProject);

    fileMenu->addSeparator();

    m_saveAct = fileMenu->addAction(QStringLiteral("Save Project"));
    m_saveAct->setShortcut(QKeySequence::Save);
    connect(m_saveAct, &QAction::triggered, this, &MainWindow::onSaveProject);

    m_saveAsAct = fileMenu->addAction(QStringLiteral("Save Project As..."));
    m_saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAct, &QAction::triggered, this, &MainWindow::onSaveProjectAs);

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
}

void MainWindow::updateTitle()
{
    setWindowTitle(QString::fromUtf8("SANKO TV \xE2\x80\x94 %1").arg(m_projectName));
}

// --- Scene ownership ------------------------------------------------------

void MainWindow::freeScenes()
{
    for (Scene *scene : m_scenes)
        delete scene; // Scene destructor deletes its panels
    m_scenes.clear();
}

void MainWindow::buildScenesFromJson(const QJsonArray &scenes)
{
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

void MainWindow::onNewProject()
{
    freeScenes();
    m_consistencyEntries.clear();
    if (m_consistencyBoard)
        m_consistencyBoard->refresh();
    if (m_animatic)
        m_animatic->setAudioPath(QString()); // clear any scratch audio
    m_currentProjectPath.clear();
    m_projectName = QStringLiteral("Untitled Project");
    updateSaveActions();
    updateTitle();
}

void MainWindow::onOpenProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Project"), QDir::homePath(),
        QStringLiteral("SankoTV Project (*.sankotv)"));
    if (path.isEmpty())
        return;
    loadFromPath(path);
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
    if (loaded.mismatch)
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
    return true;
}
