#pragma once

#include "StoryboardModel.h"

#include <QPoint>
#include <QSize>
#include <QString>
#include <QVector>

// CANVAS-ONLY project resize (V1): every panel's layers are rebuilt at the
// new size with the existing artwork re-anchored at the CENTRE. Nothing is
// scaled, resampled, or interpolated — expanding adds margin, contracting
// crops symmetrically, and the surviving pixels are byte-identical.
//
// STAGING IS PER PANEL, NOT PER PROJECT. Staging the whole project before
// swapping would hold the old and new sets at once: HANDOFF records a
// 50-panel 4K scene at ~5 GB of layer images and a probe that OOM-CRASHED
// at that load, so a 2x peak cannot ship. Each panel instead stages its own
// layers, and swaps only when all of them allocated; the old images are
// released as the swap completes, so the peak is the larger of the two full
// sets plus ONE panel.
//
// Per-panel staging cannot make the project-wide operation infallible on
// its own, so atomicity is bought two other ways: plan() refuses UP FRONT
// when the memory is not there (a refusal before any mutation is perfectly
// atomic), and the caller runs behind a save prompt, so the file on disk is
// the rollback if a panel fails anyway. A partial resize is never silent —
// the loader's panel census reports it and names the panels.
namespace ProjectResize {

// Where the old artwork's top-left lands on the new canvas. Integer
// division puts the odd pixel of an odd difference on the right/bottom;
// deterministic, and asserted as such in the gate.
inline QPoint centreOffset(const QSize &oldSize, const QSize &newSize)
{
    return QPoint((newSize.width() - oldSize.width()) / 2,
                  (newSize.height() - oldSize.height()) / 2);
}

struct Plan
{
    QSize oldSize;
    QSize newSize;
    QPoint offset;
    bool crops = false; // either dimension shrinks: artwork will be lost
    int panelCount = 0;
    int layerCount = 0; // non-group layers that get rebuilt

    // Memory arithmetic, all in bytes.
    qint64 currentBytes = 0;      // layer images held now
    qint64 resizedBytes = 0;      // layer images afterwards
    qint64 largestPanelBytes = 0; // the biggest single panel, at the NEW size
    qint64 additionalBytes = 0;   // peak ADDITIONAL requirement over what is held
    qint64 requiredBytes = 0;     // additionalBytes with the safety factor applied
    qint64 availableBytes = 0;    // physical memory available right now
    bool memoryKnown = false;     // false = no reliable query on this platform
    bool fits = false;
    QString refusal; // empty when fits; names what is needed and available
};

// Measure the project and decide whether the resize can be attempted. Never
// mutates anything.
Plan plan(const QVector<Scene *> &scenes, const QSize &oldSize,
          const QSize &newSize);

// Stage one panel's layers at the new size and swap them in. Returns false
// WITHOUT touching the panel if any allocation failed, so a caller that
// stops here leaves that panel exactly as it was.
bool resizePanel(Panel *panel, const QSize &newSize, const QPoint &offset);

struct Outcome
{
    bool ok = true;
    int panelsResized = 0;
    int failedSceneNumber = 0; // populated when ok == false
    int failedPanelIndex = 0;  // 1-based, as displayed
};

// Every panel, in order. Stops at the first failure and reports where.
Outcome apply(const QVector<Scene *> &scenes, const QSize &newSize,
              const QPoint &offset);

// Human-readable sizes for the refusal and the confirm dialog.
QString formatBytes(qint64 bytes);

} // namespace ProjectResize
