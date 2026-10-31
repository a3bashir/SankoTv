#pragma once

#include <QPair>
#include <QString>
#include <QVector>

// CONTROLLED FOLDER ACCESS (Windows Security > Virus & threat protection >
// Ransomware protection; the user's item of 2026-10-31). When it is on, an
// app Windows does not trust may READ the protected folders - Documents,
// Pictures, Videos, Music, Desktop, Favorites and their Public twins - but
// may not WRITE there: creating a file fails with "access denied" and
// Windows shows its own "Unauthorized changes blocked" notice. A save, an
// export or a recording that fails that way gets a plain message naming
// the folder and what to do, instead of "could not be written". Nothing
// here tries to get round the block: allowing an app needs the artist's
// own click in Windows Security.
namespace ProtectedFolders {

// The protected folder `path` lies in, by the name the artist knows
// ("Documents"), or empty when it is in none of them.
QString folderNameFor(const QString &path);

// Windows refuses a new file in `folder` (or, if that does not exist yet,
// in its nearest existing parent) with access denied. Probes with a
// temporary file that is deleted at once.
bool writeDenied(const QString &folder);

// Windows' own Controlled folder access is ON (block mode). Read from the
// registry - group policy first, then Windows Security's own setting; a
// value that is absent or unreadable is NOT on. With another antivirus in
// charge (Norton 360 on the user's PC) Windows hides the setting, and that
// antivirus may block the same writes itself.
bool controlledFolderAccessOn();

// The plain explanation for a write to `target` (a file, or the folder an
// export writes into) that has just failed, when it was denied and
// `target` is in a protected folder - empty otherwise, so the caller's own
// message stands. `doing` is "saving", "exporting" or "recording". The
// wording names Controlled folder access, and the way to allow SankoTV
// there, only when that setting is on; otherwise it is neutral - "Windows
// or your antivirus" (the user's correction, 2026-10-31).
QString blockedMessage(const QString &target, const QString &doing);

// Test seams: these (name, directory) pairs instead of Windows' own
// folders (an empty list restores them); and Controlled folder access
// reported as on (1) or off (0) instead of read from Windows (-1).
void setFoldersForTest(const QVector<QPair<QString, QString>> &folders);
void setControlledFolderAccessForTest(int state);

} // namespace ProtectedFolders
