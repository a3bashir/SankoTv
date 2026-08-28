// SankoTV Brush Library test — PERMANENT, ships with the code.
//
// Promoted from the Phase 1 TEMP target after the project's own history
// argued for it: the Phase 4a pixel fixture lived only in a standalone
// harness and became permanently unverifiable, and a floating-toolbar seam
// was archived and revived three times, losing improvements each round.
// Everything this file guards is pinned HERE, in-tree.
//
// FIXTURE (all inputs pinned):
//   Stroke:   33 raw points on the diagonal (100,100) -> (860,440) at
//             960x540, pressure 1.0, seed 42 — the SankoPaintPixelLock path.
//   Preview:  BrushPreviewRenderer's fixed swatch fixture (S-wave, pressure
//             ramp 0.15->1.0->0.55, tiltX ramp 0->40 deg, seed 4242,
//             222x26 at 2x supersample) — pinned in BrushPreviewRenderer.
//   Smudge:   three colour bands (0xc03030 / 0x30c050 / 0x3050c0).
//
// ASSERTS (Debug and Release must both pass):
//   (a)  every category has >= 10 brushes; ids/names unique; roster printed
//   (b)  codec: bytes idempotent; save -> load -> render BYTE-identical for
//        all built-ins through SankoPaintHostAdapter::render(); preset files
//        round-trip; settingsHash is rename-stable and edit-sensitive, and
//        equals SHA-256 over the versioned wire bytes (so a wire-format bump
//        re-keys every preview automatically)
//   (k)  RGBA16 exactly as designed; no Sketching/Drawing/Inking brush trips
//        the colour stroke buffer
//   (t)  tilt: the five uniform/geometric brushes have tiltAffectsShape off;
//        the preview fixture's tilt ramp REACHES the engine (a tilt-enabled
//        brush renders differently from its tilt-disabled copy)
//   (p)  previews: deterministic (two renders byte-identical, all brushes;
//        combined SHA printed for cross-config comparison); non-empty;
//        smudge swatches differ from their background (they drag colour);
//        rendered OFF the UI thread; cache hit serves identical bytes and
//        survives a renderer restart; cancellation drops queued and
//        in-flight results; shutdown mid-generation neither hangs nor
//        crashes; perf numbers reported (informational, loose bounds)
//
// Run: build/<config>/SankoBrushLibraryTest.exe (exit code = failure count;
// report written to brushlib_test.txt in the working directory).
//
// RE-BASELINING THE COMBINED PREVIEW SHA — read before "fixing" a failure.
// The combined SHA printed by (p1) fingerprints all 62 preview renders. It
// changes LEGITIMATELY only when one of these changes on purpose:
//   * a roster recipe (BuiltinRoster.cpp) — e.g. the Phase 4 settings studio
//     editing built-in defaults,
//   * the preview fixture (BrushPreviewRenderer: sample path, seed, sizing,
//     background — bump kSwatchRevision when you do this),
//   * the engine's rendering itself (which SankoPaintPixelLock also catches).
// The procedure: (1) make the intended change; (2) run this test in BOTH
// Debug and Release; (3) confirm the two configs print the SAME new SHA —
// if they differ, you broke cross-config determinism, stop; (4) record the
// new SHA in the commit message of the change that moved it, with one line
// on why. The response to an UNEXPLAINED SHA change is to find the cause;
// deleting or loosening the determinism assertion is never the fix. (This
// project lost the Phase 4a baseline exactly once by keeping it outside the
// tree — that is why the SHA and this procedure live HERE.)

#include "BrushLibraryModel.h"
#include "SankoSettings.h"
#include "BrushPresetCodec.h"
#include "BrushPreviewRenderer.h"
#include "BuiltinRoster.h"
#include "SankoPaintHostAdapter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <memory>

#ifdef Q_OS_WIN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

using brushlib::BrushPreset;
using brushlib::BrushPresetCodec;
using brushlib::BrushPreviewRenderer;

