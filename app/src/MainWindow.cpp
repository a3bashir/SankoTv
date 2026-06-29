#include "MainWindow.h"

#include "AnimaticPage.h"
#include "ConsistencyBoard.h"
#include "DashboardPage.h"
#include "ScriptEditorPage.h"
#include "StoryboardModel.h"
#include "StoryboardPage.h"

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QUuid>
#include <Qt>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setMinimumSize(1280, 720);

    setupMenuBar();

    m_stack = new QStackedWidget(this);
    m_dashboard = new DashboardPage;
    m_scriptEditor = new ScriptEditorPage;
    m_storyboard = new StoryboardPage;
    m_animatic = new AnimaticPage;
    m_consistencyBoard = new ConsistencyBoard;

    m_storyboard->setConsistencyEntries(&m_consistencyEntries); // read-only
    m_consistencyBoard->setEntries(&m_consistencyEntries);      // read-write

    m_stack->addWidget(m_dashboard);        // index 0
    m_stack->addWidget(m_scriptEditor);     // index 1
    m_stack->addWidget(m_storyboard);       // index 2
    m_stack->addWidget(m_animatic);         // index 3
    m_stack->addWidget(m_consistencyBoard); // index 4

    // Dashboard -> Script Editor (fresh project).
    connect(m_dashboard, &DashboardPage::newProjectRequested, this, [this] {
        onNewProject();
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

    // Storyboard <-> Consistency Board.
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

    menuBar()->addMenu(QStringLiteral("Edit"));
    menuBar()->addMenu(QStringLiteral("View"));
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
        scene->panels.append(makeBlankPanel()); // one blank panel per scene
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
    const QFileInfo info(path);
    const QString folder = info.absolutePath();

    QJsonArray scenesArray;
    for (int i = 0; i < m_scenes.size(); ++i) {
        Scene *scene = m_scenes.at(i);

        QJsonObject sceneObj;
        sceneObj[QStringLiteral("name")] = QStringLiteral("Scene %1").arg(scene->number);
        sceneObj[QStringLiteral("number")] = scene->number;     // preserved (not lost)
        sceneObj[QStringLiteral("location")] = scene->location;
        sceneObj[QStringLiteral("timeOfDay")] = scene->timeOfDay; // preserved
        sceneObj[QStringLiteral("action")] = scene->action;

        QJsonArray panelsArray;
        for (int j = 0; j < scene->panels.size(); ++j) {
            Panel *panel = scene->panels.at(j);

            const QString pngName = QStringLiteral("panel_s%1_p%2.png").arg(i).arg(j);
            panel->pixmap.save(folder + QStringLiteral("/") + pngName, "PNG");

            QJsonObject panelObj;
            panelObj[QStringLiteral("duration")] = panel->duration;
            panelObj[QStringLiteral("shotType")] = panel->shotType;
            panelObj[QStringLiteral("camera")] = panel->cameraAngle;
            panelObj[QStringLiteral("lens")] = panel->lens;
            panelObj[QStringLiteral("mood")] = panel->mood;
            panelObj[QStringLiteral("notes")] = panel->notes;
            panelObj[QStringLiteral("pixmapFile")] = pngName;
            panelsArray.append(panelObj);
        }
        sceneObj[QStringLiteral("panels")] = panelsArray;
        scenesArray.append(sceneObj);
    }

    // Consistency board entries + their thumbnail PNGs.
    QJsonArray consistencyArray;
    for (const ConsistencyEntry &entry : m_consistencyEntries) {
        QJsonObject entryObj;
        entryObj[QStringLiteral("id")] = entry.id;
        entryObj[QStringLiteral("name")] = entry.name;
        entryObj[QStringLiteral("type")] = entry.type;
        entryObj[QStringLiteral("description")] = entry.description;

        QJsonArray tagsArray;
        for (const QString &tag : entry.tags)
            tagsArray.append(tag);
        entryObj[QStringLiteral("tags")] = tagsArray;

        QString thumbFile;
        if (!entry.thumbnail.isNull()) {
            QString safeName = entry.name;
            safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")),
                             QStringLiteral("_"));
            thumbFile = QStringLiteral("consistency_%1_%2.png").arg(safeName, entry.id);
            entry.thumbnail.save(folder + QStringLiteral("/") + thumbFile, "PNG");
        }
        entryObj[QStringLiteral("thumbnailFile")] = thumbFile;
        consistencyArray.append(entryObj);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("projectName")] = m_projectName;
    root[QStringLiteral("scenes")] = scenesArray;
    root[QStringLiteral("consistencyBoard")] = consistencyArray;
    root[QStringLiteral("audioPath")] = m_animatic->audioPath(); // scratch track (path only)

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Save Project"),
                             QStringLiteral("Could not write to:\n%1").arg(path));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    m_currentProjectPath = path;
    return true;
}

