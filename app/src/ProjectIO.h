#pragma once

#include "StoryboardModel.h"
#include "TimelineTracks.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

// File-level project serialization, extracted from MainWindow with the
// resolution epic. Two reasons, both structural:
//  1. The pixels-win reconciliation (manifest vs artwork) is CORRECTNESS
//     logic, not UI — it belongs where it can be tested.
//  2. The verification seam must drive real save/load without constructing
//     MainWindow, whose page teardown persists dock/toolbar state into the
//     REAL registry (forbidden during verification).
// MainWindow keeps everything user-facing: file dialogs, the mismatch
// dialog, page updates, recents. This owns bytes, JSON, and sibling PNGs.
namespace ProjectIO {

// WHAT EACH IMAGE FILE ON DISK HOLDS (the save freeze, 2026-10-27).
//
// Every save used to re-encode every panel flatten and every layer as a
// PNG on the UI thread, changed or not: 54 images at 3840x2160 froze the
// window for 25 seconds on each Ctrl+S. The ledger lets a save skip an
// image whose file already holds it.
//
// It is keyed by FILE, never by panel or layer. Image files are named by
// POSITION, so a panel deleted and brought back by Undo lands on a file
// that another panel has been written to since; a ledger keyed by panel
// would call it unchanged and leave the other panel's pixels in its file.
// Keyed by file it asks the only question that matters: does THIS file
// hold THIS image?
//
// The content is an identity, not a hash: a layer's QImage::cacheKey()
// (ser_no << 32 | detach_no: unique for the life of the process, and
// changed by QPainter::begin, bits()/scanLine() and assignment - the paint
// engine's own authority check already rests on this), and for a flatten
// every input flattenedPixmap() reads. An entry also keeps the file's size
// and modification time as this process left them, so a file deleted, or
// written since by anything else - another program, another project - no
// longer matches and is written again. A wrong match would cost an artist
// their work and a missed one costs a write, so every doubt writes.
class ImageLedger
{
public:
    // True only when `file` was last written or read by this process as
    // `content` and is exactly as it was left: a file, same size, same
    // modification time.
    bool holds(const QString &file, const QByteArray &content) const;
    // `file` now holds `content`: it was just written, or just read.
    // Records nothing for a file that is not there. `origin`: for a layer's
    // file, where its pixels lie in frame space (version 3): a file skipped
    // or renamed is still named with the place it holds.
    void record(const QString &file, const QByteArray &content, const QPoint &origin = QPoint());
    QPoint originOf(const QString &file) const;
    void forget(const QString &file);
    void merge(const ImageLedger &other);
    int size() const { return int(m_entries.size()); }
    // A file under `folder` (any depth) recorded as holding `content` and
    // still as it was left: the old name of an image that is only being
    // renamed (the id names, 2026-11-04). Empty when none.
    QString fileHolding(const QByteArray &content, const QString &folder) const;
    // Every such file, sorted by name (the safe save, 2026-11-23: an image may be in its
    // own file or in an earlier one of its own).
    QStringList filesHolding(const QByteArray &content, const QString &folder) const;

private:
    struct Entry
    {
        QByteArray content;
        qint64 bytes = -1;
        QDateTime modified;
        QPoint origin;
    };
    static QString keyFor(const QString &file);
    QHash<QString, Entry> m_entries;
};

// The identities the ledger compares: a layer is its image; a panel's
// flatten is everything flattenedPixmap() reads.
QByteArray layerContent(const Layer &layer);
QByteArray flattenContent(const Panel &panel);
// A layer's motion (batch I-1) in the file's form (version 6, below), and back: the keys
// that could not be one are left out, and their number returned.
QJsonObject layerMotionJson(const Layer &layer);
int layerMotionFromJson(const QJsonObject &motion, Layer *layer);

// THE RECOVERY COPY'S IMAGES, OFF THE UI THREAD (2026-11-05). With a plan in
// SaveData, projectToJson composites and encodes nothing: each image that
// must be written goes into the plan - a layer or a thumbnail as a shared
// copy of its QImage, a flatten as a snapshot of its panel's layers (shared
// copies too) - and runImagePlan writes them, on whatever thread calls it.
// The JSON, the names, the ledger's skips and renames are still decided on
// the caller's thread; recording what landed in the ledger is the caller's,
// after runImagePlan, back on its own thread. The snapshots must be released
// on the UI thread (a Panel holds QPixmap caches), so the plan's owner keeps
// it until then.
struct ImageJob
{
    QString file;                         // absolute
    QByteArray content;                   // the ledger's identity
    QImage image;                         // a layer or a thumbnail
    layertiles::TilesPtr tiles;           // a layer at rest: `rect` of these, made where it runs
    QRect rect;
    QPoint origin;                        // a layer's place in frame space (the ledger's)
    std::shared_ptr<const Panel> flatten; // a flatten: its panel's layers
    bool ok = false;
    Qt::HANDLE composeThread = nullptr;   // where a flatten was composited
    Qt::HANDLE encodeThread = nullptr;    // where it was encoded and written
    // A SAVE'S IMAGE (saving step 2, 2026-11-23; runSavePlan): `file` is a name nothing has,
    // and the image is written whole, pushed through and named, as every file of the safe
    // save is. `fold`: a layer that is still a full image (the panel on the canvas, edited)
    // - `image` is reduced to its drawn part where the job runs, with `tiles` the layer's
    // tiles so far, `frameAt` the frame's top-left in the image and `paper` for a
    // Background; the place it then has (`origin`) and a Background's `fill` are written
    // into the project file's text at scenes[scene].panels[panel].layers[layer].
    bool fold = false;
    QPoint frameAt;
    bool paper = false;
    int scene = -1, panel = -1, layer = -1;
    quint32 fill = 0;
    QString why;                          // a failure's reason, as the writer and the file gave it
    bool tried = false;                   // the job ran (false: an earlier one had failed)
    bool stopped = false;                 // a check stopped the writer in it
    Qt::HANDLE foldThread = nullptr;      // where a full image was reduced to its drawn part
};
struct ImagePlan
{
    QVector<ImageJob> jobs;
};
// Writes every job - "<name>.part", then renamed over its name, so a file
// under its name is always whole - several at once, and starts no new one
// once `cancel` is set. True when every job was written.
bool runImagePlan(ImagePlan &plan, const std::atomic<bool> &cancel);

// THE SAVE IN THE BACKGROUND (saving step 2, 2026-11-23; the report of that day, approved).
// A Save used to composite its flattens on the UI thread and then stand still there until
// its encoders had written every image, and write the project file there too: the window
// was frozen from Ctrl+S to the last file. Now a Save is made as the recovery copy has been
// since 2026-11-05. ON THE CALLER'S THREAD, projectToJson with SaveData::plan and
// SaveData::planIsSave: the names (settle), the project file's text and the ledger's skips,
// and every image that must be written as a job of shared copies - nothing composited,
// nothing reduced, nothing encoded, nothing written. ON WHATEVER THREAD RUNS IT, runSavePlan:
// each job's flatten composited, each full layer reduced to its drawn part, every image
// encoded, written whole, pushed through to the disk and named - then the caller's
// switchProjectFile. The pictures are the bytes the Save wrote before (a flatten without
// an alpha channel, as QPixmap::toImage gave it). Recording what landed in the ledger is
// the caller's, back on its own thread, where the plan must also be released.
struct [[nodiscard]] PlanResult
{
    bool ok = false;
    bool stopped = false;     // a check stopped the writer (setSaveStopAfterForTest)
    QString failedFile;       // the earliest image in the save's order that could not be written
    QString reason;           // why, in words an artist can act on
    int imagesWritten = 0;
    QStringList writtenFiles; // every image the plan wrote (absolute), in the save's order
    int encodeThreads = 0;    // distinct threads that encoded them
    // Where the work ran, one entry per step: each full layer reduced, each flatten
    // composited, each image encoded and written.
    QVector<Qt::HANDLE> foldThreadIds;
    QVector<Qt::HANDLE> composeThreadIds;
    QVector<Qt::HANDLE> encodeThreadIds;
};
// `root`: the project file's text as projectToJson gave it; the places and fills of the
// layers reduced here are written into it (only when every image landed).
PlanResult runSavePlan(ImagePlan &plan, QJsonObject *root);

// THE FILE'S VERSION (2026-11-06, batch 6a). 2: each panel says its stage
// margin ("margin": [left, top, right, bottom], omitted when 0), and the
// camera keys and the vanishing points are in FRAME SPACE - px from the
// shot frame's top-left, the same numbers whatever a panel's margin. 1
// (and none): stage px, the project's one margin. MainWindow refuses a later
// version than this build's rather than misread it. A project without a
// margin has the same numbers in both.
//
// VERSION 3 (batch 6b-V, 2026-11-07): a layer's file holds only what is drawn,
// placed in frame space: "origin": [x, y], its top-left from the frame's. Art
// outside the stage is in it too, kept and hidden (Q13). A Background also says
// "fill", the colour of the rest of it, and a plain one's file is one pixel. A
// layer's file may still be the whole stage (the panel on the canvas, saved as it
// is, with its place). A 6a build refuses this file.
//
// VERSION 4 (batch 6c, 2026-11-07): a panel's "margin" is obeyed, when its layers
// are the frame plus that margin; each of its layers' places follows it. Only a
// project with a panel at its own margin is written so, and an older build refuses
// it rather than give that panel the project's margin. A project without one stays
// the version 3 it was. Reads 1 to 4.
//
// VERSION 5 (batch F1, 2026-11-12): a scene may say "lengthLocked": true. Only a
// project with a locked scene is written so - an older build refuses it rather than
// drop the lock silently - and one without stays the version 3 or 4 it was. Its
// panels' margins are read as version 4's. Reads 1 to 5.
//
// VERSION 6 (batch I-1, 2026-11-24): a layer may say "motion": {"animate", "pivot": [x, y]
// in frame space, "keys": [{"at", "x", "y", "rotation", "scale", "opacity", "ease"}]} -
// its own keys (x and y how far it is moved, stage px). Only a project with a layer that
// has motion kept (Animate on, or keys) is written so - an older build refuses it rather
// than drop the motion silently - and one without stays the version 3, 4 or 5 it was. Its
// panels' margins are read as version 4's. Reads 1 to 6.
inline constexpr int kFileVersion = 4;
inline constexpr int kFileVersionOneMargin = 3;
inline constexpr int kFileVersionLockedScene = 5;
inline constexpr int kFileVersionLayerMotion = 6;
// The newest version this build reads (MainWindow refuses a later one).
inline constexpr int kFileVersionNewest = kFileVersionLayerMotion;
// The older form, for the checks' fixtures only: version 2, every layer's file the
// whole stage, with no "origin" or "fill".
void setWriteVersion2ForTest(bool on);
// The vanishing points of a "perspective" object moved by (dx, dy): the
// file's frame space and the stage's px (version 2). Nothing else in it
// changes.
QJsonObject perspectiveMoved(const QJsonObject &perspective, double dx, double dy);

struct SaveData
{
    QString projectName;
    int fps = 24;
    QSize canvasSize; // the project's real resolution (post-reconcile)
    // THE STAGE MARGIN (Phase 2A): px each side of the shot; 0 x 0 = none.
    // With one, the file says the shot's size as "frameWidth" /
    // "frameHeight" (the canvas is the stage); without one it writes
    // neither, so every project made before 2A saves the JSON it did.
    QSize stageMargin = QSize(0, 0);
    QVector<Scene *> scenes; // not owned
    QVector<ConsistencyEntry> consistency;
    // The audio track: WHERE ITS FILE IS NOW (absolute), empty for none.
    // audioLinked = an old project's reference to a file outside the
    // project, written back exactly as it was given and never copied. A
    // track that is not linked is the project's own: see projectToJson.
    QString audioPath;
    bool audioLinked = false;
    QJsonObject perspective;
    // SYMMETRY (batch H, 2026-11-22): the switch and the axis's place, as
    // StoryboardPage::symmetryToJson gives them. Written as "symmetry" only when not
    // empty - off, the axis in the frame's middle, is the manifest it always was.
    QJsonObject symmetry;
    TimelineTracks tracks; // lock / visibility / mute, written under "tracks"
    // What the image files already hold (see ImageLedger): the save skips
    // every image whose file holds it, and records each one it writes.
    // Null (the seam, the fixtures) writes every image, as before.
    ImageLedger *ledger = nullptr;
    // THE RECOVERY COPY (2026-11-05): images go into this plan instead of
    // being written here (see ImagePlan). A Save that writes on the caller's
    // thread leaves it null.
    ImagePlan *plan = nullptr;
    // THE SAVE IN THE BACKGROUND (saving step 2, 2026-11-23): the plan is a Save's -
    // the safe save's names, nothing written over - and runSavePlan writes it.
    // Needs the ledger. False with a plan: the recovery copy's, as before.
    bool planIsSave = false;
};

// The outcome of a save's IMAGE pass.
//
// Every write used to be unchecked — QDir::mkpath and every QImage::save
// had its result discarded — so a save that could not write its pixels
// still produced a complete, well-formed manifest NAMING those files. The
// project then reopened with blank layers AT THE RIGHT SIZE, because the
// loader deliberately fills a missing image at the panel's size so that a
// genuinely damaged old project still opens (see projectFromJson). Correct
// there; catastrophic here, because it made a failed save look exactly
// like a successful one.
//
// Returning the failure rather than a bare QJsonObject is what makes
// ignoring it impossible: there is no manifest to write until this says ok.
struct [[nodiscard]] WriteResult
{
    QJsonObject root;       // meaningful ONLY when ok is true
    bool ok = false;
    QString failedFile;     // the first file that could not be written
    QString reason;         // why, in words an artist can act on
    int imagesWritten = 0;  // before the failure, if any
    int imagesSkipped = 0;  // unchanged: their files already hold them
    QStringList writtenFiles; // every image this save wrote (absolute)
    // Images only RENAMED to their id name (the first save of a project
    // named by position, 2026-11-04): the file moved, nothing re-encoded.
    int imagesRenamed = 0;
    QStringList renamedFiles; // their new names (absolute)
    int encodeThreads = 0;  // distinct threads that encoded them
    // Where the work ran (2026-11-05), one entry per step: each flatten this
    // call composited (on the caller's thread) and each image it encoded.
    QVector<Qt::HANDLE> composeThreadIds;
    QVector<Qt::HANDLE> encodeThreadIds;
    // The audio copy was CANCELLED by the artist (Save As with a large
    // track): ok is false, and there is nothing to warn about.
    bool cancelled = false;
    // A check stopped the writer at one of its steps (setSaveStopAfterForTest): ok is false
    // and nothing was tidied - the disk is as a process that died there would leave it.
    bool stopped = false;
    // Where the project's own audio track is once this save is on disk
    // (absolute; empty for no track or a linked one). After a Save As it is
    // the NEW project's copy, which the caller should now play.
    QString audioFile;
};

// Build the project's JSON root AND write its images (panel flattens +
// per-layer images + consistency thumbnails). The flatten PNGs follow
// flattenedPixmap(), which is panel-sized — so what lands on disk always
// matches the manifest beside it.
//
// STOPS AT THE FIRST FAILURE and reports it. Writing the rest would only
// spread a half-save further, and the manifest is not written either way:
// the caller must leave the previous project file exactly as it was.
//
// Takes the PROJECT FILE PATH, not its folder, and that is deliberate: the
// images go into "<basename>_assets/" beside the .sankotv, and the basename
// is the only thing guaranteed unique within a directory. Passing a folder
// would leave the subfolder name to the caller, and a caller that forgot —
// or that used data.projectName, which is NOT unique — would put two
// projects' pixels back in one place. Two projects saved into one folder
// used to write the SAME positionally-named files and silently overwrite
// each other's artwork; this signature is what makes that unrepeatable.
//
// Loading is unaffected: names are stored RELATIVE to the manifest, so an
// old project naming flat files still finds them exactly where it did.
//
// THE AUDIO TRACK (media policy, 2026-10-04) is carried the same way. A
// track the project owns is named RELATIVE to the manifest
// ("<basename>_assets/<file>"), and a save whose assets folder does not
// hold the file yet - Save As, a project file renamed by hand, a project
// that had no file when the audio was imported - COPIES IT IN FIRST, before
// any image and before the manifest (ProjectMedia::adopt: never overwrites,
// never leaves a half file). A copy that cannot be made fails the save like
// an image that cannot be written. Two cases are written back untouched:
// a LINKED track (an old project's absolute path - converted only by the
// artist, never by a save), and an owned track whose file is not there to
// copy (already shown as missing; the reference is carried, the save does
// not fail over a file nobody could have saved). audioProgress reports the
// copy and may cancel it (WriteResult::cancelled).
WriteResult projectToJson(const SaveData &data, const QString &projectFilePath,
                          const std::function<bool(qint64, qint64)> &audioProgress = {});

// The images subfolder a given project file uses ("<basename>_assets").
QString assetSubdirFor(const QString &projectFilePath);

// One panel that does not match the project's chosen size, located for the
// user: "Scene 2, panel 7" rather than a bare count.
struct OffSizePanel
{
    int sceneNumber = 0; // as displayed
    int panelIndex = 0;  // 1-based within its scene, as displayed
    QSize size;
};

struct LoadedProject
{
    QString projectName; // may be empty: caller falls back to the filename
    int fps = 24;
    QSize manifestSize;  // as stored (invalid/empty when keys absent)
    QSize pixelSize;     // the artwork's REAL size — the authority
    bool mismatch = false; // manifest present, valid, and != pixelSize
    // PANEL-vs-PANEL disagreement, a different axis from `mismatch` (which
    // is manifest-vs-artwork). A project whose panels are not all the same
    // size used to load silently at whichever size happened to come first;
    // now the disagreement is counted, located and reported. Empty and
    // false for every uniform project.
    bool mixedSizes = false;
    int majorityPanelCount = 0;        // panels at pixelSize
    QVector<OffSizePanel> offSizePanels; // every panel that is not
    QVector<Scene *> scenes; // caller takes ownership
    QVector<ConsistencyEntry> consistency;
    // The audio track as the manifest gives it, RESOLVED BY STRING WORK
    // ONLY (load never consults the disk to decide what a record says):
    // a relative name is the project's own file and comes back absolute,
    // under the project's folder; an absolute path is an old project's
    // LINKED file and comes back exactly as stored, with audioLinked set.
    QString audioPath;
    bool audioLinked = false;
    QJsonObject perspective;
    QJsonObject symmetry; // "symmetry" as the file has it (batch H); empty when absent
    TimelineTracks tracks; // defaults (unlocked, visible, unmuted) when absent
    // Camera keys the file named that could not be read (a panel index or a
    // number out of range): left out, never guessed. 0 for every project
    // written by the app.
    int cameraKeysDropped = 0;
    // THE STAGE MARGIN (Phase 2A): px each side, from the file's frame size
    // against the artwork's real size. A frame that cannot be one - larger
    // than the canvas, not centred on whole pixels - is no margin and
    // stageMarginRepaired says so; the artwork is never cropped for it.
    QSize stageMargin = QSize(0, 0);
    bool stageMarginRepaired = false;
    // Panels whose "margin" (version 2, batch 6a) is not the one their
    // pixels and the frame give: opened with the pixels' margin, counted.
    int panelMarginsRepaired = 0;
    // Layers whose "parallax" (Phase 2B) could not be a depth - not a
    // number, or outside 0 .. 2: loaded on the picture plane, counted here.
    int parallaxRepaired = 0;
    // A layer's motion keys (batch I-1) that could not be one - a number not finite, a
    // place outside the panel, a size at or below 0: left out, counted here.
    int motionKeysDropped = 0;
    // Panels the file gave no usable "id" (every project before 2026-11-04):
    // each got a fresh one, saved on the next save.
    int panelIdsMinted = 0;
    // Every image file this load read, recorded as what it now holds, so
    // the first save after opening writes only what has changed since.
    ImageLedger images;
};

// THE IMAGE NAMES (2026-11-04): a panel's files are named by its stable
// id, never its position - "p_<12 hex>.png" for the flatten and
// "p_<12 hex>_l_<12 hex>.png" for a layer (the ids' first twelve hex
// digits; the whole id when twelve collide within one save). The one
// function the writer, the loader's migration and Clean Up Project Folder
// share: every image name the scenes would be saved under, in order, in
// the assets folder (relative to it).
QString imageIdStem(const QString &id);
QStringList projectImageNames(const QVector<Scene *> &scenes);
// Whether a file name is one the id scheme makes (so Clean Up may touch
// it), and the panel stem it names.
bool isIdImageName(const QString &fileName);
QString panelStemOfImageName(const QString &fileName);

// THE SAFE SAVE (saving step 1, 2026-11-23; the report of that day, approved by the user).
//
// Until then a Save wrote a changed image STRAIGHT OVER ITS OWN FILE, and the project file
// after all the images. A save cut short - a crash, a power cut - could leave the project
// file naming a picture cut in half (the layer opens blank, and its saved state before was
// that same file), or the old project file with some new pictures (a layer's place is said
// in the project file, so a new picture under the old place opens shifted). Only the project
// file was pushed through to the disk.
//
// Now:
//  - A SAVE WRITES OVER NOTHING. An image that changed goes into a new file - its name with
//    "~2", "~3", ... before ".png" (imageNameVariant; "x.png" is their base name,
//    imageBaseName) - written whole under "<name>.part", pushed through to the disk, then
//    named. An image that did not change stays in the file it is in, under the name it has:
//    nothing is renamed either (a project saved by position before 2026-11-04 keeps those
//    names until an image changes).
//  - THE PROJECT FILE IS THE ONE SWITCH (switchProjectFile): written whole beside itself,
//    pushed through, and put in the old one's place in one step. Before that step the disk
//    holds the whole old save, after it the whole new one; there is no state between.
//  - THE PREVIOUS SAVED VERSION is what the switch replaced: its project file is kept as
//    previousVersionFile() and its pictures stay where they are (one it shares with the new
//    version is one file). It is replaced only after the next save has completed. With
//    SaveOptions::keepPrevious off it is removed instead, right after the switch.
//  - ONLY THEN are pictures removed, and only superseded ones: a file of this project's own
//    images folder that the version dropping out named for an image the new version has in
//    another file, and that neither the new version nor the kept previous one names. The
//    files of a panel or a layer that is gone stay, as before, for Clean Up Project Folder.
//
// The recovery copy keeps its names (it writes over its own files, in its own folder); its
// images are now pushed through too and take their names in one step (runImagePlan).
QString imageNameVariant(const QString &name, int n); // ("x.png", 3) -> "x~3.png"; n <= 1: name
QString imageBaseName(const QString &name);           // "x~3.png" -> "x.png"

// Where the previous saved version's project file is kept: in the project's images folder,
// not a ".sankotv" (its names are relative to the PROJECT file's folder, so opened from
// where it lies it would find none of its pictures).
QString previousVersionFile(const QString &projectFilePath);

struct SaveOptions
{
    // Preferences > Saving: keep the previous saved version (the default), or delete the
    // previous pictures right away.
    bool keepPrevious = true;
};

enum class SwitchFailure { None, CannotOpen, ShortWrite, CannotComplete };

struct [[nodiscard]] SwitchResult
{
    bool ok = false;               // the new project file is in place: the save has completed
    SwitchFailure failure = SwitchFailure::None;
    QString systemSays;            // what the file system said, when it failed
    bool unchanged = false;        // the project file said exactly this already: nothing replaced
    bool previousKept = false;     // what was replaced is now the previous saved version
    QStringList removedFiles;      // superseded pictures removed after the switch (absolute)
    bool stopped = false;          // a check stopped the writer (setSaveStopAfterForTest)
    // THE SHORT RETRY (saving step 2): how often the project file's name was tried (1: at
    // once), and whether the switch failed because another program held the file to the end.
    int tries = 0;
    bool held = false;
};

// THE SHORT RETRY (saving step 2, 2026-11-23; the user's word of that day). Another program -
// an antivirus, OneDrive, Windows' search - may hold a file for a moment just as the save
// gives it its name or reads the project file it is about to replace; Windows then refuses
// (a sharing or lock violation, or "access is denied"). The writer waits and tries again:
// kNameTries tries in all, kNamePauseMs between them - about 2 seconds - before it reports
// the failure it would have reported at once. Every name the writer gives is tried so (the
// project file's, a picture's, the previous saved version's, the recovery copy's): the same
// hold can meet any of them. It runs where the writer runs, so in the background for a Save.
// Any other refusal - a name that is taken, a path that is not there - fails at once.
inline constexpr int kNameTries = 21;
inline constexpr int kNamePauseMs = 100;
// For the checks: called on the writer's thread before each further try, IN PLACE OF the
// pause, with the file being named (or read) and the tries made so far - so a check decides
// by the count of tries, never by the clock. Null: the pause.
void setNameRetryHookForTest(std::function<void(const QString &file, int triesMade)> hook);
// The writer as it was before the retry: one try. For the checks' red run.
void setNameRetryOffForTest(bool off);

// STEP 2 OF A SAVE, after projectToJson has said every image is on disk: the project file,
// the previous saved version, the superseded pictures - in that order (see above). On a
// failure (ok false) the project file on disk is the one that was there.
SwitchResult switchProjectFile(const QString &projectFilePath, const QJsonObject &root,
                               const SaveOptions &options = SaveOptions());

// THE PROJECT FILE FIRST. It is the one file a save still replaces, so a save asks this
// before it writes a picture: true when a project file is there and may not be replaced -
// read-only by its own attribute, or its opening for writing refused (as Norton 360 refused
// SankoTV every overwrite in Documents on 2026-11-08 while it let new files be made).
// Nothing is written by asking; `systemSays` is what the system said.
bool projectFileRefused(const QString &projectFilePath, QString *systemSays = nullptr);

// The previous saved version of a project, if one is kept: its project file's contents
// (as projectFromJson reads them, with the PROJECT file's folder), when that version was
// saved, and how many of the files it names are not there (0 for a whole one).
struct PreviousVersion
{
    bool exists = false;
    QJsonObject root;
    QDateTime savedAt;
    int missingFiles = 0;
};
PreviousVersion readPreviousVersion(const QString &projectFilePath);

// The files of the project's own images folder that the project AS SAVED names - its project
// file on disk and, when one is kept, its previous saved version: the file names alone,
// lower-cased. What Clean Up Project Folder must leave (a picture written again by a later
// save is under a "~n" name the scenes alone cannot tell). False when the project file
// cannot be read: then nothing is known, and nothing may be removed on its word.
bool savedImageNames(const QString &projectFilePath, QSet<QString> *names);

// THE WRITER'S STEPS, for the checks: every file it makes is written (W), pushed through to
// the disk (P) and named (N), and it reports each.
enum class SaveStep {
    ImageWritten, ImagePushed, ImageNamed,
    ImageOverwritten, // the writer before 2026-11-23 (setOverwriteInPlaceForTest; a caller without a ledger)
    ProjectFileWritten, ProjectFilePushed, ProjectFileSwitched,
    PreviousWritten, PreviousPushed, PreviousNamed, PreviousRemoved,
    PictureRemoved,
};
struct SaveStepRecord
{
    SaveStep step;
    QString file;
};
QString saveStepName(SaveStep step);
// The log the writer's steps are appended to (null: none).
void setSaveLogForTest(QVector<SaveStepRecord> *log);
// The writer does `steps` steps and then nothing more - no cleaning up, as a process that
// died there (-1: off). Counting starts again from this call.
void setSaveStopAfterForTest(int steps);
// The writer calls `hold` when it has done `step` steps (-1: off); it may never return.
void setSaveHoldAtForTest(int step, std::function<void()> hold);
// The pushes to the disk are left out, and not reported: the control of the check that
// reads their order.
void setSaveSkipPushForTest(bool skip);
// The writer as it was before 2026-11-23: an image over its own file, the project file by
// QSaveFile, no previous version, nothing removed. For the checks' red run.
void setOverwriteInPlaceForTest(bool on);

// Rebuild the model from a parsed root + its folder. Applies every
// migration (legacy single-PNG panels, legacy shared layers, background
// insertion, take folding) and the PIXELS-WIN reconciliation: pixelSize is
// what the project must open at; mismatch says whether the manifest lied
// (the caller shows canvasMismatchDialogText and the next save corrects
// the file). Artwork is never rescaled, cropped, or discarded.
//
// When the PANELS THEMSELVES disagree, pixelSize is the MAJORITY size
// (ties to the first encountered) rather than whichever panel came first:
// an odd panel at position zero would otherwise become the project size
// and every new panel would be created at it, spreading the defect. For a
// uniform project the majority IS the first, so this path cannot move.
// The disagreement is reported through mixedSizes/offSizePanels — never
// repaired, because repairing means altering artwork.
//
// LoadedProject::images records every layer file read. A panel's flatten
// file is recorded too, but only when the manifest's own file is given
// and is no older than the flatten and every layer beside it: a save
// writes its manifest last, so a newer image was written after the last
// save that finished (a save that failed part-way, another program), and
// the flatten can no longer be trusted to show these layers.
LoadedProject projectFromJson(const QJsonObject &root, const QString &folder,
                              const QString &manifestFile = QString());

// THE OPEN IN PARALLEL (2026-10-31; a 4K project's open spent 1.75 of its
// 2 s decoding layer PNGs one after another): projectFromJson decodes every
// layer file it will read on worker threads first. The gate's witness:
// how many layer files were decoded off the UI thread and on it since the
// last reset; and the old one-at-a-time way, for the control that both
// give the same pixels.
struct DecodeCounts
{
    int offUiThread = 0;
    int onUiThread = 0;
};
DecodeCounts decodeCountsForTest();
void resetDecodeCountsForTest();
void setSerialDecodeForTest(bool serial);

// The message for a project whose PANELS disagree with each other. The
// plain mismatch text (canvasMismatchDialogText) must never be shown for
// one of these: it says "its artwork is W x H", which is false when only
// some panels are, and it promises a correction that would otherwise be
// recording an accident of ordering. Everything below is true of a mixed
// project — it names the majority, locates every dissenter, states that
// nothing is altered, and says exactly what the next save does write.
inline QString mixedCanvasSizesDialogText(const QSize &manifest,
                                          const QSize &chosen,
                                          int majorityCount,
                                          const QVector<OffSizePanel> &offSize)
{
    QString text = QStringLiteral(
                       "This project's panels are not all the same size.\n\n"
                       "%1 panel%2 %3 %4 \xC3\x97 %5, and %6:\n")
                       .arg(majorityCount)
                       .arg(majorityCount == 1 ? "" : "s")
                       .arg(majorityCount == 1 ? "is" : "are")
                       .arg(chosen.width())
                       .arg(chosen.height())
                       .arg(offSize.size() == 1
                                ? QStringLiteral("one is not")
                                : QStringLiteral("%1 are not").arg(offSize.size()));
    const int kList = 8; // enough to act on; a full list helps nobody
    for (int i = 0; i < offSize.size() && i < kList; ++i)
        text += QStringLiteral("    Scene %1, panel %2 - %3 \xC3\x97 %4\n")
                    .arg(offSize.at(i).sceneNumber)
                    .arg(offSize.at(i).panelIndex)
                    .arg(offSize.at(i).size.width())
                    .arg(offSize.at(i).size.height());
    if (offSize.size() > kList)
        text += QStringLiteral("    ... and %1 more\n")
                    .arg(offSize.size() - kList);
    text += QStringLiteral(
                "\nOpening at %1 \xC3\x97 %2, the size most panels already "
                "are. No artwork is modified: the panels above keep their "
                "own pixels exactly as they are, and new panels are created "
                "at %1 \xC3\x97 %2.")
                .arg(chosen.width())
                .arg(chosen.height());
    if (manifest.isValid() && !manifest.isEmpty() && manifest != chosen)
        text += QStringLiteral(
                    "\n\nThe project file separately records %1 \xC3\x97 %2; "
                    "the next save will record %3 \xC3\x97 %4 instead. That "
                    "corrects the file's own record only — it does not "
                    "change any panel.")
                    .arg(manifest.width())
                    .arg(manifest.height())
                    .arg(chosen.width())
                    .arg(chosen.height());
    return text;
}

} // namespace ProjectIO
