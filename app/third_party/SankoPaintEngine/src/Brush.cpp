#include "Brush.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
QImage proceduralGrain(Brush::GrainPreset preset)
{
    constexpr int side = 128;
    QImage image(side, side, QImage::Format_Grayscale8);
    quint32 state = 0x53414e4bU + quint32(preset) * 0x9e3779b9U;
    auto noise = [&] {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return int(state & 255U);
    };
    for (int y = 0; y < side; ++y) {
        uchar *row = image.scanLine(y);
        for (int x = 0; x < side; ++x) {
            const int n = noise();
            int value = 128;
            switch (preset) {
            case Brush::GrainPreset::Paper:
                value = 205 + (n - 128) / 5 + ((x * 3 + y * 5) % 11 - 5);
                break;
            case Brush::GrainPreset::Canvas:
                value = 190 + (n - 128) / 7
                    + ((x % 12) < 2 ? -48 : 12) + ((y % 12) < 2 ? -48 : 12);
                break;
            case Brush::GrainPreset::Chalk:
                value = 170 + (n - 128) / 2 + ((x + y) % 7 == 0 ? -45 : 0);
                break;
            case Brush::GrainPreset::Charcoal:
                value = 145 + (n - 128) * 3 / 4 + ((x * 7 + y * 3) % 17 - 8);
                break;
            case Brush::GrainPreset::Custom: value = 255; break;
            }
            row[x] = uchar(qBound(0, value, 255));
        }
    }
    return image;
}
}

Brush::Brush()
{
    setGrainPreset(GrainPreset::Paper);
}

Brush::Brush(const Brush &other)
{
    *this = other;
}

Brush &Brush::operator=(const Brush &other)
{
    if (this == &other)
        return *this;
    m_size = other.m_size;
    m_spacing = other.m_spacing;
    m_opacity = other.m_opacity;
    m_flow = other.m_flow;
    m_hardness = other.m_hardness;
    m_color = other.m_color;
    m_toolMode = other.m_toolMode;
    m_tiltAffectsShape = other.m_tiltAffectsShape;
    m_rotationAffectsShape = other.m_rotationAffectsShape;
    m_maxTiltElongation = other.m_maxTiltElongation;
    m_scatterAlong = other.m_scatterAlong;
    m_scatterPerpendicular = other.m_scatterPerpendicular;
    m_scatterCount = other.m_scatterCount;
    m_sizeJitter = other.m_sizeJitter;
    m_angleJitter = other.m_angleJitter;
    m_roundnessJitter = other.m_roundnessJitter;
    m_spacingJitter = other.m_spacingJitter;
    m_grainScale = other.m_grainScale;
    m_grainDepth = other.m_grainDepth;
    m_grainContrast = other.m_grainContrast;
    m_grainRotation = other.m_grainRotation;
    m_grainMode = other.m_grainMode;
    m_grainPreset = other.m_grainPreset;
    m_grainAffectsColor = other.m_grainAffectsColor;
    m_grainTexture = other.m_grainTexture;
    m_smudgeStrength = other.m_smudgeStrength;
    m_hueJitter = other.m_hueJitter;
    m_saturationJitter = other.m_saturationJitter;
    m_brightnessJitter = other.m_brightnessJitter;
    m_customShape = other.m_customShape;
    m_sizePressureCurve = other.m_sizePressureCurve;
    m_opacityPressureCurve = other.m_opacityPressureCurve;
    m_hardnessPressureCurve = other.m_hardnessPressureCurve;
    m_flowPressureCurve = other.m_flowPressureCurve;
    m_scatterPressureCurve = other.m_scatterPressureCurve;
    m_smudgePressureCurve = other.m_smudgePressureCurve;
    m_sizeJitterPressureCurve = other.m_sizeJitterPressureCurve;
    m_angleJitterPressureCurve = other.m_angleJitterPressureCurve;
    m_roundnessJitterPressureCurve = other.m_roundnessJitterPressureCurve;
    m_spacingJitterPressureCurve = other.m_spacingJitterPressureCurve;
    m_grainDepthPressureCurve = other.m_grainDepthPressureCurve;
    m_hueJitterPressureCurve = other.m_hueJitterPressureCurve;
    m_saturationJitterPressureCurve = other.m_saturationJitterPressureCurve;
    m_brightnessJitterPressureCurve = other.m_brightnessJitterPressureCurve;
    m_tipCache = other.m_tipCache;
    m_tipCacheLru = other.m_tipCacheLru;
    m_tipCacheBytes = other.m_tipCacheBytes;
    m_tipCacheRegenerationCount = other.m_tipCacheRegenerationCount;
    m_dualBrushEnabled = other.m_dualBrushEnabled;
    m_dualBlendMode = other.m_dualBlendMode;
    m_dualMasterOpacity = other.m_dualMasterOpacity;
    m_secondaryBrush = other.m_secondaryBrush
        ? std::make_unique<Brush>(*other.m_secondaryBrush) : nullptr;
    // Phase 5 is exactly two levels. A secondary is the same full Brush type,
    // but cannot recursively activate a third brush.
    if (m_secondaryBrush)
        m_secondaryBrush->m_dualBrushEnabled = false;
    return *this;
}

