// SankoTV brush pixel-lock test — PERMANENT, ships with the code.
//
// This is the committed baseline for the Sanko Paint brush pipeline,
// superseding the standalone Phase 4a closure hash
// (421845ebe66ca7892ddee3f49728365ce20c12424018470b56ef8cab67ed1ff7),
// whose fixture definition was never imported and is therefore
// unreproducible in this tree. The lesson that created this file: a pixel
// lock is only a lock if its fixture ships with the code. Every input is
// pinned EXPLICITLY below — never rely on current defaults.
//
// FIXTURE (all inputs pinned):
//   Canvas:      960 x 540 (SankoTV's fixed panel resolution)
//   Background:  fully transparent ARGB32 (beforeRegion cleared to 0)
//   Brush:       procedural round tip (engine default shape)
//                colour   opaque black rgba(0,0,0,255)
//                size     24 px
//                opacity  1.0
//                hardness 1.0
//                spacing  0.05
//                size/opacity pressure curves {0,0}->{1,1}; hardness {0,1}->{1,1}
//                (all points at pressure 1.0, so curves are pinned for
//                stability, not effect)
//   Stroke:      33 raw points on the diagonal (100,100) -> (860,440),
//                pressure 1.0, no tilt/rotation
//   Seed:        42 (primary slot 0; dual secondary slot 1)
//   Render:      SankoPaintHostAdapter::render() — the exact commit path.
//
// ASSERTS (Debug and Release must both pass; the shared baseline hash IS
// the cross-config determinism proof):
//   1. R16 plain-brush CPU render hashes to kBaselineR16
//   2. CPU render is deterministic (two renders byte-identical)
//   3. R16 CPU vs GPU max channel delta <= 3/255   (Phase 4a closure value)
//   4. RGBA16 colour-stroke-buffer path (hue jitter 0.2) CPU vs GPU <= 2/255
//   5. Phase 5 dual-brush CPU vs GPU <= 3/255
//   6. ERASE composite (the Eraser Library engine pass): CPU erase render
//      over a deterministic filled base hashes to kBaselineErase and is
//      deterministic; CPU vs GPU <= 3/255; full coverage at opacity 1
//      leaves alpha EXACTLY 0; an untouched corner is byte-identical to
//      the base; partially-erased pixels keep their colour channels.
//      Each absence assertion carries its positive control.
//
// Run:  build/<config>/SankoPaintPixelLock.exe   (exit code = failure count;
// GPU checks fall back to CPU-vs-CPU reporting if no D3D11 device exists,
// and say so — the hash lock itself is CPU-only and always authoritative.)

#include "SankoPaintHostAdapter.h"
#include "SankoSettings.h"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QDir>

#include <cstdio>
#include <memory>

static const char kBaselineErase[] =
    "0bc243812b4334c3ff7af0c76a9bbc40b9ffe3a98dbdc41b9ad06cddd46fa32f";
static const char kBaselineR16[] =
    "666f7b455228e18020ad6b4967740de762e559140e8e52c98ddb415a9f91547a";

static ::Brush fixtureBrush()
{
    ::Brush b;
    b.setColor(QColor(0, 0, 0, 255));
    b.setSize(24);
    b.setOpacity(1.0);
    b.setHardness(1.0);
    b.setSpacing(0.05);
    b.sizePressureCurve().setControlPoints({{0.0, 0.0}, {1.0, 1.0}});
    b.opacityPressureCurve().setControlPoints({{0.0, 0.0}, {1.0, 1.0}});
    b.hardnessPressureCurve().setControlPoints({{0.0, 1.0}, {1.0, 1.0}});
    return b;
}

static SankoPaintHostAdapter::StrokeResult renderFixture(::Brush b, bool gpu,
                                                         bool dual)
{
    if (dual)
        b.setDualBrushEnabled(true);
    StrokeBuilder sb(QSize(960, 540), b, false, 42, 0);
    std::unique_ptr<StrokeBuilder> sb2;
    if (dual)
        sb2 = std::make_unique<StrokeBuilder>(QSize(960, 540),
                                              b.secondaryBrush(), false, 42, 1);
    for (int i = 0; i <= 32; ++i) {
        StrokePoint p;
        p.position =
            QPointF(100 + i * (760.0 / 32), 100 + i * (340.0 / 32));
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
    w.beforeRegion = before;
    w.seed = 42;
    w.preferGpu = gpu;
    StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush, before,
                                        w.affectedRect.topLeft());
    if (sb2)
        StrokeBuilder::resolveColorDynamics(w.secondaryStamps,
                                            w.brush.secondaryBrush(), before,
                                            w.affectedRect.topLeft());
    return SankoPaintHostAdapter::render(w);
}

// Deterministic filled base for the erase fixture: erasing transparency is
// a no-op, so the erase checks render over THIS, never over transparent.
static QImage eraseBase(const QSize &size)
{
    QImage base(size, QImage::Format_ARGB32);
    for (int y = 0; y < base.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(base.scanLine(y));
        for (int x = 0; x < base.width(); ++x)
            row[x] = qRgba(30 + (x * 7) % 200, 40 + (y * 5) % 180,
                           60 + ((x + y) * 3) % 160, 255);
    }
    return base;
}

