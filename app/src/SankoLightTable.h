#pragma once

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QRegularExpression>
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
// "LAYER row button: onion (on)"): the Light Table's own icon, icons/lighttable.svg, with nothing
// added: the Light Table's button in the panel strip and the Onion Skin's on the LAYER row alike.
// On, a button says so with its purple border and purple background only; the ghosts' colours
// never come into the icons.
//
// IN THE COLOUR AND THE LINE OF THE ICONS AROUND IT (the user's word after hand test OS1,
// 2026-11-27; in OS-1 it was white with the file's line in both places). The file draws the
// bulb as Figma exports an inside stroke: the outline's shapes as a mask, and over them a line
// 1.5 of its 13 x 21 units thick inside the outline. bulbSvg keeps the mask - the outline, the
// shape and its proportions, as drawn - and draws inside it a line `line` units thick in
// `color`: bulbSvg(#CCCCCC, 1.5) is the file's own drawing.
//   - On the LAYER row, the Eye's and the Speaker's: their grey, #a4a7ae (the tint both are
//     drawn in, SankoTimelineTheme::kTrackControl), and their line: track_eye.svg's 1.15 and
//     track_speaker.svg's 1.2 and 1.1, drawn at their 16 px, read 1.13 and 1.17 px by the line
//     measure of the checks ((sy2), (bu1), at 32 times); the bulb's reads 1.17, within 0.02 of
//     their middle (the measure reads in sixteenths of a pixel; 1.68 to 1.74 units all read so,
//     1.66 reads 1.11). It is drawn there 9 x 14.5 (its 13 x 21 units at 0.691): 1.71 units,
//     1.18 px as written.
//   - In the panel strip, Duplicate's and Delete's: their #CCCCCC (their files' colour, drawn as
//     it is) and 1.32 px, between Duplicate's 1.33 (1.5 of its 18 drawn at 16) and Delete's 1.31
//     across its uprights (1.5 of its 16 drawn at 14; 1.24 across its bars, 17 drawn at 14). The
//     bulb is drawn there 20 high, its proportions kept (0.952 of its units), so 1.386 units.
inline const QString kIconPath = QStringLiteral(":/icons/lighttable.svg");
inline constexpr double kFileLine = 1.5;      // the file's own line, in its units
inline constexpr double kLayerRowLine = 1.71;  // 1.18 px at 9 x 14.5 (reads 1.17)
inline constexpr double kStripLine = 1.386;    // 1.32 px at 20 high
inline const QColor kStripColor(0xcc, 0xcc, 0xcc); // duplicate.svg's and delete.svg's
inline QByteArray bulbSvg(const QColor &color, double line)
{
    QFile file(kIconPath);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    const QByteArray svg = file.readAll();
    const qsizetype head = svg.indexOf('>'); // the end of the <svg ...> tag
    const qsizetype maskStart = svg.indexOf("<mask"), maskEnd = svg.indexOf("</mask>");
    if (head < 0 || maskStart <= head || maskEnd < maskStart)
        return QByteArray();
    const QByteArray mask = svg.mid(maskStart, maskEnd + 7 - maskStart);
    const QRegularExpressionMatch id = QRegularExpression(QStringLiteral("id=\"([^\"]+)\"")).match(QString::fromLatin1(mask));
    if (!id.hasMatch())
        return QByteArray();
    // The outline's shapes as one path, masked once: two would each be blended into the mask's
    // soft edge where they meet (the base's top), drawing that edge twice.
    static const QRegularExpression pathData(QStringLiteral("\\sd=\"([^\"]+)\""));
    QByteArray outline;
    for (QRegularExpressionMatchIterator it = pathData.globalMatch(QString::fromLatin1(mask)); it.hasNext();)
        outline += (outline.isEmpty() ? "" : " ") + it.next().captured(1).toLatin1();
    if (outline.isEmpty())
        return QByteArray();
    return svg.left(head + 1) + mask + "<path d=\"" + outline + "\" stroke=\"" + color.name().toLatin1()
        + "\" stroke-width=\"" + QByteArray::number(2.0 * line, 'f', 4) + "\" fill=\"none\" mask=\"url(#"
        + id.captured(1).toLatin1() + ")\"/></svg>";
}

} // namespace SankoLightTable
