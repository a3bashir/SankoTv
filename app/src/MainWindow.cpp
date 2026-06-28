#include "MainWindow.h"

#include <QApplication>
#include <QFont>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <Qt>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("SANKO TV"));
    setMinimumSize(1280, 720);

    // Dark application background.
    setStyleSheet(QStringLiteral("QMainWindow { background-color: #0a0a0a; }"));

    setupMenuBar();
    setupCentralWidget();
}

void MainWindow::setupMenuBar()
{
    QMenuBar *bar = menuBar();
    bar->addMenu(QStringLiteral("File"));
    bar->addMenu(QStringLiteral("Edit"));
    bar->addMenu(QStringLiteral("View"));
}

void MainWindow::setupCentralWidget()
{
    QLabel *title = new QLabel(QStringLiteral("SANKO TV"), this);
    title->setAlignment(Qt::AlignCenter);

    QFont font = title->font();
    font.setPointSize(72);
    font.setBold(true);
    title->setFont(font);

    // White text on the dark background.
    title->setStyleSheet(QStringLiteral("color: #ffffff; background-color: #0a0a0a;"));

    setCentralWidget(title);
}
