#include "BrushPresetCodec.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>

#include <type_traits>

namespace brushlib {
namespace {

constexpr quint32 kBrushMagic = 0x534E4B42;  // "SNKB"
constexpr quint32 kPresetMagic = 0x534E4B50; // "SNKP"
// Wire format history — writers always stamp the NEWEST version; readers
// accept every version listed here, loading older files onto a fresh
// (default) Brush so absent-by-design fields keep their defaults:
//   v1  the original field list.
//   v2  + per-dynamic-property control source and minimum (14 x 2 fields,
//       inserted after the pressure curves). A v1 file loads with
//       source = Pressure and minimum = 0.0 everywhere — exactly its
//       pre-v2 behaviour. A v2 file handed to a v1-era build is REFUSED
//       cleanly (its version check is `!= 1`), never misparsed.
//   v3  + the static tip transform (angle, roundness, flip X, flip Y).
//       v1 and v2 files land on the defaults (0 deg, roundness 1.0, no
//       flips) — again their exact prior behaviour.
//   v4  + fadeDistance (canvas px the Fade source decays over). Older
//       files land on the default 256; the field is only read when a
//       control source is Fade, which no pre-v4 file can have set to a
//       distance other than the default anyway.
//   v11: eraseMode (the Eraser Library engine pass).
constexpr quint16 kVersion = 11;
constexpr quint16 kMinReadVersion = 1;

// A fixed QDataStream version pins the wire format independently of the Qt
// the app is built with — presets written today load byte-identically later.
constexpr QDataStream::Version kStreamVersion = QDataStream::Qt_6_5;

// Encoded-image memo (2026-08-29). The studio serialises the whole session
// brush for undo bookkeeping on EVERY gesture, and PNG-encoding an embedded
// image costs up to hundreds of ms - a 14 MB photo-preset re-encoded per
// slider move froze the studio (measured: 428 ms per encode at 18 MP,
// 69 ms at the 2048 cap). Images are immutable between gestures, so the
// bytes are memoised on QImage::cacheKey().
//
// WHY cacheKey IS SAFE AS THE KEY (the stale-memo hazard, considered):
// Qt documents cacheKey as "a number that identifies the contents of this
// QImage object", where "distinct QImage objects can only have the same
// key if they refer to the same contents" - the key is minted per data
// allocation and re-minted on detach, so REPLACING an image with a
// different one of identical dimensions yields a different key, and
// modifying an image in place detaches and re-mints. Both properties are
// PINNED by the gate (b10), not assumed: two same-dimension images must
// key differently, and swapping a brush's grain for a same-size image
// must serialise the NEW bytes.
//
// Determinism: the memo stores exactly the bytes it produced, so repeated
// saves are byte-identical by construction (the codec idempotence checks
// b1/b2 stand guard). Bounded at 16 entries - presets carry at most a few
// images; on overflow the memo clears rather than evicts (simplicity over
// an LRU nothing needs).
QMutex g_encodeMemoMutex;
QHash<qint64, QByteArray> g_encodeMemo;
int g_imageEncodeCount = 0; // test seam: how many REAL encodes ran

QByteArray encodePng(const QImage &image)
{
    if (image.isNull())
        return QByteArray();
    QMutexLocker lock(&g_encodeMemoMutex);
    const qint64 key = image.cacheKey();
    const auto it = g_encodeMemo.constFind(key);
    if (it != g_encodeMemo.constEnd())
        return it.value();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    ++g_imageEncodeCount;
    if (g_encodeMemo.size() >= 16)
        g_encodeMemo.clear();
    g_encodeMemo.insert(key, png);
    return png;
}

QImage decodePng(const QByteArray &png)
{
    if (png.isEmpty())
        return QImage();
    return QImage::fromData(png, "PNG");
}

// --- The single field walker ----------------------------------------------
// Both directions run THIS function; a Writer or Reader visitor supplies the
// direction. Order is the wire format — append new fields at the end under a
// version bump, never reorder.

struct Writer
{
    QDataStream &s;
    template <typename Get, typename Set> void field(Get get, Set)
    {
        s << get();
    }
    void curve(PressureCurve &c) { s << c.controlPoints(); }
    template <typename Get, typename Set> void image(Get get, Set)
    {
        s << encodePng(get());
    }
    template <typename Get> bool branch(Get get)
    {
        const bool value = get();
        s << value;
        return value;
    }
};

struct Reader
{
    QDataStream &s;
    template <typename Get, typename Set> void field(Get, Set set)
    {
        std::decay_t<std::invoke_result_t<Get>> value{};
        s >> value;
        set(value);
    }
    void curve(PressureCurve &c)
    {
        QVector<QPointF> points;
        s >> points;
        c.setControlPoints(points);
    }
    template <typename Get, typename Set> void image(Get, Set set)
    {
        QByteArray png;
        s >> png;
        const QImage img = decodePng(png);
        // A pre-cap file can carry an oversized image; the Brush setter
        // caps it in memory, and this flag lets the caller tell the user
        // BEFORE the file is ever rewritten in capped form (the "flag the
        // re-save, never silently rewrite" rule). Accumulates across the
        // dual-brush nested walk.
        if (img.width() > ::Brush::kMaxCustomImageDim
            || img.height() > ::Brush::kMaxCustomImageDim)
            cappedOnLoad = true;
        set(img);
    }
    template <typename Get> bool branch(Get)
    {
        bool value = false;
        s >> value;
        return value;
    }
    bool cappedOnLoad = false;
};

// wireVersion gates the versioned field blocks: the walker IS the format,
// in both directions, so a version-1 walk reads/writes exactly the v1
// layout and a version-2 walk adds the v2 block — no second field list to
// drift from.
template <typename V>
void walkBrush(::Brush &b, V &v, quint16 wireVersion, int depth = 0)
{
    using B = ::Brush;
    // Base settings.
    v.field([&] { return b.size(); }, [&](int x) { b.setSize(x); });
    v.field([&] { return b.spacing(); }, [&](qreal x) { b.setSpacing(x); });
    v.field([&] { return b.opacity(); }, [&](qreal x) { b.setOpacity(x); });
    v.field([&] { return b.flow(); }, [&](qreal x) { b.setFlow(x); });
    v.field([&] { return b.hardness(); }, [&](qreal x) { b.setHardness(x); });
    v.field([&] { return b.color(); },
            [&](const QColor &x) { b.setColor(x); });
    v.field([&] { return qint32(b.toolMode()); },
            [&](qint32 x) { b.setToolMode(B::ToolMode(x)); });
    v.field([&] { return b.smudgeStrength(); },
            [&](qreal x) { b.setSmudgeStrength(x); });
    // Tilt / rotation response.
    v.field([&] { return b.tiltAffectsShape(); },
            [&](bool x) { b.setTiltAffectsShape(x); });
    v.field([&] { return b.rotationAffectsShape(); },
            [&](bool x) { b.setRotationAffectsShape(x); });
    v.field([&] { return b.maxTiltElongation(); },
            [&](qreal x) { b.setMaxTiltElongation(x); });
    // Scatter + jitter.
    v.field([&] { return b.scatterAlong(); },
            [&](qreal x) { b.setScatterAlong(x); });
    v.field([&] { return b.scatterPerpendicular(); },
            [&](qreal x) { b.setScatterPerpendicular(x); });
    v.field([&] { return b.scatterCount(); },
            [&](int x) { b.setScatterCount(x); });
    v.field([&] { return b.sizeJitter(); },
            [&](qreal x) { b.setSizeJitter(x); });
    v.field([&] { return b.angleJitter(); },
            [&](qreal x) { b.setAngleJitter(x); });
    v.field([&] { return b.roundnessJitter(); },
            [&](qreal x) { b.setRoundnessJitter(x); });
    v.field([&] { return b.spacingJitter(); },
            [&](qreal x) { b.setSpacingJitter(x); });
    // Grain. Preset FIRST (a non-Custom preset loads its built-in texture);
    // the custom-grain image slot is UNCONDITIONAL (null unless Custom) so
    // reader and writer never branch differently. setCustomGrain() flips the
    // preset to Custom, restoring the exact written state on load.
    v.field([&] { return b.grainScale(); },
            [&](qreal x) { b.setGrainScale(x); });
    v.field([&] { return b.grainDepth(); },
            [&](qreal x) { b.setGrainDepth(x); });
    v.field([&] { return b.grainContrast(); },
            [&](qreal x) { b.setGrainContrast(x); });
    v.field([&] { return b.grainRotation(); },
            [&](qreal x) { b.setGrainRotation(x); });
    v.field([&] { return qint32(b.grainMode()); },
            [&](qint32 x) { b.setGrainMode(B::GrainMode(x)); });
    v.field([&] { return b.grainAffectsColor(); },
            [&](bool x) { b.setGrainAffectsColor(x); });
    v.field([&] { return qint32(b.grainPreset()); },
            [&](qint32 x) {
                if (B::GrainPreset(x) != B::GrainPreset::Custom)
                    b.setGrainPreset(B::GrainPreset(x));
            });
    v.image([&] {
                return b.grainPreset() == B::GrainPreset::Custom
                    ? b.grainTexture()
                    : QImage();
            },
            [&](const QImage &img) {
                if (!img.isNull())
                    b.setCustomGrain(img);
            });
    // Colour dynamics.
    v.field([&] { return b.hueJitter(); },
            [&](qreal x) { b.setHueJitter(x); });
    v.field([&] { return b.saturationJitter(); },
            [&](qreal x) { b.setSaturationJitter(x); });
    v.field([&] { return b.brightnessJitter(); },
            [&](qreal x) { b.setBrightnessJitter(x); });
    // Custom tip (null slot = keep the procedural round tip).
    // ALWAYS applied — setCustomShape(null) clears. The old non-null guard
    // left a phantom tip on the SECONDARY of a dual brush whose B is
    // procedural: setDualBrushEnabled(true) during the read clones the
    // partially-loaded primary (including its custom shape) into the
    // secondary, and skipping the empty image left that clone in place —
    // B reloaded with A's tip and the preset re-saved differently.
    // (Grain has no such hole: reading the grainPreset field regenerates
    // the procedural texture for every non-Custom preset.)
    v.image([&] { return b.customShape(); },
            [&](const QImage &img) { b.setCustomShape(img); });
    // Dual-brush configuration (blend + master always; payload below).
    v.field([&] { return qint32(b.dualBlendMode()); },
            [&](qint32 x) { b.setDualBlendMode(B::DualBlendMode(x)); });
    v.field([&] { return b.dualMasterOpacity(); },
            [&](qreal x) { b.setDualMasterOpacity(x); });
    // Every per-property pressure curve, fixed order.
    v.curve(b.sizePressureCurve());
    v.curve(b.opacityPressureCurve());
    v.curve(b.hardnessPressureCurve());
    v.curve(b.flowPressureCurve());
    v.curve(b.scatterPressureCurve());
    v.curve(b.smudgePressureCurve());
    v.curve(b.sizeJitterPressureCurve());
    v.curve(b.angleJitterPressureCurve());
    v.curve(b.roundnessJitterPressureCurve());
    v.curve(b.spacingJitterPressureCurve());
    v.curve(b.grainDepthPressureCurve());
    v.curve(b.hueJitterPressureCurve());
    v.curve(b.saturationJitterPressureCurve());
    v.curve(b.brightnessJitterPressureCurve());
    // v2: control source + minimum per dynamic property, in the SAME order
    // as the curves above. Sits before the dual-brush branch so the
    // secondary brush carries its own block inside its own recursive walk.
    // A v1 file simply has no block; the fresh Brush the reader loads into
    // already defaults to Pressure / 0.0 — its exact pre-v2 behaviour.
    if (wireVersion >= 2) {
        // FROZEN at the original 14 properties. kDynamicPropertyCount grew
        // to 15 in Phase 6c (ForegroundBackground), but this block's wire
        // layout was defined when there were 14 and every v2..v6 file
        // carries exactly 14 entries. The 15th property's source, minimum
        // and curve live in the v7 block below.
        for (int i = 0; i < 14; ++i) {
            const auto property = B::DynamicProperty(i);
            v.field(
                [&] { return qint32(b.controlSource(property)); },
                [&](qint32 x) {
                    b.setControlSource(property, B::ControlSource(x));
                });
            v.field([&] { return b.controlMinimum(property); },
                    [&](qreal x) { b.setControlMinimum(property, x); });
        }
    }
    // v3: the static tip transform. Before the dual-brush branch for the
    // same reason as the v2 block: the secondary carries its own copy
    // inside its own recursive walk.
    if (wireVersion >= 3) {
        v.field([&] { return b.tipAngle(); },
                [&](qreal x) { b.setTipAngle(x); });
        v.field([&] { return b.tipRoundness(); },
                [&](qreal x) { b.setTipRoundness(x); });
        v.field([&] { return b.tipFlipX(); },
                [&](bool x) { b.setTipFlipX(x); });
        v.field([&] { return b.tipFlipY(); },
                [&](bool x) { b.setTipFlipY(x); });
    }
    // v4: the Fade source's decay span. Before the dual-brush branch, as
    // with every versioned block, so the secondary carries its own copy.
    if (wireVersion >= 4) {
        v.field([&] { return b.fadeDistance(); },
                [&](qreal x) { b.setFadeDistance(x); });
    }
    // v5: wet edges (Phase 6a). Before the dual-brush branch, as with
    // every versioned block, so the secondary carries its own copy. A v4
    // or older file has no field; the fresh Brush defaults to 0.0 — off,
    // its exact pre-6a behaviour.
    if (wireVersion >= 5) {
        v.field([&] { return b.wetEdges(); },
                [&](qreal x) { b.setWetEdges(x); });
    }
    // v6: build-up / airbrush (Phase 6b). Before the dual-brush branch,
    // as with every versioned block. A v5 or older file has no field; the
    // fresh Brush defaults to 0.0 — off, its exact pre-6b behaviour.
    if (wireVersion >= 6) {
        v.field([&] { return b.buildUp(); },
                [&](qreal x) { b.setBuildUp(x); });
    }
    // v7: colour dynamics (Phase 6c). Before the dual-brush branch, as
    // with every versioned block. A v6 or older file has no fields; the
    // fresh Brush defaults (jitter 0, white background, purity 0, per-tip
    // true, Pressure source, minimum 0, linear curve) render identically.
    if (wireVersion >= 7) {
        v.field([&] { return b.fgBgJitter(); },
                [&](qreal x) { b.setFgBgJitter(x); });
        v.field([&] { return b.backgroundColor(); },
                [&](const QColor &x) { b.setBackgroundColor(x); });
        v.field([&] { return b.purity(); },
                [&](qreal x) { b.setPurity(x); });
        v.field([&] { return b.colorDynamicsPerTip(); },
                [&](bool x) { b.setColorDynamicsPerTip(x); });
        v.field(
            [&] {
                return qint32(b.controlSource(
                    B::DynamicProperty::ForegroundBackground));
            },
            [&](qint32 x) {
                b.setControlSource(B::DynamicProperty::ForegroundBackground,
                                   B::ControlSource(x));
            });
        v.field(
            [&] {
                return b.controlMinimum(
                    B::DynamicProperty::ForegroundBackground);
            },
            [&](qreal x) {
                b.setControlMinimum(B::DynamicProperty::ForegroundBackground,
                                    x);
            });
        v.curve(b.fgBgJitterPressureCurve());
    }
    // v8: dual-brush combination semantics (Phase 6d). Before the dual
    // branch, as with every versioned block. A v7 or older file has no
    // field; the fresh Brush defaults to Composite — the engine's
    // original dual semantics, its exact pre-6d behaviour.
    if (wireVersion >= 8) {
        v.field([&] { return qint32(b.dualMode()); },
                [&](qint32 x) { b.setDualMode(B::DualMode(x)); });
    }
    // v9: noise (Phase 6e). Before the dual branch, as with every
    // versioned block. A v8 or older file has no field; the fresh Brush
    // defaults to 0.0 — off, its exact pre-6e behaviour. NOT a dynamic:
    // the frozen v2 dynamics block stays at 14 entries untouched.
    if (wireVersion >= 9) {
        v.field([&] { return b.noise(); },
                [&](qreal x) { b.setNoise(x); });
    }
    // v10: texture blend mode. Before the dual branch, as with every
    // versioned block. A v9 or older file has no field; the fresh Brush
    // defaults to Multiply — the engine's original texture behaviour, its
    // exact prior rendering. The setter clamps out-of-range values back
    // to Multiply, never to an unmapped enum.
    if (wireVersion >= 10) {
        v.field([&] { return qint32(b.textureBlendMode()); },
                [&](qint32 x) {
                    b.setTextureBlendMode(B::TextureBlendMode(x));
                });
    }
    // v11: erase mode (the Eraser Library). A v10 or older file has no
    // field; the fresh Brush defaults to false - every existing preset
    // paints exactly as before. Old builds refuse v11 files cleanly at the
    // wrapper version check, never misparse them.
    if (wireVersion >= 11) {
        v.field([&] { return b.eraseMode(); },
                [&](bool x) { b.setEraseMode(x); });
    }
    // Secondary brush: one level deep, exactly like the engine renders it
    // (primary slot 0 + secondary slot 1). A disabled dual brush serialises
    // no secondary payload — presets describe what the brush DOES.
    const bool secondary =
        v.branch([&] { return depth == 0 && b.dualBrushEnabled(); });
    if (secondary && depth == 0) {
        b.setDualBrushEnabled(true);
        walkBrush(b.secondaryBrush(), v, wireVersion, depth + 1);
    } else if (depth == 0) {
        b.setDualBrushEnabled(false);
    }
}

} // namespace

QByteArray BrushPresetCodec::saveBrush(const ::Brush &brush)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(kStreamVersion);
    s << kBrushMagic << kVersion; // writing always produces the newest
    Writer w{s};
    // Writer only calls getters, so the const_cast never mutates.
    walkBrush(const_cast<::Brush &>(brush), w, kVersion);
    return bytes;
}

