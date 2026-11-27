#include "ProjectIO.h"

#include "ProjectMedia.h"
#include "camera/ShotCamera.h"

#include <QBuffer>
#include <QColor> // a scene's colour, read back (batch E)
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSemaphore>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

// THE SAFE SAVE (saving step 1, 2026-11-23) pushes each file through to the disk and names it
// in one step of the file system's own; Qt has no call for either, so Windows' are used.
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

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

void ImageLedger::record(const QString &file, const QByteArray &content, const QPoint &origin)
{
    const QFileInfo now(file);
    if (!now.isFile()) {
        forget(file);
        return;
    }
    m_entries.insert(keyFor(file), Entry{content, now.size(), now.lastModified(), origin});
}

QPoint ImageLedger::originOf(const QString &file) const
{
    const auto it = m_entries.constFind(keyFor(file));
    return it == m_entries.cend() ? QPoint() : it->origin;
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

QString ImageLedger::fileHolding(const QByteArray &content, const QString &folder) const
{
    const QString prefix = QDir::cleanPath(QFileInfo(folder).absoluteFilePath()) + QLatin1Char('/');
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->content != content || !it.key().startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const QFileInfo now(it.key());
        if (now.isFile() && now.size() == it->bytes && now.lastModified() == it->modified)
            return it.key();
    }
    return QString();
}

QStringList ImageLedger::filesHolding(const QByteArray &content, const QString &folder) const
{
    QStringList files;
    const QString prefix = QDir::cleanPath(QFileInfo(folder).absoluteFilePath()) + QLatin1Char('/');
    for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
        if (it->content != content || !it.key().startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const QFileInfo now(it.key());
        if (now.isFile() && now.size() == it->bytes && now.lastModified() == it->modified)
            files.append(it.key());
    }
    files.sort(); // the same answer every run, whatever the table's order
    return files;
}

// --- The image names (2026-11-04) ----------------------------------------------

QString imageIdStem(const QString &id)
{
    QString hex;
    for (const QChar c : id)
        if (c.isLetterOrNumber())
            hex += c.toLower();
    return hex.left(12);
}

namespace {
QString fullIdHex(const QString &id)
{
    QString hex;
    for (const QChar c : id)
        if (c.isLetterOrNumber())
            hex += c.toLower();
    return hex.isEmpty() ? QStringLiteral("0") : hex;
}

// The names for one save, panel by panel: the flatten first, then one per
// raster layer (a folder owns no pixels). A stem that twelve digits cannot
// keep apart within the save takes the whole id.
struct PanelNames
{
    QString flatten;
    QHash<QString, QString> layers; // layer id -> file name
};

// A drawing of a run's key in PanelNames::layers (batch I-2): no id holds a '/'.
QString drawingNameKey(const QString &layerId, const QString &drawingId)
{
    return layerId + QLatin1Char('/') + drawingId;
}

QVector<PanelNames> namesFor(const QVector<Scene *> &scenes)
{
    QVector<PanelNames> out;
    QSet<QString> used;
    for (const Scene *scene : scenes) {
        if (!scene)
            continue;
        for (const Panel *panel : scene->panels) {
            if (!panel)
                continue;
            PanelNames names;
            QString stem = imageIdStem(panel->id);
            if (stem.isEmpty() || used.contains(QStringLiteral("p_") + stem))
                stem = fullIdHex(panel->id);
            used.insert(QStringLiteral("p_") + stem);
            names.flatten = QStringLiteral("p_%1.png").arg(stem);
            for (const Layer &layer : panel->layers) {
                if (layer.type == QLatin1String("group"))
                    continue;
                QString lstem = imageIdStem(layer.id);
                if (lstem.isEmpty()
                    || used.contains(QStringLiteral("p_%1_l_%2").arg(stem, lstem)))
                    lstem = fullIdHex(layer.id);
                used.insert(QStringLiteral("p_%1_l_%2").arg(stem, lstem));
                names.layers.insert(layer.id, QStringLiteral("p_%1_l_%2.png").arg(stem, lstem));
                // A RUN OF DRAWINGS (batch I-2): a file to each drawing, by its own id after the
                // layer's ("..._d_<12 hex>.png"), under the key drawingNameKey gives.
                if (layer.hasRun())
                    for (const LayerDrawing &drawing : layer.drawings) {
                        QString dstem = imageIdStem(drawing.id);
                        if (dstem.isEmpty()
                            || used.contains(QStringLiteral("p_%1_l_%2_d_%3").arg(stem, lstem, dstem)))
                            dstem = fullIdHex(drawing.id);
                        used.insert(QStringLiteral("p_%1_l_%2_d_%3").arg(stem, lstem, dstem));
                        names.layers.insert(drawingNameKey(layer.id, drawing.id),
                                            QStringLiteral("p_%1_l_%2_d_%3.png").arg(stem, lstem, dstem));
                    }
            }
            out.append(names);
        }
    }
    return out;
}
} // namespace

QStringList projectImageNames(const QVector<Scene *> &scenes)
{
    QStringList list;
    for (const PanelNames &names : namesFor(scenes)) {
        list.append(names.flatten);
        for (auto it = names.layers.cbegin(); it != names.layers.cend(); ++it)
            list.append(it.value());
    }
    return list;
}

bool isIdImageName(const QString &fileName)
{
    // "~<n>" before ".png": the same image written again by a later save (the safe save,
    // 2026-11-23 - a save never writes over a file; see imageNameVariant).
    static const QRegularExpression rx(
        QStringLiteral("^p_[0-9a-f]{12,32}(_l_[0-9a-f]{12,32}(_d_[0-9a-f]{12,32})?)?(~[0-9]+)?\\.png$"),
        QRegularExpression::CaseInsensitiveOption);
    return rx.match(fileName).hasMatch();
}

QString panelStemOfImageName(const QString &fileName)
{
    if (!isIdImageName(fileName))
        return QString();
    const QString plain = imageBaseName(fileName);
    const QString body = plain.mid(2, plain.size() - 6); // between "p_" and ".png"
    const int l = body.indexOf(QStringLiteral("_l_"));
    return (l < 0 ? body : body.left(l)).toLower();
}

// --- The names of an image written again (the safe save, 2026-11-23) -----------------------
// A save never writes over a file. An image that changed goes into a file of its own: its
// name with "~2", "~3", ... before the extension - the lowest that is free. "x.png" is the
// base name of them all.
QString imageNameVariant(const QString &name, int n)
{
    if (n <= 1)
        return name;
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString tail = QStringLiteral("~%1").arg(n);
    return dot < 0 ? name + tail : name.left(dot) + tail + name.mid(dot);
}

QString imageBaseName(const QString &name)
{
    static const QRegularExpression rx(QStringLiteral("~[0-9]+(\\.[^./\\\\~]*)?$"));
    const QRegularExpressionMatch m = rx.match(name);
    if (!m.hasMatch())
        return name;
    return name.left(m.capturedStart()) + m.captured(1);
}

QByteArray layerContent(const Layer &layer)
{
    QByteArray id;
    QDataStream s(&id, QIODevice::WriteOnly);
    s << quint8('L') << layer.contentKey();
    return id;
}

QByteArray flattenContent(const Panel &panel)
{
    // Everything Panel::flattenedPixmap() reads: each layer's pixels, its
    // opacity and visibility, whether it is a folder, and which folder it
    // is in (a folder's own visibility and opacity reach its members). The
    // canvas size and the frame's place: a margin moves no pixel of a layer
    // (batch 6b-V), so they are said here, or the flatten would stay the old
    // stage's.
    QByteArray id;
    QDataStream s(&id, QIODevice::WriteOnly);
    s << quint8('F') << panel.frameAt();
    for (const Layer &layer : panel.layers) {
        // (A run of drawings, batch I-2: the flatten is the panel's first frame, so its
        // FIRST drawing's pixels, whichever the layer shows. Without a run: the layer's own.)
        s << layer.pixelsOf(0).contentKey() << layer.pixelsOf(0).pixelSize() << layer.opacity << layer.visible
          << layer.type << layer.groupId;
        // A layer's motion (batch I-1): the flatten is its first frame, so a key moved is
        // another flatten. Nothing for a layer without motion, whose id reads as before.
        if (layer.hasMotionData())
            s << quint8('M') << QJsonDocument(layerMotionJson(layer)).toJson(QJsonDocument::Compact);
    }
    return id;
}

QJsonObject layerMotionJson(const Layer &layer)
{
    QJsonObject motion;
    motion[QStringLiteral("animate")] = layer.animate;
    motion[QStringLiteral("pivot")] = QJsonArray{layer.pivot.x(), layer.pivot.y()};
    QJsonArray keys;
    for (const shotcam::LayerKey &key : layer.keys) {
        QJsonObject k;
        k[QStringLiteral("at")] = key.at;
        k[QStringLiteral("x")] = key.pose.dx;
        k[QStringLiteral("y")] = key.pose.dy;
        k[QStringLiteral("rotation")] = key.pose.rotation;
        k[QStringLiteral("scale")] = key.pose.scale;
        k[QStringLiteral("opacity")] = key.pose.opacity;
        k[QStringLiteral("ease")] = shotcam::easeName(key.ease);
        keys.append(k);
    }
    motion[QStringLiteral("keys")] = keys;
    return motion;
}

int layerMotionFromJson(const QJsonObject &motion, Layer *layer)
{
    int dropped = 0;
    layer->animate = motion.value(QStringLiteral("animate")).toBool(false);
    const QJsonArray pivot = motion.value(QStringLiteral("pivot")).toArray();
    if (pivot.size() == 2 && pivot.at(0).isDouble() && pivot.at(1).isDouble()
        && std::isfinite(pivot.at(0).toDouble()) && std::isfinite(pivot.at(1).toDouble()))
        layer->pivot = QPointF(pivot.at(0).toDouble(), pivot.at(1).toDouble());
    layer->keys.clear();
    for (const QJsonValue &v : motion.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject k = v.toObject();
        shotcam::LayerKey key;
        key.at = k.value(QStringLiteral("at")).toDouble(-1.0);
        key.pose.dx = k.value(QStringLiteral("x")).toDouble(0.0);
        key.pose.dy = k.value(QStringLiteral("y")).toDouble(0.0);
        key.pose.rotation = k.value(QStringLiteral("rotation")).toDouble(0.0);
        key.pose.scale = k.value(QStringLiteral("scale")).toDouble(1.0);
        key.pose.opacity = k.value(QStringLiteral("opacity")).toDouble(1.0);
        bool known = false;
        key.ease = shotcam::easeFromName(k.value(QStringLiteral("ease")).toString(), &known);
        const bool fine = std::isfinite(key.at) && key.at >= 0.0 && key.at <= 1.0 && std::isfinite(key.pose.dx)
            && std::isfinite(key.pose.dy) && std::isfinite(key.pose.rotation) && std::isfinite(key.pose.scale)
            && key.pose.scale > 0.0 && std::isfinite(key.pose.opacity) && key.pose.opacity >= 0.0
            && key.pose.opacity <= 1.0;
        if (!fine) {
            ++dropped;
            continue;
        }
        layer->keys.append(key);
    }
    return dropped;
}

namespace {
// The older file form, for the checks' fixtures (ProjectIO.h).
std::atomic<bool> g_writeVersion2ForTest{false};
}

void setWriteVersion2ForTest(bool on)
{
    g_writeVersion2ForTest.store(on);
}

