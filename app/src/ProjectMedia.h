#pragma once

#include <QString>

#include <functional>

// MEDIA A PROJECT OWNS LIVES IN ITS "<name>_assets" FOLDER (policy adopted
// 2026-10-03, built for the audio track 2026-10-04; see HANDOFF). This is
// the one place a media file is brought INTO that folder - at import, by
// Locate, by Copy Audio Into Project, and by a save whose project does not
// hold the file yet (Save As; a project file renamed by hand).
//
// Three rules, all of which exist to protect what is already there:
//
// - NOTHING IN THE FOLDER IS EVER OVERWRITTEN OR DELETED. A file already
//   there may be what the saved manifest names, or what an undo would bring
//   back. A name that is taken is either the same file (reused, no second
//   copy) or it is stepped past: "name (2).wav", "name (3).wav".
// - A FILE APPEARS UNDER ITS REAL NAME ONLY WHEN IT IS COMPLETE. The copy is
//   written beside it under a temporary name and renamed at the end
//   (QSaveFile); a cancelled or failed copy leaves nothing behind.
// - A FAILURE IS REPORTED, never absorbed: the caller is told which file
//   and why, in words that can be shown as they are.
//
// No widgets here. The caller passes a progress function if it wants one
// (MediaCopyProgress is the app's).
namespace ProjectMedia {

// Called as the bytes go by. Return false to CANCEL.
using Progress = std::function<bool(qint64 done, qint64 total)>;

struct Adoption
{
    enum Outcome {
        Copied,    // written into the assets folder under `file`
        Reused,    // an identical file was already there: `file` is it
        InPlace,   // the source IS a file in the assets folder
        Cancelled, // the progress function said stop; nothing was left
        Failed     // see failedFile / reason; nothing was left
    };
    Outcome outcome = Failed;
    QString file;       // absolute path inside the assets folder, when ok()
    QString failedFile; // Failed: what could not be read or written
    QString reason;     // Failed: why, in words an artist can act on

    bool ok() const
    {
        return outcome == Copied || outcome == Reused || outcome == InPlace;
    }
};

// "<folder of the project file>/<name>_assets", absolute.
QString assetsFolder(const QString &projectFilePath);

// Is `file` directly inside this project's assets folder? A question about
// the two PATHS (compared as Windows compares names); the disk is not asked.
bool isInAssets(const QString &file, const QString &projectFilePath);

// How the manifest names a file in the assets folder: relative to the
// project file, forward slashes - "Film_assets/first take.wav" - exactly
// as panel images are named.
QString storedName(const QString &fileInAssets, const QString &projectFilePath);

// A name in the assets folder that nothing has: `fileName` itself, or the
// first free "name (2).ext", "name (3).ext"...
QString freeName(const QString &projectFilePath, const QString &fileName);

// Bring `source` into the project's assets folder (created if need be).
Adoption adopt(const QString &source, const QString &projectFilePath,
               const Progress &progress = {});

// For the gate: the disk "fills" after this many bytes of a copy have been
// written (-1 = no limit). The write is refused there exactly as a full
// disk refuses it, so what is exercised after it is the real clean-up.
void setWriteLimitForTest(qint64 bytes);

} // namespace ProjectMedia
