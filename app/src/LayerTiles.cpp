#include "LayerTiles.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace layertiles {

namespace {

int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

QRect tileRect(int x, int y)
{
    return QRect(x * kTile, y * kTile, kTile, kTile);
}

QImage newTile(quint32 fill)
{
    QImage tile(kTile, kTile, QImage::Format_ARGB32_Premultiplied);
    tile.fill(fill);
    return tile;
}

// Every pixel of `part` (image px) is `fill`.
bool allFill(const QImage &image, const QRect &part, quint32 fill)
{
    for (int y = part.top(); y <= part.bottom(); ++y) {
        const quint32 *row = reinterpret_cast<const quint32 *>(image.constScanLine(y)) + part.left();
        for (int x = 0; x < part.width(); ++x)
            if (row[x] != fill)
                return false;
    }
    return true;
}

// The rows of `part` (image px) into `tile`, whose (0,0) is at `tileAt` (image px).
void copyIntoTile(QImage &tile, const QImage &image, const QRect &part, const QPoint &tileAt)
{
    for (int y = part.top(); y <= part.bottom(); ++y)
        std::memcpy(reinterpret_cast<quint32 *>(tile.scanLine(y - tileAt.y())) + (part.left() - tileAt.x()),
                    reinterpret_cast<const quint32 *>(image.constScanLine(y)) + part.left(),
                    size_t(part.width()) * 4);
}

const Tile *findTile(const Tiles &tiles, int x, int y)
{
    auto it = std::lower_bound(tiles.tiles.cbegin(), tiles.tiles.cend(), std::make_pair(y, x),
                               [](const Tile &t, const std::pair<int, int> &at) {
                                   return std::make_pair(t.y, t.x) < at;
                               });
    return it != tiles.tiles.cend() && it->x == x && it->y == y ? &*it : nullptr;
}

void finish(Tiles &out)
{
    std::sort(out.tiles.begin(), out.tiles.end(), [](const Tile &a, const Tile &b) {
        return std::make_pair(a.y, a.x) < std::make_pair(b.y, b.x);
    });
    QRect drawn;
    for (const Tile &t : std::as_const(out.tiles))
        drawn |= tileRect(t.x, t.y);
    out.drawn = drawn;
}

} // namespace

qint64 freshKey()
{
    // QImage::cacheKey() is a serial number in the high half, far from this
    // range's sign bit.
    static std::atomic<qint64> next{1};
    return -next.fetch_add(1);
}

bool foldable(const QImage &image)
{
    return !image.isNull() && image.format() == QImage::Format_ARGB32_Premultiplied;
}

quint32 paperFill(const QImage &image, const TilesPtr &previous)
{
    if (image.isNull())
        return previous ? previous->fill : 0xffffffffu;
    const quint32 first = reinterpret_cast<const quint32 *>(image.constScanLine(0))[0];
    if (allFill(image, image.rect(), first))
        return first;
    return previous ? previous->fill : 0xffffffffu;
}

TilesPtr fold(const QImage &image, const QPoint &frameAt, quint32 fill, const TilesPtr &previous, qint64 key)
{
    auto out = std::make_shared<Tiles>();
    out->fill = fill;
    out->key = key ? key : image.cacheKey();
    const QRect stage(-frameAt, image.size());
    const int x0 = floorDiv(stage.left(), kTile), x1 = floorDiv(stage.right(), kTile);
    const int y0 = floorDiv(stage.top(), kTile), y1 = floorDiv(stage.bottom(), kTile);
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx) {
            const QRect tile = tileRect(tx, ty);
            const QRect inStage = tile.intersected(stage);
            const QRect part = inStage.translated(frameAt); // image px
            const QPoint tileAt = tile.topLeft() + frameAt;
            const bool blank = allFill(image, part, fill);
            // What the tile holds outside this image, kept (Q13).
            const Tile *before = previous && inStage != tile ? findTile(*previous, tx, ty) : nullptr;
            if (blank && !before)
                continue;
            Tile t;
            t.x = tx;
            t.y = ty;
            t.pixels = before ? before->pixels.copy() : newTile(fill);
            copyIntoTile(t.pixels, image, part, tileAt);
            if (before && allFill(t.pixels, t.pixels.rect(), fill))
                continue;
            out->tiles.append(std::move(t));
        }
    // Tiles wholly outside this image: art kept and hidden, shared as they are.
    if (previous)
        for (const Tile &t : previous->tiles)
            if (!tileRect(t.x, t.y).intersects(stage))
                out->tiles.append(t);
    finish(*out);
    return out;
}

TilesPtr empty(quint32 fill, qint64 key)
{
    auto out = std::make_shared<Tiles>();
    out->fill = fill;
    out->key = key;
    return out;
}