namespace {

QByteArray thumbnailContent(const QPixmap &pixmap)
{
    QByteArray id;
    QDataStream s(&id, QIODevice::WriteOnly);
    s << quint8('C') << pixmap.cacheKey();
    return id;
}

// --- THE SAFE SAVE'S STEPS (saving step 1, 2026-11-23) -------------------------------------
// Every file a save makes is made the same way: written whole under "<name>.part", PUSHED
// THROUGH TO THE DISK, and only then given its name - in one step of the file system's, itself
// written through. Each of those is a step the writer reports here, in order. The checks
// read that report (the order of the pushes), stop the writer after any step - from then on
// it does nothing at all, as a process that died there does nothing, cleaning up included -
// or hold it at a step for a second process to be killed in.
struct SaveHooks
{
    QMutex mutex;
    QVector<SaveStepRecord> *log = nullptr;
    int steps = 0;
    int stopAfter = -1;
    int holdAt = -1;
    std::function<void()> hold;
    bool dead = false;
    std::atomic<bool> skipPush{false};
    std::atomic<bool> overwriteInPlace{false};
    // The short retry (saving step 2): the checks' hook in place of the pause, and the
    // writer of before it (one try).
    std::function<void(const QString &, int)> retryHook;
    std::atomic<bool> retryOff{false};
};
SaveHooks &saveHooks()
{
    static SaveHooks hooks;
    return hooks;
}

// The writer has just done `step`. False: it is to do nothing more.
bool stepDone(SaveStep step, const QString &file)
{
    SaveHooks &h = saveHooks();
    std::function<void()> hold;
    {
        QMutexLocker lock(&h.mutex);
        if (h.dead)
            return false;
        ++h.steps;
        if (h.log)
            h.log->append({step, QDir::cleanPath(file)});
        if (h.steps == h.holdAt)
            hold = h.hold;
        if (h.stopAfter >= 0 && h.steps >= h.stopAfter)
            h.dead = true;
    }
    if (hold)
        hold(); // may never come back: the process is killed in it
    QMutexLocker lock(&h.mutex);
    return !h.dead;
}

bool writerStopped()
{
    SaveHooks &h = saveHooks();
    QMutexLocker lock(&h.mutex);
    return h.dead;
}

// What the file holds, pushed through Windows' cache to the disk.
bool pushToDisk(QFile &file)
{
    if (!file.flush())
        return false;
    if (saveHooks().skipPush.load())
        return true; // a check's control: the push left out
#ifdef Q_OS_WIN
    const intptr_t handle = _get_osfhandle(file.handle());
    return handle != -1 && FlushFileBuffers(HANDLE(handle)) != 0;
#else
    return ::fsync(file.handle()) == 0;
#endif
}

#ifdef Q_OS_WIN
std::wstring nativePath(const QString &file)
{
    QString path = QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath());
    // Past Windows' classic length the path needs its long form.
    if (path.size() >= 240 && path.size() > 2 && path.at(1) == QLatin1Char(':'))
        path.prepend(QStringLiteral("\\\\?\\"));
    return path.toStdWString();
}
#endif

// THE SHORT RETRY (saving step 2, 2026-11-23; ProjectIO.h). `file` could not be named or
// read at the try that made `triesMade`, because another program holds it: true when the
// writer is to try again - after the pause, or after the checks' hook in its place.
bool tryAgain(const QString &file, int triesMade)
{
    SaveHooks &h = saveHooks();
    if (h.retryOff.load() || triesMade >= kNameTries)
        return false;
    std::function<void(const QString &, int)> hook;
    {
        QMutexLocker lock(&h.mutex);
        if (h.dead)
            return false; // a check stopped the writer: it does nothing more
        hook = h.retryHook;
    }
    if (hook)
        hook(QDir::cleanPath(file), triesMade);
    else
        QThread::msleep(kNamePauseMs);
    return true;
}

// `from` takes the name `to`, in one step, written through. With `over`, in place of the
// file that has the name; without, only if nothing has it. A refusal that is another
// program holding one of the two for a moment is tried again (tryAgain): `tries` is how
// often it was tried, `held` whether it failed as held to the end.
bool giveName(const QString &from, const QString &to, bool over, QString *why, int *tries = nullptr,
              bool *held = nullptr)
{
#ifdef Q_OS_WIN
    const std::wstring a = nativePath(from), b = nativePath(to);
    int made = 0;
    DWORD error = 0;
    bool holding = false;
    for (;;) {
        ++made;
        if (MoveFileExW(a.c_str(), b.c_str(), (over ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH)) {
            if (tries)
                *tries = made;
            return true;
        }
        error = GetLastError();
        // What Windows says when another program has the file open: of the file being
        // replaced, "access is denied"; of the one being moved, a sharing violation. (A
        // folder where the file is to be is "access is denied" too, and stays so: not held.)
        holding = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
            || error == ERROR_USER_MAPPED_FILE || (error == ERROR_ACCESS_DENIED && !QFileInfo(to).isDir());
        if (!holding || !tryAgain(to, made))
            break;
    }
    if (tries)
        *tries = made;
    if (held)
        *held = holding;
    if (why)
        *why = qt_error_string(int(error));
    return false;
#else
    if (tries)
        *tries = 1;
    if (!over && QFileInfo::exists(to)) {
        if (why)
            *why = QStringLiteral("a file of that name is there already");
        return false;
    }
    if (std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0)
        return true;
    if (why)
        *why = qt_error_string();
    return false;
#endif
}

enum class Whole { Done, Failed, Stopped };

// `bytes` as the file `to`, whole or not at all: its three steps, each reported.
Whole writeWhole(const QString &to, const QByteArray &bytes, bool over, SaveStep written, SaveStep pushed,
                 SaveStep named, QString *why)
{
    if (writerStopped())
        return Whole::Stopped;
    const QString part = to + QStringLiteral(".part");
    QFile out(part);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (why)
            *why = out.errorString();
        return Whole::Failed;
    }
    if (out.write(bytes) != bytes.size()) {
        if (why)
            *why = out.errorString();
        out.close();
        QFile::remove(part);
        return Whole::Failed;
    }
    if (!stepDone(written, part))
        return Whole::Stopped;
    if (!pushToDisk(out)) {
        if (why)
            *why = out.errorString();
        out.close();
        QFile::remove(part);
        return Whole::Failed;
    }
    if (!saveHooks().skipPush.load() && !stepDone(pushed, part))
        return Whole::Stopped;
    out.close();
    if (!giveName(part, to, over, why)) {
        QFile::remove(part);
        return Whole::Failed;
    }
    return stepDone(named, to) ? Whole::Done : Whole::Stopped;
}

