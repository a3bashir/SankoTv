#pragma once

#include "BrushImporter.h"

namespace brushlib {

// Photoshop .abr brush importer. LOSSY BY DESIGN and honest about it:
// Photoshop's brush model does not map cleanly onto Sanko Paint's, so the
// goal is to give an artist their tip shapes and basic behaviour, never to
// reproduce Photoshop. Every import produces a per-brush report of what was
// mapped, approximated, and dropped (importWithReport). Format notes, the
// full mapping table, and the defensive-parsing contract live in the cpp.
class AbrImporter : public BrushImporter
{
public:
    QString name() const override;
    bool probe(const QByteArray &bytes) const override;
    QVector<BrushPreset> import(const QByteArray &bytes) const override;
    QVector<BrushPreset> importWithReport(const QByteArray &bytes,
                                          QString *report) const override;

    // The content-derived id ("user/abr-<sha>") an abr preset with this
    // name and brush WOULD carry under the current mapping generation.
    // A stored preset whose id matches its recomputation is CURRENT — the
    // library's upgrade-on-reimport path must never claim it; a mismatch
    // marks an older generation (or a rename), which is what the upgrade
    // exists for.
    static QString contentId(const BrushPreset &preset);
};

} // namespace brushlib