static SankoPaintHostAdapter::StrokeResult renderEraseFixture(bool gpu)
{
    ::Brush b = fixtureBrush();
    b.setEraseMode(true);
    StrokeBuilder sb(QSize(960, 540), b, false, 42, 0);
    for (int i = 0; i <= 32; ++i) {
        StrokePoint p;
        p.position =
            QPointF(100 + i * (760.0 / 32), 100 + i * (340.0 / 32));
        p.pressure = 1.0;
        sb.addRawPoint(p);
    }
    SankoPaintHostAdapter::StrokeWork w;
    w.layerKey = QStringLiteral("eraseFixture");
    w.canvasSize = QSize(960, 540);
    w.brush = b;
    w.rawPoints = sb.rawPoints();
    w.primaryStamps = sb.stamps();
    w.affectedRect = sb.affectedRect().intersected(QRect(0, 0, 960, 540));
    w.beforeRegion = eraseBase(QSize(960, 540)).copy(w.affectedRect);
    w.seed = 42;
    w.preferGpu = gpu;
    StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush,
                                        w.beforeRegion,
                                        w.affectedRect.topLeft());
    return SankoPaintHostAdapter::render(w);
}

static QString shaHex(const QImage &img)
{
    const QImage n = img.convertToFormat(QImage::Format_ARGB32);
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(n.constBits()),
                           n.sizeInBytes()),
            QCryptographicHash::Sha256)
            .toHex());
}