// The image files a project file names, by what they are: "P:<panel id>" a panel's flatten,
// "L:<panel id>:<layer id>" a layer, "C:<entry id>" a Consistency Board thumbnail - and all
// the names, lower-cased, relative to the project file as written. A panel without an id
// (a project from before 2026-11-04) is in `named` only.
struct NamedImages
{
    QHash<QString, QString> byKey;
    QSet<QString> named;
};
NamedImages namedImages(const QJsonObject &root)
{
    NamedImages out;
    auto add = [&out](const QString &key, const QString &name) {
        if (name.isEmpty())
            return;
        out.named.insert(name.toLower());
        if (!key.isEmpty())
            out.byKey.insert(key, name);
    };
    for (const QJsonValue &sv : root.value(QStringLiteral("scenes")).toArray())
        for (const QJsonValue &pv : sv.toObject().value(QStringLiteral("panels")).toArray()) {
            const QJsonObject p = pv.toObject();
            const QString pid = p.value(QStringLiteral("id")).toString();
            add(pid.isEmpty() ? QString() : QStringLiteral("P:") + pid, p.value(QStringLiteral("pixmapFile")).toString());
            for (const QJsonValue &lv : p.value(QStringLiteral("layers")).toArray()) {
                const QJsonObject l = lv.toObject();
                const QString lid = l.value(QStringLiteral("id")).toString();
                // A run of drawings (version 7): each drawing's file, "D:<panel id>:<layer
                // id>:<drawing id>"; the layer's own imageFile is then its first drawing's
                // and has no key of its own.
                const QJsonArray drawings = l.value(QStringLiteral("drawings")).toArray();
                if (!drawings.isEmpty()) {
                    for (const QJsonValue &dv : drawings) {
                        const QJsonObject d = dv.toObject();
                        const QString did = d.value(QStringLiteral("id")).toString();
                        add(pid.isEmpty() || lid.isEmpty() || did.isEmpty()
                                ? QString()
                                : QStringLiteral("D:%1:%2:%3").arg(pid, lid, did),
                            d.value(QStringLiteral("imageFile")).toString());
                    }
                    add(QString(), l.value(QStringLiteral("imageFile")).toString());
                    continue;
                }
                add(pid.isEmpty() || lid.isEmpty() ? QString() : QStringLiteral("L:%1:%2").arg(pid, lid),
                    l.value(QStringLiteral("imageFile")).toString());
            }
        }
    for (const QJsonValue &cv : root.value(QStringLiteral("consistencyBoard")).toArray()) {
        const QJsonObject c = cv.toObject();
        const QString cid = c.value(QStringLiteral("id")).toString();
        add(cid.isEmpty() ? QString() : QStringLiteral("C:") + cid, c.value(QStringLiteral("thumbnailFile")).toString());
    }
    return out;
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
    // `assets`: this project's own image folder, the only one adopt() may
    // rename from; `keep`: every image name this save uses, relative to
    // `folder` - the files adopt() must leave where they are; `plan`: when
    // set, the images to write go there instead of to the encoders.
    ImageSaver(const QString &folder, const QString &assets, ImageLedger *ledger,
               WriteResult *res, const QStringList &keep, ImagePlan *plan, bool planIsSave)
        : m_folder(folder), m_assets(assets), m_ledger(ledger), m_res(res), m_plan(plan),
          // THE SAFE SAVE (2026-11-23): a Save, which knows what its files hold - written
          // here, or (saving step 2) as a plan of its own for runSavePlan. The recovery copy
          // (a plan that is not a Save's) and a caller without a ledger (the seam's
          // fixtures) write as before.
          m_safe(ledger && (!plan || planIsSave) && !saveHooks().overwriteInPlace.load())
    {
        for (const QString &name : keep) {
            m_keep.insert(QDir::cleanPath(QFileInfo(folder + QStringLiteral("/") + name)
                                              .absoluteFilePath())
                              .toLower());
            m_bases.insert(QFileInfo(name).fileName().toLower());
        }
        // Enough to finish a full 4K save in about two seconds, few enough
        // that the encoders' working copies (one image each) stay bounded.
        int threads = qBound(1, QThread::idealThreadCount(), 8);
        {
            // A check that stops or holds the writer at a step needs the steps in one order.
            SaveHooks &h = saveHooks();
            QMutexLocker lock(&h.mutex);
            if (h.stopAfter >= 0 || h.holdAt >= 0)
                threads = 1;
        }
        m_pool.setMaxThreadCount(threads);
        m_slots.release(threads * 2);
    }
    ~ImageSaver() { m_pool.waitForDone(); }

    bool failed() const { return m_failed.load(); }

    // Where the layer about to be given to layer() stands in the project file's arrays
    // (saving step 2: a layer reduced where the plan runs has its place written there).
    // `drawing`: the layer's drawing of that index (a run, batch I-2); -1 the layer itself.
    void at(int scene, int panel, int layer, int drawing = -1)
    {
        m_scene = scene;
        m_panel = panel;
        m_layer = layer;
        m_drawing = drawing;
    }

    // A layer's file (version 3): only what is drawn when the tiles say what the
    // layer holds; the whole stage when they do not yet (the panel on the canvas,
    // edited, needs no fold on the way out). Says in `layerObj` where it lies.
    // Returns the name the layer's file has after the save (relative, as `baseName` is): with
    // the safe save that is the file that already holds it, or a new one - never `baseName`
    // itself when a file of that name is there (settle()).
    QString layer(const Layer &layer, const QPoint &frameAt, const QString &baseName, QJsonObject *layerObj)
    {
        const QByteArray content = layerContent(layer);
        const bool oldForm = g_writeVersion2ForTest.load();
        bool have = false;
        const QString relName = m_safe && !oldForm ? settle(baseName, content, &have) : baseName;
        const QString full = m_folder + QStringLiteral("/") + relName;
        const bool paper = layer.type == QLatin1String("background");
        layertiles::TilesPtr tiles = layer.atRest() || layer.tilesCurrent() ? layer.tiles : nullptr;
        QPoint origin;
        if (oldForm) {
            // The older form: never skipped, as a place cannot be said for it.
            origin = -frameAt;
            pushLayer(relName, layer.pixels(), nullptr, QRect(), content, origin);
        } else if (m_safe ? have : (skip(relName, content) || adopt(relName, content))) {
            origin = m_ledger->originOf(full);
        } else {
            // A full image not yet folded folds here, so a project saved, opened
            // and saved again writes the same bytes: only the layers edited since
            // the panel was entered. The recovery copy keeps the UI thread short
            // and takes the whole stage, unless the file has to hold art outside
            // the stage.
            const bool mustFold = layer.tiles
                && layertiles::hasHidden(*layer.tiles, QRect(-frameAt, layer.pixelSize()));
            // THE SAVE IN THE BACKGROUND (saving step 2): a Save's plan takes the full image
            // as a shared copy and it is reduced to its drawn part where the plan runs
            // (ImageJob::fold) - the same file a Save always wrote, none of it made here.
            // Where the layer then lies, and a Background's fill, runSavePlan writes into
            // the project file's text; what is said here stands only until then.
            if (m_plan && m_safe && !tiles && layertiles::foldable(layer.image)) {
                ImageJob job;
                job.file = full;
                job.content = content;
                job.image = layer.image;
                job.tiles = layer.tiles;
                job.fold = true;
                job.frameAt = frameAt;
                job.paper = paper;
                job.scene = m_scene;
                job.panel = m_panel;
                job.layer = m_layer;
                job.drawing = m_drawing;
                m_plan->jobs.append(std::move(job));
                (*layerObj)[QStringLiteral("origin")] = QJsonArray{-frameAt.x(), -frameAt.y()};
                (*layerObj)[QStringLiteral("size")] =
                    QJsonArray{layer.pixelSize().width(), layer.pixelSize().height()};
                return relName;
            }
            if (!tiles && layertiles::foldable(layer.image) && (!m_plan || mustFold))
                tiles = layertiles::fold(layer.image, frameAt,
                                         paper ? layertiles::paperFill(layer.image, layer.tiles) : 0u,
                                         layer.tiles);
            if (tiles) {
                // Nothing drawn: one pixel of the fill, for a file to name.
                const QRect rect = tiles->drawn.isEmpty() ? QRect(0, 0, 1, 1) : tiles->drawn;
                origin = rect.topLeft();
                pushLayer(relName, QImage(), tiles, rect, content, origin);
            } else {
                origin = -frameAt;
                pushLayer(relName, layer.image, nullptr, QRect(), content, origin);
            }
        }
        if (!oldForm) {
            (*layerObj)[QStringLiteral("origin")] = QJsonArray{origin.x(), origin.y()};
            // The stage it is read at: the pixels stay the authority for a panel's
            // size, as a whole file's size was.
            (*layerObj)[QStringLiteral("size")] =
                QJsonArray{layer.pixelSize().width(), layer.pixelSize().height()};
            if (paper) {
                const quint32 fill = tiles ? tiles->fill : layertiles::paperFill(layer.image, layer.tiles);
                (*layerObj)[QStringLiteral("fill")] =
                    QStringLiteral("%1").arg(fill, 8, 16, QLatin1Char('0'));
            }
        }
        return relName;
    }

    // Returns the name the flatten's file has after the save, as layer() does.
    QString flatten(const Panel &panel, const QString &baseName)
    {
        const QByteArray content = flattenContent(panel);
        if (m_safe) {
            bool have = false;
            const QString relName = settle(baseName, content, &have);
            if (!have && m_plan) {
                // (Saving step 2) the panel's layers as shared copies; runSavePlan
                // composites them where it runs.
                auto snapshot = std::make_shared<Panel>();
                snapshot->layers = panel.layers;
                ImageJob job;
                job.file = m_folder + QStringLiteral("/") + relName;
                job.content = content;
                job.flatten = std::move(snapshot);
                m_plan->jobs.append(std::move(job));
            } else if (!have) {
                m_res->composeThreadIds.append(QThread::currentThreadId());
                push(relName, panel.flattenedPixmap().toImage(), content);
            }
            return relName;
        }
        const QString &relName = baseName;
        if (skip(relName, content) || adopt(relName, content))
            return relName;
        if (m_plan) {
            // The panel's layers as shared copies; runImagePlan composites
            // them where it runs.
            auto snapshot = std::make_shared<Panel>();
            snapshot->layers = panel.layers;
            ImageJob job;
            job.file = m_folder + QStringLiteral("/") + relName;
            job.content = content;
            job.flatten = std::move(snapshot);
            m_plan->jobs.append(std::move(job));
            return relName;
        }
        m_res->composeThreadIds.append(QThread::currentThreadId());
        push(relName, panel.flattenedPixmap().toImage(), content);
        return relName;
    }

    // Returns the name the thumbnail's file has after the save, as layer() does.
    QString thumbnail(const QPixmap &pixmap, const QString &baseName)
    {
        const QByteArray content = thumbnailContent(pixmap);
        if (m_safe) {
            m_bases.insert(QFileInfo(baseName).fileName().toLower());
            bool have = false;
            const QString relName = settle(baseName, content, &have);
            if (!have)
                push(relName, pixmap.toImage(), content);
            return relName;
        }
        if (!skip(baseName, content))
            push(baseName, pixmap.toImage(), content);
        return baseName;
    }

    // Waits for every write, then reports them in the save's order.
    void finish()
    {
        m_pool.waitForDone();
        if (m_stopped.load()) {
            // A check stopped the writer: as after a death, nothing is tidied, nothing learnt.
            m_res->ok = false;
            m_res->stopped = true;
            m_res->reason = QStringLiteral("The save was stopped by a check.");
            return;
        }
        std::sort(m_done.begin(), m_done.end(),
                  [](const Done &a, const Done &b) { return a.order < b.order; });
        QSet<Qt::HANDLE> threads;
        for (const Done &d : m_done) {
            if (d.ok) {
                ++m_res->imagesWritten;
                m_res->writtenFiles.append(d.file);
                m_res->encodeThreadIds.append(d.thread);
                threads.insert(d.thread);
                if (m_ledger)
                    m_ledger->record(d.file, d.content, d.origin);
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
                // What the system said, for the report and the recording.
                if (!d.why.isEmpty())
                    m_res->reason += QStringLiteral("\n\n(%1)").arg(d.why);
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
        QPoint origin;
        QString why; // a failure's reason, as the writer and the file gave it
    };

    bool skip(const QString &relName, const QByteArray &content)
    {
        if (!m_ledger || !m_ledger->holds(m_folder + QStringLiteral("/") + relName,
                                          content))
            return false;
        ++m_res->imagesSkipped;
        return true;
    }

    // THE SAFE SAVE (2026-11-23): THE FILE THIS IMAGE IS IN AFTER THE SAVE. A save writes over
    // nothing: until the project file is switched, every file the saved project names must be
    // exactly as it was, or a save cut short would leave the old project file naming new or
    // half-written pictures.
    //  - A file in this project's own images folder that already HOLDS the image, as the
    //    ledger left it, is named as it is (`have`): the image's own file or an earlier one of
    //    its own (an Undo back to what the previous saved version holds), else a file no
    //    image of this save has a claim to - a project saved by position keeps its files and
    //    their names; nothing is renamed any more (a rename before the switch would take a
    //    file from under the old project file). Never the file of another image of this
    //    save: a duplicated panel's layers hold its source's pixels, and the source keeps
    //    its file.
    //  - Otherwise the image is written into a file of its own: its base name when nothing
    //    has it, else the lowest free "~n" of it (imageNameVariant).
    QString settle(const QString &baseName, const QByteArray &content, bool *have)
    {
        const int slash = baseName.lastIndexOf(QLatin1Char('/'));
        const QString dir = baseName.left(slash + 1); // "<project>_assets/"
        const QString base = baseName.mid(slash + 1).toLower();
        const QString here = QDir::cleanPath(QFileInfo(m_folder + QStringLiteral("/") + dir).absoluteFilePath());
        QString own, other;
        for (const QString &file : m_ledger->filesHolding(content, m_assets)) {
            const QFileInfo info(file);
            if (m_claimed.contains(file.toLower())
                || QDir::cleanPath(info.absolutePath()).compare(here, Qt::CaseInsensitive) != 0)
                continue;
            const QString itsBase = imageBaseName(info.fileName()).toLower();
            if (itsBase == base) {
                if (own.isEmpty())
                    own = file;
            } else if (!m_bases.contains(itsBase) && other.isEmpty()) {
                other = file;
            }
        }
        const QString found = own.isEmpty() ? other : own;
        if (!found.isEmpty()) {
            m_claimed.insert(found.toLower());
            ++m_res->imagesSkipped;
            *have = true;
            return dir + QFileInfo(found).fileName();
        }
        *have = false;
        for (int n = 1;; ++n) {
            const QString candidate = imageNameVariant(baseName, n);
            const QString full =
                QDir::cleanPath(QFileInfo(m_folder + QStringLiteral("/") + candidate).absoluteFilePath());
            if (m_claimed.contains(full.toLower()) || QFileInfo::exists(full))
                continue;
            m_claimed.insert(full.toLower());
            return candidate;
        }
    }

    // THE RENAME (2026-11-04): the image's pixels are already on disk under
    // another name in this project's OWN assets folder - the position name
    // the project was saved with before the id names - and that file is as
    // the ledger left it: it is MOVED to its id name, nothing re-encoded.
    // Only from that folder, which is this project's alone (named from its
    // file): a Save As into the folder of the original must COPY, not take
    // the original's files, and an old flat-named project's files beside
    // its manifest may be another manifest's too, so they are left where
    // they are and written afresh (the (h) checks). Never over a file that
    // exists (that file is some other panel's, or an orphan the artist
    // keeps), and never FROM a file this save names: a duplicated panel's
    // layers hold the same pixels as its source's, and the three white
    // backgrounds of three panels are one content - taking the source's
    // file would leave the source without one. A rename that fails falls
    // back to the encoder.
    bool adopt(const QString &relName, const QByteArray &content)
    {
        if (!m_ledger)
            return false;
        const QString full = m_folder + QStringLiteral("/") + relName;
        const QString have = m_ledger->fileHolding(content, m_assets);
        if (have.isEmpty() || QFileInfo(have) == QFileInfo(full) || QFileInfo::exists(full)
            || m_keep.contains(QDir::cleanPath(QFileInfo(have).absoluteFilePath()).toLower()))
            return false;
        if (!QFile::rename(have, full))
            return false;
        const QPoint origin = m_ledger->originOf(have);
        m_ledger->forget(have);
        m_ledger->record(full, content, origin);
        ++m_res->imagesRenamed;
        m_res->renamedFiles.append(full);
        return true;
    }

    void push(const QString &relName, const QImage &image, const QByteArray &content)
    {
        pushLayer(relName, image, nullptr, QRect(), content, QPoint());
    }

    // `tiles`: the image is `rect` of these, made where it is encoded.
    void pushLayer(const QString &relName, const QImage &image, const layertiles::TilesPtr &tiles,
                   const QRect &rect, const QByteArray &content, const QPoint &origin)
    {
        if (m_failed.load())
            return; // already failed; do not pile more writes on
        const QString full = m_folder + QStringLiteral("/") + relName;
        if (m_plan) { // a shared copy, written by runImagePlan
            ImageJob job;
            job.file = full;
            job.content = content;
            job.image = image;
            job.tiles = tiles;
            job.rect = rect;
            job.origin = origin;
            m_plan->jobs.append(std::move(job));
            return;
        }
        const int order = m_next++;
        m_slots.acquire(); // bounds the images waiting to be encoded
        m_pool.start([this, order, full, image, tiles, rect, content, origin] {
            Done d{order, full, content, false, QThread::currentThreadId(), origin};
            if (!m_failed.load() && m_safe) {
                // THE SAFE SAVE (2026-11-23): the same PNG bytes QImage::save makes, encoded
                // in memory, then the file made whole, pushed to the disk and named - `full`
                // is a name nothing has (settle()), so no file of the saved project is touched.
                const QImage picture = tiles ? layertiles::unfold(*tiles, rect) : image;
                QByteArray png;
                QString says;
                bool encoded = false;
                if (!picture.isNull()) {
                    QBuffer buffer(&png);
                    buffer.open(QIODevice::WriteOnly);
                    QImageWriter writer(&buffer, "PNG");
                    encoded = writer.write(picture);
                    if (!encoded)
                        says = writer.errorString();
                }
                Whole made = Whole::Failed;
                if (encoded)
                    made = writeWhole(full, png, false, SaveStep::ImageWritten, SaveStep::ImagePushed,
                                      SaveStep::ImageNamed, &says);
                d.ok = made == Whole::Done;
                if (made == Whole::Stopped)
                    m_stopped.store(true);
                if (!d.ok) {
                    m_failed.store(true);
                    d.why = picture.isNull()
                        ? QStringLiteral("the picture to write was empty")
                        : QStringLiteral("%1 x %2 px; %3").arg(picture.width()).arg(picture.height()).arg(says);
                }
                QMutexLocker lock(&m_mutex);
                m_done.append(d);
            } else if (!m_failed.load()) {
                // QImage::save's own writer, so the same bytes; kept here so a
                // failure can say why (a hand test of 2026-11-08 could not, 6c).
                const QImage picture = tiles ? layertiles::unfold(*tiles, rect) : image;
                QImageWriter writer(full, "PNG");
                d.ok = !picture.isNull() && writer.write(picture);
                if (d.ok && !stepDone(SaveStep::ImageOverwritten, full)) {
                    d.ok = false; // (today's writer under a check's stop: the red run)
                    m_stopped.store(true);
                }
                if (!d.ok) {
                    m_failed.store(true);
                    // The writer's words hold the file's when it could not open it;
                    // the file's are added only when they do not.
                    const auto *file = qobject_cast<QFileDevice *>(writer.device());
                    const QString fileSays = file && file->error() != QFileDevice::NoError
                        ? file->errorString()
                        : QString();
                    QString says = writer.errorString();
                    if (!fileSays.isEmpty() && !says.contains(fileSays))
                        says += QStringLiteral(": ") + fileSays;
                    d.why = picture.isNull()
                        ? QStringLiteral("the picture to write was empty")
                        : QStringLiteral("%1 x %2 px; %3").arg(picture.width()).arg(picture.height()).arg(says);
                }
                QMutexLocker lock(&m_mutex);
                m_done.append(d);
            }
            m_slots.release();
        });
    }

    const QString m_folder;
    const QString m_assets;
    ImageLedger *const m_ledger;
    WriteResult *const m_res;
    ImagePlan *const m_plan; // the recovery copy's: nothing encoded here
    const bool m_safe;       // the safe save (2026-11-23): see settle()
    QSet<QString> m_keep;    // lower-cased absolute paths
    QSet<QString> m_bases;   // the base file names of this save's images, lower-cased
    QSet<QString> m_claimed; // the files this save's images are in, lower-cased absolute paths
    int m_scene = -1, m_panel = -1, m_layer = -1; // see at()
    int m_drawing = -1;
    int m_next = 0;
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_stopped{false}; // a check stopped the writer (stepDone)
    QSemaphore m_slots;
    QMutex m_mutex;
    QVector<Done> m_done;
    QThreadPool m_pool; // last: destroyed first, after its writes finish
};

} // namespace

bool runImagePlan(ImagePlan &plan, const std::atomic<bool> &cancel)
{
    // As many at once as a Save encodes; each task composites at most one
    // flatten, so the working images stay bounded by the thread count.
    QThreadPool pool;
    pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount(), 8));
    std::atomic<bool> failed{false};
    for (ImageJob &job : plan.jobs) {
        ImageJob *j = &job; // the vector is not touched while the pool runs
        pool.start([j, &cancel, &failed] {
            if (cancel.load() || failed.load())
                return;
            QImage image = j->tiles ? layertiles::unfold(*j->tiles, j->rect) : j->image;
            if (j->flatten) {
                image = j->flatten->flattenedImage();
                j->composeThread = QThread::currentThreadId();
            }
            const QString part = j->file + QStringLiteral(".part");
            // (The safe save, 2026-11-23) the copy's image too is pushed through to the disk
            // before it is named, and takes its name in one step, in place of the copy's older
            // file: it was removed first, and for that moment the copy named a file not there.
            bool ok = false;
            if (!image.isNull()) {
                QFile out(part);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QImageWriter writer(&out, "PNG");
                    ok = writer.write(image) && pushToDisk(out);
                    out.close();
                }
                ok = ok && giveName(part, j->file, true, nullptr);
            }
            if (!ok) {
                QFile::remove(part);
                failed.store(true);
            }
            j->encodeThread = QThread::currentThreadId();
            j->ok = ok;
        });
    }
    pool.waitForDone(); // the results are read after this, on this thread
    if (cancel.load() || failed.load())
        return false;
    for (const ImageJob &job : plan.jobs)
        if (!job.ok)
            return false;
    return true;
}

