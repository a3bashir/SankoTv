#pragma once

#include "BrushPreset.h"

#include <QByteArray>
#include <QVector>

namespace brushlib {

// Import extension point. The Import button walks the registry and hands the
// file's bytes to the first importer whose probe() accepts them, so a
// third-party importer (.abr — see the phase-8 feasibility report) plugs in
// by registration without the library changing shape. This phase registers
// only the native importer (.sankobrush single presets and .sankobrushset
// bundles).
class BrushImporter
{
public:
    virtual ~BrushImporter() = default;
    virtual QString name() const = 0;
    virtual bool probe(const QByteArray &bytes) const = 0;
    virtual QVector<BrushPreset> import(const QByteArray &bytes) const = 0;

    static void registerImporter(BrushImporter *importer); // takes ownership
    static const QVector<BrushImporter *> &importers();
};

} // namespace brushlib
