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
#include "SankoSettings.h"
#include "ConsistencyBoard.h"
#include "DrawingCanvas.h"
#include "PerspectiveTool.h"
#include "MainWindow.h"
#include "StoryboardPage.h"
#include "NewProjectDialog.h"
#include "ProjectIO.h"
#include "StoryboardModel.h"
#include "devrecorder/DevRecorder.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QStandardPaths>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPlainTextEdit>
#include <QFileInfo>
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
    const QString path = folder + QStringLiteral("/") + name
        + QStringLiteral(".sankotv");
    // The PROJECT FILE PATH, not its folder: projectToJson derives the
    // "<basename>_assets" subfolder from it, which is what keeps two
    // projects in one directory from writing the same image files.
    const ProjectIO::WriteResult w_ = ProjectIO::projectToJson(data, path);
    if (!w_.ok)
        check(QStringLiteral("fixture images written"), false, w_.reason);
    const QJsonObject root_ = w_.root;
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


// ---- (e) Close Project, and (f) the recovery paths ------------------------
// Close is a state the app had never deliberately entered: it has always
// STARTED with no project, but never returned there from a loaded one. Every
// item below came out of the audit that preceded it.
void runClosePass(const QString &projectA, const QString &projectB,
                  const QString &scratch)
{
    out() << "--- (e) Close Project: what it leaves behind ---" << Qt::endl;
    MainWindow window;
    window.resize(1400, 880);
    window.show();
    pump(900);
    check(QStringLiteral("(e) project opens"), window.loadProjectForTest(projectA));
    pump(600);

    auto *storyboard = window.findChild<StoryboardPage *>();
    auto *canvas = window.findChild<DrawingCanvas *>();
    if (!storyboard || !canvas) {
        check(QStringLiteral("(e) found the page and canvas"), false);
        return;
    }

    // Build up exactly the state Close has to clear, and PROVE it is there:
    // clearing something that was never present proves nothing.
    storyboard->copySelectedPanel();
    canvas->selectAll();
    canvas->copySelection();
    canvas->perspective()->addVanishingPoint(QPointF(120, 80));
    pump(300);
    check(QStringLiteral("(e) control: clipboard, canvas clipboard and a "
                         "vanishing point all exist before the close"),
          storyboard->hasPanelClipboard() && canvas->hasCanvasClipboard()
              && canvas->perspective()->count() == 1);
    window.undoStackForTest()->push(
        new QUndoCommand(QStringLiteral("something to undo")));
    check(QStringLiteral("(e) control: the undo stack is not empty, and the "
                         "project is dirty"),
          window.undoStackForTest()->count() > 0 && window.isDirty());

    // Close now PROMPTS when there is unsaved work - which the control
    // above just proved there is - and this gate cannot answer a modal.
    // Clearing the flag makes the prompt a no-op so the REAL close path
    // still runs end to end; the prompt's own decision is asserted
    // separately below, as a query. Everything else built up above
    // (clipboards, vanishing points, undo stack) is untouched by this.
    window.markCleanForTest();
    window.closeProjectForTest();
    pump(500);

    check(QStringLiteral("(e) the Dashboard is showing"),
          window.onDashboardForTest());
    check(QStringLiteral("(e) no scenes and no active panel"),
          !window.activePanelSizeForTest().isValid());
    check(QStringLiteral("(e) the project path and name are cleared"),
          window.projectPathForTest().isEmpty()
              && window.projectNameForTest() == QStringLiteral("Untitled Project"));
    check(QStringLiteral("(e) frame rate and canvas size are back to their "
                         "idle values"),
          window.projectFpsForTest() == 24
              && storyboard->projectCanvasSize() == QSize(960, 540));
    check(QStringLiteral("(e) the undo stack is empty"),
          window.undoStackForTest()->count() == 0);
    check(QStringLiteral("(e) the panel clipboard is cleared"),
          !storyboard->hasPanelClipboard());
    check(QStringLiteral("(e) the CANVAS clipboard is cleared"),
          !canvas->hasCanvasClipboard());
    check(QStringLiteral("(e) the perspective vanishing points are cleared"),
          canvas->perspective()->count() == 0);
    check(QStringLiteral("(e) the project is clean (nothing left to prompt "
                         "about)"),
          !window.isDirty());

    out() << "--- (f) close -> open, and close -> new ---" << Qt::endl;
    check(QStringLiteral("(f) opening a project after a close works"),
          window.loadProjectForTest(projectB));
    pump(500);
    check(QStringLiteral("(f) the canvas shows the reopened project"),
          window.activePanelSizeForTest().isValid());
    check(QStringLiteral("(f) it opens clean"), !window.isDirty());

    window.closeProjectForTest();
    pump(400);
    window.newProjectForTest();
    pump(400);
    check(QStringLiteral("(f) New Project after a close leaves no panel and "
                         "no dirt"),
          !window.activePanelSizeForTest().isValid() && !window.isDirty());
    check(QStringLiteral("(f) closing twice in a row is harmless"),
          (window.closeProjectForTest(), pump(200),
           window.onDashboardForTest() && !window.isDirty()));

    // ---- the prompt DECISION, as a query rather than a modal -----------
    out() << "--- (e) the unsaved-changes decision ---" << Qt::endl;
    check(QStringLiteral("(e) a clean project needs no prompt"),
          !window.shouldPromptToSave());
    check(QStringLiteral("(e) reopening for the prompt checks"),
          window.loadProjectForTest(projectA));
    pump(400);
    window.applyProjectSettingsForTest(QStringLiteral("Edited"), 30);
    pump(200);
    check(QStringLiteral("(e) a dirty project DOES need a prompt"),
          window.shouldPromptToSave());

    // Answer -> consequence, including the one that matters most.
    check(QStringLiteral("(e) answering Cancel does NOT allow the transition"),
          !window.mayDiscardForTest(MainWindow::DiscardAnswer::Cancel));
    check(QStringLiteral("(e) answering Discard DOES allow it"),
          window.mayDiscardForTest(MainWindow::DiscardAnswer::Discard));
    check(QStringLiteral("(e) answering Save allows it when the save "
                         "SUCCEEDS"),
          window.mayDiscardForTest(MainWindow::DiscardAnswer::Save)
              && !window.isDirty());

    // THE failure the prompt exists to prevent, and which would arrive
    // THROUGH the prompt: the artist chooses Save, the save does not happen,
    // and the transition proceeds anyway - discarding the work they just
    // asked to keep. Driven without a modal by making the write fail: the
    // project's folder is removed, so saving to its path cannot succeed.
    window.applyProjectSettingsForTest(QStringLiteral("EditedAgain"), 60);
    pump(200);
    // Force the WRITE to fail in a way that does not depend on the
    // folder existing: saving now mkpaths its assets subfolder, which
    // recreates a deleted directory, so removing the folder no longer
    // makes a save fail. Putting a DIRECTORY where the .sankotv should
    // go makes QFile::open refuse, whatever else exists.
    const QString blocked = window.projectPathForTest();
    QFile::remove(blocked);
    QDir().mkpath(blocked);
    pump(150);
    // A save that FAILS legitimately warns the artist, and that warning is
    // a modal this run cannot click. Dismiss whatever modal appears while
    // the save is attempted: the assertion is about the TRANSITION being
    // refused, and blocking forever on the dialog proves nothing. (Without
    // this the family hung here - the check passed previously only because
    // the deletion happened to leave the save succeeding some runs.)
    QTimer dismisser;
    dismisser.setInterval(120);
    QObject::connect(&dismisser, &QTimer::timeout, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    dismisser.start();
    const bool allowed = window.mayDiscardForTest(MainWindow::DiscardAnswer::Save);
    dismisser.stop();
    check(QStringLiteral("(e) a SAVE THAT DID NOT HAPPEN must NOT allow the "
                         "transition"),
          !allowed, allowed ? QStringLiteral("it proceeded - work would be "
                                             "lost") : QString());
    check(QStringLiteral("(e) ...and the project is still dirty afterwards"),
          window.isDirty());

    Q_UNUSED(scratch);
    // The window is deliberately DIRTY here, and closing it now goes
    // through the real closeEvent, which prompts - a modal this gate
    // cannot answer. Clean it first: the prompt itself is out of scope
    // (its decision is asserted above), and hanging the gate on a
    // dialog nobody can click proves nothing.
    window.markCleanForTest();
    window.close();
    pump(300);
}


// ---- (g) the view resets on every project transition ----------------------
// One long-lived DrawingCanvas serves every project, so view state that is
// never reset simply carries over. That looked like zoom and rotation being
// saved into a project and leaking between projects, when NEITHER WAS EVER
// WRITTEN TO DISK - there is no view state in the save format at all.
//
// The whole risk here is ONE of these being left out of the reset, so each
// is asserted individually, for each of the three transitions, behind a
// control proving it was non-default first. And grid and safe-area are
// asserted NOT to reset, so nobody later "fixes" them into the same path:
// they are app-wide preferences in QSettings, not project view state.
void runViewResetPass(const QString &projectA, const QString &projectB)
{
    out() << "--- (g) view state resets on Open / New / Close ---" << Qt::endl;
    MainWindow window;
    window.resize(1400, 880);
    window.show();
    pump(900);
    check(QStringLiteral("(g) project opens"), window.loadProjectForTest(projectA));
    pump(600);
    auto *canvas = window.findChild<DrawingCanvas *>();
    if (!canvas) {
        check(QStringLiteral("(g) found the canvas"), false);
        return;
    }

    // Drive the view far from default through the REAL setters.
    auto disturb = [canvas] {
        // CTRL+wheel at an OFF-CENTRE point: the real zoom path (plain
        // wheel is ignored by the canvas), and unlike
        // setViewZoom (which centres) it moves the pan offset too, so the
        // pan reset is not asserted against a value that was never
        // disturbed.
        // ONE step is enough to make zoom and pan non-default, and it
        // keeps the cost down: with onion skin, light table, rotation
        // and a big zoom all live, every repaint in this section is
        // expensive, and four steps made the family four times slower.
        for (int i = 0; i < 1; ++i) {
            QWheelEvent wheel(QPointF(200, 150),
                              canvas->mapToGlobal(QPoint(200, 150)), QPoint(),
                              QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
                              Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(canvas, &wheel);
        }
        canvas->setViewRotation(45.0);
        canvas->toggleFlipH();
        canvas->setOnionSkinEnabled(true);
        canvas->setLightTableEnabled(true);
        canvas->setGridVisible(true); // a PREFERENCE: must survive
        pump(120);
    };
    auto isDisturbed = [canvas] {
        return !qFuzzyCompare(canvas->viewZoom(), 0.85)
            && !qFuzzyIsNull(canvas->viewRotation()) && canvas->viewFlipH()
            && canvas->isOnionSkinEnabled() && canvas->isLightTableEnabled()
            && !canvas->viewPanOffset().isNull();
    };
    // Each item, named, so a reset that forgets one says WHICH one.
    auto checkDefaults = [canvas](const QString &transition) {
        check(QStringLiteral("(g) %1: zoom back to the startup 0.85")
                  .arg(transition),
              qFuzzyCompare(canvas->viewZoom(), 0.85),
              QStringLiteral("%1").arg(canvas->viewZoom()));
        check(QStringLiteral("(g) %1: pan offset cleared").arg(transition),
              canvas->viewPanOffset().isNull(),
              QStringLiteral("%1,%2").arg(canvas->viewPanOffset().x())
                  .arg(canvas->viewPanOffset().y()));
        check(QStringLiteral("(g) %1: rotation back to 0").arg(transition),
              qFuzzyIsNull(canvas->viewRotation()),
              QStringLiteral("%1").arg(canvas->viewRotation()));
        check(QStringLiteral("(g) %1: horizontal flip cleared").arg(transition),
              !canvas->viewFlipH());
        check(QStringLiteral("(g) %1: onion skin off").arg(transition),
              !canvas->isOnionSkinEnabled());
        check(QStringLiteral("(g) %1: light table off").arg(transition),
              !canvas->isLightTableEnabled());
        // The preference must NOT be swept up in the reset.
        check(QStringLiteral("(g) %1: grid (an app-wide PREFERENCE) is NOT "
                             "reset").arg(transition),
              canvas->gridVisible());
    };

    // --- transition 1: OPEN another project ---------------------------
    disturb();
    check(QStringLiteral("(g) control: the view is non-default before OPEN"),
          isDisturbed());
    check(QStringLiteral("(g) opening another project"),
          window.loadProjectForTest(projectB));
    pump(500);
    checkDefaults(QStringLiteral("open"));

    // --- transition 2: NEW project ------------------------------------
    disturb();
    check(QStringLiteral("(g) control: the view is non-default before NEW"),
          isDisturbed());
    window.newProjectForTest();
    pump(400);
    checkDefaults(QStringLiteral("new"));

    // --- transition 3: CLOSE ------------------------------------------
    check(QStringLiteral("(g) reopening for the close check"),
          window.loadProjectForTest(projectA));
    pump(500);
    disturb();
    check(QStringLiteral("(g) control: the view is non-default before CLOSE"),
          isDisturbed());
    window.markCleanForTest(); // the close prompt is asserted elsewhere
    window.closeProjectForTest();
    pump(400);
    checkDefaults(QStringLiteral("close"));

    // Reopening the FIRST project must show the default view, which is the
    // symptom that was reported as state leaking between projects.
    check(QStringLiteral("(g) reopening the first project"),
          window.loadProjectForTest(projectA));
    pump(500);
    check(QStringLiteral("(g) the reopened project shows the DEFAULT view "
                         "(the reported leak)"),
          qFuzzyCompare(canvas->viewZoom(), 0.85)
              && qFuzzyIsNull(canvas->viewRotation()));

    canvas->setGridVisible(false); // leave the preference as we found it
    window.markCleanForTest();
    window.close();
    pump(300);
}


// ---- (h) Save As produces an INDEPENDENT project --------------------------
// The bug this guards: panels and layers were written as image files named
// by POSITION, carrying nothing that identifies the project, into whatever
// folder held the .sankotv. Two projects in one folder wrote THE SAME
// FILES, so whichever saved last overwrote the other's artwork - silently,
// because at the moment of the Save As both held identical pixels.
//
// The bug was SYMMETRIC: either project could destroy the other. So this
// checks BOTH directions. A check that only edited the copy would pass over
// a fix that isolated one side and not the other.
void runSaveAsIndependencePass(const QString &scratch)
{
    out() << "--- (h) Save As independence, both directions ---" << Qt::endl;
    const QString shared = scratch + QStringLiteral("/saveas_shared");
    QDir(shared).removeRecursively();
    QDir().mkpath(shared);

    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);

    const QString a = shared + QStringLiteral("/SB_001.sankotv");
    const QString b = shared + QStringLiteral("/SB_002.sankotv");
    const QString fixture = writeProject(scratch + QStringLiteral("/saveas_src"),
                                         QStringLiteral("Source"),
                                         QSize(960, 540), 24, 1, 2);
    check(QStringLiteral("(h) fixture opens"), window.loadProjectForTest(fixture));
    pump(500);
    check(QStringLiteral("(h) save as SB_001"), window.saveProjectForTest(a));
    pump(300);
    check(QStringLiteral("(h) save as SB_002, SAME folder"),
          window.saveProjectForTest(b));
    pump(300);

    // Each must own a subfolder named from its FILE, not its project name:
    // both of these carry projectName "Source".
    check(QStringLiteral("(h) each project has its own assets folder"),
          QDir(shared + QStringLiteral("/SB_001_assets")).exists()
              && QDir(shared + QStringLiteral("/SB_002_assets")).exists());
    check(QStringLiteral("(h) no loose image files beside the manifests"),
          QDir(shared).entryList({QStringLiteral("*.png")}, QDir::Files).isEmpty());

    auto snapshot = [](const QString &dir) {
        QMap<QString, QByteArray> result;
        for (const QFileInfo &fi : QDir(dir).entryInfoList(
                 {QStringLiteral("*.png")}, QDir::Files, QDir::Name)) {
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::ReadOnly))
                result.insert(fi.fileName(),
                              QCryptographicHash::hash(
                                  f.readAll(), QCryptographicHash::Sha256));
        }
        return result;
    };
    auto paintAndSave = [&window](const QString &path) {
        auto *canvas = window.findChild<DrawingCanvas *>();
        if (canvas) {
            const QTransform t = canvas->viewTransformForTest();
            sendMouse(canvas, QEvent::MouseButtonPress, t.map(QPointF(120, 100)),
                      Qt::LeftButton);
            for (int i = 1; i <= 8; ++i)
                sendMouse(canvas, QEvent::MouseMove,
                          t.map(QPointF(120 + i * 25, 100 + i * 18)),
                          Qt::LeftButton);
            sendMouse(canvas, QEvent::MouseButtonRelease,
                      t.map(QPointF(320, 244)), Qt::LeftButton);
            pump(700);
        }
        window.saveProjectForTest(path);
        pump(300);
    };

    // --- direction 1: edit the COPY, the ORIGINAL must not move ---------
    const QMap<QString, QByteArray> beforeA =
        snapshot(shared + QStringLiteral("/SB_001_assets"));
    check(QStringLiteral("(h) control: SB_001 has images to compare"),
          beforeA.size() >= 2, QStringLiteral("%1 file(s)").arg(beforeA.size()));
    window.loadProjectForTest(b);
    pump(400);
    paintAndSave(b);
    check(QStringLiteral("(h) editing the COPY leaves the ORIGINAL "
                         "byte-identical"),
          snapshot(shared + QStringLiteral("/SB_001_assets")) == beforeA);

    // POSITIVE CONTROL: the comparison must SEE a real pixel change, or
    // "byte-identical" above only means the comparison is blind.
    const QMap<QString, QByteArray> b1 =
        snapshot(shared + QStringLiteral("/SB_002_assets"));
    paintAndSave(b);
    check(QStringLiteral("(h) CONTROL: the same comparison DETECTS a real "
                         "pixel change"),
          snapshot(shared + QStringLiteral("/SB_002_assets")) != b1);

    // --- direction 2: edit the ORIGINAL, the COPY must not move ---------
    // The bug was symmetric - whichever saved last clobbered the other - so
    // isolating one side only would still lose work.
    const QMap<QString, QByteArray> beforeB =
        snapshot(shared + QStringLiteral("/SB_002_assets"));
    window.loadProjectForTest(a);
    pump(400);
    paintAndSave(a);
    check(QStringLiteral("(h) editing the ORIGINAL leaves the COPY "
                         "byte-identical"),
          snapshot(shared + QStringLiteral("/SB_002_assets")) == beforeB);

    // --- the migration path every existing project takes ----------------
    // An OLD project names flat files beside its manifest. It must still
    // load, and its first save under this build must write _assets/ while
    // leaving those flat files alone: another manifest in that folder may
    // still need them.
    out() << "--- (h) migration: an old flat-named project ---" << Qt::endl;
    const QString oldDir = scratch + QStringLiteral("/legacy_flat");
    QDir(oldDir).removeRecursively();
    QDir().mkpath(oldDir);
    const QString oldPath = oldDir + QStringLiteral("/Legacy.sankotv");
    window.loadProjectForTest(fixture);
    pump(400);
    window.saveProjectForTest(oldPath);
    pump(300);
    {
        // Rewrite it into the OLD flat layout: strip the subfolder from the
        // stored names and put the images beside the manifest.
        QFile f(oldPath);
        f.open(QIODevice::ReadOnly);
        QString text = QString::fromUtf8(f.readAll());
        f.close();
        text.remove(QStringLiteral("Legacy_assets/"));
        QFile w(oldPath);
        w.open(QIODevice::WriteOnly);
        w.write(text.toUtf8());
        w.close();
        for (const QFileInfo &fi :
             QDir(oldDir + QStringLiteral("/Legacy_assets"))
                 .entryInfoList({QStringLiteral("*.png")}, QDir::Files))
            QFile::copy(fi.absoluteFilePath(),
                        oldDir + QStringLiteral("/") + fi.fileName());
        QDir(oldDir + QStringLiteral("/Legacy_assets")).removeRecursively();
    }
    const QStringList flatBefore =
        QDir(oldDir).entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name);
    check(QStringLiteral("(h) control: the legacy project really is flat"),
          !flatBefore.isEmpty()
              && !QDir(oldDir + QStringLiteral("/Legacy_assets")).exists(),
          QStringLiteral("%1 flat png").arg(flatBefore.size()));
    const QMap<QString, QByteArray> flatHashes = snapshot(oldDir);

    check(QStringLiteral("(h) an OLD flat-named project still loads"),
          window.loadProjectForTest(oldPath));
    pump(500);
    check(QStringLiteral("(h) ...with its artwork found at the old flat names"),
          window.activePanelSizeForTest() == QSize(960, 540));

    window.saveProjectForTest(oldPath); // its FIRST save under this build
    pump(400);
    check(QStringLiteral("(h) its first save writes an _assets folder"),
          QDir(oldDir + QStringLiteral("/Legacy_assets")).exists());
    check(QStringLiteral("(h) ...and leaves the old flat images UNTOUCHED"),
          snapshot(oldDir) == flatHashes,
          QStringLiteral("%1 flat file(s) before").arg(flatHashes.size()));

    window.markCleanForTest();
    window.close();
    pump(300);
}


