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

// One panel that does not match the project's chosen size, located for the
// user: "Scene 2, panel 7" rather than a bare count.
struct OffSizePanel
{
    int sceneNumber = 0; // as displayed
    int panelIndex = 0;  // 1-based within its scene, as displayed
    QSize size;
};

struct LoadedProject
{
    QString projectName; // may be empty: caller falls back to the filename
    int fps = 24;
    QSize manifestSize;  // as stored (invalid/empty when keys absent)
    QSize pixelSize;     // the artwork's REAL size — the authority
    bool mismatch = false; // manifest present, valid, and != pixelSize
    // PANEL-vs-PANEL disagreement, a different axis from `mismatch` (which
    // is manifest-vs-artwork). A project whose panels are not all the same
    // size used to load silently at whichever size happened to come first;
    // now the disagreement is counted, located and reported. Empty and
    // false for every uniform project.
    bool mixedSizes = false;
    int majorityPanelCount = 0;        // panels at pixelSize
    QVector<OffSizePanel> offSizePanels; // every panel that is not
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
//
// When the PANELS THEMSELVES disagree, pixelSize is the MAJORITY size
// (ties to the first encountered) rather than whichever panel came first:
// an odd panel at position zero would otherwise become the project size
// and every new panel would be created at it, spreading the defect. For a
// uniform project the majority IS the first, so this path cannot move.
// The disagreement is reported through mixedSizes/offSizePanels — never
// repaired, because repairing means altering artwork.
LoadedProject projectFromJson(const QJsonObject &root, const QString &folder);

// The message for a project whose PANELS disagree with each other. The
// plain mismatch text (canvasMismatchDialogText) must never be shown for
// one of these: it says "its artwork is W x H", which is false when only
// some panels are, and it promises a correction that would otherwise be
// recording an accident of ordering. Everything below is true of a mixed
// project — it names the majority, locates every dissenter, states that
// nothing is altered, and says exactly what the next save does write.
inline QString mixedCanvasSizesDialogText(const QSize &manifest,
                                          const QSize &chosen,
                                          int majorityCount,
                                          const QVector<OffSizePanel> &offSize)
{
    QString text = QStringLiteral(
                       "This project's panels are not all the same size.\n\n"
                       "%1 panel%2 %3 %4 \xC3\x97 %5, and %6:\n")
                       .arg(majorityCount)
                       .arg(majorityCount == 1 ? "" : "s")
                       .arg(majorityCount == 1 ? "is" : "are")
                       .arg(chosen.width())
                       .arg(chosen.height())
                       .arg(offSize.size() == 1
                                ? QStringLiteral("one is not")
                                : QStringLiteral("%1 are not").arg(offSize.size()));
    const int kList = 8; // enough to act on; a full list helps nobody
    for (int i = 0; i < offSize.size() && i < kList; ++i)
        text += QStringLiteral("    Scene %1, panel %2 - %3 \xC3\x97 %4\n")
                    .arg(offSize.at(i).sceneNumber)
                    .arg(offSize.at(i).panelIndex)
                    .arg(offSize.at(i).size.width())
                    .arg(offSize.at(i).size.height());
    if (offSize.size() > kList)
        text += QStringLiteral("    ... and %1 more\n")
                    .arg(offSize.size() - kList);
    text += QStringLiteral(
                "\nOpening at %1 \xC3\x97 %2, the size most panels already "
                "are. No artwork is modified: the panels above keep their "
                "own pixels exactly as they are, and new panels are created "
                "at %1 \xC3\x97 %2.")
                .arg(chosen.width())
                .arg(chosen.height());
    if (manifest.isValid() && !manifest.isEmpty() && manifest != chosen)
        text += QStringLiteral(
                    "\n\nThe project file separately records %1 \xC3\x97 %2; "
                    "the next save will record %3 \xC3\x97 %4 instead. That "
                    "corrects the file's own record only — it does not "
                    "change any panel.")
                    .arg(manifest.width())
                    .arg(manifest.height())
                    .arg(chosen.width())
                    .arg(chosen.height());
    return text;
}

} // namespace ProjectIO
