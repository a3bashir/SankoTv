// PERMANENT variable-resolution lock (SankoCanvasSizeLock).
//
// Why this family exists: the other six permanent families all pin 960x540
// fixtures, so the gate they form is a regression gate at the LEGACY size
// only. The resolution epic (pixels win over the manifest, required-QSize
// factories, instance canvasSize()) and the performance pass (the
// self-validating flatten-thumb cache) were verified by large seams — but
// seams are archived, and archived evidence catches nothing in CI. This
// family puts the variable-size invariants in the gate: a regression at
// 1920x1080 or 3840x2160 fails HERE, not in a user's project.
//
// The trap this file is built to avoid is the vacuous shape: "assert a
// dimension and never draw" passes over a canvas that silently crops every
// stroke to a top-left 960x540. So EVERY size this family pins is also a
// size it DRAWS at, through the real synthetic-event path, asserting real
// pixels at the far edge — a stroke entered through the bottom-right
// corner must paint (W-1, H-1), and the same sampler first proves that
// pixel empty. Sizes: 960x540 (control — this family must agree with the
// legacy gate where they overlap), 1920x1080, 2048x1080 (width exactly
// 8x256: the right edge sits ON a tile boundary), 3840x2160, and 777x1013
// (portrait, odd, tile-misaligned — the shape every 16:9 assumption
// breaks on).
//
// Sections:
//   (a) size authority: layers, canvasSize(), flatten — at all five sizes
//   (b) far-edge strokes with pre-stroke positive controls — all five
//   (c) undo/redo byte-exact across a 256px tile boundary — all five
//   (d) persistence: byte-identical load, second-save fixed point, PNGs on
//       disk at panel size, plus a comparator control that must FAIL —
//       at 960x540, 1920x1080, 777x1013 (presence bugs, not magnitude
//       bugs: the large sizes add PNG-encode cost without adding a
//       distinct failure mode)
//   (e) migration locks: keyless legacy at 960x540; ancient single-PNG
//       panels; the lying manifest opens at PIXEL size with the mismatch
//       dialog's EXACT string (asserted against an independent literal);
//       deferred fill of a missing PNG at the PANEL's size
//   (f) cross-size staleness (selection mask / mirror / composite carry
//       nothing across setActivePanel) + the flatten-thumb cache at
//       777x1013 (byte-identical to an uncached recompute; one edit
//       invalidates)
//   (g) project settings (promoted from the Part 1 seam, the two checks
//       that catch a real regression): the frame rate RE-DERIVES the
//       animatic's per-block frame counts (24/30/60 fps -> 240/300/600
//       frames for 10 s of panels — reading the stored rate back would be
//       vacuous), and the Project Settings dialog keeps edits PENDING
//       (editing emits nothing, Cancel emits nothing, Apply emits once and
//       stays open, OK emits once and closes) — a future convenience that
//       applies on edit or on Cancel would silently mutate the project.
//
// Scope note, stated rather than implied: this family drives DrawingCanvas
// and ProjectIO directly; it does NOT construct StoryboardPage, so the
// page's button wiring (Clear/duplicate/add) is covered by the required-
// QSize factory signatures the compiler enforces plus the archived 3a
// seam, not re-proven here.
//
// Scratch discipline: QSettings redirected to INI under a scratch root,
// QStandardPaths test mode, every file under the scratch root, removed and
// asserted at exit — this runs on real machines forever.
//
// Run: build/<config>/SankoCanvasSizeLock.exe (exit code = failure count).
// Needs a GUI session (widgets + synthetic events) but never samples
// screen pixels — no grabWindow, none of EdgeLock's capture sensitivity.

#include "AnimaticTimeline.h"
#include "DrawingCanvas.h"
#include "ProjectIO.h"
#include "ProjectSettingsDialog.h"
#include "StoryboardModel.h"
#include "brushlib/StudioControls.h"

