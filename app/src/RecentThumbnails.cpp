#include "RecentThumbnails.h"
#include "RecentProjects.h"

#include <QFileInfo>
#include <QImageReader>

namespace {

QString keyFor(const QString &projectPath, const QSize &pixelSize)
{
    return projectPath + QLatin1Char('|') + QString::number(pixelSize.width())
        + QLatin1Char('x') + QString::number(pixelSize.height());
}

} // namespace

RecentThumbnails::RecentThumbnails(QObject *parent) : QObject(parent)
{
    // Interval 0: one timeout per turn of the event loop, so input and
    // painting are served between any two decodes.
    m_pump.setInterval(0);
    connect(&m_pump, &QTimer::timeout, this, &RecentThumbnails::decodeNext);
}

void RecentThumbnails::request(const QString &projectPath,
                               const QSize &pixelSize)
{
    if (projectPath.isEmpty() || !pixelSize.isValid() || pixelSize.isEmpty())
        return;
    const QString key = keyFor(projectPath, pixelSize);
    const QString source = RecentProjects::thumbnailSource(projectPath);
    const QDateTime modified =
        source.isEmpty() ? QDateTime() : QFileInfo(source).lastModified();

    const auto known = m_thumbs.constFind(key);
    if (known != m_thumbs.constEnd() && known->source == source
        && known->sourceModified == modified)
        return; // what is decoded is still what is on disk

    for (const Job &job : m_queue)
        if (job.projectPath == projectPath && job.pixelSize == pixelSize)
            return; // already waiting its turn
    m_queue.append({projectPath, pixelSize});
    if (!m_pump.isActive())
        m_pump.start();
}

QPixmap RecentThumbnails::pixmap(const QString &projectPath,
                                 const QSize &pixelSize) const
{
    // A STALE picture is returned until its replacement is decoded: the
    // list keeps showing the old first panel for one more turn rather than
    // flashing empty.
    return m_thumbs.value(keyFor(projectPath, pixelSize)).pixmap;
}

bool RecentThumbnails::answered(const QString &projectPath,
                                const QSize &pixelSize) const
{
    return m_thumbs.contains(keyFor(projectPath, pixelSize));
}

void RecentThumbnails::decodeNext()
{
    if (m_queue.isEmpty()) {
        m_pump.stop();
        return;
    }
    const Job job = m_queue.takeFirst();
    if (m_queue.isEmpty())
        m_pump.stop();

    Thumb thumb;
    thumb.source = RecentProjects::thumbnailSource(job.projectPath);
    if (!thumb.source.isEmpty()) {
        thumb.sourceModified = QFileInfo(thumb.source).lastModified();
        QImageReader reader(thumb.source);
        const QSize full = reader.size(); // header only, no pixel decode
        if (full.isValid() && !full.isEmpty()) {
            // COVER the target: the list crops to its well, as it always did.
            reader.setScaledSize(
                full.scaled(job.pixelSize, Qt::KeepAspectRatioByExpanding));
            thumb.pixmap = QPixmap::fromImage(reader.read());
            ++m_decodes;
        }
    }
    // Stored even when there is no picture, so a project without one is
    // asked about once per load rather than queued forever.
    m_thumbs.insert(keyFor(job.projectPath, job.pixelSize), thumb);
    emit ready(job.projectPath);
}
