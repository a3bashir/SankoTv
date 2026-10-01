#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

// THE recent-projects store, and the one place that knows which image
// stands for a project in a recents list.
//
// It used to live inside NewProjectDialog as static members, because the
// dialog was the only thing that showed recents. It is its own unit so that
// whatever shows them - the dialog's list, File > Open Recent, the start
// window - reads and writes ONE definition, and so that a tool can link the
// store without linking a dialog.
//
// STORAGE IS UNCHANGED BY THE MOVE, deliberately: sankoSettings() array
// "recentProjects", entries {path, lastOpened as ISO date}, capped at kCap,
// most recent first. Same key, same fields, same cap as the dialog wrote, so
// a list written by an older build is read as-is and nothing migrates.
// (On Windows that is HKEY_CURRENT_USER\Software\SankoTV\SankoTV - see
// SankoSettings.h for why it is not the app-level organization.)
//
// Recorded by MainWindow on every successful load and save, and by the New
// Project dialog when it creates a project.
namespace RecentProjects {

struct Entry
{
    QString path;         // absolute .sankotv path
    QDateTime lastOpened;
};

constexpr int kCap = 10;

// Most recent first. Entries whose file has gone missing are STILL listed:
// whoever shows them dims them and offers to remove them, rather than
// having a project silently vanish from the list.
QVector<Entry> entries();
// Moves the path to the top (paths compare case-insensitively: the same
// file reached by two spellings is one project) and stamps it now.
void record(const QString &path);
void remove(const QString &path);

// Verification only: route storage to a scratch ini (empty = the real
// store). Separate from sankoSettingsSetOverrideForTest so a family can
// keep its recents apart from the rest of its scratch settings.
void setSettingsOverride(const QString &iniPath);

// The image file that stands for a project in a recents list: the flatten
// of its first panel, AS THE PROJECT'S OWN MANIFEST NAMES IT. Empty when
// the project has no panel yet, or cannot be read. The path is not checked
// for existence - a caller decoding it must cope with a file that is not
// there, exactly as it must cope with one that will not decode.
QString thumbnailSource(const QString &projectPath);
// How many times a manifest has actually been opened to answer that (the
// answer is memoised per project file until the file changes).
int manifestReadsForTest();

} // namespace RecentProjects
