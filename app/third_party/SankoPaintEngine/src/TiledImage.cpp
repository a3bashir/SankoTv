#include "TiledImage.h"

#include <QPainter>
#include <QtConcurrentMap>
#include <cstring>

namespace {
struct MipmapJob { QPoint coordinate; QImage base; };
struct MipmapResult { QPoint coordinate; QVector<QImage> levels; };
}

TiledImage::TiledImage(QImage::Format format)
    : m_format(format)
{
}

void TiledImage::reset(const QSize &size)
{
    m_size = size;
    m_tiles.clear();
    m_dirtyTiles.clear();
    m_mipmaps.clear();
    m_mipmapGenerations.clear();
}

QPoint TiledImage::tileForPixel(const QPoint &pixel)
{
    return {pixel.x() / TileSize, pixel.y() / TileSize};
}

QRect TiledImage::tileLayerRect(const QPoint &tile)
{
    return {tile.x() * TileSize, tile.y() * TileSize, TileSize, TileSize};
}

QVector<QPoint> TiledImage::tilesForRect(const QRect &layerRect)
{
    QVector<QPoint> result;
    if (layerRect.isEmpty())
        return result;
    const QPoint first = tileForPixel(layerRect.topLeft());
    const QPoint last = tileForPixel(layerRect.bottomRight());
    result.reserve((last.x() - first.x() + 1) * (last.y() - first.y() + 1));
    for (int y = first.y(); y <= last.y(); ++y)
        for (int x = first.x(); x <= last.x(); ++x)
            result.append({x, y});
    return result;
}

const QImage *TiledImage::tile(const QPoint &coordinate) const
{
    const auto found = m_tiles.constFind(coordinate);
    return found == m_tiles.constEnd() ? nullptr : &found.value();
}

QImage &TiledImage::ensureTile(const QPoint &coordinate)
{
    auto found = m_tiles.find(coordinate);
    if (found == m_tiles.end()) {
        QImage image(TileSize, TileSize, m_format);
        image.fill(0);
        found = m_tiles.insert(coordinate, image);
    }
    return found.value();
}

QImage TiledImage::read(const QRect &requestedRect) const
{
    const QRect region = requestedRect.intersected(rect());
    QImage output(region.size(), m_format);
    output.fill(0);
    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QPoint &coordinate : tilesForRect(region)) {
        const QImage *source = tile(coordinate);
        if (!source)
            continue;
        const QRect tileRect = tileLayerRect(coordinate);
        const QRect intersection = region.intersected(tileRect);
        painter.drawImage(intersection.topLeft() - region.topLeft(), *source,
                          intersection.translated(-tileRect.topLeft()));
    }
    return output;
}

void TiledImage::write(const QRect &requestedRect, const QImage &pixels)
{
    writeImpl(requestedRect, pixels, true);
}

void TiledImage::writeCoherent(const QRect &requestedRect, const QImage &pixels)
{
    writeImpl(requestedRect, pixels, false);
}

void TiledImage::writeImpl(const QRect &requestedRect, const QImage &pixels, bool markDirty)
{
    const QRect region = requestedRect.intersected(rect());
    if (region.isEmpty() || pixels.size() != requestedRect.size())
        return;
    const QImage source = pixels.format() == m_format
        ? pixels : pixels.convertToFormat(m_format);
    const int bytesPerPixel = source.depth() / 8;
    for (const QPoint &coordinate : tilesForRect(region)) {
        const QRect tileRect = tileLayerRect(coordinate);
        const QRect intersection = region.intersected(tileRect);
        QImage &destination = ensureTile(coordinate);
        const QRect sourceRect = intersection.translated(-requestedRect.topLeft());
        const QPoint destinationTopLeft = intersection.topLeft() - tileRect.topLeft();
        for (int row = 0; row < sourceRect.height(); ++row) {
            uchar *destinationBytes = destination.scanLine(destinationTopLeft.y() + row)
                + destinationTopLeft.x() * bytesPerPixel;
            const uchar *sourceBytes = source.constScanLine(sourceRect.y() + row)
                + sourceRect.x() * bytesPerPixel;
            std::memcpy(destinationBytes, sourceBytes,
                        size_t(sourceRect.width() * bytesPerPixel));
        }
        if (markDirty)
            m_dirtyTiles.insert(coordinate);
        if (tileIsEmpty(destination))
            m_tiles.remove(coordinate);
        m_mipmaps.remove(coordinate);
    }
}

