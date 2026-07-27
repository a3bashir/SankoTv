#pragma once

#include "PressureCurve.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <memory>

// Phase-1 brush data. The value-style getters/setters deliberately keep this
// independent of UI and rendering ownership, making serialization easy later.
class Brush
{
public:
    enum class ToolMode { Paint, Smudge };
    enum class GrainMode { Rolling, StaticCanvas };
    enum class GrainPreset { Paper, Canvas, Chalk, Charcoal, Custom };
    enum class DualBlendMode {
        NormalOver,
        Multiply,
        Mask,
        Subtract,
        Screen,
        Overlay,
        LinearBurn
    };
    Brush();
    Brush(const Brush &other);
    Brush &operator=(const Brush &other);
    Brush(Brush &&) noexcept = default;
    Brush &operator=(Brush &&) noexcept = default;
    ~Brush() = default;
    int size() const { return m_size; }
    void setSize(int pixels);

    qreal spacing() const { return m_spacing; }
    void setSpacing(qreal fraction);

    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal opacity);

    qreal flow() const { return m_flow; }
    void setFlow(qreal flow);

    qreal hardness() const { return m_hardness; }
    void setHardness(qreal hardness);

    QColor color() const { return m_color; }
    void setColor(const QColor &color) { m_color = color; }

    ToolMode toolMode() const { return m_toolMode; }
    void setToolMode(ToolMode mode) { m_toolMode = mode; }
    bool smudgeActive() const { return m_toolMode == ToolMode::Smudge
                                      && m_smudgeStrength > 0.0; }

    bool tiltAffectsShape() const { return m_tiltAffectsShape; }
    void setTiltAffectsShape(bool enabled) { m_tiltAffectsShape = enabled; }
    bool rotationAffectsShape() const { return m_rotationAffectsShape; }
    void setRotationAffectsShape(bool enabled) { m_rotationAffectsShape = enabled; }
    qreal maxTiltElongation() const { return m_maxTiltElongation; }
    void setMaxTiltElongation(qreal factor);

    qreal scatterAlong() const { return m_scatterAlong; }
    void setScatterAlong(qreal fraction);
    qreal scatterPerpendicular() const { return m_scatterPerpendicular; }
    void setScatterPerpendicular(qreal fraction);
    int scatterCount() const { return m_scatterCount; }
    void setScatterCount(int count);

    qreal sizeJitter() const { return m_sizeJitter; }
    void setSizeJitter(qreal amount);
    qreal angleJitter() const { return m_angleJitter; }
    void setAngleJitter(qreal amount);
    qreal roundnessJitter() const { return m_roundnessJitter; }
    void setRoundnessJitter(qreal amount);
    qreal spacingJitter() const { return m_spacingJitter; }
    void setSpacingJitter(qreal amount);

    qreal grainScale() const { return m_grainScale; }
    void setGrainScale(qreal pixels);
    qreal grainDepth() const { return m_grainDepth; }
    void setGrainDepth(qreal amount);
    qreal grainContrast() const { return m_grainContrast; }
    void setGrainContrast(qreal amount);
    qreal grainRotation() const { return m_grainRotation; }
    void setGrainRotation(qreal degrees);
    GrainMode grainMode() const { return m_grainMode; }
    void setGrainMode(GrainMode mode) { m_grainMode = mode; }
    bool grainAffectsColor() const { return m_grainAffectsColor; }
    void setGrainAffectsColor(bool enabled) { m_grainAffectsColor = enabled; }
    GrainPreset grainPreset() const { return m_grainPreset; }
    void setGrainPreset(GrainPreset preset);
    void setCustomGrain(const QImage &grayscaleTexture);
    const QImage &grainTexture() const { return m_grainTexture; }
    bool hasGrain() const { return m_grainDepth > 0.0 && !m_grainTexture.isNull(); }

    qreal smudgeStrength() const { return m_smudgeStrength; }
    void setSmudgeStrength(qreal amount);
    qreal hueJitter() const { return m_hueJitter; }
    void setHueJitter(qreal amount);
    qreal saturationJitter() const { return m_saturationJitter; }
    void setSaturationJitter(qreal amount);
    qreal brightnessJitter() const { return m_brightnessJitter; }
    void setBrightnessJitter(qreal amount);
    bool usesColorStrokeBuffer() const;

    bool dualBrushEnabled() const { return m_dualBrushEnabled && bool(m_secondaryBrush); }
    void setDualBrushEnabled(bool enabled);
    Brush &secondaryBrush();
    const Brush &secondaryBrush() const;
    DualBlendMode dualBlendMode() const { return m_dualBlendMode; }
    void setDualBlendMode(DualBlendMode mode) { m_dualBlendMode = mode; }
    qreal dualMasterOpacity() const { return m_dualMasterOpacity; }
    void setDualMasterOpacity(qreal opacity);

    // Empty custom shape selects the procedural round tip. Non-empty images
    // are converted to grayscale and scaled into the cached stamp on demand.
    void setCustomShape(const QImage &grayscaleMask);
    void clearCustomShape();
    bool hasCustomShape() const { return !m_customShape.isNull(); }
    const QImage &customShape() const { return m_customShape; }

    // Every dynamic property owns an independent pressure curve. Keeping the
    // slots in this plain model preserves serialization stability.
    PressureCurve &sizePressureCurve() { return m_sizePressureCurve; }
    const PressureCurve &sizePressureCurve() const { return m_sizePressureCurve; }
    PressureCurve &opacityPressureCurve() { return m_opacityPressureCurve; }
    const PressureCurve &opacityPressureCurve() const { return m_opacityPressureCurve; }
    PressureCurve &hardnessPressureCurve() { return m_hardnessPressureCurve; }
    const PressureCurve &hardnessPressureCurve() const { return m_hardnessPressureCurve; }
    PressureCurve &flowPressureCurve() { return m_flowPressureCurve; }
    const PressureCurve &flowPressureCurve() const { return m_flowPressureCurve; }
    PressureCurve &scatterPressureCurve() { return m_scatterPressureCurve; }
    const PressureCurve &scatterPressureCurve() const { return m_scatterPressureCurve; }
    PressureCurve &smudgePressureCurve() { return m_smudgePressureCurve; }
    const PressureCurve &smudgePressureCurve() const { return m_smudgePressureCurve; }
    PressureCurve &sizeJitterPressureCurve() { return m_sizeJitterPressureCurve; }
    const PressureCurve &sizeJitterPressureCurve() const { return m_sizeJitterPressureCurve; }
    PressureCurve &angleJitterPressureCurve() { return m_angleJitterPressureCurve; }
    const PressureCurve &angleJitterPressureCurve() const { return m_angleJitterPressureCurve; }
    PressureCurve &roundnessJitterPressureCurve() { return m_roundnessJitterPressureCurve; }
    const PressureCurve &roundnessJitterPressureCurve() const { return m_roundnessJitterPressureCurve; }
    PressureCurve &spacingJitterPressureCurve() { return m_spacingJitterPressureCurve; }
    const PressureCurve &spacingJitterPressureCurve() const { return m_spacingJitterPressureCurve; }
    PressureCurve &grainDepthPressureCurve() { return m_grainDepthPressureCurve; }
    const PressureCurve &grainDepthPressureCurve() const { return m_grainDepthPressureCurve; }
    PressureCurve &hueJitterPressureCurve() { return m_hueJitterPressureCurve; }
    const PressureCurve &hueJitterPressureCurve() const { return m_hueJitterPressureCurve; }
    PressureCurve &saturationJitterPressureCurve() { return m_saturationJitterPressureCurve; }
    const PressureCurve &saturationJitterPressureCurve() const { return m_saturationJitterPressureCurve; }
    PressureCurve &brightnessJitterPressureCurve() { return m_brightnessJitterPressureCurve; }
    const PressureCurve &brightnessJitterPressureCurve() const { return m_brightnessJitterPressureCurve; }

    const QImage &shape() const;
    const QImage &shape(qreal effectiveSize, qreal effectiveHardness) const;
    int tipCacheRegenerationCount() const { return m_tipCacheRegenerationCount; }
    int tipCacheEntryCount() const { return m_tipCache.size(); }
    void resetTipCacheStatistics() const { m_tipCacheRegenerationCount = 0; }
    // Undo settings snapshots retain source images and all editable values,
    // but never transient generated tips. Redo regenerates those deterministically.
    void clearRuntimeCaches();