namespace {
// The flatten as a Save has always written it. It was panel.flattenedPixmap().toImage(): a
// QPixmap made from a picture with no pixel less than opaque holds it WITHOUT an alpha
// channel, and so did the file. A QPixmap belongs to the UI thread; this is the same
// picture made without one.
QImage asPixmapHolds(const QImage &image)
{
    if (image.format() != QImage::Format_ARGB32_Premultiplied)
        return image;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(line[x]) != 255)
                return image;
    }
    return image.convertToFormat(QImage::Format_RGB32);
}

// One layer's place and (a Background's) fill, written into the project file's text.
void sayLayerPlace(QJsonObject *root, const ImageJob &job)
{
    QJsonArray scenes = root->value(QStringLiteral("scenes")).toArray();
    if (job.scene < 0 || job.scene >= scenes.size())
        return;
    QJsonObject scene = scenes.at(job.scene).toObject();
    QJsonArray panels = scene.value(QStringLiteral("panels")).toArray();
    if (job.panel < 0 || job.panel >= panels.size())
        return;
    QJsonObject panel = panels.at(job.panel).toObject();
    QJsonArray layers = panel.value(QStringLiteral("layers")).toArray();
    if (job.layer < 0 || job.layer >= layers.size())
        return;
    QJsonObject layer = layers.at(job.layer).toObject();
    if (job.drawing >= 0) {
        // A drawing of a run (batch I-2): its own entry says where it lies.
        QJsonArray drawings = layer.value(QStringLiteral("drawings")).toArray();
        if (job.drawing >= drawings.size())
            return;
        QJsonObject drawing = drawings.at(job.drawing).toObject();
        drawing[QStringLiteral("origin")] = QJsonArray{job.origin.x(), job.origin.y()};
        if (job.paper)
            drawing[QStringLiteral("fill")] = QStringLiteral("%1").arg(job.fill, 8, 16, QLatin1Char('0'));
        drawings[job.drawing] = drawing;
        layer[QStringLiteral("drawings")] = drawings;
    } else {
        layer[QStringLiteral("origin")] = QJsonArray{job.origin.x(), job.origin.y()};
        if (job.paper)
            layer[QStringLiteral("fill")] = QStringLiteral("%1").arg(job.fill, 8, 16, QLatin1Char('0'));
    }
    layers[job.layer] = layer;
    panel[QStringLiteral("layers")] = layers;
    panels[job.panel] = panel;
    scene[QStringLiteral("panels")] = panels;
    scenes[job.scene] = scene;
    (*root)[QStringLiteral("scenes")] = scenes;
}
} // namespace

PlanResult runSavePlan(ImagePlan &plan, QJsonObject *root)
{
    PlanResult res;
    // As many at once as a Save encoded; each task makes at most one picture (a flatten
    // composited, a layer reduced), so the working images stay bounded by the thread count.
    int threads = qBound(1, QThread::idealThreadCount(), 8);
    {
        // A check that stops or holds the writer at a step needs the steps in one order.
        SaveHooks &h = saveHooks();
        QMutexLocker lock(&h.mutex);
        if (h.stopAfter >= 0 || h.holdAt >= 0)
            threads = 1;
    }
    QThreadPool pool;
    pool.setMaxThreadCount(threads);
    std::atomic<bool> failed{false};
    for (ImageJob &job : plan.jobs) {
        ImageJob *j = &job; // the vector is not touched while the pool runs
        pool.start([j, &failed] {
            if (failed.load())
                return; // already failed; do not pile more writes on
            j->tried = true;
            QImage picture;
            if (j->flatten) {
                picture = asPixmapHolds(j->flatten->flattenedImage());
                j->composeThread = QThread::currentThreadId();
            } else if (j->fold) {
                // Only what is drawn, as ImageSaver::layer makes it for a Save on its own
                // thread: nothing drawn is one pixel of the fill, for a file to name.
                const layertiles::TilesPtr tiles = layertiles::fold(
                    j->image, j->frameAt, j->paper ? layertiles::paperFill(j->image, j->tiles) : 0u, j->tiles);
                const QRect rect = tiles->drawn.isEmpty() ? QRect(0, 0, 1, 1) : tiles->drawn;
                j->origin = rect.topLeft();
                j->fill = tiles->fill;
                picture = layertiles::unfold(*tiles, rect);
                j->foldThread = QThread::currentThreadId();
            } else {
                picture = j->tiles ? layertiles::unfold(*j->tiles, j->rect) : j->image;
            }
            // The same PNG bytes QImage::save makes, encoded in memory, then the file made
            // whole, pushed to the disk and named - `file` is a name nothing has.
            QByteArray png;
            QString says;
            bool encoded = false;
            if (!picture.isNull()) {
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                QImageWriter writer(&buffer, "PNG");
                encoded = writer.write(picture);
                if (!encoded)
                    says = writer.errorString();
            }
            Whole made = Whole::Failed;
            if (encoded)
                made = writeWhole(j->file, png, false, SaveStep::ImageWritten, SaveStep::ImagePushed,
                                  SaveStep::ImageNamed, &says);
            j->encodeThread = QThread::currentThreadId();
            j->ok = made == Whole::Done;
            j->stopped = made == Whole::Stopped;
            if (!j->ok) {
                failed.store(true);
                j->why = picture.isNull()
                    ? QStringLiteral("the picture to write was empty")
                    : QStringLiteral("%1 x %2 px; %3").arg(picture.width()).arg(picture.height()).arg(says);
            }
        });
    }
    pool.waitForDone(); // the results are read after this, on this thread
    res.ok = true;
    QSet<Qt::HANDLE> encoders;
    for (const ImageJob &job : plan.jobs) {
        if (job.stopped) {
            // A check stopped the writer: as after a death, nothing is tidied, nothing learnt.
            res.ok = false;
            res.stopped = true;
            res.reason = QStringLiteral("The save was stopped by a check.");
            return res;
        }
    }
    for (const ImageJob &job : plan.jobs) {
        if (job.foldThread)
            res.foldThreadIds.append(job.foldThread);
        if (job.composeThread)
            res.composeThreadIds.append(job.composeThread);
        if (job.ok) {
            ++res.imagesWritten;
            res.writtenFiles.append(job.file);
            res.encodeThreadIds.append(job.encodeThread);
            encoders.insert(job.encodeThread);
        } else if (job.tried && res.ok) {
            // The EARLIEST image in the save's order that failed, as the Save reported it.
            res.ok = false;
            res.failedFile = job.file;
            res.reason = QStringLiteral(
                "The image could not be written. The disk may be full, "
                "the file may be open in another program, or the folder "
                "may be read-only.");
            // What the system said, for the report and the recording.
            if (!job.why.isEmpty())
                res.reason += QStringLiteral("\n\n(%1)").arg(job.why);
        }
    }
    res.encodeThreads = int(encoders.size());
    if (res.ok && failed.load()) {
        // (Not reachable: a job that failed is tried and not ok. Never a manifest on a doubt.)
        res.ok = false;
        res.reason = QStringLiteral("An image could not be written.");
    }
    if (res.ok && root)
        for (const ImageJob &job : plan.jobs)
            if (job.fold)
                sayLayerPlace(root, job);
    return res;
}

