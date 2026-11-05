#include "ProjectRecovery.h"

#include "ProjectIO.h" // assetSubdirFor: the copy's images folder

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace ProjectRecovery {

namespace {

QString baseOf(const QString &projectPath)
{
    return QFileInfo(projectPath).completeBaseName();
}

QString dirOf(const QString &projectPath)
{
    return QFileInfo(projectPath).absolutePath();
}

// One spelling for a path, for comparisons: absolute, clean, lower-cased
// (Windows paths compare without case).
QString norm(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

QJsonObject readJson(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return QJsonObject();
    return QJsonDocument::fromJson(f.readAll()).object();
}

void removeFolderIfEmpty(const QString &folder)
{
    QDir dir(folder);
    if (dir.exists() && dir.isEmpty())
        QDir().rmdir(dir.absolutePath());
}

} // namespace

QString folderFor(const QString &projectPath)
{
    return dirOf(projectPath) + QStringLiteral("/") + baseOf(projectPath)
        + QStringLiteral(".recovery");
}

QString copyPathFor(const QString &projectPath)
{
    return folderFor(projectPath) + QStringLiteral("/") + baseOf(projectPath)
        + QStringLiteral(".recovery.sankotv");
}

QString lockPathFor(const QString &projectPath)
{
    return folderFor(projectPath) + QStringLiteral("/session.lock");
}

bool isSafeTarget(const QString &projectPath, const QString &target)
{
    if (projectPath.isEmpty() || target.isEmpty())
        return false;
    const QString t = norm(target);
    if (t == norm(projectPath))
        return false; // the project file itself
    const QString assets = norm(dirOf(projectPath) + QStringLiteral("/")
                                + ProjectIO::assetSubdirFor(projectPath))
        + QLatin1Char('/');
    if (t.startsWith(assets))
        return false; // the project's own images
    const QString folder = norm(folderFor(projectPath)) + QLatin1Char('/');
    if (!t.startsWith(folder))
        return false; // anywhere but the recovery folder
    return t.endsWith(QStringLiteral(".sankotv"));
}

bool writeLock(const QString &projectPath, const QString &sessionId,
               const QString &build)
{
    if (projectPath.isEmpty())
        return false;
    if (!QDir().mkpath(folderFor(projectPath)))
        return false;
    QJsonObject lock;
    lock[QStringLiteral("pid")] = double(QCoreApplication::applicationPid());
    lock[QStringLiteral("sessionId")] = sessionId;
    lock[QStringLiteral("build")] = build;
    lock[QStringLiteral("openedAt")] =
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    lock[QStringLiteral("project")] = QFileInfo(projectPath).absoluteFilePath();
    QSaveFile f(lockPathFor(projectPath));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const QByteArray json = QJsonDocument(lock).toJson(QJsonDocument::Indented);
    if (f.write(json) != json.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

void removeLock(const QString &projectPath)
{
    if (projectPath.isEmpty())
        return;
    QFile::remove(lockPathFor(projectPath));
    removeFolderIfEmpty(folderFor(projectPath));
}

bool hasLock(const QString &projectPath)
{
    return !projectPath.isEmpty() && QFileInfo::exists(lockPathFor(projectPath));
}

bool hasCopy(const QString &projectPath)
{
    return !projectPath.isEmpty() && QFileInfo::exists(copyPathFor(projectPath));
}

QDateTime copyTime(const QString &projectPath)
{
    if (!hasCopy(projectPath))
        return QDateTime();
    const QJsonObject header =
        readJson(copyPathFor(projectPath)).value(QStringLiteral("recovery")).toObject();
    const QDateTime savedAt = QDateTime::fromString(
        header.value(QStringLiteral("savedAt")).toString(), Qt::ISODateWithMs);
    return savedAt.isValid() ? savedAt : QFileInfo(copyPathFor(projectPath)).lastModified();
}

void discardCopy(const QString &projectPath)
{
    if (projectPath.isEmpty())
        return;
    const QString copy = copyPathFor(projectPath);
    QFile::remove(copy);
    QDir(folderFor(projectPath) + QStringLiteral("/") + ProjectIO::assetSubdirFor(copy))
        .removeRecursively();
    removeFolderIfEmpty(folderFor(projectPath));
}

Pending pendingFor(const QString &projectPath)
{
    Pending p;
    if (!hasLock(projectPath) || !hasCopy(projectPath))
        return p;
    p.copyPath = copyPathFor(projectPath);
    p.copyTime = copyTime(projectPath);
    p.projectTime = QFileInfo(projectPath).lastModified();
    p.crashed = p.copyTime.isValid()
        && (!p.projectTime.isValid() || p.copyTime > p.projectTime);
    return p;
}

} // namespace ProjectRecovery