// ---- (k) a recording carries its provenance -------------------------------
// The git hash in system.txt has gone wrong SILENTLY twice: first it was
// captured at configure time and named a commit four builds behind the
// binary (a hash that LIED), then the fix generated it at build time but
// included the header in DevRecorder.cpp — where nothing uses it — while
// the #ifdef consuming it sits in MainWindow.cpp, which never saw the
// macro and compiled the empty branch (a hash that WASN'T THERE). The old
// lie is why the new silence went unnoticed. This drives the REAL wiring:
// MainWindow's HostInfo -> Recorder -> system.txt, and fails loudly.
void runProvenancePass(const QString &scratch)
{
    out() << "--- (k) system.txt carries a real git hash ---" << Qt::endl;
    MainWindow window; // constructs + initializes the recorder singleton
    window.resize(1100, 700);
    window.show();
    pump(600);
    auto *rec = devrec::Recorder::instance();
    rec->startRecording();
    pump(700); // one perf tick; system.txt is written at session start
    rec->stopRecording();
    pump(300);

    // SANKOTV_DEVREC_DIR (set in main BEFORE the first MainWindow, because
    // the singleton reads it once in its constructor) points the output at
    // the scratch root. The recorder's settings read now goes through
    // sankoSettings() - the choke point that ended the two-argument
    // QSettings bypass which let a measurement probe write into the user's
    // real recordings folder on 2026-08-27 - but the env var stays the
    // redirect HERE: it is read before any test override could matter and
    // is the recorder's own documented escape hatch.
    const QString root = scratch + QStringLiteral("/devrec");
    const QStringList sessions = QDir(root).entryList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    check(QStringLiteral("(k) the recording landed under the SCRATCH root, "
                         "not Documents"),
          !sessions.isEmpty(),
          QStringLiteral("%1 session(s) under %2").arg(sessions.size())
              .arg(root));
    QString gitLine, osLine;
    if (!sessions.isEmpty()) {
        QFile f(root + QStringLiteral("/") + sessions.last()
                + QStringLiteral("/system.txt"));
        check(QStringLiteral("(k) system.txt exists"),
              f.open(QIODevice::ReadOnly));
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(QStringLiteral("git: ")))
                gitLine = line.mid(5).trimmed();
            if (line.startsWith(QStringLiteral("os: ")))
                osLine = line.mid(4).trimmed();
        }
    }
    // CONTROL: the parser sees THIS file's values (an empty git line must
    // mean the hash is missing, not that the read went wrong).
    check(QStringLiteral("(k) control: the parser reads this system.txt "
                         "(os line non-empty)"),
          !osLine.isEmpty(), osLine);
    check(QStringLiteral("(k) the git hash is PRESENT - not empty, not "
                         "\"unknown\""),
          !gitLine.isEmpty() && gitLine != QStringLiteral("unknown"),
          gitLine.isEmpty() ? QStringLiteral("EMPTY - provenance lost again")
                            : gitLine);

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (j) the Size CTL bar shows what the engine holds ---------------------
// PRE-EXISTING display lie, fixed with the 5000 cap: the bar's mirror
// clamped to 200 while the studio's slider could already set 2048, so the
// bar displayed 200 whenever a large library brush was active. Nothing
// could observe it from outside — the slider class is file-local — which
// is why the bar grew sizeCtlDisplayedSizeForTest() alongside the fix.
void runSizeCtlAgreementPass(const QString &scratch)
{
    out() << "--- (j) Size CTL bar <-> engine agreement ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/ctl_src"),
                                         QStringLiteral("Ctl"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(j) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *canvas = window.findChild<DrawingCanvas *>();
    auto *page = window.findChild<StoryboardPage *>();
    if (!canvas || !page) {
        check(QStringLiteral("(j) found canvas and storyboard page"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);

    // CONTROL first: a small value flows preset -> engine -> bar, so the
    // 5000 assertions below cannot pass on a bar that just shows anything.
    ::Brush smallPreset;
    smallPreset.setSize(152);
    canvas->setPaintBrush(smallPreset);
    pump(200);
    check(QStringLiteral("(j) control: a 152 preset reaches the engine and "
                         "the bar"),
          canvas->paintBrush().size() == 152
              && page->sizeCtlDisplayedSizeForTest() == 152,
          QStringLiteral("engine=%1 bar=%2")
              .arg(canvas->paintBrush().size())
              .arg(page->sizeCtlDisplayedSizeForTest()));

    // The library/studio path: a 5000 preset must land in the engine AND
    // on the bar. Before this pass the engine clamped it to 2048 and the
    // bar displayed 200.
    ::Brush preset;
    preset.setSize(5000);
    canvas->setPaintBrush(preset);
    pump(200);
    check(QStringLiteral("(j) a 5000 preset lands in the ENGINE"),
          canvas->paintBrush().size() == 5000,
          QStringLiteral("engine=%1").arg(canvas->paintBrush().size()));
    check(QStringLiteral("(j) ...and the BAR displays 5000, not a clamp"),
          page->sizeCtlDisplayedSizeForTest() == 5000,
          QStringLiteral("bar=%1").arg(page->sizeCtlDisplayedSizeForTest()));

    // The Size CTL path itself: the bar's own setter spans the range.
    canvas->setBrushToolSize(5000);
    check(QStringLiteral("(j) setBrushToolSize(5000) holds in the engine"),
          canvas->paintBrush().size() == 5000);

    // The ERASER keeps its 1..200 world, and coming back from it must not
    // squash the brush's 5000 through the eraser's clamp.
    canvas->setTool(DrawingCanvas::Eraser);
    pump(200);
    const int eraserShown = page->sizeCtlDisplayedSizeForTest();
    check(QStringLiteral("(j) eraser mode shows an eraser-range value"),
          eraserShown >= 1 && eraserShown <= 200,
          QStringLiteral("bar=%1").arg(eraserShown));
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    check(QStringLiteral("(j) back to Brush: 5000 SURVIVED the eraser "
                         "round-trip"),
          page->sizeCtlDisplayedSizeForTest() == 5000
              && canvas->paintBrush().size() == 5000,
          QStringLiteral("bar=%1 engine=%2")
              .arg(page->sizeCtlDisplayedSizeForTest())
              .arg(canvas->paintBrush().size()));

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (i) a save that CANNOT fully happen must not look like one ----------
// Every write in the save used to be unchecked: QDir::mkpath, every
// QImage::save, and QFile::write's byte count. A save that could not write
// its pixels still produced a complete manifest NAMING them, returned true,
// and called setClean() - so the title's [*] cleared and the artist could
// close the app on work that never reached disk. The loader's deliberate
// null-fill (a missing image becomes a transparent layer at the PANEL's
// size, so a genuinely damaged old project still opens) then made the
// result look entirely healthy: right dimensions, blank art, no complaint.
//
// Two of the three assertions below are ABSENCE assertions - "the manifest
// did not change" - and an absence assertion is worthless without a control
// proving the same comparison can see a change. That control runs first.
void runSaveFailurePass(const QString &scratch)
{
    out() << "--- (i) a save that cannot finish must fail loudly ---" << Qt::endl;
    const QString dir = scratch + QStringLiteral("/savefail");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);

    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);

    // A save that FAILS warns, and that warning is a modal this run cannot
    // click. Every failing save below is wrapped in this dismisser.
    QTimer dismisser;
    dismisser.setInterval(120);
    QObject::connect(&dismisser, &QTimer::timeout, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });

    const QString fixture = writeProject(scratch + QStringLiteral("/savefail_src"),
                                         QStringLiteral("Source"),
                                         QSize(960, 540), 24, 1, 2);
    const QString proj = dir + QStringLiteral("/Work.sankotv");
    check(QStringLiteral("(i) fixture opens"), window.loadProjectForTest(fixture));
    pump(400);
    check(QStringLiteral("(i) first save succeeds"),
          window.saveProjectForTest(proj));
    pump(300);

    auto manifest = [&proj] {
        QFile f(proj);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
        return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
    };
    // The frame rate is stored IN the manifest, which the pixels are not.
    // That matters more than it looks - see editVisibly below.
    auto manifestFps = [&proj]() -> int {
        QFile f(proj);
        if (!f.open(QIODevice::ReadOnly))
            return -1;
        return QJsonDocument::fromJson(f.readAll())
            .object()
            .value(QStringLiteral("fps"))
            .toInt(-1);
    };
    auto paint = [&window] {
        auto *canvas = window.findChild<DrawingCanvas *>();
        if (!canvas)
            return;
        const QTransform t = canvas->viewTransformForTest();
        sendMouse(canvas, QEvent::MouseButtonPress, t.map(QPointF(140, 120)),
                  Qt::LeftButton);
        for (int i = 1; i <= 8; ++i)
            sendMouse(canvas, QEvent::MouseMove,
                      t.map(QPointF(140 + i * 22, 120 + i * 16)), Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, t.map(QPointF(316, 248)),
                  Qt::LeftButton);
        pump(700);
    };

    // An edit the MANIFEST would record, not just the pixels.
    //
    // This distinction is the whole reason the control below exists. The
    // first version of this section painted a stroke and asserted the
    // manifest hash changed - and it does NOT: artwork lives in the PNGs,
    // and the JSON beside them is byte-identical before and after a stroke.
    // Which means "the manifest is unchanged" would have passed on a
    // BROKEN build too, for a reason that has nothing to do with the fix.
    // Changing the frame rate puts a difference where the comparison can
    // actually see one, so "unchanged" becomes a real assertion.
    auto editVisibly = [&window, &paint](int fps) {
        window.applyProjectSettingsForTest(window.projectNameForTest(), fps);
        paint(); // and real pixels at stake as well
    };

    // POSITIVE CONTROL, FIRST. Every "manifest unchanged" check below is an
    // absence assertion; if this same comparison cannot detect a real edit
    // then they prove nothing at all.
    const QByteArray beforeControl = manifest();
    check(QStringLiteral("(i) control: the fixture really has a manifest"),
          !beforeControl.isEmpty());
    editVisibly(30);
    const bool controlSaved = window.saveProjectForTest(proj);
    pump(200);
    check(QStringLiteral("(i) CONTROL: a real edit + save DOES change the "
                         "manifest hash"),
          controlSaved && manifest() != beforeControl && manifestFps() == 30,
          QStringLiteral("fps on disk = %1").arg(manifestFps()));

    // --- (i.1) mkpath blocked: a FILE sits where _assets must go ---------
    // The folder is perfectly writable, so the manifest write would happily
    // succeed - this is the case where an unchecked mkpath produced a
    // manifest naming forty PNGs that were never written.
    const QString assetsDir = dir + QStringLiteral("/Work_assets");
    QDir(assetsDir).removeRecursively();
    {
        QFile blocker(assetsDir); // a FILE named exactly like the folder
        blocker.open(QIODevice::WriteOnly);
        blocker.write("not a folder");
        blocker.close();
    }
    check(QStringLiteral("(i) control: the assets path is now a FILE, not a "
                         "folder"),
          QFileInfo(assetsDir).isFile() && !QFileInfo(assetsDir).isDir());

    editVisibly(60); // work to lose, AND a change the manifest would record
    check(QStringLiteral("(i) control: the project is dirty before the failing "
                         "save"),
          window.isDirty());
    const QByteArray beforeBlocked = manifest();

    dismisser.start();
    const bool okBlocked = window.saveProjectForTest(proj);
    dismisser.stop();
    pump(200);

    check(QStringLiteral("(i.1) mkpath blocked: the save REPORTS failure"),
          !okBlocked);
    check(QStringLiteral("(i.1) ...the manifest on disk is UNCHANGED"),
          manifest() == beforeBlocked);
    check(QStringLiteral("(i.1) ...still recording the LAST SAVED frame rate, "
                         "not the unsaved one"),
          manifestFps() == 30,
          QStringLiteral("fps on disk = %1, in memory = 60").arg(manifestFps()));
    check(QStringLiteral("(i.1) ...and the project is STILL DIRTY"),
          window.isDirty(),
          window.isDirty() ? QString()
                           : QStringLiteral("clean - the artist could close "
                                            "on unsaved work"));

    QFile::remove(assetsDir); // let the next case have its folder back

    // --- (i.2) one image of many cannot be written -----------------------
    // A directory standing where a PNG must go: QImage::save cannot write
    // it, whatever else on the disk is fine. This is disk-full-at-file-17
    // and antivirus-holds-one-file, without needing either.
    check(QStringLiteral("(i) a good save in between restores the baseline"),
          window.saveProjectForTest(proj));
    pump(300);
    const QString onePng = assetsDir + QStringLiteral("/panel_s0_p0_layer1.png");
    QFile::remove(onePng);
    QDir().mkpath(onePng); // a DIRECTORY where the image belongs
    check(QStringLiteral("(i) control: one image path is now a directory"),
          QFileInfo(onePng).isDir());

    editVisibly(48);
    check(QStringLiteral("(i) control: dirty again before the failing save"),
          window.isDirty());
    const QByteArray beforeOne = manifest();
    const int fpsOnDiskBeforeOne = manifestFps();

    dismisser.start();
    const bool okOne = window.saveProjectForTest(proj);
    dismisser.stop();
    pump(200);

    check(QStringLiteral("(i.2) one unwritable image: the save REPORTS "
                         "failure"),
          !okOne);
    check(QStringLiteral("(i.2) ...the manifest on disk is UNCHANGED"),
          manifest() == beforeOne);
    check(QStringLiteral("(i.2) ...still recording the LAST SAVED frame rate, "
                         "not the unsaved one"),
          manifestFps() == fpsOnDiskBeforeOne && manifestFps() != 48,
          QStringLiteral("fps on disk = %1, in memory = 48").arg(manifestFps()));
    check(QStringLiteral("(i.2) ...and the project is STILL DIRTY"),
          window.isDirty(),
          window.isDirty() ? QString()
                           : QStringLiteral("clean - the artist could close "
                                            "on unsaved work"));

    // --- (i.3) the manifest itself cannot be written ---------------------
    // A directory where the .sankotv goes. The images all write fine, so
    // this is specifically the manifest leg: it must not clear the flag
    // either, and QSaveFile must leave nothing half-written behind.
    QDir(onePng).removeRecursively();
    check(QStringLiteral("(i) a good save in between restores the baseline"),
          window.saveProjectForTest(proj));
    pump(300);

    const QString blockedProj = dir + QStringLiteral("/Blocked.sankotv");
    QDir().mkpath(blockedProj);
    paint();
    dismisser.start();
    const bool okManifest = window.saveProjectForTest(blockedProj);
    dismisser.stop();
    pump(200);

    check(QStringLiteral("(i.3) unwritable manifest: the save REPORTS "
                         "failure"),
          !okManifest);
    check(QStringLiteral("(i.3) ...and the project is STILL DIRTY"),
          window.isDirty());
    check(QStringLiteral("(i.3) ...and QSaveFile left no temp file behind"),
          QDir(dir).entryList({QStringLiteral("*.sankotv.*")},
                              QDir::Files).isEmpty(),
          QDir(dir).entryList({QStringLiteral("*.sankotv.*")},
                              QDir::Files).join(QStringLiteral(", ")));

    // --- (i.4) a failed Save As must not rename the open project ---------
    const QString nameBefore = window.projectNameForTest();
    const QString pathBefore = window.projectPathForTest();
    check(QStringLiteral("(i) control: the project has a name and a path"),
          !nameBefore.isEmpty() && !pathBefore.isEmpty());
    dismisser.start();
    window.saveProjectForTest(blockedProj); // still a directory
    dismisser.stop();
    pump(200);
    check(QStringLiteral("(i.4) a failed save leaves the project PATH alone"),
          window.projectPathForTest() == pathBefore);

    // --- (i.5) and after all that, a good save still works ---------------
    // Without this the whole section could pass on a save that is simply
    // broken for every input.
    check(QStringLiteral("(i.5) a normal save still succeeds afterwards"),
          window.saveProjectForTest(proj));
    check(QStringLiteral("(i.5) ...and THAT one does clear the dirty flag"),
          !window.isDirty());

    window.markCleanForTest();
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
    // Every settings read/write in app code goes through sankoSettings();
    // point the store at scratch so the family can NEVER touch the
    // user's real settings, driven or not.
    sankoSettingsSetOverrideForTest(scratch
                                    + QStringLiteral("/sanko_settings.ini"));
    // BEFORE the first MainWindow: the Recorder singleton reads its output
    // root ONCE in its constructor, and its settings-based override uses
    // the two-argument QSettings form that ignores the scratch redirect.
    // The env var is the only redirect it honours unconditionally.
    qputenv("SANKOTV_DEVREC_DIR",
            (scratch + QStringLiteral("/devrec")).toUtf8());
    // Snapshot the REAL settings store (org SankoTV) before anything runs.
    // Constructing the two-argument form here is DELIBERATE - it is the
    // verification instrument for the store the app must never touch while
    // sankoSettings() is overridden. Read-only.
    QMap<QString, QVariant> realStoreBefore;
    {
        const QSettings real(QStringLiteral("SankoTV"),
                             QStringLiteral("SankoTV"));
        for (const QString &k : real.allKeys())
            realStoreBefore.insert(k, real.value(k));
    }
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
    runClosePass(b, c, scratch);
    // NOT project b: runClosePass deliberately deletes its folder to make a
    // save fail, so opening it again here would fail for a reason that has
    // nothing to do with the view.
    runViewResetPass(a, c);
    runSaveAsIndependencePass(scratch);
    runSaveFailurePass(scratch);
    runSizeCtlAgreementPass(scratch);
    runProvenancePass(scratch);

    // ---- (l) sankoSettings: scratch lands, the real store does not ----
    // Both halves asserted: landing in scratch is only half the guarantee,
    // and the real-store half is the one that would have caught the probe
    // contamination (a redirect that silently failed while everything
    // appeared to work).
    out() << "--- (l) sankoSettings honours the scratch override ---"
          << Qt::endl;
    sankoSettings().setValue(QStringLiteral("scratchProbe/sentinel"), 0x5EA1);
    {
        QSettings ini(scratch + QStringLiteral("/sanko_settings.ini"),
                      QSettings::IniFormat);
        check(QStringLiteral("(l) a helper write LANDS in the scratch ini"),
              ini.value(QStringLiteral("scratchProbe/sentinel")).toInt()
                  == 0x5EA1);
    }
    {
        const QSettings real(QStringLiteral("SankoTV"),
                             QStringLiteral("SankoTV"));
        check(QStringLiteral("(l) ...and NOT in the real store"),
              !real.contains(QStringLiteral("scratchProbe/sentinel")));
        check(QStringLiteral("(l) control: the real store is readable and "
                             "non-empty (the comparison can see keys)"),
              !realStoreBefore.isEmpty(),
              QStringLiteral("%1 key(s)").arg(realStoreBefore.size()));
        QMap<QString, QVariant> realStoreAfter;
        for (const QString &k : real.allKeys())
            realStoreAfter.insert(k, real.value(k));
        check(QStringLiteral("(l) the ENTIRE family changed NOTHING in the "
                             "real store (keys and values identical)"),
              realStoreAfter == realStoreBefore,
              QStringLiteral("%1 -> %2 key(s)")
                  .arg(realStoreBefore.size()).arg(realStoreAfter.size()));
    }

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