QJsonObject perspectiveMoved(const QJsonObject &perspective, double dx, double dy)
{
    if ((dx == 0.0 && dy == 0.0) || !perspective.contains(QStringLiteral("vps")))
        return perspective; // no margin, or no points: the object as it was
    QJsonObject out = perspective;
    QJsonArray vps;
    for (const QJsonValue &v : perspective.value(QStringLiteral("vps")).toArray()) {
        QJsonObject point = v.toObject();
        if (point.contains(QStringLiteral("x")))
            point[QStringLiteral("x")] = point.value(QStringLiteral("x")).toDouble() + dx;
        if (point.contains(QStringLiteral("y")))
            point[QStringLiteral("y")] = point.value(QStringLiteral("y")).toDouble() + dy;
        vps.append(point);
    }
    out[QStringLiteral("vps")] = vps;
    return out;
}

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

    // THE IMAGE NAMES (2026-11-04): by the panels' stable ids, never their
    // positions, so an insert, move or delete rewrites no file; the same
    // list Clean Up Project Folder reads.
    const QVector<PanelNames> names = namesFor(data.scenes);
    QStringList keep;
    for (const PanelNames &n : names) {
        keep << rel + n.flatten;
        for (const QString &layerName : n.layers)
            keep << rel + layerName;
    }
    // (The writer of before 2026-11-23, for the checks' red run and control, takes no plan
    // of a Save's: it writes each image over its own file here, as it did.)
    ImagePlan *const plan =
        data.planIsSave && saveHooks().overwriteInPlace.load() ? nullptr : data.plan;
    ImageSaver saver(folder, assetsPath, data.ledger, &res, keep, plan, data.planIsSave);
    int named = 0;

    // The project's margin on each side (Phase 2A): where the frame starts
    // on every panel's stage - the origin of the camera keys and the
    // vanishing points in the file (version 2, batch 6a).
    const shotcam::Stage projectStage = shotcam::stageWithMargin(data.canvasSize, data.stageMargin);
    const QMargins projectMargin = projectStage.hasMargin()
        ? QMargins(projectStage.margin().width(), projectStage.margin().height(),
                   projectStage.margin().width(), projectStage.margin().height())
        : QMargins();
    bool anyOwnMargin = false;
    bool anyLocked = false;
    bool anyMotion = false; // a layer with motion kept (batch I-1): version 6
    bool anyRun = false;    // a layer with a run of drawings (batch I-2): version 7

    QJsonArray scenesArray;
    for (int i = 0; i < data.scenes.size() && !saver.failed(); ++i) {
        Scene *scene = data.scenes.at(i);

        QJsonObject sceneObj;
        sceneObj[QStringLiteral("name")] = QStringLiteral("Scene %1").arg(scene->number);
        sceneObj[QStringLiteral("number")] = scene->number;     // preserved (not lost)
        sceneObj[QStringLiteral("location")] = scene->location;
        sceneObj[QStringLiteral("timeOfDay")] = scene->timeOfDay; // preserved
        sceneObj[QStringLiteral("action")] = scene->action;
        // The length lock (batch F1): written only when set, and then the file is
        // version 5.
        if (scene->lengthLocked) {
            sceneObj[QStringLiteral("lengthLocked")] = true;
            anyLocked = true;
        }
        // The scene's colour (batch E): written only once chosen - Auto is no key at all,
        // so an older file and an Auto scene read alike.
        if (!scene->colorTag.isEmpty())
            sceneObj[QStringLiteral("colorTag")] = scene->colorTag;

        QJsonArray panelsArray;
        for (int j = 0; j < scene->panels.size() && !saver.failed(); ++j) {
            Panel *panel = scene->panels.at(j);
            if (!panel)
                continue;
            const PanelNames &panelNames = names.at(named++);

            // Flattened composite — kept for forward-compat (older builds and any
            // external tool reading pixmapFile still see the merged drawing).
            // flattenedPixmap() is panel-sized, so this PNG matches the layers.
            // (The safe save, 2026-11-23: the file it IS in after the save - its base name the
            // first time, a "~n" of it once it has changed since a save.)
            const QString pngName = saver.flatten(*panel, rel + panelNames.flatten);

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
                // Depth (Phase 2B): only off the picture plane, so a project
                // without depth saves the JSON it saved before.
                if (layer.parallax != 1.0)
                    layerObj[QStringLiteral("parallax")] = layer.parallax;
                // LAYER MOTION (batch I-1): only on a layer with motion kept - Animate on, or
                // keys - so a project without one saves the JSON it saved before; with one,
                // the file is version 6.
                if (layer.hasMotionData() && layer.type != QLatin1String("group")
                    && layer.type != QLatin1String("background")) {
                    layerObj[QStringLiteral("motion")] = layerMotionJson(layer);
                    anyMotion = true;
                }

                if (layer.type != QLatin1String("group") && layer.hasRun()) {
                    // A RUN OF DRAWINGS (batch I-2; version 7): each drawing a file of its own,
                    // written as a layer's is - skipped when its file holds it already, so a
                    // stroke on one drawing writes that one. Each entry says its drawing's
                    // place; the layer's own "imageFile" is its first drawing's.
                    const QPoint frameAt = panel->ownMargin ? panel->frameAt()
                                                            : QPoint(projectMargin.left(), projectMargin.top());
                    QJsonArray drawingsArray;
                    for (int d = 0; d < layer.drawings.size() && !saver.failed(); ++d) {
                        const LayerDrawing &drawing = layer.drawings.at(d);
                        QJsonObject drawingObj;
                        drawingObj[QStringLiteral("id")] = drawing.id;
                        drawingObj[QStringLiteral("start")] = drawing.start;
                        saver.at(int(scenesArray.size()), int(panelsArray.size()), int(layersArray.size()), d);
                        const QString drawingPng =
                            saver.layer(layer.asDrawing(d), frameAt,
                                        rel + panelNames.layers.value(drawingNameKey(layer.id, drawing.id)),
                                        &drawingObj);
                        drawingObj[QStringLiteral("imageFile")] = drawingPng;
                        drawingsArray.append(drawingObj);
                    }
                    layerObj[QStringLiteral("drawings")] = drawingsArray;
                    layerObj[QStringLiteral("runRate")] = layer.runRate;
                    if (!drawingsArray.isEmpty())
                        layerObj[QStringLiteral("imageFile")] =
                            drawingsArray.first().toObject().value(QStringLiteral("imageFile"));
                    anyRun = true;
                } else if (layer.type != QLatin1String("group")) { // folders own no pixels
                    saver.at(int(scenesArray.size()), int(panelsArray.size()), int(layersArray.size()));
                    const QString layerPng =
                        saver.layer(layer,
                                    panel->ownMargin ? panel->frameAt()
                                                     : QPoint(projectMargin.left(), projectMargin.top()),
                                    rel + panelNames.layers.value(layer.id), &layerObj);
                    layerObj[QStringLiteral("imageFile")] = layerPng;
                }
                layersArray.append(layerObj);
            }

            QJsonObject panelObj;
            panelObj[QStringLiteral("id")] = panel->id; // the stable id (2026-11-04)
            // THE LENGTH IN FRAMES at the project's rate (2026-11-06, batch
            // 5). "duration" (whole seconds) is read from older files and no
            // longer written.
            panelObj[QStringLiteral("frames")] = shotcam::panelFrames(*panel, data.fps);
            // THE PANEL'S STAGE MARGIN (2026-11-06, batch 6a; version 2):
            // only with one, so a panel without says what it always said.
            // A panel at the project's margin writes the project's (a panel
            // made without the app's own paths - a test fixture - is at the
            // project's stage by its pixels all the same); one at its own
            // (Stage mode, 6c) writes its own, and the file says version 4.
            if (const QMargins panelMargin = panel->ownMargin ? panel->margin : projectMargin;
                !panelMargin.isNull())
                panelObj[QStringLiteral("margin")] =
                    QJsonArray{panelMargin.left(), panelMargin.top(), panelMargin.right(),
                               panelMargin.bottom()};
            anyOwnMargin = anyOwnMargin || panel->ownMargin;
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
        // before the camera existed. In frame space (version 2, batch 6a).
        const QJsonObject camera = shotcam::toFrameSpaceJson(
            *scene, QPointF(projectMargin.left(), projectMargin.top()));
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
            thumbFile = saver.thumbnail(entry.thumbnail,
                                        rel + QStringLiteral("consistency_%1_%2.png").arg(safeName, entry.id));
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
    root[QStringLiteral("version")] = g_writeVersion2ForTest.load()
        ? 2
        : (anyRun ? kFileVersionLayerDrawings
                  : (anyMotion ? kFileVersionLayerMotion
                               : (anyLocked ? kFileVersionLockedScene
                                            : (anyOwnMargin ? kFileVersion : kFileVersionOneMargin))));
    root[QStringLiteral("projectName")] = data.projectName;
    // fps drives the animatic; canvasWidth/Height are the project's real
    // canvas resolution, reconciled to the pixels on load — what is written
    // here is always the truth of the artwork saved beside it.
    root[QStringLiteral("fps")] = data.fps;
    root[QStringLiteral("canvasWidth")] = data.canvasSize.width();
    root[QStringLiteral("canvasHeight")] = data.canvasSize.height();
    // The shot's size, ONLY with a stage margin (Phase 2A): without one the
    // keys are absent and the manifest is the one it always was.
    const shotcam::Stage stage = shotcam::stageWithMargin(data.canvasSize, data.stageMargin);
    if (stage.hasMargin()) {
        root[QStringLiteral("frameWidth")] = stage.frame.width();
        root[QStringLiteral("frameHeight")] = stage.frame.height();
    }
    root[QStringLiteral("scenes")] = scenesArray;
    root[QStringLiteral("consistencyBoard")] = consistencyArray;
    root[QStringLiteral("audioPath")] = storedAudio;
    // The vanishing points in frame space (version 2, batch 6a).
    root[QStringLiteral("perspective")] =
        perspectiveMoved(data.perspective, -projectMargin.left(), -projectMargin.top());
    // Symmetry's switch and axis (batch H), in frame space already. Only when there is
    // something to say: without the key a reader gets "off, in the middle".
    if (!data.symmetry.isEmpty())
        root[QStringLiteral("symmetry")] = data.symmetry;
    // The tracks' lock / visibility / mute flags (TimelineTracks.h). Always
    // written; a reader without the key gets the defaults.
    root[QStringLiteral("tracks")] = data.tracks.toJson();

    // Reached only with every image on disk: res.ok is still true, so the
    // caller is clear to write the manifest.
    res.root = root;
    return res;
}