void TiledImage::load(const QImage &image)
{
    reset(image.size());
    const QImage converted = image.convertToFormat(m_format);
    for (const QPoint &coordinate : tilesForRect(rect())) {
        const QRect tileRect = tileLayerRect(coordinate).intersected(rect());
        const QImage pixels = converted.copy(tileRect);
        if (tileIsEmpty(pixels))
            continue;
        QImage &destination = ensureTile(coordinate);
        QPainter painter(&destination);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QPoint(), pixels);
        m_dirtyTiles.insert(coordinate);
        m_mipmaps.remove(coordinate);
    }
}

int TiledImage::regenerateMipmaps(const QSet<QPoint> &tiles)
{
    QVector<MipmapJob> jobs;
    for (const QPoint &coordinate : tiles) {
        const QImage *base = tile(coordinate);
        if (!base) {
            m_mipmaps.remove(coordinate);
            continue;
        }
        jobs.append({coordinate, base->copy()});
    }
    // Workers touch detached snapshots. Publishing remains on the UI thread,
    // so tile allocation, dirty state and undo never race the worker pool.
    const QVector<MipmapResult> results = QtConcurrent::blockingMapped(
        jobs, [](const MipmapJob &job) {
        MipmapResult result{job.coordinate, {}};
        QVector<QImage> levels;
        QImage current = job.base;
        while (current.width() > 1 || current.height() > 1) {
            current = current.scaled(qMax(1, current.width() / 2),
                                     qMax(1, current.height() / 2),
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            levels.append(current);
        }
        result.levels = std::move(levels);
        return result;
    });
    for (const MipmapResult &result : results) {
        m_mipmaps.insert(result.coordinate, result.levels);
        m_mipmapGenerations[result.coordinate] += 1;
    }
    return results.size();
}

const QImage *TiledImage::mipmap(const QPoint &coordinate, int level) const
{
    if (level <= 0)
        return tile(coordinate);
    const auto found = m_mipmaps.constFind(coordinate);
    if (found == m_mipmaps.constEnd() || found.value().isEmpty())
        return tile(coordinate);
    return &found.value().at(qBound(0, level - 1, found.value().size() - 1));
}

QImage TiledImage::toImage() const
{
    QImage output(m_size, m_format);
    output.fill(0);
    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (auto it = m_tiles.constBegin(); it != m_tiles.constEnd(); ++it)
        painter.drawImage(tileLayerRect(it.key()).topLeft(), it.value());
    return output;
}

QSet<QPoint> TiledImage::takeDirtyTiles()
{
    const QSet<QPoint> dirty = m_dirtyTiles;
    m_dirtyTiles.clear();
    return dirty;
}

void TiledImage::markAllDirty()
{
    for (auto it = m_tiles.constBegin(); it != m_tiles.constEnd(); ++it)
        m_dirtyTiles.insert(it.key());
}

bool TiledImage::tileIsEmpty(const QImage &image) const
{
    if (m_format == QImage::Format_Grayscale8) {
        for (int y = 0; y < image.height(); ++y) {
            const uchar *row = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x)
                if (row[x] != 0) return false;
        }
    } else if (m_format == QImage::Format_Grayscale16) {
        for (int y = 0; y < image.height(); ++y) {
            const quint16 *row = reinterpret_cast<const quint16 *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (row[x] != 0) return false;
        }
    } else {
        for (int y = 0; y < image.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(row[x]) != 0) return false;
        }
    }
    return true;
}
