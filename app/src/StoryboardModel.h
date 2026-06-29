#pragma once

#include <QPixmap>
#include <QString>
#include <QStringList>
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
    int duration = 3; // seconds on screen in the animatic
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

// A fresh panel with a blank white 960x540 (16:9) canvas.
inline Panel *makeBlankPanel()
{
    Panel *panel = new Panel;
    panel->pixmap = QPixmap(960, 540);
    panel->pixmap.fill(Qt::white);
    return panel;
}

// A character or location reference, used by the Consistency Board to keep
// designs consistent across shots.
struct ConsistencyEntry
{
    QString id;          // UUID, generated on creation
    QString name;        // e.g. "Elena", "Warehouse Interior"
    QString type;        // "Character" or "Location"
    QString description; // costume, hair, distinguishing features
    QPixmap thumbnail;   // reference image, stored 320x180 (null = placeholder)
    QStringList tags;    // e.g. ["protagonist", "scene 1", "blue coat"]
};
