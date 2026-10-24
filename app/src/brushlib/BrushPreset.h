#pragma once

#include "Brush.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace brushlib {

// The content keys of a preset's brush (2026-10-24): the preview-cache key
// of its swatch, of its ERASE swatch (the Eraser Library's variant), and
// its width-ratio key. Hashing a brush means PNG-encoding its tip (67 ms
// per 1254x1254 tip in Release), so these are computed ONCE, off the UI
// thread, and stored on the preset; see BrushPreset below for why they can
// never go stale.
struct BrushKeys
{
    QByteArray swatch;      // BrushPresetCodec::settingsHash(brush)
    QByteArray eraseSwatch; // ...of the brush with eraseMode forced on
    QByteArray width;       // BrushWidthRatio::key(brush)
    bool valid = false;
    bool operator==(const BrushKeys &o) const
    {
        return valid == o.valid && swatch == o.swatch
            && eraseSwatch == o.eraseSwatch && width == o.width;
    }
    bool operator!=(const BrushKeys &o) const { return !(*this == o); }
};

// One library entry: identity + shelf metadata + the COMPLETE engine brush.
// The Brush member is the same class the engine renders from and the undo
// system's replay snapshots copy — there is no reduced "preset view" of a
// brush that could drift from what the engine actually uses.
//
// THE BRUSH IS REACHABLE ONLY THROUGH setBrush() (2026-10-24): the member
// is private, so there is no assignment path that could leave the stored
// keys describing another brush - setBrush() drops them and bumps the
// revision, and storeKeys() accepts keys only for the revision they were
// computed from. Debug builds assert on EVERY read of stored keys that
// they equal a fresh computation (BrushLibrary (m4) pins the setter and
// the stale-revision drop).
class BrushPreset
{
public:
    QString id;       // stable: "builtin/<category-slug>/<name-slug>" or "user/<uuid>"
    QString name;     // display name (renameable; not part of settingsHash)
    QString category; // "Sketching" | "Drawing" | "Inking" | "Painting"
                      // | "Artistic" | "Watercolor"  (never "Recent")
    bool builtin = false;
    // TRANSIENT (never serialised): the preset FILE carries an image larger
    // than Brush::kMaxCustomImageDim, capped in memory at load. The studio
    // tells the user before Done rewrites the file in capped form; the
    // model clears this after that write. Pre-cap files only.
    bool imagesCappedOnLoad = false;

    const ::Brush &brush() const { return m_brush; }
    // The ONE way a preset's brush changes.
    void setBrush(const ::Brush &brush)
    {
        m_brush = brush;
        ++m_brushRevision;
        m_keys = BrushKeys();
    }
    quint64 brushRevision() const { return m_brushRevision; }

    bool keysKnown() const { return m_keys.valid; }
    // Stored keys (valid only when keysKnown()). Debug: equal to a fresh
    // computation, every time they are read.
    const BrushKeys &keys() const
    {
        Q_ASSERT(!m_keys.valid || m_keys == computeKeys(m_brush));
        return m_keys;
    }
    // Keys computed from the brush at `forRevision`; dropped when the
    // brush has changed since (the worker's answer arrived too late).
    void storeKeys(const BrushKeys &keys, quint64 forRevision)
    {
        if (keys.valid && forRevision == m_brushRevision)
            m_keys = keys;
    }
    // The pure computation (BrushPresetCodec.cpp): hashes the brush, so
    // never on the UI thread outside a Debug assert.
    static BrushKeys computeKeys(const ::Brush &brush);

private:
    ::Brush m_brush;
    quint64 m_brushRevision = 0;
    BrushKeys m_keys;
};

} // namespace brushlib
