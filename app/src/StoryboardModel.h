#pragma once

#include "LayerTiles.h"
#include "camera/CameraModel.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QMargins>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QUuid>
#include <QVector>

#include <cmath>   // std::floor
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

// THE PIXELS OF ONE DRAWING (batch I-2, 2026-11-26; Figma I-2 727:757, approved): what a
// Layer held as its own before a layer could hold a run of drawings. A Layer IS the pixels of
// the drawing it shows (Layer::shown) - it inherits these - so everything that reads or draws
// on `layer.image` works on that drawing, as it always has; the run's other drawings keep
// theirs in Layer::drawings.
struct LayerPixels
{
    // THE PIXELS (batch 6b-V, 2026-11-07). `image` is the full image, stage-sized,
    // ARGB32_Premultiplied: only the panel on the canvas is sure to have it, so
    // that every tool works as it always has. Any other panel's layer is at rest:
    // a null image and `tiles`, read at `stageRect` (LayerTiles.h). A layer can
    // hold both: then `tilesOf` is the cacheKey() of the image the tiles hold, and
    // an edit shows by the key moving on. Whatever reads ANOTHER panel's pixels
    // reads hasPixels, pixelSize, contentKey, pixels and paintTo below; only what
    // works on the panel on the canvas may read `image`. Folding: LayerFolder.
    QImage image;
    layertiles::TilesPtr tiles;
    QRect stageRect;        // frame space: where the stage lies, for `tiles`
    qint64 tilesOf = 0;

    bool atRest() const { return image.isNull() && tiles; }
    bool hasPixels() const { return !image.isNull() || tiles; }
    QSize pixelSize() const { return !image.isNull() ? image.size() : tiles ? stageRect.size() : QSize(); }
    // The tiles still say what the image says.
    bool tilesCurrent() const { return tiles && !image.isNull() && image.cacheKey() == tilesOf; }
    // THE IDENTITY OF THE PIXELS, what cacheKey() was before there were tiles: it
    // moves on with every edit and stays put through a fold and an unfold, so a
    // thumbnail, a mip and the save's ledger see the same pixels as the same.
    qint64 contentKey() const
    {
        if (image.isNull())
            return tiles ? tiles->key : 0;
        return tilesCurrent() ? tiles->key : image.cacheKey();
    }
    // NOTHING DRAWN (2026-11-26, the user's note 7 after hand test I2-1: a new, empty frame is
    // grey on the timeline until it is drawn on): every pixel clear. The tiles say it at once
    // - a fold keeps no tile that is all its fill - and a full image is read through to its
    // first drawn pixel, so whoever asks often keeps the answer by contentKey().
    bool blank() const
    {
        if (image.isNull() || tilesCurrent())
            return !tiles || (tiles->tiles.isEmpty() && (tiles->fill >> 24) == 0);
        if (image.depth() != 32)
            return false; // (never a layer's)
        for (int y = 0; y < image.height(); ++y) {
            const quint32 *line = reinterpret_cast<const quint32 *>(image.constScanLine(y));
            for (int x = 0, w = image.width(); x < w; ++x)
                if (line[x] >> 24)
                    return false;
        }
        return true;
    }
    // The same, where they are read: the same pixels on another stage are another
    // picture, and a margin change moves no pixel, so the identity stays put through
    // it (batch 6c). What a cache of the stage's image asks by.
    qint64 placedKey() const
    {
        return contentKey() ^ qint64(qHash(stageRect.topLeft(), qHash(pixelSize())));
    }
    // The stage's pixels as a full image: the layer's own, or one made for the
    // caller from the tiles. A copy of nothing when the layer holds the image.
    QImage pixels() const
    {
        if (!image.isNull() || !tiles)
            return image;
        return layertiles::unfold(*tiles, stageRect);
    }
    // The full image back on this layer.
    void unfoldNow()
    {
        if (atRest()) {
            image = layertiles::unfold(*tiles, stageRect);
            tilesOf = image.cacheKey();
        }
    }
    // Paints the stage's pixels with the painter's opacity, the stage's top-left
    // at the painter's origin (identity transform), as drawImage(0, 0, pixels()).
    void paintTo(QPainter &painter) const
    {
        if (!image.isNull())
            painter.drawImage(0, 0, image);
        else if (tiles)
            layertiles::paint(painter, *tiles, stageRect);
    }
};

