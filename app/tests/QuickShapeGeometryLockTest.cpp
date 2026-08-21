// SankoTV QuickShape GEOMETRY lock — PERMANENT, ships with the code.
//
// Raster preview-vs-commit equality once passed while Edit Shape conversions
// were geometrically WRONG: preview and commit consumed the same too-sparse
// replay stream, so the pixels agreed with each other while both disagreed
// with the displayed editing guide (the stage-6 integration defect, found on
// a physical Cintiq). This test locks the invariant those tests cannot see:
//
//   the canonical Edit Shape geometry, the displayed guide, and the EXACT
//   point stream submitted to the engine replay all describe the same path.
//
// Per vertex shape (Triangle, Rectangle, Polygon 3/4/5/6/8):
//   * displayed node count equals the selected count
//   * every replay sample lies on one of the displayed edges
//     (centerline tolerance 0.5 document px)
//   * every displayed vertex is reached exactly by a sample
//   * samples traverse the nodes in the same cyclic order, close exactly
//     once, and contain NO interior/diagonal segment
//   * the stream is DENSE: no gap between consecutive samples exceeds
//     kMaxSampleGap — sparse corner-only streams render wrongly because the
//     engine's causal input smoothing (StrokeBuilder::addRawPoint) blends
//     across corners; density is what keeps that displacement sub-pixel
// Conversion matrix: Ellipse -> each vertex type/count; Triangle ->
// Rectangle; Rectangle -> Polygon 6; Polygon 6 -> 8 -> 6; ROTATED Ellipse ->
// Rectangle; rotated Rectangle -> Polygon 6.
// Async staleness: Triangle, Rectangle, Polygon 6 requested back-to-back —
// only the final geometry may become the settled preview.
// Plus one preview-vs-commit byte-equality round on the converted shape
// (both families are required; neither alone caught the stage-6 defect).
//
// Run: build/<config>/SankoQuickShapeGeometryLock.exe (exit = failures).

#include "DrawingCanvas.h"
#include "StoryboardModel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLineF>
#include <QPointingDevice>
#include <QPushButton>
#include <QTabletEvent>

#include <cmath>
#include <cstdio>
#include <functional>

