#pragma once

// AUTO-SAVE AND CRASH RECOVERY (batch 3 of the proposals, 2026-11-04): where
// a project's recovery copy and session lock live, whether a target is a
// safe place for the copy, and what the folder says when the project is
// opened again. Pure path and file logic, no widgets: MainWindow writes the
// copy through ProjectIO and asks here where and whether.
//
// The copy is NEVER the project. It lives in "<base>.recovery/" beside the
// project file - the manifest "<base>.recovery.sankotv" and its images in
// "<base>.recovery_assets/" (ProjectIO's own naming for that manifest) -
// and the project file and its assets folder are never written by it.
//
// The lock "session.lock" is written while the project is open and removed
// by a clean close. A lock left behind AND a copy newer than the project
// file mean SankoTV closed without saving: the artist is offered the copy.

#include <QDateTime>
#include <QString>

namespace ProjectRecovery {

// <dir>/<base>.recovery
QString folderFor(const QString &projectPath);
// <dir>/<base>.recovery/<base>.recovery.sankotv
QString copyPathFor(const QString &projectPath);
// <dir>/<base>.recovery/session.lock
QString lockPathFor(const QString &projectPath);

// May a recovery copy of `projectPath` be written to `target`? Only a
// manifest inside the project's own recovery folder: never the project
// file, never anything in its assets folder, never a path elsewhere.
bool isSafeTarget(const QString &projectPath, const QString &target);

// The lock: this process's id, the session and the build, as JSON.
bool writeLock(const QString &projectPath, const QString &sessionId,
               const QString &build);
// Removes the lock, and the recovery folder when nothing else is in it.
void removeLock(const QString &projectPath);
bool hasLock(const QString &projectPath);

bool hasCopy(const QString &projectPath);
// When the copy was written: its header's savedAt, else the manifest's
// file time; invalid when there is no copy.
QDateTime copyTime(const QString &projectPath);
// Removes the copy's manifest and images. The lock stays (the project may
// still be open); the folder goes when nothing else is in it.
void discardCopy(const QString &projectPath);

// What the folder says when the project is opened.
struct Pending
{
    bool crashed = false;  // a lock left behind and a copy newer than the file
    QString copyPath;
    QDateTime copyTime;
    QDateTime projectTime;
};
Pending pendingFor(const QString &projectPath);

} // namespace ProjectRecovery