// ONE DRAWING OF A LAYER'S RUN (batch I-2). `start`: the frame of the panel it is first
// shown at, counted at Layer::runRate frames per second - 0 for the first; it is shown until
// the next one starts, the last to the panel's end. `px`: its pixels, EMPTY for the drawing
// the layer shows (those are the layer's own).
struct LayerDrawing
{
    QString id;      // UUID: the name of its file (ProjectIO)
    int start = 0;
    LayerPixels px;
};

// One drawing layer inside a panel. QImage (not QPixmap) so strokes and
// compositing work at pixel level off the GPU.
struct Layer : LayerPixels
{
    QString id;             // UUID
    QString name;           // "Layer 1", "Reference", ...
    QString type;           // "raster" (drawn) or "image" (imported)
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
    // DEPTH (Phase 2B, the Phase 2 spec's 4.2): how far the layer moves
    // when the camera does, as a parallax. 1 = the picture plane (Phase 1's
    // behaviour, every older layer), 0 = fixed to the camera, 0.5 = half as
    // far, 1.5 = a near layer; 0 .. 2. A folder's value is its members';
    // a member's own is kept for when it leaves (Panel::effectiveParallax).
    double parallax = 1.0;
    // LAYER MOTION (batch I-1, 2026-11-24; Figma I-1 693:755). The layer's own keys and
    // whether they are used: the Animate diamond in the Layers panel. Switched off, the keys
    // are kept, unused (greyed on the LAYER row), until it is on again (the user's answer
    // b). `pivot`: the centre of the layer's drawn art when it first animated (answer 2),
    // in FRAME space - stage px less the panel's frameAt() - so a margin change leaves it on
    // the art. Copied with the layer: a duplicated panel or layer has the same move.
    bool animate = false;
    QVector<shotcam::LayerKey> keys;
    QPointF pivot;
    // The keys are in use: Animate on, at least one key, a layer that has pixels to move.
    bool moves() const
    {
        return animate && !keys.isEmpty() && type != QLatin1String("group")
            && type != QLatin1String("background");
    }
    // What the file and the version follow: a layer with motion of any kind kept.
    bool hasMotionData() const { return animate || !keys.isEmpty(); }

