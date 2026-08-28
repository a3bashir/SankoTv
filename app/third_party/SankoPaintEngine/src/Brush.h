#pragma once

#include "PressureCurve.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <array>
#include <memory>

// Phase-1 brush data. The value-style getters/setters deliberately keep this
// independent of UI and rendering ownership, making serialization easy later.
class Brush
{
public:
    enum class ToolMode { Paint, Smudge };
    enum class GrainMode { Rolling, StaticCanvas };
    enum class GrainPreset { Paper, Canvas, Chalk, Charcoal, Custom };
    // HOW the grain texture combines with tip coverage (Photoshop's
    // texture blend mode popup). Multiply is the engine's original — and
    // only, until this enum — behaviour, and stays the default so every
    // existing preset renders exactly as before. Formulas and per-mode
    // confidence grading live in TextureBlend.h.
    enum class TextureBlendMode {
        Multiply,
        Subtract,
        Darken,
        Overlay,
        ColorBurn,
        LinearBurn,
        HardMix,
        Height,
        LinearHeight
    };
    // What DRIVES a dynamic property (Photoshop's "Control" popup). The
    // per-property curve stays: the source selects WHAT produces the 0-1
    // driving value, the curve remaps it. Only Pressure is live today;
    // Fade / Tilt / Direction are structural placeholders whose driving
    // value is a constant 1.0 until the dynamics phase supplies real ones,
    // so selecting them is inert, never undefined. None holds the property
    // at its BASE value and ignores the curve entirely.
    enum class ControlSource { None, Pressure, Fade, Tilt, Direction };
    // The fourteen dynamic properties, one per pressure-curve slot, in the
    // codec's wire order. Used to key the control source and minimum.
    enum class DynamicProperty {
        Size, Opacity, Hardness, Flow, Scatter, Smudge,
        SizeJitter, AngleJitter, RoundnessJitter, SpacingJitter,
        GrainDepth, HueJitter, SaturationJitter, BrightnessJitter,
        // Phase 6c: interpolates each stamp's colour toward the background
        // colour. Wire note: the v2 dynamics block is FROZEN at the
        // original 14 — this property's source/minimum/curve serialise in
        // the v7 block (BrushPresetCodec), and AbrImporter::contentId
        // deliberately fingerprints only the first 14 until the importer
        // maps colour dynamics (which will need a mapping-generation
        // bump).
        ForegroundBackground,
    };
    static constexpr int kDynamicPropertyCount = 15;

