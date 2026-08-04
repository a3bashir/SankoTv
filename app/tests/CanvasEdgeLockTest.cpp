// PERMANENT canvas-border lock (SankoCanvasEdgeLock).
//
// History this test exists to keep closed: strokes reached the document
// boundary in the LAYER (byte-proven against a padded reference) while the
// SCREEN still showed a pale one-pixel gap, twice, from two different
// display-side causes — the canvas frame's cosmetic pen centred ON the
// boundary path (half its width blended into the outermost row/column), and
// white paper leaking between separately-antialiased stacked content quads at
// alpha(1-alpha) weight. Both only appear when the boundary lands on a
// FRACTIONAL device coordinate, so every assertion here runs at fractional
// scales; integer-only zoom coverage already cost two investigation cycles.
//
// Method: lay a stroke along each document edge, capture the window with
// QScreen::grabWindow at NATIVE resolution (never QWidget::grab(): it
// substitutes a palette fill on translucent top-levels), and assert the
// outermost artwork device pixel. Aligned boundary => must equal the ink.
// Fractional boundary => must not exceed the brightest LEGITIMATE contributor
// (ink, gutter, the grid colour when visible) + 2. The permanent canvas frame
// was ultimately REMOVED: even seated outside the boundary, its antialiased
// fringe composited a constant pale column (measured 122,122,122 at 400% on
// the reporter's machine), so NO frame value is allowed here — the only
// permitted colours at the boundary are ink, gutter, and grid. A sample that
// falls outside the capture FAILS; skipping once hid a third of a run.
//
// Run: build/<config>/SankoCanvasEdgeLock.exe (exit code = failure count).
// Requires a desktop session (it measures real screen pixels).

#include "DrawingCanvas.h"
#include "SankoPaintHostAdapter.h"
#include "StoryboardModel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QTextStream>
#include <QTransform>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

qreal g_grabDpr = 1.0;
int g_failures = 0, g_checks = 0;

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void sendMouse(QWidget *w, QEvent::Type type, const QPointF &local, Qt::MouseButton b)
{
    QMouseEvent ev(type, local, w->mapToGlobal(local.toPoint()), b,
                   type == QEvent::MouseButtonRelease ? Qt::NoButton : b, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &ev);
}

QString rgb(QRgb c)
{
    return QStringLiteral("%1,%2,%3").arg(qRed(c), 3).arg(qGreen(c), 3).arg(qBlue(c), 3);
}

// grabWindow can transiently return an EMPTY pixmap (compositor hiccup, the
// window momentarily occluded or unmapped). That must never be treated as a
// capture: g_grabDpr would become img.width()/top->width() == 0, silently
// collapsing every sample coordinate to the origin and reporting the accurate
// but deeply misleading "SAMPLE OUT OF CAPTURE px=(0,-1)". Retry, and on
// persistent failure return a null image so the caller fails with a message
// that names the real problem.
QImage grabCanvas(QWidget *canvas)
{
    QWidget *top = canvas->window();
    if (top->width() <= 0 || top->height() <= 0)
        return QImage();
    QImage img;
    for (int attempt = 0; attempt < 6; ++attempt) {
        img = top->screen()->grabWindow(top->winId()).toImage();
        if (!img.isNull() && img.width() > 0 && img.height() > 0)
            break;
        pump(100);
    }
    if (img.isNull() || img.width() <= 0)
        return QImage();
    g_grabDpr = img.width() / qreal(top->width());
    const QPoint tl = canvas->mapTo(top, QPoint(0, 0));
    const QRect dev(QPoint(qRound(tl.x() * g_grabDpr), qRound(tl.y() * g_grabDpr)),
                    QSize(qRound(canvas->width() * g_grabDpr),
                          qRound(canvas->height() * g_grabDpr)));
    img = img.copy(dev.intersected(img.rect()));
    img.setDevicePixelRatio(1.0);
    return img;
}

struct Edge {
    const char *name;
    QPointF on, off;
    QPointF inward;
};

struct Workspace {
    QColor gutter;
    bool gridOn;
    QColor gridColor;
};

