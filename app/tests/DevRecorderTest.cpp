// Tests for the TEMPORARY developer recorder (app/src/devrecorder/).
// Pattern follows SankoPaintPixelLock: a console exe, one PASS/FAIL line per
// check, exit code = failure count.
// Run: build/<config>/SankoDevRecorderTest.exe

#include "devrecorder/DevRecorder.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>

#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *name, const QString &info = {})
{
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                info.isEmpty() ? "" : "  ", qPrintable(info));
    if (!ok)
        ++failures;
}

static void settle(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QLabel window(QStringLiteral("devrec test window"));
    window.setObjectName(QStringLiteral("devrecTestWindow"));
    window.resize(400, 300);
    window.show();
    settle(300);

    const QString root =
        QDir::tempPath() + QStringLiteral("/sankotv_devrec_test");
    QDir(root).removeRecursively();

    auto *rec = devrec::Recorder::instance();
    rec->initialize(&window,
                    {QStringLiteral("DevRecorderTest"), QStringLiteral("1"),
                     QStringLiteral("test"), QStringLiteral("none")});
    rec->setOutputRoot(root);
    rec->setCaptureIntervalMs(200);
    rec->setDownscaleFactor(1);
    rec->setStateProvider([] {
        return QVariantMap{{QStringLiteral("tool"), QStringLiteral("Brush")},
                           {QStringLiteral("size"), 24}};
    });

    // --- 1. session folder creation and structure ------------------------
    rec->startRecording();
    const QString dir = rec->sessionDir();
    settle(600); // at least two interval captures land
    // synthetic input for the event log
    for (int i = 0; i < 10; ++i) {
        QMouseEvent mv(QEvent::MouseMove, QPointF(10 + i, 20),
                       window.mapToGlobal(QPoint(10 + i, 20)), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &mv);
    }
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(30, 30),
                      window.mapToGlobal(QPoint(30, 30)), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &press);

    // --- 3. Issue Happened marker ---------------------------------------
    const double beforeMark = 0.0; // markers carry their own t
    rec->markIssue(QStringLiteral("test marker note"));
    settle(300);
    rec->stopRecording();

    check(!dir.isEmpty() && QDir(dir).exists(), "session folder created",
          dir);
    check(QFile::exists(dir + QStringLiteral("/events.jsonl")),
          "events.jsonl exists");
    check(QFile::exists(dir + QStringLiteral("/system.txt")),
          "system.txt exists");
    check(QFile::exists(dir + QStringLiteral("/summary.txt")),
          "summary.txt exists");
    const QStringList shots =
        QDir(dir + QStringLiteral("/screenshots"))
            .entryList(QDir::Files, QDir::Name);
    check(shots.size() >= 2, "screenshots captured",
          QStringLiteral("count=%1").arg(shots.size()));

    // --- 2. records parseable; ordering by seq ---------------------------
    QFile ev(dir + QStringLiteral("/events.jsonl"));
    ev.open(QIODevice::ReadOnly | QIODevice::Text);
    qint64 lastSeq = -1;
    int parsed = 0, mouseMoves = 0, markers = 0;
    double markerT = -1.0;
    bool ordered = true, allParse = true;
    while (!ev.atEnd()) {
        const QByteArray line = ev.readLine().trimmed();
        if (line.isEmpty())
            continue;
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            allParse = false;
            continue;
        }
        ++parsed;
        const QJsonObject o = doc.object();
        const qint64 seq = qint64(o.value(QLatin1String("seq")).toDouble());
        if (seq <= lastSeq)
            ordered = false;
        lastSeq = seq;
        const QString type = o.value(QLatin1String("type")).toString();
        if (type == QLatin1String("mouseMove"))
            ++mouseMoves;
        if (type == QLatin1String("marker")) {
            ++markers;
            markerT = o.value(QLatin1String("t")).toDouble();
        }
    }
    check(allParse && parsed > 10, "all records parse as JSON",
          QStringLiteral("records=%1").arg(parsed));
    check(ordered, "sequence numbers strictly increasing");
    check(mouseMoves >= 10, "synthetic mouse events recorded",
          QStringLiteral("moves=%1").arg(mouseMoves));
    check(markers == 1 && markerT > beforeMark, "marker with sane timestamp",
          QStringLiteral("t=%1").arg(markerT));

    // --- marker appears in summary with context --------------------------
    QFile sum(dir + QStringLiteral("/summary.txt"));
    sum.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString summary = QString::fromUtf8(sum.readAll());
    check(summary.contains(QStringLiteral("ISSUE MARKERS (1)"))
              && summary.contains(QStringLiteral("test marker note"))
              && summary.contains(QStringLiteral("last events before")),
          "marker + context in summary.txt");

    // --- 6. capture targets the app window only --------------------------
    bool sizeOk = !shots.isEmpty();
    if (!shots.isEmpty()) {
        const QImage img(dir + QStringLiteral("/screenshots/")
                         + shots.first());
        const qreal dpr = window.devicePixelRatio();
        // window-sized (within DPR rounding), never screen-sized
        sizeOk = qAbs(img.width() - qRound(window.width() * dpr)) <= 4
            && qAbs(img.height() - qRound(window.height() * dpr)) <= 40;
    }
    check(sizeOk, "capture is window-sized (never the desktop)");

    // --- 4. bounded queue drop policy counts ------------------------------
    rec->setQueueCapacity(16);
    rec->startRecording();
    for (int i = 0; i < 500; ++i) { // flood without letting the drain run
        QMouseEvent mv(QEvent::MouseMove, QPointF(1 + (i % 50), 5),
                       window.mapToGlobal(QPoint(1, 5)), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &mv);
    }
    settle(200);
    rec->stopRecording();
    const QString dir2 = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                              QDir::Name)
                             .constLast();
    QFile sum2(root + QLatin1Char('/') + dir2
               + QStringLiteral("/summary.txt"));
    sum2.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString summary2 = QString::fromUtf8(sum2.readAll());
    bool droppedCounted = false;
    for (const QString &line : summary2.split(QLatin1Char('\n')))
        if (line.startsWith(QStringLiteral("records: "))) {
            const qint64 n =
                line.mid(9, line.indexOf(QStringLiteral("   ")) - 9)
                    .toLongLong();
            droppedCounted = n > 0;
        }
    check(droppedCounted, "overflow drops are counted in the summary");

    // --- 5. graceful stop keeps every buffered record ---------------------
    rec->setQueueCapacity(20000);
    rec->startRecording();
    const QString dir3 = rec->sessionDir();
    for (int i = 0; i < 200; ++i) {
        QMouseEvent mv(QEvent::MouseMove, QPointF(2 + (i % 60), 8),
                       window.mapToGlobal(QPoint(2, 8)), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &mv);
    }
    rec->stopRecording(); // immediate stop: buffered records must survive
    QFile ev3(dir3 + QStringLiteral("/events.jsonl"));
    ev3.open(QIODevice::ReadOnly | QIODevice::Text);
    int moves3 = 0;
    while (!ev3.atEnd())
        if (ev3.readLine().contains("\"mouseMove\""))
            ++moves3;
    check(moves3 >= 200, "graceful stop loses no buffered records",
          QStringLiteral("moves=%1").arg(moves3));

    // --- wheel, camera, classifier (logging-gap coverage) -----------------
    struct Cam
    {
        double zoom = 1.0;
        int visX = 0;
    } cam;
    rec->setCameraProvider([&cam]() -> QVariantMap {
        return {{QStringLiteral("zoom"), cam.zoom},
                {QStringLiteral("scalePx"), cam.zoom},
                {QStringLiteral("visX"), cam.visX},
                {QStringLiteral("visY"), 0},
                {QStringLiteral("visW"), 400},
                {QStringLiteral("visH"), 300},
                {QStringLiteral("canvasW"), 960},
                {QStringLiteral("canvasH"), 540},
                {QStringLiteral("paperOnScreen"), cam.zoom > 0.0}};
    });
    rec->setPressClassifier([](QWidget *, const QPoint &) -> QVariantMap {
        return {{QStringLiteral("grabAccepted"), true},
                {QStringLiteral("hitRegion"), QStringLiteral("strip")}};
    });
    rec->startRecording();
    const QString dir4 = rec->sessionDir();
    settle(100);
    // wheel event
    QWheelEvent wheel(QPointF(50, 50), window.mapToGlobal(QPoint(50, 50)),
                      QPoint(0, 0), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&window, &wheel);
    // classified press/release (attribution input for the camera change)
    QMouseEvent p4(QEvent::MouseButtonPress, QPointF(40, 40),
                   window.mapToGlobal(QPoint(40, 40)), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &p4);
    QMouseEvent r4(QEvent::MouseButtonRelease, QPointF(40, 40),
                   window.mapToGlobal(QPoint(40, 40)), Qt::LeftButton,
                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &r4);
    cam.visX = 200; // camera pan: change-driven record expected
    settle(400);
    cam.zoom = 0.0; // degenerate: zero scale + paper off-screen
    settle(400);
    rec->stopRecording();
    QFile ev4(dir4 + QStringLiteral("/events.jsonl"));
    ev4.open(QIODevice::ReadOnly | QIODevice::Text);
    bool wheelSeen = false, camChange = false, camBeforeAfter = false;
    bool degenerate = false, attributed = false, classified = false;
    bool dragMovedSeen = false;
    while (!ev4.atEnd()) {
        const QJsonObject o =
            QJsonDocument::fromJson(ev4.readLine()).object();
        const QString type = o.value(QLatin1String("type")).toString();
        if (type == QLatin1String("wheel")
            && o.value(QLatin1String("angleY")).toInt() == 120)
            wheelSeen = true;
        if (type == QLatin1String("mousePress")
            && o.value(QLatin1String("grabAccepted")).toBool()
            && o.value(QLatin1String("hitRegion")).toString()
                   == QLatin1String("strip"))
            classified = true;
        if (type == QLatin1String("mouseRelease")
            && o.contains(QLatin1String("dragMoved")))
            dragMovedSeen = true;
        if (type == QLatin1String("cameraChange")) {
            camChange = true;
            const QJsonObject before =
                o.value(QLatin1String("before")).toObject();
            const QJsonObject after =
                o.value(QLatin1String("after")).toObject();
            if (before.value(QLatin1String("visX")).toInt() == 0
                && after.value(QLatin1String("visX")).toInt() == 200)
                camBeforeAfter = true;
            if (o.contains(QLatin1String("DEGENERATE")))
                degenerate = true;
            if (o.value(QLatin1String("lastInput"))
                    .toObject()
                    .value(QLatin1String("type"))
                    .toString()
                    .startsWith(QLatin1String("mouse")))
                attributed = true;
        }
    }
    check(wheelSeen, "wheel event recorded with delta");
    check(camChange && camBeforeAfter,
          "camera change emitted on change with before/after");
    check(degenerate, "degenerate camera state flagged");
    check(attributed, "camera change attributed to last input");
    check(classified && dragMovedSeen,
          "press classified; release carries dragMoved");
    // summary states the build config prominently
    QFile sum4(dir4 + QStringLiteral("/summary.txt"));
    sum4.open(QIODevice::ReadOnly | QIODevice::Text);
    check(QString::fromUtf8(sum4.readAll())
              .contains(QStringLiteral("BUILD: test")),
          "summary.txt states build config");
    rec->setCameraProvider({});
    rec->setPressClassifier({});

    // --- perturbation: UI event-loop gap, recording OFF vs ON -------------
    auto measureGap = [&](int ms) {
        QElapsedTimer total, gapClock;
        total.start();
        gapClock.start();
        qint64 last = 0;
        double maxGap = 0.0;
        while (total.elapsed() < ms) {
            for (int i = 0; i < 20; ++i) {
                QMouseEvent mv(QEvent::MouseMove, QPointF(3 + (i % 40), 9),
                               window.mapToGlobal(QPoint(3, 9)), Qt::NoButton,
                               Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(&window, &mv);
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            const qint64 now = gapClock.elapsed();
            maxGap = qMax(maxGap, double(now - last));
            last = now;
        }
        return maxGap;
    };
    const double offGap = measureGap(1500);
    rec->startRecording();
    const double onGap = measureGap(1500);
    rec->stopRecording();
    std::printf("INFO overhead under synthetic load: max UI-thread gap "
                "OFF=%.1f ms ON=%.1f ms\n",
                offGap, onGap);

    std::printf("%s (%d failure%s)\n",
                failures ? "RESULT FAIL" : "RESULT PASS", failures,
                failures == 1 ? "" : "s");
    return failures;
}
