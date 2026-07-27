#include "DualBrushCompositor.h"

#include "PixelCompositor.h"
#include "TiledImage.h"

#include <QPainter>
#include <algorithm>

namespace {
struct PixelF {
    qreal r = 0.0;
    qreal g = 0.0;
    qreal b = 0.0;
    qreal a = 0.0;
};

PixelF readPixel(QRgb pixel)
{
    return {qRed(pixel) / 255.0, qGreen(pixel) / 255.0,
            qBlue(pixel) / 255.0, qAlpha(pixel) / 255.0};
}

qreal blendChannel(qreal a, qreal b, Brush::DualBlendMode mode)
{
    switch (mode) {
    case Brush::DualBlendMode::Multiply: return a * b;
    case Brush::DualBlendMode::Subtract: return std::max<qreal>(0.0, a - b);
    case Brush::DualBlendMode::Screen: return 1.0 - (1.0 - a) * (1.0 - b);
    case Brush::DualBlendMode::Overlay:
        return a <= 0.5 ? 2.0 * a * b : 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
    case Brush::DualBlendMode::LinearBurn: return std::max<qreal>(0.0, a + b - 1.0);
    default: return b;
    }
}

PixelF combine(PixelF a, PixelF b, Brush::DualBlendMode mode)
{
    if (mode == Brush::DualBlendMode::Mask)
        return {a.r, a.g, a.b, a.a * b.a};
    if (mode == Brush::DualBlendMode::NormalOver) {
        const qreal alpha = b.a + a.a * (1.0 - b.a);
        if (alpha <= 0.0) return {};
        return {(b.r * b.a + a.r * a.a * (1.0 - b.a)) / alpha,
                (b.g * b.a + a.g * a.a * (1.0 - b.a)) / alpha,
                (b.b * b.a + a.b * a.a * (1.0 - b.a)) / alpha, alpha};
    }

    const qreal alpha = a.a + b.a - a.a * b.a;
    if (alpha <= 0.0) return {};
    const auto channel = [&](qreal ca, qreal cb) {
        const qreal overlap = blendChannel(ca, cb, mode);
        const qreal premult = ca * a.a * (1.0 - b.a)
            + cb * b.a * (1.0 - a.a) + overlap * a.a * b.a;
        return std::clamp(premult / alpha, 0.0, 1.0);
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), alpha};
}

int byte(qreal value)
{
    return qBound(0, qRound(std::clamp(value, 0.0, 1.0) * 255.0), 255);
}
}

QImage DualBrushCompositor::composite(const QImage &baseImage, const QImage &aImage,
                                      const QImage &bImage, Brush::DualBlendMode mode,
                                      qreal masterOpacity)
{
    QImage base = baseImage.convertToFormat(QImage::Format_ARGB32);
    const QImage a = aImage.convertToFormat(QImage::Format_ARGB32);
    const QImage b = bImage.convertToFormat(QImage::Format_ARGB32);
    const int width = std::min({base.width(), a.width(), b.width()});
    const int height = std::min({base.height(), a.height(), b.height()});
    masterOpacity = std::clamp(masterOpacity, 0.0, 1.0);
    for (int y = 0; y < height; ++y) {
        QRgb *destination = reinterpret_cast<QRgb *>(base.scanLine(y));
        const QRgb *rowA = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *rowB = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            PixelF source = combine(readPixel(rowA[x]), readPixel(rowB[x]), mode);
            source.a *= masterOpacity;
            if (source.a <= 0.0)
                continue;
            const PixelF old = readPixel(destination[x]);
            const qreal outA = source.a + old.a * (1.0 - source.a);
            const auto channel = [&](qreal sc, qreal dc) {
                return outA > 0.0
                    ? (sc * source.a + dc * old.a * (1.0 - source.a)) / outA : 0.0;
            };
            destination[x] = qRgba(byte(channel(source.r, old.r)),
                                   byte(channel(source.g, old.g)),
                                   byte(channel(source.b, old.b)), byte(outA));
        }
    }
    return base;
}

QHash<QPoint, QImage> DualBrushCompositor::colorizeCoverageTiles(
    const QHash<QPoint, QImage> &coverageTiles, const QColor &color)
{
    QHash<QPoint, QImage> output;
    for (auto it = coverageTiles.cbegin(); it != coverageTiles.cend(); ++it) {
        QImage tile(it.value().size(), QImage::Format_ARGB32);
        tile.fill(Qt::transparent);
        PixelCompositor::compositeRgbaMaskTile(tile, QPoint(), it.value(), color);
        output.insert(it.key(), tile);
    }
    return output;
}

QImage DualBrushCompositor::tilesToRegion(const QHash<QPoint, QImage> &tiles,
                                          const QRect &region)
{
    QImage output(region.size(), QImage::Format_ARGB32);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (auto it = tiles.cbegin(); it != tiles.cend(); ++it) {
        const QPoint origin = TiledImage::tileLayerRect(it.key()).topLeft() - region.topLeft();
        painter.drawImage(origin, it.value());
    }
    return output;
}

QHash<QPoint, QImage> DualBrushCompositor::regionToTiles(
    const QImage &regionImage, const QPoint &regionCanvasOrigin, const QSet<QPoint> &tiles)
{
    QHash<QPoint, QImage> output;
    for (const QPoint &coordinate : tiles) {
        QImage tile(TiledImage::TileSize, TiledImage::TileSize, QImage::Format_ARGB32);
        tile.fill(Qt::transparent);
        QPainter painter(&tile);
        const QPoint tileOrigin = TiledImage::tileLayerRect(coordinate).topLeft();
        painter.drawImage(regionCanvasOrigin - tileOrigin, regionImage);
        output.insert(coordinate, tile);
    }
    return output;
}