#include <QApplication>
#include <QPushButton>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

int g_checks = 0, g_failures = 0;

void check(const QString &label, bool ok, const QString &detail = QString())
{
    ++g_checks;
    if (!ok)
        ++g_failures;
    out() << QStringLiteral("  %1 %2%3")
                 .arg(ok ? "PASS" : "**FAIL**", label,
                      detail.isEmpty() ? QString()
                                       : QStringLiteral(" [%1]").arg(detail))
          << Qt::endl;
}

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

// The pump after release is CORRECTNESS, not padding: the stroke publishes
// asynchronously, and asserting layer bytes before the publish lands would
// fail a healthy canvas. Trim runtime by dropping SIZES, never by
// shortening this.
void strokeAlong(DrawingCanvas *canvas, const QPointF &from, const QPointF &to)
{
    const QTransform t = canvas->viewTransformForTest();
    const QPointF wOn = t.map(from), wOff = t.map(to);
    sendMouse(canvas, QEvent::MouseButtonPress, wOn, Qt::LeftButton);
    for (int i = 1; i <= 24; ++i)
        sendMouse(canvas, QEvent::MouseMove, wOn + (wOff - wOn) * (i / 24.0),
                  Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, wOff, Qt::LeftButton);
    pump(900);
}

// Deterministic content (seeded LCG, never rand()): the fixture must be
// identical on every machine and every run.
quint32 g_seed = 0x9e3779b9u;
quint32 nextRand()
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed;
}
int randIn(int lo, int hi) { return lo + int(nextRand() % quint32(hi - lo + 1)); }

void paintDetail(QImage &img, int elements)
{
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int w = img.width(), h = img.height();
    for (int i = 0; i < elements; ++i) {
        const QColor c(randIn(0, 255), randIn(0, 255), randIn(0, 255),
                       randIn(120, 255));
        if (i % 3 == 0) {
            p.setPen(QPen(c, randIn(2, 14)));
            p.setBrush(Qt::NoBrush);
            p.drawLine(randIn(0, w), randIn(0, h), randIn(0, w), randIn(0, h));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QPoint(randIn(0, w), randIn(0, h)),
                          randIn(4, w / 8), randIn(4, h / 8));
        }
    }
}

Panel *makeDetailedPanel(const QSize &S)
{
    Panel *panel = makeBlankPanel(S);
    paintDetail(panel->layers[1].image, 160);
    return panel;
}

QByteArray fileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

bool writeJson(const QString &path, const QJsonObject &root)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QJsonObject readJson(const QString &path)
{
    return QJsonDocument::fromJson(fileBytes(path)).object();
}

void freeScenes(QVector<Scene *> &scenes)
{
    for (Scene *s : scenes)
        delete s;
    scenes.clear();
}