QImage unfold(const Tiles &tiles, const QRect &rect)
{
    QImage image(rect.size(), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return image;
    image.fill(tiles.fill);
    for (const Tile &t : tiles.tiles) {
        const QRect tile = tileRect(t.x, t.y);
        const QRect part = tile.intersected(rect);
        if (part.isEmpty())
            continue;
        for (int y = part.top(); y <= part.bottom(); ++y)
            std::memcpy(reinterpret_cast<quint32 *>(image.scanLine(y - rect.top())) + (part.left() - rect.left()),
                        reinterpret_cast<const quint32 *>(t.pixels.constScanLine(y - tile.top()))
                            + (part.left() - tile.left()),
                        size_t(part.width()) * 4);
    }
    return image;
}

void paint(QPainter &painter, const Tiles &tiles, const QRect &stage)
{
    const QPoint at = -stage.topLeft();
    // A tile REPLACES the fill: drawing the fill under it would blend a tile's
    // translucent pixels with what a full image never held beneath them.
    QImage fillTile;
    auto paintFill = [&](const QRect &part) { // frame space
        if (qAlpha(tiles.fill) == 255 && painter.opacity() == 1.0
            && painter.compositionMode() == QPainter::CompositionMode_SourceOver) {
            painter.fillRect(part.translated(at), QColor::fromRgb(tiles.fill));
            return;
        }
        if (fillTile.isNull())
            fillTile = newTile(tiles.fill);
        for (int y = part.top(); y <= part.bottom(); y += kTile)
            for (int x = part.left(); x <= part.right(); x += kTile)
                painter.drawImage(QPoint(x, y) + at, fillTile,
                                  QRect(0, 0, qMin(kTile, part.right() - x + 1), qMin(kTile, part.bottom() - y + 1)));
    };
    if (tiles.tiles.isEmpty()) {
        if (tiles.fill)
            paintFill(stage);
        return;
    }
    if (!tiles.fill) {
        for (const Tile &t : tiles.tiles) {
            const QRect tile = tileRect(t.x, t.y);
            const QRect part = tile.intersected(stage);
            if (!part.isEmpty())
                painter.drawImage(part.topLeft() + at, t.pixels, part.translated(-tile.topLeft()));
        }
        return;
    }
    const int x0 = floorDiv(stage.left(), kTile), x1 = floorDiv(stage.right(), kTile);
    const int y0 = floorDiv(stage.top(), kTile), y1 = floorDiv(stage.bottom(), kTile);
    for (int ty = y0; ty <= y1; ++ty)
        for (int tx = x0; tx <= x1; ++tx) {
            const QRect tile = tileRect(tx, ty);
            const QRect part = tile.intersected(stage);
            if (const Tile *t = findTile(tiles, tx, ty))
                painter.drawImage(part.topLeft() + at, t->pixels, part.translated(-tile.topLeft()));
            else
                paintFill(part);
        }
}

QRect drawnBounds(const Tiles &tiles, const QRect &stage)
{
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (const Tile &t : tiles.tiles) {
        const QRect tile = tileRect(t.x, t.y);
        const QRect part = tile.intersected(stage);
        for (int y = part.top(); y <= part.bottom(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(t.pixels.constScanLine(y - tile.top()))
                - tile.left();
            int first = -1, last = -1;
            for (int x = part.left(); x <= part.right(); ++x)
                if (qAlpha(row[x])) {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            if (first < 0)
                continue;
            left = qMin(left, first);
            right = qMax(right, last);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    if (right < left)
        return QRect();
    return QRect(QPoint(left, top), QPoint(right, bottom)).translated(-stage.topLeft());
}

bool hasHidden(const Tiles &tiles, const QRect &stage)
{
    for (const Tile &t : tiles.tiles) {
        const QRect tile = tileRect(t.x, t.y);
        if (stage.contains(tile))
            continue;
        if (!tile.intersects(stage))
            return true; // kept only because it held something
        const QRect in = tile.intersected(stage).translated(-tile.topLeft());
        for (int y = 0; y < kTile; ++y) {
            const quint32 *row = reinterpret_cast<const quint32 *>(t.pixels.constScanLine(y));
            for (int x = 0; x < kTile; ++x)
                if (row[x] != tiles.fill && !in.contains(x, y))
                    return true;
        }
    }
    return false;
}

TilesPtr trimmed(const Tiles &tiles, const QRect &stage)
{
    auto out = std::make_shared<Tiles>();
    out->fill = tiles.fill;
    out->key = freshKey();
    for (const Tile &t : tiles.tiles) {
        const QRect tile = tileRect(t.x, t.y);
        if (!tile.intersects(stage))
            continue;
        if (stage.contains(tile)) {
            out->tiles.append(t);
            continue;
        }
        Tile cut;
        cut.x = t.x;
        cut.y = t.y;
        cut.pixels = newTile(tiles.fill);
        const QRect part = tile.intersected(stage);
        copyIntoTile(cut.pixels, t.pixels, part.translated(-tile.topLeft()), QPoint(0, 0));
        if (!allFill(cut.pixels, cut.pixels.rect(), tiles.fill))
            out->tiles.append(std::move(cut));
    }
    finish(*out);
    return out;
}

bool ArtCells::anyIn(const QRect &rect) const
{
    if (cells.isEmpty() || rect.isEmpty())
        return false;
    const QRect span = QRect(QPoint(floorDiv(rect.left(), kArtCell), floorDiv(rect.top(), kArtCell)),
                             QPoint(floorDiv(rect.right(), kArtCell), floorDiv(rect.bottom(), kArtCell)))
                           .intersected(cells);
    for (int y = span.top(); y <= span.bottom(); ++y)
        for (int x = span.left(); x <= span.right(); ++x)
            if (isOn(x, y))
                return true;
    return false;
}

ArtCells artCells(const Tiles &tiles)
{
    ArtCells out;
    if (tiles.tiles.isEmpty())
        return out;
    constexpr int kPerTile = kTile / kArtCell;
    int left = tiles.tiles.first().x, right = left, top = tiles.tiles.first().y, bottom = top;
    for (const Tile &t : tiles.tiles) {
        left = qMin(left, t.x);
        right = qMax(right, t.x);
        top = qMin(top, t.y);
        bottom = qMax(bottom, t.y);
    }
    out.cells = QRect(left * kPerTile, top * kPerTile, (right - left + 1) * kPerTile,
                      (bottom - top + 1) * kPerTile);
    out.on.resize(out.cells.width() * out.cells.height());
    out.on.fill(0);
    const QRgb fill = tiles.fill;
    for (const Tile &t : tiles.tiles) {
        for (int cy = 0; cy < kPerTile; ++cy) {
            const int row = ((t.y - top) * kPerTile + cy) * out.cells.width();
            for (int py = cy * kArtCell; py < (cy + 1) * kArtCell; ++py) {
                const QRgb *line = reinterpret_cast<const QRgb *>(t.pixels.constScanLine(py));
                for (int cx = 0; cx < kPerTile; ++cx) {
                    uchar &cell = out.on[row + (t.x - left) * kPerTile + cx];
                    if (cell)
                        continue;
                    const QRgb *from = line + cx * kArtCell;
                    for (int i = 0; i < kArtCell; ++i)
                        if (from[i] != fill) {
                            cell = 1;
                            break;
                        }
                }
            }
        }
    }
    return out;
}

QPainterPath artOutline(const QVector<ArtCells> &layers)
{
    QPainterPath path;
    QRect all;
    for (const ArtCells &layer : layers)
        all |= layer.cells;
    if (all.isEmpty())
        return path;
    // One map of them all, with a clear row and column around it.
    const int w = all.width() + 2, h = all.height() + 2;
    QVector<uchar> on(w * h, 0);
    for (const ArtCells &layer : layers)
        for (int y = layer.cells.top(); y <= layer.cells.bottom(); ++y)
            for (int x = layer.cells.left(); x <= layer.cells.right(); ++x)
                if (layer.isOn(x, y))
                    on[(y - all.top() + 1) * w + x - all.left() + 1] = 1;
    const auto cell = [&](int x, int y) { return on.at(y * w + x); };
    const auto place = [&](int x, int y) {
        return QPointF((all.left() + x - 1) * kArtCell, (all.top() + y - 1) * kArtCell);
    };
    // Each run of edge between a full cell and an empty one, as one line.
    for (int y = 1; y < h; ++y) {
        int from = -1;
        for (int x = 0; x < w; ++x) {
            const bool edge = cell(x, y - 1) != cell(x, y);
            if (edge && from < 0)
                from = x;
            if (!edge && from >= 0) {
                path.moveTo(place(from, y));
                path.lineTo(place(x, y));
                from = -1;
            }
        }
    }
    for (int x = 1; x < w; ++x) {
        int from = -1;
        for (int y = 0; y < h; ++y) {
            const bool edge = cell(x - 1, y) != cell(x, y);
            if (edge && from < 0)
                from = y;
            if (!edge && from >= 0) {
                path.moveTo(place(x, from));
                path.lineTo(place(x, y));
                from = -1;
            }
        }
    }
    return path;
}

} // namespace layertiles