// --- THE PROJECT FILE IS THE ONE SWITCH (saving step 1, 2026-11-23; ProjectIO.h) -----------

QString previousVersionFile(const QString &projectFilePath)
{
    return QFileInfo(projectFilePath).absolutePath() + QStringLiteral("/") + assetSubdirFor(projectFilePath)
        + QStringLiteral("/_previous_saved_version.json");
}

PreviousVersion readPreviousVersion(const QString &projectFilePath)
{
    PreviousVersion out;
    QFile f(previousVersionFile(projectFilePath));
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return out;
    out.root = doc.object();
    out.savedAt = QDateTime::fromString(
        out.root.value(QStringLiteral("previousVersion")).toObject().value(QStringLiteral("savedAt")).toString(),
        Qt::ISODateWithMs);
    out.root.remove(QStringLiteral("previousVersion")); // the project file as it was
    const QString folder = QFileInfo(projectFilePath).absolutePath();
    const NamedImages images = namedImages(out.root);
    for (const QString &name : images.named)
        out.missingFiles += QFileInfo(folder + QStringLiteral("/") + name).isFile() ? 0 : 1;
    out.exists = true;
    return out;
}

bool savedImageNames(const QString &projectFilePath, QSet<QString> *names)
{
    const QString ownPrefix = assetSubdirFor(projectFilePath).toLower() + QLatin1Char('/');
    auto take = [&](const QJsonObject &root) {
        for (const QString &name : namedImages(root).named) // lower-cased already
            if (name.startsWith(ownPrefix))
                names->insert(name.mid(ownPrefix.size()));
    };
    QFile f(projectFilePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
        return false;
    take(doc.object());
    const PreviousVersion previous = readPreviousVersion(projectFilePath);
    if (previous.exists)
        take(previous.root);
    return true;
}

bool projectFileRefused(const QString &projectFilePath, QString *systemSays)
{
    const QFileInfo info(projectFilePath);
    if (!info.isFile())
        return false; // none yet, or something else in its place: the switch's to meet
    if (!info.isWritable()) {
        if (systemSays)
            *systemSays = QStringLiteral("The file is read-only.");
        return true;
    }
#ifdef Q_OS_WIN
    // Opened for writing as it stands - nothing written, so its bytes and its time stay -
    // and shared, so a file another program merely has open is not taken for a refusal.
    const std::wstring native = nativePath(projectFilePath);
    const HANDLE h = CreateFileW(native.c_str(), GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED)
            return false;
        if (systemSays)
            *systemSays = qt_error_string(int(error));
        return true;
    }
    CloseHandle(h);
#endif
    return false;
}

SwitchResult switchProjectFile(const QString &projectFilePath, const QJsonObject &root, const SaveOptions &options)
{
    SwitchResult res;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (saveHooks().overwriteInPlace.load()) {
        // The writer before 2026-11-23, for the checks' red run: QSaveFile, nothing else.
        QSaveFile file(projectFilePath);
        if (!file.open(QIODevice::WriteOnly)) {
            res.failure = SwitchFailure::CannotOpen;
            res.systemSays = file.errorString();
            return res;
        }
        if (file.write(json) != json.size()) {
            file.cancelWriting();
            res.failure = SwitchFailure::ShortWrite;
            return res;
        }
        if (!file.commit()) {
            res.failure = SwitchFailure::CannotComplete;
            res.systemSays = file.errorString();
            return res;
        }
        res.ok = true;
        res.stopped = !stepDone(SaveStep::ProjectFileSwitched, projectFilePath);
        return res;
    }
    if (writerStopped()) {
        res.stopped = true;
        return res;
    }

    // What is being replaced: the version this save makes the previous one. (The short
    // retry, saving step 2: a project file that is there and cannot be read just now -
    // another program holds it - is tried again like a name, tryAgain.)
    QByteArray oldBytes;
    QDateTime oldSavedAt;
    bool had = false;
    for (int made = 1;; ++made) {
        QFile f(projectFilePath);
        if (f.open(QIODevice::ReadOnly)) {
            oldBytes = f.readAll();
            oldSavedAt = QFileInfo(projectFilePath).lastModified();
            had = true;
            break;
        }
        if (!QFileInfo(projectFilePath).isFile() || !tryAgain(projectFilePath, made))
            break;
    }
    // A save that changes nothing replaces nothing - or the previous saved version would
    // become a second copy of this one.
    if (had && oldBytes == json) {
        res.ok = true;
        res.unchanged = true;
        return res;
    }
    // A project file that may not be written is refused before anything is made, as it
    // always was (QSaveFile's own rule).
    if (QFileInfo::exists(projectFilePath) && !QFileInfo(projectFilePath).isWritable()) {
        res.failure = SwitchFailure::CannotOpen;
        res.systemSays = QStringLiteral("Existing file %1 is not writable").arg(projectFilePath);
        return res;
    }

    // 1. THE PROJECT FILE: whole beside itself, pushed through, then in the old one's place.
    const QString part = projectFilePath + QStringLiteral(".part");
    {
        QFile out(part);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            res.failure = SwitchFailure::CannotOpen;
            res.systemSays = out.errorString();
            return res;
        }
        if (out.write(json) != json.size()) {
            res.systemSays = out.errorString();
            out.close();
            QFile::remove(part);
            res.failure = SwitchFailure::ShortWrite;
            return res;
        }
        if (!stepDone(SaveStep::ProjectFileWritten, part)) {
            res.stopped = true;
            return res;
        }
        if (!pushToDisk(out)) {
            res.systemSays = out.errorString();
            out.close();
            QFile::remove(part);
            res.failure = SwitchFailure::CannotComplete;
            return res;
        }
        if (!saveHooks().skipPush.load() && !stepDone(SaveStep::ProjectFilePushed, part)) {
            res.stopped = true;
            return res;
        }
    }
    QString why;
    if (!giveName(part, projectFilePath, true, &why, &res.tries, &res.held)) {
        QFile::remove(part);
        res.failure = SwitchFailure::CannotComplete;
        res.systemSays = why;
        return res;
    }
    res.ok = true; // THE SAVE HAS COMPLETED. Nothing below can unsave it.
    if (!stepDone(SaveStep::ProjectFileSwitched, projectFilePath)) {
        res.stopped = true;
        return res;
    }

    // 2. THE PREVIOUS SAVED VERSION - replaced only now, after the save has completed.
    const QString folder = QFileInfo(projectFilePath).absolutePath();
    const QString assets = assetSubdirFor(projectFilePath);
    const QString previousFile = previousVersionFile(projectFilePath);
    QVector<NamedImages> droppingOut; // the versions nothing will name after this
    NamedImages older;                // the previous saved version as this save found it
    bool hasOlder = false;
    {
        QFile f(previousFile);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                older = namedImages(doc.object());
                hasOlder = true;
            }
        }
    }
    const QJsonDocument oldDoc = had ? QJsonDocument::fromJson(oldBytes) : QJsonDocument();
    const bool replacedOne = had && oldDoc.isObject(); // a project file that opens was replaced
    NamedImages previous; // what the kept previous version names
    if (options.keepPrevious && replacedOne) {
        QJsonObject previousRoot = oldDoc.object();
        QJsonObject header;
        header[QStringLiteral("savedAt")] = oldSavedAt.toString(Qt::ISODateWithMs);
        header[QStringLiteral("replacedAt")] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        previousRoot[QStringLiteral("previousVersion")] = header;
        const Whole made = writeWhole(previousFile, QJsonDocument(previousRoot).toJson(QJsonDocument::Indented), true,
                                      SaveStep::PreviousWritten, SaveStep::PreviousPushed, SaveStep::PreviousNamed,
                                      &why);
        if (made == Whole::Stopped) {
            res.stopped = true;
            return res;
        }
        if (made != Whole::Done)
            return res; // the one before it stays, whole: none of its pictures may go
        res.previousKept = true;
        previous = namedImages(oldDoc.object());
        if (hasOlder)
            droppingOut.append(older);
    } else if (options.keepPrevious) {
        // NOTHING THAT OPENS WAS REPLACED - a first save, a Save As to a new place, a project
        // file that was not one: a previous saved version that lies there stays what it is,
        // with its pictures. (It is the last version known to open.)
        if (hasOlder)
            previous = older;
    } else {
        // "Delete the previous pictures right away": what was replaced drops out too.
        if (QFileInfo::exists(previousFile)) {
            if (!QFile::remove(previousFile))
                return res; // it still names its pictures: they stay
            if (!stepDone(SaveStep::PreviousRemoved, previousFile)) {
                res.stopped = true;
                return res;
            }
        }
        if (hasOlder)
            droppingOut.append(older);
        if (replacedOne)
            droppingOut.append(namedImages(oldDoc.object()));
    }

    // 3. THE SUPERSEDED PICTURES, last. Only in this project's own images folder; only a
    // file that neither the new version nor the kept previous one names.
    const NamedImages now = namedImages(root);
    const QString ownPrefix = assets.toLower() + QLatin1Char('/');
    auto unnamed = [&](const QString &lower) {
        return lower.startsWith(ownPrefix) && !now.named.contains(lower) && !previous.named.contains(lower);
    };
    QStringList doomed;
    // (a) what a version dropping out named for an image the new version has in another
    // file. An image that is gone - a deleted panel's, a removed layer's - keeps its files:
    // those are Clean Up Project Folder's, on the artist's word.
    for (const NamedImages &old : droppingOut)
        for (auto it = old.byKey.cbegin(); it != old.byKey.cend(); ++it) {
            const QString lower = it.value().toLower();
            const auto here = now.byKey.constFind(it.key());
            if (here != now.byKey.cend() && here.value().toLower() != lower && unnamed(lower)
                && !doomed.contains(it.value(), Qt::CaseInsensitive))
                doomed.append(it.value());
        }
    // (b) what an interrupted save left of an image the new version has: another "~n" of
    // its file's name that nothing names.
    QSet<QString> bases;
    for (auto it = now.byKey.cbegin(); it != now.byKey.cend(); ++it)
        if (it.value().toLower().startsWith(ownPrefix))
            bases.insert(imageBaseName(it.value().mid(ownPrefix.size())).toLower());
    for (const QFileInfo &fi : QDir(folder + QStringLiteral("/") + assets).entryInfoList(QDir::Files, QDir::Name)) {
        const QString name = fi.fileName();
        const QString relative = assets + QLatin1Char('/') + name;
        // ("<name>.part": a file a save died in the middle of.)
        const QString whole = name.endsWith(QStringLiteral(".part"), Qt::CaseInsensitive) ? name.chopped(5) : name;
        if (bases.contains(imageBaseName(whole).toLower()) && unnamed(relative.toLower())
            && !doomed.contains(relative, Qt::CaseInsensitive))
            doomed.append(relative);
    }
    doomed.sort();
    for (const QString &name : doomed) {
        const QString file = folder + QStringLiteral("/") + name;
        if (!QFile::remove(file))
            continue; // open in another program, say: it stays, a later save tries again
        res.removedFiles.append(file);
        if (!stepDone(SaveStep::PictureRemoved, file)) {
            res.stopped = true;
            return res;
        }
    }
    return res;
}

