// Tests for the TEMPORARY developer recorder (app/src/devrecorder/).
// Pattern follows SankoPaintPixelLock: a console exe, one PASS/FAIL line per
// check, exit code = failure count.
// Run: build/<config>/SankoDevRecorderTest.exe

#include "devrecorder/DevRecorder.h"
#include "SankoSettings.h"

#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QPushButton>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QLabel>
#include <QAction>
#include <QMenu>
#include <QMouseEvent>

#include <algorithm>
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

// QtGui's own "a key arrived - does a shortcut claim it?" entry point: the
// function QTest::keyClick calls before it delivers the key. Declared here
// (as QtTest's header does) instead of linking QtTest for one function.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(
    QObject *o, ulong timestamp, int k, Qt::KeyboardModifiers mods,
    const QString &text = QString(), bool autorep = false, ushort count = 1);

// Shortcuts resolve against the ACTIVE window. Windows may refuse to hand
// focus to a test that was started from another program, so when asking
// nicely does not work the application is told directly - what is under
// test is which widgets a shortcut is attached to, not the focus policy of
// the desktop.
static void activate(QWidget *w)
{
    w->activateWindow();
    w->raise();
    settle(120);
    if (QApplication::activeWindow() != w) {
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(w);
        QT_WARNING_POP
        settle(60);
    }
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
    // Every settings read/write in app code goes through sankoSettings();
    // point the store at scratch so the family can NEVER touch the
    // user's real settings, driven or not.
    sankoSettingsSetOverrideForTest(root
                                    + QStringLiteral("/sanko_settings.ini"));
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

    // --- 5. a modal produces ONE modalOpen, not one per widget -----------
    // Qt delivers WindowBlocked/WindowUnblocked (and WindowActivate/
    // Deactivate) to EVERY widget in the application, not just top-levels.
    // Unfiltered, a single dialog produced 388 modalOpen records in a real
    // session - one per button, label and scrollbar - and each ran a full
    // desktop z-order walk. The count is the regression check: it is not a
    // number that happens to match today, it is "one modal, one record".
    {
        const QString root5 = root + QStringLiteral("/modal");
        rec->setOutputRoot(root5);
        rec->startRecording();
        const QString dir5 = rec->sessionDir();
        settle(200);
        // The children must be on the BLOCKED window, not on the dialog:
        // WindowBlocked is delivered to the widgets being blocked, and the
        // modal's own hierarchy is not among them. A bare window with no
        // children yields exactly one record whether the filter is there or
        // not - which is how the first version of this check passed while
        // the filter was disabled, i.e. proved nothing.
        QVector<QPushButton *> kids;
        for (int i = 0; i < 8; ++i) {
            auto *b = new QPushButton(QStringLiteral("kid %1").arg(i), &window);
            b->setGeometry(4, 4 + i * 20, 90, 18);
            b->show();
            kids.append(b);
        }
        settle(150);
        {
            QDialog dialog(&window);
            dialog.setModal(true);
            dialog.resize(220, 120);
            QTimer::singleShot(250, &dialog, [&dialog] { dialog.accept(); });
            dialog.exec();
        }
        settle(300);
        rec->stopRecording();

        int opens = 0, closes = 0, nonWindow = 0;
        QFile f5(dir5 + QStringLiteral("/events.jsonl"));
        if (f5.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!f5.atEnd()) {
                const QJsonObject o =
                    QJsonDocument::fromJson(f5.readLine()).object();
                const QString type = o.value(QLatin1String("type")).toString();
                if (type == QLatin1String("modalOpen")) {
                    ++opens;
                    if (o.value(QLatin1String("w")).toString().contains(
                            QLatin1String("QPushButton")))
                        ++nonWindow;
                } else if (type == QLatin1String("modalClose")) {
                    ++closes;
                }
            }
        }
        // The window under test is the only top-level that gets blocked, so
        // exactly one of each. Anything more means the filter is gone.
        check(opens == 1, "one modalOpen per modal (not one per widget)",
              QStringLiteral("got %1").arg(opens));
        check(closes == 1, "one modalClose per modal",
              QStringLiteral("got %1").arg(closes));
        check(nonWindow == 0,
              "no modalOpen from a non-window child widget",
              QStringLiteral("got %1").arg(nonWindow));
        rec->setOutputRoot(root);
        qDeleteAll(kids);
    }

    // --- 6. the recorder's keys work INSIDE a modal dialog ----------------
    // They did not: both actions hung off the host's menu, and Qt refuses a
    // shortcut whose only widget is blocked by a modal - so the one moment
    // a dialog bug is on screen was the one moment it could not be marked.
    // The recorder now attaches its actions to each modal window as it is
    // shown. Keys are delivered the way a real key press is resolved
    // (qt_sendShortcutOverrideEvent: true when a shortcut claimed the key).
    {
        const QString root6 = root + QStringLiteral("/modalkeys");
        rec->setOutputRoot(root6);
        // What the host does: the actions live on its window.
        window.addAction(rec->toggleAction());
        window.addAction(rec->markAction());
        activate(&window);
        const bool startsFromWindow = qt_sendShortcutOverrideEvent(
            &window, 0, Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
        settle(200);
        check(startsFromWindow && rec->isRecording(),
              "control: Ctrl+Shift+R starts a recording from the host window");
        const QString dir6 = rec->sessionDir();

        QDialog dialog(&window);
        dialog.setModal(true);
        dialog.resize(220, 120);
        dialog.show();
        activate(&dialog);
        check(dialog.actions().contains(rec->markAction())
                  && dialog.actions().contains(rec->toggleAction()),
              "a modal dialog is given both recorder actions as it is shown");
        const bool marked = qt_sendShortcutOverrideEvent(
            &dialog, 0, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        settle(150);
        check(marked, "Ctrl+Shift+B is claimed with a modal dialog open");

        // THE CONTROL: take the actions back off the dialog and the same
        // key press, sent the same way, is claimed by nothing. Without this
        // the check above could not tell the bridge from an accident.
        dialog.removeAction(rec->markAction());
        dialog.removeAction(rec->toggleAction());
        const bool markedWithout = qt_sendShortcutOverrideEvent(
            &dialog, 0, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
        settle(150);
        check(!markedWithout,
              "control: without the bridge's attachment the key is NOT "
              "claimed under a modal (the defect, reproduced)");
        dialog.addAction(rec->toggleAction());
        dialog.addAction(rec->markAction());

        const bool stopped = qt_sendShortcutOverrideEvent(
            &dialog, 0, Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
        settle(200);
        check(stopped && !rec->isRecording(),
              "Ctrl+Shift+R stops the recording from inside the modal");
        dialog.close();
        settle(150);

        int markers6 = 0;
        QFile f6(dir6 + QStringLiteral("/events.jsonl"));
        if (f6.open(QIODevice::ReadOnly | QIODevice::Text))
            while (!f6.atEnd())
                if (QJsonDocument::fromJson(f6.readLine())
                        .object()
                        .value(QLatin1String("type"))
                        .toString()
                    == QLatin1String("marker"))
                    ++markers6;
        check(markers6 == 1,
              "exactly ONE marker landed in the session: the press under the "
              "modal, and not the refused one",
              QStringLiteral("got %1").arg(markers6));
        window.removeAction(rec->toggleAction());
        window.removeAction(rec->markAction());
        rec->setOutputRoot(root);
    }

    // Every record of a session, in order.
    auto records = [](const QString &dir) {
        QVector<QJsonObject> rs;
        QFile f(dir + QStringLiteral("/events.jsonl"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text))
            while (!f.atEnd())
                rs.append(QJsonDocument::fromJson(f.readLine()).object());
        return rs;
    };

    // --- 7. a message box is logged by what it says ------------------------
    // A real session had a message box open for 131 s and the record said
    // only "QMessageBox, 400x150": not which one, not what it asked, not
    // what was answered. Title, text, informative text and buttons at
    // modalOpen (and on the state poll that follows it); the button that
    // closed it at modalClose; a file dialog's title at modalOpen.
    {
        const QString root7 = root + QStringLiteral("/msgbox");
        rec->setOutputRoot(root7);
        rec->startRecording();
        const QString dir7 = rec->sessionDir();
        settle(200);
        {
            QMessageBox box(QMessageBox::Question, QStringLiteral("Delete this panel?"),
                            QStringLiteral("Panel 3 of scene 1 will be deleted."),
                            QMessageBox::Yes | QMessageBox::No, &window);
            box.setInformativeText(QStringLiteral("This can be undone."));
            box.setDefaultButton(QMessageBox::No);
            QTimer::singleShot(250, &box, [&box] {
                box.button(QMessageBox::No)->click();
            });
            box.exec();
        }
        settle(300);
        {
            // Not the native dialog: that is a shell window, not a QWidget,
            // and the check is about the title the recorder reads.
            QFileDialog dlg(&window, QStringLiteral("Locate the sound"));
            dlg.setOption(QFileDialog::DontUseNativeDialog, true);
            QTimer::singleShot(250, &dlg, [&dlg] { dlg.reject(); });
            dlg.exec();
        }
        settle(300);
        rec->stopRecording();

        QJsonObject boxOpen, boxClose, boxPoll, fileOpen;
        for (const QJsonObject &o : records(dir7)) {
            const QString type = o.value(QLatin1String("type")).toString();
            const QString modal = o.value(QLatin1String("modal")).toString();
            if (type == QLatin1String("modalOpen") && modal == QLatin1String("QMessageBox"))
                boxOpen = o;
            else if (type == QLatin1String("modalClose")
                     && modal == QLatin1String("QMessageBox"))
                boxClose = o;
            else if (type == QLatin1String("statePoll")
                     && o.value(QLatin1String("modal.class")).toString()
                         == QLatin1String("QMessageBox")
                     && boxPoll.isEmpty())
                boxPoll = o;
            else if (type == QLatin1String("modalOpen")
                     && modal == QLatin1String("QFileDialog"))
                fileOpen = o;
        }
        QStringList buttons;
        for (const QJsonValue &v : boxOpen.value(QLatin1String("buttons")).toArray())
            buttons << v.toString();
        check(boxOpen.value(QLatin1String("title")).toString()
                      == QLatin1String("Delete this panel?")
                  && boxOpen.value(QLatin1String("text")).toString()
                      == QLatin1String("Panel 3 of scene 1 will be deleted.")
                  && boxOpen.value(QLatin1String("informativeText")).toString()
                      == QLatin1String("This can be undone.")
                  && buttons.size() == 2 && buttons.contains(QStringLiteral("Yes"))
                  && buttons.contains(QStringLiteral("No")),
              "modalOpen of a message box carries its title, text, informative "
              "text and buttons (mnemonics stripped)",
              QStringLiteral("title '%1' text '%2' info '%3' buttons [%4]")
                  .arg(boxOpen.value(QLatin1String("title")).toString(),
                       boxOpen.value(QLatin1String("text")).toString(),
                       boxOpen.value(QLatin1String("informativeText")).toString(),
                       buttons.join(QLatin1String(", "))));
        check(boxPoll.value(QLatin1String("modal.text")).toString()
                      == QLatin1String("Panel 3 of scene 1 will be deleted.")
                  && boxPoll.value(QLatin1String("modal.title")).toString()
                      == QLatin1String("Delete this panel?"),
              "the state poll taken at modalOpen carries the same text",
              QStringLiteral("modal.text '%1'")
                  .arg(boxPoll.value(QLatin1String("modal.text")).toString()));
        check(boxClose.value(QLatin1String("closedBy")).toString() == QLatin1String("No")
                  && boxClose.value(QLatin1String("title")).toString()
                      == QLatin1String("Delete this panel?"),
              "modalClose names the button that closed the message box",
              QStringLiteral("closedBy '%1' result %2")
                  .arg(boxClose.value(QLatin1String("closedBy")).toString())
                  .arg(boxClose.value(QLatin1String("result")).toInt()));
        check(fileOpen.value(QLatin1String("title")).toString()
                  == QLatin1String("Locate the sound"),
              "modalOpen of a file dialog carries its title",
              QStringLiteral("title '%1'")
                  .arg(fileOpen.value(QLatin1String("title")).toString()));
        rec->setOutputRoot(root);
    }

    // --- 8. keys: navigation and command keys only, never in a text field --
    // Synthetic QKeyEvents sent straight to the widgets (never the real
    // keyboard, never dependent on which window is active): a press and
    // its release, as a keyboard sends them.
    {
        const QString root8 = root + QStringLiteral("/keys");
        auto *field = new QLineEdit(&window);
        field->setObjectName(QStringLiteral("nameField"));
        field->setGeometry(4, 200, 120, 22);
        field->show();
        auto *button = new QPushButton(QStringLiteral("target"), &window);
        button->setObjectName(QStringLiteral("target"));
        button->setGeometry(4, 230, 90, 22);
        button->show();
        settle(150);
        auto press = [](QWidget *to, int key, Qt::KeyboardModifiers mods,
                        const QString &text = QString()) {
            QKeyEvent down(QEvent::KeyPress, key, mods, text);
            QApplication::sendEvent(to, &down);
            QKeyEvent up(QEvent::KeyRelease, key, mods, text);
            QApplication::sendEvent(to, &up);
        };
        rec->setOutputRoot(root8);
        rec->startRecording();
        const QString dir8 = rec->sessionDir();
        settle(200);
        // Typing into the text field: a word, then the keys that ARE
        // logged elsewhere, then Ctrl+A and F5 - none of it may appear.
        for (QChar c : QStringLiteral("panel"))
            press(field, c.unicode() - 'a' + Qt::Key_A, Qt::NoModifier, QString(c));
        press(field, Qt::Key_Delete, Qt::NoModifier);
        press(field, Qt::Key_Left, Qt::NoModifier);
        press(field, Qt::Key_Home, Qt::NoModifier);
        press(field, Qt::Key_A, Qt::ControlModifier, QStringLiteral("\x01"));
        press(field, Qt::Key_F5, Qt::NoModifier);
        press(field, Qt::Key_Return, Qt::NoModifier);
        settle(100);
        rec->markIssue(QStringLiteral("field done"));
        // Outside the text field: Delete (the positive control), a letter
        // on its own (not logged), Ctrl+Z, F5, Left, Escape, Control alone
        // (not logged), and the same Delete twice in a row (two records).
        press(button, Qt::Key_Delete, Qt::NoModifier);
        press(button, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
        press(button, Qt::Key_Z, Qt::ControlModifier, QStringLiteral("\x1a"));
        press(button, Qt::Key_F5, Qt::NoModifier);
        press(button, Qt::Key_Left, Qt::NoModifier);
        press(button, Qt::Key_Escape, Qt::NoModifier);
        press(button, Qt::Key_Control, Qt::ControlModifier);
        press(button, Qt::Key_Delete, Qt::NoModifier);
        press(button, Qt::Key_Delete, Qt::NoModifier);
        settle(300);
        rec->stopRecording();

        int beforeMark = 0;
        QStringList after;
        QStringList widgets;
        bool marked = false, textLeaked = false;
        for (const QJsonObject &o : records(dir8)) {
            const QString type = o.value(QLatin1String("type")).toString();
            if (type == QLatin1String("marker")) {
                marked = true;
                continue;
            }
            if (type != QLatin1String("key"))
                continue;
            if (o.contains(QLatin1String("text")))
                textLeaked = true;
            if (!marked) {
                ++beforeMark;
                continue;
            }
            after << o.value(QLatin1String("name")).toString();
            widgets << o.value(QLatin1String("w")).toString();
        }
        check(marked, "the marker between the two halves was written");
        check(beforeMark == 0,
              "typing into a text field logs NO key - not the letters, not "
              "Delete, Left, Home, Ctrl+A, F5 or Enter",
              QStringLiteral("%1 key record(s) before the marker").arg(beforeMark));
        const QStringList expected{QStringLiteral("Del"),  QStringLiteral("Ctrl+Z"),
                                   QStringLiteral("F5"),   QStringLiteral("Left"),
                                   QStringLiteral("Esc"),  QStringLiteral("Del"),
                                   QStringLiteral("Del")};
        check(after == expected,
              "CONTROL, outside a text field: Delete, Ctrl+Z, F5, Left, Escape and "
              "two more Deletes are logged, once each, in order; a letter on its "
              "own and Control by itself are not",
              QStringLiteral("logged [%1]").arg(after.join(QLatin1String(", "))));
        check(!widgets.isEmpty()
                  && std::all_of(widgets.cbegin(), widgets.cend(), [](const QString &w) {
                         return w == QLatin1String("target:QPushButton");
                     }),
              "each key record names the widget that received it",
              QStringLiteral("widgets [%1]").arg(widgets.join(QLatin1String(", "))));
        check(!textLeaked, "no key record carries the event's text");
        delete field;
        delete button;
        rec->setOutputRoot(root);
    }

    // --- 9. menu and shortcut ACTIONS are logged (2026-10-26) --------------
    // The second 1B hand test lost an undo: Ctrl+Z belongs to a QAction, so
    // its key never arrives as a KeyPress and its QShortcutEvent goes to the
    // action, not a widget. Every action that fires is now an "action"
    // record with its text: through the shortcut map (the real resolution,
    // qt_sendShortcutOverrideEvent) and through a menu click.
    {
        const QString root9 = root + QStringLiteral("/actions");
        rec->setOutputRoot(root9);
        QAction undo(QStringLiteral("&Undo Move Camera"), &window);
        undo.setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
        window.addAction(&undo);
        QAction never(QStringLiteral("Never Fired"), &window);
        never.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F12));
        window.addAction(&never);
        QMenu menu(&window);
        menu.addAction(&undo); // the same action in a menu: one record, not two
        QAction *redo = menu.addAction(QStringLiteral("Redo Move Camera"));
        activate(&window);
        rec->startRecording();
        const QString dir9 = rec->sessionDir();
        settle(150);
        menu.popup(window.mapToGlobal(QPoint(10, 10)));
        settle(150);
        const QPoint item = menu.actionGeometry(redo).center();
        QMouseEvent down(QEvent::MouseButtonPress, QPointF(item),
                         menu.mapToGlobal(item), Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(&menu, &down);
        QMouseEvent up(QEvent::MouseButtonRelease, QPointF(item),
                       menu.mapToGlobal(item), Qt::LeftButton, Qt::NoButton,
                       Qt::NoModifier);
        QApplication::sendEvent(&menu, &up);
        settle(150);
        activate(&window);
        int undoFired = 0;
        QObject::connect(&undo, &QAction::triggered, [&undoFired] { ++undoFired; });
        const bool claimed =
            qt_sendShortcutOverrideEvent(&window, 0, Qt::Key_Z, Qt::ControlModifier);
        settle(300);
        rec->stopRecording();

        QStringList actions;
        for (const QJsonObject &o : records(dir9))
            if (o.value(QLatin1String("type")).toString() == QLatin1String("action"))
                actions << QStringLiteral("%1|%2|%3")
                               .arg(o.value(QLatin1String("text")).toString(),
                                    o.value(QLatin1String("via")).toString(),
                                    o.value(QLatin1String("key")).toString());
        check(claimed && undoFired == 1
                  && actions.count(QStringLiteral("Undo Move Camera|shortcut|Ctrl+Z")) == 1
                  && actions.filter(QStringLiteral("Undo Move Camera")).size() == 1,
              "a shortcut that fires (Ctrl+Z on an action that is also in a menu) is "
              "logged ONCE as an action, with its text and key",
              QStringLiteral("claimed=%1 fired=%2 [%3]").arg(claimed).arg(undoFired)
                  .arg(actions.join(QLatin1String(", "))));
        check(actions.contains(QStringLiteral("Redo Move Camera|menu|")),
              "a menu item clicked is logged as an action with its text",
              actions.join(QLatin1String(", ")));
        check(actions.filter(QStringLiteral("Never Fired")).isEmpty() && actions.size() == 2,
              "control: an action that never fired is not logged; nothing else is",
              actions.join(QLatin1String(", ")));
        window.removeAction(&undo);
        window.removeAction(&never);
        rec->setOutputRoot(root);
    }

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
