#pragma once

#include "BrushPreset.h"

#include <QVector>

namespace brushlib {

// The built-in brush roster, DEFINED IN CODE so it ships versioned with the
// app (only per-user state — favourites, hidden, order, user copies — lives
// on disk). Categories and counts are asserted by the library tests.
QVector<BrushPreset> builtinRoster();
// (The dedicated eraser roster is GONE - 2026-08-28: the Eraser Library
// MIRRORS the brush roster, one definition referenced twice. eraseMode is
// applied at activation on a copy; no stored preset carries it.)

// Fixed category order, matching the Figma sidebar (Recent excluded — it is
// usage history, not a category).
QStringList builtinCategories();

} // namespace brushlib
