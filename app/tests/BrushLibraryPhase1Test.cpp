// SankoTV Brush Library — Phase 1 verification (TEMPORARY test target).
//
// Covers the model layer before any UI exists, using the same seeded render
// path as SankoPaintPixelLock:
//   (a) every category has >= 10 brushes; the full roster + recipes printed
//   (b) preset round-trip: saveBrush -> loadBrush -> render is BYTE-
//       identical to rendering the original, for every builtin; codec bytes
//       are idempotent (save(load(save)) == save)
//   (k) RGBA16 audit: exactly the intended brushes report
//       usesColorStrokeBuffer(), and no pencil/pen trips it
//
// Removed at Phase 5 together with the in-app seam; the in-app seam then
// re-asserts (b) through the canvas.  Run: exit code = failure count;
// report written to brushlib_phase1.txt next to the exe's working dir.

#include "BrushLibraryModel.h"
#include "BrushPresetCodec.h"
#include "BuiltinRoster.h"
#include "SankoPaintHostAdapter.h"

#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QTextStream>

#include <cstdio>
#include <memory>

using brushlib::BrushPreset;
using brushlib::BrushPresetCodec;

namespace {

QString renderHash(const ::Brush &brush)
{
    // The pixel-lock fixture path: 33 points on the diagonal, pressure 1,
    // seed 42, rendered through SankoPaintHostAdapter::render() — the exact
    // machinery the undo system's seeded replay uses.
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
        // A pure smudge over emptiness is a no-op (its commit is correctly
        // invalid), so smudge brushes get deterministic content to drag:
        // three opaque colour bands across the region.
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
    w.preferGpu = false; // CPU: the deterministic reference path
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
    if (b.toolMode() == ::Brush::ToolMode::Smudge
        || b.smudgeStrength() > 0)
        s += QStringLiteral(" smudge=%1").arg(b.smudgeStrength(), 0, 'f', 2);
    if (b.hueJitter() > 0 || b.saturationJitter() > 0
        || b.brightnessJitter() > 0)
        s += QStringLiteral(" colorJitter");
    if (b.grainAffectsColor())
        s += QStringLiteral(" colorGrain");
    if (b.dualBrushEnabled())
        s += QStringLiteral(" dual");
    if (b.tiltAffectsShape())
        s += QStringLiteral(" tilt");
    return s;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv); // QImage/QPainter need a Gui app

    QFile out(QStringLiteral("brushlib_phase1.txt"));
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

    // The RGBA16 set we DESIGNED — (k) asserts reality matches intent.
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

    // Unique ids and names.
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

    // ---- (b) codec round-trip: bytes idempotent + render identical ------
    {
        bool bytesOk = true, renderOk = true, presetOk = true;
        QString firstBad;
        for (const BrushPreset &p : roster) {
            const QByteArray bytes = BrushPresetCodec::saveBrush(p.brush);
            ::Brush loaded;
            if (!BrushPresetCodec::loadBrush(bytes, loaded)) {
                bytesOk = false;
                firstBad = p.name + QStringLiteral(" (load failed)");
                break;
            }
            if (BrushPresetCodec::saveBrush(loaded) != bytes) {
                bytesOk = false;
                firstBad = p.name + QStringLiteral(" (bytes drift)");
                break;
            }
            const QString h0 = renderHash(p.brush);
            const QString h1 = renderHash(loaded);
            if (h0 != h1 || h0.startsWith(QStringLiteral("RENDER-FAILED"))) {
                renderOk = false;
                firstBad = p.name + QStringLiteral(" (render ") + h0
                    + QStringLiteral(" vs ") + h1 + QStringLiteral(")");
                break;
            }
            // Full-preset file round-trip too (id/name/category/builtin).
            BrushPreset back;
            if (!BrushPresetCodec::loadPreset(
                    BrushPresetCodec::savePreset(p), back)
                || back.id != p.id || back.name != p.name
                || back.category != p.category || back.builtin != p.builtin
                || BrushPresetCodec::saveBrush(back.brush) != bytes) {
                presetOk = false;
                firstBad = p.name + QStringLiteral(" (preset file)");
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

    // ---- (b4) settingsHash: rename-stable, edit-sensitive ---------------
    {
        const BrushPreset &p = roster.first();
        const QByteArray h0 = BrushPresetCodec::settingsHash(p.brush);
        ::Brush edited = p.brush;
        edited.setSize(p.brush.size() + 5);
        check(QStringLiteral(
                  "(b4) settingsHash ignores name, changes on edit"),
              BrushPresetCodec::settingsHash(p.brush) == h0
                  && BrushPresetCodec::settingsHash(edited) != h0);
    }

    // ---- (k) RGBA16 audit ------------------------------------------------
    {
        bool ok = true;
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
        }
        check(QStringLiteral(
                  "(k) RGBA16 exactly as designed (%1 of %2 brushes)")
                  .arg(rgbaCount)
                  .arg(roster.size()),
              ok && rgbaCount == expectedRgba16.size(), detail);
        // Every Sketching/Drawing/Inking brush must stay R16.
        bool pencilsOk = true;
        for (const BrushPreset &p : roster)
            if ((p.category == QStringLiteral("Sketching")
                 || p.category == QStringLiteral("Drawing")
                 || p.category == QStringLiteral("Inking"))
                && p.brush.usesColorStrokeBuffer()) {
                pencilsOk = false;
                detail += p.id;
            }
        check(QStringLiteral("(k2) no pencil/pen/ink brush trips RGBA16"),
              pencilsOk, detail);
    }

    ts << nl << "FAILURES: " << failures << nl;
    out.close();
    return failures;
}