    // A RUN OF DRAWINGS (batch I-2, 2026-11-26; Figma I-2 727:757, approved). Empty: the
    // layer has one drawing, its own pixels, as every layer before I-2 (and no project
    // without a run changes). Otherwise two or more, in time order (LayerDrawing): the frames
    // they start at rise, the first one's is 0. `shown` is the drawing the layer's own pixels
    // are - the one under the playhead for the panel on the canvas, so every tool draws on
    // it (Panel::showDrawingsAtPlace); the others keep theirs in `px`. The starts are FRAMES,
    // counted at `runRate`: a panel made longer or shorter moves no drawing (the user's
    // answer 6: "the drawings keep their frames and the last one holds, while the keys
    // stretch"), and one past the panel's end is kept, unseen, until the panel is long
    // enough again. Copied with the layer, as its keys are.
    QVector<LayerDrawing> drawings;
    int shown = 0;
    int runRate = 24;
    bool hasRun() const { return drawings.size() > 1; }
    int drawingCount() const { return hasRun() ? int(drawings.size()) : 1; }
    int drawingStart(int i) const { return hasRun() && i >= 0 && i < drawings.size() ? drawings.at(i).start : 0; }
    // The drawing shown at `frame` (counted at runRate): the last that starts at or before it.
    int drawingAt(int frame) const
    {
        int at = 0;
        for (int i = 1; i < drawings.size() && drawings.at(i).start <= frame; ++i)
            at = i;
        return at;
    }
    int drawingIndexOf(const QString &drawingId) const
    {
        for (int i = 0; i < drawings.size(); ++i)
            if (drawings.at(i).id == drawingId)
                return i;
        return -1;
    }
    // The id of the drawing the layer shows; empty for a layer without a run.
    QString shownId() const { return hasRun() && shown >= 0 && shown < drawings.size() ? drawings.at(shown).id : QString(); }
    // The pixels of drawing `i`, wherever they are kept.
    const LayerPixels &pixelsOf(int i) const
    {
        return !hasRun() || i == shown || i < 0 || i >= drawings.size() ? static_cast<const LayerPixels &>(*this)
                                                                       : drawings.at(i).px;
    }
    LayerPixels &pixelsOf(int i)
    {
        return !hasRun() || i == shown || i < 0 || i >= drawings.size() ? static_cast<LayerPixels &>(*this)
                                                                       : drawings[i].px;
    }
    // This layer as drawing `i` alone - its pixels the layer's own, no run: for whatever
    // draws, measures or writes one drawing with the code that takes a layer. Shared pixels,
    // nothing copied.
    Layer asDrawing(int i) const
    {
        Layer one = *this;
        static_cast<LayerPixels &>(one) = pixelsOf(i);
        one.drawings.clear();
        one.shown = 0;
        return one;
    }
    // Drawing `i` onto the layer: the one shown goes back among the others. No pixel is
    // copied or changed. False when it is there already (or there is no such drawing).
    bool show(int i)
    {
        if (!hasRun() || i == shown || i < 0 || i >= drawings.size())
            return false;
        drawings[shown].px = static_cast<const LayerPixels &>(*this);
        static_cast<LayerPixels &>(*this) = drawings.at(i).px;
        drawings[i].px = LayerPixels();
        shown = i;
        return true;
    }
    // The starts counted at `rate` from now on, each drawing at its time to the nearest frame
    // and never on the frame of the one before it. What an edit does first, so that it works
    // in the project's frames; nothing for a run already at that rate.
    void countRunAt(int rate)
    {
        rate = rate < 1 ? 1 : rate;
        if (!hasRun() || rate == runRate) {
            runRate = hasRun() ? runRate : rate;
            return;
        }
        for (int i = 1; i < drawings.size(); ++i) {
            const int moved = int(std::floor(double(drawings.at(i).start) * rate / runRate + 0.5));
            drawings[i].start = moved > drawings.at(i - 1).start ? moved : drawings.at(i - 1).start + 1;
        }
        runRate = rate;
    }
    // Every drawing's pixels, the shown one's first: for whatever must reach all of them (a
    // fold, a margin, a resize).
    template <typename Visit>
    void forEachPixels(Visit visit)
    {
        visit(static_cast<LayerPixels &>(*this));
        if (hasRun())
            for (int i = 0; i < drawings.size(); ++i)
                if (i != shown)
                    visit(drawings[i].px);
    }
    template <typename Visit>
    void forEachPixels(Visit visit) const
    {
        visit(static_cast<const LayerPixels &>(*this));
        if (hasRun())
            for (int i = 0; i < drawings.size(); ++i)
                if (i != shown)
                    visit(drawings.at(i).px);
    }
};

// LAYER MOTION (batch I-1): a pose as a transform of stage px about `pivot` (stage px):
// sized and turned about it, then moved. Panel::motionOf, and the canvas's Move box.
inline QTransform layerPoseTransform(const QPointF &pivot, const shotcam::LayerPose &pose)
{
    QTransform t;
    t.translate(pivot.x() + pose.dx, pivot.y() + pose.dy);
    t.rotate(pose.rotation);
    t.scale(pose.scale, pose.scale);
    t.translate(-pivot.x(), -pivot.y());
    return t;
}

// The frame of a panel of `frames` a layer key sits on: its place, to the nearest frame, on
// one of the panel's frames (batch I-1). "A key at the playhead" is a key on its frame.
inline int layerKeyFrame(const shotcam::LayerKey &key, int frames)
{
    frames = frames < 1 ? 1 : frames;
    const int f = int(std::floor(key.at * frames + 0.5));
    return f < 0 ? 0 : (f > frames - 1 ? frames - 1 : f);
}

