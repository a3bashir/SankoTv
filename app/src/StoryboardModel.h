#pragma once

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>

// One AI-generated "take" of a panel: a single Seedance render attempt. A panel
// can accumulate many takes; the director selects the best one.
struct GeneratedTake
{
    QString id;                 // UUID
    QString videoPath;          // relative filename of this take's mp4
    QString promptUsed;         // the Claude-generated prompt for this take
    QString timestamp;          // when generated (QDateTime::toString)
    QString status;             // "Complete" or "Failed"
    double costEstimate = 0.0;  // e.g. 0.05
};

// One drawing layer inside a panel. QImage (not QPixmap) so strokes and
// compositing work at pixel level off the GPU.
struct Layer
{
    QString id;             // UUID
    QString name;           // "Layer 1", "Reference", ...
    QString type;           // "raster" (drawn) or "image" (imported)
    QImage image;           // ARGB32_Premultiplied, canvas-sized
    bool visible = true;
    double opacity = 1.0;   // 0.0 - 1.0
    bool locked = false;
};

// One undo step: a snapshot of a single layer's pixels. Keyed by layer id so
// undo still lands on the right layer after reorder / delete of other layers.
struct LayerUndoEntry
{
    QString layerId;
    QImage image;
};

// A fresh, fully transparent canvas-sized layer image.
inline QImage makeLayerImage()
{
    QImage img(960, 540, QImage::Format_ARGB32_Premultiplied); // 16:9 canvas
    img.fill(Qt::transparent);
    return img;
}

// A new raster layer with a fresh UUID and a transparent image.
inline Layer makeRasterLayer(const QString &name)
{
    Layer layer;
    layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    layer.name = name;
    layer.type = QStringLiteral("raster");
    layer.image = makeLayerImage();
    return layer;
}

// One storyboard panel: its layered drawing plus shot metadata and undo history.
struct Panel
{
    // Layer stack, index 0 = bottom-most. Drawing goes to the active layer.
    QVector<Layer> layers;
    int activeLayerIndex = 0;

    QString shotType = QStringLiteral("Medium");
    QString cameraAngle = QStringLiteral("Eye level");
    QString lens = QStringLiteral("Normal (35-50mm)");
    QString mood;
    QString notes;
    int duration = 3; // seconds on screen in the animatic
    QVector<LayerUndoEntry> undoStack; // capped to 20 snapshots by DrawingCanvas

    // AI video generation (Generation screen).
    QString generationStatus = QStringLiteral("Not Queued"); // Not Queued, Queued,
                                                             // Generating, Complete, Failed
    QString generatedVideoPath; // mirrors the SELECTED take's videoPath (Export/Save compat)
    QString falRequestId;       // fal.ai request id, for polling

    // Version tree: multiple generated takes; one is selected as the chosen shot.
    QVector<GeneratedTake> takes;
    QString selectedTakeId;     // id of the currently chosen take

    Layer *activeLayer()
    {
        if (activeLayerIndex < 0 || activeLayerIndex >= layers.size())
            return nullptr;
        return &layers[activeLayerIndex];
    }
    const Layer *activeLayer() const
    {
        if (activeLayerIndex < 0 || activeLayerIndex >= layers.size())
            return nullptr;
        return &layers.at(activeLayerIndex);
    }

    // Composite all VISIBLE layers bottom-to-top with per-layer opacity onto
    // white paper. This is the single merged view — thumbnails, onion skin,
    // Animatic, Generation, and Export all read this instead of raw pixels.
    QPixmap flattenedPixmap() const
    {
        QImage out(960, 540, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::white); // paper — keeps blank checks / ghosts / export identical
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const Layer &layer : layers) {
            if (!layer.visible || layer.image.isNull() || layer.opacity <= 0.0)
                continue;
            painter.setOpacity(qBound(0.0, layer.opacity, 1.0));
            painter.drawImage(0, 0, layer.image);
        }
        painter.end();
        return QPixmap::fromImage(out);
    }
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

// A fresh panel with a single blank raster layer ("Layer 1") on 960x540 (16:9).
inline Panel *makeBlankPanel()
{
    Panel *panel = new Panel;
    panel->layers.append(makeRasterLayer(QStringLiteral("Layer 1")));
    panel->activeLayerIndex = 0;
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
