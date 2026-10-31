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

#include <functional>

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
    // Records nothing for a file that is not there.
    void record(const QString &file, const QByteArray &content);
    void forget(const QString &file);
    void merge(const ImageLedger &other);
    int size() const { return int(m_entries.size()); }

private:
    struct Entry
    {
        QByteArray content;
        qint64 bytes = -1;
        QDateTime modified;
    };
    static QString keyFor(const QString &file);
    QHash<QString, Entry> m_entries;
};

// The identities the ledger compares: a layer is its image; a panel's
// flatten is everything flattenedPixmap() reads.
QByteArray layerContent(const QImage &image);
QByteArray flattenContent(const Panel &panel);

struct SaveData
{
    QString projectName;
    int fps = 24;
    QSize canvasSize; // the project's real resolution (post-reconcile)
    QVector<Scene *> scenes; // not owned
    QVector<ConsistencyEntry> consistency;
    // The audio track: WHERE ITS FILE IS NOW (absolute), empty for none.
    // audioLinked = an old project's reference to a file outside the
    // project, written back exactly as it was given and never copied. A
    // track that is not linked is the project's own: see projectToJson.
    QString audioPath;
    bool audioLinked = false;
    QJsonObject perspective;
    TimelineTracks tracks; // lock / visibility / mute, written under "tracks"
    // What the image files already hold (see ImageLedger): the save skips
    // every image whose file holds it, and records each one it writes.
    // Null (the seam, the fixtures) writes every image, as before.
    ImageLedger *ledger = nullptr;
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
    int encodeThreads = 0;  // distinct threads that encoded them
    // The audio copy was CANCELLED by the artist (Save As with a large
    // track): ok is false, and there is nothing to warn about.
    bool cancelled = false;
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
    TimelineTracks tracks; // defaults (unlocked, visible, unmuted) when absent
    // Camera keys the file named that could not be read (a panel index or a
    // number out of range): left out, never guessed. 0 for every project
    // written by the app.
    int cameraKeysDropped = 0;
    // Every image file this load read, recorded as what it now holds, so
    // the first save after opening writes only what has changed since.
    ImageLedger images;
};

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
