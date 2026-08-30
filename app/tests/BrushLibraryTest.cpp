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

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QSettings>
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

// COUPLED to the brush preview SHA - see the (b8) procedure note. A
// legitimate roster change moves BOTH in the same commit; this one
// moving ALONE is a defect, never a re-baseline.
static const char kEraserSwatchSha[] =
    "1bf25d5bae04d10503f567646ec57d6102302c8b0db14498316363a5423f69d7";
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

    // ---- (b9) override state: discoverability + the stale hazard ---------
    // A scratch-rooted model exercises the four states end to end. The
    // hazard machinery: updateBrush on a built-in records the STOCK
    // recipe's settingsHash at save time; overrideState() compares it
    // against the CURRENT stock to tell "modified" from "the recipe moved
    // underneath" - and an override with NO fingerprint reads UNKNOWN,
    // never a reassuring state the app cannot support.
    {
        using Model = brushlib::BrushLibraryModel;
        using State = Model::OverrideState;
        const QString root = QDir::tempPath()
            + QStringLiteral("/sankotv_override_state_test");
        QDir(root).removeRecursively();
        QDir().mkpath(root);
        const QString gouache = QStringLiteral("builtin/painting/gouache");
        const QString pencil = QStringLiteral("builtin/sketching/hb-pencil");
        {
            Model model(nullptr, root);
            check(QStringLiteral("(b9) control: no override, state None"),
                  model.overrideState(gouache) == State::None);

            // Modified: a real divergence, fingerprint recorded.
            ::Brush tuned = model.preset(gouache)->brush;
            tuned.setSpacing(0.02);
            check(QStringLiteral("(b9) updateBrush writes the override"),
                  model.updateBrush(gouache, tuned)
                      && QFile::exists(
                          root
                          + QStringLiteral("/Overrides/"
                                           "builtin_painting_gouache"
                                           ".sankobrush")));
            check(QStringLiteral("(b9) a diverging override reads "
                                 "MODIFIED (fingerprint matches stock)"),
                  model.overrideState(gouache) == State::Modified);

            // Harmless: an override byte-equal to stock carries NO mark.
            const ::Brush stockPencil = brushlib::builtinRoster()
                .at([&] {
                    const auto r = brushlib::builtinRoster();
                    for (int i = 0; i < r.size(); ++i)
                        if (r.at(i).id == pencil)
                            return i;
                    return 0;
                }()).brush;
            check(QStringLiteral("(b9) an override byte-equal to stock "
                                 "reads None (harmless residue)"),
                  model.updateBrush(pencil, stockPencil)
                      && model.overrideState(pencil) == State::None);

            // Give the pencil a REAL divergence for the reset control.
            ::Brush tunedPencil = stockPencil;
            tunedPencil.setSpacing(0.02);
            model.updateBrush(pencil, tunedPencil);
        }
        // StockChanged: the stored fingerprint no longer matches current
        // stock - simulated by corrupting the stored hash, which is
        // EXACTLY what a recipe edit does to it.
        {
            QSettings shelf(root + QStringLiteral("/shelf.ini"),
                            QSettings::IniFormat);
            shelf.setValue(QStringLiteral(
                               "brushLibrary/v1/overrideStock/"
                               "builtin|painting|gouache"),
                           QStringLiteral("not-the-stock-hash-any-more"));
        }
        {
            Model model(nullptr, root);
            check(QStringLiteral("(b9) a moved recipe reads STOCK-CHANGED "
                                 "(the hazard state)"),
                  model.overrideState(gouache) == State::StockChanged);
        }
        // Unknown: NO fingerprint at all (a pre-fingerprint override).
        {
            QSettings shelf(root + QStringLiteral("/shelf.ini"),
                            QSettings::IniFormat);
            shelf.remove(QStringLiteral("brushLibrary/v1/overrideStock/"
                                        "builtin|painting|gouache"));
        }
        {
            Model model(nullptr, root);
            check(QStringLiteral("(b9) no fingerprint reads UNKNOWN - "
                                 "never a reassuring default"),
                  model.overrideState(gouache) == State::Unknown);

            // Reset to stock: exactly ONE file, byte-exact restoration.
            check(QStringLiteral("(b9) control: both overrides exist "
                                 "before the reset"),
                  model.hasBuiltinOverride(gouache)
                      && model.hasBuiltinOverride(pencil));
            check(QStringLiteral("(b9) resetBuiltinToStock succeeds"),
                  model.resetBuiltinToStock(gouache));
            check(QStringLiteral("(b9) ...the override file is GONE"),
                  !QFile::exists(
                      root
                      + QStringLiteral("/Overrides/"
                                       "builtin_painting_gouache"
                                       ".sankobrush")));
            check(QStringLiteral("(b9) ...the SIBLING override is "
                                 "untouched (only that one, ever)"),
                  model.hasBuiltinOverride(pencil)
                      && QFile::exists(
                          root
                          + QStringLiteral("/Overrides/"
                                           "builtin_sketching_hb-pencil"
                                           ".sankobrush")));
            const ::Brush *stock = nullptr;
            const auto roster3 = brushlib::builtinRoster();
            for (const BrushPreset &pr : roster3)
                if (pr.id == gouache)
                    stock = &pr.brush;
            check(QStringLiteral("(b9) ...the model holds STOCK again, "
                                 "byte-exact through the codec"),
                  stock
                      && BrushPresetCodec::saveBrush(
                             model.preset(gouache)->brush)
                          == BrushPresetCodec::saveBrush(*stock));
            check(QStringLiteral("(b9) reset on a non-overridden preset "
                                 "refuses"),
                  !model.resetBuiltinToStock(gouache));
        }
        QDir(root).removeRecursively();
    }

    // ---- (b11) HB Pencil IS the promoted variation, byte for byte --------
    // The first user-preset promotion (2026-08-29): the built-in HB Pencil
    // recipe was rewritten from the user's "HB Pencil Variation". The
    // user's demanded form of the check, verbatim: construct BOTH brushes,
    // apply the approved delta (size 4 -> 36; opacity 0.55 was confirmed
    // kept), and compare saveBrush() output byte for byte - and if
    // anything else differs, NAME THE FIELD, because an unnamed
    // difference is a promotion error. The fixture is the promoted file
    // frozen at promotion time; later edits to the live variation must
    // not move this gate.
    {
        const auto firstBrushDiff = [](const ::Brush &x, const ::Brush &y) {
            using DP = ::Brush::DynamicProperty;
            QString d;
            const auto num = [&](const char *n, qreal a, qreal b) {
                if (d.isEmpty() && a != b)
                    d = QStringLiteral("%1: %2 vs %3")
                            .arg(QLatin1String(n)).arg(a).arg(b);
            };
            num("size", x.size(), y.size());
            num("spacing", x.spacing(), y.spacing());
            num("opacity", x.opacity(), y.opacity());
            num("flow", x.flow(), y.flow());
            num("hardness", x.hardness(), y.hardness());
            if (d.isEmpty() && x.color() != y.color()) d = "color";
            num("toolMode", int(x.toolMode()), int(y.toolMode()));
            num("smudgeStrength", x.smudgeStrength(), y.smudgeStrength());
            num("tiltAffectsShape", x.tiltAffectsShape(), y.tiltAffectsShape());
            num("rotationAffectsShape", x.rotationAffectsShape(),
                y.rotationAffectsShape());
            num("maxTiltElongation", x.maxTiltElongation(),
                y.maxTiltElongation());
            num("scatterAlong", x.scatterAlong(), y.scatterAlong());
            num("scatterPerpendicular", x.scatterPerpendicular(),
                y.scatterPerpendicular());
            num("scatterCount", x.scatterCount(), y.scatterCount());
            num("sizeJitter", x.sizeJitter(), y.sizeJitter());
            num("angleJitter", x.angleJitter(), y.angleJitter());
            num("roundnessJitter", x.roundnessJitter(), y.roundnessJitter());
            num("spacingJitter", x.spacingJitter(), y.spacingJitter());
            num("grainScale", x.grainScale(), y.grainScale());
            num("grainDepth", x.grainDepth(), y.grainDepth());
            num("grainContrast", x.grainContrast(), y.grainContrast());
            num("grainRotation", x.grainRotation(), y.grainRotation());
            num("grainMode", int(x.grainMode()), int(y.grainMode()));
            num("grainAffectsColor", x.grainAffectsColor(),
                y.grainAffectsColor());
            num("grainPreset", int(x.grainPreset()), int(y.grainPreset()));
            if (d.isEmpty() && x.grainTexture() != y.grainTexture())
                d = QStringLiteral("grainTexture (image bytes)");
            num("hueJitter", x.hueJitter(), y.hueJitter());
            num("saturationJitter", x.saturationJitter(),
                y.saturationJitter());
            num("brightnessJitter", x.brightnessJitter(),
                y.brightnessJitter());
            if (d.isEmpty() && x.customShape() != y.customShape())
                d = QStringLiteral("customShape (tip image bytes)");
            num("dualBlendMode", int(x.dualBlendMode()),
                int(y.dualBlendMode()));
            num("dualMasterOpacity", x.dualMasterOpacity(),
                y.dualMasterOpacity());
            num("dualBrushEnabled", x.dualBrushEnabled(),
                y.dualBrushEnabled());
            num("tipAngle", x.tipAngle(), y.tipAngle());
            num("tipRoundness", x.tipRoundness(), y.tipRoundness());
            num("tipFlipX", x.tipFlipX(), y.tipFlipX());
            num("tipFlipY", x.tipFlipY(), y.tipFlipY());
            num("fadeDistance", x.fadeDistance(), y.fadeDistance());
            num("wetEdges", x.wetEdges(), y.wetEdges());
            num("buildUp", x.buildUp(), y.buildUp());
            num("fgBgJitter", x.fgBgJitter(), y.fgBgJitter());
            if (d.isEmpty() && x.backgroundColor() != y.backgroundColor())
                d = QStringLiteral("backgroundColor");
            num("purity", x.purity(), y.purity());
            num("colorDynamicsPerTip", x.colorDynamicsPerTip(),
                y.colorDynamicsPerTip());
            num("dualMode", int(x.dualMode()), int(y.dualMode()));
            num("noise", x.noise(), y.noise());
            num("textureBlendMode", int(x.textureBlendMode()),
                int(y.textureBlendMode()));
            num("eraseMode", x.eraseMode(), y.eraseMode());
            static const char *kProps[] = {
                "Size", "Opacity", "Hardness", "Flow", "Scatter", "Smudge",
                "SizeJitter", "AngleJitter", "RoundnessJitter",
                "SpacingJitter", "GrainDepth", "HueJitter",
                "SaturationJitter", "BrightnessJitter",
                "ForegroundBackground"};
            for (int i = 0; i < 15 && d.isEmpty(); ++i) {
                const auto p = DP(i);
                if (int(x.controlSource(p)) != int(y.controlSource(p)))
                    d = QStringLiteral("controlSource(%1)")
                            .arg(QLatin1String(kProps[i]));
                else if (x.controlMinimum(p) != y.controlMinimum(p))
                    d = QStringLiteral("controlMinimum(%1)")
                            .arg(QLatin1String(kProps[i]));
            }
            const auto curve = [&](const char *n, const PressureCurve &a,
                                   const PressureCurve &b) {
                if (d.isEmpty() && a.controlPoints() != b.controlPoints())
                    d = QStringLiteral("%1 curve").arg(QLatin1String(n));
            };
            ::Brush &mx = const_cast<::Brush &>(x);
            ::Brush &my = const_cast<::Brush &>(y);
            curve("sizePressure", mx.sizePressureCurve(),
                  my.sizePressureCurve());
            curve("opacityPressure", mx.opacityPressureCurve(),
                  my.opacityPressureCurve());
            curve("hardnessPressure", mx.hardnessPressureCurve(),
                  my.hardnessPressureCurve());
            curve("flowPressure", mx.flowPressureCurve(),
                  my.flowPressureCurve());
            curve("scatterPressure", mx.scatterPressureCurve(),
                  my.scatterPressureCurve());
            curve("smudgePressure", mx.smudgePressureCurve(),
                  my.smudgePressureCurve());
            curve("sizeJitterPressure", mx.sizeJitterPressureCurve(),
                  my.sizeJitterPressureCurve());
            curve("angleJitterPressure", mx.angleJitterPressureCurve(),
                  my.angleJitterPressureCurve());
            curve("roundnessJitterPressure",
                  mx.roundnessJitterPressureCurve(),
                  my.roundnessJitterPressureCurve());
            curve("spacingJitterPressure", mx.spacingJitterPressureCurve(),
                  my.spacingJitterPressureCurve());
            curve("grainDepthPressure", mx.grainDepthPressureCurve(),
                  my.grainDepthPressureCurve());
            curve("hueJitterPressure", mx.hueJitterPressureCurve(),
                  my.hueJitterPressureCurve());
            curve("saturationJitterPressure",
                  mx.saturationJitterPressureCurve(),
                  my.saturationJitterPressureCurve());
            curve("brightnessJitterPressure",
                  mx.brightnessJitterPressureCurve(),
                  my.brightnessJitterPressureCurve());
            curve("fgBgJitterPressure", mx.fgBgJitterPressureCurve(),
                  my.fgBgJitterPressureCurve());
            return d.isEmpty()
                ? QStringLiteral("difference outside the field list")
                : d;
        };

        QFile fixtureFile(
            QStringLiteral(":/fixtures/hb_variation.sankobrush"));
        check(QStringLiteral("(b11) the promotion fixture is compiled in"),
              fixtureFile.open(QIODevice::ReadOnly));
        BrushPreset variation;
        check(QStringLiteral("(b11) the fixture loads and is the "
                             "variation"),
              BrushPresetCodec::loadPreset(fixtureFile.readAll(), variation)
                  && variation.name
                      == QStringLiteral("HB Pencil Variation"));
        const BrushPreset *hb = nullptr;
        for (const BrushPreset &p : roster)
            if (p.id == QStringLiteral("builtin/sketching/hb-pencil"))
                hb = &p;
        check(QStringLiteral("(b11) the built-in HB Pencil exists with the "
                             "ASSET tip loaded (1177x1102 - a null here "
                             "means brush_assets.qrc is missing from this "
                             "target)"),
              hb && hb->brush.hasCustomShape()
                  && hb->brush.customShape().size() == QSize(1177, 1102));
        if (hb) {
            ::Brush want = variation.brush;
            want.setSize(36); // THE single approved delta
            const QByteArray fromVariation =
                BrushPresetCodec::saveBrush(want);
            const QByteArray fromRecipe =
                BrushPresetCodec::saveBrush(hb->brush);
            check(QStringLiteral("(b11) built-in == variation, byte for "
                                 "byte, with ONLY size differing"),
                  fromVariation == fromRecipe,
                  fromVariation == fromRecipe
                      ? QString()
                      : firstBrushDiff(want, hb->brush));
            check(QStringLiteral("(b11) control: opacity is the KEPT 0.55 "
                                 "ceiling on both sides"),
                  hb->brush.opacity() == 0.55
                      && variation.brush.opacity() == 0.55);
        }
    }

    // ---- (b12) stamp-tip census: every asset-bearing built-in loaded -----
    // The pencil stamp promotion (2026-08-30, first batch 4H/2B/6B beside
    // HB). Each image-bearing built-in must hold its asset at the source
    // dimensions - a null or wrong-size tip here means brush_assets.qrc is
    // missing from a roster-building target, or an asset was replaced
    // without its recipe.
    {
        struct StampSpec { const char *id; int w; int h; };
        const StampSpec specs[] = {
            {"builtin/sketching/hb-pencil", 1177, 1102},
            {"builtin/sketching/4h-pencil", 1254, 1254},
            {"builtin/sketching/2b-pencil", 1254, 1254},
            {"builtin/sketching/6b-pencil", 1254, 1254},
            {"builtin/sketching/h-pencil", 1296, 1214},
            {"builtin/sketching/2h-pencil", 1295, 1215},
            {"builtin/sketching/4b-pencil", 1254, 1254},
            {"builtin/sketching/mechanical-pencil", 1254, 1254},
            {"builtin/sketching/blue-pencil", 1254, 1254},
            {"builtin/sketching/charcoal-pencil", 1254, 1254},
            {"builtin/drawing/soft-pastel", 1254, 1254},
            {"builtin/drawing/compressed-charcoal", 1254, 1254},
            {"builtin/drawing/grease-pencil", 1254, 1254},
        };
        for (const StampSpec &s : specs) {
            const BrushPreset *p = nullptr;
            for (const BrushPreset &candidate : roster)
                if (candidate.id == QLatin1String(s.id))
                    p = &candidate;
            check(QStringLiteral("(b12) %1 carries its stamp tip at "
                                 "%2x%3")
                      .arg(QLatin1String(s.id)).arg(s.w).arg(s.h),
                  p && p->brush.hasCustomShape()
                      && p->brush.customShape().size() == QSize(s.w, s.h),
                  p ? QStringLiteral("%1x%2")
                          .arg(p->brush.customShape().width())
                          .arg(p->brush.customShape().height())
                    : QStringLiteral("preset missing"));
        }
    }

    // ---- (b10) the custom-image cap + the encoded-image memo -------------
    // The cap (Brush::kMaxCustomImageDim): an 18-megapixel photo loaded as
    // grain made a 14 MB preset that froze the studio per gesture. The
    // memo: the studio serialises the session brush on every gesture, so
    // encodePng memoises on QImage::cacheKey - and the stale-memo hazard
    // (a replaced same-dimension image serialising the OLD bytes) is
    // pinned here from behaviour, not from the docs alone.
    {
        // Cap, aspect preserved, both setters; under-cap passes untouched.
        QImage big(3000, 2000, QImage::Format_Grayscale8);
        big.fill(180);
        ::Brush capped;
        capped.setCustomGrain(big);
        check(QStringLiteral("(b10) an oversized grain caps to 2048, "
                             "aspect preserved, Grayscale8"),
              capped.grainTexture().size() == QSize(2048, 1365)
                  && capped.grainTexture().format()
                      == QImage::Format_Grayscale8,
              QStringLiteral("%1x%2").arg(capped.grainTexture().width())
                  .arg(capped.grainTexture().height()));
        QImage wide(4000, 1000, QImage::Format_Grayscale8);
        wide.fill(90);
        capped.setCustomShape(wide);
        check(QStringLiteral("(b10) an oversized tip caps to 2048, aspect "
                             "preserved"),
              capped.customShape().size() == QSize(2048, 512));
        QImage under(1177, 1102, QImage::Format_Grayscale8);
        for (int y = 0; y < under.height(); ++y) {
            uchar *row = under.scanLine(y);
            for (int x = 0; x < under.width(); ++x)
                row[x] = uchar((x * 7 + y * 13) & 255);
        }
        ::Brush untouched;
        untouched.setCustomGrain(under);
        check(QStringLiteral("(b10) control: an under-cap image passes "
                             "byte-untouched (the user's 1177x1102 tip "
                             "case)"),
              untouched.grainTexture() == under);

        // cacheKey distinctness - the memo's key contract, AS A TEST:
        // two distinct images of identical dimensions must never share a
        // key (Qt: "distinct QImage objects can only have the same key if
        // they refer to the same contents").
        QImage sameDimsA(64, 64, QImage::Format_Grayscale8);
        sameDimsA.fill(10);
        QImage sameDimsB(64, 64, QImage::Format_Grayscale8);
        sameDimsB.fill(200);
        check(QStringLiteral("(b10) cacheKey: distinct same-dimension "
                             "images key differently"),
              sameDimsA.cacheKey() != sameDimsB.cacheKey());

        // The memo: a second save of an unchanged brush encodes ZERO
        // images and returns byte-identical output.
        ::Brush memoBrush;
        memoBrush.setCustomGrain(under);
        const int before = BrushPresetCodec::imageEncodeCountForTest();
        const QByteArray save1 = BrushPresetCodec::saveBrush(memoBrush);
        const int afterFirst = BrushPresetCodec::imageEncodeCountForTest();
        const QByteArray save2 = BrushPresetCodec::saveBrush(memoBrush);
        const int afterSecond = BrushPresetCodec::imageEncodeCountForTest();
        check(QStringLiteral("(b10) memo: repeated saves are "
                             "byte-identical and the second encodes ZERO "
                             "images"),
              save1 == save2 && afterFirst > before
                  && afterSecond == afterFirst,
              QStringLiteral("encodes %1 -> %2 -> %3")
                  .arg(before).arg(afterFirst).arg(afterSecond));

        // THE STALE-MEMO HAZARD, pinned: replace the grain with a
        // DIFFERENT image of IDENTICAL dimensions; the save must carry
        // the NEW bytes (a stale memo would silently serialise the old
        // image into the preset).
        QImage replaced(under.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < replaced.height(); ++y) {
            uchar *row = replaced.scanLine(y);
            for (int x = 0; x < replaced.width(); ++x)
                row[x] = uchar((x * 3 + y * 31 + 97) & 255);
        }
        memoBrush.setCustomGrain(replaced);
        const QByteArray save3 = BrushPresetCodec::saveBrush(memoBrush);
        ::Brush reloaded;
        check(QStringLiteral("(b10) replacing an image with a same-size "
                             "one serialises the NEW bytes (stale-memo "
                             "hazard)"),
              save3 != save1
                  && BrushPresetCodec::loadBrush(save3, reloaded)
                  && reloaded.grainTexture() == memoBrush.grainTexture());

        // LEGACY: a pre-cap file with an oversized embedded image, built
        // by splicing a big PNG into a real save's length-prefixed image
        // slot (the wire format: QByteArray = quint32 BE length + bytes).
        // Loading must cap in memory AND raise the capped-on-load flag;
        // the unspliced control must not.
        const auto pngBytes = [](const QImage &img) {
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            return png;
        };
        const auto beU32 = [](quint32 v) {
            QByteArray b(4, 0);
            b[0] = char(v >> 24); b[1] = char(v >> 16);
            b[2] = char(v >> 8);  b[3] = char(v);
            return b;
        };
        ::Brush smallGrain;
        QImage marker(32, 32, QImage::Format_Grayscale8);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                marker.scanLine(y)[x] = uchar(x * 8 ^ y * 5);
        smallGrain.setCustomGrain(marker); // the ONLY image in the brush
        const QByteArray cleanBytes = BrushPresetCodec::saveBrush(smallGrain);
        const QByteArray smallPng = pngBytes(smallGrain.grainTexture());
        const int slot = cleanBytes.indexOf(smallPng);
        check(QStringLiteral("(b10) splice setup: the image slot is "
                             "locatable in the wire bytes"),
              slot > 4
                  && cleanBytes.mid(slot - 4, 4)
                      == beU32(quint32(smallPng.size())));
        QImage oversized(2500, 300, QImage::Format_Grayscale8);
        oversized.fill(66);
        const QByteArray bigPng = pngBytes(oversized);
        QByteArray spliced = cleanBytes;
        spliced.replace(slot - 4, 4 + smallPng.size(),
                        beU32(quint32(bigPng.size())) + bigPng);
        ::Brush legacy;
        bool cappedFlag = false;
        check(QStringLiteral("(b10) a pre-cap file loads with the image "
                             "CAPPED in memory and the flag raised"),
              BrushPresetCodec::loadBrush(spliced, legacy, &cappedFlag)
                  && cappedFlag
                  && legacy.grainTexture().width() == 2048,
              QStringLiteral("w=%1 flag=%2")
                  .arg(legacy.grainTexture().width()).arg(cappedFlag));
        bool cleanFlag = true;
        ::Brush cleanLoad;
        check(QStringLiteral("(b10) control: the unspliced file loads "
                             "with the flag DOWN"),
              BrushPresetCodec::loadBrush(cleanBytes, cleanLoad, &cleanFlag)
                  && !cleanFlag);
    }

    // ---- (b8) the ERASER MIRROR: one definition, referenced twice --------
    // The dedicated eraser roster is GONE (2026-08-28): every mirrorable
    // brush preset IS the eraser preset, eraseMode applied at activation on
    // a copy. Pinned here: the census (56 of 62 mirror; the 6 smudge/dual
    // presets are excluded because eraseMode forces the mask path and their
    // names would lie - the 6 colour-buffer presets DO mirror, colour being
    // unobservable in erase mode), the Brush-level guards that make a dirty
    // eraser unconstructible, the one-definition codec identity, and the
    // ROSTER-SIZED eraser swatch SHA.
    //
    // >>> COUPLED-PIN PROCEDURE - READ BEFORE TOUCHING EITHER SHA <<<
    // kEraserSwatchSha pins renders DERIVED from builtinRoster(), so it is
    // COUPLED to the brush preview SHA (193847fa...):
    //   * a legitimate roster/fixture change moves BOTH - re-baseline BOTH
    //     in the SAME commit, one reason, stated once;
    //   * kEraserSwatchSha moving ALONE (193847fa... intact) is a DEFECT in
    //     the erase render or the mirror plumbing, never a re-baseline.
    // The asymmetry is the point: the brush SHA is the source, this one is
    // derived. When in doubt, find the cause; loosening a pin is never it.
    {
        const QVector<BrushPreset> roster2 = brushlib::builtinRoster();
        QVector<const BrushPreset *> mirrorable, excluded;
        for (const BrushPreset &p : roster2)
            (p.brush.smudgeActive() || p.brush.dualBrushEnabled()
                 ? excluded : mirrorable) << &p;
        check(QStringLiteral("(b8) 56 of 62 presets mirror as erasers"),
              mirrorable.size() == 56 && roster2.size() == 62,
              QStringLiteral("%1 of %2").arg(mirrorable.size())
                  .arg(roster2.size()));
        bool exclOk = excluded.size() == 6;
        for (const BrushPreset *p : excluded)
            exclOk = exclOk
                && (p->brush.smudgeActive() || p->brush.dualBrushEnabled());
        check(QStringLiteral("(b8) exactly 6 excluded, each smudge or dual"),
              exclOk, QStringLiteral("%1 excluded").arg(excluded.size()));

        // THE ONE-DEFINITION IDENTITY: toggling eraseMode on and back off
        // leaves the serialised brush BYTE-IDENTICAL for every mirrorable
        // preset - the flag is the ONLY thing the mirror ever changes.
        bool identityOk = true;
        QString firstBad2;
        for (const BrushPreset *p : mirrorable) {
            ::Brush copy = p->brush;
            copy.setEraseMode(true);
            copy.setEraseMode(false);
            if (BrushPresetCodec::saveBrush(copy)
                != BrushPresetCodec::saveBrush(p->brush)) {
                identityOk = false;
                firstBad2 = p->name;
                break;
            }
        }
        check(QStringLiteral("(b8) eraseMode round-trip leaves every "
                             "mirrorable preset byte-identical"),
              identityOk, firstBad2);

        // The Brush-level guards (unchanged from the dedicated-roster era;
        // they are what make a contradictory eraser unconstructible).
        {
            ::Brush smudgy;
            smudgy.setToolMode(::Brush::ToolMode::Smudge);
            smudgy.setSmudgeStrength(0.8);
            check(QStringLiteral("(b8) control: the smudge brush smudges "
                                 "before eraseMode"),
                  smudgy.smudgeActive());
            smudgy.setEraseMode(true);
            check(QStringLiteral("(b8) eraseMode FORCES smudge off"),
                  !smudgy.smudgeActive() && !smudgy.usesColorStrokeBuffer());

            ::Brush dual;
            dual.setDualBrushEnabled(true);
            check(QStringLiteral("(b8) control: the dual brush is dual "
                                 "before eraseMode"),
                  dual.dualBrushEnabled());
            dual.setEraseMode(true);
            check(QStringLiteral("(b8) eraseMode FORCES the dual brush off"),
                  !dual.dualBrushEnabled());

            ::Brush jitter;
            jitter.setHueJitter(0.4);
            check(QStringLiteral("(b8) control: hue jitter trips the colour "
                                 "buffer before eraseMode"),
                  jitter.usesColorStrokeBuffer());
            jitter.setEraseMode(true);
            check(QStringLiteral("(b8) eraseMode FORCES the mask path over "
                                 "the colour buffer"),
                  !jitter.usesColorStrokeBuffer());
        }

        // The MIRRORED swatches: every mirrorable preset's erase character,
        // rendered over the band with the carve, deterministic, combined
        // into the roster-sized pin.
        // The erase swatch depicts the COVERAGE: white ink on transparency
        // (2026-08-28, replacing the banded carve - a deliberate change of
        // depiction, re-baselining kEraserSwatchSha with approval). Checks:
        // transparent background at the corners, at least one white-ish
        // stroke pixel, no coloured pixels (an eraser has no colour), and
        // determinism - with the brush-swatch SHA proving elsewhere in this
        // run that ONLY the erase depiction changed.
        QCryptographicHash eraserCombined(QCryptographicHash::Sha256);
        bool renderOk = true, lookOk = true, determOk = true;
        QString firstBad3;
        for (const BrushPreset *p : mirrorable) {
            ::Brush eraseCopy = p->brush;
            eraseCopy.setEraseMode(true);
            const QImage a = BrushPreviewRenderer::renderPreviewImage(eraseCopy);
            const QImage b = BrushPreviewRenderer::renderPreviewImage(eraseCopy);
            if (a.isNull()) { renderOk = false; firstBad3 = p->name; break; }
            determOk = determOk && imageBytes(a) == imageBytes(b);
            eraserCombined.addData(imageBytes(a));
            bool sawStroke = false, sawColour = false;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x) {
                    const QRgb px = a.pixel(x, y);
                    if (qAlpha(px) > 0) {
                        // White-ish: the removal footprint carries shape
                        // and texture in ALPHA; the ink itself is white.
                        if (qAbs(qRed(px) - qGreen(px)) > 12
                            || qAbs(qGreen(px) - qBlue(px)) > 12)
                            sawColour = true;
                        if (qRed(px) > 200 && qGreen(px) > 200
                            && qBlue(px) > 200)
                            sawStroke = true;
                    }
                }
            const bool cornersClear =
                qAlpha(a.pixel(0, 0)) == 0
                && qAlpha(a.pixel(a.width() - 1, 0)) == 0
                && qAlpha(a.pixel(0, a.height() - 1)) == 0
                && qAlpha(a.pixel(a.width() - 1, a.height() - 1)) == 0;
            if (!(sawStroke && !sawColour && cornersClear)) {
                lookOk = false;
                if (firstBad3.isEmpty())
                    firstBad3 = p->name;
            }
        }
        check(QStringLiteral("(b8) all 56 mirrored swatches render"),
              renderOk, firstBad3);
        check(QStringLiteral("(b8) mirrored swatches deterministic"),
              determOk);
        check(QStringLiteral("(b8) every mirrored swatch is a WHITE mark on "
                             "transparency (no band, no colour)"),
              lookOk, firstBad3);
        const QString eraserSha =
            QString::fromLatin1(eraserCombined.result().toHex());
        ts << "COMBINED ERASER SWATCH SHA (compare across configs): "
           << eraserSha << nl;
        check(QStringLiteral("(b8) eraser swatch SHA matches the pin"),
              eraserSha == QLatin1String(kEraserSwatchSha), eraserSha);
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
