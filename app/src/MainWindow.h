#pragma once

#include <QMainWindow>
#include <QString>
#include <QVector>

class DashboardPage;
class ScriptEditorPage;
class StoryboardPage;
class AnimaticPage;
class QStackedWidget;
class QAction;
class QJsonArray;

struct Scene;

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
    bool saveToPath(const QString &path);
    bool loadFromPath(const QString &path);

    QStackedWidget *m_stack = nullptr;
    DashboardPage *m_dashboard = nullptr;
    ScriptEditorPage *m_scriptEditor = nullptr;
    StoryboardPage *m_storyboard = nullptr;
    AnimaticPage *m_animatic = nullptr;

    // MainWindow owns the scene/panel objects; pages hold non-owning pointers.
    QVector<Scene *> m_scenes;
    QString m_currentProjectPath;
    QString m_projectName = QStringLiteral("Untitled Project");

    QAction *m_saveAct = nullptr;
    QAction *m_saveAsAct = nullptr;
};
