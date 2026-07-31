// SankoTV canvas brush-ASSEMBLY lock — PERMANENT, ships with the code.
//
// SankoPaintPixelLock constructs its own Brush from pinned parameters, so it
// guards the RENDER path only. This test guards the path the pixel lock
// provably cannot see: the DrawingCanvas slider slots assembling the working
// paint brush (setColor / setBrushToolSize / setBrushOpacity /
// setBrushHardness / setPressureToSize / setPressureToOpacity). A regression
// there — a slider rebuilding the brush and clobbering fields, a default
// changing silently — passes the pixel lock cleanly and fails HERE.
//
// FIXTURE (all inputs pinned; NEVER rely on current defaults):
//   Slider state:  colour rgba(0,0,0,255), size 24 px, opacity 100 %,
//                  hardness 100 %, pressure-to-size ON, pressure-to-opacity
//                  ON — applied through the canvas's PUBLIC slots in this
//                  exact order (the pressure toggles last: they are the
//                  historical stroke-start resync trigger).
//   Stroke:        the canvas's OWN paintBrush() rendered over the
//                  SankoPaintPixelLock stroke — 33 points on the diagonal
//                  (100,100)->(860,440) at 960x540, pressure 1.0, seed 42,
//                  CPU render via SankoPaintHostAdapter::render().
//
// BASELINE: captured from the pre-Brush-Library slider pipeline (the
// syncPaintBrushSettings() rebuild path) and REQUIRED to survive the
// working-brush rework unchanged — same slider positions, same pixels,
// forever. If this hash ever changes, the canvas's brush assembly changed
// behaviour for existing users; that must be a deliberate decision, recorded
// by updating this constant IN THE SAME COMMIT as the behaviour change, with
// the reason in the commit message. Deleting the assertion is never the fix.
//
// Run: build/<config>/SankoCanvasBrushLock.exe (exit code = failure count).

#include "DrawingCanvas.h"
#include "SankoPaintHostAdapter.h"

#include <QApplication>
#include <QCryptographicHash>

#include <cstdio>
#include <memory>

// Captured from the pre-Brush-Library slider pipeline. It EQUALS the
// SankoPaintPixelLock R16 baseline, and that is expected and meaningful: at
// these slider values the canvas assembles exactly the pixel-lock fixture
// brush (size 24, opacity 1, hardness 1, spacing 0.05, linear size/opacity
// curves, flat hardness curve). The two tests still guard different code:
// the pixel lock cannot notice the canvas assembling a DIFFERENT brush from
// the same sliders — this test exists for precisely that failure.
static const char kBaselineAssembly[] =
    "666f7b455228e18020ad6b4967740de762e559140e8e52c98ddb415a9f91547a";

// SECOND FIXTURE (Phase 4): mid-range slider values — opacity 50 %,
// hardness 40 %, size 7 px, pressure toggles OFF. The first fixture pins
// opacity and hardness at their range ENDPOINTS, where almost any
// slider->field mapping produces the same result (s/100 and (s/100)^2 agree
// at 0 and 100). This fixture pins the interior of the mapping, so a
// nonlinear regression that still passes the endpoint fixture fails here.
// Captured from the Phase 3 working-brush pipeline; the same
// change-deliberately-and-record rule applies.
static const char kBaselineAssemblyMidRange[] =
    "cafcec7f7288c2ac29fb09a7bbf83de6cbcbf20c5cb5a66fba478770b94767df";

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

static QString renderWith(const ::Brush &brush)
{
    ::Brush b = brush;
    StrokeBuilder sb(QSize(960, 540), b, false, 42, 0);
    for (int i = 0; i <= 32; ++i) {
        StrokePoint p;
        p.position = QPointF(100 + i * (760.0 / 32), 100 + i * (340.0 / 32));
        p.pressure = 1.0;
        sb.addRawPoint(p);
    }
    SankoPaintHostAdapter::StrokeWork w;
    w.layerKey = QStringLiteral("assembly-fixture");
    w.canvasSize = QSize(960, 540);
    w.brush = b;
    w.rawPoints = sb.rawPoints();
    w.primaryStamps = sb.stamps();
    w.affectedRect = sb.affectedRect().intersected(QRect(0, 0, 960, 540));
    QImage before(w.affectedRect.size(), QImage::Format_ARGB32);
    before.fill(Qt::transparent);
    w.beforeRegion = before;
    w.seed = 42;
    w.preferGpu = false;
    StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush, before,
                                        w.affectedRect.topLeft());
    const auto result = SankoPaintHostAdapter::render(w);
    if (!result.succeeded)
        return QStringLiteral("RENDER-FAILED");
    return shaHex(result.afterRegion);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv); // DrawingCanvas is a QWidget

    int failures = 0;
    auto check = [&](const char *label, bool ok, const QString &d) {
        if (!ok)
            ++failures;
        fprintf(ok ? stdout : stderr, "%s %s   %s\n", ok ? "PASS" : "FAIL",
                label, qPrintable(d));
    };

    // Assemble the working brush EXCLUSIVELY through the canvas's public
    // slider slots, exactly as the Brush Options panel drives them.
    auto canvas = std::make_unique<DrawingCanvas>();
    canvas->setColor(QColor(0, 0, 0, 255));
    canvas->setBrushToolSize(24);
    canvas->setBrushOpacity(100);
    canvas->setBrushHardness(100);
    canvas->setPressureToSize(true);
    canvas->setPressureToOpacity(true);

    const QString h = renderWith(canvas->paintBrush());
    check("canvas-assembled brush renders the pinned baseline",
          h == QLatin1String(kBaselineAssembly), h);

    // Determinism of the assembly itself: a second canvas driven through the
    // same slots must produce the identical brush (byte-compared through the
    // render, which covers every field that can affect pixels).
    auto canvas2 = std::make_unique<DrawingCanvas>();
    canvas2->setColor(QColor(0, 0, 0, 255));
    canvas2->setBrushToolSize(24);
    canvas2->setBrushOpacity(100);
    canvas2->setBrushHardness(100);
    canvas2->setPressureToSize(true);
    canvas2->setPressureToOpacity(true);
    check("assembly is deterministic across canvas instances",
          renderWith(canvas2->paintBrush()) == h, h);

    // Mid-range fixture: interior slider values through the same public
    // slots (toggles last, OFF — the flat-curve branch of both toggles).
    auto canvas3 = std::make_unique<DrawingCanvas>();
    canvas3->setColor(QColor(0, 0, 0, 255));
    canvas3->setBrushToolSize(7);
    canvas3->setBrushOpacity(50);
    canvas3->setBrushHardness(40);
    canvas3->setPressureToSize(false);
    canvas3->setPressureToOpacity(false);
    const QString hm = renderWith(canvas3->paintBrush());
    check("mid-range slider values render the pinned baseline",
          hm == QLatin1String(kBaselineAssemblyMidRange), hm);

    return failures;
}
