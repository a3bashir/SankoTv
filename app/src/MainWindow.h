#pragma once

#include <QMainWindow>
#include <QString>
#include <QVector>

class DashboardPage;
class QUndoStack;
class ScriptEditorPage;
class StoryboardPage;
class AnimaticPage;
class ConsistencyBoard;
class GenerationPage;
class QStackedWidget;
class QAction;
class QJsonArray;

struct Scene;
struct ConsistencyEntry;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void setupMenuBar();
    void updateSaveActions();
    void updateTitle();
    void freeScenes();
    void buildScenesFromJson(const QJsonArray &scenes);

    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onPreferences(); // Edit > Preferences... (category list + settings pane)
    // File > Project Settings... — settings of the CURRENTLY OPEN project
    // (name, frame rate; canvas facts read-only). Pending until Apply/OK.
    void onProjectSettings();
    void applyProjectSettings(const QString &projectName, int fps);
    bool saveToPath(const QString &path);
    bool loadFromPath(const QString &path);

    QStackedWidget *m_stack = nullptr;
    QUndoStack *m_undoStack = nullptr; // ONE app-wide chronological history
    DashboardPage *m_dashboard = nullptr;
    ScriptEditorPage *m_scriptEditor = nullptr;
    StoryboardPage *m_storyboard = nullptr;
    AnimaticPage *m_animatic = nullptr;
    ConsistencyBoard *m_consistencyBoard = nullptr;
    GenerationPage *m_generation = nullptr;

    // MainWindow owns the scene/panel objects; pages hold non-owning pointers.
    QVector<Scene *> m_scenes;
    QVector<ConsistencyEntry> m_consistencyEntries;
    QString m_currentProjectPath;
    QString m_projectName = QStringLiteral("Untitled Project");
    // Project properties. fps drives the animatic timeline; canvasWidth/
    // Height are the project's REAL canvas resolution — the single source
    // of truth that shapes every layer at create/load. At runtime the
    // panels' own pixels are the authority (Panel::canvasSize()); on load
    // these are RECONCILED to the pixels (pixels win, the user is told of
    // any disagreement, the manifest is corrected on save). The 960x540
    // initialisers are only the pre-project idle values; every create/load
    // path overwrites them before a panel can exist.
    int m_projectFps = 24;
    int m_canvasWidth = 960;
    int m_canvasHeight = 540;

    QAction *m_saveAct = nullptr;
    QAction *m_saveAsAct = nullptr;
    QAction *m_projectSettingsAct = nullptr;
    // Edit-menu panel clipboard actions (enabled once a panel is copied).
    QAction *m_pastePanelAct = nullptr;
    QAction *m_pastePanelInPlaceAct = nullptr;
};