bool BrushPresetCodec::loadBrush(const QByteArray &bytes, ::Brush &out,
                                 bool *imagesCappedOnLoad)
{
    QDataStream s(bytes);
    s.setVersion(kStreamVersion);
    quint32 magic = 0;
    quint16 version = 0;
    s >> magic >> version;
    if (magic != kBrushMagic || version < kMinReadVersion
        || version > kVersion)
        return false; // unknown future format: refuse, never misparse
    ::Brush fresh; // load into defaults so absent-by-design state is clean
    Reader r{s};
    walkBrush(fresh, r, version);
    if (s.status() != QDataStream::Ok)
        return false;
    out = fresh;
    if (imagesCappedOnLoad)
        *imagesCappedOnLoad = r.cappedOnLoad;
    return true;
}

QByteArray BrushPresetCodec::settingsHash(const ::Brush &brush)
{
    return QCryptographicHash::hash(saveBrush(brush),
                                    QCryptographicHash::Sha256);
}

QByteArray BrushPresetCodec::savePreset(const BrushPreset &preset)
{
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(kStreamVersion);
    s << kPresetMagic << kVersion << preset.id << preset.name
      << preset.category << preset.builtin << saveBrush(preset.brush);
    return bytes;
}

bool BrushPresetCodec::loadPreset(const QByteArray &bytes, BrushPreset &out)
{
    QDataStream s(bytes);
    s.setVersion(kStreamVersion);
    quint32 magic = 0;
    quint16 version = 0;
    s >> magic >> version;
    // The preset wrapper's own fields are unchanged since v1; its version
    // tracks kVersion so a v2 file meets a v1-era build's `!= 1` check and
    // is refused at the WRAPPER, before any field is misread.
    if (magic != kPresetMagic || version < kMinReadVersion
        || version > kVersion)
        return false;
    BrushPreset p;
    QByteArray brushBytes;
    s >> p.id >> p.name >> p.category >> p.builtin >> brushBytes;
    bool capped = false;
    if (s.status() != QDataStream::Ok
        || !loadBrush(brushBytes, p.brush, &capped))
        return false;
    p.imagesCappedOnLoad = capped;
    out = p;
    return true;
}

int BrushPresetCodec::imageEncodeCountForTest()
{
    QMutexLocker lock(&g_encodeMemoMutex);
    return g_imageEncodeCount;
}

} // namespace brushlib
