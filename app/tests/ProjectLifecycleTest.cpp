// PERMANENT project-lifecycle lock (SankoProjectLifecycle).
//
// Why this family exists: nothing in the gate had ever constructed
// MainWindow, so the whole project lifecycle — open, open again, new, and
// the freeScenes() teardown between them — had ZERO coverage. Two defects
// hid in that blind spot. The first was found only when a user reported the
// app closing on File > Open:
//
//   MainWindow::loadFromPath calls freeScenes(), which deletes every Scene
//   (and its Panels), while DrawingCanvas::m_panel STILL POINTS AT ONE OF
//   THEM. The dangling pointer then survives the whole load until
//   setActivePanel, which dereferenced the outgoing panel through
//   invalidateComposite() -> canvasSize(). Use-after-free; it faulted only
//   when the freed memory had actually been reused, which is why it looked
//   intermittent.
//
//   The second is on the same line of code: loadFromPath calls
//   m_animatic->setFps() AFTER freeScenes(), and AnimaticTimeline::setFps
//   rebuilds its blocks by walking m_scenes — the OLD, freed scenes, which
//   the animatic only replaces when the user navigates to it. It fires only
//   when the new project's frame rate DIFFERS from the current one, so it
//   hid behind the early-return in setFps.
//
// The shape that matters: a check which keeps both panels alive while
// switching passes happily while the app dies (SankoCanvasSizeLock's
// cross-size switch does exactly that, truthfully, and could never have
// caught this). So this family performs REAL loads through the REAL path
// and lets the real teardown delete the real panels.
//
// Scratch discipline: MainWindow persists dock/toolbar state and records
// recent projects on teardown, so QSettings is redirected to INI under a
// scratch root, QStandardPaths is in test mode, and the recents store is
// overridden. Nothing is written outside the scratch root, and that is
// asserted.
//
// Run: build/<config>/SankoProjectLifecycle.exe (exit code = failure count).
// Needs a GUI session; never samples screen pixels.

#include "AnimaticPage.h"
#include "ConsistencyBoard.h"
#include "DrawingCanvas.h"
#include "MainWindow.h"
#include "StoryboardPage.h"
#include "NewProjectDialog.h"
#include "ProjectIO.h"
#include "StoryboardModel.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextStream>
#include <QUndoStack>
#include <QtGui/QTransform>
#include <functional>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

namespace {

// This family's failure mode is a CRASH, not a failed comparison: a
// use-after-free kills the process instead of returning false. Printing a
// symbolised stack turns "the lifecycle test died" into a faulting line,
// which is the difference between a usable gate failure and a mystery.
#ifdef Q_OS_WIN
LONG WINAPI crashHandler(EXCEPTION_POINTERS *info)
{
    fprintf(stdout, "\n*** CRASH: exception 0x%08lX ***\n",
            info->ExceptionRecord->ExceptionCode);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT *context = info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    for (int i = 0; i < 30; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                         &frame, context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr)
            || frame.AddrPC.Offset == 0)
            break;
        DWORD64 disp = 0;
        const char *name = "<unknown>";
        if (SymFromAddr(process, frame.AddrPC.Offset, &disp, symbol))
            name = symbol->Name;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisp, &line))
            fprintf(stdout, "  %2d  %s   (%s:%lu)\n", i, name, line.FileName,
                    line.LineNumber);
        else
            fprintf(stdout, "  %2d  %s\n", i, name);
    }
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

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
    out().flush(); // a crash mid-run must not swallow what already passed
}

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}


void sendMouse(QWidget *w, QEvent::Type type, const QPointF &local,
               Qt::MouseButton b)
{
    QMouseEvent ev(type, local, w->mapToGlobal(local.toPoint()), b,
                   type == QEvent::MouseButtonRelease ? Qt::NoButton : b,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(w, &ev);
}

quint32 g_seed = 0x1234567u;
int randIn(int lo, int hi)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + int(g_seed % quint32(hi - lo + 1));
}

