#pragma once

#include "StoryboardModel.h"

#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector>

// File-level project serialization, extracted from MainWindow with the
// resolution epic. Two reasons, both structural:
//  1. The pixels-win reconciliation (manifest vs artwork) is CORRECTNESS
//     logic, not UI — it belongs where it can be tested.
//  2. The verification seam must drive real save/load without constructing
//     MainWindow, whose page teardown persists dock/toolbar state into the
//     REAL registry (forbidden during verification).
// MainWindow keeps everything user-facing: file dialogs, the mismatch
// dialog, page updates, recents. This owns bytes, JSON, and sibling PNGs.
namespace ProjectIO {

struct SaveData
{
    QString projectName;
    int fps = 24;
    QSize canvasSize; // the project's real resolution (post-reconcile)
    QVector<Scene *> scenes; // not owned
    QVector<ConsistencyEntry> consistency;
    QString audioPath;
    QJsonObject perspective;
};

// Build the project's JSON root AND write the sibling PNGs (panel flattens
// + per-layer images + consistency thumbnails) into `folder`. The flatten
// PNGs follow flattenedPixmap(), which is panel-sized — so what lands on
// disk always matches the manifest beside it.
QJsonObject projectToJson(const SaveData &data, const QString &folder);

struct LoadedProject
{
    QString projectName; // may be empty: caller falls back to the filename
    int fps = 24;
    QSize manifestSize;  // as stored (invalid/empty when keys absent)
    QSize pixelSize;     // the artwork's REAL size — the authority
    bool mismatch = false; // manifest present, valid, and != pixelSize
    QVector<Scene *> scenes; // caller takes ownership
    QVector<ConsistencyEntry> consistency;
    QString audioPath;
    QJsonObject perspective;
};

// Rebuild the model from a parsed root + its folder. Applies every
// migration (legacy single-PNG panels, legacy shared layers, background
// insertion, take folding) and the PIXELS-WIN reconciliation: pixelSize is
// what the project must open at; mismatch says whether the manifest lied
// (the caller shows canvasMismatchDialogText and the next save corrects
// the file). Artwork is never rescaled, cropped, or discarded.
LoadedProject projectFromJson(const QJsonObject &root, const QString &folder);

} // namespace ProjectIO
