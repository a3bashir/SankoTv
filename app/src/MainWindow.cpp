#include "MainWindow.h"

#include "DashboardPage.h"
#include "ScriptEditorPage.h"
#include "StoryboardPage.h"

#include <QJsonArray>
#include <QMenuBar>
#include <QStackedWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SANKO TV"));
    setMinimumSize(1280, 720);

    setupMenuBar();

    m_stack = new QStackedWidget(this);
    m_dashboard = new DashboardPage;
    m_scriptEditor = new ScriptEditorPage;
    m_storyboard = new StoryboardPage;

    m_stack->addWidget(m_dashboard);     // index 0
    m_stack->addWidget(m_scriptEditor);  // index 1
    m_stack->addWidget(m_storyboard);    // index 2

    // Navigation between screens.
    connect(m_dashboard, &DashboardPage::newProjectRequested, this, [this] {
        m_stack->setCurrentWidget(m_scriptEditor);
    });
    connect(m_scriptEditor, &ScriptEditorPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_dashboard);
    });
    connect(m_scriptEditor, &ScriptEditorPage::continueRequested, this,
            [this](const QJsonArray &scenes) {
        m_storyboard->loadScenes(scenes);
        m_stack->setCurrentWidget(m_storyboard);
    });
    connect(m_storyboard, &StoryboardPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_scriptEditor);
    });

    setCentralWidget(m_stack);
}

void MainWindow::setupMenuBar()
{
    QMenuBar *bar = menuBar();
    bar->addMenu(QStringLiteral("File"));
    bar->addMenu(QStringLiteral("Edit"));
    bar->addMenu(QStringLiteral("View"));
}