void checkBoundary(const QImage &shot, const QPointF &bw, const QPointF &inwardW,
                   const QColor &ink, const Workspace &ws, const QString &label)
{
    const qreal len = std::hypot(inwardW.x(), inwardW.y());
    const QPointF u = len > 0 ? inwardW / len : QPointF(1, 0);
    const QPointF bDev = bw * g_grabDpr;
    const QPointF s0 = bDev + u * 0.5; // centre of the outermost artwork pixel
    const QPoint p0(int(std::floor(s0.x())), int(std::floor(s0.y())));
    ++g_checks;
    if (shot.isNull()) {
        ++g_failures;
        out() << QStringLiteral("  **FAIL** %1 SCREEN CAPTURE FAILED (empty grab)")
                     .arg(label) << Qt::endl;
        return;
    }
    if (!shot.rect().contains(p0)) {
        ++g_failures;
        out() << QStringLiteral("  **FAIL** %1 SAMPLE OUT OF CAPTURE px=(%2,%3) "
                                "capture=%4x%5")
                     .arg(label).arg(p0.x()).arg(p0.y())
                     .arg(shot.width()).arg(shot.height()) << Qt::endl;
        return;
    }
    const QRgb got = shot.pixel(p0);
    const qreal fx = qAbs(u.x()) > qAbs(u.y()) ? bDev.x() - std::floor(bDev.x())
                                               : bDev.y() - std::floor(bDev.y());
    const bool aligned = fx < 0.001 || fx > 0.999;
    int ceiling = qMax(ink.red(), ws.gutter.red());
    if (ws.gridOn)
        ceiling = qMax(ceiling, ws.gridColor.red());
    const bool ok = aligned ? qAbs(qRed(got) - ink.red()) <= 1
                            : qRed(got) <= ceiling + 2;
    if (!ok)
        ++g_failures;
    out() << QStringLiteral("  %1 %2 got=%3 %4")
                 .arg(ok ? "PASS" : "**FAIL**", label, rgb(got),
                      aligned ? QStringLiteral("(aligned: ink %1)").arg(ink.red())
                              : QStringLiteral("(fractional: ceiling %1)").arg(ceiling + 2))
          << Qt::endl;
}