namespace {

QString renderHash(const ::Brush &brush)
{
    ::Brush b = brush;
    StrokeBuilder sb(QSize(960, 540), b, false, 42, 0);
    std::unique_ptr<StrokeBuilder> sb2;
    if (b.dualBrushEnabled())
        sb2 = std::make_unique<StrokeBuilder>(QSize(960, 540),
                                              b.secondaryBrush(), false, 42,
                                              1);
    for (int i = 0; i <= 32; ++i) {
        StrokePoint p;
        p.position = QPointF(100 + i * (760.0 / 32), 100 + i * (340.0 / 32));
        p.pressure = 1.0;
        sb.addRawPoint(p);
        if (sb2)
            sb2->addRawPoint(p);
    }
    SankoPaintHostAdapter::StrokeWork w;
    w.layerKey = QStringLiteral("fixture");
    w.canvasSize = QSize(960, 540);
    w.brush = b;
    w.rawPoints = sb.rawPoints();
    w.primaryStamps = sb.stamps();
    if (sb2)
        w.secondaryStamps = sb2->stamps();
    w.affectedRect = sb.affectedRect().intersected(QRect(0, 0, 960, 540));
    QImage before(w.affectedRect.size(), QImage::Format_ARGB32);
    before.fill(Qt::transparent);
    if (b.smudgeActive()) {
        QPainter p(&before);
        const int h = before.height();
        p.fillRect(0, 0, before.width(), h / 3, QColor(0xc0, 0x30, 0x30));
        p.fillRect(0, h / 3, before.width(), h / 3,
                   QColor(0x30, 0xc0, 0x50));
        p.fillRect(0, 2 * h / 3, before.width(), h - 2 * (h / 3),
                   QColor(0x30, 0x50, 0xc0));
        p.end();
    }
    w.beforeRegion = before;
    w.seed = 42;
    w.preferGpu = false;
    StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush, before,
                                        w.affectedRect.topLeft());
    if (sb2)
        StrokeBuilder::resolveColorDynamics(w.secondaryStamps,
                                            w.brush.secondaryBrush(), before,
                                            w.affectedRect.topLeft());
    const auto result = SankoPaintHostAdapter::render(w);
    if (!result.succeeded)
        return QStringLiteral("RENDER-FAILED:") + result.error;
    const QImage n =
        result.afterRegion.convertToFormat(QImage::Format_ARGB32);
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(n.constBits()),
                           n.sizeInBytes()),
            QCryptographicHash::Sha256)
            .toHex());
}

QByteArray imageBytes(const QImage &img)
{
    const QImage n = img.convertToFormat(QImage::Format_ARGB32);
    return QByteArray(reinterpret_cast<const char *>(n.constBits()),
                      int(n.sizeInBytes()));
}

QString recipeSummary(const ::Brush &b)
{
    QString s = QStringLiteral("size=%1 hard=%2 op=%3 flow=%4 spac=%5")
                    .arg(b.size())
                    .arg(b.hardness(), 0, 'f', 2)
                    .arg(b.opacity(), 0, 'f', 2)
                    .arg(b.flow(), 0, 'f', 2)
                    .arg(b.spacing(), 0, 'f', 2);
    if (b.hasGrain())
        s += QStringLiteral(" grain(d=%1,%2)")
                 .arg(b.grainDepth(), 0, 'f', 2)
                 .arg(b.grainMode() == ::Brush::GrainMode::Rolling
                          ? QStringLiteral("roll")
                          : QStringLiteral("static"));
    if (b.scatterCount() > 1 || b.scatterAlong() > 0.0)
        s += QStringLiteral(" scatter(n=%1)").arg(b.scatterCount());
    if (b.sizeJitter() > 0 || b.angleJitter() > 0)
        s += QStringLiteral(" jitter");
    if (b.hasCustomShape())
        s += QStringLiteral(" customTip");
    if (b.toolMode() == ::Brush::ToolMode::Smudge)
        s += QStringLiteral(" smudge=%1").arg(b.smudgeStrength(), 0, 'f', 2);
    if (b.hueJitter() > 0 || b.saturationJitter() > 0
        || b.brightnessJitter() > 0)
        s += QStringLiteral(" colorJitter");
    if (b.grainAffectsColor())
        s += QStringLiteral(" colorGrain");
    if (b.dualBrushEnabled())
        s += QStringLiteral(" dual");
    if (!b.tiltAffectsShape())
        s += QStringLiteral(" noTilt");
    return s;
}

quint64 peakWorkingSetMb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize / (1024 * 1024);
#endif
    return 0;
}