// ---- (a)(b)(c): authority + far-edge pixels + undo, one size ---------------
void runSizePass(const QSize &S, DrawingCanvas *canvas, QUndoStack *stack)
{
    const int W = S.width(), H = S.height();
    out() << QStringLiteral("--- (a)(b)(c) %1x%2 ---").arg(W).arg(H) << Qt::endl;
    const QPointF wc(canvas->width() / 2.0, canvas->height() / 2.0);

    stack->clear();
    Panel *panel = makeBlankPanel(S);

    // (a) authority
    bool sizesOk = panel->layers.size() == 2;
    for (const Layer &l : panel->layers)
        sizesOk = sizesOk && l.image.size() == S;
    check(QStringLiteral("(a) fresh panel layers are %1x%2").arg(W).arg(H), sizesOk);
    canvas->setActivePanel(panel);
    pump(100);
    check(QStringLiteral("(a) canvas->canvasSize() reports the panel"),
          canvas->canvasSize() == S);
    check(QStringLiteral("(a) flattenedPixmap() is panel-sized"),
          panel->flattenedPixmap().size() == S);

    // (b) far-edge stroke, with the pre-stroke control on the same sampler
    canvas->setTool(DrawingCanvas::Brush);
    canvas->setColor(QColor(20, 20, 20));
    const QImage &li = panel->layers.at(panel->activeLayerIndex).image;
    check(QStringLiteral("(b) control: corner (%1,%2) empty before the stroke")
              .arg(W - 1).arg(H - 1),
          qAlpha(li.pixel(W - 1, H - 1)) == 0);
    canvas->placeViewForTest(4.0, QPointF(W, H), wc);
    pump(120);
    strokeAlong(canvas, QPointF(W + 30, H + 30), QPointF(W - 40, H - 40));
    check(QStringLiteral("(b) corner stroke paints (%1,%2)").arg(W - 1).arg(H - 1),
          qAlpha(li.pixel(W - 1, H - 1)) == 255,
          QStringLiteral("alpha=%1").arg(qAlpha(li.pixel(W - 1, H - 1))));
    check(QStringLiteral("(b) flatten carries the corner ink (no crop)"),
          qGray(panel->flattenedPixmap().toImage().pixel(W - 1, H - 1)) < 128);

    // (c) tile-boundary stroke, undo/redo byte-exact
    panel->layers[panel->activeLayerIndex].image.fill(Qt::transparent);
    canvas->setActivePanel(panel); // full resync after the direct fill
    stack->clear();
    canvas->placeViewForTest(1.0, QPointF(260, 260), wc);
    pump(120);
    const QImage blank = panel->layers.at(panel->activeLayerIndex).image.copy();
    strokeAlong(canvas, QPointF(200, 220), QPointF(320, 300));
    const QImage after = panel->layers.at(panel->activeLayerIndex).image.copy();
    check(QStringLiteral("(c) tile stroke painted + one undo entry"),
          stack->count() == 1 && after != blank);
    canvas->undo();
    pump(300);
    check(QStringLiteral("(c) undo restores blank byte-exactly"),
          panel->layers.at(panel->activeLayerIndex).image == blank);
    canvas->redo();
    pump(300);
    check(QStringLiteral("(c) redo restores the stroke byte-exactly"),
          panel->layers.at(panel->activeLayerIndex).image == after);

    canvas->setActivePanel(nullptr);
    delete panel;
}

