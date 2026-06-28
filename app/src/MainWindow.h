#pragma once

#include <QMainWindow>

class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

private:
    void setupMenuBar();
    void setupDashboard();

    QWidget *createHeaderBar();
    QWidget *createContentArea();
    QWidget *createProjectCard(const QString &name, const QString &modified);
};
