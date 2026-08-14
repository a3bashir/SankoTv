#include "ColorStrokeBuffer.h"

#include "GrainField.h"
#include "NoiseField.h"
#include "TextureBlend.h"
#include "WetEdges.h"
#include "TiledImage.h"

#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
int byteRound(qreal value) { return qBound(0, qRound(value * 255.0), 255); }
}

QImage ColorStrokeBuffer::composite(const QImage &input, const Brush &brush,
                                    const QVector<StrokeStamp> &stamps,
                                    quint32 subBrushSlot)
{
    QImage output = input.convertToFormat(QImage::Format_ARGB32);
    TiledImage accumulator(QImage::Format_RGBA64);
    accumulator.reset(output.size());
    StrokeBuilder shaper(output.size(), brush, false);
    // Grain sampling is GrainField — the one CPU implementation of the
    // shader's grain term, shared with StrokeBuilder's coverage path.
    const GrainField grainField(brush);

    for (const StrokeStamp &stamp : stamps) {
        if (stamp.effectiveSize < .5 || stamp.effectiveOpacity <= 0.0) continue;
        const QImage tip = shaper.shapedTipForStamp(stamp);
        const int left = qRound(stamp.point.position.x() - tip.width() * .5);
        const int top = qRound(stamp.point.position.y() - tip.height() * .5);
        const QRect clipped = QRect(left, top, tip.width(), tip.height()).intersected(output.rect());
        // Noise (Phase 6e) perturbs the tip's coverage BEFORE grain — the
        // same order as StrokeBuilder::placeStamp and both stamp shaders.
        // The stamp-local key floors the offset from the raster centre
        // with the 0.25 bias (see NoiseField.h).
        const qreal noiseAmount = brush.noise();
        const QPointF rasterCentre(left + tip.width() * .5,
                                   top + tip.height() * .5);
        for (const QPoint &coordinate : TiledImage::tilesForRect(clipped)) {
            const QRect tileRect = TiledImage::tileLayerRect(coordinate);
            const QRect intersection = clipped.intersected(tileRect);
            QImage &tile = accumulator.ensureTile(coordinate);
            for (int y = intersection.top(); y <= intersection.bottom(); ++y) {
                QRgba64 *row = reinterpret_cast<QRgba64 *>(tile.scanLine(y - tileRect.top()));
                const uchar *tipRow = tip.constScanLine(y - top);
                for (int x = intersection.left(); x <= intersection.right(); ++x) {
                    qreal coverage = tipRow[x - left] / 255.0;
                    if (noiseAmount > 0.0)
                        coverage = noisyCoverage(
                            coverage, noiseAmount, stamp.noiseSeed,
                            qFloor(x + 0.5 - rasterCentre.x() + 0.25),
                            qFloor(y + 0.5 - rasterCentre.y() + 0.25));
                    // 1.0 unless grain is live; used for BOTH the coverage
                    // and (below) the colour modulation, exactly as before.
                    // Multiply keeps this expression VERBATIM; the other
                    // texture blend modes combine through TextureBlend.h
                    // instead — the same slot, the same anchoring. The
                    // COLOUR modulation (grainAffectsColor) keeps the
                    // multiply-shaped factor under every mode: it is a
                    // tint, not coverage arithmetic, and the roster's
                    // grain-affects-colour brushes are all Multiply.
                    qreal grainModulation = 1.0;
                    if (stamp.effectiveGrainDepth > 0.0
                        && !grainField.isNull()) {
                        // Rolling anchors to the raster centre the tip is
                        // DRAWN at — the same quantised centre both GPU
                        // instance builders pass (see StrokeBuilder).
                        grainModulation = grainField.modulation(
                            x, y,
                            QPointF(left + tip.width() * .5,
                                    top + tip.height() * .5),
                            stamp.effectiveGrainDepth);
                        if (brush.textureBlendMode()
                            == ::Brush::TextureBlendMode::Multiply) {
                            coverage *= grainModulation;
                        } else {
                            coverage = textureBlendCoverage(
                                brush.textureBlendMode(), coverage,
                                grainField.shapedSample(
                                    x, y,
                                    QPointF(left + tip.width() * .5,
                                            top + tip.height() * .5)),
                                stamp.effectiveGrainDepth);
                        }
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
                        red *= grainModulation;
                        green *= grainModulation;
                        blue *= grainModulation;
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

    // Wet edges: per-STROKE transfer at this publication boundary, the
    // colour-path twin of StrokeBuilder::publishMask8 and of the
    // colorStrokeBuffer branch in publish.frag. The buffer stores coverage
    // NORMALISED (opacity multiplies below), so the ceiling is 1. Inert
    // while smudging; the dual path composites through its own untouched
    // shader, so cpu dual sub-strokes never reach this function with wet
    // applied (see SankoPaintHostAdapter::cpuSource routing).
    const qreal wetPublish =
        (brush.smudgeActive() || brush.dualBrushEnabled()
         || subBrushSlot != 0)
        ? 0.0
        : brush.wetEdges();
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
                // Colour recovery below divides by the ORIGINAL coverage
                // (the stored colour is premultiplied by it); only the
                // published ALPHA takes the wet transfer.
                const qreal published = wetPublish > 0.0
                    ? wetEdgesAlpha(normalized, 1.0, wetPublish)
                    : normalized;
                const qreal sa = byteRound(published * brush.opacity() * colorAlpha) / 255.0;
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