// ---- (d): persistence at one size ------------------------------------------
void runPersistencePass(const QSize &S, const QString &projRoot)
{
    const int W = S.width(), H = S.height();
    out() << QStringLiteral("--- (d) persistence %1x%2 ---").arg(W).arg(H)
          << Qt::endl;
    const QString folder1 = projRoot + QStringLiteral("/p_%1x%2_a").arg(W).arg(H);
    const QString folder2 = projRoot + QStringLiteral("/p_%1x%2_b").arg(W).arg(H);
    QDir().mkpath(folder1);
    QDir().mkpath(folder2);

    Scene *scene = new Scene;
    scene->number = 1;
    // Explicit fields: the load path normalizes an empty timeOfDay to
    // "UNSPECIFIED", which would break the fixed point for a reason
    // unrelated to size.
    scene->location = QStringLiteral("INT. LOCK");
    scene->timeOfDay = QStringLiteral("DAY");
    scene->action = QStringLiteral("Persist.");
    scene->panels.append(makeDetailedPanel(S));
    scene->panels.append(makeBlankPanel(S));

    ProjectIO::SaveData d1;
    d1.projectName = QStringLiteral("SizeLock %1x%2").arg(W).arg(H);
    d1.fps = 24;
    d1.canvasSize = S;
    d1.scenes = {scene};
    const QJsonObject root1 = ProjectIO::projectToJson(d1, folder1);
    writeJson(folder1 + QStringLiteral("/proj.sankotv"), root1);

    ProjectIO::LoadedProject L = ProjectIO::projectFromJson(
        readJson(folder1 + QStringLiteral("/proj.sankotv")), folder1);
    check(QStringLiteral("(d) load: pixelSize honest, no mismatch"),
          L.pixelSize == S && !L.mismatch && L.manifestSize == S);
    bool layersEqual = L.scenes.size() == 1 && L.scenes.at(0)->panels.size() == 2;
    if (layersEqual) {
        for (int p = 0; p < 2 && layersEqual; ++p) {
            const Panel *orig = scene->panels.at(p);
            const Panel *got = L.scenes.at(0)->panels.at(p);
            layersEqual = orig->layers.size() == got->layers.size();
            for (int k = 0; layersEqual && k < orig->layers.size(); ++k)
                layersEqual = orig->layers.at(k).image == got->layers.at(k).image;
        }
    }
    check(QStringLiteral("(d) every layer byte-identical after load"), layersEqual);

    // Comparator control: the equality above must be ABLE to fail. Modify
    // one pixel of a loaded layer and demand inequality — a comparator that
    // cannot see this would make the previous PASS decorative.
    if (L.scenes.size() == 1) {
        QImage &img = L.scenes[0]->panels[0]->layers[1].image;
        img.setPixel(W / 2, H / 2, qRgba(1, 2, 3, 255));
        check(QStringLiteral("(d) control: comparator detects a 1px change"),
              img != scene->panels.at(0)->layers.at(1).image);
        img.setPixel(W / 2, H / 2,
                     scene->panels.at(0)->layers.at(1).image.pixel(W / 2, H / 2));
    }

    check(QStringLiteral("(d) flatten PNGs on disk are %1x%2").arg(W).arg(H),
          QImage(folder1 + QStringLiteral("/panel_s0_p0.png")).size() == S
              && QImage(folder1 + QStringLiteral("/panel_s0_p1.png")).size() == S);

    ProjectIO::SaveData d2;
    d2.projectName = L.projectName;
    d2.fps = L.fps;
    d2.canvasSize = L.pixelSize;
    d2.scenes = L.scenes;
    d2.consistency = L.consistency;
    d2.audioPath = L.audioPath;
    d2.perspective = L.perspective;
    const QJsonObject root2 = ProjectIO::projectToJson(d2, folder2);
    check(QStringLiteral("(d) second save: identical JSON"),
          QJsonDocument(root1).toJson() == QJsonDocument(root2).toJson());
    bool pngsEqual = true;
    const QStringList pngs =
        QDir(folder1).entryList({QStringLiteral("*.png")}, QDir::Files);
    for (const QString &name : pngs)
        pngsEqual = pngsEqual
            && fileBytes(folder1 + QStringLiteral("/") + name)
                == fileBytes(folder2 + QStringLiteral("/") + name);
    check(QStringLiteral("(d) second save: identical PNG bytes (%1 files)")
              .arg(pngs.size()),
          pngsEqual && !pngs.isEmpty());

    freeScenes(L.scenes);
    delete scene;
}

