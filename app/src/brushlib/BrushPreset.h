#pragma once

#include "Brush.h"

#include <QString>

namespace brushlib {

// One library entry: identity + shelf metadata + the COMPLETE engine brush.
// The Brush member is the same class the engine renders from and the undo
// system's replay snapshots copy — there is no reduced "preset view" of a
// brush that could drift from what the engine actually uses.
struct BrushPreset
{
    QString id;       // stable: "builtin/<category-slug>/<name-slug>" or "user/<uuid>"
    QString name;     // display name (renameable; not part of settingsHash)
    QString category; // "Sketching" | "Drawing" | "Inking" | "Painting"
                      // | "Artistic" | "Watercolor"  (never "Recent")
    bool builtin = false;
    ::Brush brush;
    // TRANSIENT (never serialised): the preset FILE carries an image larger
    // than Brush::kMaxCustomImageDim, capped in memory at load. The studio
    // tells the user before Done rewrites the file in capped form; the
    // model clears this after that write. Pre-cap files only.
    bool imagesCappedOnLoad = false;
};

} // namespace brushlib
