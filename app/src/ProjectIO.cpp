#include "ProjectIO.h"

#include "ProjectMedia.h"
#include "camera/ShotCamera.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QMutex>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <vector>

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

QString ImageLedger::keyFor(const QString &file)
{
    return QDir::cleanPath(QFileInfo(file).absoluteFilePath());
}

bool ImageLedger::holds(const QString &file, const QByteArray &content) const
{
    const auto it = m_entries.constFind(keyFor(file));
    if (it == m_entries.cend() || it->content != content)
        return false;
    const QFileInfo now(file);
    return now.isFile() && now.size() == it->bytes
        && now.lastModified() == it->modified;
}

void ImageLedger::record(const QString &file, const QByteArray &content)
{
    const QFileInfo now(file);
    if (!now.isFile()) {
        forget(file);
        return;
    }
    m_entries.insert(keyFor(file), Entry{content, now.size(), now.lastModified()});
}

void ImageLedger::forget(const QString &file)
{
    m_entries.remove(keyFor(file));
}

void ImageLedger::merge(const ImageLedger &other)
{
    for (auto it = other.m_entries.cbegin(); it != other.m_entries.cend(); ++it)
        m_entries.insert(it.key(), it.value());
}

QByteArray layerContent(const QImage &image)
{
    QByteArray id;
    QDataStream s(&id, QIODevice::WriteOnly);
    s << quint8('L') << image.cacheKey();
    return id;
}

QByteArray flattenContent(const Panel &panel)
{
    // Everything Panel::flattenedPixmap() reads: each layer's pixels, its
    // opacity and visibility, whether it is a folder, and which folder it
    // is in (a folder's own visibility and opacity reach its members). The
    // canvas size is the layers' own, so their images already carry it.
    QByteArray id;
    QDataStream s(&id, QIODevice::WriteOnly);
    s << quint8('F');
    for (const Layer &layer : panel.layers)
        s << layer.image.cacheKey() << layer.opacity << layer.visible
          << layer.type << layer.groupId;
    return id;
}

namespace {

QByteArray thumbnailContent(const QPixmap &pixmap)
{
    QByteArray id;
    QDataStream s(&id, QIODevice::WriteOnly);
    s << quint8('C') << pixmap.cacheKey();
    return id;
}

// Where the save's images reach the disk, so the one place that can fail.
//
// An image whose file already holds it (ImageLedger) is skipped. The rest
// are ENCODED IN PARALLEL: each one whole, by one thread, with the same
// QImage::save call the UI thread used to make, so every file is the same
// bytes it always was - only several are made at once. The flattens are
// still made on the UI thread (flattenedPixmap is a QPixmap) and only for
// panels that changed; at most one per thread waits in the queue, so a
// large project never holds every flatten in memory at once.
//
// A failure stops further writes and is reported as the EARLIEST image in
// the save's order that failed, as the sequential writer reported it. A
// save that cannot write every pixel it is about to name must not become
// a project file: the caller writes the manifest only after finish() says
// every image is on disk.
class ImageSaver
{
public:
    ImageSaver(const QString &folder, ImageLedger *ledger, WriteResult *res)
        : m_folder(folder), m_ledger(ledger), m_res(res)
    {
        // Enough to finish a full 4K save in about two seconds, few enough
        // that the encoders' working copies (one image each) stay bounded.
        const int threads = qBound(1, QThread::idealThreadCount(), 8);
        m_pool.setMaxThreadCount(threads);
        m_slots.release(threads * 2);
    }
    ~ImageSaver() { m_pool.waitForDone(); }

    bool failed() const { return m_failed.load(); }

    void layer(const QImage &image, const QString &relName)
    {
        const QByteArray content = layerContent(image);
        if (!skip(relName, content))
            push(relName, image, content);
    }

    void flatten(const Panel &panel, const QString &relName)
    {
        const QByteArray content = flattenContent(panel);
        if (!skip(relName, content))
            push(relName, panel.flattenedPixmap().toImage(), content);
    }

    void thumbnail(const QPixmap &pixmap, const QString &relName)
    {
        const QByteArray content = thumbnailContent(pixmap);
        if (!skip(relName, content))
            push(relName, pixmap.toImage(), content);
    }