// ---- (e): migration locks ---------------------------------------------------
void runMigrationPass(const QString &projRoot)
{
    out() << "--- (e) migration / mismatch locks ---" << Qt::endl;
    Panel *panel = makeBlankPanel(QSize(960, 540));
    {
        QPainter p(&panel->layers[1].image);
        p.fillRect(100, 80, 300, 200, QColor(200, 30, 30));
        p.fillRect(700, 400, 200, 100, QColor(30, 60, 200));
    }
    Scene *scene = new Scene;
    scene->number = 1;
    scene->panels.append(panel);

    const QString folder = projRoot + QStringLiteral("/migrate");
    QDir().mkpath(folder);
    ProjectIO::SaveData d;
    d.projectName = QStringLiteral("Migrate");
    d.fps = 24;
    d.canvasSize = QSize(960, 540);
    d.scenes = {scene};
    QJsonObject root = ProjectIO::projectToJson(d, folder);

    // Keyless legacy: silent, 960x540, artwork untouched.
    QJsonObject legacyRoot = root;
    legacyRoot.remove(QStringLiteral("canvasWidth"));
    legacyRoot.remove(QStringLiteral("canvasHeight"));
    ProjectIO::LoadedProject Lk = ProjectIO::projectFromJson(legacyRoot, folder);
    check(QStringLiteral("(e) keyless legacy opens at 960x540, silent"),
          Lk.pixelSize == QSize(960, 540) && !Lk.mismatch);
    check(QStringLiteral("(e) legacy artwork byte-identical"),
          Lk.scenes.size() == 1
              && Lk.scenes.at(0)->panels.at(0)->layers.at(1).image
                  == panel->layers.at(1).image);
    freeScenes(Lk.scenes);

    // Ancient single-PNG panel shape: flatten becomes Layer 1, all 960x540.
    QJsonObject ancientRoot = legacyRoot;
    {
        QJsonArray scenes = ancientRoot.value(QStringLiteral("scenes")).toArray();
        QJsonObject sc = scenes.at(0).toObject();
        QJsonArray panels = sc.value(QStringLiteral("panels")).toArray();
        QJsonObject p0 = panels.at(0).toObject();
        p0.remove(QStringLiteral("layers"));
        panels[0] = p0;
        sc[QStringLiteral("panels")] = panels;
        scenes[0] = sc;
        ancientRoot[QStringLiteral("scenes")] = scenes;
    }
    ProjectIO::LoadedProject La = ProjectIO::projectFromJson(ancientRoot, folder);
    bool ancientOk = La.pixelSize == QSize(960, 540) && !La.mismatch
        && La.scenes.size() == 1;
    if (ancientOk)
        for (const Layer &l : La.scenes.at(0)->panels.at(0)->layers)
            ancientOk = ancientOk
                && (isGroupLayer(l) || l.image.size() == QSize(960, 540));
    check(QStringLiteral("(e) single-PNG legacy migrates at 960x540"), ancientOk);
    freeScenes(La.scenes);

    // Lying manifest: pixels win, dialog string EXACT, next save honest.
    QJsonObject lyingRoot = root;
    lyingRoot[QStringLiteral("canvasWidth")] = 1920;
    lyingRoot[QStringLiteral("canvasHeight")] = 1080;
    ProjectIO::LoadedProject Ll = ProjectIO::projectFromJson(lyingRoot, folder);
    check(QStringLiteral("(e) mismatch flagged; opens at PIXEL size"),
          Ll.mismatch && Ll.pixelSize == QSize(960, 540)
              && Ll.manifestSize == QSize(1920, 1080));
    check(QStringLiteral("(e) artwork byte-identical (never rescaled)"),
          Ll.scenes.size() == 1
              && Ll.scenes.at(0)->panels.at(0)->layers.at(1).image
                  == panel->layers.at(1).image);
    // The user-facing sentence, locked against an INDEPENDENT literal — the
    // shared function compared to itself would be the vacuous shape.
    const QString expected = QString::fromUtf8(
        "This project's file says 1920 \xC3\x97 1080, but its artwork is "
        "960 \xC3\x97 540.\n\nOpening at 960 \xC3\x97 540. Your artwork "
        "is not modified; the file will be corrected on the next save.");
    check(QStringLiteral("(e) mismatch dialog text is the exact string"),
          canvasMismatchDialogText(Ll.manifestSize, Ll.pixelSize) == expected);
    const QString folderFix = projRoot + QStringLiteral("/migrate_fix");
    QDir().mkpath(folderFix);
    ProjectIO::SaveData dFix;
    dFix.projectName = Ll.projectName;
    dFix.fps = Ll.fps;
    dFix.canvasSize = Ll.pixelSize;
    dFix.scenes = Ll.scenes;
    const QJsonObject rootFix = ProjectIO::projectToJson(dFix, folderFix);
    ProjectIO::LoadedProject Lf = ProjectIO::projectFromJson(rootFix, folderFix);
    check(QStringLiteral("(e) next save writes the honest manifest"),
          rootFix.value(QStringLiteral("canvasWidth")).toInt() == 960
              && rootFix.value(QStringLiteral("canvasHeight")).toInt() == 540
              && !Lf.mismatch);
    freeScenes(Lf.scenes);
    freeScenes(Ll.scenes);

    // Deferred fill at the PANEL's size — with a control proving the layer
    // had content before its PNG went missing.
    const QString folderTall = projRoot + QStringLiteral("/tall");
    QDir().mkpath(folderTall);
    Panel *tall = makeDetailedPanel(QSize(777, 1013));
    Scene *tallScene = new Scene;
    tallScene->number = 1;
    tallScene->panels.append(tall);
    ProjectIO::SaveData dt;
    dt.projectName = QStringLiteral("Tall");
    dt.fps = 24;
    dt.canvasSize = QSize(777, 1013);
    dt.scenes = {tallScene};
    const QJsonObject tallRoot = ProjectIO::projectToJson(dt, folderTall);
    {
        ProjectIO::LoadedProject pre = ProjectIO::projectFromJson(tallRoot, folderTall);
        const QImage &before = pre.scenes.at(0)->panels.at(0)->layers.at(1).image;
        bool hadContent = false;
        for (int y = 0; y < before.height() && !hadContent; y += 16)
            for (int x = 0; x < before.width() && !hadContent; x += 16)
                hadContent = qAlpha(before.pixel(x, y)) != 0;
        check(QStringLiteral("(e) control: layer HAS content before deletion"),
              hadContent);
        freeScenes(pre.scenes);
    }
    QFile::remove(folderTall + QStringLiteral("/panel_s0_p0_layer1.png"));
    ProjectIO::LoadedProject Lt = ProjectIO::projectFromJson(tallRoot, folderTall);
    const QImage &refilled = Lt.scenes.at(0)->panels.at(0)->layers.at(1).image;
    check(QStringLiteral("(e) missing PNG refilled transparent at 777x1013"),
          refilled.size() == QSize(777, 1013)
              && qAlpha(refilled.pixel(0, 0)) == 0
              && qAlpha(refilled.pixel(776, 1012)) == 0);
    freeScenes(Lt.scenes);
    delete tallScene;
    delete scene; // owns `panel`
}