static int maxDelta(const QImage &x, const QImage &y)
{
    const QImage a = x.convertToFormat(QImage::Format_ARGB32);
    const QImage b = y.convertToFormat(QImage::Format_ARGB32);
    if (a.size() != b.size())
        return 255;
    int m = 0;
    for (int yy = 0; yy < a.height(); ++yy) {
        const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(yy));
        const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(yy));
        for (int xx = 0; xx < a.width(); ++xx) {
            m = qMax(m, qAbs(qRed(ra[xx]) - qRed(rb[xx])));
            m = qMax(m, qAbs(qGreen(ra[xx]) - qGreen(rb[xx])));
            m = qMax(m, qAbs(qBlue(ra[xx]) - qBlue(rb[xx])));
            m = qMax(m, qAbs(qAlpha(ra[xx]) - qAlpha(rb[xx])));
        }
    }
    return m;
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv); // QRhi needs a Gui application
    // Every settings read/write in app code goes through sankoSettings();
    // this override points the store at scratch so the family can NEVER
    // touch the user's real settings, driven or not.
    sankoSettingsSetOverrideForTest(QDir::tempPath()
                                    + QStringLiteral("/sanko_pixellock_settings.ini"));
    int failures = 0;
    auto check = [&failures](bool ok, const char *name, const QString &info) {
        std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                    info.isEmpty() ? "" : "  ", qPrintable(info));
        if (!ok)
            ++failures;
    };

    const auto cpuA = renderFixture(fixtureBrush(), false, false);
    const auto cpuB = renderFixture(fixtureBrush(), false, false);
    const QString hash = shaHex(cpuA.afterRegion);
    check(cpuA.succeeded && hash == QLatin1String(kBaselineR16),
          "R16 baseline hash", QStringLiteral("hash=") + hash);
    check(cpuB.succeeded && shaHex(cpuB.afterRegion) == hash,
          "seeded CPU determinism", QString());

    const auto gpuR16 = renderFixture(fixtureBrush(), true, false);
    const bool gpuLive = gpuR16.succeeded && !gpuR16.renderer.usedCpuFallback;
    const int dR16 = maxDelta(cpuA.afterRegion, gpuR16.afterRegion);
    check(gpuR16.succeeded && dR16 <= 3, "R16 CPU/GPU <= 3/255",
          QStringLiteral("max=%1%2")
              .arg(dR16)
              .arg(gpuLive ? "" : " (GPU unavailable - CPU fallback)"));

    ::Brush jb = fixtureBrush();
    jb.setHueJitter(0.2); // pins the RGBA16 colour-stroke-buffer path
    const auto cpuC = renderFixture(jb, false, false);
    const auto gpuC = renderFixture(jb, true, false);
    const int dRGBA = maxDelta(cpuC.afterRegion, gpuC.afterRegion);
    check(cpuC.succeeded && gpuC.succeeded && dRGBA <= 2,
          "RGBA16 CPU/GPU <= 2/255", QStringLiteral("max=%1").arg(dRGBA));

    const auto cpuD = renderFixture(fixtureBrush(), false, true);
    const auto gpuD = renderFixture(fixtureBrush(), true, true);
    const int dDual = maxDelta(cpuD.afterRegion, gpuD.afterRegion);
    check(cpuD.succeeded && gpuD.succeeded && dDual <= 3,
          "dual CPU/GPU <= 3/255", QStringLiteral("max=%1").arg(dDual));

    // ---- 6. the erase composite ----------------------------------------
    const auto eraseA = renderEraseFixture(false);
    const auto eraseB = renderEraseFixture(false);
    const QString eraseHash = shaHex(eraseA.afterRegion);
    check(eraseA.succeeded && eraseHash == QLatin1String(kBaselineErase),
          "erase CPU render hashes to kBaselineErase",
          QStringLiteral("hash=%1").arg(eraseHash));
    check(eraseB.succeeded && shaHex(eraseB.afterRegion) == eraseHash,
          "erase CPU render deterministic", QString());
    const auto eraseGpu = renderEraseFixture(true);
    const int dErase = maxDelta(eraseA.afterRegion, eraseGpu.afterRegion);
    check(eraseA.succeeded && eraseGpu.succeeded && dErase <= 3,
          "erase CPU/GPU <= 3/255", QStringLiteral("max=%1").arg(dErase));
    {
        const QImage base = eraseBase(QSize(960, 540));
        const QImage after =
            eraseA.afterRegion.convertToFormat(QImage::Format_ARGB32);
        const QRect r = eraseA.affectedRect;
        const QPoint spine(480 - r.x(), 271 - r.y()); // on the diagonal
        check(qAlpha(base.pixel(480, 271)) == 255,
              "control: the base is opaque at the spine", QString());
        check(after.rect().contains(spine)
                  && qAlpha(after.pixel(spine)) == 0,
              "full coverage at opacity 1 erases to alpha EXACTLY 0",
          QStringLiteral("alpha=%1")
                  .arg(after.rect().contains(spine)
                           ? qAlpha(after.pixel(spine)) : -1));
        const QRect corner(QPoint(r.width() - 24, 0), QSize(24, 24));
        bool cornerUntouched = true;
        int changed = 0;
        for (int y = corner.top(); y <= corner.bottom(); ++y)
            for (int x = corner.left(); x <= corner.right(); ++x)
                if (after.pixel(x, y) != base.pixel(r.x() + x, r.y() + y))
                    cornerUntouched = false;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x)
                if (after.pixel(x, y) != base.pixel(r.x() + x, r.y() + y))
                    ++changed;
        check(cornerUntouched,
              "zero-coverage region is byte-untouched", QString());
        check(changed > 1000,
              "CONTROL: the same comparison sees the erased pixels",
              QStringLiteral("%1 changed").arg(changed));
    }
    {
        // Partial erase: the hardness-1.0 fixture produces only 0/255
        // coverage, so a HALF-STRENGTH erase provides the partial pixels -
        // every erased pixel lands near alpha 128 and must keep its colour
        // channels exactly (straight alpha: erase touches alpha only).
        ::Brush half = fixtureBrush();
        half.setEraseMode(true);
        half.setOpacity(0.5);
        StrokeBuilder sb(QSize(960, 540), half, false, 42, 0);
        for (int i = 0; i <= 32; ++i) {
            StrokePoint p;
            p.position =
                QPointF(100 + i * (760.0 / 32), 100 + i * (340.0 / 32));
            p.pressure = 1.0;
            sb.addRawPoint(p);
        }
        SankoPaintHostAdapter::StrokeWork w;
        w.layerKey = QStringLiteral("eraseHalf");
        w.canvasSize = QSize(960, 540);
        w.brush = half;
        w.rawPoints = sb.rawPoints();
        w.primaryStamps = sb.stamps();
        w.affectedRect = sb.affectedRect().intersected(QRect(0, 0, 960, 540));
        const QImage base = eraseBase(QSize(960, 540));
        w.beforeRegion = base.copy(w.affectedRect);
        w.seed = 42;
        w.preferGpu = false;
        StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush,
                                            w.beforeRegion,
                                            w.affectedRect.topLeft());
        const auto res = SankoPaintHostAdapter::render(w);
        const QImage after =
            res.afterRegion.convertToFormat(QImage::Format_ARGB32);
        const QRect r = res.affectedRect;
        int partial = 0;
        bool coloursKept = true;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x) {
                const QRgb a = after.pixel(x, y);
                const QRgb o = base.pixel(r.x() + x, r.y() + y);
                if (qAlpha(a) > 20 && qAlpha(a) < 235
                    && qAlpha(a) != qAlpha(o)) {
                    ++partial;
                    coloursKept = coloursKept && qRed(a) == qRed(o)
                        && qGreen(a) == qGreen(o) && qBlue(a) == qBlue(o);
                }
            }
        check(res.succeeded && partial > 1000 && coloursKept,
              "half-strength erase: partial pixels keep colour channels",
              QStringLiteral("%1 partial pixel(s)").arg(partial));
    }

    std::printf("%s (%d failure%s)\n", failures ? "RESULT FAIL" : "RESULT PASS",
                failures, failures == 1 ? "" : "s");
    SankoPaintHostAdapter::shutdownGpuForCurrentThread();
    return failures;
}