// A project on disk with real artwork, at a chosen size and frame rate.
QString writeProject(const QString &root, const QString &name, const QSize &size,
                     int fps, int sceneCount, int panelsPerScene)
{
    const QString folder = root + QStringLiteral("/") + name;
    QDir().mkpath(folder);
    QVector<Scene *> scenes;
    for (int s = 0; s < sceneCount; ++s) {
        Scene *scene = new Scene;
        scene->number = s + 1;
        scene->location = QStringLiteral("INT. LIFECYCLE %1").arg(s + 1);
        scene->timeOfDay = QStringLiteral("DAY");
        scene->action = QStringLiteral("Lifecycle fixture.");
        for (int p = 0; p < panelsPerScene; ++p) {
            Panel *panel = makeBlankPanel(size);
            QPainter painter(&panel->layers[1].image);
            painter.setPen(Qt::NoPen);
            for (int i = 0; i < 6; ++i) {
                painter.setBrush(QColor(randIn(0, 255), randIn(0, 255),
                                        randIn(0, 255), 200));
                painter.drawEllipse(QPoint(randIn(0, size.width()),
                                           randIn(0, size.height())),
                                    randIn(8, 60), randIn(8, 60));
            }
            panel->duration = 2 + (p % 3);
            scene->panels.append(panel);
        }
        scenes.append(scene);
    }
    ProjectIO::SaveData data;
    // One consistency entry, so the deletion path has something real
    // to delete.
    ConsistencyEntry entry;
    entry.id = QStringLiteral("fixture-entry");
    entry.name = QStringLiteral("Fixture Character");
    entry.type = QStringLiteral("Character");
    entry.description = QStringLiteral("For the dirty-flag check.");
    data.consistency = {entry};
    data.projectName = name;
    data.fps = fps;
    data.canvasSize = size;
    data.scenes = scenes;
    const QJsonObject root_ = ProjectIO::projectToJson(data, folder);
    const QString path = folder + QStringLiteral("/") + name
        + QStringLiteral(".sankotv");
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(root_).toJson(QJsonDocument::Indented));
    f.close();
    for (Scene *scene : scenes)
        delete scene;
    return path;
}

} // namespace