private:
    void invalidateShape();
    QImage buildShape(int bucketedSize, qreal bucketedHardness) const;

    int m_size = 48;
    qreal m_spacing = 0.12;
    qreal m_opacity = 1.0;
    qreal m_flow = 1.0;
    qreal m_hardness = 0.75;
    QColor m_color = Qt::black;
    ToolMode m_toolMode = ToolMode::Paint;
    bool m_tiltAffectsShape = true;
    bool m_rotationAffectsShape = true;
    qreal m_maxTiltElongation = 4.0;
    qreal m_scatterAlong = 0.0;
    qreal m_scatterPerpendicular = 0.0;
    int m_scatterCount = 1;
    qreal m_sizeJitter = 0.0;
    qreal m_angleJitter = 0.0;
    qreal m_roundnessJitter = 0.0;
    qreal m_spacingJitter = 0.0;
    qreal m_grainScale = 96.0;
    qreal m_grainDepth = 0.0;
    qreal m_grainContrast = 1.0;
    qreal m_grainRotation = 0.0;
    GrainMode m_grainMode = GrainMode::StaticCanvas;
    GrainPreset m_grainPreset = GrainPreset::Paper;
    bool m_grainAffectsColor = false;
    QImage m_grainTexture;
    qreal m_smudgeStrength = 0.0;
    qreal m_hueJitter = 0.0;
    qreal m_saturationJitter = 0.0;
    qreal m_brightnessJitter = 0.0;
    QImage m_customShape;
    PressureCurve m_sizePressureCurve;
    PressureCurve m_opacityPressureCurve;
    PressureCurve m_hardnessPressureCurve;
    PressureCurve m_flowPressureCurve;
    PressureCurve m_scatterPressureCurve;
    PressureCurve m_smudgePressureCurve;
    PressureCurve m_sizeJitterPressureCurve;
    PressureCurve m_angleJitterPressureCurve;
    PressureCurve m_roundnessJitterPressureCurve;
    PressureCurve m_spacingJitterPressureCurve;
    PressureCurve m_grainDepthPressureCurve;
    PressureCurve m_hueJitterPressureCurve;
    PressureCurve m_saturationJitterPressureCurve;
    PressureCurve m_brightnessJitterPressureCurve;
    mutable QHash<quint64, QImage> m_tipCache;
    mutable QList<quint64> m_tipCacheLru;
    mutable qsizetype m_tipCacheBytes = 0;
    mutable int m_tipCacheRegenerationCount = 0;
    bool m_dualBrushEnabled = false;
    DualBlendMode m_dualBlendMode = DualBlendMode::NormalOver;
    qreal m_dualMasterOpacity = 1.0;
    std::unique_ptr<Brush> m_secondaryBrush;
};