    // Waits for every write, then reports them in the save's order.
    void finish()
    {
        m_pool.waitForDone();
        std::sort(m_done.begin(), m_done.end(),
                  [](const Done &a, const Done &b) { return a.order < b.order; });
        QSet<Qt::HANDLE> threads;
        for (const Done &d : m_done) {
            if (d.ok) {
                ++m_res->imagesWritten;
                m_res->writtenFiles.append(d.file);
                threads.insert(d.thread);
                if (m_ledger)
                    m_ledger->record(d.file, d.content);
                continue;
            }
            if (m_ledger)
                m_ledger->forget(d.file); // may be half-written now
            if (m_res->ok) {
                m_res->ok = false;
                m_res->failedFile = d.file;
                m_res->reason = QStringLiteral(
                    "The image could not be written. The disk may be full, "
                    "the file may be open in another program, or the folder "
                    "may be read-only.");
            }
        }
        m_res->encodeThreads = int(threads.size());
    }

private:
    struct Done
    {
        int order = 0;
        QString file;
        QByteArray content;
        bool ok = false;
        Qt::HANDLE thread = nullptr;
    };

    bool skip(const QString &relName, const QByteArray &content)
    {
        if (!m_ledger || !m_ledger->holds(m_folder + QStringLiteral("/") + relName,
                                          content))
            return false;
        ++m_res->imagesSkipped;
        return true;
    }

    void push(const QString &relName, const QImage &image, const QByteArray &content)
    {
        if (m_failed.load())
            return; // already failed; do not pile more writes on
        const int order = m_next++;
        const QString full = m_folder + QStringLiteral("/") + relName;
        m_slots.acquire(); // bounds the images waiting to be encoded
        m_pool.start([this, order, full, image, content] {
            Done d{order, full, content, false, QThread::currentThreadId()};
            if (!m_failed.load()) {
                d.ok = image.save(full, "PNG");
                if (!d.ok)
                    m_failed.store(true);
                QMutexLocker lock(&m_mutex);
                m_done.append(d);
            }
            m_slots.release();
        });
    }

    const QString m_folder;
    ImageLedger *const m_ledger;
    WriteResult *const m_res;
    int m_next = 0;
    std::atomic<bool> m_failed{false};
    QSemaphore m_slots;
    QMutex m_mutex;
    QVector<Done> m_done;
    QThreadPool m_pool; // last: destroyed first, after its writes finish
};

} // namespace