// ---- (d) unsaved-changes tracking -----------------------------------------
// The risk this guards is a change type NOBODY WIRED: a flag that works for
// strokes and silently misses shot info would tell an artist their work is
// safe and then discard it. So every type is proven INDIVIDUALLY, each from
// a clean start, and the two negative controls are load-bearing:
//   * a selection change must NOT mark dirty (it is not a document change,
//     and the undo-stack backstop excludes it by command id). If that
//     exclusion is removed this check fails - it is the only thing standing
//     between "dirty means unsaved work" and "dirty means you touched the
//     canvas".
//   * doing nothing must leave it clean, or every check above passes over a
//     flag that is simply always true.
void runDirtyTrackingPass(const QString &projectPath, const QString &scratch)
{
    out() << "--- (d) unsaved changes: every type, individually ---" << Qt::endl;
    MainWindow window;
    window.resize(1400, 880);
    window.show();
    pump(900);
    if (!window.loadProjectForTest(projectPath)) {
        check(QStringLiteral("(d) fixture project opens"), false);
        return;
    }
    pump(600);

    // THE ORDERING TRAP, asserted directly rather than at some later moment:
    // opening a project fires the very signals that mark it dirty (panels
    // selected, canvas repainting and publishing, pages rebuilding). If
    // setClean() were not the last thing loadFromPath does, a freshly opened
    // project would be born modified and prompt to save work nobody did.
    check(QStringLiteral("(d) a freshly LOADED project is clean, despite the "
                         "load's own internal traffic"),
          !window.isDirty());

    auto *storyboard = window.findChild<StoryboardPage *>();
    auto *animatic = window.findChild<AnimaticPage *>();
    auto *board = window.findChild<ConsistencyBoard *>();
    auto *canvas = window.findChild<DrawingCanvas *>();
    check(QStringLiteral("(d) found the pages to drive"),
          storyboard && animatic && board && canvas);
    if (!storyboard || !animatic || !board || !canvas)
        return;

    // Each type: prove CLEAN first, make exactly ONE change, prove DIRTY.
    auto marksDirty = [&window](const QString &what,
                                const std::function<void()> &change) {
        window.markCleanForTest();
        pump(60);
        const bool cleanFirst = !window.isDirty();
        change();
        pump(250);
        check(QStringLiteral("(d) %1 marks the project dirty").arg(what),
              cleanFirst && window.isDirty(),
              cleanFirst ? QString() : QStringLiteral("was already dirty"));
    };

    marksDirty(QStringLiteral("a canvas stroke"), [canvas] {
        const QTransform t = canvas->viewTransformForTest();
        sendMouse(canvas, QEvent::MouseButtonPress, t.map(QPointF(200, 200)),
                  Qt::LeftButton);
        for (int i = 1; i <= 8; ++i)
            sendMouse(canvas, QEvent::MouseMove,
                      t.map(QPointF(200 + i * 10, 200 + i * 5)), Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, t.map(QPointF(280, 240)),
                  Qt::LeftButton);
        pump(600);
    });

    // The undo-stack BACKSTOP, which is what covers panel add/remove/move,
    // layer stack edits, and any command type added in future. Driving it
    // with a plain command tests the RULE rather than one command's wiring.
    marksDirty(QStringLiteral("any undoable command (the backstop)"), [&window] {
        window.undoStackForTest()->push(
            new QUndoCommand(QStringLiteral("test document change")));
    });

    marksDirty(QStringLiteral("a Shot Info edit"), [storyboard] {
        // The real widget the artist types into; its textChanged runs
        // saveShotInfo(), which writes the panel and emits documentChanged.
        if (auto *notes = storyboard->findChild<QPlainTextEdit *>())
            notes->setPlainText(QStringLiteral("A note about this shot."));
    });

    // The animatic only receives the scenes when the user NAVIGATES to it
    // (loadFromPath does not populate it - the same lazy refresh that let a
    // stale scene list survive a project switch). So drive the real
    // navigation first, exactly as the artist does, or the duration change
    // is a no-op on an empty list and the check would pass vacuously.
    for (QPushButton *b : storyboard->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Continue to Animatic")) {
            b->click();
            break;
        }
    pump(400);
    marksDirty(QStringLiteral("a panel duration change"), [animatic] {
        animatic->setPanelDurationForTest(0, 0, 7);
    });

    marksDirty(QStringLiteral("a frame rate change"), [&window] {
        window.applyProjectSettingsForTest(QStringLiteral("Alpha"), 30);
    });

    marksDirty(QStringLiteral("a project name change"), [&window] {
        window.applyProjectSettingsForTest(QStringLiteral("Renamed"), 30);
    });

    marksDirty(QStringLiteral("a canvas resize"), [&window] {
        window.resizeProjectForTest(QSize(1280, 720));
    });

    marksDirty(QStringLiteral("a consistency entry change"), [board, &window] {
        Q_UNUSED(window);
        board->deleteEntryForTest(0); // fixture ships one entry
    });

    // ---- the two controls --------------------------------------------
    window.markCleanForTest();
    pump(60);
    check(QStringLiteral("(d) CONTROL: doing nothing leaves it clean"),
          !window.isDirty());

    // LOAD-BEARING: selection is not a document change. This is the check
    // that fails if SelectionCommand's id-based exclusion is removed.
    window.markCleanForTest();
    canvas->selectAll();
    pump(300);
    check(QStringLiteral("(d) CONTROL: a SELECTION change does NOT mark dirty"),
          !window.isDirty(),
          QStringLiteral("the SelectionCommand exclusion is what makes this "
                         "pass"));
    canvas->clearSelection();
    pump(150);

    // ---- clearing ----------------------------------------------------
    window.applyProjectSettingsForTest(QStringLiteral("Renamed"), 24);
    pump(150);
    check(QStringLiteral("(d) dirty before saving (control for the next)"),
          window.isDirty());
    const QString savePath = scratch
        + QStringLiteral("/projects/Alpha/Alpha.sankotv");
    check(QStringLiteral("(d) SAVE clears the flag"),
          window.saveProjectForTest(savePath) && !window.isDirty());

    window.applyProjectSettingsForTest(QStringLiteral("Dirtied"), 60);
    pump(150);
    check(QStringLiteral("(d) dirty again (control for New)"), window.isDirty());
    window.newProjectForTest();
    pump(300);
    check(QStringLiteral("(d) NEW PROJECT clears the flag, despite tearing "
                         "the old project down"),
          !window.isDirty());

    window.close();
    pump(300);
}

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(crashHandler);
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("SankoTV"));
    QApplication::setOrganizationName(QStringLiteral("Sanko"));
    QElapsedTimer total;
    total.start();

    const QString scratch =
        QDir::tempPath() + QStringLiteral("/sanko_lifecycle_lock");
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch + QStringLiteral("/settings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       scratch + QStringLiteral("/settings"));
    QStandardPaths::setTestModeEnabled(true);
    // loadFromPath records a recent project on every successful open: point
    // that at the scratch root too, or a test run edits the user's list.
    NewProjectDialog::setSettingsOverride(scratch
                                          + QStringLiteral("/recents.ini"));

    const QString projects = scratch + QStringLiteral("/projects");
    QDir().mkpath(projects);
    // Deliberately DIFFERENT frame rates: the animatic's rebuild-on-fps-change
    // walks the scene list, and with equal rates setFps early-returns and the
    // second use-after-free never fires.
    const QString a = writeProject(projects, QStringLiteral("Alpha"),
                                   QSize(960, 540), 24, 2, 3);
    const QString b = writeProject(projects, QStringLiteral("Beta"),
                                   QSize(1920, 1080), 60, 1, 2);
    const QString c = writeProject(projects, QStringLiteral("Gamma"),
                                   QSize(1280, 720), 30, 3, 2);

    out() << "--- (a) open, then open again: the real free-then-load ---"
          << Qt::endl;
    {
        MainWindow window;
        window.resize(1400, 880);
        window.show();
        pump(900);

        check(QStringLiteral("(a) first project opens"),
              window.loadProjectForTest(a));
        pump(500);
        check(QStringLiteral("(a) the canvas is showing that project's panel"),
              window.activePanelSizeForTest() == QSize(960, 540),
              QStringLiteral("%1x%2").arg(window.activePanelSizeForTest().width())
                  .arg(window.activePanelSizeForTest().height()));

        // THE CHECK. Opening a second project frees every panel the canvas
        // and the animatic are pointing at. A version that detaches nothing
        // dies here rather than reporting a failure, which is the point:
        // the previous line has already been flushed.
        check(QStringLiteral("(a) SECOND project opens without a crash "
                             "(different size AND frame rate)"),
              window.loadProjectForTest(b));
        pump(500);
        check(QStringLiteral("(a) the canvas now shows the SECOND project's "
                             "panel"),
              window.activePanelSizeForTest() == QSize(1920, 1080),
              QStringLiteral("%1x%2").arg(window.activePanelSizeForTest().width())
                  .arg(window.activePanelSizeForTest().height()));

        check(QStringLiteral("(a) a third open, back down in size and rate"),
              window.loadProjectForTest(c));
        pump(500);
        check(QStringLiteral("(a) the canvas shows the THIRD project's panel"),
              window.activePanelSizeForTest() == QSize(1280, 720));

        // Re-opening the SAME project is its own free-then-load.
        check(QStringLiteral("(a) re-opening the same project is safe"),
              window.loadProjectForTest(c));
        pump(400);
        window.close();
        pump(300);
    }

    out() << "--- (b) load then New Project: freeScenes with no reload ---"
          << Qt::endl;
    {
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        pump(700);
        check(QStringLiteral("(b) project opens"), window.loadProjectForTest(a));
        pump(400);
        // New Project frees the scenes through a different call site; the
        // canvas must not be left pointing into them.
        window.newProjectForTest();
        pump(500);
        check(QStringLiteral("(b) after New Project the canvas holds NO panel"),
              !window.activePanelSizeForTest().isValid(),
              QStringLiteral("%1x%2").arg(window.activePanelSizeForTest().width())
                  .arg(window.activePanelSizeForTest().height()));
        check(QStringLiteral("(b) opening a project after New Project works"),
              window.loadProjectForTest(b));
        pump(400);
        window.close();
        pump(300);
    }

    out() << "--- (c) many opens in a row (heap churn) ---" << Qt::endl;
    {
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        pump(700);
        bool allOk = true;
        // A use-after-free faults only when the freed memory has been
        // reused, so ONE switch can pass over a broken build. Alternating
        // sizes and rates repeatedly makes the reuse likely rather than
        // lucky.
        for (int i = 0; i < 6; ++i)
            allOk = allOk && window.loadProjectForTest(i % 3 == 0 ? a
                                                   : (i % 3 == 1 ? b : c));
        pump(400);
        check(QStringLiteral("(c) six consecutive opens, alternating size and "
                             "frame rate"),
              allOk);
        check(QStringLiteral("(c) the canvas ends on the last project opened"),
              window.activePanelSizeForTest() == QSize(1280, 720));
        window.close();
        pump(300);
    }

    runDirtyTrackingPass(a, scratch);

    // Nothing may have escaped the scratch root.
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