QString saveStepName(SaveStep step)
{
    switch (step) {
    case SaveStep::ImageWritten: return QStringLiteral("image written");
    case SaveStep::ImagePushed: return QStringLiteral("image pushed");
    case SaveStep::ImageNamed: return QStringLiteral("image named");
    case SaveStep::ImageOverwritten: return QStringLiteral("image overwritten");
    case SaveStep::ProjectFileWritten: return QStringLiteral("project file written");
    case SaveStep::ProjectFilePushed: return QStringLiteral("project file pushed");
    case SaveStep::ProjectFileSwitched: return QStringLiteral("project file switched");
    case SaveStep::PreviousWritten: return QStringLiteral("previous written");
    case SaveStep::PreviousPushed: return QStringLiteral("previous pushed");
    case SaveStep::PreviousNamed: return QStringLiteral("previous named");
    case SaveStep::PreviousRemoved: return QStringLiteral("previous removed");
    case SaveStep::PictureRemoved: return QStringLiteral("picture removed");
    }
    return QStringLiteral("?");
}

void setSaveLogForTest(QVector<SaveStepRecord> *log)
{
    SaveHooks &h = saveHooks();
    QMutexLocker lock(&h.mutex);
    h.log = log;
}

void setSaveStopAfterForTest(int steps)
{
    SaveHooks &h = saveHooks();
    QMutexLocker lock(&h.mutex);
    h.stopAfter = steps;
    h.steps = 0;
    h.dead = false;
}

void setSaveHoldAtForTest(int step, std::function<void()> hold)
{
    SaveHooks &h = saveHooks();
    QMutexLocker lock(&h.mutex);
    h.holdAt = step;
    h.hold = std::move(hold);
    h.steps = 0;
    h.dead = false;
}

void setSaveSkipPushForTest(bool skip)
{
    saveHooks().skipPush.store(skip);
}

void setOverwriteInPlaceForTest(bool on)
{
    saveHooks().overwriteInPlace.store(on);
}

void setNameRetryHookForTest(std::function<void(const QString &file, int triesMade)> hook)
{
    SaveHooks &h = saveHooks();
    QMutexLocker lock(&h.mutex);
    h.retryHook = std::move(hook);
}