WriteResult projectToJson(const SaveData &data, const QString &projectFilePath,
                          const std::function<bool(qint64, qint64)> &audioProgress)
{
    WriteResult res;
    res.ok = true; // cleared by the first failure below
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
    const QString assetsPath = folder + QStringLiteral("/") + assets;
    // CHECKED, and first: if the folder cannot be created then every image
    // below fails too, and an unchecked mkpath followed by forty unchecked
    // saves is exactly how a manifest comes to name files that were never
    // written. mkpath succeeds when the directory already exists; it fails
    // when a FILE of that name is in the way, when the parent is read-only,
    // or when the path is too long for the filesystem.
    if (!QDir().mkpath(assetsPath)) {
        res.ok = false;
        res.failedFile = assetsPath;
        res.reason = QStringLiteral(
            "The folder for this project's images could not be created. A "
            "file of the same name may be in the way, the location may be "
            "read-only, or the path may be too long.");
        return res;
    }
    // Every stored name gets this prefix; every write goes through it.
    const QString rel = assets + QStringLiteral("/");

    // THE AUDIO TRACK, FIRST: it is the one write here that can be large
    // and can be cancelled, and stopping before forty images are written
    // is cheaper than after. What the manifest will call it:
    QString storedAudio;
    if (!data.audioPath.isEmpty()) {
        if (data.audioLinked) {
            // An old project's reference, byte for byte as it was loaded.
            storedAudio = data.audioPath;
        } else if (ProjectMedia::isInAssets(data.audioPath, projectFilePath)) {
            // Already this project's own: the ordinary save. Nothing is
            // read, nothing is copied.
            res.audioFile = data.audioPath;
            storedAudio = ProjectMedia::storedName(data.audioPath, projectFilePath);
        } else if (!QFileInfo::exists(data.audioPath)) {
            // The project's own track, but its file is not there to copy
            // (it is shown as missing). The reference is carried, under a
            // name nothing else has - so the copy's track is honestly
            // missing rather than quietly becoming some other file.
            const QString name = ProjectMedia::freeName(
                projectFilePath, QFileInfo(data.audioPath).fileName());
            res.audioFile = assetsPath + QStringLiteral("/") + name;
            storedAudio = rel + name;
        } else {
            const ProjectMedia::Adoption copy = ProjectMedia::adopt(
                data.audioPath, projectFilePath, audioProgress);
            if (!copy.ok()) {
                res.ok = false;
                res.cancelled = copy.outcome == ProjectMedia::Adoption::Cancelled;
                res.failedFile = copy.failedFile;
                res.reason = QStringLiteral("The project's audio file could "
                                            "not be copied into the project. ")
                    + copy.reason;
                return res;
            }
            res.audioFile = copy.file;
            storedAudio = ProjectMedia::storedName(copy.file, projectFilePath);
        }
    }

    ImageSaver saver(folder, data.ledger, &res);

    QJsonArray scenesArray;
    for (int i = 0; i < data.scenes.size() && !saver.failed(); ++i) {
        Scene *scene = data.scenes.at(i);

        QJsonObject sceneObj;
        sceneObj[QStringLiteral("name")] = QStringLiteral("Scene %1").arg(scene->number);
        sceneObj[QStringLiteral("number")] = scene->number;     // preserved (not lost)
        sceneObj[QStringLiteral("location")] = scene->location;
        sceneObj[QStringLiteral("timeOfDay")] = scene->timeOfDay; // preserved
        sceneObj[QStringLiteral("action")] = scene->action;

        QJsonArray panelsArray;
        for (int j = 0; j < scene->panels.size() && !saver.failed(); ++j) {
            Panel *panel = scene->panels.at(j);

            // Flattened composite — kept for forward-compat (older builds and any
            // external tool reading pixmapFile still see the merged drawing).
            // flattenedPixmap() is panel-sized, so this PNG matches the layers.
            const QString pngName =
                rel + QStringLiteral("panel_s%1_p%2.png").arg(i).arg(j);
            saver.flatten(*panel, pngName);

            // Layer stack: one PNG per layer + a JSON descriptor array.
            QJsonArray layersArray;
            for (int k = 0; k < panel->layers.size() && !saver.failed(); ++k) {
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
                    saver.layer(layer.image, layerPng);
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
        // THE SHOT CAMERA (Phase 1A): written only when the scene has live
        // keys, so a project without camera moves saves the JSON it saved
        // before the camera existed.
        const QJsonObject camera = shotcam::toJson(*scene);
        if (!camera.isEmpty())
            sceneObj[QStringLiteral("camera")] = camera;
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
            saver.thumbnail(entry.thumbnail, thumbFile);
        }
        entryObj[QStringLiteral("thumbnailFile")] = thumbFile;
        consistencyArray.append(entryObj);
    }

    // Every image on disk, or the first one that could not be: the
    // encoders are waited for HERE, before any manifest exists to name them.
    saver.finish();
    if (!res.ok)
        return res; // no manifest is built from a half-written image set

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
    root[QStringLiteral("audioPath")] = storedAudio;
    root[QStringLiteral("perspective")] = data.perspective;
    // The tracks' lock / visibility / mute flags (TimelineTracks.h). Always
    // written; a reader without the key gets the defaults.
    root[QStringLiteral("tracks")] = data.tracks.toJson();

    // Reached only with every image on disk: res.ok is still true, so the
    // caller is clear to write the manifest.
    res.root = root;
    return res;
}

namespace {
// THE OPEN IN PARALLEL (2026-10-31). The load below used to decode each
// layer PNG where it met it, one after another on the UI thread: 46 4K
// layers took 1.75 s of a 2 s open. Now every file it will read is decoded
// - and converted to the layers' format - on worker threads FIRST; the load
// then takes each image in its own order, so every migration and identity
// rule sees exactly what it saw. Bounded like the save's encoders: at most
// 8 threads, one image each in flight.
std::atomic<int> g_decodedOffUi{0};
std::atomic<int> g_decodedOnUi{0};
bool g_serialDecodeForTest = false;

QImage decodeLayerFile(const QString &file)
{
    QImage img;
    img.load(file);
    if (!img.isNull())
        img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const bool ui = QCoreApplication::instance()
        && QThread::currentThread() == QCoreApplication::instance()->thread();
    (ui ? g_decodedOnUi : g_decodedOffUi).fetch_add(1);
    return img;
}

class LayerDecodes
{
public:
    // The files the load will read, in the panels' own terms: each layer's
    // imageFile in a panel with layers, the panel's pixmapFile in an old
    // panel without (a layered panel's pixmapFile is its flatten, never
    // decoded on open).
    LayerDecodes(const QJsonArray &scenes, const QString &folder)
    {
        QStringList files;
        QSet<QString> seen;
        auto want = [&](const QString &name) {
            if (name.isEmpty())
                return;
            const QString file = folder + QStringLiteral("/") + name;
            if (!seen.contains(file)) {
                seen.insert(file);
                files.append(file);
            }
        };
        for (const QJsonValue &sv : scenes)
            for (const QJsonValue &pv : sv.toObject().value(QStringLiteral("panels")).toArray()) {
                const QJsonObject panel = pv.toObject();
                const QJsonArray layers = panel.value(QStringLiteral("layers")).toArray();
                if (!layers.isEmpty())
                    for (const QJsonValue &lv : layers)
                        want(lv.toObject().value(QStringLiteral("imageFile")).toString());
                else
                    want(panel.value(QStringLiteral("pixmapFile")).toString());
            }
        std::vector<QImage> images(size_t(files.size()));
        if (g_serialDecodeForTest || files.size() < 2) {
            for (int i = 0; i < files.size(); ++i)
                images[size_t(i)] = decodeLayerFile(files.at(i));
        } else {
            QThreadPool pool;
            pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount(), 8));
            for (int i = 0; i < files.size(); ++i)
                pool.start([&images, &files, i] {
                    images[size_t(i)] = decodeLayerFile(files.at(i));
                });
            pool.waitForDone();
        }
        for (int i = 0; i < files.size(); ++i)
            m_images.insert(files.at(i), images[size_t(i)]);
    }

    // The file's pixels, as a fresh load would give them: the first reader
    // gets the decoded image, a second reader of the same file its own deep
    // copy (two loads of one file were two images). A file not collected
    // above is loaded here, as before.
    QImage take(const QString &file)
    {
        const auto it = m_images.constFind(file);
        if (it == m_images.cend())
            return decodeLayerFile(file);
        if (m_taken.contains(file))
            return it.value().copy();
        m_taken.insert(file);
        return it.value();
    }

private:
    QHash<QString, QImage> m_images;
    QSet<QString> m_taken;
};
} // namespace