quint64 workingSetMb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize / (1024 * 1024);
#endif
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    QFile out(QStringLiteral("brushlib_test.txt"));
    out.open(QIODevice::WriteOnly | QIODevice::Truncate);
    QTextStream ts(&out);
    int failures = 0;
    const char nl = 10;
    auto check = [&](const QString &label, bool ok,
                     const QString &d = QString()) {
        if (!ok)
            ++failures;
        ts << (ok ? "PASS " : "FAIL ") << label;
        if (!d.isEmpty())
            ts << "   " << d;
        ts << nl;
        out.flush();
        fprintf(ok ? stdout : stderr, "%s %s\n", ok ? "PASS" : "FAIL",
                qPrintable(label));
    };

    const QSet<QString> expectedRgba16{
        QStringLiteral("builtin/painting/blender"),
        QStringLiteral("builtin/painting/smudge-soft"),
        QStringLiteral("builtin/artistic/confetti"),
        QStringLiteral("builtin/artistic/chromatic"),
        QStringLiteral("builtin/artistic/sparkle"),
        QStringLiteral("builtin/watercolor/granulating-wash"),
        QStringLiteral("builtin/watercolor/wet-on-wet"),
        QStringLiteral("builtin/watercolor/bleed-edge"),
        QStringLiteral("builtin/watercolor/salt-texture"),
    };
    const QSet<QString> expectedNoTilt{
        QStringLiteral("builtin/inking/technical-pen"),
        QStringLiteral("builtin/sketching/mechanical-pencil"),
        QStringLiteral("builtin/inking/fine-liner"),
        QStringLiteral("builtin/inking/marker"),
        QStringLiteral("builtin/artistic/rounded-square"),
    };

    const QVector<BrushPreset> roster = brushlib::builtinRoster();

    // ---- (a) categories + roster report ---------------------------------
    ts << "== ROSTER ==" << nl;
    int total = 0;
    for (const QString &cat : brushlib::builtinCategories()) {
        int count = 0;
        ts << cat << ":" << nl;
        for (const BrushPreset &p : roster)
            if (p.category == cat) {
                ++count;
                ++total;
                ts << "  " << p.name << "  ["
                   << (p.brush.usesColorStrokeBuffer() ? "RGBA16" : "R16")
                   << "]  " << recipeSummary(p.brush) << nl;
            }
        check(QStringLiteral("(a) %1 has >= 10 brushes").arg(cat),
              count >= 10, QStringLiteral("count=%1").arg(count));
    }
    ts << "TOTAL brushes: " << total << nl << nl;
    {
        QSet<QString> ids, names;
        bool unique = true;
        for (const BrushPreset &p : roster) {
            if (ids.contains(p.id) || names.contains(p.category + p.name))
                unique = false;
            ids.insert(p.id);
            names.insert(p.category + p.name);
        }
        check(QStringLiteral("(a2) ids and per-category names unique"),
              unique);
    }

    // ---- (b) codec round-trip -------------------------------------------
    {
        bool bytesOk = true, renderOk = true, presetOk = true;
        QString firstBad;
        for (const BrushPreset &p : roster) {
            const QByteArray bytes = BrushPresetCodec::saveBrush(p.brush);
            ::Brush loaded;
            if (!BrushPresetCodec::loadBrush(bytes, loaded)
                || BrushPresetCodec::saveBrush(loaded) != bytes) {
                bytesOk = false;
                firstBad = p.name;
                break;
            }
            const QString h0 = renderHash(p.brush);
            if (h0 != renderHash(loaded)
                || h0.startsWith(QStringLiteral("RENDER-FAILED"))) {
                renderOk = false;
                firstBad = p.name;
                break;
            }
            BrushPreset back;
            if (!BrushPresetCodec::loadPreset(
                    BrushPresetCodec::savePreset(p), back)
                || back.id != p.id || back.name != p.name
                || back.category != p.category || back.builtin != p.builtin
                || BrushPresetCodec::saveBrush(back.brush) != bytes) {
                presetOk = false;
                firstBad = p.name;
                break;
            }
        }
        check(QStringLiteral("(b1) codec bytes idempotent for all %1")
                  .arg(roster.size()),
              bytesOk, firstBad);
        check(QStringLiteral(
                  "(b2) save->load->render BYTE-identical for all %1")
                  .arg(roster.size()),
              renderOk, firstBad);
        check(QStringLiteral("(b3) full preset file round-trips"), presetOk,
              firstBad);
    }
    // ---- (b5) the 5000 px cap survives the wire format -------------------
    // The engine clamp was 2048 (and the Size CTL bar's 200 was a fiction
    // on top of it); a .sankobrush carrying 5000 used to come back as 2048
    // through Brush::setSize. Both directions asserted, plus a control
    // proving the comparison sees a wrong size.
    {
        ::Brush big;
        big.setSize(5000);
        check(QStringLiteral("(b5) the ENGINE accepts size 5000"),
              big.size() == 5000,
              QStringLiteral("engine holds %1").arg(big.size()));
        ::Brush loaded;
        const bool decoded =
            BrushPresetCodec::loadBrush(BrushPresetCodec::saveBrush(big),
                                        loaded);
        check(QStringLiteral("(b5) size 5000 round-trips through "
                             ".sankobrush"),
              decoded && loaded.size() == 5000,
              QStringLiteral("came back %1").arg(loaded.size()));
        // Control: the comparison is not blind to size.
        loaded.setSize(4999);
        check(QStringLiteral("(b5) CONTROL: a wrong size IS detected"),
              loaded.size() != big.size());
    }
    // ---- (b7) erase semantics through the adapter: selection cap-once,
    // no double-erase at joints, preview invariance -----------------------
    // The classic eraser's carefully-built semantics - coverage accumulates
    // UNMASKED, the selection mask and opacity cap it ONCE, overlapping
    // segments never double-erase - are the engine's semantics by
    // construction (single UNORM16 accumulation + one masked lerp in
    // render()). Pinned here rather than trusted.
    {
        auto runErase = [](bool preview, qreal opacity,
                           const QImage &selectionMask, bool doubleBack) {
            SankoPaintHostAdapter adapter;
            adapter.brush() = ::Brush();
            adapter.brush().setEraseMode(true);
            adapter.brush().setSize(80);
            adapter.brush().setHardness(1.0);
            adapter.brush().setOpacity(opacity);
            QImage host(QSize(960, 540), QImage::Format_ARGB32);
            host.fill(QColor(60, 120, 180, 255)); // opaque: something to erase
            const QString key = QStringLiteral("b7");
            adapter.synchronizeLayer(key, host);
            StrokePoint sp;
            sp.position = QPointF(100, 270);
            sp.pressure = 1.0;
            adapter.beginStroke(key, host, sp, 4242, preview);
            auto feed = [&adapter](qreal fromX, qreal toX) {
                for (int i = 1; i <= 24; ++i) {
                    StrokePoint q;
                    q.position =
                        QPointF(fromX + (toX - fromX) * i / 24.0, 270);
                    q.pressure = 1.0;
                    q.timestamp = quint64(i * 8);
                    adapter.appendPoint(q);
                }
            };
            feed(100, 700);
            if (doubleBack)
                feed(700, 100); // the SAME pixels again, same stroke
            auto work = adapter.finishStrokeWork(false);
            work.selectionMask = selectionMask;
            return SankoPaintHostAdapter::render(work);
        };
        const auto alphaAt = [](const SankoPaintHostAdapter::StrokeResult &r,
                                int x, int y) {
            const QImage a =
                r.afterRegion.convertToFormat(QImage::Format_ARGB32);
            const QPoint local(x - r.affectedRect.x(),
                               y - r.affectedRect.y());
            return a.rect().contains(local) ? qAlpha(a.pixel(local)) : -1;
        };

        // Selection: left half masked OUT (0), right half IN (255), a soft
        // 50% band in the middle - the classic mask-caps-once shape.
        // The mask convention is LUMINANCE (render() converts to
        // Grayscale8): black = masked out, white = selected, gray = soft.
        QImage mask(QSize(960, 540), QImage::Format_ARGB32);
        mask.fill(Qt::black);
        {
            QPainter mp(&mask);
            mp.fillRect(400, 0, 80, 540, QColor(128, 128, 128));
            mp.fillRect(480, 0, 480, 540, QColor(255, 255, 255));
        }
        const auto masked = runErase(false, 1.0, mask, false);
        check(QStringLiteral("(b7) selection: fully-masked-out pixels are "
                             "UNTOUCHED (opaque)"),
              masked.succeeded && alphaAt(masked, 300, 270) == 255,
              QStringLiteral("alpha=%1").arg(alphaAt(masked, 300, 270)));
        check(QStringLiteral("(b7) selection: fully-selected pixels erase "
                             "to 0"),
              alphaAt(masked, 600, 270) == 0,
              QStringLiteral("alpha=%1").arg(alphaAt(masked, 600, 270)));
        const int soft = alphaAt(masked, 440, 270);
        check(QStringLiteral("(b7) selection: the soft 50% band erases "
                             "HALFWAY, capped once"),
              soft > 108 && soft < 148,
              QStringLiteral("alpha=%1 (expect ~128)").arg(soft));

        // No double-erase: the stroke crosses its own pixels twice at 50%
        // opacity; the coverage ceiling caps the whole stroke ONCE.
        const auto once = runErase(false, 0.5, QImage(), false);
        const auto twice = runErase(false, 0.5, QImage(), true);
        check(QStringLiteral("(b7) control: a 50% erase leaves ~50% alpha"),
              alphaAt(once, 400, 270) > 108 && alphaAt(once, 400, 270) < 148,
              QStringLiteral("alpha=%1").arg(alphaAt(once, 400, 270)));
        check(QStringLiteral("(b7) crossing the SAME pixels twice in one "
                             "stroke does NOT double-erase"),
              alphaAt(twice, 400, 270) == alphaAt(once, 400, 270),
              QStringLiteral("once=%1 twice=%2")
                  .arg(alphaAt(once, 400, 270))
                  .arg(alphaAt(twice, 400, 270)));

        // Preview invariance for ERASE strokes (extends b6 to the erase
        // composite): preview on vs off publishes identical bytes.
        const auto onE = runErase(true, 1.0, QImage(), false);
        const auto offE = runErase(false, 1.0, QImage(), false);
        const auto sha = [](const SankoPaintHostAdapter::StrokeResult &r) {
            const QImage n =
                r.afterRegion.convertToFormat(QImage::Format_ARGB32);
            return QCryptographicHash::hash(
                QByteArrayView(
                    reinterpret_cast<const char *>(n.constBits()),
                    n.sizeInBytes()),
                QCryptographicHash::Sha256);
        };
        check(QStringLiteral("(b7) ERASE published bytes IDENTICAL with "
                             "preview on vs off"),
              onE.succeeded && offE.succeeded && sha(onE) == sha(offE)
                  && onE.affectedRect == offE.affectedRect);
    }

    // ---- (b6) decimated live preview: exists, and cannot touch publishes -
    // Brushes over 256 px rasterize their in-flight preview in a SEPARATE
    // 1/k-scale builder (they previously drew BLIND over 512 - no preview
    // at all until the async publish). The safety case for that builder is
    // that nothing it rasterizes can reach published pixels; the closest
    // real check is INVARIANCE: the same stroke, preview on vs off, must
    // publish byte-identical output. A leak of preview state into the
    // stamp list, the affected rect or the composite breaks it.
    {
        struct StrokeOut {
            QByteArray afterSha;
            QRect affected;
            int previewScale = 0;
            int previewTilesAllocated = -1;
            bool succeeded = false;
        };
        auto runStroke = [](int size, bool preview, bool gpu) {
            SankoPaintHostAdapter adapter;
            adapter.brush() = ::Brush();
            adapter.brush().setSize(size);
            adapter.brush().setHardness(0.75);
            adapter.brush().setColor(QColor(200, 60, 30));
            QImage host(QSize(960, 540), QImage::Format_ARGB32);
            host.fill(Qt::transparent);
            const QString key = QStringLiteral("b6");
            adapter.synchronizeLayer(key, host);
            StrokePoint sp;
            sp.position = QPointF(96, 54);
            sp.pressure = 1.0;
            adapter.beginStroke(key, host, sp, 777, preview);
            for (int i = 1; i <= 24; ++i) {
                StrokePoint q;
                q.position = QPointF(96 + i * 20, 54 + i * 12);
                q.pressure = 1.0;
                q.timestamp = quint64(i * 8);
                adapter.appendPoint(q);
            }
            StrokeOut o;
            o.previewScale = adapter.previewScale();
            o.previewTilesAllocated = adapter.previewTiles()
                ? adapter.previewTiles()->allocatedTileCount() : -1;
            const auto work = adapter.finishStrokeWork(gpu);
            const auto res = SankoPaintHostAdapter::render(work);
            o.succeeded = res.succeeded;
            o.affected = res.affectedRect;
            o.afterSha = QCryptographicHash::hash(
                QByteArrayView(
                    reinterpret_cast<const char *>(
                        res.afterRegion.constBits()),
                    res.afterRegion.sizeInBytes()),
                QCryptographicHash::Sha256);
            return o;
        };
        const StrokeOut big = runStroke(2048, true, true);
        check(QStringLiteral("(b6) a 2048 px brush HAS a live preview "
                             "(the blind range is closed)"),
              big.succeeded && big.previewTilesAllocated > 0,
              QStringLiteral("%1 preview tiles")
                  .arg(big.previewTilesAllocated));
        check(QStringLiteral("(b6) ...decimated at 1/8 scale"),
              big.previewScale == 8,
              QStringLiteral("k=%1").arg(big.previewScale));
        const StrokeOut bigOff = runStroke(2048, false, true);
        check(QStringLiteral("(b6) CONTROL: preview off allocates no "
                             "preview tiles"),
              bigOff.succeeded && bigOff.previewTilesAllocated == 0,
              QStringLiteral("%1 tiles").arg(bigOff.previewTilesAllocated));
        // ("tiny", because <rpcndr.h> #defines "small" to char.)
        const StrokeOut tiny = runStroke(152, true, true);
        check(QStringLiteral("(b6) a 152 px brush keeps the full-res "
                             "preview path (k=1, tiles exist)"),
              tiny.succeeded && tiny.previewScale == 1
                  && tiny.previewTilesAllocated > 0);
        // THE INVARIANCE PIN, both regimes and both render paths.
        for (const int size : {152, 500, 2048}) {
            const StrokeOut on = runStroke(size, true, true);
            const StrokeOut off = runStroke(size, false, true);
            check(QStringLiteral("(b6) size %1: published bytes IDENTICAL "
                                 "with preview on vs off (GPU)")
                      .arg(size),
                  on.succeeded && off.succeeded
                      && on.afterSha == off.afterSha
                      && on.affected == off.affected);
        }
        {
            const StrokeOut on = runStroke(2048, true, false);
            const StrokeOut off = runStroke(2048, false, false);
            check(QStringLiteral("(b6) size 2048: published bytes IDENTICAL "
                                 "with preview on vs off (CPU path)"),
                  on.succeeded && off.succeeded
                      && on.afterSha == off.afterSha
                      && on.affected == off.affected);
        }
        // CONTROL: the byte comparison is not blind - different sizes
        // publish different bytes.
        check(QStringLiteral("(b6) CONTROL: 152 and 500 publish DIFFERENT "
                             "bytes"),
              runStroke(152, true, true).afterSha
                  != runStroke(500, true, true).afterSha);
    }
    {
        const BrushPreset &p = roster.first();
        const QByteArray h0 = BrushPresetCodec::settingsHash(p.brush);
        ::Brush edited = p.brush;
        edited.setSize(p.brush.size() + 5);
        const QByteArray wire = BrushPresetCodec::saveBrush(p.brush);
        check(QStringLiteral("(b4) settingsHash rename-stable, "
                             "edit-sensitive, covers the wire version"),
              BrushPresetCodec::settingsHash(p.brush) == h0
                  && BrushPresetCodec::settingsHash(edited) != h0
                  && h0 == QCryptographicHash::hash(
                         wire, QCryptographicHash::Sha256)
                  && wire.size() > 6);
    }

    // ---- (k) RGBA16 audit ------------------------------------------------
    {
        bool ok = true, pencilsOk = true;
        QString detail;
        int rgbaCount = 0;
        for (const BrushPreset &p : roster) {
            const bool rgba = p.brush.usesColorStrokeBuffer();
            if (rgba)
                ++rgbaCount;
            if (rgba != expectedRgba16.contains(p.id)) {
                ok = false;
                detail += p.id + QStringLiteral(" ");
            }
            if (rgba
                && (p.category == QStringLiteral("Sketching")
                    || p.category == QStringLiteral("Drawing")
                    || p.category == QStringLiteral("Inking")))
                pencilsOk = false;
        }
        check(QStringLiteral(
                  "(k) RGBA16 exactly as designed (%1 of %2 brushes)")
                  .arg(rgbaCount)
                  .arg(roster.size()),
              ok && rgbaCount == expectedRgba16.size(), detail);
        check(QStringLiteral("(k2) no pencil/pen/ink brush trips RGBA16"),
              pencilsOk, detail);
    }

    // ---- (t) tilt --------------------------------------------------------
    {
        bool flagsOk = true;
        QString detail;
        for (const BrushPreset &p : roster)
            if (p.brush.tiltAffectsShape()
                == expectedNoTilt.contains(p.id)) {
                flagsOk = false;
                detail += p.id + QStringLiteral(" ");
            }
        check(QStringLiteral(
                  "(t1) uniform/geometric brushes have tilt OFF (%1), "
                  "the rest ON")
                  .arg(expectedNoTilt.size()),
              flagsOk, detail);
        // The preview fixture's tilt ramp must actually reach the engine:
        // 6B Pencil (elongation 3.0) with tilt on vs off must differ.
        const BrushPreset *sixB = nullptr;
        for (const BrushPreset &p : roster)
            if (p.id == QStringLiteral("builtin/sketching/6b-pencil"))
                sixB = &p;
        bool tiltVisible = false;
        if (sixB) {
            ::Brush noTilt = sixB->brush;
            noTilt.setTiltAffectsShape(false);
            tiltVisible =
                imageBytes(BrushPreviewRenderer::renderPreviewImage(
                    sixB->brush))
                != imageBytes(
                    BrushPreviewRenderer::renderPreviewImage(noTilt));
        }
        check(QStringLiteral(
                  "(t2) the preview tilt ramp reaches the engine"),
              tiltVisible);
    }

    // ---- (p) previews ----------------------------------------------------
    QElapsedTimer timer;
    QVector<qint64> perPreviewNs;
    {
        bool deterministic = true, nonEmpty = true;
        QString firstBad;
        QCryptographicHash combined(QCryptographicHash::Sha256);
        for (const BrushPreset &p : roster) {
            timer.start();
            const QImage a = BrushPreviewRenderer::renderPreviewImage(
                p.brush);
            perPreviewNs.append(timer.nsecsElapsed());
            const QImage b = BrushPreviewRenderer::renderPreviewImage(
                p.brush);
            if (a.isNull()) {
                nonEmpty = false;
                firstBad = p.name;
            }
            if (imageBytes(a) != imageBytes(b)) {
                deterministic = false;
                firstBad = p.name;
            }
            combined.addData(imageBytes(a));
        }
        check(QStringLiteral(
                  "(p1) previews deterministic + non-empty for all %1")
                  .arg(roster.size()),
              deterministic && nonEmpty, firstBad);
        ts << "COMBINED PREVIEW SHA (compare across configs): "
           << combined.result().toHex() << nl;
    }
    {
        // Smudge swatches must visibly DRAG the band background.
        bool smudgeOk = true;
        QString detail;
        for (const BrushPreset &p : roster) {
            if (!p.brush.smudgeActive())
                continue;
            const QImage swatch =
                BrushPreviewRenderer::renderPreviewImage(p.brush);
            ::Brush inert = p.brush;
            inert.setSmudgeStrength(0.0); // bands unharmed = no drag at all
            int diff = 0;
            const QImage base =
                BrushPreviewRenderer::renderPreviewImage(inert);
            if (!swatch.isNull() && !base.isNull()
                && swatch.size() == base.size()) {
                for (int y = 0; y < swatch.height(); ++y) {
                    const QRgb *ra = reinterpret_cast<const QRgb *>(
                        swatch.constScanLine(y));
                    const QRgb *rb = reinterpret_cast<const QRgb *>(
                        base.constScanLine(y));
                    for (int x = 0; x < swatch.width(); ++x)
                        if (ra[x] != rb[x])
                            ++diff;
                }
            }
            if (diff < 200) { // a legible smear, not a couple of pixels
                smudgeOk = false;
                detail += p.name + QStringLiteral("(%1px) ").arg(diff);
            }
        }
        check(QStringLiteral(
                  "(p2) smudge swatches drag the colour bands legibly"),
              smudgeOk, detail);
    }

    // Worker-thread behaviour, cache, cancellation, shutdown, latency.
    const QString scratch =
        QDir::tempPath() + QStringLiteral("/sankotv_brushlib_test_cache");
    // Every settings read/write in app code goes through sankoSettings();
    // point the store at scratch so the family can NEVER touch the
    // user's real settings, driven or not.
    sankoSettingsSetOverrideForTest(scratch
                                    + QStringLiteral("/sanko_settings.ini"));
    QDir(scratch).removeRecursively();
    QVector<const BrushPreset *> inking;
    for (const BrushPreset &p : roster)
        if (p.category == QStringLiteral("Inking"))
            inking.append(&p);
    qint64 coldMs = 0, warmMs = 0;
    qint64 uiMaxGapNs = 0;
    {
        BrushPreviewRenderer renderer(scratch);
        int ready = 0;
        QThread *renderThread = nullptr;
        QObject::connect(&renderer, &BrushPreviewRenderer::previewReady,
                         &app, [&](const QString &, const QImage &) {
                             ++ready;
                             renderThread = renderer.lastRenderThread();
                         });
        timer.start();
        for (const BrushPreset *p : inking)
            renderer.requestPreview(p->id, p->brush);
        QElapsedTimer gap;
        gap.start();
        QElapsedTimer guard;
        guard.start();
        while (ready < inking.size() && guard.elapsed() < 60000) {
            const qint64 beforePoll = gap.nsecsElapsed();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            const qint64 slice = gap.nsecsElapsed() - beforePoll;
            uiMaxGapNs = qMax(uiMaxGapNs, slice);
        }
        coldMs = timer.elapsed();
        check(QStringLiteral("(p3) full category rendered off the UI "
                             "thread (%1 previews)")
                  .arg(inking.size()),
              ready == inking.size() && renderThread
                  && renderThread != app.thread(),
              QStringLiteral("ready=%1").arg(ready));
        // Warm: a NEW renderer over the same cache directory (restart
        // survival) must serve identical bytes, fast.
        BrushPreviewRenderer warm(scratch);
        int warmReady = 0;
        bool warmIdentical = true;
        QHash<QString, QByteArray> firstBytes;
        QObject::connect(
            &renderer, &BrushPreviewRenderer::previewReady, &app,
            [](const QString &, const QImage &) {});
        QObject::connect(&warm, &BrushPreviewRenderer::previewReady, &app,
                         [&](const QString &id, const QImage &img) {
                             ++warmReady;
                             firstBytes.insert(id, imageBytes(img));
                         });
        timer.start();
        for (const BrushPreset *p : inking)
            warm.requestPreview(p->id, p->brush);
        guard.restart();
        while (warmReady < inking.size() && guard.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        warmMs = timer.elapsed();
        for (const BrushPreset *p : inking)
            if (firstBytes.value(p->id)
                != imageBytes(
                    BrushPreviewRenderer::renderPreviewImage(p->brush)))
                warmIdentical = false;
        check(QStringLiteral("(p4) cache survives restart and serves "
                             "identical bytes"),
              warmReady == inking.size() && warmIdentical,
              QStringLiteral("cold=%1ms warm=%2ms").arg(coldMs).arg(warmMs));
        // Edit-sensitivity on disk: an edited brush renders to a NEW file.
        const int filesBefore =
            QDir(renderer.cacheDir())
                .entryList({QStringLiteral("*.png")}, QDir::Files)
                .size();
        ::Brush edited = inking.first()->brush;
        edited.setSize(edited.size() + 7);
        int editReady = 0;
        QObject::connect(&warm, &BrushPreviewRenderer::previewReady, &app,
                         [&](const QString &id, const QImage &) {
                             if (id == QStringLiteral("edited"))
                                 ++editReady;
                         });
        warm.requestPreview(QStringLiteral("edited"), edited);
        guard.restart();
        while (editReady < 1 && guard.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const int filesAfter =
            QDir(renderer.cacheDir())
                .entryList({QStringLiteral("*.png")}, QDir::Files)
                .size();
        check(QStringLiteral("(p5) settings edit renders a NEW cache entry "
                             "(rename by construction cannot: the key has "
                             "no name in it)"),
              editReady == 1 && filesAfter == filesBefore + 1,
              QStringLiteral("files %1 -> %2")
                  .arg(filesBefore)
                  .arg(filesAfter));
    }
    {
        // Cancellation: everything cancelled right after queueing -> zero
        // emissions (queued dropped, in-flight suppressed by epoch).
        BrushPreviewRenderer renderer(
            scratch + QStringLiteral("_cancel"));
        int emissions = 0;
        QObject::connect(&renderer, &BrushPreviewRenderer::previewReady,
                         &app,
                         [&](const QString &, const QImage &) {
                             ++emissions;
                         });
        for (const BrushPreset *p : inking)
            renderer.requestPreview(p->id, p->brush);
        renderer.cancelAll();
        QElapsedTimer guard;
        guard.start();
        while (guard.elapsed() < 1500)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        check(QStringLiteral(
                  "(p6) cancelAll drops queued and in-flight results"),
              emissions == 0,
              QStringLiteral("emissions=%1").arg(emissions));
    }
    {
        // Shutdown mid-generation: queue a category and destroy the
        // renderer immediately. Reaching the next check IS the assert.
        auto *renderer = new BrushPreviewRenderer(
            scratch + QStringLiteral("_shutdown"));
        for (const BrushPreset *p : inking)
            renderer->requestPreview(p->id, p->brush);
        delete renderer;
        check(QStringLiteral(
                  "(p7) shutdown mid-generation: no hang, no crash"),
              true);
    }

    // ---- numbers ---------------------------------------------------------
    {
        std::sort(perPreviewNs.begin(), perPreviewNs.end());
        const qint64 worst = perPreviewNs.isEmpty() ? 0 : perPreviewNs.last();
        const qint64 median = perPreviewNs.isEmpty()
            ? 0
            : perPreviewNs.at(perPreviewNs.size() / 2);
        qint64 cacheBytes = 0;
        const QDir cd(scratch + QStringLiteral("/r%1")
                                    .arg(BrushPreviewRenderer::kSwatchRevision));
        for (const QFileInfo &fi :
             cd.entryInfoList({QStringLiteral("*.png")}, QDir::Files))
            cacheBytes += fi.size();
        qint64 sumNs = 0;
        for (const qint64 v : perPreviewNs)
            sumNs += v;
        ts << nl << "== NUMBERS ==" << nl;
        ts << "cold generation, ALL " << perPreviewNs.size()
           << " built-ins (serial, worst case): " << sumNs / 1.0e6 << " ms"
           << nl;
        ts << "per-preview render: median "
           << median / 1.0e6 << " ms, worst " << worst / 1.0e6 << " ms"
           << nl;
        ts << "full category (" << inking.size()
           << " previews): cold " << coldMs << " ms, warm (disk cache) "
           << warmMs << " ms" << nl;
        ts << "UI-thread max event-loop gap while rendering: "
           << uiMaxGapNs / 1.0e6 << " ms" << nl;
        ts << "cache size for one category: " << cacheBytes / 1024
           << " KB (~" << (inking.isEmpty() ? 0
                                            : cacheBytes / inking.size())
           << " B/preview)" << nl;
        ts << "memory: working set " << workingSetMb() << " MB, peak "
           << peakWorkingSetMb() << " MB" << nl;
        check(QStringLiteral(
                  "(p8) UI-thread event-loop gap stays sane (< 50 ms)"),
              uiMaxGapNs < 50 * 1000 * 1000,
              QStringLiteral("%1 ms").arg(uiMaxGapNs / 1.0e6));
    }

    ts << nl << "FAILURES: " << failures << nl;
    out.close();
    return failures;
}
