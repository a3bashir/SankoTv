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
};

} // namespace brushlib
