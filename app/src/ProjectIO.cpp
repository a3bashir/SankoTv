#include "ProjectIO.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>

#include <algorithm>

namespace ProjectIO {

QString assetSubdirFor(const QString &projectFilePath)
{
    // Named from the FILE, never from data.projectName. A filename is
    // unique within its folder by construction; a project NAME is not —
    // Test_SB_006.sankotv carries projectName "Test_SB_007", and keying off
    // the name would put two projects' pixels back in one directory, which
    // is precisely the bug this exists to end.
    return QFileInfo(projectFilePath).completeBaseName()
        + QStringLiteral("_assets");
}

QJsonObject projectToJson(const SaveData &data, const QString &projectFilePath)
{
    // Pixels live in a subfolder of their own, so two projects saved into
    // one directory can never write the same files. Panels and layers are
    // named by POSITION (panel_s0_p0_layer0.png), carrying nothing that
    // identifies the project, so the directory was the only thing keeping
    // them apart — and Save As happily put two in one.
    //
    // The manifest stores each name RELATIVE to itself, so loading is
    // unchanged: an old project names flat files and finds them exactly
    // where it always did.
    const QString folder = QFileInfo(projectFilePath).absolutePath();
    const QString assets = assetSubdirFor(projectFilePath);
    QDir().mkpath(folder + QStringLiteral("/") + assets);
    // Every stored name gets this prefix; every write goes through it.
    const QString rel = assets + QStringLiteral("/");

    QJsonArray scenesArray;
    for (int i = 0; i < data.scenes.size(); ++i) {
        Scene *scene = data.scenes.at(i);

        QJsonObject sceneObj;
        sceneObj[QStringLiteral("name")] = QStringLiteral("Scene %1").arg(scene->number);
        sceneObj[QStringLiteral("number")] = scene->number;     // preserved (not lost)
        sceneObj[QStringLiteral("location")] = scene->location;
        sceneObj[QStringLiteral("timeOfDay")] = scene->timeOfDay; // preserved
        sceneObj[QStringLiteral("action")] = scene->action;

        QJsonArray panelsArray;
        for (int j = 0; j < scene->panels.size(); ++j) {
            Panel *panel = scene->panels.at(j);

            // Flattened composite — kept for forward-compat (older builds and any
            // external tool reading pixmapFile still see the merged drawing).
            // flattenedPixmap() is panel-sized, so this PNG matches the layers.
            const QString pngName =
                rel + QStringLiteral("panel_s%1_p%2.png").arg(i).arg(j);
            panel->flattenedPixmap().save(folder + QStringLiteral("/") + pngName, "PNG");

            // Layer stack: one PNG per layer + a JSON descriptor array.
            QJsonArray layersArray;
            for (int k = 0; k < panel->layers.size(); ++k) {
                const Layer &layer = panel->layers.at(k);

                QJsonObject layerObj;
                layerObj[QStringLiteral("id")] = layer.id;
                layerObj[QStringLiteral("name")] = layer.name;
                layerObj[QStringLiteral("type")] = layer.type;
                layerObj[QStringLiteral("visible")] = layer.visible;
                layerObj[QStringLiteral("opacity")] = layer.opacity;
                layerObj[QStringLiteral("locked")] = layer.locked;
                if (!layer.colorTag.isEmpty())
                    layerObj[QStringLiteral("colorTag")] = layer.colorTag;
                // Layer groups (folders): membership + UI expand state.
                if (!layer.groupId.isEmpty())
                    layerObj[QStringLiteral("groupId")] = layer.groupId;
                if (layer.type == QLatin1String("group"))
                    layerObj[QStringLiteral("groupExpanded")] = layer.groupExpanded;

                if (layer.type != QLatin1String("group")) { // folders own no pixels
                    const QString layerPng =
                        rel + QStringLiteral("panel_s%1_p%2_layer%3.png")
                                  .arg(i).arg(j).arg(k);
                    layer.image.save(folder + QStringLiteral("/") + layerPng, "PNG");
                    layerObj[QStringLiteral("imageFile")] = layerPng;
                }
                layersArray.append(layerObj);
            }

            QJsonObject panelObj;
            panelObj[QStringLiteral("duration")] = panel->duration;
            panelObj[QStringLiteral("shotType")] = panel->shotType;
            panelObj[QStringLiteral("camera")] = panel->cameraAngle;
            panelObj[QStringLiteral("lens")] = panel->lens;
            panelObj[QStringLiteral("mood")] = panel->mood;
            panelObj[QStringLiteral("notes")] = panel->notes;
            panelObj[QStringLiteral("pixmapFile")] = pngName;
            panelObj[QStringLiteral("layers")] = layersArray;
            panelObj[QStringLiteral("activeLayerIndex")] = panel->activeLayerIndex;
            panelObj[QStringLiteral("generationStatus")] = panel->generationStatus;
            panelObj[QStringLiteral("generatedVideoPath")] = panel->generatedVideoPath;
            panelObj[QStringLiteral("falRequestId")] = panel->falRequestId;

            // Version tree: all generated takes + which one is selected.
            QJsonArray takesArray;
            for (const GeneratedTake &take : panel->takes) {
                QJsonObject takeObj;
                takeObj[QStringLiteral("id")] = take.id;
                takeObj[QStringLiteral("videoPath")] = take.videoPath; // relative filename
                takeObj[QStringLiteral("promptUsed")] = take.promptUsed;
                takeObj[QStringLiteral("timestamp")] = take.timestamp;
                takeObj[QStringLiteral("status")] = take.status;
                takeObj[QStringLiteral("costEstimate")] = take.costEstimate;
                takesArray.append(takeObj);
            }
            panelObj[QStringLiteral("takes")] = takesArray;
            panelObj[QStringLiteral("selectedTakeId")] = panel->selectedTakeId;
            panelsArray.append(panelObj);
        }
        sceneObj[QStringLiteral("panels")] = panelsArray;
        scenesArray.append(sceneObj);
    }

    // Consistency board entries + their thumbnail PNGs.
    QJsonArray consistencyArray;
    for (const ConsistencyEntry &entry : data.consistency) {
        QJsonObject entryObj;
        entryObj[QStringLiteral("id")] = entry.id;
        entryObj[QStringLiteral("name")] = entry.name;
        entryObj[QStringLiteral("type")] = entry.type;
        entryObj[QStringLiteral("description")] = entry.description;

        QJsonArray tagsArray;
        for (const QString &tag : entry.tags)
            tagsArray.append(tag);
        entryObj[QStringLiteral("tags")] = tagsArray;

        QString thumbFile;
        if (!entry.thumbnail.isNull()) {
            QString safeName = entry.name;
            safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")),
                             QStringLiteral("_"));
            thumbFile = rel + QStringLiteral("consistency_%1_%2.png")
                                  .arg(safeName, entry.id);
            entry.thumbnail.save(folder + QStringLiteral("/") + thumbFile, "PNG");
        }
        entryObj[QStringLiteral("thumbnailFile")] = thumbFile;
        consistencyArray.append(entryObj);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("projectName")] = data.projectName;
    // fps drives the animatic; canvasWidth/Height are the project's real
    // canvas resolution, reconciled to the pixels on load — what is written
    // here is always the truth of the artwork saved beside it.
    root[QStringLiteral("fps")] = data.fps;
    root[QStringLiteral("canvasWidth")] = data.canvasSize.width();
    root[QStringLiteral("canvasHeight")] = data.canvasSize.height();
    root[QStringLiteral("scenes")] = scenesArray;
    root[QStringLiteral("consistencyBoard")] = consistencyArray;
    root[QStringLiteral("audioPath")] = data.audioPath;
    root[QStringLiteral("perspective")] = data.perspective;
    return root;
}

