#include "ProjectResize.h"

#include <QPainter>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ProjectResize {

namespace {

constexpr qint64 kMiB = 1024LL * 1024LL;

// SAFETY FACTOR. The whole atomicity argument rests on this check, and a
// precheck that passes and then OOMs is worse than none — the user was told
// it would fit. So the requirement is DOUBLED and a fixed reserve is held
// back on top:
//   x2  covers what the resize triggers but does not itself allocate — the
//       below/above composite caches (two canvas-sized images), a flatten
//       and a thumbnail per panel the strip rebuilds, the engine's own
//       surfaces — plus allocator fragmentation at these sizes, where a
//       33 MB contiguous block is not the same ask as 33 MB of free pages.
//   512 MB reserve keeps the machine and the rest of the app alive rather
//       than winning the check and losing the system to swap, and absorbs
//       whatever another process takes between the check and the work.
// Both numbers are CHOSEN, not measured: they are deliberately conservative
// because the failure they prevent is a crash mid-resize.
constexpr qint64 kSafetyFactor = 2;
constexpr qint64 kReserveBytes = 512 * kMiB;

qint64 imageBytes(const QSize &size)
{
    return qint64(size.width()) * qint64(size.height()) * 4; // ARGB32
}

// Available PHYSICAL memory. Deliberately ignores the pagefile: winning the
// check by counting swap would trade a crash for thrashing.
bool availablePhysicalBytes(qint64 *out)
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        *out = qint64(status.ullAvailPhys);
        return true;
    }
#endif
    Q_UNUSED(out);
    return false;
}

} // namespace

QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024LL * kMiB)
        return QStringLiteral("%1 GB").arg(double(bytes) / double(1024LL * kMiB),
                                           0, 'f', 1);
    return QStringLiteral("%1 MB").arg(double(bytes) / double(kMiB), 0, 'f', 0);
}

Plan plan(const QVector<Scene *> &scenes, const QSize &oldSize,
          const QSize &newSize)
{
    Plan p;
    p.oldSize = oldSize;
    p.newSize = newSize;
    p.offset = centreOffset(oldSize, newSize);
    p.crops = newSize.width() < oldSize.width()
        || newSize.height() < oldSize.height();

    const qint64 perLayerNew = imageBytes(newSize);
    for (const Scene *scene : scenes) {
        if (!scene)
            continue;
        for (const Panel *panel : scene->panels) {
            if (!panel)
                continue;
            ++p.panelCount;
            qint64 panelNew = 0;
            for (const Layer &layer : panel->layers) {
                if (isGroupLayer(layer))
                    continue; // a folder row: null image, never allocated
                ++p.layerCount;
                // Measure what is actually held rather than assuming every
                // layer is oldSize — a project can arrive internally
                // inconsistent, and the resize is what makes it uniform.
                if (!layer.image.isNull())
                    p.currentBytes += imageBytes(layer.image.size());
                p.resizedBytes += perLayerNew;
                panelNew += perLayerNew;
            }
            p.largestPanelBytes = qMax(p.largestPanelBytes, panelNew);
        }
    }

    // PEAK ADDITIONAL memory. Old images are released as each panel swaps,
    // so the run never holds both full sets: it grows (or shrinks) toward
    // the new total while staging one panel on top.
    p.additionalBytes =
        qMax<qint64>(0, p.resizedBytes - p.currentBytes) + p.largestPanelBytes;
    p.requiredBytes = p.additionalBytes * kSafetyFactor + kReserveBytes;
    p.memoryKnown = availablePhysicalBytes(&p.availableBytes);

    if (!newSize.isValid() || newSize.isEmpty()) {
        p.refusal = QStringLiteral("The new canvas size is not valid.");
    } else if (!p.memoryKnown) {
        // No reliable query here. Rather than guess, allow only what is
        // small enough to be safe on any machine that runs this at all.
        p.fits = p.additionalBytes <= 512 * kMiB;
        if (!p.fits)
            p.refusal = QStringLiteral(
                            "Resizing this project needs about %1 of memory, "
                            "and the available memory cannot be measured on "
                            "this system. Resize is limited to %2 here.")
                            .arg(formatBytes(p.additionalBytes),
                                 formatBytes(512 * kMiB));
    } else if (p.requiredBytes > p.availableBytes) {
        p.refusal =
            QStringLiteral(
                "Resizing this project to %1 \xC3\x97 %2 needs about %3 of "
                "memory (%4 for the images themselves, doubled to cover the "
                "thumbnails and caches the resize rebuilds, plus a %5 "
                "reserve). Only %6 is available right now.\n\n"
                "Nothing has been changed. Closing other applications, or "
                "resizing a smaller project, will make room.")
                .arg(newSize.width())
                .arg(newSize.height())
                .arg(formatBytes(p.requiredBytes),
                     formatBytes(p.additionalBytes),
                     formatBytes(kReserveBytes),
                     formatBytes(p.availableBytes));
    } else {
        p.fits = true;
    }
    return p;
}

bool resizePanel(Panel *panel, const QSize &newSize, const QPoint &offset)
{
    if (!panel || !newSize.isValid() || newSize.isEmpty())
        return false;

    // STAGE. The vector copy shares the existing images (no pixels copied
    // yet); each entry's image is then replaced by a freshly allocated one.
    // The panel itself is not touched until every allocation has succeeded.
    QVector<Layer> staged = panel->layers;
    for (Layer &layer : staged) {
        if (isGroupLayer(layer)) {
            layer.image = QImage(); // folders stay imageless
            continue;
        }
        QImage fresh(newSize, QImage::Format_ARGB32_Premultiplied);
        if (fresh.isNull())
            return false; // out of memory: the panel is left exactly as it was
        // Background layers are WHITE paper, everything else transparent —
        // invisible in the flatten (which composites onto white anyway) and
        // very visible on export or when the background is toggled.
        fresh.fill(layer.type == QLatin1String("background") ? Qt::white
                                                             : Qt::transparent);
        if (!layer.image.isNull()) {
            QPainter painter(&fresh);
            // 1:1 blit at the centre offset: no scaling, no smoothing, no
            // resampling. Qt clips whatever falls outside, which is exactly
            // the symmetric crop a contraction should produce.
            painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
            painter.drawImage(offset, layer.image);
        }
        layer.image = fresh;
    }

    // SWAP. A vector move: no allocation, no I/O, nothing that can fail.
    panel->layers = std::move(staged);
    return true;
}

Outcome apply(const QVector<Scene *> &scenes, const QSize &newSize,
              const QPoint &offset)
{
    Outcome outcome;
    for (Scene *scene : scenes) {
        if (!scene)
            continue;
        for (int i = 0; i < scene->panels.size(); ++i) {
            if (resizePanel(scene->panels.at(i), newSize, offset)) {
                ++outcome.panelsResized;
                continue;
            }
            outcome.ok = false;
            outcome.failedSceneNumber = scene->number;
            outcome.failedPanelIndex = i + 1;
            return outcome;
        }
    }
    return outcome;
}

} // namespace ProjectResize