void setNameRetryOffForTest(bool off)
{
    saveHooks().retryOff.store(off);
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
                    for (const QJsonValue &lv : layers) {
                        want(lv.toObject().value(QStringLiteral("imageFile")).toString());
                        // ...and each drawing's of a run (version 7; batch I-2).
                        for (const QJsonValue &dv : lv.toObject().value(QStringLiteral("drawings")).toArray())
                            want(dv.toObject().value(QStringLiteral("imageFile")).toString());
                    }
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
    // Version 2 (batch 6a): keys and vanishing points in frame space, a
    // margin per panel. 1, or none: stage px. (A later one is refused before
    // this is reached - MainWindow.)
    const int fileVersion = root.value(QStringLiteral("version")).toInt(1);
    QVector<QVector<QMargins>> fileMargins; // each panel's "margin", as the file says
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
    out.symmetry = root.value(QStringLiteral("symmetry")).toObject(); // batch H; absent: empty
    // No "tracks" key (every project saved before 2026-10-18): unlocked,
    // visible, unmuted - exactly what those projects were.
    out.tracks = TimelineTracks::fromJson(root.value(QStringLiteral("tracks")).toObject());

    // LEGACY shared layers ("Copy/Reuse Layer in Another Panel", removed):
    // old files store the PNG only on the FIRST instance of a sharedId;
    // later instances carry sharedId with no imageFile. MIGRATION: each such
    // reference becomes an INDEPENDENT real copy (own pixels, fresh id), so
    // old projects load with nothing missing. New saves drop sharedId.
    QHash<QString, QImage> legacySharedImages;
    // A version-3 layer's file holds its drawn part, placed in frame space: the
    // layer is at rest from the start, and these are folded in parallel at the
    // end, when the panels' stages are final.
    struct PlacedLayer
    {
        Panel *panel = nullptr;
        int index = 0;
        QImage crop;
        QPoint origin;
        quint32 fill = 0;
    };
    QVector<PlacedLayer> placed;
    // A RUN'S FURTHER DRAWINGS (version 7; batch I-2), each read into a stand-in layer after
    // its own: which layer's which drawing a stand-in is; the file's active layer for a panel
    // with any; and such a panel's flatten, said to the ledger once the stand-ins have gone.
    struct RunStandIn
    {
        Panel *panel = nullptr;
        QString ownerId;
        int drawing = 0;
        QString standInId;
    };
    QVector<RunStandIn> runStandIns;
    QHash<Panel *, int> runActive;
    struct RunFlatten
    {
        QString file;
        Panel *panel = nullptr;
    };
    QVector<RunFlatten> runFlattens;
    // The files read whole (versions 1 and 2): their place is the stage's, known
    // only once the panels' margins are.
    struct ReadWhole
    {
        QString file;
        QByteArray content;
        Panel *panel = nullptr;
    };
    QVector<ReadWhole> readWhole;
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
        scene->lengthLocked = sceneObj.value(QStringLiteral("lengthLocked")).toBool(false); // F1
        scene->colorTag = sceneObj.value(QStringLiteral("colorTag")).toString(); // batch E: empty = Auto
        if (!scene->colorTag.isEmpty() && !QColor(scene->colorTag).isValid())
            scene->colorTag.clear(); // a colour that cannot be read is Auto, not black

        const QJsonArray panelsArray = sceneObj.value(QStringLiteral("panels")).toArray();
        fileMargins.append(QVector<QMargins>());
        for (const QJsonValue &pv : panelsArray) {
            const QJsonObject panelObj = pv.toObject();
            Panel *panel = new Panel;
            {
                const QJsonArray m = panelObj.value(QStringLiteral("margin")).toArray();
                fileMargins.last().append(m.size() == 4 ? QMargins(m.at(0).toInt(), m.at(1).toInt(),
                                                                   m.at(2).toInt(), m.at(3).toInt())
                                                        : QMargins());
            }
            // The stable id (2026-11-04): the file's, when it is one (a UUID
            // without braces, as written); otherwise the fresh one the Panel
            // was made with, counted, saved on the next save.
            {
                static const QRegularExpression uuid(
                    QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
                const QString id = panelObj.value(QStringLiteral("id")).toString();
                if (uuid.match(id).hasMatch())
                    panel->id = id.toLower();
                else
                    ++out.panelIdsMinted;
            }
            // THE LENGTH (2026-11-06, batch 5): "frames" at the file's rate;
            // a file from before it has "duration" in whole seconds, which
            // keeps its time exactly (seconds x fps). Neither, or a length
            // below 1, is the old default: 3 s.
            {
                const int fileFps = qMax(1, out.fps);
                int frames = 0;
                if (panelObj.contains(QStringLiteral("frames")))
                    frames = panelObj.value(QStringLiteral("frames")).toInt(0);
                else if (panelObj.contains(QStringLiteral("duration")))
                    frames = panelObj.value(QStringLiteral("duration")).toInt(0) * fileFps;
                if (frames < 1)
                    frames = 3 * fileFps;
                panel->setLength(frames, fileFps);
            }
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
                    // Depth (Phase 2B): absent = the picture plane. A value
                    // that cannot be one (not a number, out of 0 .. 2) is
                    // the picture plane, counted.
                    if (layerObj.contains(QStringLiteral("parallax"))) {
                        const QJsonValue pv = layerObj.value(QStringLiteral("parallax"));
                        const double a = pv.toDouble(-1.0);
                        if (pv.isDouble() && std::isfinite(a) && a >= kMinParallax
                            && a <= kMaxParallax) {
                            layer.parallax = a;
                        } else {
                            layer.parallax = 1.0;
                            ++out.parallaxRepaired;
                        }
                    }
                    // Layer motion (batch I-1): absent = none.
                    if (layerObj.contains(QStringLiteral("motion")))
                        out.motionKeysDropped +=
                            layerMotionFromJson(layerObj.value(QStringLiteral("motion")).toObject(), &layer);

                    // A RUN OF DRAWINGS (version 7; batch I-2): "drawings", two or more. Each
                    // drawing's pixels are read exactly as a layer's are - by the code below, run
                    // once per drawing: the first into the layer itself (a layer shows its first
                    // drawing when the project opens), each further one into a STAND-IN layer put
                    // after it, which is placed and folded with the rest and at the end of the
                    // load handed to the layer as that drawing's pixels (runStandIns). A start
                    // that does not rise is moved on to the frame after the one before it; a file
                    // that cannot be read is a blank drawing, as a layer's is a blank layer.
                    QVector<QJsonObject> pixelEntries{layerObj};
                    if (fileVersion >= kFileVersionLayerDrawings && layer.type != QLatin1String("group")) {
                        const QJsonArray drawingsArray = layerObj.value(QStringLiteral("drawings")).toArray();
                        if (!drawingsArray.isEmpty()) {
                            pixelEntries.clear();
                            for (const QJsonValue &dv : drawingsArray)
                                pixelEntries.append(dv.toObject());
                        }
                        if (drawingsArray.size() >= 2) {
                            layer.runRate = qMax(1, layerObj.value(QStringLiteral("runRate")).toInt(24));
                            for (const QJsonObject &drawingObj : std::as_const(pixelEntries)) {
                                LayerDrawing drawing;
                                drawing.id = drawingObj.value(QStringLiteral("id")).toString();
                                if (drawing.id.isEmpty())
                                    drawing.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                                drawing.start = layer.drawings.isEmpty()
                                    ? 0
                                    : qMax(layer.drawings.last().start + 1,
                                           drawingObj.value(QStringLiteral("start")).toInt(0));
                                layer.drawings.append(drawing);
                            }
                        }
                    }
                    const Layer asRead = layer; // every field but the pixels
                    for (int entry = 0; entry < pixelEntries.size(); ++entry) {
                    // (The two names below hide the layer's own ON PURPOSE: what follows is the
                    // code that has always read a layer's pixels, now run once per drawing, and
                    // left where it was.)
                    const QJsonObject layerObj = pixelEntries.at(entry);
                    Layer layer = asRead;
                    if (entry > 0) {
                        layer.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        layer.drawings.clear();
                        layer.keys.clear();
                        layer.animate = false;
                        runStandIns.append({panel, asRead.id, entry, layer.id});
                    }

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
                    const QJsonArray originArray = layerObj.value(QStringLiteral("origin")).toArray();
                    const bool isPlaced = fileVersion >= 3 && originArray.size() == 2 && !layer.image.isNull();
                    QPoint origin;
                    if (isPlaced) {
                        origin = QPoint(originArray.at(0).toInt(), originArray.at(1).toInt());
                        const QMargins fileMargin = fileMargins.last().last();
                        PlacedLayer one;
                        one.panel = panel;
                        one.index = int(panel->layers.size());
                        one.crop = layer.image;
                        one.origin = origin;
                        one.fill = layerObj.value(QStringLiteral("fill")).toString().toUInt(nullptr, 16);
                        // Its stage for now: the sizes are read from it below. Its
                        // pixels land at the end.
                        layer.tiles = layertiles::empty(one.fill, one.crop.cacheKey());
                        const QJsonArray sizeArray = layerObj.value(QStringLiteral("size")).toArray();
                        const QSize saidSize(sizeArray.at(0).toInt(), sizeArray.at(1).toInt());
                        layer.stageRect = QRect(QPoint(-fileMargin.left(), -fileMargin.top()),
                                                saidSize.isValid() && !saidSize.isEmpty()
                                                    ? saidSize
                                                    : out.manifestSize.isValid() && !out.manifestSize.isEmpty()
                                                        ? out.manifestSize
                                                        : one.crop.size());
                        layer.image = QImage();
                        placed.append(std::move(one));
                    }
                    panel->layers.append(layer);
                    if (readOwnFile && layer.hasPixels()) {
                        const QString file = folder + QStringLiteral("/") + layerPng;
                        if (isPlaced)
                            out.images.record(file, layerContent(layer), origin);
                        else
                            readWhole.append({file, layerContent(layer), panel});
                        readKeys.insert(layer.contentKey());
                        if (!manifestTime.isValid()
                            || QFileInfo(file).lastModified() > manifestTime)
                            readNewerThanManifest = true;
                    }
                    } // each drawing of a run; once for a layer without one
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
                    if (!layer.hasPixels()
                        && layer.type != QLatin1String("group"))
                        layer.image = makeLayerImage(panelPx);
            }
            panel->activeLayerIndex =
                qBound(0, panelObj.value(QStringLiteral("activeLayerIndex")).toInt(0),
                       panel->layers.size() - 1);
            // (With a run's stand-ins among the layers the file's index is not this list's:
            // it is set again once they have gone - runActive.)
            const bool panelHasStandIns = !runStandIns.isEmpty() && runStandIns.last().panel == panel;
            if (panelHasStandIns)
                runActive.insert(panel, panelObj.value(QStringLiteral("activeLayerIndex")).toInt(0));
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
                    if (!isGroupLayer(layer) && !readKeys.contains(layer.contentKey()))
                        allRead = false;
                // (A panel with a run: said once its stand-ins have gone, when the panel's
                // layers are its own again - runFlattens.)
                if (allRead && panelHasStandIns)
                    runFlattens.append({flatFile, panel});
                else if (allRead)
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
    // A VERSION-4 FILE'S PANELS (batch 6c) differ in size by their own margins, which
    // are no disagreement: each is counted at the size it would have at the project's
    // margin, what the file's root says less what its own margin adds.
    const QSize fileManifest(root.value(QStringLiteral("canvasWidth")).toInt(0),
                             root.value(QStringLiteral("canvasHeight")).toInt(0));
    const QSize fileFrame = root.contains(QStringLiteral("frameWidth"))
        ? QSize(root.value(QStringLiteral("frameWidth")).toInt(0),
                root.value(QStringLiteral("frameHeight")).toInt(0))
        : fileManifest;
    const auto censusSize = [&](int sceneIndex, int panelIndex) {
        const QSize size = out.scenes.at(sceneIndex)->panels.at(panelIndex)->canvasSize();
        if (fileVersion < kFileVersion || !size.isValid() || sceneIndex >= fileMargins.size()
            || panelIndex >= fileMargins.at(sceneIndex).size())
            return size;
        const QMargins said = fileMargins.at(sceneIndex).at(panelIndex);
        return QSize(size.width() - said.left() - said.right() + fileManifest.width() - fileFrame.width(),
                     size.height() - said.top() - said.bottom() + fileManifest.height() - fileFrame.height());
    };
    QVector<QPair<QSize, int>> census; // size -> count, in first-seen order
    for (int s = 0; s < out.scenes.size(); ++s) {
        for (int p = 0; p < out.scenes.at(s)->panels.size(); ++p) {
            const QSize size = censusSize(s, p);
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
        for (int s = 0; s < out.scenes.size(); ++s) {
            const Scene *scene = out.scenes.at(s);
            for (int i = 0; i < scene->panels.size(); ++i) {
                const QSize size = censusSize(s, i);
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
    // THE STAGE MARGIN (Phase 2A), against the artwork's REAL size: written
    // only with a margin, so absent keys are every older project - none.
    if (root.contains(QStringLiteral("frameWidth"))
        || root.contains(QStringLiteral("frameHeight"))) {
        const QSize frame(root.value(QStringLiteral("frameWidth")).toInt(0),
                          root.value(QStringLiteral("frameHeight")).toInt(0));
        const int dw = out.pixelSize.width() - frame.width();
        const int dh = out.pixelSize.height() - frame.height();
        if (frame.width() >= 1 && frame.height() >= 1 && dw >= 0 && dh >= 0
            && dw % 2 == 0 && dh % 2 == 0)
            out.stageMargin = QSize(dw / 2, dh / 2);
        else
            out.stageMarginRepaired = true;
    }
    // EACH PANEL'S STAGE (2026-11-06, batch 6a). A panel holds the project's
    // margin - the one its pixels and the frame give, as above: the pixels win
    // - unless a version-4 file gives it its own (batch 6c), and then only when
    // its layers are the frame plus that margin. A file that says otherwise is
    // counted (panelMarginsRepaired) and not obeyed.
    const QMargins projectMargin(out.stageMargin.width(), out.stageMargin.height(),
                                 out.stageMargin.width(), out.stageMargin.height());
    const QSize projectFrame(out.pixelSize.width() - 2 * out.stageMargin.width(),
                             out.pixelSize.height() - 2 * out.stageMargin.height());
    for (int s = 0; s < out.scenes.size(); ++s) {
        Scene *scene = out.scenes.at(s);
        for (int p = 0; p < scene->panels.size(); ++p) {
            Panel *panel = scene->panels.at(p);
            panel->margin = projectMargin;
            if (fileVersion < 2 || s >= fileMargins.size() || p >= fileMargins.at(s).size())
                continue;
            const QMargins said = fileMargins.at(s).at(p);
            if (said == projectMargin)
                continue;
            const bool fits = fileVersion >= kFileVersion && !out.stageMarginRepaired && said.left() >= 0
                && said.top() >= 0 && said.right() >= 0 && said.bottom() >= 0
                && panel->canvasSize()
                    == QSize(projectFrame.width() + said.left() + said.right(),
                             projectFrame.height() + said.top() + said.bottom());
            if (fits) {
                panel->margin = said;
                panel->settleMargin(out.stageMargin);
            } else {
                ++out.panelMarginsRepaired;
            }
        }
    }
    // KEYS AND VANISHING POINTS: frame space in a version-2 file, back to
    // the stage's px by the margin every panel holds (the project's, until
    // 6c). A version-1 file has them in stage px already. When the frame
    // could not be read and the stage opens without a margin, they are put
    // back by the margin the file says they were saved from (its panels'),
    // so they stay on their artwork as a version-1 file's do.
    if (fileVersion >= 2) {
        QMargins savedFrom = projectMargin;
        if (out.stageMarginRepaired && !fileMargins.isEmpty() && !fileMargins.first().isEmpty())
            savedFrom = fileMargins.first().first();
        for (Scene *scene : out.scenes)
            shotcam::keysToStage(*scene, QPointF(savedFrom.left(), savedFrom.top()));
        out.perspective = perspectiveMoved(out.perspective, savedFrom.left(), savedFrom.top());
    }
    // WHERE EACH WHOLE FILE LIES, now that the stages are final: a later save
    // that skips it still names its place.
    for (const ReadWhole &r : std::as_const(readWhole))
        out.images.record(r.file, r.content, -r.panel->frameAt());
    // THE PROJECT AT REST (batch 6b-V): every layer as its tiles, in parallel. Only
    // the panel put on the canvas is unfolded again, by the page. The file's frame
    // when it could be read; when it could not and the stage opened without a
    // margin, its place on the stage it was saved with, so the art stays put.
    {
        const QPoint shift = out.stageMarginRepaired && !fileMargins.isEmpty() && !fileMargins.first().isEmpty()
            ? QPoint(fileMargins.first().first().left(), fileMargins.first().first().top())
            : QPoint();
        struct Job
        {
            Layer *layer = nullptr;
            QImage image;
            QPoint frameAt;
            quint32 fill = 0;
            bool paper = false;
            qint64 key = 0;
            QRect stage;
        };
        std::vector<Job> jobs;
        for (const PlacedLayer &p : std::as_const(placed)) {
            Layer &layer = p.panel->layers[p.index];
            Job job;
            job.layer = &layer;
            job.image = p.crop;
            job.frameAt = -(p.origin + shift);
            job.fill = p.fill;
            job.key = p.crop.cacheKey();
            job.stage = QRect(-p.panel->frameAt(), layer.stageRect.size());
            jobs.push_back(std::move(job));
        }
        for (Scene *scene : std::as_const(out.scenes))
            for (Panel *panel : std::as_const(scene->panels))
                for (Layer &layer : panel->layers) {
                    if (isGroupLayer(layer) || layer.image.isNull() || !layertiles::foldable(layer.image))
                        continue;
                    Job job;
                    job.layer = &layer;
                    job.image = layer.image;
                    job.frameAt = panel->frameAt();
                    job.paper = layer.type == QLatin1String("background");
                    job.key = layer.image.cacheKey();
                    job.stage = QRect(-panel->frameAt(), layer.image.size());
                    jobs.push_back(std::move(job));
                }
        auto foldOne = [&jobs](size_t i) {
            Job &job = jobs[i];
            const quint32 fill = job.paper ? layertiles::paperFill(job.image, nullptr) : job.fill;
            job.layer->tiles = layertiles::fold(job.image, job.frameAt, fill, nullptr, job.key);
            job.layer->stageRect = job.stage;
            job.layer->image = QImage();
            job.layer->tilesOf = 0;
            job.image = QImage();
        };
        if (g_serialDecodeForTest || jobs.size() < 2) {
            for (size_t i = 0; i < jobs.size(); ++i)
                foldOne(i);
        } else {
            QThreadPool pool;
            pool.setMaxThreadCount(qBound(1, QThread::idealThreadCount(), 8));
            for (size_t i = 0; i < jobs.size(); ++i)
                pool.start([&foldOne, i] { foldOne(i); });
            pool.waitForDone();
        }
    }
    // THE RUNS' DRAWINGS GO HOME (batch I-2): each stand-in's pixels - read, placed and folded
    // as a layer's - become its drawing's, and the stand-in goes. The panel's layers are then
    // the file's, so its active layer and its flatten are said now.
    for (const RunStandIn &s : std::as_const(runStandIns)) {
        int owner = -1, standIn = -1;
        for (int i = 0; i < s.panel->layers.size(); ++i) {
            if (s.panel->layers.at(i).id == s.ownerId)
                owner = i;
            else if (s.panel->layers.at(i).id == s.standInId)
                standIn = i;
        }
        if (standIn < 0)
            continue;
        if (owner >= 0 && s.drawing < s.panel->layers.at(owner).drawings.size())
            s.panel->layers[owner].drawings[s.drawing].px =
                static_cast<const LayerPixels &>(s.panel->layers.at(standIn));
        s.panel->layers.removeAt(standIn);
    }
    for (auto it = runActive.cbegin(); it != runActive.cend(); ++it)
        it.key()->activeLayerIndex = qBound(0, it.value(), int(it.key()->layers.size()) - 1);
    for (const RunFlatten &f : std::as_const(runFlattens))
        out.images.record(f.file, flattenContent(*f.panel));
    return out;
}

} // namespace ProjectIO
