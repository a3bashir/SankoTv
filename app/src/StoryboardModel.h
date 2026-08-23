#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>

#include <cstring> // memcpy in Panel::flattenFingerprint

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
    // Optional organisational colour label (Photoshop-style layer colour),
    // shown as an edge stripe in the Layers panel. Empty = none.
    QString colorTag;       // "#RRGGBB"
    // Layer groups (folders). A group is a Layer entry with type "group"
    // (null image, never drawn on); its members carry groupId == the group
    // entry's id and sit CONTIGUOUSLY directly beneath it in the vector, so
    // the block moves/duplicates/deletes as one. One level deep (groups
    // cannot nest). groupExpanded is UI state on the group entry only.
    QString groupId;        // member -> owning group's id; empty = root
    bool groupExpanded = true;
};

// True for a group (folder) entry — a UI/organisation row, never a paint
// target.
inline bool isGroupLayer(const Layer &layer)
{
    return layer.type == QLatin1String("group");
}

// A fresh, fully transparent layer image at the PROJECT'S canvas size.
// There is no default size: the caller must know what document it is
// creating pixels for — a silent 960x540 here is exactly the class of
// defect the resolution epic removed.
inline QImage makeLayerImage(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// A new raster layer with a fresh UUID and a transparent image.
inline Layer makeRasterLayer(const QString &name, const QSize &size)
{
    Layer layer;
    layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    layer.name = name;
    layer.type = QStringLiteral("raster");
    layer.image = makeLayerImage(size);
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

    // The group entry owning `layer`, or null (root layer / group row).
    const Layer *groupOf(const Layer &layer) const
    {
        if (layer.groupId.isEmpty())
            return nullptr;
        for (const Layer &candidate : layers)
            if (candidate.id == layer.groupId
                && candidate.type == QLatin1String("group"))
                return &candidate;
        return nullptr;
    }

    // Group-aware render state: a member inherits its folder's visibility
    // (both eyes must be on) and multiplies by the folder's opacity.
    bool layerEffectivelyVisible(const Layer &layer) const
    {
        if (!layer.visible)
            return false;
        const Layer *group = groupOf(layer);
        return !group || group->visible;
    }
    double layerEffectiveOpacity(const Layer &layer) const
    {
        const double own = qBound(0.0, layer.opacity, 1.0);
        const Layer *group = groupOf(layer);
        return group ? own * qBound(0.0, group->opacity, 1.0) : own;
    }

    // The panel's canvas size — THE runtime size authority. Pixels are the
    // truth: the size is whatever the layers actually are (the manifest
    // shaped them at load/create). Derived from the first layer owning an
    // image, because group folders deliberately own none. Invalid when the
    // panel has no image layers, which no creation or load path produces.
    QSize canvasSize() const
    {
        for (const Layer &layer : layers)
            if (!layer.image.isNull())
                return layer.image.size();
        return QSize();
    }

    // Composite all VISIBLE layers bottom-to-top with per-layer opacity onto
    // white paper. This is the single merged view — thumbnails, onion skin,
    // Animatic, Generation, and Export all read this instead of raw pixels.
    // Group folders paint nothing themselves; members composite with the
    // folder's visibility/opacity applied. Sized from the panel's OWN
    // layers, so it can never crop a document to some other size.
    QPixmap flattenedPixmap() const
    {
        const QSize size = canvasSize();
        if (!size.isValid())
            return QPixmap();
        QImage out(size, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::white); // paper — keeps blank checks / ghosts / export identical
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const Layer &layer : layers) {
            if (layer.type == QLatin1String("group"))
                continue;
            if (!layerEffectivelyVisible(layer) || layer.image.isNull())
                continue;
            const double opacity = layerEffectiveOpacity(layer);
            if (opacity <= 0.0)
                continue;
            painter.setOpacity(opacity);
            painter.drawImage(0, 0, layer.image);
        }
        painter.end();
        return QPixmap::fromImage(out);
    }

    // --- Flattened-THUMBNAIL cache (performance pass 3b) --------------------
    // The hot consumers of the flatten (timeline clip thumbs, panel-strip
    // thumbs, generation row thumbs) all want SMALL images, yet each call
    // paid a full-resolution composite: 16 ms per panel at 3840x2160, per
    // clip, per timeline repaint. This caches one ~512 px long-edge mip per
    // panel (~0.6 MB at 4K) and VALIDATES it on every read instead of
    // relying on invalidation calls: a fingerprint of everything that shapes
    // the flatten — layer order/count, each layer's QImage::cacheKey(), and
    // the visibility/opacity/type/group fields — is recomputed per read
    // (measured 0.11 us/panel) and the mip is rebuilt only when it differs.
    // cacheKey() changes on every detach, and every mutation path in the app
    // (engine publish, undo/redo restore, clear, merge, transform commit,
    // import, load) either assigns a new QImage or paints through
    // QPainter/bits(), both of which detach — so NO call site is trusted to
    // remember to invalidate; the cache proves its own freshness. The only
    // way to go stale is mutating pixels behind a const_cast of constBits(),
    // which nothing does. Full-resolution consumers (save, export, payloads,
    // onion skin, light table, playback display) keep calling
    // flattenedPixmap(), which stays uncached: one 4K flatten is 33 MB, and
    // caching it per panel would cost gigabytes per scene.
    QVector<quint64> flattenFingerprint() const
    {
        QVector<quint64> fp;
        fp.reserve(layers.size() * 3);
        for (const Layer &layer : layers) {
            fp.append(quint64(layer.image.cacheKey()));
            quint64 opacityBits = 0;
            static_assert(sizeof(opacityBits) == sizeof(layer.opacity),
                          "opacity bits must fit the fingerprint word");
            memcpy(&opacityBits, &layer.opacity, sizeof(opacityBits));
            fp.append(opacityBits);
            fp.append(quint64(layer.visible)
                      | (quint64(isGroupLayer(layer)) << 1)
                      | (quint64(qHash(layer.groupId)) << 2));
        }
        return fp;
    }
    QPixmap flattenedThumb() const
    {
        constexpr int kThumbLongEdge = 512; // strip wells reach ~480 device px
        QVector<quint64> fp = flattenFingerprint();
        if (m_thumbCache.isNull() || fp != m_thumbKey) {
            const QPixmap full = flattenedPixmap();
            m_thumbCache = (full.width() > kThumbLongEdge
                            || full.height() > kThumbLongEdge)
                ? full.scaled(kThumbLongEdge, kThumbLongEdge,
                              Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : full; // small canvases are already thumb-sized: never upscale
            m_thumbKey = fp;
        }
        return m_thumbCache;
    }

private:
    mutable QPixmap m_thumbCache;
    mutable QVector<quint64> m_thumbKey;
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
inline Layer makeBackgroundLayer(const QSize &size)
{
    Layer layer;
    layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    layer.name = QStringLiteral("Background");
    layer.type = QStringLiteral("background");
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
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

    // The inserted paper matches the panel's OWN pixels (pixels are the
    // authority) — never a fixed size into a stack of another size.
    const QSize size = panel->canvasSize();
    if (!size.isValid())
        return; // no image layers: nothing to put paper behind
    panel->layers.prepend(makeBackgroundLayer(size));
    panel->activeLayerIndex += 1; // everything shifted up by one
    if (panel->activeLayerIndex <= 0 && panel->layers.size() > 1)
        panel->activeLayerIndex = 1; // never leave the Background active
}

// The user-facing text shown when a project's manifest disagrees with its
// artwork's real pixel size (written by the stored-not-applied era). ONE
// place, so the load path and the verification seam assert the same string.
// Policy: pixels win, artwork is never rescaled/cropped, the manifest is
// corrected on the next save — and the user is always told.
inline QString canvasMismatchDialogText(const QSize &manifest,
                                        const QSize &pixels)
{
    return QStringLiteral(
               "This project's file says %1 \xC3\x97 %2, but its artwork is "
               "%3 \xC3\x97 %4.\n\nOpening at %3 \xC3\x97 %4. Your artwork "
               "is not modified; the file will be corrected on the next "
               "save.")
        .arg(manifest.width())
        .arg(manifest.height())
        .arg(pixels.width())
        .arg(pixels.height());
}

// Would pasting `clipboard` into a project of `projectSize` leave that
// project holding panels of two different sizes? THE decision behind the
// paste refusal, kept here — free of any widget — so the gate can assert
// the real function on real panels instead of a restatement of it.
// False for a null clipboard and for anything whose size cannot be read:
// a refusal must be certain, never a guess.
inline bool pasteWouldMixSizes(const Panel *clipboard, const QSize &projectSize)
{
    if (!clipboard)
        return false;
    const QSize source = clipboard->canvasSize();
    return source.isValid() && projectSize.isValid() && source != projectSize;
}

// A fresh panel: a locked white Background plus one transparent drawing layer
// ("Layer 1"), which is the active layer.
inline Panel *makeBlankPanel(const QSize &size)
{
    Panel *panel = new Panel;
    panel->layers.append(makeBackgroundLayer(size));                  // index 0 (locked white)
    panel->layers.append(
        makeRasterLayer(QStringLiteral("Layer 1"), size));            // index 1 (transparent)
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
