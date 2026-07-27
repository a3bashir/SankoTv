#include "BrushEngineIntegration.h"

#include <QPainter>

QString brushCoherenceTriggerName(BrushCoherenceTrigger trigger)
{
    switch (trigger) {
    case BrushCoherenceTrigger::StrokeCommit: return QStringLiteral("stroke-commit");
    case BrushCoherenceTrigger::Thumbnail: return QStringLiteral("thumbnail");
    case BrushCoherenceTrigger::CompositeCache: return QStringLiteral("composite-cache");
    case BrushCoherenceTrigger::Save: return QStringLiteral("save");
    case BrushCoherenceTrigger::UndoRedo: return QStringLiteral("undo-redo");
    case BrushCoherenceTrigger::ExplicitDemand: return QStringLiteral("explicit-demand");
    }
    return QStringLiteral("unknown");
}

bool BrushStrokeCommit::isValid() const
{
    if (affectedRect.isEmpty() != tilePatches.isEmpty())
        return false;
    QSet<QPoint> coordinates;
    QRect united;
    for (const BrushTilePatch &patch : tilePatches) {
        if (patch.layerRect.isEmpty() || patch.before.size() != patch.layerRect.size()
            || patch.after.size() != patch.layerRect.size()
            || coordinates.contains(patch.coordinate)) {
            return false;
        }
        coordinates.insert(patch.coordinate);
        united = united.united(patch.layerRect);
    }
    return coordinates == dirtyTiles && united.contains(affectedRect);
}

BrushStrokeCommit BrushStrokeCommit::capture(const TiledImage &layerBefore,
                                              const QRect &requestedRect,
                                              const QImage &afterRegion,
                                              quint64 strokeSeed)
{
    BrushStrokeCommit commit;
    commit.strokeSeed = strokeSeed;
    const QRect region = requestedRect.intersected(layerBefore.rect());
    if (region.isEmpty() || afterRegion.size() != requestedRect.size())
        return commit;

    for (const QPoint &coordinate : TiledImage::tilesForRect(region)) {
        const QRect tileRect = TiledImage::tileLayerRect(coordinate).intersected(layerBefore.rect());
        QImage before = layerBefore.read(tileRect).convertToFormat(QImage::Format_ARGB32);
        QImage after = before.copy();
        const QRect intersection = region.intersected(tileRect);
        {
            QPainter painter(&after);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(intersection.topLeft() - tileRect.topLeft(), afterRegion,
                              intersection.translated(-requestedRect.topLeft()));
        }
        if (before == after)
            continue;
        int minimumX = before.width();
        int minimumY = before.height();
        int maximumX = -1;
        int maximumY = -1;
        for (int y = 0; y < before.height(); ++y) {
            const QRgb *beforeRow = reinterpret_cast<const QRgb *>(before.constScanLine(y));
            const QRgb *afterRow = reinterpret_cast<const QRgb *>(after.constScanLine(y));
            for (int x = 0; x < before.width(); ++x) {
                if (beforeRow[x] == afterRow[x])
                    continue;
                minimumX = qMin(minimumX, x);
                minimumY = qMin(minimumY, y);
                maximumX = qMax(maximumX, x);
                maximumY = qMax(maximumY, y);
            }
        }
        if (maximumX < minimumX || maximumY < minimumY)
            continue;
        const QRect changedLocal(QPoint(minimumX, minimumY),
                                 QPoint(maximumX, maximumY));
        const QRect changedLayer = changedLocal.translated(tileRect.topLeft());
        commit.dirtyTiles.insert(coordinate);
        commit.tilePatches.append({coordinate, changedLayer,
                                   before.copy(changedLocal), after.copy(changedLocal)});
        commit.affectedRect = commit.affectedRect.united(changedLayer);
    }
    return commit;
}

void BrushStrokeCommit::applyBefore(TiledImage &layer) const
{
    apply(layer, false);
}

void BrushStrokeCommit::applyAfter(TiledImage &layer) const
{
    apply(layer, true);
}

void BrushStrokeCommit::discardPixelPayloads()
{
    for (BrushTilePatch &patch : tilePatches) {
        patch.before = QImage();
        patch.after = QImage();
    }
}

void BrushStrokeCommit::apply(TiledImage &layer, bool useAfter) const
{
    for (const BrushTilePatch &patch : tilePatches)
        layer.write(patch.layerRect, useAfter ? patch.after : patch.before);
}