LoadedProject projectFromJson(const QJsonObject &root, const QString &folder)
{
    LoadedProject out;
    out.projectName = root.value(QStringLiteral("projectName")).toString();
    out.fps = root.value(QStringLiteral("fps")).toInt(24);
    out.manifestSize = QSize(root.value(QStringLiteral("canvasWidth")).toInt(0),
                             root.value(QStringLiteral("canvasHeight")).toInt(0));
    out.audioPath = root.value(QStringLiteral("audioPath")).toString();
    out.perspective = root.value(QStringLiteral("perspective")).toObject();

    // LEGACY shared layers ("Copy/Reuse Layer in Another Panel", removed):
    // old files store the PNG only on the FIRST instance of a sharedId;
    // later instances carry sharedId with no imageFile. MIGRATION: each such
    // reference becomes an INDEPENDENT real copy (own pixels, fresh id), so
    // old projects load with nothing missing. New saves drop sharedId.
    QHash<QString, QImage> legacySharedImages;
    const QJsonArray scenesArray = root.value(QStringLiteral("scenes")).toArray();
    for (const QJsonValue &sv : scenesArray) {
        const QJsonObject sceneObj = sv.toObject();
        Scene *scene = new Scene;
        scene->number = sceneObj.value(QStringLiteral("number")).toInt();
        scene->location = sceneObj.value(QStringLiteral("location")).toString();
        scene->timeOfDay = sceneObj.value(QStringLiteral("timeOfDay")).toString();
        if (scene->timeOfDay.isEmpty())
            scene->timeOfDay = QStringLiteral("UNSPECIFIED");
        scene->action = sceneObj.value(QStringLiteral("action")).toString();

        const QJsonArray panelsArray = sceneObj.value(QStringLiteral("panels")).toArray();
        for (const QJsonValue &pv : panelsArray) {
            const QJsonObject panelObj = pv.toObject();
            Panel *panel = new Panel;
            panel->duration = panelObj.value(QStringLiteral("duration")).toInt(3);
            if (panel->duration < 1)
                panel->duration = 3;
            // Only override defaults when a non-empty value is stored.
            const QString shotType = panelObj.value(QStringLiteral("shotType")).toString();
            if (!shotType.isEmpty())
                panel->shotType = shotType;
            const QString camera = panelObj.value(QStringLiteral("camera")).toString();
            if (!camera.isEmpty())
                panel->cameraAngle = camera;
            const QString lens = panelObj.value(QStringLiteral("lens")).toString();
            if (!lens.isEmpty())
                panel->lens = lens;
            panel->mood = panelObj.value(QStringLiteral("mood")).toString();
            panel->notes = panelObj.value(QStringLiteral("notes")).toString();

            const QString genStatus = panelObj.value(QStringLiteral("generationStatus")).toString();
            if (!genStatus.isEmpty())
                panel->generationStatus = genStatus;
            panel->generatedVideoPath = panelObj.value(QStringLiteral("generatedVideoPath")).toString();
            panel->falRequestId = panelObj.value(QStringLiteral("falRequestId")).toString();

            // Version tree: reconstruct takes; a take whose file is missing is Failed.
            panel->takes.clear();
            const QJsonArray takesArray = panelObj.value(QStringLiteral("takes")).toArray();
            for (const QJsonValue &tv : takesArray) {
                const QJsonObject takeObj = tv.toObject();
                GeneratedTake take;
                take.id = takeObj.value(QStringLiteral("id")).toString();
                take.videoPath = takeObj.value(QStringLiteral("videoPath")).toString();
                take.promptUsed = takeObj.value(QStringLiteral("promptUsed")).toString();
                take.timestamp = takeObj.value(QStringLiteral("timestamp")).toString();
                take.status = takeObj.value(QStringLiteral("status")).toString();
                take.costEstimate = takeObj.value(QStringLiteral("costEstimate")).toDouble();
                if (take.videoPath.isEmpty()
                    || !QFileInfo::exists(folder + QStringLiteral("/") + take.videoPath))
                    take.status = QStringLiteral("Failed");
                panel->takes.append(take);
            }
            panel->selectedTakeId = panelObj.value(QStringLiteral("selectedTakeId")).toString();

            // Migrate pre-takes projects: fold a lone generatedVideoPath into one take.
            if (panel->takes.isEmpty() && !panel->generatedVideoPath.isEmpty()
                && panel->generationStatus == QLatin1String("Complete")) {
                GeneratedTake take;
                take.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                take.videoPath = panel->generatedVideoPath;
                take.status =
                    QFileInfo::exists(folder + QStringLiteral("/") + take.videoPath)
                        ? QStringLiteral("Complete")
                        : QStringLiteral("Failed");
                panel->takes.append(take);
                panel->selectedTakeId = take.id;
            }

            // Keep the generatedVideoPath mirror aligned with the selected take.
            for (const GeneratedTake &take : panel->takes) {
                if (take.id == panel->selectedTakeId)
                    panel->generatedVideoPath = take.videoPath;
            }

            // Layer stack. New files carry a "layers" array; legacy files (one
            // PNG per panel) are migrated into a single "Layer 1" raster layer
            // so old projects open with their drawing intact.
            panel->layers.clear();
            const QJsonArray layersArray = panelObj.value(QStringLiteral("layers")).toArray();
            if (!layersArray.isEmpty()) {
                for (const QJsonValue &lv : layersArray) {
                    const QJsonObject layerObj = lv.toObject();
                    Layer layer;
                    layer.id = layerObj.value(QStringLiteral("id")).toString();
                    if (layer.id.isEmpty())
                        layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    layer.name = layerObj.value(QStringLiteral("name")).toString();
                    if (layer.name.isEmpty())
                        layer.name = QStringLiteral("Layer %1").arg(panel->layers.size() + 1);
                    layer.type = layerObj.value(QStringLiteral("type")).toString();
                    if (layer.type.isEmpty())
                        layer.type = QStringLiteral("raster");
                    layer.visible = layerObj.value(QStringLiteral("visible")).toBool(true);
                    layer.opacity = layerObj.value(QStringLiteral("opacity")).toDouble(1.0);
                    layer.locked = layerObj.value(QStringLiteral("locked")).toBool(false);
                    layer.colorTag = layerObj.value(QStringLiteral("colorTag")).toString();
                    const QString legacySharedId =
                        layerObj.value(QStringLiteral("sharedId")).toString();
                    layer.groupId = layerObj.value(QStringLiteral("groupId")).toString();
                    layer.groupExpanded =
                        layerObj.value(QStringLiteral("groupExpanded")).toBool(true);

                    QImage img;
                    const QString layerPng = layerObj.value(QStringLiteral("imageFile")).toString();
                    if (!layerPng.isEmpty())
                        img.load(folder + QStringLiteral("/") + layerPng);
                    if (!legacySharedId.isEmpty()) {
                        if (!img.isNull()) {
                            // First instance: keeps its pixels/identity and
                            // registers them for the references that follow.
                            img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                            if (!legacySharedImages.contains(legacySharedId))
                                legacySharedImages.insert(legacySharedId, img);
                        } else {
                            // Reference instance -> independent real copy
                            // with its own identity (a missing/corrupt source
                            // falls through to the deferred fill below —
                            // never fail the whole load).
                            img = legacySharedImages.value(legacySharedId).copy();
                            layer.id = QUuid::createUuid().toString(
                                QUuid::WithoutBraces);
                        }
                    }
                    // Loaded pixels are used AT THEIR TRUE SIZE (pixels are
                    // the authority). A missing/corrupt PNG stays NULL here
                    // and is filled at the PANEL's size below, once that
                    // size is known from the sibling layers — never at a
                    // fixed stand-in size into a stack of another size.
                    layer.image = (layer.type == QLatin1String("group")
                                   || img.isNull())
                        ? QImage()
                        : img.convertToFormat(
                              QImage::Format_ARGB32_Premultiplied);
                    panel->layers.append(layer);
                }
            } else {
                // BACKWARD COMPAT: old single-PNG project -> one raster layer.
                const QString pngName = panelObj.value(QStringLiteral("pixmapFile")).toString();
                QImage img;
                if (!pngName.isEmpty())
                    img.load(folder + QStringLiteral("/") + pngName);
                Layer layer;
                layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                layer.name = QStringLiteral("Layer 1");
                layer.type = QStringLiteral("raster");
                if (!img.isNull())
                    layer.image = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                panel->layers.append(layer); // null image -> filled below
            }
            if (panel->layers.isEmpty()) {
                Layer layer; // null image -> filled at the panel size below
                layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                layer.name = QStringLiteral("Layer 1");
                layer.type = QStringLiteral("raster");
                panel->layers.append(layer);
            }
            // Fill every deferred (null-image, non-group) layer at the
            // PANEL's own size. Resolution order: the panel's loaded
            // pixels; else the manifest; else 960x540.
            //
            // DO NOT DELETE THE 960x540 ARM AS A LEFTOVER OF THE RESOLUTION
            // EPIC. It is not a default and not a fallback for new files —
            // it is a MIGRATION FACT about OLD files, and it is unreachable
            // for anything saved since the epic: every such file carries
            // canvasWidth/canvasHeight (the manifest arm) and its layers
            // carry pixels (the panel arm). This line can only run for a
            // file with NO size keys and NO decodable layer PNG in this
            // panel — which is precisely a file written by a pre-versioned
            // build, and every pre-versioned build had its canvas
            // hard-fixed at 960x540. That number is therefore the one
            // historically CORRECT recovery size for such a panel.
            // If this arm were removed, panelPx would stay invalid, the
            // layer images below would be created null/invalid, and the
            // whole load of a damaged legacy project would produce a panel
            // the canvas guards refuse to draw on — turning "old project
            // with a missing PNG opens with a blank recoverable layer"
            // into "old project cannot be used at all".
            {
                QSize panelPx = panel->canvasSize();
                if (!panelPx.isValid())
                    panelPx = (out.manifestSize.isValid()
                               && !out.manifestSize.isEmpty())
                        ? out.manifestSize
                        : QSize(960, 540); // pre-versioned era: see above
                for (Layer &layer : panel->layers)
                    if (layer.image.isNull()
                        && layer.type != QLatin1String("group"))
                        layer.image = makeLayerImage(panelPx);
            }
            panel->activeLayerIndex =
                qBound(0, panelObj.value(QStringLiteral("activeLayerIndex")).toInt(0),
                       panel->layers.size() - 1);
            // Ensure a locked white Background beneath transparent art layers.
            // Idempotent: files already saved in the new format are untouched;
            // legacy opaque-white canvases are migrated without losing art.
            migratePanelToBackground(panel);

            scene->panels.append(panel);
        }
        out.scenes.append(scene);
    }

    // Backfill scene numbers if the file didn't carry them.
    for (int i = 0; i < out.scenes.size(); ++i) {
        if (out.scenes.at(i)->number == 0)
            out.scenes[i]->number = i + 1;
    }

    // Consistency board entries.
    const QJsonArray consistencyArray =
        root.value(QStringLiteral("consistencyBoard")).toArray();
    for (const QJsonValue &cv : consistencyArray) {
        const QJsonObject entryObj = cv.toObject();
        ConsistencyEntry entry;
        entry.id = entryObj.value(QStringLiteral("id")).toString();
        if (entry.id.isEmpty())
            entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        entry.name = entryObj.value(QStringLiteral("name")).toString();
        entry.type = entryObj.value(QStringLiteral("type")).toString();
        if (entry.type.isEmpty())
            entry.type = QStringLiteral("Character");
        entry.description = entryObj.value(QStringLiteral("description")).toString();

        const QJsonArray tagsArray = entryObj.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue &tv : tagsArray)
            entry.tags << tv.toString();

        const QString thumbFile = entryObj.value(QStringLiteral("thumbnailFile")).toString();
        if (!thumbFile.isEmpty()) {
            QPixmap pm;
            if (pm.load(folder + QStringLiteral("/") + thumbFile))
                entry.thumbnail = pm;
        }
        out.consistency.append(entry);
    }

    // RECONCILE the manifest against the artwork. PIXELS WIN: the layers'
    // real dimensions are the size the project opens at; the artwork is
    // never rescaled, cropped, or discarded. A disagreeing manifest
    // (written by the stored-not-applied era) sets `mismatch`; the caller
    // shows canvasMismatchDialogText and the next save corrects the file.
    // Census of EVERY panel, not just the first: the scan used to stop at
    // the first valid panel, so a project whose panels disagreed opened
    // silently at whichever size happened to come first and the rest were
    // never looked at. Order is preserved so ties resolve to the first
    // size encountered.
    QVector<QPair<QSize, int>> census; // size -> count, in first-seen order
    for (Scene *scene : out.scenes) {
        for (Panel *panel : scene->panels) {
            const QSize size = panel->canvasSize();
            if (!size.isValid())
                continue;
            auto it = std::find_if(census.begin(), census.end(),
                                   [&size](const QPair<QSize, int> &e) {
                                       return e.first == size;
                                   });
            if (it == census.end())
                census.append({size, 1});
            else
                ++it->second;
        }
    }
    // MAJORITY wins, ties to the first seen. For a uniform project the
    // majority IS the first, so this is identical to the old behaviour;
    // only a mixed project resolves differently, and there "first" was an
    // accident of ordering that would spread to every new panel.
    for (const QPair<QSize, int> &entry : census)
        if (entry.second > out.majorityPanelCount) {
            out.majorityPanelCount = entry.second;
            out.pixelSize = entry.first;
        }
    out.mixedSizes = census.size() > 1;
    if (out.mixedSizes) {
        // Locate the dissenters so the report can name them.
        for (Scene *scene : out.scenes) {
            for (int i = 0; i < scene->panels.size(); ++i) {
                const QSize size = scene->panels.at(i)->canvasSize();
                if (size.isValid() && size != out.pixelSize)
                    out.offSizePanels.append({scene->number, i + 1, size});
            }
        }
    }
    // Same MIGRATION FACT as the deferred-fill block above — do not delete
    // as a leftover. Reaching this line requires a project with no valid
    // pixels in ANY panel and no manifest keys: only a pre-versioned build
    // (fixed 960x540 canvas by construction) could have written such a
    // file, so 960x540 is the file's true historical size, not a guess.
    // Without it, pixelSize would stay invalid and the whole project would
    // open at an invalid size the canvas guards refuse to operate on.
    if (!out.pixelSize.isValid())
        out.pixelSize = (out.manifestSize.isValid() && !out.manifestSize.isEmpty())
            ? out.manifestSize
            : QSize(960, 540); // pre-versioned era: see above
    out.mismatch = out.manifestSize.isValid() && !out.manifestSize.isEmpty()
        && out.manifestSize != out.pixelSize;
    return out;
}

} // namespace ProjectIO