void strokeAlong(DrawingCanvas *canvas, Panel *panel, const QPointF &from,
                 const QPointF &to)
{
    const QTransform t = canvas->viewTransformForTest();
    const QPointF wOn = t.map(from), wOff = t.map(to);
    sendMouse(canvas, QEvent::MouseButtonPress, wOn, Qt::LeftButton);
    for (int i = 1; i <= 24; ++i)
        sendMouse(canvas, QEvent::MouseMove, wOn + (wOff - wOn) * (i / 24.0),
                  Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, wOff, Qt::LeftButton);
    pump(800);
    Q_UNUSED(panel);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QWidget window;
    // grabWindow captures the screen region of the window, including anything
    // stacked above it; staying on top keeps other windows out of the samples.
    window.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    // Odd size on purpose: the fit scale becomes an ugly fraction like a real
    // resized window, so the startup condition is genuinely fractional.
    window.resize(qMin(1147, avail.width() - 60), qMin(671, avail.height() - 80));
    auto *lay = new QVBoxLayout(&window);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *canvas = new DrawingCanvas(&window);
    lay->addWidget(canvas);

    QUndoStack stack;
    Panel *panel = makeBlankPanel();
    canvas->setUndoStack(&stack);
    canvas->setActivePanel(panel);
    canvas->setTool(DrawingCanvas::Brush);
    canvas->setGridEnabled(false);
    canvas->setCameraFrameEnabled(false);
    canvas->setSafeAreaEnabled(false);
    canvas->setTitleSafeEnabled(false);
    window.show();
    window.raise();
    window.activateWindow();
    pump(400);
    // Wait for the geometry to stop changing, then for the first composited
    // frame to actually reach the screen (the first grabs after show() can
    // return an all-white surface, which would read as white-gap failures).
    QSize stable = canvas->size();
    for (int settled = 0; settled < 3; ) {
        pump(120);
        if (canvas->size() == stable) { ++settled; continue; }
        stable = canvas->size();
        settled = 0;
    }
    for (int tries = 0; tries < 30; ++tries) {
        const QImage probe = grabCanvas(canvas);
        if (!probe.isNull() && qRed(probe.pixel(3, probe.height() / 2)) < 250)
            break;
        pump(150);
    }

    const QSize doc = DrawingCanvas::canvasSize();
    const double fit = qMin(canvas->width() / double(doc.width()),
                            canvas->height() / double(doc.height()));
    const double startupScale = fit * 0.85;
    out() << QStringLiteral("doc=%1x%2 widget=%3x%4 dpr=%5 startupScale=%6")
                 .arg(doc.width()).arg(doc.height())
                 .arg(canvas->width()).arg(canvas->height())
                 .arg(window.devicePixelRatioF()).arg(startupScale, 0, 'f', 6)
          << Qt::endl;

    const QVector<Edge> edges = {
        {"left",   QPointF(0, doc.height() * .35), QPointF(0, doc.height() * .65), QPointF(1, 0)},
        {"right",  QPointF(doc.width(), doc.height() * .35),
                   QPointF(doc.width(), doc.height() * .65), QPointF(-1, 0)},
        {"top",    QPointF(doc.width() * .33, 0), QPointF(doc.width() * .61, 0), QPointF(0, 1)},
        {"bottom", QPointF(doc.width() * .33, doc.height()),
                   QPointF(doc.width() * .61, doc.height()), QPointF(0, -1)},
    };
    const Workspace wsDark{QColor(0x0a, 0x0a, 0x0a), false, QColor(0x2a, 0x2a, 0x2a)};
    const Workspace wsUser{QColor(0x4d, 0x4d, 0x4d), true, QColor(0x6d, 0x6d, 0x6d)};
    const QColor dark(20, 20, 20), light(240, 240, 240);

    // --- paper coverage: no white pixel survives a full-document fill -------
    out() << "--- paper fully covered by artwork (white run within diff run) ---"
          << Qt::endl;
    canvas->setWorkspaceForTest(wsDark.gutter, false, DrawingCanvas::GridStyle::Square,
                                wsDark.gridColor);
    for (const double scale : {startupScale, 1.37, 4.0}) {
        canvas->placeViewForTest(scale, QPointF(doc.width() / 2.0, doc.height() / 2.0),
                                 QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
        panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
        canvas->update();
        pump(200);
        const QImage blank = grabCanvas(canvas);
        panel->layers[panel->activeLayerIndex].image.fill(QColor(20, 20, 20, 255));
        canvas->update();
        pump(200);
        const QImage filled = grabCanvas(canvas);
        panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
        canvas->update();

        // A null capture here would crash: constScanLine() on a null image is
        // undefined. Fail loudly instead.
        if (blank.isNull() || filled.isNull()) {
            ++g_checks; ++g_failures;
            out() << QStringLiteral("  **FAIL** scale=%1 SCREEN CAPTURE FAILED")
                         .arg(scale, 0, 'f', 4) << Qt::endl;
            continue;
        }
        const int midY = blank.height() / 2;
        int whiteLo = -1, whiteHi = -1, diffLo = -1, diffHi = -1;
        const QRgb *lb = reinterpret_cast<const QRgb *>(blank.constScanLine(midY));
        const QRgb *lf = reinterpret_cast<const QRgb *>(filled.constScanLine(midY));
        const int w = qMin(blank.width(), filled.width());
        for (int x = 0; x < w; ++x) {
            if (qRed(lb[x]) > 250 && qGreen(lb[x]) > 250) {
                if (whiteLo < 0) whiteLo = x;
                whiteHi = x;
            }
            if (qAbs(qRed(lb[x]) - qRed(lf[x])) > 10) {
                if (diffLo < 0) diffLo = x;
                diffHi = x;
            }
        }
        ++g_checks;
        const bool ok = whiteLo >= diffLo && whiteHi <= diffHi && whiteLo >= 0;
        if (!ok)
            ++g_failures;
        out() << QStringLiteral("  %1 scale=%2 paper x[%3..%4] covered x[%5..%6]")
                     .arg(ok ? "PASS" : "**FAIL**").arg(scale, 0, 'f', 4)
                     .arg(whiteLo).arg(whiteHi).arg(diffLo).arg(diffHi) << Qt::endl;
    }

    // --- edges under fractional scales, both workspaces, rotation ----------
    struct Cond {
        QString name;
        double scale, rotation;
        const Workspace *ws;
        QColor ink;
    };
    const QVector<Cond> conds = {
        {"startup fit*0.85, user workspace", startupScale, 0, &wsUser, dark},
        {"startup fit*0.85, dark gutter", startupScale, 0, &wsDark, dark},
        {"137%, user workspace", 1.37, 0, &wsUser, dark},
        {"137% rotated 30deg, dark gutter", 1.37, 30, &wsDark, dark},
        {"400%, user workspace", 4.0, 0, &wsUser, dark},
        {"400%, dark gutter, light ink", 4.0, 0, &wsDark, light},
    };
    for (const Cond &cond : conds) {
        out() << QStringLiteral("--- %1 ---").arg(cond.name) << Qt::endl;
        canvas->setWorkspaceForTest(cond.ws->gutter, cond.ws->gridOn,
                                    DrawingCanvas::GridStyle::Dashed, cond.ws->gridColor);
        canvas->setViewRotation(cond.rotation);
        canvas->setColor(cond.ink);
        for (const Edge &e : edges) {
            panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
            const QPointF mid = (e.on + e.off) / 2.0;
            canvas->placeViewForTest(cond.scale, mid,
                                     QPointF(canvas->width() / 2.0,
                                             canvas->height() / 2.0));
            pump(150);
            strokeAlong(canvas, panel, e.on, e.off);
            const QTransform t = canvas->viewTransformForTest();
            const QPointF bw = t.map(mid);
            checkBoundary(grabCanvas(canvas), bw, t.map(mid + e.inward * 8.0) - bw,
                          cond.ink, *cond.ws, QStringLiteral("%1 edge").arg(e.name));
        }
    }
    canvas->setViewRotation(0.0);

    // --- corners at 400% ----------------------------------------------------
    out() << "--- corners @400%, user workspace ---" << Qt::endl;
    canvas->setWorkspaceForTest(wsUser.gutter, true, DrawingCanvas::GridStyle::Dashed,
                                wsUser.gridColor);
    canvas->setColor(dark);
    for (const QPoint &corner : {QPoint(0, 0), QPoint(doc.width(), 0),
                                 QPoint(0, doc.height()), QPoint(doc.width(), doc.height())}) {
        panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
        canvas->placeViewForTest(4.0, QPointF(corner),
                                 QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
        pump(150);
        strokeAlong(canvas, panel, QPointF(corner), QPointF(corner) + QPointF(0.3, 0.3));
        const QTransform t = canvas->viewTransformForTest();
        const QPointF w = t.map(QPointF(corner));
        const QPointF inward(corner.x() == 0 ? 1.0 : -1.0, corner.y() == 0 ? 1.0 : -1.0);
        checkBoundary(grabCanvas(canvas), w, inward, dark, wsUser,
                      QStringLiteral("corner (%1,%2)").arg(corner.x()).arg(corner.y()));
    }

    // --- 400% boundary integrity: no pale column, ever ---------------------
    // The reporter's screenshot showed a constant 122,122,122 column between
    // gutter and artwork at 400%: the removed permanent frame outline. This
    // section pins its absence with pure-black diagonal strokes ENTERING from
    // outside the document on a near-black gutter: every device pixel across
    // the crossing must be as dark as ink or gutter (<= gutter+2). 42 (the
    // old frame), 122 (its fringe) and 255 (paper) all fail. The same stroke
    // is also re-rendered through the engine on a padded document and compared
    // byte-exactly over the shared region, so premature brush clipping cannot
    // hide behind a display that happens to look right.
    out() << "--- 400% boundary integrity (diagonal entry strokes) ---" << Qt::endl;
    canvas->setWorkspaceForTest(wsDark.gutter, false, DrawingCanvas::GridStyle::Square,
                                wsDark.gridColor);
    canvas->setViewRotation(0.0);
    const QColor black(0, 0, 0);
    canvas->setColor(black);
    struct Cross {
        const char *name;
        QPointF from, to;    // canvas-space diagonal, crossing the boundary
        QPointF crossing;    // the boundary point it crosses
        QPointF inward;      // direction into the document
        QPoint layerPx;      // document pixel that must be painted
        QMargins pad;        // reference-document padding for this case
    };
    const double W = doc.width(), H = doc.height();
    const QVector<Cross> crossings = {
        {"left edge",    {-40, 230},    {40, 310},     {0, 270},  {1, 0},
         QPoint(0, 270), QMargins(120, 0, 0, 0)},
        {"right edge",   {W + 40, 230}, {W - 40, 310}, {W, 270},  {-1, 0},
         QPoint(int(W) - 1, 270), QMargins(0, 0, 120, 0)},
        {"top edge",     {300, -40},    {380, 40},     {340, 0},  {0, 1},
         QPoint(340, 0), QMargins(0, 120, 0, 0)},
        {"bottom edge",  {300, H + 40}, {380, H - 40}, {340, H},  {0, -1},
         QPoint(340, int(H) - 1), QMargins(0, 0, 0, 120)},
        {"top-left",     {-40, -40},    {40, 40},      {0, 0},    {1, 1},
         QPoint(0, 0), QMargins(120, 120, 0, 0)},
        {"top-right",    {W + 40, -40}, {W - 40, 40},  {W, 0},    {-1, 1},
         QPoint(int(W) - 1, 0), QMargins(0, 120, 120, 0)},
        {"bottom-left",  {-40, H + 40}, {40, H - 40},  {0, H},    {1, -1},
         QPoint(0, int(H) - 1), QMargins(120, 0, 0, 120)},
        {"bottom-right", {W + 40, H + 40}, {W - 40, H - 40}, {W, H}, {-1, -1},
         QPoint(int(W) - 1, int(H) - 1), QMargins(0, 0, 120, 120)},
    };
    for (const Cross &c : crossings) {
        panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
        canvas->placeViewForTest(4.0, c.crossing,
                                 QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
        pump(150);
        strokeAlong(canvas, panel, c.from, c.to);

        // (1) the stroke, entered from OUTSIDE, reaches the document pixel.
        const QImage &li = panel->layers.at(panel->activeLayerIndex).image;
        const int alpha = qAlpha(li.pixel(c.layerPx));
        ++g_checks;
        if (alpha != 255) ++g_failures;
        out() << QStringLiteral("  %1 %2: entering stroke paints doc (%3,%4) alpha=%5")
                     .arg(alpha == 255 ? "PASS" : "**FAIL**", c.name)
                     .arg(c.layerPx.x()).arg(c.layerPx.y()).arg(alpha) << Qt::endl;

        // (2)(3)(4) the screen crossing: gutter outside, ink inside, and NO
        // pixel anywhere in the window brighter than gutter+2.
        const QImage shot = grabCanvas(canvas);
        const QTransform t = canvas->viewTransformForTest();
        const QPointF bw = t.map(c.crossing);
        const QPointF dir = t.map(c.crossing + c.inward * 8.0) - bw;
        const qreal len = std::hypot(dir.x(), dir.y());
        const QPointF u = len > 0 ? dir / len : QPointF(1, 0);
        const QPointF bDev = bw * g_grabDpr;
        QString seq;
        int worst = -1, worstStep = 0;
        QRgb outsidePx = 0, insidePx = 0;
        bool oob = false;
        for (int step = -3; step <= 3; ++step) {
            const QPointF sp = bDev + u * (step + 0.5);
            const QPoint px(int(std::floor(sp.x())), int(std::floor(sp.y())));
            if (!shot.rect().contains(px)) {
                oob = true;
                seq += QStringLiteral("%1[--OOB--] ").arg(step, 2);
                continue;
            }
            const QRgb v = shot.pixel(px);
            if (step == -1) outsidePx = v;
            if (step == 0) insidePx = v;
            const int bright = qMax(qMax(qRed(v), qGreen(v)), qBlue(v));
            if (bright > worst) { worst = bright; worstStep = step; }
            seq += QStringLiteral("%1[%2] ").arg(step, 2).arg(rgb(v));
        }
        const int gut = wsDark.gutter.red();
        const bool noPale = !oob && worst <= gut + 2;
        ++g_checks;
        if (!noPale) ++g_failures;
        out() << QStringLiteral("  %1 %2: no pale column (worst=%3 at step %4, limit %5)%6")
                     .arg(noPale ? "PASS" : "**FAIL**", c.name)
                     .arg(worst).arg(worstStep).arg(gut + 2)
                     .arg(oob ? " OOB" : "") << Qt::endl;
        out() << QStringLiteral("      %1").arg(seq) << Qt::endl;
        ++g_checks;
        const bool outOk = !oob && qAbs(qRed(outsidePx) - gut) <= 2;
        if (!outOk) ++g_failures;
        out() << QStringLiteral("  %1 %2: pixel outside is the gutter (%3)")
                     .arg(outOk ? "PASS" : "**FAIL**", c.name, rgb(outsidePx)) << Qt::endl;
        ++g_checks;
        const bool inOk = !oob && qRed(insidePx) <= 2 && qGreen(insidePx) <= 2
                          && qBlue(insidePx) <= 2;
        if (!inOk) ++g_failures;
        out() << QStringLiteral("  %1 %2: first artwork pixel is the ink (%3)")
                     .arg(inOk ? "PASS" : "**FAIL**", c.name, rgb(insidePx)) << Qt::endl;

        // (5) engine-level padded reference: the same diagonal on a document
        // grown past the crossed sides, cropped back — byte-identical, or the
        // brush was clipped prematurely.
        ::Brush probe;
        probe.setSize(48);
        probe.setHardness(0.9);
        probe.setSpacing(0.05);
        probe.setColor(black);
        auto render = [&](const QSize &sz, const QPointF &a, const QPointF &b) {
            QImage host(sz, QImage::Format_ARGB32);
            host.fill(Qt::transparent);
            SankoPaintHostAdapter adapter;
            adapter.setBrush(probe);
            adapter.synchronizeLayer(QStringLiteral("probe"), host);
            StrokePoint sp;
            sp.position = a;
            sp.pressure = 1.0;
            adapter.beginStroke(QStringLiteral("probe"), host, sp, 0x9e3779b9ULL, true);
            for (int i = 1; i <= 24; ++i) {
                StrokePoint q;
                q.position = a + (b - a) * (i / 24.0);
                q.pressure = 1.0;
                q.timestamp = quint64(i);
                adapter.appendPoint(q);
            }
            auto work = adapter.finishStrokeWork(false); // CPU: byte-deterministic
            auto result = SankoPaintHostAdapter::render(work);
            adapter.publish(result, host);
            return host;
        };
        const QImage clipped = render(doc, c.from, c.to);
        const QSize padSize(doc.width() + c.pad.left() + c.pad.right(),
                            doc.height() + c.pad.top() + c.pad.bottom());
        const QPointF shift(c.pad.left(), c.pad.top());
        const QImage padded = render(padSize, c.from + shift, c.to + shift);
        int diff = 0;
        for (int y = 0; y < doc.height() && diff == 0; ++y)
            for (int x = 0; x < doc.width(); ++x)
                if (clipped.pixel(x, y) != padded.pixel(x + int(shift.x()),
                                                        y + int(shift.y()))) {
                    ++diff;
                    break;
                }
        ++g_checks;
        if (diff != 0) ++g_failures;
        out() << QStringLiteral("  %1 %2: matches padded reference (engine, CPU)")
                     .arg(diff == 0 ? "PASS" : "**FAIL**", c.name) << Qt::endl;
    }

    // --- boundary undo/redo -------------------------------------------------
    out() << "--- undo/redo at the boundary ---" << Qt::endl;
    {
        stack.clear();
        panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
        const Edge &e = edges.at(0);
        canvas->placeViewForTest(startupScale, (e.on + e.off) / 2.0,
                                 QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
        pump(150);
        strokeAlong(canvas, panel, e.on, e.off);
        const QImage after = panel->layers.at(panel->activeLayerIndex).image.copy();
        ++g_checks;
        if (stack.count() != 1) ++g_failures;
        out() << QStringLiteral("  %1 one stroke -> one undo entry (count=%2)")
                     .arg(stack.count() == 1 ? "PASS" : "**FAIL**")
                     .arg(stack.count()) << Qt::endl;
        canvas->undo();
        pump(400);
        int left = 0;
        for (int y = 0; y < doc.height(); ++y)
            if (qAlpha(panel->layers.at(panel->activeLayerIndex).image.pixel(0, y)))
                ++left;
        ++g_checks;
        if (left != 0) ++g_failures;
        out() << QStringLiteral("  %1 undo clears the boundary column (%2 left)")
                     .arg(left == 0 ? "PASS" : "**FAIL**").arg(left) << Qt::endl;
        canvas->redo();
        pump(400);
        ++g_checks;
        const bool same = panel->layers.at(panel->activeLayerIndex).image == after;
        if (!same) ++g_failures;
        out() << QStringLiteral("  %1 redo restores boundary pixels byte-exactly")
                     .arg(same ? "PASS" : "**FAIL**") << Qt::endl;
    }

    out() << QStringLiteral("RESULT %1 (checks=%2 failures=%3)")
                 .arg(g_failures == 0 ? "PASS" : "FAIL")
                 .arg(g_checks).arg(g_failures)
          << Qt::endl;
    out().flush();
    return g_failures;
}
