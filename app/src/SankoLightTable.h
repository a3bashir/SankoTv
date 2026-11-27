#pragma once

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QString>

// THE LIGHT TABLE'S SETTINGS (OS-1, 2026-11-27; the user's answers after hand test SL, Figma
// OS-1 745:757 section 4 A): Edit > Preferences > Light Table, made like the Onion Skin page. For
// the panel BEFORE the one on the canvas (as it ends: its last frame) and the panel AFTER it (as
// it begins: its first frame), each on its own: the ghost's colour (a hue slider, as the
// Perspective guides and the Onion Skin have) and its opacity. A setting of SankoTV on this
// computer (sankoSettings, "lightTable/..."), never in the project file - as is the switch
// itself (switchKey), remembered after a restart.
//
// THE DEFAULTS: the red and the green the Light Table has always had, at its 35 %.
//
// WHAT TAKES THEM: only the Light Table - the panels before and after, under the drawing. The
// frames of a layer before and after are the Onion Skin's (SankoOnionSkin.h); neither switch
// turns the other off.
namespace SankoLightTable {

enum class Side { Before, After };

inline const QColor kBeforeDefault(0xff, 0x4d, 0x4d); // the Light Table's red since it was made
inline const QColor kAfterDefault(0x4d, 0xff, 0x91);  // ...and its green
inline constexpr int kDefaultOpacityPercent = 35;
inline constexpr int kMinOpacityPercent = 5;

struct Choice
{
    QColor color;
    int opacityPercent = kDefaultOpacityPercent;
};

inline QColor defaultColor(Side side)
{
    return side == Side::Before ? kBeforeDefault : kAfterDefault;
}

inline Choice &ref(Side side)
{
    static Choice before{kBeforeDefault, kDefaultOpacityPercent};
    static Choice after{kAfterDefault, kDefaultOpacityPercent};
    return side == Side::Before ? before : after;
}

inline Choice choice(Side side)
{
    return ref(side);
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
        if (c.color.rgb() != defaultColor(side).rgb() || c.opacityPercent != kDefaultOpacityPercent)
            return false;
    }
    return true;
}

inline void setDefaults()
{
    for (Side side : {Side::Before, Side::After})
        ref(side) = Choice{defaultColor(side), kDefaultOpacityPercent};
}

inline double opacity(Side side)
{
    return ref(side).opacityPercent / 100.0;
}

inline QString sideName(Side side)
{
    return side == Side::Before ? QStringLiteral("before") : QStringLiteral("after");
}

// The settings' keys (sankoSettings): lightTable/<before|after><Color|Opacity>, and the switch.
inline QString colorKey(Side side) { return QStringLiteral("lightTable/") + sideName(side) + QStringLiteral("Color"); }
inline QString opacityKey(Side side) { return QStringLiteral("lightTable/") + sideName(side) + QStringLiteral("Opacity"); }
inline QString switchKey() { return QStringLiteral("lightTable/on"); }

// THE ICON OF BOTH SWITCHES (the user's word after hand test SL; their Figma frame 749:892,
// "LAYER row button: onion (on)"): the Light Table's own icon, icons/lighttable.svg - drawn there
// in #CCCCCC - in white, with nothing added: the Light Table's button in the panel strip and the
// Onion Skin's on the LAYER row alike. On, a button says so with its purple border and purple
// background only; the ghosts' colours never come into the icons.
inline const QString kIconPath = QStringLiteral(":/icons/lighttable.svg");
inline QByteArray whiteIconSvg()
{
    QFile file(kIconPath);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    QByteArray svg = file.readAll();
    svg.replace("#CCCCCC", "#FFFFFF");
    return svg;
}

} // namespace SankoLightTable