// ---- (f): cross-size staleness + the thumb cache ----------------------------
void runStalenessPass(DrawingCanvas *canvas, QUndoStack *stack)
{
    out() << "--- (f) cross-size staleness + thumb cache ---" << Qt::endl;
    const QPointF wc(canvas->width() / 2.0, canvas->height() / 2.0);

    stack->clear();
    Panel *small = makeBlankPanel(QSize(960, 540));
    Panel *tall = makeBlankPanel(QSize(777, 1013));
    canvas->setActivePanel(small);
    canvas->setTool(DrawingCanvas::Brush);
    canvas->setColor(QColor(10, 10, 10));
    canvas->placeViewForTest(1.0, QPointF(480, 270), wc);
    pump(120);
    canvas->selectAll(); // leaves a 960x540 selection mask cache behind
    strokeAlong(canvas, QPointF(100, 100), QPointF(400, 300));
    check(QStringLiteral("(f) stroke inside the old selection landed"),
          qAlpha(small->layers.at(small->activeLayerIndex).image.pixel(250, 200)) > 0);
    canvas->setActivePanel(tall);
    pump(120);
    check(QStringLiteral("(f) canvas reports the new size after the switch"),
          canvas->canvasSize() == QSize(777, 1013));
    canvas->selectAll(); // must rebuild the mask at 777x1013
    canvas->placeViewForTest(4.0, QPointF(777, 1013), wc);
    pump(120);
    strokeAlong(canvas, QPointF(777 + 30, 1013 + 30), QPointF(777 - 40, 1013 - 40));
    check(QStringLiteral("(f) corner stroke lands at (776,1012) after the "
                         "cross-size switch"),
          qAlpha(tall->layers.at(tall->activeLayerIndex).image.pixel(776, 1012))
              == 255);
    check(QStringLiteral("(f) flatten sizes stay honest"),
          small->flattenedPixmap().size() == QSize(960, 540)
              && tall->flattenedPixmap().size() == QSize(777, 1013));
    canvas->setActivePanel(nullptr);

    // Thumb cache at a non-legacy size: identical to an uncached recompute,
    // and one edit invalidates. Without these two checks the 3b cache has
    // NO CI coverage at any size but 960x540.
    Panel *detail = makeDetailedPanel(QSize(777, 1013));
    auto reference = [](const Panel *p) {
        const QPixmap full = p->flattenedPixmap();
        return ((full.width() > 512 || full.height() > 512)
                    ? full.scaled(512, 512, Qt::KeepAspectRatio,
                                  Qt::SmoothTransformation)
                    : full)
            .toImage();
    };
    check(QStringLiteral("(f) flattenedThumb == uncached recompute at 777x1013"),
          detail->flattenedThumb().toImage() == reference(detail));
    const QImage cachedBefore = detail->flattenedThumb().toImage();
    {
        QPainter p(&detail->layers[1].image);
        p.fillRect(50, 60, 300, 260, QColor(20, 180, 60));
    }
    const QImage cachedAfter = detail->flattenedThumb().toImage();
    check(QStringLiteral("(f) an edit invalidates the thumb, result == fresh"),
          cachedAfter != cachedBefore && cachedAfter == reference(detail));

    delete small;
    delete tall;
    delete detail;
}