void Brush::setDualBrushEnabled(bool enabled)
{
    if (enabled && !m_secondaryBrush) {
        m_secondaryBrush = std::make_unique<Brush>(*this);
        m_secondaryBrush->m_dualBrushEnabled = false;
        m_secondaryBrush->m_secondaryBrush.reset();
    }
    m_dualBrushEnabled = enabled;
}

Brush &Brush::secondaryBrush()
{
    if (!m_secondaryBrush) {
        m_secondaryBrush = std::make_unique<Brush>(*this);
        m_secondaryBrush->m_dualBrushEnabled = false;
        m_secondaryBrush->m_secondaryBrush.reset();
    }
    return *m_secondaryBrush;
}

const Brush &Brush::secondaryBrush() const
{
    static const Brush defaultSecondary;
    return m_secondaryBrush ? *m_secondaryBrush : defaultSecondary;
}

void Brush::clearRuntimeCaches()
{
    m_tipCache.clear();
    m_tipCacheLru.clear();
    m_tipCacheBytes = 0;
    m_tipCacheRegenerationCount = 0;
    if (m_secondaryBrush)
        m_secondaryBrush->clearRuntimeCaches();
}

void Brush::setDualMasterOpacity(qreal opacity)
{
    m_dualMasterOpacity = std::clamp(opacity, 0.0, 1.0);
}

void Brush::setSize(int pixels)
{
    pixels = std::clamp(pixels, 1, 2048);
    // Size is part of the cache key. Retain existing buckets so moving the
    // UI slider never frees up to 64 MiB of QImages on the UI thread.
    m_size = pixels;
}

void Brush::setSpacing(qreal fraction)
{
    m_spacing = std::clamp(fraction, 0.01, 10.0);
}

void Brush::setOpacity(qreal opacity)
{
    m_opacity = std::clamp(opacity, 0.0, 1.0);
}

void Brush::setFlow(qreal flow)
{
    m_flow = std::clamp(flow, 0.0, 1.0);
}

void Brush::setHardness(qreal hardness)
{
    hardness = std::clamp(hardness, 0.0, 1.0);
    // Hardness is bucketed into the cache key; no invalidation is required.
    m_hardness = hardness;
}

void Brush::setMaxTiltElongation(qreal factor)
{
    m_maxTiltElongation = std::clamp(factor, 1.0, 10.0);
}

void Brush::setScatterAlong(qreal fraction)
{
    m_scatterAlong = std::clamp(fraction, 0.0, 10.0);
}

void Brush::setScatterPerpendicular(qreal fraction)
{
    m_scatterPerpendicular = std::clamp(fraction, 0.0, 10.0);
}

void Brush::setScatterCount(int count)
{
    m_scatterCount = std::clamp(count, 1, 16);
}

void Brush::setSizeJitter(qreal amount)
{
    m_sizeJitter = std::clamp(amount, 0.0, 1.0);
}

void Brush::setAngleJitter(qreal amount)
{
    m_angleJitter = std::clamp(amount, 0.0, 1.0);
}

void Brush::setRoundnessJitter(qreal amount)
{
    m_roundnessJitter = std::clamp(amount, 0.0, 1.0);
}

void Brush::setSpacingJitter(qreal amount)
{
    m_spacingJitter = std::clamp(amount, 0.0, 1.0);
}

void Brush::setGrainScale(qreal pixels) { m_grainScale = std::clamp(pixels, 1.0, 2048.0); }
void Brush::setGrainDepth(qreal amount) { m_grainDepth = std::clamp(amount, 0.0, 1.0); }
void Brush::setGrainContrast(qreal amount) { m_grainContrast = std::clamp(amount, 0.0, 4.0); }
void Brush::setGrainRotation(qreal degrees) { m_grainRotation = std::remainder(degrees, 360.0); }
void Brush::setSmudgeStrength(qreal amount) { m_smudgeStrength = std::clamp(amount, 0.0, 1.0); }
void Brush::setHueJitter(qreal amount) { m_hueJitter = std::clamp(amount, 0.0, 1.0); }
void Brush::setSaturationJitter(qreal amount) { m_saturationJitter = std::clamp(amount, 0.0, 1.0); }
void Brush::setBrightnessJitter(qreal amount) { m_brightnessJitter = std::clamp(amount, 0.0, 1.0); }

void Brush::setGrainPreset(GrainPreset preset)
{
    if (preset == GrainPreset::Custom)
        return;
    m_grainPreset = preset;
    m_grainTexture = proceduralGrain(preset);
}