DecodeCounts decodeCountsForTest()
{
    return {g_decodedOffUi.load(), g_decodedOnUi.load()};
}

void resetDecodeCountsForTest()
{
    g_decodedOffUi = 0;
    g_decodedOnUi = 0;
}

void setSerialDecodeForTest(bool serial)
{
    g_serialDecodeForTest = serial;
}

LoadedProject projectFromJson(const QJsonObject &root, const QString &folder,
                              const QString &manifestFile)
{
    LoadedProject out;
    // The manifest's time, when known: the flatten trust test (see the
    // declaration). Invalid = trust no flatten.
    const QDateTime manifestTime =
        manifestFile.isEmpty() ? QDateTime() : QFileInfo(manifestFile).lastModified();
    out.projectName = root.value(QStringLiteral("projectName")).toString();
    out.fps = root.value(QStringLiteral("fps")).toInt(24);
    out.manifestSize = QSize(root.value(QStringLiteral("canvasWidth")).toInt(0),
                             root.value(QStringLiteral("canvasHeight")).toInt(0));
    // The audio track. A relative name is the project's own file, in its
    // assets folder, and resolves against THIS manifest's folder - which is
    // what lets the folder be moved or copied. An absolute path is an old
    // project's link to a file outside it and is kept exactly as stored.
    // String work only: whether the file is there is the animatic's to
    // find out and to show, never a reason to change what the record says.
    const QString storedAudio = root.value(QStringLiteral("audioPath")).toString();
    out.audioLinked = !storedAudio.isEmpty() && QDir::isAbsolutePath(storedAudio);
    out.audioPath = storedAudio.isEmpty() || out.audioLinked
        ? storedAudio
        : QDir::cleanPath(folder + QStringLiteral("/") + storedAudio);
    out.perspective = root.value(QStringLiteral("perspective")).toObject();
    // No "tracks" key (every project saved before 2026-10-18): unlocked,
    // visible, unmuted - exactly what those projects were.
    out.tracks = TimelineTracks::fromJson(root.value(QStringLiteral("tracks")).toObject());

    // LEGACY shared layers ("Copy/Reuse Layer in Another Panel", removed):
    // old files store the PNG only on the FIRST instance of a sharedId;
    // later instances carry sharedId with no imageFile. MIGRATION: each such
    // reference becomes an INDEPENDENT real copy (own pixels, fresh id), so
    // old projects load with nothing missing. New saves drop sharedId.
    QHash<QString, QImage> legacySharedImages;
    const QJsonArray scenesArray = root.value(QStringLiteral("scenes")).toArray();
    // Every layer file below, decoded in parallel before the walk (see
    // LayerDecodes); the walk takes them in its own order.
    LayerDecodes decodes(scenesArray, folder);
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

            // Version tree: reconstruct takes EXACTLY AS SAVED.
            // This used to consult the disk: a take whose video file was
            // not beside the project was rewritten to "Failed", and the next
            // save made that permanent - the record of a finished, paid-for
            // render replaced by a claim that it had failed, because a
            // folder had been moved or a project opened from a Save As copy
            // (which does not carry the videos). Whether a file is present
            // is a fact about the disk today; a take's status is a fact
            // about what happened when it was generated. Load reads the
            // second and leaves the first to whoever displays takes - which,
            // since the Generation page was removed, is nobody.
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
                panel->takes.append(take);
            }
            panel->selectedTakeId = panelObj.value(QStringLiteral("selectedTakeId")).toString();

            // Migrate pre-takes projects: fold a lone generatedVideoPath into
            // one take. The panel says the render completed - that is the
            // condition for folding it at all - so the take says so too,
            // whether or not the file is here today (see above).
            if (panel->takes.isEmpty() && !panel->generatedVideoPath.isEmpty()
                && panel->generationStatus == QLatin1String("Complete")) {
                GeneratedTake take;
                take.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                take.videoPath = panel->generatedVideoPath;
                take.status = QStringLiteral("Complete");
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
            // The images this panel read from their own files, and whether
            // any of those files is newer than the manifest.
            QSet<qint64> readKeys;
            bool readNewerThanManifest = false;
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
                        img = decodes.take(folder + QStringLiteral("/") + layerPng);
                    // Pixels from the layer's OWN file - not a legacy
                    // reference's copy, made below only when this is null.
                    const bool readOwnFile = !img.isNull();
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
                    if (readOwnFile && !layer.image.isNull()) {
                        const QString file = folder + QStringLiteral("/") + layerPng;
                        out.images.record(file, layerContent(layer.image));
                        readKeys.insert(layer.image.cacheKey());
                        if (!manifestTime.isValid()
                            || QFileInfo(file).lastModified() > manifestTime)
                            readNewerThanManifest = true;
                    }
                }
            } else {
                // BACKWARD COMPAT: old single-PNG project -> one raster layer.
                const QString pngName = panelObj.value(QStringLiteral("pixmapFile")).toString();
                QImage img;
                if (!pngName.isEmpty())
                    img = decodes.take(folder + QStringLiteral("/") + pngName);
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

            // The flatten beside them holds these layers only if every
            // layer drawn into it is one just read from its own file, still
            // untouched by the migrations above, and nothing here is newer
            // than the manifest (see the declaration).
            const QString flatPng = panelObj.value(QStringLiteral("pixmapFile")).toString();
            if (!layersArray.isEmpty() && !flatPng.isEmpty() && manifestTime.isValid()
                && !readNewerThanManifest) {
                const QString flatFile = folder + QStringLiteral("/") + flatPng;
                bool allRead = QFileInfo(flatFile).lastModified() <= manifestTime;
                for (const Layer &layer : panel->layers)
                    if (!isGroupLayer(layer) && !readKeys.contains(layer.image.cacheKey()))
                        allRead = false;
                if (allRead)
                    out.images.record(flatFile, flattenContent(*panel));
            }

            scene->panels.append(panel);
        }
        // The camera after the panels (its keys name them by index). No
        // "camera" (every project saved before Phase 1A): the neutral one.
        int droppedKeys = 0;
        shotcam::fromJson(sceneObj.value(QStringLiteral("camera")).toObject(),
                          *scene, &droppedKeys);
        out.cameraKeysDropped += droppedKeys;
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
            if (pm.load(folder + QStringLiteral("/") + thumbFile)) {
                entry.thumbnail = pm;
                out.images.record(folder + QStringLiteral("/") + thumbFile,
                                  thumbnailContent(pm));
            }
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
