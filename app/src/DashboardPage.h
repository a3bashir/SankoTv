#pragma once

#include <QWidget>

class DashboardPage : public QWidget
{
    Q_OBJECT

public:
    explicit DashboardPage(QWidget *parent = nullptr);

signals:
    void newProjectRequested();

private:
    QWidget *createHeaderBar();
    QWidget *createContentArea();
    QWidget *createProjectCard(const QString &name, const QString &modified);
};
