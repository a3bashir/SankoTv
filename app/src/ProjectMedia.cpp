#include "ProjectMedia.h"

#include "ProjectIO.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>

namespace ProjectMedia {

namespace {

const qint64 kBlock = 4 * 1024 * 1024;
qint64 g_writeLimitForTest = -1;

QString megabytes(qint64 bytes)
{
    return QStringLiteral("%1 MB").arg(qMax<qint64>(1, (bytes + 524288) / 1048576));
}

Adoption failed(const QString &file, const QString &reason)
{
    Adoption a;
    a.outcome = Adoption::Failed;
    a.failedFile = QDir::toNativeSeparators(file);
    a.reason = reason;
    return a;
}

enum class Compared { Same, Different, Cancelled };

// Byte for byte. Two files of the same name and the same size are the
// common case of "this one again", and a guess from size and date would
// decide which recording a project plays. A file that cannot be read is
// Different: its name is stepped past, never trusted.
Compared compare(const QString &a, const QString &b, qint64 size,
                 const Progress &progress)
{
    QFile fa(a), fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly))
        return Compared::Different;
    qint64 done = 0;
    for (;;) {
        const QByteArray ba = fa.read(kBlock);
        const QByteArray bb = fb.read(kBlock);
        if (ba != bb)
            return Compared::Different;
        if (ba.isEmpty())
            break;
        done += ba.size();
        if (progress && !progress(done, size))
            return Compared::Cancelled;
    }
    return fa.error() == QFile::NoError && fb.error() == QFile::NoError
        ? Compared::Same
        : Compared::Different;
}

// "first take.wav", 3 -> "first take (3).wav"
QString numbered(const QString &fileName, int n)
{
    if (n <= 1)
        return fileName;
    const QFileInfo info(fileName);
    const QString suffix = info.suffix();
    return suffix.isEmpty()
        ? QStringLiteral("%1 (%2)").arg(fileName).arg(n)
        : QStringLiteral("%1 (%2).%3").arg(info.completeBaseName()).arg(n).arg(suffix);
}

} // namespace

QString assetsFolder(const QString &projectFilePath)
{
    return QDir::cleanPath(QFileInfo(projectFilePath).absolutePath()
                           + QStringLiteral("/")
                           + ProjectIO::assetSubdirFor(projectFilePath));
}

bool isInAssets(const QString &file, const QString &projectFilePath)
{
    if (file.isEmpty() || projectFilePath.isEmpty())
        return false;
    return QDir::cleanPath(QFileInfo(file).absolutePath())
               .compare(assetsFolder(projectFilePath), Qt::CaseInsensitive) == 0;
}

QString storedName(const QString &fileInAssets, const QString &projectFilePath)
{
    return ProjectIO::assetSubdirFor(projectFilePath) + QStringLiteral("/")
        + QFileInfo(fileInAssets).fileName();
}

QString freeName(const QString &projectFilePath, const QString &fileName)
{
    const QString folder = assetsFolder(projectFilePath) + QStringLiteral("/");
    for (int n = 1; n < 10000; ++n) {
        const QString candidate = numbered(fileName, n);
        if (!QFileInfo::exists(folder + candidate))
            return candidate;
    }
    return fileName;
}

void setWriteLimitForTest(qint64 bytes)
{
    g_writeLimitForTest = bytes;
}

Adoption adopt(const QString &source, const QString &projectFilePath,
               const Progress &progress)
{
    const QFileInfo src(source);
    if (!src.isFile())
        return failed(source, QStringLiteral("The file could not be found."));

    // CHECKED, as the save's own mkpath is: it fails when a FILE of that
    // name is in the way, when the location is read-only, or when the path
    // is too long.
    const QString folder = assetsFolder(projectFilePath);
    if (!QDir().mkpath(folder))
        return failed(folder,
                      QStringLiteral("The folder for this project's files could "
                                     "not be created. A file of the same name "
                                     "may be in the way, the location may be "
                                     "read-only, or the path may be too long."));

    Adoption result;
    if (isInAssets(src.absoluteFilePath(), projectFilePath)) {
        // Picked from the project's own folder (an earlier track, say):
        // it is already where it belongs.
        result.outcome = Adoption::InPlace;
        result.file = QDir::cleanPath(src.absoluteFilePath());
        return result;
    }

    // THE NAME. The file's own, unless something else has it.
    const qint64 size = src.size();
    QString target;
    for (int n = 1; n < 10000 && target.isEmpty(); ++n) {
        const QString candidate =
            folder + QStringLiteral("/") + numbered(src.fileName(), n);
        const QFileInfo there(candidate);
        if (!there.exists()) {
            target = candidate;
            break;
        }
        if (!there.isFile() || there.size() != size)
            continue; // someone else's name
        switch (compare(src.absoluteFilePath(), candidate, size, progress)) {
        case Compared::Same:
            result.outcome = Adoption::Reused;
            result.file = candidate;
            return result;
        case Compared::Cancelled:
            result.outcome = Adoption::Cancelled;
            return result;
        case Compared::Different:
            break;
        }
    }
    if (target.isEmpty())
        return failed(folder + QStringLiteral("/") + src.fileName(),
                      QStringLiteral("No free name could be found for it in "
                                     "the project's folder."));

    // Room for it, asked BEFORE writing: filling the disk the project is
    // saved on is worse than refusing the import.
    const QStorageInfo disk(folder);
    if (disk.isValid() && disk.isReady() && disk.bytesAvailable() >= 0
        && disk.bytesAvailable() < size + 16 * 1024 * 1024)
        return failed(target,
                      QStringLiteral("There is not enough free space: the file "
                                     "is %1 and the disk has %2 free.")
                          .arg(megabytes(size), megabytes(disk.bytesAvailable())));

    QFile in(src.absoluteFilePath());
    if (!in.open(QIODevice::ReadOnly))
        return failed(source,
                      QStringLiteral("The file could not be read. It may be "
                                     "open in another program."));
    QSaveFile out(target);
    out.setDirectWriteFallback(false); // a half file under the real name: never
    if (!out.open(QIODevice::WriteOnly))
        return failed(target,
                      QStringLiteral("The file could not be created in the "
                                     "project's folder. The folder may be "
                                     "read-only."));
    qint64 done = 0;
    QByteArray block;
    for (;;) {
        block = in.read(kBlock);
        if (block.isEmpty())
            break;
        const bool full = g_writeLimitForTest >= 0
            && done + block.size() > g_writeLimitForTest;
        if (full || out.write(block) != block.size()) {
            out.cancelWriting();
            return failed(target,
                          QStringLiteral("Only part of the file could be "
                                         "written. The disk may be full."));
        }
        done += block.size();
        if (progress && !progress(done, size)) {
            out.cancelWriting();
            result.outcome = Adoption::Cancelled;
            return result;
        }
    }
    if (in.error() != QFile::NoError || done != size) {
        out.cancelWriting();
        return failed(source,
                      QStringLiteral("The file could not be read to the end."));
    }
    if (!out.commit())
        return failed(target,
                      QStringLiteral("The copy could not be completed: %1")
                          .arg(out.errorString()));
    result.outcome = Adoption::Copied;
    result.file = target;
    return result;
}

} // namespace ProjectMedia
