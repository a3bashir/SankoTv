#pragma once

#include <QColor>
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
    // Reuse-across-panels: instances of the SAME layer in different panels
    // carry the same non-empty sharedId; their pixel data is kept identical
    // (edits propagate to every instance, the project stores the image once
    // and references it by this id). Empty = a normal, panel-local layer.
    // Visibility/opacity/lock stay PER INSTANCE.
    QString sharedId;
    // Optional organisational colour label (Photoshop-style layer colour),
    // shown as an edge stripe in the Layers panel. Empty = none.
    QString colorTag;       // "#RRGGBB"
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
    // Undo/redo lives in the app-wide QUndoStack (MainWindow), not per panel.

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

// The locked white Background layer that sits beneath every drawing layer.
// Drawing layers stay transparent, so moving/selecting art never carries an
// opaque white fill — the paper comes from this layer instead. Marked type
// "background" (persisted) so migration is idempotent.
inline Layer makeBackgroundLayer()
{
    Layer layer;
    layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    layer.name = QStringLiteral("Background");
    layer.type = QStringLiteral("background");
    QImage img(960, 540, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    layer.image = img;
    layer.locked = true; // not drawn on during normal work
    return layer;
}

// True if the image looks like a LEGACY opaque white canvas (all four corners
// opaque and near-white) rather than a transparent art layer (transparent
// corners). Used to migrate old projects without touching real art layers.
inline bool looksLikeWhiteCanvas(const QImage &img)
{
    if (img.isNull() || img.width() < 2 || img.height() < 2)
        return false;
    const int w = img.width(), h = img.height();
    const QPoint corners[4] = {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}};
    for (const QPoint &p : corners) {
        const QColor c = img.pixelColor(p);
        if (c.alpha() < 250 || c.red() < 240 || c.green() < 240 || c.blue() < 240)
            return false;
    }
    return true;
}

// Migrate a legacy white-canvas raster layer to a transparent art layer:
// fully-opaque near-white pixels become transparent (the paper moves to the
// Background layer), while black/grey art and anti-aliased edges are kept.
inline void keyWhiteToTransparent(QImage &img)
{
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = line[x];
            if (qAlpha(c) == 255 && qRed(c) >= 250 && qGreen(c) >= 250 && qBlue(c) >= 250)
                line[x] = qRgba(0, 0, 0, 0);
        }
    }
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Ensure a panel has a locked white Background layer at the bottom and that
// drawing layers are transparent. Safe & idempotent:
//  - a panel that already has a Background layer is left untouched;
//  - legacy opaque-white RASTER canvases are keyed to transparency (image
//    layers are never keyed);
//  - the Background is inserted beneath everything and activeLayerIndex is
//    shifted so it keeps pointing at the same drawing layer.
inline void migratePanelToBackground(Panel *panel)
{
    if (!panel || panel->layers.isEmpty())
        return;
    if (panel->layers.first().type == QLatin1String("background"))
        return; // already migrated

    for (Layer &layer : panel->layers) {
        if (layer.type == QLatin1String("raster") && looksLikeWhiteCanvas(layer.image))
            keyWhiteToTransparent(layer.image);
    }

    panel->layers.prepend(makeBackgroundLayer());
    panel->activeLayerIndex += 1; // everything shifted up by one
    if (panel->activeLayerIndex <= 0 && panel->layers.size() > 1)
        panel->activeLayerIndex = 1; // never leave the Background active
}

// A fresh panel: a locked white Background plus one transparent drawing layer
// ("Layer 1"), which is the active layer.
inline Panel *makeBlankPanel()
{
    Panel *panel = new Panel;
    panel->layers.append(makeBackgroundLayer());                      // index 0 (locked white)
    panel->layers.append(makeRasterLayer(QStringLiteral("Layer 1"))); // index 1 (transparent)
    panel->activeLayerIndex = 1;                                      // draw on the transparent layer
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
