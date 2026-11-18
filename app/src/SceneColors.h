#pragma once

#include <QColor>
#include <QString>

#include "StoryboardModel.h"

// THE SCENE COLOURS (batch E, 2026-11-15; Figma E-1, approved with the user's answers): a
// muted set of eight, taken in order - scene 1 Slate, scene 2 Teal ... the ninth starts again -
// while a scene's colour is Auto (Scene::colorTag empty). Left out on purpose: the purple of
// the selection, the amber of the warnings, the pin's blue (#4D9FFF). Only the timeline and
// the Scenes panel draw them: never the canvas, a thumbnail, the film or an export.
namespace scenecolor {

struct Swatch
{
    const char *name;
    QRgb rgb;
};

inline constexpr Swatch kSet[8] = {
    {"Slate", 0x5B6E9C}, {"Teal", 0x4F8A85}, {"Moss", 0x6E8B52}, {"Ochre", 0xA88B4C},
    {"Rust", 0xA3654F},  {"Plum", 0x8C5D88}, {"Steel", 0x6C7D8E}, {"Sand", 0x98856A},
};
inline constexpr int kSetSize = 8;

inline const Swatch &autoSwatch(int sceneIndex)
{
    return kSet[((sceneIndex % kSetSize) + kSetSize) % kSetSize];
}

// The colour a scene shows: its own once chosen, else the set's at its place.
inline QColor colorOf(const Scene &scene, int sceneIndex)
{
    const QColor own(scene.colorTag);
    return !scene.colorTag.isEmpty() && own.isValid() ? own : QColor(autoSwatch(sceneIndex).rgb);
}

// "#RRGGBB" in capitals, the form a chosen colour is kept in.
inline QString tagOf(const QColor &color)
{
    return color.name(QColor::HexRgb).toUpper();
}

} // namespace scenecolor
