#include "RecentProjects.h"
#include "SankoSettings.h"

#include <QFileInfo>
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

QString thumbnailSource(const QString &projectPath)
{
    return QFileInfo(projectPath).absolutePath()
        + QStringLiteral("/panel_s0_p0.png");
}

} // namespace RecentProjects