namespace {

int g_fail = 0;

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_fail;
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A WALL-CLOCK budget is meaningless on a starved event loop: what is being
// waited for here is asynchronous (a concurrent preview render, a UI rebuild),
// so when the machine gives this process a fraction of a core the budget can
// expire before the work has been SCHEDULED, let alone finished — which is how
// a loaded, correct machine reported "preview settles" as a failure. The
// budget is therefore spent in RESPONSIVE time: a turn of the loop that costs
// far more than it asked for bought no progress, so that time is not charged
// against the deadline. A quiet machine is unaffected; a loaded one waits
// longer for the same amount of real progress, bounded by a hard ceiling so a
// genuinely hung condition still fails instead of hanging.
bool waitFor(const std::function<bool()> &cond, int timeoutMs)
{
    constexpr qint64 kTurnBudgetMs = 25; // a turn asks for 10 ms; allow slack
    const qint64 hardCapMs = qint64(timeoutMs) * 10 + 5000;
    QElapsedTimer t;
    t.start();
    qint64 stalled = 0;
    while (t.elapsed() - stalled < timeoutMs && t.elapsed() < hardCapMs) {
        if (cond())
            return true;
        const qint64 before = t.elapsed();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        const qint64 turn = t.elapsed() - before;
        if (turn > kTurnBudgetMs)
            stalled += turn - kTurnBudgetMs;
    }
    return cond();
}

const QPointingDevice *stylusDevice()
{
    static QPointingDevice dev(
        QStringLiteral("lock stylus"), 1, QInputDevice::DeviceType::Stylus,
        QPointingDevice::PointerType::Pen,
        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure,
        1, 1);
    return &dev;
}

void sendTablet(QWidget *w, QEvent::Type type, const QPointF &local,
                qreal pressure)
{
    QTabletEvent ev(type, stylusDevice(), local,
                    w->mapToGlobal(local.toPoint()), pressure, 0.0, 0.0, 0.0,
                    0.0, 0.0, Qt::NoModifier, Qt::LeftButton,
                    type == QEvent::TabletRelease ? Qt::NoButton
                                                  : Qt::LeftButton);
    QCoreApplication::sendEvent(w, &ev);
}

// Hold-to-shape is a WALL-CLOCK product behaviour, driven here through a
// SYNTHETIC event stream — and that combination, not the product, is what made
// "circle recognized" flake when runs were launched back-to-back on a loaded
// machine (measured 5/10 failures back-to-back, 0/16 spaced 2 s apart):
//
//   * one starved pump between two circle samples can outlast the production
//     hold, so the hold timer fires in the MIDDLE of the circle. The velocity
//     gate reads zero precisely BECAUSE starvation delivered no sample for
//     >160 ms, so recognition then runs on a partial ARC, not the circle;
//   * the wait for the final hold was a flat 3 s of WALL clock, which a
//     starved event loop can burn through before the hold timer is serviced.
//
// What the test asserts is unchanged and in fact tightened. The hold that
// produces the shape is still the one DrawingCanvas installs at press — it is
// captured, not hardcoded, so the product owns the timing under test. Only the
// SYNTHETIC gesture adapts: while the circle is being fed the hold interval is
// raised out of reach, so starvation cannot mis-time it; the instant the last
// point is delivered the production timing is restored, which restarts the
// hold timer (Qt restarts an active timer when its interval changes) and
// begins a genuine rest of the product's own duration, with the pen stationary
// and the whole circle collected. The wait for that rest is sized from the
// latency actually OBSERVED while feeding, not a constant. A disturbed attempt
// is retried rather than scored, and the recognized shape is now NAMED, so a
// premature arc can no longer pass as "circle recognized".
constexpr int kDrawGuardHoldMs = 60000; // no starved pump reaches this
constexpr int kCircleAttempts = 4;

// The recognizer names a closed round shape "Circle" (aspect < 1.25) or
// "Ellipse"; a partial sweep is "Arc" / "Elliptical Arc". Only the former is
// the gesture this test drives.
bool isClosedRoundShape(const QString &name)
{
    return name == QLatin1String("Circle") || name == QLatin1String("Ellipse");
}

bool driveCircleOnce(DrawingCanvas *canvas, QString *why)
{
    // WIDGET coordinates: the canvas converts to document space internally,
    // so the permanent test needs no private transform access.
    const QPointF centre(canvas->width() * 0.5, canvas->height() * 0.5);
    const qreal r = qMin(canvas->width(), canvas->height()) * 0.30;
    QVector<QPointF> path;
    for (int i = 0; i <= 56; ++i) {
        const qreal a = -M_PI / 2.0 + 2.0 * M_PI * i / 56;
        path.append(centre + QPointF(std::cos(a) * r, std::sin(a) * r));
    }

    QString recognized;
    QObject probe;
    QObject::connect(canvas->quickShape(),
                     &quickshape::QuickShapeSession::shapeRecognized, &probe,
                     [&recognized](const quickshape::QuickShapeResult &result) {
                         recognized = result.name;
                     });

    sendTablet(canvas, QEvent::TabletPress, path.first(), 1.0);
    // The canvas installs the product timing on press (applyQuickShapeTiming);
    // capture it so the hold under test stays the product's own.
    const quickshape::QuickShapeTiming production =
        canvas->quickShape()->timing();
    quickshape::QuickShapeTiming guard = production;
    guard.holdDurationMs = kDrawGuardHoldMs;
    canvas->quickShape()->setTiming(guard);

    QElapsedTimer feed;
    feed.start();
    qint64 prev = 0, worstGap = 0;
    bool activeDuringFeed = false;
    for (const QPointF &pt : path) {
        sendTablet(canvas, QEvent::TabletMove, pt, 1.0);
        pump(4);
        const qint64 now = feed.elapsed();
        worstGap = qMax(worstGap, now - prev);
        prev = now;
        if (canvas->quickShape()->hasActiveShape())
            activeDuringFeed = true;
    }

    // Restoring the production timing restarts the hold timer: the rest begins
    // HERE, once, with the whole circle collected and the pen stationary.
    canvas->quickShape()->setTiming(production);
    const qint64 budget =
        production.holdDurationMs * 3 + worstGap * 10 + 2000;
    QElapsedTimer since;
    since.start();
    waitFor([&] {
        if (canvas->quickShape()->hasActiveShape())
            return true;
        if (since.elapsed() >= 16) { // a resting pen keeps reporting, at a
            since.restart();         // device-like cadence, not every turn
            sendTablet(canvas, QEvent::TabletMove, path.last(), 1.0);
        }
        return canvas->quickShape()->hasActiveShape();
    }, int(budget));
    sendTablet(canvas, QEvent::TabletRelease, path.last(), 0.0);
    pump(30);

    if (activeDuringFeed) {
        *why = QStringLiteral("recognition fired while the circle was still "
                              "being fed (worst feed gap %1 ms)").arg(worstGap);
        return false;
    }
    if (!canvas->quickShape()->hasActiveShape()) {
        *why = QStringLiteral("no recognition within %1 ms (hold %2 ms, worst "
                              "feed gap %3 ms)")
                   .arg(budget).arg(production.holdDurationMs).arg(worstGap);
        return false;
    }
    if (!isClosedRoundShape(recognized)) {
        *why = QStringLiteral("recognized '%1', not a closed round shape")
                   .arg(recognized);
        return false;
    }
    return true;
}

bool driveCircle(DrawingCanvas *canvas, Panel *panel)
{
    QString why;
    for (int attempt = 1; attempt <= kCircleAttempts; ++attempt) {
        if (driveCircleOnce(canvas, &why))
            return true;
        std::printf("     circle attempt %d disturbed: %s\n", attempt,
                    why.toUtf8().constData());
        std::fflush(stdout);
        // Discard the disturbed attempt COMPLETELY: any half-built shape, the
        // session's pointer/collect state, and the freehand pixels the aborted
        // stroke baked into the layer on release. The next attempt then starts
        // from the same blank page the first one did.
        canvas->cancelQuickShape();
        canvas->quickShape()->reset();
        pump(120);
        panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
        canvas->update();
        pump(250);
    }
    std::printf("     circle NOT recognized in %d attempts: %s\n",
                kCircleAttempts, why.toUtf8().constData());
    std::fflush(stdout);
    return false;
}

QPushButton *canvasButton(DrawingCanvas *canvas, const QString &text)
{
    QPushButton *found = nullptr;
    const auto buttons = canvas->findChildren<QPushButton *>();
    for (QPushButton *b : buttons)
        if (b->text() == text && b->isVisible())
            found = b; // LAST match: rebuilds leave deleteLater ghosts first
    return found;
}

// The shape type bar is rebuilt asynchronously after a recognition or a
// conversion, so "the button is not there yet" and "the button is missing" are
// different things — and on a loaded machine a flat settle turned the first
// into the second, scoring every remaining conversion as a geometry failure.
// Wait (in responsive time) for the control to exist before clicking it; what
// happens once it is clicked is unchanged.
bool clickCanvasButton(DrawingCanvas *canvas, const QString &text)
{
    QPushButton *b = nullptr;
    waitFor([&] { return (b = canvasButton(canvas, text)) != nullptr; }, 4000);
    if (!b)
        return false;
    b->click();
    pump(60);
    return true;
}

constexpr qreal kOnEdgeTolerance = 0.5; // document px, centerline
constexpr qreal kMaxSampleGap = 8.0;    // document px between replay samples
constexpr qreal kCornerHit = 0.5;       // a sample must sit ON each vertex

struct GeometryVerdict
{
    bool ok = true;
    QString detail;
};

// Verify the replay stream against the displayed canonical nodes.
GeometryVerdict verifyVertexShape(DrawingCanvas *canvas, int expectedNodes,
                                  bool closed)
{
    GeometryVerdict v;
    const auto geometry = canvas->quickShapeCanonicalGeometryForTest();
    const QVector<QPointF> nodes = geometry.nodes;
    if (nodes.size() != expectedNodes) {
        v.ok = false;
        v.detail = QStringLiteral("node count %1 != %2")
                       .arg(nodes.size()).arg(expectedNodes);
        return v;
    }
    const QVector<StrokePoint> stream = canvas->quickShapeReplayStreamForTest();
    if (stream.size() < expectedNodes + 1) {
        v.ok = false;
        v.detail = QStringLiteral("stream too short: %1").arg(stream.size());
        return v;
    }
    const int n = nodes.size();
    const int edges = closed ? n : n - 1;

    // 1. every sample on a displayed edge; classify each sample's edge.
    QVector<int> sampleEdge(stream.size(), -1);
    qreal worstOff = 0.0;
    for (int s = 0; s < stream.size(); ++s) {
        qreal best = 1e9;
        int bestEdge = -1;
        for (int e = 0; e < edges; ++e) {
            const QLineF edge(nodes.at(e), nodes.at((e + 1) % n));
            // distance from point to SEGMENT
            const QPointF ap = stream.at(s).position - edge.p1();
            const QPointF ab = edge.p2() - edge.p1();
            const qreal len2 = ab.x() * ab.x() + ab.y() * ab.y();
            const qreal t = len2 > 0
                ? qBound(0.0, (ap.x() * ab.x() + ap.y() * ab.y()) / len2, 1.0)
                : 0.0;
            const QPointF proj = edge.p1() + ab * t;
            const qreal d = QLineF(stream.at(s).position, proj).length();
            if (d < best) {
                best = d;
                bestEdge = e;
            }
        }
        worstOff = qMax(worstOff, best);
        sampleEdge[s] = bestEdge;
    }
    if (worstOff > kOnEdgeTolerance) {
        v.ok = false;
        v.detail = QStringLiteral("sample off displayed edges by %1 px")
                       .arg(worstOff);
        return v;
    }

    // 2. every vertex reached exactly by some sample.
    for (int i = 0; i < n; ++i) {
        bool hit = false;
        for (const StrokePoint &p : stream)
            if (QLineF(p.position, nodes.at(i)).length() <= kCornerHit) {
                hit = true;
                break;
            }
        if (!hit) {
            v.ok = false;
            v.detail = QStringLiteral("vertex %1 never reached").arg(i);
            return v;
        }
    }

    // 3. density: no sparse jumps (the engine smoothing bound).
    for (int s = 1; s < stream.size(); ++s) {
        const qreal gap =
            QLineF(stream.at(s - 1).position, stream.at(s).position).length();
        if (gap > kMaxSampleGap) {
            v.ok = false;
            v.detail = QStringLiteral("sparse gap %1 px at sample %2 "
                                      "(engine smoothing would cut corners)")
                           .arg(gap).arg(s);
            return v;
        }
    }

    // 4. cyclic order + every edge represented + closes exactly once + no
    //    interior segment: edge indices must advance monotonically through
    //    0..edges-1 (allowing dwell on an edge), covering all edges, and
    //    wrap at most at the very end (the closing sample).
    int covered = 0;
    int current = sampleEdge.first();
    if (current != 0) {
        v.ok = false;
        v.detail = QStringLiteral("stream starts on edge %1").arg(current);
        return v;
    }
    covered = 1;
    for (int s = 1; s < stream.size(); ++s) {
        const int e = sampleEdge.at(s);
        if (e == current)
            continue;
        // corner samples sit on two edges; edge classification may flip to
        // the NEXT edge only.
        if (e == current + 1 || (closed && current == edges - 1 && e == 0
                                 && s >= stream.size() - 2)) {
            current = e;
            ++covered;
            continue;
        }
        v.ok = false;
        v.detail = QStringLiteral("edge order break: %1 -> %2 at sample %3 "
                                  "(interior/diagonal or reordered segment)")
                       .arg(current).arg(e).arg(s);
        return v;
    }
    // the closing sample may re-classify onto edge 0; edges-1 must be seen.
    if (covered < edges) {
        v.ok = false;
        v.detail = QStringLiteral("only %1 of %2 edges represented")
                       .arg(covered).arg(edges);
        return v;
    }
    return v;
}

bool convertAndVerify(DrawingCanvas *canvas, const QString &button,
                      int expectedNodes, const char *label)
{
    if (!clickCanvasButton(canvas, button)) {
        std::printf("FAIL %s (button '%s' missing)\n", label,
                    button.toUtf8().constData());
        ++g_fail;
        return false;
    }
    const GeometryVerdict v = verifyVertexShape(canvas, expectedNodes, true);
    if (!v.ok)
        std::printf("     %s: %s\n", label, v.detail.toUtf8().constData());
    check(v.ok, label);
    return v.ok;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    auto *canvas = new DrawingCanvas;
    canvas->resize(1390, 782);
    canvas->show();
    Panel *panel = makeBlankPanel(QSize(960, 540));
    canvas->setActivePanel(panel);
    pump(200);
    canvas->paintBrush().setSize(24);

    // ---- recognize a circle, enter Edit Shape ------------------------------
    check(driveCircle(canvas, panel), "circle recognized");
    if (!canvas->quickShape()->hasActiveShape())
        return g_fail + 1;
    clickCanvasButton(canvas, QStringLiteral("Edit Shape"));

    // ---- Ellipse -> every vertex type and count ----------------------------
    convertAndVerify(canvas, QStringLiteral("Triangle"), 3,
                     "Ellipse -> Triangle: guide == replay");
    clickCanvasButton(canvas, QStringLiteral("Ellipse"));
    convertAndVerify(canvas, QStringLiteral("Rectangle"), 4,
                     "Ellipse -> Rectangle: guide == replay");
    clickCanvasButton(canvas, QStringLiteral("Ellipse"));
    clickCanvasButton(canvas, QStringLiteral("Polygon"));
    convertAndVerify(canvas, QStringLiteral("3"), 3,
                     "Polygon 3: guide == replay");
    convertAndVerify(canvas, QStringLiteral("4"), 4,
                     "Polygon 4: guide == replay");
    convertAndVerify(canvas, QStringLiteral("5"), 5,
                     "Polygon 5: guide == replay");
    convertAndVerify(canvas, QStringLiteral("6"), 6,
                     "Polygon 6: guide == replay");
    convertAndVerify(canvas, QStringLiteral("8"), 8,
                     "Polygon 8: guide == replay");

    // ---- cross-conversions -------------------------------------------------
    convertAndVerify(canvas, QStringLiteral("Triangle"), 3,
                     "Polygon 8 -> Triangle");
    convertAndVerify(canvas, QStringLiteral("Rectangle"), 4,
                     "Triangle -> Rectangle");
    clickCanvasButton(canvas, QStringLiteral("Polygon"));
    convertAndVerify(canvas, QStringLiteral("6"), 6, "Rectangle -> Polygon 6");
    convertAndVerify(canvas, QStringLiteral("8"), 8, "Polygon 6 -> Polygon 8");
    convertAndVerify(canvas, QStringLiteral("6"), 6, "Polygon 8 -> Polygon 6");

    // ---- rotated frames ----------------------------------------------------
    {
        auto g = canvas->quickShape()->currentGeometry();
        using G = quickshape::QuickShapeGeometry;
        G rotated;
        rotated.kind = G::Ellipse;
        rotated.name = QStringLiteral("Ellipse");
        rotated.center = QPointF(430, 240);
        rotated.radiusX = 150;
        rotated.radiusY = 90;
        rotated.rotationRad = 0.5;
        rotated.startAngleRad = -M_PI / 2.0;
        rotated.spanAngleRad = 2.0 * M_PI;
        canvas->quickShape()->setEditedGeometry(rotated);
        pump(80);
        // the type bar rebuild keys off the canvas geometry; go through the
        // canvas conversion path via the buttons after re-entering the bar
        convertAndVerify(canvas, QStringLiteral("Rectangle"), 4,
                         "ROTATED Ellipse -> Rectangle: guide == replay");
        clickCanvasButton(canvas, QStringLiteral("Polygon"));
        convertAndVerify(canvas, QStringLiteral("6"), 6,
                         "rotated Rectangle -> Polygon 6: guide == replay");
    }

    // ---- async staleness: only the LAST conversion becomes visible ---------
    {
        // Calibrate the budget for the race below on THIS machine. What the
        // race waits for is a CONCURRENT preview render, and that render
        // competes for CPU with everything else running; a constant wall-clock
        // budget is therefore a statement about the machine, not about the
        // product, and on a loaded one it expired while a correct render was
        // still queued. (Responsive-time accounting does not help here: the
        // main loop stays responsive while the WORKER thread is the one being
        // starved.) So time one settle first and scale the race's budget from
        // it — a quiet machine keeps the original 8 s.
        // How slowly is this machine running the loop right now? Cheap, and it
        // still says something useful when the settle probe below cannot.
        QElapsedTimer probe;
        probe.start();
        pump(50);
        const qreal stretch = qMax(1.0, probe.elapsed() / 50.0);

        QElapsedTimer calibration;
        calibration.start();
        const bool calibrated = waitFor([canvas] {
            return !canvas->quickShapePreviewBusyForTest()
                   && !canvas->quickShapePreviewForTest().isNull();
        }, 5000); // calibration only: never sit here long enough to matter
        const qint64 oneSettleMs =
            calibrated ? qMax<qint64>(1, calibration.elapsed()) : 0;
        const int raceBudgetMs = int(qBound(
            qint64(8000), qMax(oneSettleMs * 12, qint64(8000 * stretch)),
            qint64(60000)));

        clickCanvasButton(canvas, QStringLiteral("Triangle"));
        // no settle waits: fire the next conversions immediately
        if (QPushButton *b = canvasButton(canvas, QStringLiteral("Rectangle")))
            b->click();
        if (QPushButton *b = canvasButton(canvas, QStringLiteral("Polygon")))
            b->click();
        if (QPushButton *b = canvasButton(canvas, QStringLiteral("6")))
            b->click();
        const bool settled = waitFor([canvas] {
            return !canvas->quickShapePreviewBusyForTest()
                   && !canvas->quickShapePreviewForTest().isNull();
        }, raceBudgetMs);
        if (!settled)
            std::printf("     preview never settled in %d ms (one settle "
                        "measured %lld ms): busy=%d previewNull=%d\n",
                        raceBudgetMs, oneSettleMs,
                        int(canvas->quickShapePreviewBusyForTest()),
                        int(canvas->quickShapePreviewForTest().isNull()));
        check(settled, "stale-race: preview settles");
        check(canvas->quickShapeCanonicalGeometryForTest().nodes.size() == 6,
              "stale-race: final geometry is Polygon 6");
        // The settled preview must equal a FRESH render of the current
        // (Polygon 6) commit — an accepted stale Triangle/Rectangle result
        // would differ. Compare via the replay stream identity: re-render by
        // committing to the blank layer and matching bytes.
        Layer &layer = panel->layers[panel->activeLayerIndex];
        const QImage blank = layer.image.copy();
        const QImage preview = canvas->quickShapePreviewForTest()
                                   .convertToFormat(
                                       QImage::Format_ARGB32_Premultiplied);
        canvas->commitQuickShape();
        waitFor([canvas] { return true; }, 100);
        pump(600);
        const bool baked = layer.image != blank;
        check(baked, "stale-race: commit landed");
        check(preview == layer.image.convertToFormat(
                             QImage::Format_ARGB32_Premultiplied),
              "stale-race + byte-equality: settled preview IS the Polygon 6 "
              "commit (no stale result accepted; pixels equal)");
    }

    delete canvas;
    delete panel;
    std::printf("FAILURES %d\n", g_fail);
    return g_fail;
}
