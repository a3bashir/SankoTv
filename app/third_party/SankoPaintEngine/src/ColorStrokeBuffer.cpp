#include "ColorStrokeBuffer.h"

#include "TiledImage.h"

#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
qreal wrappedGrain(const QImage &grain, qreal u, qreal v)
{
    if (grain.isNull()) return 1.0;
    u -= std::floor(u); v -= std::floor(v);
    const qreal x = u * grain.width() - .5;
    const qreal y = v * grain.height() - .5;
    const int x0 = (int(std::floor(x)) % grain.width() + grain.width()) % grain.width();
    const int y0 = (int(std::floor(y)) % grain.height() + grain.height()) % grain.height();
    const int x1 = (x0 + 1) % grain.width(), y1 = (y0 + 1) % grain.height();
    const qreal fx = x - std::floor(x), fy = y - std::floor(y);
    const auto value = [&](int px, int py) { return grain.constScanLine(py)[px] / 255.0; };
    return (value(x0, y0) * (1.0 - fx) + value(x1, y0) * fx) * (1.0 - fy)
        + (value(x0, y1) * (1.0 - fx) + value(x1, y1) * fx) * fy;
}

int byteRound(qreal value) { return qBound(0, qRound(value * 255.0), 255); }
}

QImage ColorStrokeBuffer::composite(const QImage &input, const Brush &brush,
                                    const QVector<StrokeStamp> &stamps)
{
    QImage output = input.convertToFormat(QImage::Format_ARGB32);
    TiledImage accumulator(QImage::Format_RGBA64);
    accumulator.reset(output.size());
    StrokeBuilder shaper(output.size(), brush, false);
    const QImage grain = brush.grainTexture().convertToFormat(QImage::Format_Grayscale8);

    for (const StrokeStamp &stamp : stamps) {
        if (stamp.effectiveSize < .5 || stamp.effectiveOpacity <= 0.0) continue;
        const QImage tip = shaper.shapedTipForStamp(stamp);
        const int left = qRound(stamp.point.position.x() - tip.width() * .5);
        const int top = qRound(stamp.point.position.y() - tip.height() * .5);
        const QRect clipped = QRect(left, top, tip.width(), tip.height()).intersected(output.rect());
        const qreal gc = std::cos(qDegreesToRadians(brush.grainRotation()));
        const qreal gs = std::sin(qDegreesToRadians(brush.grainRotation()));
        for (const QPoint &coordinate : TiledImage::tilesForRect(clipped)) {
            const QRect tileRect = TiledImage::tileLayerRect(coordinate);
            const QRect intersection = clipped.intersected(tileRect);
            QImage &tile = accumulator.ensureTile(coordinate);
            for (int y = intersection.top(); y <= intersection.bottom(); ++y) {
                QRgba64 *row = reinterpret_cast<QRgba64 *>(tile.scanLine(y - tileRect.top()));
                const uchar *tipRow = tip.constScanLine(y - top);
                for (int x = intersection.left(); x <= intersection.right(); ++x) {
                    qreal coverage = tipRow[x - left] / 255.0;
                    qreal grainValue = 1.0;
                    if (stamp.effectiveGrainDepth > 0.0 && !grain.isNull()) {
                        const qreal px = brush.grainMode() == Brush::GrainMode::StaticCanvas
                            ? x + .5 : x + .5 - stamp.point.position.x();
                        const qreal py = brush.grainMode() == Brush::GrainMode::StaticCanvas
                            ? y + .5 : y + .5 - stamp.point.position.y();
                        const qreal u = (gc * px + gs * py) / brush.grainScale();
                        const qreal v = (-gs * px + gc * py) / brush.grainScale();
                        grainValue = wrappedGrain(grain, u, v);
                        grainValue = std::clamp((grainValue - .5) * brush.grainContrast() + .5,
                                               0.0, 1.0);
                        coverage *= 1.0 - stamp.effectiveGrainDepth
                            + grainValue * stamp.effectiveGrainDepth;
                    }
                    const qreal opacityMultiplier = brush.opacity() > 0.0
                        ? stamp.effectiveOpacity / brush.opacity() : 0.0;
                    const qreal deposit = coverage * stamp.effectiveFlow * opacityMultiplier;
                    if (deposit <= 0.0) continue;
                    QRgba64 &old = row[x - tileRect.left()];
                    const qreal oldA = old.alpha() / 65535.0;
                    qreal red = stamp.resolvedColor.redF();
                    qreal green = stamp.resolvedColor.greenF();
                    qreal blue = stamp.resolvedColor.blueF();
                    if (brush.grainAffectsColor()) {
                        const qreal modulation = 1.0 - stamp.effectiveGrainDepth
                            + grainValue * stamp.effectiveGrainDepth;
                        red *= modulation; green *= modulation; blue *= modulation;
                    }
                    const qreal nextA = deposit + oldA * (1.0 - deposit);
                    const qreal nextR = red * deposit + old.red() / 65535.0 * (1.0 - deposit);
                    const qreal nextG = green * deposit + old.green() / 65535.0 * (1.0 - deposit);
                    const qreal nextB = blue * deposit + old.blue() / 65535.0 * (1.0 - deposit);
                    old = QRgba64::fromRgba64(quint16(qRound(nextR * 65535.0)),
                        quint16(qRound(nextG * 65535.0)), quint16(qRound(nextB * 65535.0)),
                        quint16(qRound(nextA * 65535.0)));
                }
            }
        }
    }

    for (auto it = accumulator.allocatedTiles().cbegin();
         it != accumulator.allocatedTiles().cend(); ++it) {
        const QRect tileRect = TiledImage::tileLayerRect(it.key()).intersected(output.rect());
        const QImage &tile = it.value();
        for (int y = tileRect.top(); y <= tileRect.bottom(); ++y) {
            QRgb *destination = reinterpret_cast<QRgb *>(output.scanLine(y));
            const QRgba64 *stroke = reinterpret_cast<const QRgba64 *>(
                tile.constScanLine(y - tileRect.top()));
            for (int x = tileRect.left(); x <= tileRect.right(); ++x) {
                const QRgba64 value = stroke[x - tileRect.left()];
                const qreal normalized = value.alpha() / 65535.0;
                if (normalized <= 0.0) continue;
                const qreal colorAlpha = brush.smudgeActive()
                    ? 1.0 : brush.color().alphaF();
                const qreal sa = byteRound(normalized * brush.opacity() * colorAlpha) / 255.0;
                const QRgb old = destination[x];
                const qreal da = qAlpha(old) / 255.0;
                const qreal oa = sa + da * (1.0 - sa);
                const auto channel = [&](quint16 premult, int dc) {
                    const qreal source = (premult / 65535.0) / normalized;
                    return oa > 0.0 ? byteRound((source * sa + dc / 255.0 * da
                        * (1.0 - sa)) / oa) : 0;
                };
                destination[x] = qRgba(channel(value.red(), qRed(old)),
                    channel(value.green(), qGreen(old)), channel(value.blue(), qBlue(old)),
                    byteRound(oa));
            }
        }
    }
    return output;
}
