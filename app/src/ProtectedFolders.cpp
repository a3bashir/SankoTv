#include "ProtectedFolders.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ProtectedFolders {

namespace {
QVector<QPair<QString, QString>> g_foldersForTest;

// Windows' default list for Controlled Folder Access, as the artist names
// them. (OneDrive-redirected Documents and the like are where
// QStandardPaths says they are.)
QVector<QPair<QString, QString>> folders()
{
    if (!g_foldersForTest.isEmpty())
        return g_foldersForTest;
    QVector<QPair<QString, QString>> out;
    auto add = [&](const QString &name, const QString &dir) {
        if (!dir.isEmpty())
            out.append({name, QDir::cleanPath(dir)});
    };
    add(QStringLiteral("Documents"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    add(QStringLiteral("Pictures"),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
    add(QStringLiteral("Videos"),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    add(QStringLiteral("Music"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation));
    add(QStringLiteral("Desktop"),
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
    add(QStringLiteral("Favorites"), QDir::homePath() + QStringLiteral("/Favorites"));
    const QString pub = qEnvironmentVariable("PUBLIC");
    if (!pub.isEmpty())
        for (const char *sub : {"Documents", "Pictures", "Videos", "Music", "Desktop"})
            add(QStringLiteral("Public ") + QLatin1String(sub),
                pub + QLatin1Char('/') + QLatin1String(sub));
    return out;
}

bool inside(const QString &path, const QString &dir)
{
    const QString p = QDir::cleanPath(QDir::fromNativeSeparators(path));
    return p.compare(dir, Qt::CaseInsensitive) == 0
        || p.startsWith(dir + QLatin1Char('/'), Qt::CaseInsensitive);
}
} // namespace

QString folderNameFor(const QString &path)
{
    for (const auto &f : folders())
        if (inside(path, f.second))
            return f.first;
    return QString();
}

bool writeDenied(const QString &folder)
{
#ifdef Q_OS_WIN
    QString dir = QDir::cleanPath(QDir::fromNativeSeparators(folder));
    while (!QFileInfo(dir).isDir()) {
        const QString up = QFileInfo(dir).path();
        if (up == dir || up.isEmpty())
            return false;
        dir = up;
    }
    const QString probe = QDir::toNativeSeparators(
        dir + QStringLiteral("/.sankotv_write_check_%1.tmp")
                  .arg(QCoreApplication::applicationPid()));
    const HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(probe.utf16()), GENERIC_WRITE, 0,
                                 nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(h); // deleted on close
    return false;
#else
    Q_UNUSED(folder);
    return false;
#endif
}

QString blockedMessage(const QString &target, const QString &doing)
{
    const QString name = folderNameFor(target);
    if (name.isEmpty())
        return QString();
    const QFileInfo info(target);
    const QString dir = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    if (!writeDenied(dir))
        return QString();
    const QString instead = doing == QLatin1String("saving")
        ? QStringLiteral("save to another folder")
        : doing == QLatin1String("exporting") ? QStringLiteral("export to another folder")
                                              : QStringLiteral("record to another folder");
    if (controlledFolderAccessOn())
        return QStringLiteral(
                   "Windows blocked SankoTV from %1 to %2 (Ransomware protection › "
                   "Controlled folder access). Allow SankoTV there, or %3.\n\n"
                   "To allow it: Windows Security › Virus & threat protection › "
                   "Ransomware protection › Allow an app through Controlled folder access.")
            .arg(doing, name, instead);
    // Not Windows' own setting (off, or hidden because another antivirus is
    // in charge): say what is known, and no more.
    return QStringLiteral("Windows or your antivirus blocked SankoTV from %1 to %2. "
                          "Allow SankoTV in your security settings, or %3.")
        .arg(doing, name, instead);
}

namespace {
int g_cfaForTest = -1;
}

bool controlledFolderAccessOn()
{
    if (g_cfaForTest >= 0)
        return g_cfaForTest == 1;
#ifdef Q_OS_WIN
    // 1 = block. (0 off, 2 audit only, 3 / 4 the disk-modification modes -
    // none of those blocks a write to a protected folder.)
    auto read = [](const QString &key) {
        const QSettings reg(key, QSettings::NativeFormat);
        const QVariant v = reg.value(QStringLiteral("EnableControlledFolderAccess"));
        return v.isValid() ? v.toInt() : -1;
    };
    const int policy = read(QStringLiteral(
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows Defender\\"
        "Windows Defender Exploit Guard\\Controlled Folder Access"));
    if (policy >= 0)
        return policy == 1;
    return read(QStringLiteral(
               "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Defender\\"
               "Windows Defender Exploit Guard\\Controlled Folder Access"))
        == 1;
#else
    return false;
#endif
}

void setFoldersForTest(const QVector<QPair<QString, QString>> &list)
{
    g_foldersForTest.clear();
    for (const auto &f : list)
        g_foldersForTest.append({f.first, QDir::cleanPath(QDir::fromNativeSeparators(f.second))});
}

void setControlledFolderAccessForTest(int state)
{
    g_cfaForTest = state;
}

} // namespace ProtectedFolders
