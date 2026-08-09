#include "StrokeBuilder.h"
#include "PixelCompositor.h"

#include <QColor>
#include <QLineF>
#include <QTransform>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
inline uchar unorm16To8(quint16 value)
{
    // Rounded UNORM conversion. Values produced by the flow=1 compatibility
    // path are alpha8*257 and therefore round-trip byte-exactly.
    return static_cast<uchar>((quint32(value) * 255u + 32767u) / 65535u);
}

QImage grayscale16To8(const QImage &source)
{
    QImage output(source.size(), QImage::Format_Grayscale8);
    output.fill(0);
    for (int y = 0; y < source.height(); ++y) {
        const quint16 *input = reinterpret_cast<const quint16 *>(source.constScanLine(y));
        uchar *destination = output.scanLine(y);
        for (int x = 0; x < source.width(); ++x)
            destination[x] = unorm16To8(input[x]);
    }
    return output;
}
}

StrokeBuilder::StrokeBuilder(const QSize &canvasSize, const Brush &brush,
                             bool rasterizePreview, quint64 seed, quint32 subBrushSlot)
{
    reset(canvasSize, brush, rasterizePreview, seed, subBrushSlot);
}

void StrokeBuilder::reset(const QSize &canvasSize, const Brush &brush,
                          bool rasterizePreview, quint64 seed, quint32 subBrushSlot)
{
    m_canvasSize = canvasSize;
    m_brush = brush;
    m_brush.resetTipCacheStatistics();
    m_grainField = GrainField(m_brush);
    m_rawPoints.clear();
    m_smoothedPoints.clear();
    m_stamps.clear();
    m_stampPositions.clear();
    m_affectedRect = QRect();
    m_strokeMask.reset(canvasSize);
    m_previewTiles.reset(canvasSize);
    m_distanceToNextStamp = 0.0;
    m_walk = DynamicWalkState();
    m_seed = seed;
    m_subBrushSlot = subBrushSlot;
    m_nextStampIndex = 0;
    m_rasterizePreview = rasterizePreview;
}

void StrokeBuilder::addRawPoint(const QPointF &point)
{
    addRawPoint(StrokePoint{point, 1.0, 0, 0.0, 0.0, 0.0});
}

void StrokeBuilder::addRawPoint(const StrokePoint &input)
{
    if (!m_canvasSize.isValid())
        return;
    StrokePoint point = input;
    point.pressure = std::clamp(point.pressure, 0.0, 1.0);
    if (!m_rawPoints.isEmpty()) {
        const StrokePoint &last = m_rawPoints.constLast();
        const bool samePosition = QLineF(last.position, point.position).length() < 0.01;
        const bool samePressure = qAbs(last.pressure - point.pressure) < 0.0001;
        if (samePosition && samePressure && last.timestamp == point.timestamp)
            return;
    }
    m_rawPoints.append(point);
    StrokePoint smoothed = point;
    const int count = m_rawPoints.size();
    if (count >= 3) {
        // Causal weighted moving average: stabilizes the newest position
        // without revising any already-rendered part of the stroke.
        smoothed.position = m_rawPoints.at(count - 3).position * 0.15
            + m_rawPoints.at(count - 2).position * 0.25
            + point.position * 0.60;
    }
    appendSmoothedPoint(smoothed);
}

QVector<StrokePoint> StrokeBuilder::smoothPath(const QVector<StrokePoint> &points, int radius)
{
    if (points.size() < 3 || radius <= 0)
        return points;

    QVector<StrokePoint> result = points;
    for (int i = 2; i < points.size(); ++i) {
        result[i].position = points.at(i - 2).position * 0.15
            + points.at(i - 1).position * 0.25
            + points.at(i).position * 0.60;
    }
    return result;
}