    // One sample of every driving value a control source can select,
    // computed CPU-side during the path walk from the shared point stream
    // (so both render paths resolve identically). All 0-1:
    //   pressure   the stylus pressure as captured.
    //   fade       1 at the stroke start, decaying LINEARLY to 0 at
    //              fadeDistance() canvas px of arc length, 0 beyond.
    //   direction  the smoothed stroke heading, (degrees + 180) / 360 —
    //              heading 0 (+X) encodes as 0.5.
    //   tilt       clamp(hypot(tiltX, tiltY) / 60, 0, 1) — the same
    //              normalisation the tilt TIP SHAPING uses, but a separate
    //              consumer: a brush can drive a dynamic from tilt without
    //              tilt shaping its tip, and vice versa.
    struct DynamicSource
    {
        qreal pressure = 1.0;
        qreal fade = 1.0;
        qreal direction = 0.5;
        qreal tilt = 0.0;
    };
    // How the dual brush COMBINES (Phase 6d). Composite is the engine's
    // original semantics: A and B are two full strokes, blended at
    // publication. Modulate is Photoshop's: B is a coverage PATTERN that
    // modulates A's alpha through the blend mode — it contributes no
    // colour and never paints outside A's marks (the ag-psd dualBrush
    // block carries only tip/spacing/scatter/count/blend, so Photoshop's
    // format cannot express an independent second brush).
    enum class DualMode { Composite, Modulate };
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
    // ERASE MODE: the stroke's accumulated coverage REMOVES alpha from the
    // layer instead of depositing colour. Everything upstream of the
    // publication boundary (stamps, tips, hardness, pressure curves, flow,
    // wet edges, the opacity ceiling) is blend-agnostic and behaves
    // identically; only the final composite differs, on both render paths.
    // Erase wins over smudge (smudgeActive() below) and forces the MASK
    // path (usesColorStrokeBuffer() returns false): a smudge, dual or
    // colour-jitter eraser is a contradiction, not a combination.
    bool eraseMode() const { return m_eraseMode; }
    void setEraseMode(bool erase) { m_eraseMode = erase; }
    bool smudgeActive() const { return !m_eraseMode
                                      && m_toolMode == ToolMode::Smudge
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
    TextureBlendMode textureBlendMode() const { return m_textureBlendMode; }
    void setTextureBlendMode(TextureBlendMode mode)
    {
        // Serialised as an int: an out-of-range value (corrupt or future
        // file) falls back to Multiply — the behaviour every brush already
        // had — never to an unmapped enum.
        m_textureBlendMode =
            (int(mode) >= int(TextureBlendMode::Multiply)
             && int(mode) <= int(TextureBlendMode::LinearHeight))
                ? mode
                : TextureBlendMode::Multiply;
    }
    // Wet edges (Phase 6a): pools paint toward the stroke's rim like
    // watercolour. An AMOUNT, not Photoshop's checkbox: 0 = off (the
    // default; bit-identical to the pre-6a engine), ~0.6 reads like the
    // Photoshop toggle, 1 = pure ring. Applied ONCE per stroke at the
    // publication boundary on the ACCUMULATED coverage — never per stamp —
    // and inert while smudging and (in 6a) while the dual brush is on.
    // Build-up / airbrush (Phase 6b): while the pointer is held, stamps
    // keep landing at a TIME-driven cadence even with no movement. An
    // amount, 0 = off (the default; the walk is bit-identical to the
    // pre-6b engine), 1 = 60 stamps/second. Driven by the recorded
    // per-point timestamps — never by live wall clock — so replay,
    // undo/redo and both renderers reproduce a stroke exactly.
    // Colour dynamics (Phase 6c). foregroundBackground jitter interpolates
    // each stamp's colour from the brush colour toward backgroundColor —
    // Photoshop's FG/BG Jitter, the piece imported ABR brushes lose today.
    // It is DynamicProperty::ForegroundBackground (per-stamp, drivable).
    // purity shifts saturation toward grey (-1) or full chroma (+1) after
    // the jitters — deterministic and directional, distinct from the
    // random symmetric saturation jitter. colorDynamicsPerTip false
    // evaluates the colour RNG once per stroke instead of per stamp
    // (Photoshop's "Apply Per Tip" unchecked); default true is the
    // engine's existing per-stamp behaviour.
    qreal fgBgJitter() const { return m_fgBgJitter; }
    void setFgBgJitter(qreal amount);
    const QColor &backgroundColor() const { return m_backgroundColor; }
    void setBackgroundColor(const QColor &color);
    qreal purity() const { return m_purity; }
    void setPurity(qreal amount);
    bool colorDynamicsPerTip() const { return m_colorDynamicsPerTip; }
    void setColorDynamicsPerTip(bool perTip) { m_colorDynamicsPerTip = perTip; }
    PressureCurve &fgBgJitterPressureCurve() { return m_fgBgJitterPressureCurve; }
    const PressureCurve &fgBgJitterPressureCurve() const { return m_fgBgJitterPressureCurve; }

    qreal buildUp() const { return m_buildUp; }
    void setBuildUp(qreal amount);
    qreal wetEdges() const { return m_wetEdges; }
    void setWetEdges(qreal amount);
    // Noise (Phase 6e): per-pixel roughening of the tip's alpha falloff —
    // a STATIC 0-1 amount, like wetEdges, not a dynamic (see NoiseField.h
    // for the model and the rationale).
    qreal noise() const { return m_noise; }
    void setNoise(qreal amount);
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

    // eraseMode forces the MASK path here too: the dual publication shader
    // has no erase branch, so an erase+dual brush would deposit colour -
    // the exact contradiction the guard exists to make unconstructible.
    // (Also keeps the codec from serialising a secondary payload for
    // erasers: walkBrush branches on THIS accessor.)
    bool dualBrushEnabled() const { return !m_eraseMode
                                          && m_dualBrushEnabled
                                          && bool(m_secondaryBrush); }
    void setDualBrushEnabled(bool enabled);
    DualMode dualMode() const { return m_dualMode; }
    void setDualMode(DualMode mode)
    {
        // Serialised as an int: an out-of-range value (corrupt or future
        // file) falls back to Composite — the engine's original dual
        // semantics — never to an unmapped enum.
        m_dualMode = (mode == DualMode::Modulate) ? DualMode::Modulate
                                                  : DualMode::Composite;
    }
    Brush &secondaryBrush();
    const Brush &secondaryBrush() const;
    DualBlendMode dualBlendMode() const { return m_dualBlendMode; }
    void setDualBlendMode(DualBlendMode mode) { m_dualBlendMode = mode; }
    qreal dualMasterOpacity() const { return m_dualMasterOpacity; }
    void setDualMasterOpacity(qreal opacity);

    // --- Static tip transform ---------------------------------------------
    // Photoshop-style STATIC tip controls: applied to every stamp
    // regardless of input. They are NOT dynamics — no ControlSource, no
    // minimum, deliberately absent from DynamicProperty — which is what
    // lets a static angle and a dynamic angle jitter vary independently
    // (jitter varies AROUND the static angle, not around zero). They fold
    // into the ONE per-stamp affine; the composition order is documented
    // at that affine (StrokeBuilder::shapedTipForStamp) and mirrored by
    // the GPU instance build.
    qreal tipAngle() const { return m_tipAngle; }         // degrees
    void setTipAngle(qreal degrees);                      // wrapped (-180,180]
    qreal tipRoundness() const { return m_tipRoundness; } // 1.0 = round
    void setTipRoundness(qreal roundness);                // clamped 0.01..1
    bool tipFlipX() const { return m_tipFlipX; }
    void setTipFlipX(bool flip) { m_tipFlipX = flip; }
    bool tipFlipY() const { return m_tipFlipY; }
    void setTipFlipY(bool flip) { m_tipFlipY = flip; }

    // Empty custom shape selects the procedural round tip. Non-empty images
    // are converted to grayscale and scaled into the cached stamp on demand.
    void setCustomShape(const QImage &grayscaleMask);
    void clearCustomShape();
    bool hasCustomShape() const { return !m_customShape.isNull(); }
    const QImage &customShape() const { return m_customShape; }

    // --- Control source + minimum per dynamic property --------------------
    // source: what drives the property (default Pressure — existing brushes
    //         keep today's behaviour exactly).
    // minimum: floor of the resolved multiplier, 0..1 (default 0.0 — again
    //          today's behaviour). Applied AFTER the curve remaps the
    //          driving value and BEFORE the base value is scaled:
    //              multiplier = minimum + (1 - minimum) * curve(source)
    //          so the resolved value spans [base * minimum, base] as the
    //          source goes 0 -> 1. resolveDynamic() below is the ONE
    //          implementation of that order; every consumer must go through
    //          it so the CPU and GPU paths can never disagree about it.
    ControlSource controlSource(DynamicProperty property) const
    {
        return m_controlSources[size_t(property)];
    }
    void setControlSource(DynamicProperty property, ControlSource source);
    qreal controlMinimum(DynamicProperty property) const
    {
        return m_controlMinimums[size_t(property)];
    }
    void setControlMinimum(DynamicProperty property, qreal minimum);
    const PressureCurve &dynamicCurve(DynamicProperty property) const;
    // The resolved 0-1 multiplier for one property at one input sample.
    // Stamp resolution (StrokeBuilder) calls this for all fourteen slots;
    // stamps are resolved ONCE, before the CPU/GPU split, which is what
    // keeps the two render paths structurally identical here.
    qreal resolveDynamic(DynamicProperty property,
                         const DynamicSource &source) const;

    // FADE source span: canvas px of arc length over which the fade source
    // decays 1 -> 0 (linear, the Photoshop default; the property's curve
    // then remaps that ramp like any other source).
    qreal fadeDistance() const { return m_fadeDistance; }
    void setFadeDistance(qreal canvasPx); // clamped 1..8192

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
    bool m_eraseMode = false;
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
    qreal m_wetEdges = 0.0; // 0 = off; publication-boundary rim pooling
    qreal m_buildUp = 0.0;  // 0 = off; time-driven stamp cadence
    qreal m_noise = 0.0;    // 0 = off; per-pixel falloff roughening
    qreal m_fgBgJitter = 0.0;          // 0 = off; toward backgroundColor
    QColor m_backgroundColor = QColor(Qt::white);
    qreal m_purity = 0.0;              // -1 grey .. +1 full chroma
    bool m_colorDynamicsPerTip = true; // false = one colour per stroke
    PressureCurve m_fgBgJitterPressureCurve;
    qreal m_grainDepth = 0.0;
    qreal m_grainContrast = 1.0;
    qreal m_grainRotation = 0.0;
    GrainMode m_grainMode = GrainMode::StaticCanvas;
    TextureBlendMode m_textureBlendMode = TextureBlendMode::Multiply;
    GrainPreset m_grainPreset = GrainPreset::Paper;
    bool m_grainAffectsColor = false;
    QImage m_grainTexture;
    qreal m_smudgeStrength = 0.0;
    qreal m_hueJitter = 0.0;
    qreal m_saturationJitter = 0.0;
    qreal m_brightnessJitter = 0.0;
    QImage m_customShape;
    qreal m_tipAngle = 0.0;      // degrees; 0 = untransformed
    qreal m_tipRoundness = 1.0;  // 1 = round, toward 0 = flattened
    bool m_tipFlipX = false;
    bool m_tipFlipY = false;
    qreal m_fadeDistance = 256.0; // canvas px; only read when a source is Fade
    std::array<ControlSource, kDynamicPropertyCount> m_controlSources;
    std::array<qreal, kDynamicPropertyCount> m_controlMinimums;
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
    DualMode m_dualMode = DualMode::Composite;
    DualBlendMode m_dualBlendMode = DualBlendMode::NormalOver;
    qreal m_dualMasterOpacity = 1.0;
    std::unique_ptr<Brush> m_secondaryBrush;
};
