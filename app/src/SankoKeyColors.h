#pragma once

#include <QColor>
#include <QString>
#include <QVector>

#include <cmath>
#include <utility>

// KEY COLOURS (batch I-1b round 2, 2026-11-25; the user's note 7, Figma I-1b 701:755 section 5):
// Edit > Preferences > Keyframes chooses the colour of the camera's keys and of the layers' keys,
// each on its own - a preset, or Custom... with the SankoTV colour picker. Purple #7C6EF6 is the
// default for both. A setting of SankoTV on this computer (sankoSettings, "keyframes/..."), never
// in the project file.
//
// WHAT TAKES THE COLOUR: the CAMERA row's keys and the lines between them, and the Camera tool's
// key marks on the canvas (the panel's key frames in Stage mode) - Row::Camera; the LAYER row's
// keys, lines and dim marks, the Layers panel's Animate diamond, and the Move tool's path and key
// marks on the canvas - Row::Layer. The playhead, the buttons and every other purple accent stay
// as they are.
//
// HOW - IN FULL (the user's change before hand test I1b-3, 2026-11-25: "drawn clearly in that
// colour, as the Figma preview I-1b showed, not mapped to today's purple brightness"; the first
// build mapped each part to its tone of the old purple look, and blue barely showed): a resting key
// is filled with the colour (key) and outlined with it a third of the way to white (keyEdge); the
// line between keys is the colour; another layer's dim mark is the colour at 60 %. Today's purple
// keys are a little stronger for it (the user: "that's fine"). A colour too dark to read against
// the timeline's dark ground (#15161C) is lifted, lightness only, just to a 3:1 contrast
// (readable) - every preset is far above it and is drawn as it is.
namespace SankoKeyColors {

enum class Row { Camera, Layer };

inline const QColor kDefault(0x7c, 0x6e, 0xf6);

// The presets, as drawn in I-1b: the default first, then seven.
struct Preset
{
    QString name;
    QColor color;
};
inline QVector<Preset> presets()
{
    return {{QStringLiteral("Purple"), kDefault},
            {QStringLiteral("Blue"), QColor(0x4c, 0x9a, 0xff)},
            {QStringLiteral("Teal"), QColor(0x2e, 0xc4, 0xb6)},
            {QStringLiteral("Green"), QColor(0x6c, 0xc0, 0x4a)},
            {QStringLiteral("Yellow"), QColor(0xf2, 0xb3, 0x3d)},
            {QStringLiteral("Orange"), QColor(0xf2, 0x7a, 0x3d)},
            {QStringLiteral("Pink"), QColor(0xe7, 0x6f, 0xb4)},
            {QStringLiteral("White"), QColor(0xe5, 0xe5, 0xed)}};
}

inline QColor &baseRef(Row row)
{
    static QColor camera = kDefault;
    static QColor layer = kDefault;
    return row == Row::Camera ? camera : layer;
}

// The row's colour as chosen and kept (the settings, the Preferences page's diamonds); what is
// drawn is key() below.
inline QColor base(Row row)
{
    return baseRef(row);
}

inline void setBase(Row row, const QColor &color)
{
    baseRef(row) = color.isValid() ? QColor(color.rgb()) : kDefault;
}

inline bool isDefault(Row row)
{
    return baseRef(row).rgb() == kDefault.rgb();
}

// The ground the timeline's rows are painted on, and the least contrast a key keeps against it
// (WCAG's 3:1 for a graphic).
inline const QColor kGround(0x15, 0x16, 0x1c);
inline constexpr double kMinContrast = 3.0;

inline double luminance(const QColor &c)
{
    auto channel = [](int v) {
        const double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}

inline double contrast(const QColor &a, const QColor &b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb)
        std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

// `color` as it is when it reads at 3:1 against the ground; else its lightness raised (its hue and
// saturation kept) just as far as that needs.
inline QColor readable(const QColor &color)
{
    if (contrast(color, kGround) >= kMinContrast)
        return QColor(color.rgb());
    float h = 0, s = 0, l = 0, a = 0;
    color.getHslF(&h, &s, &l, &a);
    for (int i = 1; i <= 100; ++i) {
        const QColor lifted = QColor::fromHslF(h < 0 ? 0.0f : h, s, l + (1.0f - l) * i / 100.0f);
        if (contrast(lifted, kGround) >= kMinContrast)
            return QColor(lifted.rgb());
    }
    return QColor(Qt::white);
}

// The row's key as drawn: a resting key's fill, the line between keys, the canvas's key marks.
inline QColor key(Row row)
{
    return readable(baseRef(row));
}

// A key's outline - and the CAMERA row's selected key and its move: the colour a third of the way
// to white.
inline QColor keyEdge(Row row)
{
    const QColor k = key(row);
    return QColor(k.red() + (255 - k.red()) / 3, k.green() + (255 - k.green()) / 3,
                  k.blue() + (255 - k.blue()) / 3);
}

// Another layer's key on the LAYER row: the colour, dim.
inline QColor dim(Row row)
{
    QColor c = key(row);
    c.setAlphaF(0.6f);
    return c;
}

// The settings' keys (sankoSettings): the chosen colour, and the row's custom colour (the diamond
// after the presets, once Custom... has picked one).
inline QString settingsKey(Row row)
{
    return row == Row::Camera ? QStringLiteral("keyframes/cameraKeyColor")
                              : QStringLiteral("keyframes/layerKeyColor");
}

inline QString customSettingsKey(Row row)
{
    return row == Row::Camera ? QStringLiteral("keyframes/cameraKeyCustom")
                              : QStringLiteral("keyframes/layerKeyCustom");
}

} // namespace SankoKeyColors
