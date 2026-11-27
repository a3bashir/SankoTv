#pragma once

#include <QColor>
#include <QString>

// THE ONION SKIN'S SETTINGS (batch I-2, 2026-11-26; the user's answer 5, Figma I-2 727:757
// sections 3 and 3b): Edit > Preferences > Onion Skin. For the drawings BEFORE the one on the
// canvas and for the drawings AFTER it, each on its own: how many are shown (0 to 3; 0: none on
// that side), the ghost's colour (since 2026-11-27 a hue slider, as the Perspective tool's
// guides have - the hue at full strength; until then eight presets and Custom...) and its
// opacity. A setting of SankoTV on this computer (sankoSettings, "onionSkin/..."), never in the
// project file.
//
// THE DEFAULTS: one before, in the blue the onion skin has always had, at its 30 %; one after,
// in orange at 30 %. Blue and orange stay apart on white paper and beside black ink, and they
// are the pair most colour-blind eyes still tell apart (red and green, the usual pair, is the
// one they cannot).
//
// WHAT TAKES THEM: on a layer with a run of drawings, the ghosts of that layer's drawings
// before and after the one under the playhead - the nearest at the opacity set, each one
// further away at kFalloff of the one before it. The "before" colour and opacity are also the
// previous PANEL's ghost: on a run's first drawing, and on a layer with a single drawing, where
// it is what the onion skin showed before I-2. The Light Table (previous panel red, next green)
// is another thing and is not set here. The O key switches the onion skin on and off.
namespace SankoOnionSkin {

enum class Side { Before, After };

inline const QColor kBeforeDefault(0x4d, 0x9f, 0xff); // the onion skin's blue since it was made
inline const QColor kAfterDefault(0xf2, 0x7a, 0x3d);  // the key colours' Orange
inline constexpr int kDefaultCount = 1;
inline constexpr int kMaxCount = 3;
inline constexpr int kDefaultOpacityPercent = 30;
inline constexpr int kMinOpacityPercent = 5;
inline constexpr double kFalloff = 0.6;

struct Choice
{
    int count = kDefaultCount;
    QColor color;
    int opacityPercent = kDefaultOpacityPercent;
};

inline QColor defaultColor(Side side)
{
    return side == Side::Before ? kBeforeDefault : kAfterDefault;
}

inline Choice &ref(Side side)
{
    static Choice before{kDefaultCount, kBeforeDefault, kDefaultOpacityPercent};
    static Choice after{kDefaultCount, kAfterDefault, kDefaultOpacityPercent};
    return side == Side::Before ? before : after;
}

inline Choice choice(Side side)
{
    return ref(side);
}

inline void setCount(Side side, int count)
{
    ref(side).count = count < 0 ? 0 : (count > kMaxCount ? kMaxCount : count);
}

inline void setColor(Side side, const QColor &color)
{
    ref(side).color = color.isValid() ? QColor(color.rgb()) : defaultColor(side);
}

inline void setOpacityPercent(Side side, int percent)
{
    ref(side).opacityPercent = percent < kMinOpacityPercent ? kMinOpacityPercent : (percent > 100 ? 100 : percent);
}

inline bool isDefault()
{
    for (Side side : {Side::Before, Side::After}) {
        const Choice &c = ref(side);
        if (c.count != kDefaultCount || c.color.rgb() != defaultColor(side).rgb()
            || c.opacityPercent != kDefaultOpacityPercent)
            return false;
    }
    return true;
}

inline void setDefaults()
{
    for (Side side : {Side::Before, Side::After})
        ref(side) = Choice{kDefaultCount, defaultColor(side), kDefaultOpacityPercent};
}

// The opacity a ghost is painted with: `distance` 1 the drawing next to the one on the canvas.
inline double opacityAt(Side side, int distance)
{
    double o = ref(side).opacityPercent / 100.0;
    for (int i = 1; i < distance; ++i)
        o *= kFalloff;
    return o;
}

inline QString sideName(Side side)
{
    return side == Side::Before ? QStringLiteral("before") : QStringLiteral("after");
}

// The settings' keys (sankoSettings): onionSkin/<before|after><Count|Color|Opacity>.
// (...Custom, the colour Custom... had last picked, is no longer written or read: 2026-11-27.)
inline QString countKey(Side side) { return QStringLiteral("onionSkin/") + sideName(side) + QStringLiteral("Count"); }
inline QString colorKey(Side side) { return QStringLiteral("onionSkin/") + sideName(side) + QStringLiteral("Color"); }
inline QString opacityKey(Side side) { return QStringLiteral("onionSkin/") + sideName(side) + QStringLiteral("Opacity"); }

} // namespace SankoOnionSkin
