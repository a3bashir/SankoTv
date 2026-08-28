#pragma once

#include "BrushPreset.h"

#include <QVector>

namespace brushlib {

// The built-in brush roster, DEFINED IN CODE so it ships versioned with the
// app (only per-user state — favourites, hidden, order, user copies — lives
// on disk). Categories and counts are asserted by the library tests.
QVector<BrushPreset> builtinRoster();

// The eraser presets - a SEPARATE roster with its own pinned preview
// SHA, never merged into builtinRoster() (whose SHA and per-category
// counts are pinned baselines). Eight erasers; see the .cpp.
QVector<BrushPreset> builtinEraserRoster();

// Fixed category order, matching the Figma sidebar (Recent excluded — it is
// usage history, not a category).
QStringList builtinCategories();

} // namespace brushlib