// ---- (g): project settings — fps re-derives timing; dialog edits pend ----
void runProjectSettingsPass()
{
    out() << "--- (g) project settings: fps timing + pending dialog ---" << Qt::endl;
    // FPS -> DERIVED frame counts (10 s of panels: 2 + 3 + 5).
    Scene *sc = new Scene;
    sc->number = 1;
    for (int dur : {2, 3, 5}) {
        Panel *p = makeBlankPanel(QSize(960, 540));
        p->duration = dur;
        sc->panels.append(p);
    }
    {
        AnimaticTimeline tl;
        tl.setFps(24);
        tl.setScenes({sc});
        check(QStringLiteral("(g) 24 fps -> 240 derived frames"),
              tl.totalFramesForTest() == 240,
              QStringLiteral("%1").arg(tl.totalFramesForTest()));
        tl.setFps(30);
        check(QStringLiteral("(g) 30 fps -> 300 (re-derived, not stored)"),
              tl.totalFramesForTest() == 300,
              QStringLiteral("%1").arg(tl.totalFramesForTest()));
        tl.setFps(60);
        check(QStringLiteral("(g) 60 fps -> 600 (re-derived)"),
              tl.totalFramesForTest() == 600,
              QStringLiteral("%1").arg(tl.totalFramesForTest()));
    }
    delete sc;

    // The dialog's pending contract.
    auto fpsIndexOf = [](ProjectSettingsDialog &d, int fps) {
        for (int i = 0; i < 8; ++i) {
            d.fpsDropdown()->setCurrentIndex(i);
            if (d.fps() == fps)
                return i;
        }
        return -1;
    };
    {
        ProjectSettingsDialog d(QStringLiteral("Alpha"), 24, QSize(1920, 1080));
        int applies = 0;
        QObject::connect(&d, &ProjectSettingsDialog::applied, &d,
                         [&](const QString &, int) { ++applies; });
        d.show();
        pump(80);
        d.nameField()->setText(QStringLiteral("Beta"));
        emit d.nameField()->textEdited(QStringLiteral("Beta"));
        d.fpsDropdown()->choose(fpsIndexOf(d, 30));
        pump(30);
        check(QStringLiteral("(g) editing the dialog applies nothing (pending)"),
              applies == 0 && d.projectName() == QStringLiteral("Beta")
                  && d.fps() == 30);
        d.cancelButton()->click();
        pump(30);
        check(QStringLiteral("(g) Cancel applies nothing and closes"),
              applies == 0 && d.result() == QDialog::Rejected && !d.isVisible());
    }
    {
        ProjectSettingsDialog d(QStringLiteral("Alpha"), 24, QSize(1920, 1080));
        int applies = 0;
        QString lastName;
        int lastFps = 0;
        QObject::connect(&d, &ProjectSettingsDialog::applied, &d,
                         [&](const QString &n, int f) {
                             ++applies;
                             lastName = n;
                             lastFps = f;
                         });
        d.show();
        pump(80);
        d.nameField()->setText(QStringLiteral("Gamma"));
        emit d.nameField()->textEdited(QStringLiteral("Gamma"));
        d.fpsDropdown()->choose(fpsIndexOf(d, 60));
        d.applyButton()->click();
        pump(30);
        check(QStringLiteral("(g) Apply applies once and stays open"),
              applies == 1 && lastName == QStringLiteral("Gamma") && lastFps == 60
                  && d.isVisible() && d.result() != QDialog::Accepted);
        d.fpsDropdown()->choose(fpsIndexOf(d, 25));
        d.okButton()->click();
        pump(30);
        check(QStringLiteral("(g) OK applies the newer value once and closes"),
              applies == 2 && lastFps == 25 && d.result() == QDialog::Accepted
                  && !d.isVisible());
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SankoTV"));
    QApplication::setOrganizationName(QStringLiteral("Sanko"));

    QElapsedTimer total;
    total.start();

    const QString scratch =
        QDir::tempPath() + QStringLiteral("/sanko_canvas_size_lock");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch + QStringLiteral("/settings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       scratch + QStringLiteral("/settings"));
    QStandardPaths::setTestModeEnabled(true);
    const QString projRoot = scratch + QStringLiteral("/projects");
    QDir().mkpath(projRoot);

    {
        QWidget window;
        window.resize(1147, 671);
        auto *lay = new QVBoxLayout(&window);
        lay->setContentsMargins(0, 0, 0, 0);
        auto *canvas = new DrawingCanvas(&window);
        lay->addWidget(canvas);
        QUndoStack stack;
        canvas->setUndoStack(&stack);
        canvas->setGridEnabled(false);
        canvas->setCameraFrameEnabled(false);
        canvas->setSafeAreaEnabled(false);
        canvas->setTitleSafeEnabled(false);
        window.show();
        pump(400);

        const QVector<QSize> allSizes = {
            {960, 540}, {1920, 1080}, {2048, 1080}, {3840, 2160}, {777, 1013}};
        for (const QSize &s : allSizes)
            runSizePass(s, canvas, &stack);

        // (d) at three sizes: presence bugs, not magnitude bugs — the large
        // sizes add PNG-encode seconds without a distinct failure mode.
        for (const QSize &s :
             {QSize(960, 540), QSize(1920, 1080), QSize(777, 1013)})
            runPersistencePass(s, projRoot);

        runMigrationPass(projRoot);
        runStalenessPass(canvas, &stack);
        window.close();
        pump(100);
    }
    runProjectSettingsPass();

    const bool removed = QDir(scratch).removeRecursively();
    check(QStringLiteral("scratch root removed cleanly"),
          removed && !QDir(scratch).exists());

    out() << QStringLiteral("RESULT %1 (checks=%2 failures=%3) in %4 s")
                 .arg(g_failures == 0 ? "PASS" : "FAIL")
                 .arg(g_checks).arg(g_failures)
                 .arg(total.elapsed() / 1000.0, 0, 'f', 1)
          << Qt::endl;
    out().flush();
    return g_failures;
}