// The depth range and the picture plane (Phase 2B).
inline constexpr double kMinParallax = 0.0;
inline constexpr double kMaxParallax = 2.0;

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
    // THE PANEL'S LENGTH (2026-11-06, batch 5 of the proposals of
    // 2026-11-06; it was whole seconds, 1 to 30): `frames` counted at
    // `rate` frames per second - the project's rate when the length was
    // last set. Read it ONLY through shotcam::panelFrames(panel, fps),
    // which counts it at the rate asked for (rounded to the nearest frame,
    // at least 1). So a frame-rate change keeps every panel's TIME and
    // rewrites no panel, and whatever holds a panel or a length (the undo
    // history, the panel clipboard) stays right at any rate. At least 1
    // frame, no maximum. 72 at 24 is the old default, 3 s, for a panel
    // made without a length; the app's own new panels get 1 s
    // (StoryboardPage::makeProjectPanel, MainWindow::buildScenesFromJson).
    int frames = 72;
    int rate = 24;
    void setLength(int frameCount, int fps)
    {
        frames = frameCount < 1 ? 1 : frameCount;
        rate = fps < 1 ? 1 : fps;
    }
    // Undo/redo lives in the app-wide QUndoStack (MainWindow), not per panel.

    // THE PANEL'S STAGE MARGIN (2026-11-06, batch 6a of the proposals of
    // 2026-11-06): the px of stage around the shot's frame on each side -
    // this panel's own, where it was the project's alone (Phase 2A). The
    // layers are the frame plus this margin; pixels stay the truth for the
    // size (canvasSize()). Until Stage mode (batch 6c) can change one
    // panel's, every panel holds the project's margin: the loader gives it
    // from the pixels, a new panel the project's, a margin change every
    // panel the new one, and Duplicate and Paste the source's with its
    // pixels. Saved per panel as "margin": [left, top, right, bottom]
    // (omitted when 0, file version 2). stage() is the panel's stage.
    //
    // ITS OWN MARGIN (batch 6c, Stage mode): `ownMargin` when this panel's
    // differs from the project's, side by side if it likes. The camera's keys
    // stay in one space across the scene's panels - the stage px of the
    // project's margin, which is frame space plus that margin - so a move
    // between panels of different stages is smooth and every panel at the
    // project's margin reads them as it always has. `keyShift` is what this
    // panel adds to read a key on its own stage (shotcam::toPanelSpace), zero
    // unless ownMargin. The pages and the exports ask shotcam::panelStage.
    QMargins margin;
    bool ownMargin = false;
    QPoint keyShift;
    shotcam::Stage stage() const { return shotcam::stageWithMargins(canvasSize(), margin); }
    // A new margin and the stage size it makes: EVERY LAYER AT REST, then only its
    // rectangle, nothing to copy (batch 6b-V). What a full image holds that its
    // tiles do not yet is folded first. The caller reopens the panel on the canvas.
    void marginTo(const QMargins &newMargin, const QSize &newSize)
    {
        const QRect stageRect(QPoint(-newMargin.left(), -newMargin.top()), newSize);
        for (Layer &layer : layers) {
            if (layer.type == QLatin1String("group"))
                continue;
            const bool paper = layer.type == QLatin1String("background");
            const QPoint at = frameAt();
            // Every drawing of a run (batch I-2), each as the layer's own pixels always were.
            layer.forEachPixels([&](LayerPixels &px) {
                if (!px.image.isNull() && !px.tilesCurrent()) {
                    const QImage image = layertiles::foldable(px.image)
                        ? px.image
                        : px.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    const quint32 fill = paper ? layertiles::paperFill(image, px.tiles) : 0u;
                    px.tiles = layertiles::fold(image, at, fill, px.tiles, px.image.cacheKey());
                }
                px.image = QImage();
                px.tilesOf = 0;
                if (px.tiles) // a drawing that holds nothing yet stays nothing
                    px.stageRect = stageRect;
            });
            layer.stageRect = stageRect;
        }
        margin = newMargin;
    }
    // Whether this panel holds the project's margin or its own, after the
    // project's or the panel's changed.
    void settleMargin(const QSize &projectMargin)
    {
        const QMargins project(projectMargin.width(), projectMargin.height(), projectMargin.width(),
                               projectMargin.height());
        ownMargin = margin != project;
        keyShift = ownMargin
            ? QPoint(margin.left() - projectMargin.width(), margin.top() - projectMargin.height())
            : QPoint();
    }
    // The stage this panel would have at the project's margin: what the keys'
    // space measures. Its own size unless it has its own margin.
    QSize cameraCanvas() const
    {
        const QSize size = canvasSize();
        if (!ownMargin)
            return size;
        const QSize frame = stage().frame;
        return QSize(frame.width() + 2 * (margin.left() - keyShift.x()),
                     frame.height() + 2 * (margin.top() - keyShift.y()));
    }

    // The shot camera's name for this panel (camera/CameraModel.h): a key
    // is anchored to it. Minted per object, never copied, never saved.
    shotcam::PanelUid uid;
    // THE PANEL'S STABLE ID (2026-11-04, batch 2 of the proposals of
    // 2026-11-02): a UUID minted when the panel is made, saved in the
    // manifest as "id", and the name of its image files
    // (p_<id12>.png, p_<id12>_l_<layer id12>.png - ProjectIO), so an
    // insert, move or delete changes the manifest and no file. A clone
    // (StoryboardPage::clonePanel) is a new Panel: its own id. A loaded
    // panel without one is given one and saved with it.
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // AI video generation. KEPT DATA WITH NO UI: the Generation screen that
    // wrote these was removed (HANDOFF "Pass 3"), and nothing in the app
    // reads or shows them now. ProjectIO still loads and saves every one,
    // exactly as saved, so a project that carries takes keeps them -
    // Lifecycle (am) is the only thing that will notice if that stops
    // being true.
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

    // THE FOLDER RULE (Phase 2B, the user's decision D11), its one place: a
    // member uses its folder's depth (its own is kept, unused); the paper
    // is always the picture plane.
    double effectiveParallax(const Layer &layer) const
    {
        if (layer.type == QLatin1String("background"))
            return 1.0;
        const Layer *group = groupOf(layer);
        return qBound(kMinParallax, group ? group->parallax : layer.parallax, kMaxParallax);
    }
    // A layer that is drawn: not a folder, has pixels, visible and not clear. A layer that
    // moves (batch I-1) has its opacity in its keys, so its slider's value does not hide it.
    bool layerDrawn(const Layer &layer) const
    {
        return !isGroupLayer(layer) && layer.hasPixels() && layerEffectivelyVisible(layer)
            && (layer.moves() || layerEffectiveOpacity(layer) > 0.0);
    }

    // --- LAYER MOTION (batch I-1) ------------------------------------------------------
    // THE LAYER'S MOTION FIRST, THEN ITS DEPTH AND THE CAMERA (the user's words): every
    // consumer that draws a panel at a time - the canvas, the preview, the SHOTS clips, the
    // strip, the MP4 and the stills - poses a moving layer with motionOf(layerPoseAt) in
    // stage px, and only then places it for its depth (shotcam::layerToStage) and the
    // camera. `at` is the place in the panel: 0 its first frame, 1 its end.
    bool layerMoves(const Layer &layer) const { return layer.moves() && layerDrawn(layer); }
    bool hasMotion() const
    {
        for (const Layer &layer : layers)
            if (layerMoves(layer))
                return true;
        return false;
    }
    shotcam::LayerPose layerPoseAt(const Layer &layer, double at) const
    {
        if (!layer.moves()) {
            shotcam::LayerPose rest;
            rest.opacity = layer.opacity;
            return rest;
        }
        return shotcam::poseFromKeys(layer.keys, at, layer.opacity);
    }
    QPointF pivotOnStage(const Layer &layer) const { return layer.pivot + QPointF(frameAt()); }
    // Stage px to stage px: sized and turned about the pivot, then moved.
    QTransform motionOf(const Layer &layer, const shotcam::LayerPose &pose) const
    {
        return layerPoseTransform(pivotOnStage(layer), pose);
    }
    QTransform motionAt(const Layer &layer, double at) const
    {
        return layer.moves() ? motionOf(layer, layerPoseAt(layer, at)) : QTransform();
    }
    // What it is drawn with at `at`: its keyed opacity when it moves, times its folder's.
    double layerOpacityAt(const Layer &layer, double at) const
    {
        if (!layer.moves())
            return layerEffectiveOpacity(layer);
        const double own = qBound(0.0, layerPoseAt(layer, at).opacity, 1.0);
        const Layer *group = groupOf(layer);
        return group ? own * qBound(0.0, group->opacity, 1.0) : own;
    }

    // --- DRAWINGS (batch I-2) ----------------------------------------------------------
    // A layer with a run shows, at every frame, the drawing exposed there. Every consumer
    // that draws a panel at a time asks drawnAtPlace(layer, at) for the layer to draw - the
    // layer itself when it has no run, so a project without one draws exactly as before.
    // `at` is the place in the panel, as for the motion: 0 its first frame, 1 its end.
    bool layerHasRun(const Layer &layer) const { return layer.hasRun() && !isGroupLayer(layer); }
    bool hasRuns() const
    {
        for (const Layer &layer : layers)
            if (layerHasRun(layer))
                return true;
        return false;
    }
    // The frame of the layer's run (counted at its runRate) at the place `at`.
    int runFrameAtPlace(const Layer &layer, double at) const
    {
        const double f = at * frames * double(layer.runRate < 1 ? 1 : layer.runRate) / double(rate < 1 ? 1 : rate);
        return f <= 0.0 ? 0 : int(std::floor(f + 1e-6));
    }
    int drawingAtPlace(const Layer &layer, double at) const
    {
        return layerHasRun(layer) ? layer.drawingAt(runFrameAtPlace(layer, at)) : 0;
    }
    // A run with a drawing that starts inside the panel: its picture changes over its time.
    bool runChangesIn(const Layer &layer) const
    {
        if (!layerHasRun(layer) || !layerEffectivelyVisible(layer))
            return false;
        return layer.drawings.at(1).start
            < double(frames) * double(layer.runRate < 1 ? 1 : layer.runRate) / double(rate < 1 ? 1 : rate);
    }
    bool hasLiveRuns() const
    {
        for (const Layer &layer : layers)
            if (runChangesIn(layer))
                return true;
        return false;
    }
    // The picture is not one picture: a layer moves, or a run changes drawing.
    bool changesOverTime() const { return hasMotion() || hasLiveRuns(); }
    // On every layer with a run, the drawing exposed at `at` (the canvas draws on what a
    // layer shows). True when any layer changed drawing; no pixel is copied.
    bool showDrawingsAtPlace(double at)
    {
        bool changed = false;
        for (Layer &layer : layers)
            if (layerHasRun(layer))
                changed = layer.show(layer.drawingAt(runFrameAtPlace(layer, at))) || changed;
        return changed;
    }
    // True when any drawn layer is off the picture plane: only then does
    // the camera place the layers one by one (shotcam's layered path).
    bool hasDepth() const
    {
        for (const Layer &layer : layers)
            if (layerDrawn(layer) && effectiveParallax(layer) != 1.0)
                return true;
        return false;
    }
    int depthLayerCount() const
    {
        int n = 0;
        for (const Layer &layer : layers)
            if (layerDrawn(layer) && effectiveParallax(layer) != 1.0)
                ++n;
        return n;
    }

    // The panel's canvas size — THE runtime size authority. Pixels are the
    // truth: the size is whatever the layers actually are (the manifest
    // shaped them at load/create). Derived from the first layer owning an
    // image, because group folders deliberately own none. Invalid when the
    // panel has no image layers, which no creation or load path produces.
    QSize canvasSize() const
    {
        for (const Layer &layer : layers)
            if (layer.hasPixels())
                return layer.pixelSize();
        return QSize();
    }

    // Where the frame's top-left is in this panel's full images.
    QPoint frameAt() const { return QPoint(margin.left(), margin.top()); }
    // The full images back, one after another (LayerFolder::open does it in
    // parallel, and this is then nothing). The panel on the canvas has them.
    void unfoldNow()
    {
        for (Layer &layer : layers)
            layer.unfoldNow();
    }

    // Composite all VISIBLE layers bottom-to-top with per-layer opacity onto
    // white paper. This is the single merged view — thumbnails, onion skin,
    // the animatic preview, and Export all read this instead of raw pixels.
    // Group folders paint nothing themselves; members composite with the
    // folder's visibility/opacity applied. Sized from the panel's OWN
    // layers, so it can never crop a document to some other size.
    // `at` (2026-11-26, the user's answer 2 to hand test I2-1): the place in the panel the
    // picture is of - 0, the first frame, for everyone who shows a panel as one picture; the
    // onion skin asks for the previous panel's LAST frame (what was on screen just before the
    // cut): each run's drawing there, each moving layer's pose there.
    QPixmap flattenedPixmap(double at = 0.0) const
    {
        const QImage out = flattenedImage(at);
        return out.isNull() ? QPixmap() : QPixmap::fromImage(out);
    }
    // The same composite as an image - usable on a worker thread (a QPixmap
    // belongs to the UI thread), for the strip's parallel thumbnails.
    QImage flattenedImage(double at = 0.0) const
    {
        const QSize size = canvasSize();
        if (!size.isValid())
            return QImage();
        QImage out(size, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::white); // paper — keeps blank checks / ghosts / export identical
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const Layer &stored : layers) {
            if (stored.type == QLatin1String("group"))
                continue;
            // A run of drawings (batch I-2): its FIRST drawing, whichever the layer shows -
            // the flatten is the panel's first frame, as for a layer that moves. (At another
            // place: the drawing exposed there.)
            Layer first;
            if (layerHasRun(stored))
                first = stored.asDrawing(drawingAtPlace(stored, at));
            const Layer &layer = layerHasRun(stored) ? first : stored;
            if (!layerEffectivelyVisible(layer) || !layer.hasPixels())
                continue;
            // A layer that moves (batch I-1): AT THE PANEL'S FIRST FRAME (the user's answer
            // 8: the strip's thumbnails and the SHOTS clips show the first frame), its
            // whole image through its pose - never tile by tile, which would seam when
            // turned or sized.
            if (layerMoves(layer)) {
                const double posed = layerOpacityAt(layer, at);
                if (posed <= 0.0)
                    continue;
                painter.save();
                painter.setOpacity(posed);
                painter.setTransform(motionAt(layer, at));
                painter.drawImage(0, 0, layer.pixels());
                painter.restore();
                continue;
            }
            const double opacity = layerEffectiveOpacity(layer);
            if (opacity <= 0.0)
                continue;
            painter.setOpacity(opacity);
            layer.paintTo(painter);
        }
        painter.end();
        return out;
    }
    // Everything of a layer's motion that shapes a picture (batch I-1), for the
    // fingerprints: nothing for a layer without motion, so theirs read as before.
    static void appendMotion(QVector<quint64> &fp, const Layer &layer)
    {
        if (!layer.hasMotionData())
            return;
        auto bits = [](double v) {
            quint64 b = 0;
            memcpy(&b, &v, sizeof(b));
            return b;
        };
        fp.append(0x4d4f54494f4eull ^ quint64(layer.animate)); // "MOTION"
        fp.append(bits(layer.pivot.x()));
        fp.append(bits(layer.pivot.y()));
        for (const shotcam::LayerKey &k : layer.keys)
            fp << bits(k.at) << bits(k.pose.dx) << bits(k.pose.dy) << bits(k.pose.rotation)
               << bits(k.pose.scale) << bits(k.pose.opacity) << quint64(k.ease);
    }

    // --- Flattened-THUMBNAIL cache (performance pass 3b) --------------------
    // The hot consumers of the flatten (timeline clip thumbs, panel-strip
    // thumbs) all want SMALL images, yet each call
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
            // A run (batch I-2): its first drawing's pixels are the flatten's, whichever the
            // layer shows. A layer without one: its own, as before.
            fp.append(quint64(layer.pixelsOf(0).contentKey()));
            quint64 opacityBits = 0;
            static_assert(sizeof(opacityBits) == sizeof(layer.opacity),
                          "opacity bits must fit the fingerprint word");
            memcpy(&opacityBits, &layer.opacity, sizeof(opacityBits));
            fp.append(opacityBits);
            fp.append(quint64(layer.visible)
                      | (quint64(isGroupLayer(layer)) << 1)
                      | (quint64(qHash(layer.groupId)) << 2));
            appendMotion(fp, layer); // batch I-1: a key moved is another picture
        }
        // The stage they are read at (batch 6c): a margin moves no pixel, and the
        // flatten is another picture.
        const QSize stageSize = canvasSize();
        fp.append((quint64(quint32(stageSize.width())) << 32) | quint32(stageSize.height()));
        fp.append((quint64(quint32(margin.left())) << 32) | quint32(margin.top()));
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
    // THE STRIP IN PARALLEL (2026-10-31): the open builds every stale strip
    // thumbnail on worker threads. flattenedThumbImage is flattenedThumb's
    // composite and scale in images (worker-safe), with the fingerprint it
    // was built from; adoptThumb (UI thread) puts it in the cache only if
    // the panel still matches that fingerprint.
    bool thumbFresh() const
    {
        return !m_thumbCache.isNull() && flattenFingerprint() == m_thumbKey;
    }
    QImage flattenedThumbImage(QVector<quint64> *fingerprint) const
    {
        constexpr int kThumbLongEdge = 512; // as flattenedThumb()
        *fingerprint = flattenFingerprint();
        const QImage full = flattenedImage();
        return (full.width() > kThumbLongEdge || full.height() > kThumbLongEdge)
            ? full.scaled(kThumbLongEdge, kThumbLongEdge, Qt::KeepAspectRatio,
                          Qt::SmoothTransformation)
            : full;
    }
    void adoptThumb(const QImage &thumb, const QVector<quint64> &fingerprint) const
    {
        if (thumb.isNull() || fingerprint != flattenFingerprint())
            return;
        m_thumbCache = QPixmap::fromImage(thumb);
        m_thumbKey = fingerprint;
    }
    // THE FRAME'S THUMBNAIL (Phase 2A, the stage margin): the SHOTS clip of
    // the base framing is the frame cut out of the flatten and reduced as
    // flattenedThumb() reduces a whole panel, so a margin project's clip is
    // the picture a frame-sized panel's is. Validated the same way, plus
    // the rectangle.
    QPixmap flattenedFrameThumb(const QRect &frame) const
    {
        constexpr int kThumbLongEdge = 512; // as flattenedThumb()
        QVector<quint64> fp = flattenFingerprint();
        if (m_frameThumbCache.isNull() || fp != m_frameThumbKey
            || frame != m_frameThumbRect) {
            const QPixmap full = flattenedPixmap().copy(frame);
            m_frameThumbCache = (full.width() > kThumbLongEdge
                                 || full.height() > kThumbLongEdge)
                ? full.scaled(kThumbLongEdge, kThumbLongEdge,
                              Qt::KeepAspectRatio, Qt::SmoothTransformation)
                : full;
            m_frameThumbKey = fp;
            m_frameThumbRect = frame;
        }
        return m_frameThumbCache;
    }

