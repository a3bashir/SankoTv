#include "RecentProjects.h"
#include "SankoSettings.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace RecentProjects {

namespace {

QString g_settingsOverride; // verification: scratch ini path

QSettings store()
{
    if (!g_settingsOverride.isEmpty())
        return QSettings(g_settingsOverride, QSettings::IniFormat);
    return sankoSettings();
}

void write(const QVector<Entry> &list)
{
    QSettings s = store();
    s.beginWriteArray(QStringLiteral("recentProjects"),
                      int(qMin(list.size(), qsizetype(kCap))));
    for (int i = 0; i < list.size() && i < kCap; ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("path"), list.at(i).path);
        s.setValue(QStringLiteral("lastOpened"),
                   list.at(i).lastOpened.toString(Qt::ISODate));
    }
    s.endArray();
}

} // namespace

void setSettingsOverride(const QString &iniPath)
{
    g_settingsOverride = iniPath;
}

QVector<Entry> entries()
{
    QSettings s = store();
    QVector<Entry> out;
    const int n = s.beginReadArray(QStringLiteral("recentProjects"));
    for (int i = 0; i < n && i < kCap; ++i) {
        s.setArrayIndex(i);
        Entry e;
        e.path = s.value(QStringLiteral("path")).toString();
        e.lastOpened = QDateTime::fromString(
            s.value(QStringLiteral("lastOpened")).toString(), Qt::ISODate);
        if (!e.path.isEmpty())
            out.append(e);
    }
    s.endArray();
    return out;
}

void record(const QString &path)
{
    QVector<Entry> list = entries();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list.at(i).path.compare(path, Qt::CaseInsensitive) == 0)
            list.removeAt(i);
    list.prepend({path, QDateTime::currentDateTime()});
    write(list);
}

void remove(const QString &path)
{
    QVector<Entry> list = entries();
    for (int i = list.size() - 1; i >= 0; --i)
        if (list.at(i).path.compare(path, Qt::CaseInsensitive) == 0)
            list.removeAt(i);
    write(list);
}

// ASK THE MANIFEST. This used to be "panel_s0_p0.png beside the project
// file", which was where the first panel's flatten lived until saves moved
// every image into "<basename>_assets/" (so that two projects in one folder
// stop overwriting each other's pixels). Nothing told the recents list, and
// for a month it showed either no thumbnail at all - every project saved
// since - or, worse, a STALE one: a flat file left behind by an older save,
// which is the same file for every project sharing that folder, so a Save As
// copy displayed its sibling's old first panel as its own.
//
// The manifest names the file relative to itself, and that is the only
// statement of where the image is that stays true across both layouts: an
// old project names the flat file and finds it; a new one names its own
// assets folder. Guessing a second location here would be the same mistake
// again the next time the layout moves.
//
// Memoised on the manifest's modification time: a recents list repaints on
// every hover, and parsing a project file per row per repaint is the kind
// of cost the 3b performance pass removed from this path once already.
namespace {
int g_manifestReads = 0;
}

int manifestReadsForTest() { return g_manifestReads; }

QString thumbnailSource(const QString &projectPath)
{
    struct Resolved
    {
        QDateTime manifestModified;
        QString source;
    };
    static QHash<QString, Resolved> memo;

    const QFileInfo manifest(projectPath);
    if (!manifest.exists()) {
        memo.remove(projectPath);
        return QString();
    }
    const QDateTime modified = manifest.lastModified();
    const auto known = memo.constFind(projectPath);
    if (known != memo.constEnd() && known->manifestModified == modified)
        return known->source;

    QString source;
    QFile file(projectPath);
    if (file.open(QIODevice::ReadOnly)) {
        ++g_manifestReads;
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        const QJsonArray scenes = root.value(QStringLiteral("scenes")).toArray();
        // The first panel of the first scene that HAS one. A project just
        // created has no scenes at all, and that is an answer, not an error.
        for (const QJsonValue &scene : scenes) {
            const QJsonArray panels =
                scene.toObject().value(QStringLiteral("panels")).toArray();
            if (panels.isEmpty())
                continue;
            const QString relative = panels.first()
                                         .toObject()
                                         .value(QStringLiteral("pixmapFile"))
                                         .toString();
            if (!relative.isEmpty())
                source = manifest.absolutePath() + QLatin1Char('/') + relative;
            break;
        }
    }
    memo.insert(projectPath, {modified, source});
    return source;
}

} // namespace RecentProjects
