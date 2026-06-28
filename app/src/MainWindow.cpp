#include "MainWindow.h"

#include "DashboardPage.h"
#include "ScriptEditorPage.h"

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

    m_stack->addWidget(m_dashboard);     // index 0
    m_stack->addWidget(m_scriptEditor);  // index 1

    // Navigation between the two screens.
    connect(m_dashboard, &DashboardPage::newProjectRequested, this, [this] {
        m_stack->setCurrentWidget(m_scriptEditor);
    });
    connect(m_scriptEditor, &ScriptEditorPage::backRequested, this, [this] {
        m_stack->setCurrentWidget(m_dashboard);
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