private:
    mutable QPixmap m_thumbCache;
    mutable QVector<quint64> m_thumbKey;
    mutable QPixmap m_frameThumbCache;
    mutable QVector<quint64> m_frameThumbKey;
    mutable QRect m_frameThumbRect;
};

// A scene carried over from the Script Editor, owning its panels.
struct Scene
{
    int number = 0;
    QString location;
    QString timeOfDay;
    QString action;
    QVector<Panel *> panels;
    // THE SHOT CAMERA (Phase 1A, 2026-10-25): one per scene, keys anchored
    // to panels (camera/ShotCamera.h evaluates it). Empty = the neutral
    // camera, the whole canvas: the shot every project had before.
    shotcam::SceneCamera camera;
    // THE SCENE LENGTH LOCK (batch F1, 2026-11-12; Figma F-1): its length - the sum
    // of its panels - holds. Inside it a new panel splits the one it follows, a
    // deleted panel's time goes to the one before, an edge rolls, a typed duration
    // takes from its neighbour, and its camera keys keep their film frames.
    // Locking is an undo step; saved as "lengthLocked" (file version 5).
    bool lengthLocked = false;
    // THE SCENE'S COLOUR (batch E, 2026-11-15; Figma E-1): "#RRGGBB" once chosen; empty is
    // Auto - the colour of the muted set at the scene's place (SceneColors.h). Shown on the
    // SCENES row, along its clips and on its card; never in the picture. Saved as "colorTag"
    // only when chosen.
    QString colorTag;

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
    // A panel's own margin (batch 6c) is no other size: the frame is what must agree.
    const QSize source = clipboard->cameraCanvas();
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
