#pragma once

#include "BrushPreset.h"

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QThread>
#include <QWaitCondition>

namespace brushlib {

// Worker-thread stroke-swatch renderer for the Brush Library rows
// (Figma 245:23, preview area 222x26).
//
// - Renders through SankoPaintHostAdapter::render(StrokeWork) — the SAME
//   path the undo replay and the codec round-trip test use. CPU only
//   (preferGpu=false), so previews never contend with the engine's serial
//   GPU commit worker; painting while previews generate shares only CPU
//   cores, and the worker runs at LowPriority so strokes win.
// - FIXED seed (4242) and a FIXED sample path: an S-wave with a pressure
//   ramp 0.15 -> 1.0 -> 0.55 (shows taper and the size/opacity/flow curves;
//   flat-curve pens stay uniform) and a tiltX ramp 0 -> 40 degrees along the
//   stroke (tilt-responsive brushes broaden toward the right end;
//   tilt-disabled brushes stay constant — one swatch shows both the untilted
//   and tilted character). Length ~200px exercises spacing, scatter, grain
//   and jitter; the wave's direction changes reveal angle-following tips.
// - Smudge-mode brushes render over three opaque colour bands (they MOVE
//   colour, and an empty background would be a no-op); everything else
//   renders over transparency (the UI composites the checkerboard).
// - Disk cache keyed by BrushPresetCodec::settingsHash: rename-stable,
//   edit-sensitive, and inherently invalidated by a codec wire-format bump
//   (the hash covers the serialised bytes INCLUDING the version header).
//   kSwatchRevision versions the renderer's own fixture (path/seed/size);
//   bump it when the swatch look changes.
class BrushPreviewRenderer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kSwatchW = 222;
    static constexpr int kSwatchH = 26;
    static constexpr quint32 kSwatchRevision = 1;

    // cacheRootOverride: tests point this at a scratch directory; empty uses
    // QStandardPaths::CacheLocation.
    explicit BrushPreviewRenderer(const QString &cacheRootOverride = QString(),
                                  QObject *parent = nullptr);
    ~BrushPreviewRenderer() override; // drains + joins the worker; no hang

    // Queue a render (deduplicated: a preset already queued or in flight is
    // not rendered twice). Emits previewReady(presetId, image) on the
    // caller's thread via a queued connection.
    void requestPreview(const QString &presetId, const ::Brush &brush);

    // Category switched: obsolete queued requests are dropped; the one
    // in-flight render finishes but its result is NOT emitted.
    void cancelAll();
    // Preset deleted while rendering: its result must never surface.
    void cancelPreset(const QString &presetId);

    QString cacheDir() const;

    // The pure, synchronous core — deterministic; the worker calls it and
    // the permanent brushlib test calls it directly.
    static QImage renderPreviewImage(const ::Brush &brush);

    // Test hook: the thread the last render actually ran on.
    QThread *lastRenderThread() const { return m_lastRenderThread; }

signals:
    void previewReady(const QString &presetId, const QImage &image);

private:
    struct Job
    {
        QString presetId;
        ::Brush brush;
        quint64 epoch = 0;
    };
    void workerLoop();
    QImage loadOrRender(const ::Brush &brush);
    static void prune(const QString &dir);

    QString m_cacheRoot;
    QThread *m_thread = nullptr; // QThread::create(workerLoop)
    QMutex m_mutex;
    QWaitCondition m_wake;
    QQueue<Job> m_queue;
    QSet<QString> m_pendingIds; // queued or in flight
    quint64 m_epoch = 0;        // bumped by cancelAll
    bool m_quit = false;
    QThread *m_lastRenderThread = nullptr;
};

} // namespace brushlib