void Brush::setCustomGrain(const QImage &texture)
{
    if (texture.isNull()) return;
    m_grainTexture = texture.convertToFormat(QImage::Format_Grayscale8);
    m_grainPreset = GrainPreset::Custom;
}

bool Brush::usesColorStrokeBuffer() const
{
    return m_hueJitter > 0.0 || m_saturationJitter > 0.0
        || m_brightnessJitter > 0.0 || smudgeActive()
        || (m_grainAffectsColor && hasGrain());
}

void Brush::setCustomShape(const QImage &mask)
{
    if (mask.isNull()) {
        clearCustomShape();
        return;
    }
    m_customShape = mask.convertToFormat(QImage::Format_Grayscale8);
    invalidateShape();
}

void Brush::clearCustomShape()
{
    if (!m_customShape.isNull()) {
        m_customShape = QImage();
        invalidateShape();
    }
}

const QImage &Brush::shape() const
{
    return shape(m_size, m_hardness);
}

void Brush::invalidateShape()
{
    m_tipCache.clear();
    m_tipCacheLru.clear();
    m_tipCacheBytes = 0;
    m_tipCacheRegenerationCount = 0;
}

const QImage &Brush::shape(qreal effectiveSize, qreal effectiveHardness) const
{
    const int requestedSize = std::clamp(qRound(effectiveSize), 1, 2048);
    // Sub-pixel differences are invisible on large tips but extremely costly.
    // Preserve 1 px buckets through 256 px, then gradually widen them.
    const int sizeStep = requestedSize <= 256 ? 1 : requestedSize <= 1024 ? 4 : 8;
    const int sizeBucket = std::clamp(qRound(qreal(requestedSize) / sizeStep) * sizeStep,
                                      1, 2048);
    // Five-percent hardness buckets cap procedural variants at 21 per size.
    const int hardnessBucket = m_customShape.isNull()
        ? std::clamp(qRound(std::clamp(effectiveHardness, 0.0, 1.0) * 20.0), 0, 20)
        : 0;
    const quint64 key = (static_cast<quint64>(sizeBucket) << 8)
                      | static_cast<quint64>(hardnessBucket);
    auto found = m_tipCache.constFind(key);
    if (found == m_tipCache.constEnd()) {
        const qreal bucketedHardness = hardnessBucket / 20.0;
        QImage generated = buildShape(sizeBucket, bucketedHardness);
        const qsizetype generatedBytes = generated.sizeInBytes();
        constexpr qsizetype maximumCacheBytes = 64 * 1024 * 1024;
        while (!m_tipCacheLru.isEmpty()
               && m_tipCacheBytes + generatedBytes > maximumCacheBytes) {
            const quint64 oldestKey = m_tipCacheLru.takeFirst();
            auto oldest = m_tipCache.find(oldestKey);
            if (oldest != m_tipCache.end()) {
                m_tipCacheBytes -= oldest.value().sizeInBytes();
                m_tipCache.erase(oldest);
            }
        }
        m_tipCache.insert(key, generated);
        m_tipCacheLru.append(key);
        m_tipCacheBytes += generatedBytes;
        ++m_tipCacheRegenerationCount;
        found = m_tipCache.constFind(key);
    } else {
        m_tipCacheLru.removeOne(key);
        m_tipCacheLru.append(key);
    }
    return found.value();
}

QImage Brush::buildShape(int bucketedSize, qreal bucketedHardness) const
{
    if (!m_customShape.isNull()) {
        return m_customShape.scaled(bucketedSize, bucketedSize, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_Grayscale8);
    }

    QImage shape(bucketedSize, bucketedSize, QImage::Format_Grayscale8);
    shape.fill(0);
    const qreal center = (bucketedSize - 1) * 0.5;
    const qreal radius = bucketedSize * 0.5;

    for (int y = 0; y < bucketedSize; ++y) {
        uchar *row = shape.scanLine(y);
        for (int x = 0; x < bucketedSize; ++x) {
            const qreal dx = (x - center) / radius;
            const qreal dy = (y - center) / radius;
            const qreal distance = std::sqrt(dx * dx + dy * dy);
            qreal alpha = 0.0;
            if (distance < 1.0) {
                if (bucketedHardness >= 0.999) {
                    alpha = 1.0;
                } else if (distance <= bucketedHardness) {
                    alpha = 1.0;
                } else {
                    const qreal t = (distance - bucketedHardness) / (1.0 - bucketedHardness);
                    // Gaussian-like center with a smooth zero-valued boundary.
                    alpha = std::exp(-3.0 * t * t) * (1.0 - t);
                }
            }
            row[x] = static_cast<uchar>(qRound(std::clamp(alpha, 0.0, 1.0) * 255.0));
        }
    }
    return shape;
}
