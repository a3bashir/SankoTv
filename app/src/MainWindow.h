#pragma once

#include <QMainWindow>

class DashboardPage;
class ScriptEditorPage;
class StoryboardPage;
class QStackedWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

private:
    void setupMenuBar();

    QStackedWidget *m_stack = nullptr;
    DashboardPage *m_dashboard = nullptr;
    ScriptEditorPage *m_scriptEditor = nullptr;
    StoryboardPage *m_storyboard = nullptr;
};
