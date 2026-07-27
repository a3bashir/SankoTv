#pragma once

#include "TiledImage.h"

#include <QImage>
#include <QMetaType>
#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>

enum class BrushCoherenceTrigger
{
    StrokeCommit,
    Thumbnail,
    CompositeCache,
    Save,
    UndoRedo,
    ExplicitDemand
};

QString brushCoherenceTriggerName(BrushCoherenceTrigger trigger);

class BrushCpuCoherenceProvider
{
public:
    virtual ~BrushCpuCoherenceProvider() = default;

    // Implementations backed primarily by GPU textures read the requested
    // tiles into cpuBackingStore with TiledImage::writeCoherent(). Calls are made at integration
    // boundaries only, never while stamps are being generated.
    virtual bool ensureCpuCoherent(TiledImage &cpuBackingStore,
                                   const QSet<QPoint> &tiles,
                                   BrushCoherenceTrigger trigger) = 0;
};

class CpuResidentBrushCoherenceProvider final : public BrushCpuCoherenceProvider
{
public:
    bool ensureCpuCoherent(TiledImage &, const QSet<QPoint> &,
                           BrushCoherenceTrigger) override { return true; }
};

struct BrushTilePatch
{
    QPoint coordinate;
    QRect layerRect;
    QImage before;
    QImage after;
};

struct BrushStrokeCommit
{
    quint64 strokeSeed = 0;
    QRect affectedRect;
    QSet<QPoint> dirtyTiles;
    QVector<BrushTilePatch> tilePatches;

    bool isEmpty() const { return tilePatches.isEmpty(); }
    bool isValid() const;

    static BrushStrokeCommit capture(const TiledImage &layerBefore,
                                     const QRect &affectedRect,
                                     const QImage &afterRegion,
                                     quint64 strokeSeed = 0);
    void applyBefore(TiledImage &layer) const;
    void applyAfter(TiledImage &layer) const;
    void discardPixelPayloads();

private:
    void apply(TiledImage &layer, bool useAfter) const;
};

Q_DECLARE_METATYPE(BrushCoherenceTrigger)
Q_DECLARE_METATYPE(BrushStrokeCommit)
