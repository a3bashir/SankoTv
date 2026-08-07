#pragma once

#include "BrushPreset.h"

#include <QByteArray>
#include <QVector>

namespace brushlib {

// Import extension point. The Import button walks the registry and hands the
// file's bytes to the first importer whose probe() accepts them, so a
// foreign-format importer plugs in by registration without the library
// changing shape. Two are registered: the NATIVE one (.sankobrush single
// presets and .sankobrushset bundles — lossless) and the Photoshop
// AbrImporter (.abr — lossy, and reports what it lost). Registration order
// is the probe order; no importer may claim bytes another format owns.
class BrushImporter
{
public:
    virtual ~BrushImporter() = default;
    virtual QString name() const = 0;
    virtual bool probe(const QByteArray &bytes) const = 0;
    virtual QVector<BrushPreset> import(const QByteArray &bytes) const = 0;

    // Lossy importers (foreign formats) additionally produce a per-brush
    // human-readable report of what was mapped, approximated, and dropped —
    // a brush that silently looks wrong is worse than one that says why.
    // The default forwards to import() with no report, so the native
    // importer (lossless by definition) needs nothing.
    virtual QVector<BrushPreset> importWithReport(const QByteArray &bytes,
                                                  QString *report) const
    {
        if (report)
            report->clear();
        return import(bytes);
    }

    static void registerImporter(BrushImporter *importer); // takes ownership
    static const QVector<BrushImporter *> &importers();
};

} // namespace brushlib
