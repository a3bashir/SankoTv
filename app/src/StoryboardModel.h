#pragma once

#include <QPixmap>
#include <QString>
#include <QVector>

// One storyboard panel: its drawing plus shot metadata and an undo history.
struct Panel
{
    QPixmap pixmap;
    QString shotType = QStringLiteral("Medium");
    QString cameraAngle = QStringLiteral("Eye level");
    QString lens = QStringLiteral("Normal (35-50mm)");
    QString mood;
    QString notes;
    QVector<QPixmap> undoStack; // capped to 20 snapshots by DrawingCanvas
};

// A scene carried over from the Script Editor, owning its panels.
struct Scene
{
    int number = 0;
    QString location;
    QString timeOfDay;
    QString action;
    QVector<Panel *> panels;

    ~Scene()
    {
        for (Panel *panel : panels)
            delete panel;
    }
};
