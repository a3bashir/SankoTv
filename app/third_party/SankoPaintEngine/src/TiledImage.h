#pragma once

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QVector>

class TiledImage
{
public:
    static constexpr int TileSize = 256;

    explicit TiledImage(QImage::Format format = QImage::Format_ARGB32);
    void reset(const QSize &size);
    QSize size() const { return m_size; }
    QRect rect() const { return QRect(QPoint(), m_size); }
    QImage::Format format() const { return m_format; }

    static QPoint tileForPixel(const QPoint &pixel);
    static QRect tileLayerRect(const QPoint &tile);
    static QVector<QPoint> tilesForRect(const QRect &layerRect);

    bool hasTile(const QPoint &tile) const { return m_tiles.contains(tile); }
    int allocatedTileCount() const { return m_tiles.size(); }
    const QImage *tile(const QPoint &coordinate) const;
    QImage &ensureTile(const QPoint &coordinate);
    const QHash<QPoint, QImage> &allocatedTiles() const { return m_tiles; }

    QImage read(const QRect &layerRect) const;
    void write(const QRect &layerRect, const QImage &pixels);
    // GPU readback updates CPU pixels without scheduling those same pixels
    // for upload back to the GPU.
    void writeCoherent(const QRect &layerRect, const QImage &pixels);
    void load(const QImage &image);
    QImage toImage() const;

    const QSet<QPoint> &dirtyTiles() const { return m_dirtyTiles; }
    QSet<QPoint> takeDirtyTiles();
    void markDirty(const QPoint &tile) { m_dirtyTiles.insert(tile); }
    void markAllDirty();
    int regenerateMipmaps(const QSet<QPoint> &tiles);
    const QImage *mipmap(const QPoint &tile, int level) const;
    int mipmapGeneration(const QPoint &tile) const { return m_mipmapGenerations.value(tile); }

private:
    void writeImpl(const QRect &layerRect, const QImage &pixels, bool markDirty);
    bool tileIsEmpty(const QImage &tile) const;

    QSize m_size;
    QImage::Format m_format;
    QHash<QPoint, QImage> m_tiles;
    QSet<QPoint> m_dirtyTiles;
    QHash<QPoint, QVector<QImage>> m_mipmaps;
    QHash<QPoint, int> m_mipmapGenerations;
};
