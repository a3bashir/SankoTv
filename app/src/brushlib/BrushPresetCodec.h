#pragma once

#include "BrushPreset.h"

#include <QByteArray>

namespace brushlib {

// Versioned binary codec for brush presets (.sankobrush).
//
// ANTI-DRIFT DESIGN: there is exactly ONE enumeration of the Brush's fields
// (walkBrush() in the .cpp). Save, load, and settingsHash() all flow through
// that single walker, so a field added to the walker is automatically
// serialised, deserialised, and hashed — and a field NOT added to it is
// missing from all three, which the round-trip render test catches. There is
// no second field list anywhere that could fall out of step.
//
// The rendered-stroke round-trip guarantee reuses the SAME machinery the
// undo system's seeded replay uses: a loaded preset's Brush fed through
// SankoPaintHostAdapter::render() must produce a byte-identical stroke to
// the original (asserted by the library tests, not just claimed).
class BrushPresetCodec
{
public:
    // Brush payload only (no name/id/category) — the unit the preview cache
    // keys on: renames must not invalidate previews. imagesCappedOnLoad
    // (optional) reports that the file carried an image larger than
    // Brush::kMaxCustomImageDim, which the setter capped in memory — the
    // caller's cue to tell the user before the file is ever rewritten in
    // capped form.
    static QByteArray saveBrush(const ::Brush &brush);
    static bool loadBrush(const QByteArray &bytes, ::Brush &out,
                          bool *imagesCappedOnLoad = nullptr);

    // Test seam: number of REAL PNG encodes performed (the encoded-image
    // memo makes repeated saves of an unchanged image cost zero encodes —
    // the per-gesture serialization fix, pinned by the gate).
    static int imageEncodeCountForTest();

    // SHA-256 of saveBrush() — the preview-cache key.
    static QByteArray settingsHash(const ::Brush &brush);

    // Full preset file payload (identity + metadata + brush).
    static QByteArray savePreset(const BrushPreset &preset);
    static bool loadPreset(const QByteArray &bytes, BrushPreset &out);
};

} // namespace brushlib