bool MainWindow::loadFromPath(const QString &path)
{
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

    m_projectName = root.value(QStringLiteral("projectName")).toString();
    if (m_projectName.isEmpty())
        m_projectName = QFileInfo(path).completeBaseName();

    const QJsonArray scenesArray = root.value(QStringLiteral("scenes")).toArray();
    for (const QJsonValue &sv : scenesArray) {
        const QJsonObject sceneObj = sv.toObject();
        Scene *scene = new Scene;
        scene->number = sceneObj.value(QStringLiteral("number")).toInt();
        scene->location = sceneObj.value(QStringLiteral("location")).toString();
        scene->timeOfDay = sceneObj.value(QStringLiteral("timeOfDay")).toString();
        if (scene->timeOfDay.isEmpty())
            scene->timeOfDay = QStringLiteral("UNSPECIFIED");
        scene->action = sceneObj.value(QStringLiteral("action")).toString();

        const QJsonArray panelsArray = sceneObj.value(QStringLiteral("panels")).toArray();
        for (const QJsonValue &pv : panelsArray) {
            const QJsonObject panelObj = pv.toObject();
            Panel *panel = new Panel;
            panel->duration = panelObj.value(QStringLiteral("duration")).toInt(3);
            if (panel->duration < 1)
                panel->duration = 3;
            // Only override defaults when a non-empty value is stored.
            const QString shotType = panelObj.value(QStringLiteral("shotType")).toString();
            if (!shotType.isEmpty())
                panel->shotType = shotType;
            const QString camera = panelObj.value(QStringLiteral("camera")).toString();
            if (!camera.isEmpty())
                panel->cameraAngle = camera;
            const QString lens = panelObj.value(QStringLiteral("lens")).toString();
            if (!lens.isEmpty())
                panel->lens = lens;
            panel->mood = panelObj.value(QStringLiteral("mood")).toString();
            panel->notes = panelObj.value(QStringLiteral("notes")).toString();

            const QString pngName = panelObj.value(QStringLiteral("pixmapFile")).toString();
            QPixmap pixmap;
            if (!pngName.isEmpty())
                pixmap.load(folder + QStringLiteral("/") + pngName);
            if (pixmap.isNull()) {
                pixmap = QPixmap(960, 540);
                pixmap.fill(Qt::white);
            }
            panel->pixmap = pixmap;

            scene->panels.append(panel);
        }
        m_scenes.append(scene);
    }

    // Backfill scene numbers if the file didn't carry them.
    for (int i = 0; i < m_scenes.size(); ++i) {
        if (m_scenes.at(i)->number == 0)
            m_scenes[i]->number = i + 1;
    }

    // Consistency board entries.
    m_consistencyEntries.clear();
    const QJsonArray consistencyArray =
        root.value(QStringLiteral("consistencyBoard")).toArray();
    for (const QJsonValue &cv : consistencyArray) {
        const QJsonObject entryObj = cv.toObject();
        ConsistencyEntry entry;
        entry.id = entryObj.value(QStringLiteral("id")).toString();
        if (entry.id.isEmpty())
            entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.name = entryObj.value(QStringLiteral("name")).toString();
        entry.type = entryObj.value(QStringLiteral("type")).toString();
        if (entry.type.isEmpty())
            entry.type = QStringLiteral("Character");
        entry.description = entryObj.value(QStringLiteral("description")).toString();

        const QJsonArray tagsArray = entryObj.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue &tv : tagsArray)
            entry.tags << tv.toString();

        const QString thumbFile = entryObj.value(QStringLiteral("thumbnailFile")).toString();
        if (!thumbFile.isEmpty()) {
            QPixmap pm;
            if (pm.load(folder + QStringLiteral("/") + thumbFile))
                entry.thumbnail = pm;
        }
        m_consistencyEntries.append(entry);
    }
    if (m_consistencyBoard)
        m_consistencyBoard->refresh();

    // Scratch audio track (loaded only if the file still exists at that path).
    m_animatic->setAudioPath(root.value(QStringLiteral("audioPath")).toString());

    m_currentProjectPath = path;
    updateSaveActions();
    updateTitle();

    // Skip the Script Editor: go straight to the Storyboard.
    m_storyboard->loadScenes(m_scenes);
    m_stack->setCurrentWidget(m_storyboard);
    return true;
}