StrokePoint StrokeBuilder::interpolate(const StrokePoint &a, const StrokePoint &b, qreal t)
{
    StrokePoint point;
    point.position = a.position + (b.position - a.position) * t;
    point.pressure = a.pressure + (b.pressure - a.pressure) * t;
    point.timestamp = a.timestamp + qRound64((b.timestamp - a.timestamp) * t);
    point.tiltX = a.tiltX + (b.tiltX - a.tiltX) * t;
    point.tiltY = a.tiltY + (b.tiltY - a.tiltY) * t;
    qreal rotationDelta = std::fmod(b.rotation - a.rotation + 540.0, 360.0) - 180.0;
    point.rotation = a.rotation + rotationDelta * t;
    const qreal viewportDelta = std::fmod(b.viewportRotation - a.viewportRotation + 540.0, 360.0) - 180.0;
    point.viewportRotation = a.viewportRotation + viewportDelta * t;
    return point;
}

namespace {
quint64 indexedHash(quint64 seed, quint64 index, quint64 property, quint32 subBrushSlot)
{
    quint64 value = seed ^ (index * 0x9e3779b97f4a7c15ULL)
        ^ (property * 0xd1b54a32d192ed03ULL)
        ^ (quint64(subBrushSlot) * 0xa24baed4963ee407ULL);
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

qreal indexedUnit(quint64 seed, quint64 index, quint64 property, quint32 subBrushSlot)
{
    return (indexedHash(seed, index, property, subBrushSlot) >> 11)
        * (1.0 / 9007199254740992.0);
}

qreal indexedSigned(quint64 seed, quint64 index, quint64 property, quint32 subBrushSlot)
{
    return indexedUnit(seed, index, property, subBrushSlot) * 2.0 - 1.0;
}
}

// Advance the per-stroke dynamics-source state by one stamp-group sample.
// Distance-based on both axes: the fade arc accumulates the walked path,
// and the heading smooths with a per-step weight of exp(-step / tau) so a
// straight run split into ANY number of sub-steps composes to the same
// accumulator — rate-independent by construction, unlike the app-level
// stabilizer EMA recorded in HANDOFF.
void StrokeBuilder::advanceWalk(DynamicWalkState &walk,
                                const QPointF &position,
                                const QPointF &direction)
{
    constexpr qreal kHeadingSmoothingPx = 8.0; // tau, canvas px of arc
    qreal step = 0.0;
    if (walk.hasLast)
        step = QLineF(walk.lastPosition, position).length();
    walk.lastPosition = position;
    walk.hasLast = true;
    walk.arcLength += step;
    const qreal len = std::hypot(direction.x(), direction.y());
    if (len <= 0.000001 || step <= 0.0)
        return; // stationary: the heading holds its previous value
    const QPointF d = direction / len;
    if (!walk.headingSeeded) {
        // The first REAL segment snaps the heading — no blend-in from the
        // arbitrary +X the accumulator starts at.
        walk.headingAccum = d;
        walk.headingSeeded = true;
        return;
    }
    const qreal keep = std::exp(-step / kHeadingSmoothingPx);
    walk.headingAccum = walk.headingAccum * keep + d * (1.0 - keep);
    // A sharp reversal can cancel the accumulator to ~zero; snapping to
    // the new direction keeps the heading DEFINED (and convergence fast)
    // instead of normalising noise.
    if (std::hypot(walk.headingAccum.x(), walk.headingAccum.y()) < 1e-6)
        walk.headingAccum = d;
}

Brush::DynamicSource StrokeBuilder::sourceSample(const StrokePoint &point,
                                                 const Brush &brush,
                                                 const DynamicWalkState &walk,
                                                 qreal *headingDegrees)
{
    Brush::DynamicSource s;
    s.pressure = point.pressure;
    // Fade: linear 1 -> 0 across fadeDistance() canvas px of arc, held at
    // 0 beyond (the property's curve remaps this ramp like any source).
    s.fade = std::clamp(1.0 - walk.arcLength / brush.fadeDistance(),
                        0.0, 1.0);
    const qreal heading = qRadiansToDegrees(
        std::atan2(walk.headingAccum.y(), walk.headingAccum.x()));
    if (headingDegrees)
        *headingDegrees = heading;
    // Direction: heading (-180, 180] -> 0-1, so +X encodes as 0.5.
    s.direction = (heading + 180.0) / 360.0;
    // Tilt: magnitude with the same 60-degree normalisation the tilt TIP
    // SHAPING uses — but a separate consumer; neither gates the other.
    s.tilt = std::clamp(
        std::hypot(point.tiltX, point.tiltY) / 60.0, 0.0, 1.0);
    return s;
}

StrokeStamp StrokeBuilder::resolveStamp(const StrokePoint &point, const Brush &brush,
                                        quint64 seed, quint64 index,
                                        const QPointF &rawDirection,
                                        quint32 subBrushSlot,
                                        const Brush::DynamicSource &src,
                                        qreal headingDegrees)
{
    StrokeStamp stamp;
    stamp.point = point;
    stamp.index = index;
    // Every dynamic multiplier resolves through Brush::resolveDynamic —
    // control source, curve remap, then minimum floor, in that one place.
    using P = Brush::DynamicProperty;
    const qreal sizeJitterAmount = brush.sizeJitter()
        * brush.resolveDynamic(P::SizeJitter, src);
    const qreal sizeMultiplier = std::max<qreal>(0.05,
        1.0 + indexedSigned(seed, index, 1, subBrushSlot) * sizeJitterAmount);
    stamp.effectiveSize = brush.size() * brush.resolveDynamic(P::Size, src)
        * sizeMultiplier;
    stamp.effectiveOpacity = brush.opacity() * brush.resolveDynamic(P::Opacity, src);
    stamp.effectiveHardness = brush.hardness() * brush.resolveDynamic(P::Hardness, src);
    stamp.effectiveFlow = brush.flow() >= 0.999999 ? 1.0
        : brush.flow() * brush.resolveDynamic(P::Flow, src);
    stamp.angleJitterDegrees = indexedSigned(seed, index, 2, subBrushSlot) * 180.0
        * brush.angleJitter() * brush.resolveDynamic(P::AngleJitter, src);
    // DIRECTION -> ANGLE (the Phase 3 headline): when the angle property's
    // control source is Direction, the smoothed heading joins the stamp's
    // rotation term. Both renderers already add this field into the ONE
    // per-stamp affine (rotation = static angle + tilt azimuth + barrel +
    // THIS - viewport when physical), so the tip follows the stroke and
    // the STATIC tipAngle acts as an OFFSET from the heading, exactly as
    // Phase 2's affine comment reserved. No renderer changes.
    if (brush.controlSource(P::AngleJitter) == Brush::ControlSource::Direction)
        stamp.angleJitterDegrees += headingDegrees;
    stamp.roundness = std::max<qreal>(0.05, 1.0 - indexedUnit(seed, index, 3, subBrushSlot)
        * brush.roundnessJitter()
        * brush.resolveDynamic(P::RoundnessJitter, src));
    stamp.spacingMultiplier = std::max<qreal>(0.05,
        1.0 + indexedSigned(seed, index, 4, subBrushSlot) * brush.spacingJitter()
        * brush.resolveDynamic(P::SpacingJitter, src));

    QPointF direction = rawDirection;
    const qreal length = std::hypot(direction.x(), direction.y());
    direction = length > 0.000001 ? direction / length : QPointF(1.0, 0.0);
    const QPointF normal(-direction.y(), direction.x());
    const qreal scatterPressure = brush.resolveDynamic(P::Scatter, src);
    const qreal along = indexedSigned(seed, index, 5, subBrushSlot) * brush.scatterAlong()
        * scatterPressure * stamp.effectiveSize;
    const qreal perpendicular = indexedSigned(seed, index, 6, subBrushSlot) * brush.scatterPerpendicular()
        * scatterPressure * stamp.effectiveSize;
    stamp.scatterOffset = direction * along + normal * perpendicular;
    stamp.point.position += stamp.scatterOffset;
    stamp.effectiveGrainDepth = brush.grainDepth()
        * brush.resolveDynamic(P::GrainDepth, src);
    stamp.effectiveSmudge = (brush.smudgeActive() ? brush.smudgeStrength() : 0.0)
        * brush.resolveDynamic(P::Smudge, src);
    stamp.effectiveHueJitter = brush.hueJitter()
        * brush.resolveDynamic(P::HueJitter, src);
    stamp.effectiveSaturationJitter = brush.saturationJitter()
        * brush.resolveDynamic(P::SaturationJitter, src);
    stamp.effectiveBrightnessJitter = brush.brightnessJitter()
        * brush.resolveDynamic(P::BrightnessJitter, src);
    qreal hue = brush.color().hsvHueF();
    if (hue < 0.0) hue = 0.0;
    hue = std::fmod(hue + indexedSigned(seed, index, 101, subBrushSlot)
                    * stamp.effectiveHueJitter + 1.0, 1.0);
    const qreal saturation = qBound(0.0, brush.color().hsvSaturationF()
        + indexedSigned(seed, index, 102, subBrushSlot) * stamp.effectiveSaturationJitter, 1.0);
    const qreal brightness = qBound(0.0, brush.color().valueF()
        + indexedSigned(seed, index, 103, subBrushSlot) * stamp.effectiveBrightnessJitter, 1.0);
    stamp.resolvedColor = QColor::fromHsvF(
        hue, saturation, brightness, brush.color().alphaF());
    return stamp;
}

void StrokeBuilder::resolveColorDynamics(QVector<StrokeStamp> &stamps, const Brush &brush,
                                         const QImage &preStrokeRegion,
                                         const QPoint &regionCanvasOrigin)
{
    if (stamps.isEmpty() || !brush.smudgeActive() || preStrokeRegion.isNull())
        return;
    struct PremultipliedColor {
        qreal red = 0.0;
        qreal green = 0.0;
        qreal blue = 0.0;
        qreal alpha = 0.0;
    };
    PremultipliedColor reservoir;
    bool hasReservoir = false;
    const auto premultiply = [](const QColor &color) {
        const qreal alpha = color.alphaF();
        return PremultipliedColor{color.redF() * alpha, color.greenF() * alpha,
                                  color.blueF() * alpha, alpha};
    };
    const auto blend = [](const PremultipliedColor &a,
                          const PremultipliedColor &b, qreal t) {
        PremultipliedColor result;
        result.red = a.red + (b.red - a.red) * t;
        result.green = a.green + (b.green - a.green) * t;
        result.blue = a.blue + (b.blue - a.blue) * t;
        result.alpha = a.alpha + (b.alpha - a.alpha) * t;
        return result;
    };
    for (StrokeStamp &stamp : stamps) {
        const qreal strength = std::clamp(stamp.effectiveSmudge, 0.0, 1.0);
        // A pressure curve can turn smudge off for an individual stamp. A
        // pure smudge must not fall back to painting the selected brush color.
        if (strength <= 0.0) {
            stamp.effectiveOpacity = 0.0;
            stamp.resolvedColor = Qt::transparent;
            continue;
        }
        const QPoint samplePoint = stamp.point.position.toPoint() - regionCanvasOrigin;
        const QColor sampled = preStrokeRegion.rect().contains(samplePoint)
            ? preStrokeRegion.pixelColor(samplePoint) : QColor(Qt::transparent);
        const PremultipliedColor pickup = premultiply(sampled);
        if (!hasReservoir && pickup.alpha > 0.0) {
            reservoir = pickup;
            hasReservoir = true;
        } else if (hasReservoir) {
            // Strength is carry: at 1.0 the picked-up color persists; at low
            // values the reservoir follows the frozen underlying layer.
            reservoir = blend(pickup, reservoir, strength);
        }
        if (!hasReservoir || reservoir.alpha <= 0.000001) {
            stamp.effectiveOpacity = 0.0;
            stamp.resolvedColor = Qt::transparent;
            continue;
        }
        stamp.effectiveOpacity *= reservoir.alpha;
        stamp.resolvedColor.setRgbF(
            std::clamp(reservoir.red / reservoir.alpha, 0.0, 1.0),
            std::clamp(reservoir.green / reservoir.alpha, 0.0, 1.0),
            std::clamp(reservoir.blue / reservoir.alpha, 0.0, 1.0), 1.0);
    }
}

qreal StrokeBuilder::localSpacing(const StrokeStamp &stamp, const Brush &brush)
{
    // One-pixel floor avoids a zero-pressure endpoint creating infinitely many stamps.
    return std::max<qreal>(0.25, brush.spacing() * std::max<qreal>(1.0, stamp.effectiveSize)
        * stamp.spacingMultiplier);
}

QVector<StrokeStamp> StrokeBuilder::resamplePath(const QVector<StrokePoint> &points,
                                                 const Brush &brush, quint64 seed,
                                                 quint32 subBrushSlot)
{
    QVector<StrokeStamp> result;
    if (points.isEmpty())
        return result;
    quint64 index = 0;
    DynamicWalkState walk;
    auto appendGroup = [&](const StrokePoint &point, const QPointF &direction) {
        // One source sample per stamp GROUP: scatter copies share the same
        // driving values; only their indexed RNG differs.
        advanceWalk(walk, point.position, direction);
        qreal headingDegrees = 0.0;
        const Brush::DynamicSource src =
            sourceSample(point, brush, walk, &headingDegrees);
        const bool scatterActive = (brush.scatterAlong() > 0.0
            || brush.scatterPerpendicular() > 0.0)
            && brush.resolveDynamic(Brush::DynamicProperty::Scatter, src)
                > 0.0;
        const int count = scatterActive ? brush.scatterCount() : 1;
        const int first = result.size();
        for (int copy = 0; copy < count; ++copy)
            result.append(resolveStamp(point, brush, seed, index++, direction,
                                       subBrushSlot, src, headingDegrees));
        return result.at(first);
    };
    StrokeStamp spacingStamp = appendGroup(points.first(), QPointF(1.0, 0.0));
    if (points.size() == 1)
        return result;

    qreal distanceToNext = localSpacing(spacingStamp, brush);
    for (int i = 1; i < points.size(); ++i) {
        const StrokePoint &a = points.at(i - 1);
        const StrokePoint &b = points.at(i);
        const qreal segmentLength = QLineF(a.position, b.position).length();
        if (segmentLength <= 0.000001)
            continue;
        qreal consumed = 0.0;
        while (segmentLength - consumed + 1e-9 >= distanceToNext) {
            consumed += distanceToNext;
            const StrokePoint sample = interpolate(a, b, consumed / segmentLength);
            const QPointF direction = b.position - a.position;
            spacingStamp = appendGroup(sample, direction);
            distanceToNext = localSpacing(spacingStamp, brush);
        }
        distanceToNext -= segmentLength - consumed;
    }
    return result;
}

void StrokeBuilder::appendSmoothedPoint(const StrokePoint &point)
{
    if (m_smoothedPoints.isEmpty()) {
        m_smoothedPoints.append(point);
        m_distanceToNextStamp = appendStampGroup(point, QPointF(1.0, 0.0));
        return;
    }
    const StrokePoint previous = m_smoothedPoints.constLast();
    m_smoothedPoints.append(point);
    appendSegment(previous, point);
}

void StrokeBuilder::appendSegment(const StrokePoint &a, const StrokePoint &b)
{
    const qreal segmentLength = QLineF(a.position, b.position).length();
    if (segmentLength <= 0.000001)
        return;
    qreal consumed = 0.0;
    while (segmentLength - consumed + 1e-9 >= m_distanceToNextStamp) {
        consumed += m_distanceToNextStamp;
        const StrokePoint sample = interpolate(a, b, consumed / segmentLength);
        m_distanceToNextStamp = appendStampGroup(sample, b.position - a.position);
    }
    m_distanceToNextStamp -= segmentLength - consumed;
}

qreal StrokeBuilder::appendStampGroup(const StrokePoint &point, const QPointF &direction)
{
    // One source sample per stamp GROUP (see resamplePath's twin).
    advanceWalk(m_walk, point.position, direction);
    qreal headingDegrees = 0.0;
    const Brush::DynamicSource src =
        sourceSample(point, m_brush, m_walk, &headingDegrees);
    const bool scatterActive = (m_brush.scatterAlong() > 0.0
        || m_brush.scatterPerpendicular() > 0.0)
        && m_brush.resolveDynamic(Brush::DynamicProperty::Scatter, src)
            > 0.0;
    const int count = scatterActive ? m_brush.scatterCount() : 1;
    StrokeStamp spacingStamp;
    for (int copy = 0; copy < count; ++copy) {
        StrokeStamp stamp = resolveStamp(point, m_brush, m_seed, m_nextStampIndex++, direction,
                                         m_subBrushSlot, src, headingDegrees);
        if (copy == 0)
            spacingStamp = stamp;
        m_stamps.append(stamp);
        m_stampPositions.append(stamp.point.position);
        placeStamp(stamp);
    }
    return localSpacing(spacingStamp, m_brush);
}

void StrokeBuilder::placeStamp(const StrokeStamp &stamp)
{
    if (stamp.effectiveSize < 0.5 || stamp.effectiveOpacity <= 0.0)
        return;
    if (!m_rasterizePreview) {
        const int size = std::clamp(qRound(stamp.effectiveSize), 1, 2048);
        const QRect bounds(qRound(stamp.point.position.x() - size * .5),
                           qRound(stamp.point.position.y() - size * .5), size, size);
        m_affectedRect = m_affectedRect.united(bounds.intersected(m_strokeMask.rect()));
        return;
    }
    const QImage tip = shapedTipForStamp(stamp);
    const int left = qRound(stamp.point.position.x() - tip.width() * 0.5);
    const int top = qRound(stamp.point.position.y() - tip.height() * 0.5);
    const QRect stampRect(left, top, tip.width(), tip.height());
    const QRect clipped = stampRect.intersected(m_strokeMask.rect());
    if (clipped.isEmpty())
        return;
    // GRAIN ON THE COVERAGE PATH (the CPU grain gap). The stamp shader has
    // always multiplied coverage by the grain modulation; this path never
    // did — CPU grain lived only in ColorStrokeBuffer, so a brush whose
    // grain does not affect colour rendered textureless on the CPU (27
    // roster brushes, worst 230/255 against the GPU). GrainField is the
    // shader's term, shared with ColorStrokeBuffer; effectiveGrainDepth
    // was resolved once in resolveStamp through the single dynamic
    // resolver. depth 0 (or no grain) takes the guarded path and leaves
    // every byte exactly as before.
    const bool grainActive =
        stamp.effectiveGrainDepth > 0.0 && !m_grainField.isNull();
    // Rolling grain anchors to the stamp AS DRAWN: the raster centre after
    // the integer footprint quantisation above, which is byte-for-byte the
    // centre both GPU instance builders pass ("match the reference
    // renderer's integer raster footprint exactly"). Using the exact
    // continuous position instead shifts the Rolling pattern by up to half
    // a pixel against the GPU — measured 12/255 on a fine grain before
    // this anchored centre. StaticCanvas ignores it entirely.
    const QPointF rasterCentre(left + tip.width() * 0.5,
                               top + tip.height() * 0.5);

    for (const QPoint &coordinate : TiledImage::tilesForRect(clipped)) {
        const QRect tileRect = TiledImage::tileLayerRect(coordinate);
        const QRect intersection = clipped.intersected(tileRect);
        QImage &maskTile = m_strokeMask.ensureTile(coordinate);
        QImage &previewTile = m_previewTiles.ensureTile(coordinate);
        for (int y = intersection.top(); y <= intersection.bottom(); ++y) {
            quint16 *destination = reinterpret_cast<quint16 *>(
                maskTile.scanLine(y - tileRect.top()));
            QRgb *preview = reinterpret_cast<QRgb *>(previewTile.scanLine(y - tileRect.top()));
            const uchar *source = tip.constScanLine(y - top);
            for (int x = intersection.left(); x <= intersection.right(); ++x) {
                const int localX = x - tileRect.left();
                // The shader's order: coverage is grain-modulated BEFORE
                // either deposit rule sees it.
                const qreal grain = grainActive
                    ? m_grainField.modulation(x, y, rasterCentre,
                                              stamp.effectiveGrainDepth)
                    : 1.0;
                const qreal coverage = grain * (source[x - left] / 255.0);
                quint16 combinedAlpha = 0;
                if (stamp.effectiveFlow >= 0.999999) {
                    // Preserve the Phase 1-3 8-bit rounding exactly, then map
                    // that byte losslessly into UNORM16. (grain 1.0 keeps the
                    // arithmetic bit-identical to the pre-grain expression.)
                    const int strokeAlpha =
                        qRound(grain * source[x - left] * stamp.effectiveOpacity);
                    combinedAlpha = std::max<quint16>(destination[localX],
                        static_cast<quint16>(strokeAlpha * 257));
                } else {
                    // Flow is accumulated in UNORM16 and is quantized to 8-bit
                    // only when the completed stroke is published.
                    const qreal oldAlpha = destination[localX] / 65535.0;
                    const qreal deposit = coverage * stamp.effectiveFlow;
                    const qreal next = oldAlpha + std::max<qreal>(0.0,
                        stamp.effectiveOpacity - oldAlpha) * deposit;
                    combinedAlpha = static_cast<quint16>(
                        qBound(0, qRound(next * 65535.0), 65535));
                }
                if (combinedAlpha != destination[localX]) {
                    destination[localX] = combinedAlpha;
                    const QColor color = m_brush.color();
                    const int previewAlpha = qRound(unorm16To8(combinedAlpha) * color.alphaF());
                    preview[localX] = qPremultiply(qRgba(color.red(), color.green(), color.blue(), previewAlpha));
                }
            }
        }
        m_strokeMask.markDirty(coordinate);
        m_previewTiles.markDirty(coordinate);
    }
    m_affectedRect = m_affectedRect.united(clipped);
}

QImage StrokeBuilder::shapedTipForStamp(const StrokeStamp &stamp) const
{
    // THE per-stamp affine — the one place tip shaping composes (the GPU
    // instance build mirrors these exact terms). Composition order:
    //   ellipticity = tilt anisotropy x STATIC ROUNDNESS x roundness jitter
    //   rotation    = STATIC ANGLE + tilt azimuth + barrel rotation
    //                 + angle jitter  (- viewport rotation when the
    //                 orientation is physical; Phase 3 adds the
    //                 direction-driven term HERE)
    //   flips       = sign on the TIP-LOCAL sample axes, i.e. applied
    //                 BEFORE rotation (forward order: flip the tip, THEN
    //                 rotate it), matching Photoshop — for a non-zero
    //                 angle this differs visibly from flipping after.
    // Nothing bakes into the cached base tip: the cache stays keyed on
    // bucketed (size, hardness) only.
    //
    // SAME-SOURCE SAMPLING (the custom-tip CPU/GPU divergence fix). A
    // custom tip is sampled from the ORIGINAL mask — the very image the
    // GPU uploads — not from shape()'s bucket-rescaled copy. The two
    // chains used to be
    //     CPU: original -> SmoothTransformation rescale -> bilinear
    //     GPU: original -> bilinear (shader)
    // which agree at identity but diverge along hard edges once rotated
    // or compressed (corpus worst 171/255; 81/255 from angle jitter alone
    // with no statics). Procedural tips never diverged because NEITHER
    // path samples an image for them — both evaluate the same analytic
    // falloff — and that is exactly the property this restores for custom
    // tips: one source image, one filtering, on both paths. The sample
    // coordinates below are normalised [0,1], so no affine change is
    // needed for the missing rescale; resolution only enters at the texel
    // lookup, which now matches the shader's texel-center convention on
    // the same pixels. shape() and its bucket cache still serve the
    // procedural branch's per-stamp call unchanged.
    const QImage &baseTip = m_brush.hasCustomShape()
        ? m_brush.customShape()
        : m_brush.shape(stamp.effectiveSize, stamp.effectiveHardness);
    const qreal tiltMagnitude = std::hypot(stamp.point.tiltX, stamp.point.tiltY);
    const bool applyTilt = m_brush.tiltAffectsShape() && tiltMagnitude > 0.01;
    const bool applyRotation = m_brush.rotationAffectsShape()
        && qAbs(std::remainder(stamp.point.rotation, 360.0)) > 0.01;
    const qreal normalizedTilt = std::clamp(tiltMagnitude / 60.0, 0.0, 1.0);
    const qreal elongation = 1.0
        + (m_brush.maxTiltElongation() - 1.0) * normalizedTilt;
    const qreal compression = (applyTilt ? 1.0 / elongation : 1.0)
        * m_brush.tipRoundness() * stamp.roundness;
    const qreal azimuthDegrees = applyTilt
        ? qRadiansToDegrees(std::atan2(stamp.point.tiltY, stamp.point.tiltX)) : 0.0;
    const qreal barrelDegrees = applyRotation ? stamp.point.rotation : 0.0;
    const bool physicalOrientation = applyTilt || applyRotation;
    const qreal angle = qDegreesToRadians(
        m_brush.tipAngle()
        + azimuthDegrees + barrelDegrees + stamp.angleJitterDegrees
        - (physicalOrientation ? stamp.point.viewportRotation : 0.0));
    const qreal cosine = std::cos(angle);
    const qreal sine = std::sin(angle);
    const qreal flipSignX = m_brush.tipFlipX() ? -1.0 : 1.0;
    const qreal flipSignY = m_brush.tipFlipY() ? -1.0 : 1.0;
    const int outputSize = std::clamp(qRound(stamp.effectiveSize), 1, 2048);
    QImage output(outputSize, outputSize, QImage::Format_Grayscale8);
    output.fill(0);

    const auto customSample = [&](qreal u, qreal v) {
        const qreal sourceX = u * baseTip.width() - .5;
        const qreal sourceY = v * baseTip.height() - .5;
        const int x0 = qBound(0, int(std::floor(sourceX)), baseTip.width() - 1);
        const int y0 = qBound(0, int(std::floor(sourceY)), baseTip.height() - 1);
        const int x1 = qMin(x0 + 1, baseTip.width() - 1);
        const int y1 = qMin(y0 + 1, baseTip.height() - 1);
        const qreal fx = sourceX - std::floor(sourceX);
        const qreal fy = sourceY - std::floor(sourceY);
        const qreal top = baseTip.constScanLine(y0)[x0] * (1.0 - fx)
            + baseTip.constScanLine(y0)[x1] * fx;
        const qreal bottom = baseTip.constScanLine(y1)[x0] * (1.0 - fx)
            + baseTip.constScanLine(y1)[x1] * fx;
        return (top * (1.0 - fy) + bottom * fy) / 255.0;
    };

    for (int y = 0; y < outputSize; ++y) {
        uchar *row = output.scanLine(y);
        for (int x = 0; x < outputSize; ++x) {
            const qreal localX = ((x + .5) / outputSize) * 2.0 - 1.0;
            const qreal localY = ((y + .5) / outputSize) * 2.0 - 1.0;
            qreal transformedX = cosine * localX + sine * localY;
            const qreal transformedY = -sine * localX + cosine * localY;
            transformedX /= compression;
            const qreal distance = std::hypot(transformedX, transformedY);
            if (distance >= 1.0) continue;
            qreal coverage = 0.0;
            if (m_brush.hasCustomShape()) {
                // Flips negate the TIP-LOCAL axes (coordinates AFTER the
                // inverse rotation above) — the backward-mapped form of
                // "flip the tip, then rotate". Distance is sign-blind, so
                // the procedural falloff below needs nothing.
                coverage = customSample(flipSignX * transformedX * .5 + .5,
                                        flipSignY * transformedY * .5 + .5);
            } else if (stamp.effectiveHardness >= .999 || distance <= stamp.effectiveHardness) {
                coverage = 1.0;
            } else {
                const qreal t = (distance - stamp.effectiveHardness)
                    / qMax(1.0 - stamp.effectiveHardness, .001);
                coverage = std::exp(-3.0 * t * t) * (1.0 - t);
            }
            row[x] = uchar(qBound(0, qRound(coverage * 255.0), 255));
        }
    }
    return output;
}

QImage StrokeBuilder::compositeOnto(const QImage &destination) const
{
    QImage output = destination.convertToFormat(QImage::Format_ARGB32);
    if (m_affectedRect.isEmpty())
        return output;

    for (auto tile = m_strokeMask.allocatedTiles().constBegin();
         tile != m_strokeMask.allocatedTiles().constEnd(); ++tile) {
        const QImage publishedMask = grayscale16To8(tile.value());
        PixelCompositor::compositeGrayscaleTile(
            output, TiledImage::tileLayerRect(tile.key()).topLeft(), publishedMask,
            m_brush.color());
    }
    return output;
}

QImage StrokeBuilder::strokeMask() const
{
    const QImage source = m_strokeMask.toImage();
    return grayscale16To8(source);
}
