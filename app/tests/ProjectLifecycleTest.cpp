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
#include "DashboardPage.h"
#include "DrawingCanvas.h"
#include "FloatingToolWindow.h"
#include "PerspectiveTool.h"
#include "MainWindow.h"
#include "StoryboardPage.h"
#include "RecentProjectsView.h"
#include "RecentThumbnails.h"
#include "RealStoreView.h"
#include "brushlib/BrushLibraryPanel.h"
#include "brushlib/BrushLibraryModel.h"
#include "brushlib/BrushPresetCodec.h"
#include "brushlib/BrushSettingsStudio.h"
#include "brushlib/BrushWidthRatio.h"
#include "brushlib/BuiltinRoster.h"
#include "StrokeBuilder.h"
#include "NewProjectDialog.h"
#include "ProjectIO.h"
#include "RecentProjects.h"
#include "SankoTheme.h"
#include "StoryboardModel.h"
#include "devrecorder/DevRecorder.h"

#include "AnimaticTimeline.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDataStream>
#include <QDockWidget>
#include <QEnterEvent>
#include <QMainWindow>
#include <QMenu>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QCryptographicHash>
#include <QDir>
#include <QThread>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
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
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QScrollArea>
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

    // The animatic used to receive the scenes only when the user NAVIGATED
    // to its screen, so this check had to click "Continue to Animatic"
    // first or the duration change was a no-op on an empty list. There is
    // no such screen now: the animatic is the timeline in the workspace and
    // holds the scenes from the moment the project opens. The control keeps
    // the old point - a duration change against an EMPTY animatic would
    // pass vacuously - by proving it is not empty, with no navigation.
    check(QStringLiteral("(d) control: the animatic already holds the "
                         "project's panels, with no screen to visit"),
          animatic->itemCountForTest() > 0,
          QStringLiteral("%1 panel(s)").arg(animatic->itemCountForTest()));
    // A duration change is an UNDOABLE command now, so it reaches the flag
    // through the undo-stack backstop rather than a documentChanged of its
    // own; this is the request the timeline's drag makes.
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


// ---- (t) the recent-projects store ----------------------------------------
// The list a returning artist sees first. Its behaviour was checked once, by
// a seam, when the New Project dialog was built, and never by the gate - so
// this pins both WHAT it does and the SHAPE it stores, because the shape is
// what an existing user's list depends on: a change to the key or the field
// names would orphan every list already on disk and nothing else would say.
void runRecentsStorePass(const QString &scratch)
{
    out() << "--- (t) the recent-projects store ---" << Qt::endl;
    const QString ini = scratch + QStringLiteral("/recents_store.ini");
    QFile::remove(ini);
    RecentProjects::setSettingsOverride(ini);

    check(QStringLiteral("(t) control: a fresh store reads as empty"),
          RecentProjects::entries().isEmpty());

    const QString a = QStringLiteral("C:/Scratch/Alpha/Alpha.sankotv");
    const QString b = QStringLiteral("C:/Scratch/Beta/Beta.sankotv");
    RecentProjects::record(a);
    RecentProjects::record(b);
    QVector<RecentProjects::Entry> list = RecentProjects::entries();
    check(QStringLiteral("(t) the most recently recorded project is FIRST"),
          list.size() == 2 && list.at(0).path == b && list.at(1).path == a);
    check(QStringLiteral("(t) each entry is stamped with when it was recorded"),
          list.size() == 2
              && qAbs(list.at(0).lastOpened.secsTo(
                     QDateTime::currentDateTime())) < 30);

    // The same file reached by a different spelling is ONE project.
    RecentProjects::record(a.toUpper());
    list = RecentProjects::entries();
    check(QStringLiteral("(t) re-recording moves a project to the top "
                         "without duplicating it (case-insensitive)"),
          list.size() == 2 && list.at(0).path == a.toUpper()
              && list.at(1).path == b,
          QStringLiteral("%1 entr%2").arg(list.size())
              .arg(list.size() == 1 ? "y" : "ies"));

    for (int i = 0; i < 12; ++i)
        RecentProjects::record(
            QStringLiteral("C:/Scratch/P%1/P%1.sankotv").arg(i));
    list = RecentProjects::entries();
    check(QStringLiteral("(t) the list is capped at %1, newest kept")
              .arg(RecentProjects::kCap),
          list.size() == RecentProjects::kCap
              && list.first().path.endsWith(QStringLiteral("P11.sankotv"))
              && list.last().path.endsWith(QStringLiteral("P2.sankotv")),
          QStringLiteral("%1 entries, first %2, last %3")
              .arg(list.size())
              .arg(list.isEmpty() ? QString() : list.first().path,
                   list.isEmpty() ? QString() : list.last().path));

    RecentProjects::remove(QStringLiteral("c:/scratch/p7/P7.SANKOTV"));
    list = RecentProjects::entries();
    bool hasP7 = false, orderKept = true;
    for (int i = 0; i < list.size(); ++i) {
        hasP7 = hasP7 || list.at(i).path.contains(QStringLiteral("/P7/"));
        if (i > 0)
            orderKept = orderKept
                && list.at(i - 1).lastOpened >= list.at(i).lastOpened;
    }
    check(QStringLiteral("(t) remove drops exactly that project "
                         "(case-insensitive) and keeps the rest in order"),
          list.size() == RecentProjects::kCap - 1 && !hasP7 && orderKept);
    RecentProjects::remove(QStringLiteral("C:/Scratch/Nowhere/None.sankotv"));
    check(QStringLiteral("(t) removing a project that is not listed changes "
                         "nothing"),
          RecentProjects::entries().size() == RecentProjects::kCap - 1);

    // THE STORED SHAPE. Read back raw, not through the store, so this
    // fails if the store and its reader ever change together.
    {
        QSettings raw(ini, QSettings::IniFormat);
        const int n = raw.beginReadArray(QStringLiteral("recentProjects"));
        raw.setArrayIndex(0);
        const QStringList keys = raw.childKeys();
        const QString firstPath = raw.value(QStringLiteral("path")).toString();
        const QDateTime firstStamp = QDateTime::fromString(
            raw.value(QStringLiteral("lastOpened")).toString(), Qt::ISODate);
        raw.endArray();
        check(QStringLiteral("(t) on disk: array \"recentProjects\", each "
                             "entry exactly {path, lastOpened as an ISO date}"),
              n == RecentProjects::kCap - 1 && keys.size() == 2
                  && keys.contains(QStringLiteral("path"))
                  && keys.contains(QStringLiteral("lastOpened"))
                  && firstPath == list.first().path && firstStamp.isValid(),
              QStringLiteral("%1 entries, keys: %2").arg(n)
                  .arg(keys.join(QLatin1Char(','))));
    }

    // AN EXISTING USER'S LIST. Written here the way every build before the
    // store moved out of the dialog wrote it - raw, with no help from the
    // code under test - and it must read back entry for entry.
    {
        const QString legacy = scratch + QStringLiteral("/recents_legacy.ini");
        QFile::remove(legacy);
        const QStringList paths = {
            QStringLiteral("C:/Users/Artist/Documents/SankoTV/One/One.sankotv"),
            QStringLiteral("D:/Work/Two With Spaces/Two.sankotv"),
            QStringLiteral("C:/Gone/Three.sankotv")};
        const QStringList stamps = {QStringLiteral("2026-09-30T19:10:29"),
                                    QStringLiteral("2026-08-28T18:16:17"),
                                    QStringLiteral("2026-08-24T22:02:34")};
        {
            QSettings s(legacy, QSettings::IniFormat);
            s.beginWriteArray(QStringLiteral("recentProjects"), 3);
            for (int i = 0; i < 3; ++i) {
                s.setArrayIndex(i);
                s.setValue(QStringLiteral("path"), paths.at(i));
                s.setValue(QStringLiteral("lastOpened"), stamps.at(i));
            }
            s.endArray();
        }
        RecentProjects::setSettingsOverride(legacy);
        const QVector<RecentProjects::Entry> read = RecentProjects::entries();
        bool same = read.size() == 3;
        for (int i = 0; same && i < 3; ++i)
            same = read.at(i).path == paths.at(i)
                && read.at(i).lastOpened.toString(Qt::ISODate) == stamps.at(i);
        check(QStringLiteral("(t) a list written by an OLDER build reads back "
                             "entry for entry: paths, order and dates"),
              same, QStringLiteral("%1 of 3 read").arg(read.size()));
        check(QStringLiteral("(t) ...including an entry whose file is gone "
                             "(it is listed, not dropped)"),
              read.size() == 3 && read.at(2).path == paths.at(2));
    }

    RecentProjects::setSettingsOverride(scratch
                                        + QStringLiteral("/recents.ini"));
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


// ---- (u) a recent project's thumbnail comes from ITS OWN manifest ---------
// The defect: the recents list looked for "panel_s0_p0.png" beside the
// project file, which is where a first panel's flatten lived until saves
// moved images into "<basename>_assets/". From then on a freshly saved
// project showed no thumbnail, and a project sharing a folder with an older
// flat save showed THAT file - the same picture for every project in the
// folder, and belonging to none of them. It survived a month because no
// gate ever asked which file the list was reading.
//
// Every project here is written through the real ProjectIO save, so the
// layout under test is the one the app produces, not one this file assumes.
void runRecentThumbnailSourcePass(const QString &scratch)
{
    out() << "--- (u) the recents thumbnail is the manifest's first panel ---"
          << Qt::endl;
    const QString root = scratch + QStringLiteral("/thumb_source");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    // One solid-colour panel per project, so "whose picture is this" has a
    // one-pixel answer.
    auto saveSolid = [](const QString &projectPath, const QColor &color) {
        Scene *scene = new Scene;
        scene->number = 1;
        Panel *panel = makeBlankPanel(QSize(960, 540));
        panel->layers[1].image.fill(color);
        scene->panels.append(panel);
        ProjectIO::SaveData data;
        data.projectName = QFileInfo(projectPath).completeBaseName();
        data.fps = 24;
        data.canvasSize = QSize(960, 540);
        data.scenes = {scene};
        const ProjectIO::WriteResult w = ProjectIO::projectToJson(data, projectPath);
        QFile f(projectPath);
        if (w.ok && f.open(QIODevice::WriteOnly))
            f.write(QJsonDocument(w.root).toJson(QJsonDocument::Indented));
        f.close();
        delete scene;
        return w.ok;
    };
    auto centre = [](const QString &png) {
        const QImage img(png);
        return img.isNull() ? QColor() : img.pixelColor(img.width() / 2,
                                                         img.height() / 2);
    };

    // 1. A project saved today.
    const QString fresh = root + QStringLiteral("/Fresh/Fresh.sankotv");
    QDir().mkpath(QFileInfo(fresh).absolutePath());
    check(QStringLiteral("(u) fixture: a project saved through the real "
                         "save path"),
          saveSolid(fresh, QColor(200, 30, 30)));
    const QString freshThumb = RecentProjects::thumbnailSource(fresh);
    check(QStringLiteral("(u) control: the OLD location is empty for it - "
                         "the reason new projects showed no thumbnail"),
          !QFileInfo::exists(QFileInfo(fresh).absolutePath()
                             + QStringLiteral("/panel_s0_p0.png")));
    check(QStringLiteral("(u) the thumbnail is the file its manifest names, "
                         "inside its own assets folder"),
          freshThumb.endsWith(QStringLiteral("/Fresh_assets/panel_s0_p0.png"))
              && QFileInfo::exists(freshThumb),
          freshThumb);
    check(QStringLiteral("(u) ...and it is that project's picture"),
          centre(freshThumb) == QColor(200, 30, 30));

    // 2. Two projects in ONE folder, beside a stale flat file from an older
    //    save: the arrangement in which the list showed the wrong picture.
    const QString sharedDir = root + QStringLiteral("/Shared");
    QDir().mkpath(sharedDir);
    const QString stale = sharedDir + QStringLiteral("/panel_s0_p0.png");
    {
        QImage old(960, 540, QImage::Format_ARGB32_Premultiplied);
        old.fill(QColor(20, 20, 20));
        old.save(stale, "PNG");
    }
    const QString first = sharedDir + QStringLiteral("/Board.sankotv");
    const QString copy = sharedDir + QStringLiteral("/Board_A.sankotv");
    const bool sharedSaved = saveSolid(first, QColor(30, 160, 60))
        && saveSolid(copy, QColor(40, 60, 210));
    check(QStringLiteral("(u) control: the stale flat file is there, and is "
                         "NEITHER project's picture"),
          sharedSaved && QFileInfo::exists(stale)
              && centre(stale) == QColor(20, 20, 20));
    const QString firstThumb = RecentProjects::thumbnailSource(first);
    const QString copyThumb = RecentProjects::thumbnailSource(copy);
    check(QStringLiteral("(u) neither project in a shared folder resolves to "
                         "the stale flat file"),
          QFileInfo(firstThumb) != QFileInfo(stale)
              && QFileInfo(copyThumb) != QFileInfo(stale),
          firstThumb + QStringLiteral(" | ") + copyThumb);
    check(QStringLiteral("(u) each resolves to ITS OWN first panel"),
          firstThumb != copyThumb && centre(firstThumb) == QColor(30, 160, 60)
              && centre(copyThumb) == QColor(40, 60, 210));

    // 3. A project from before the assets folder: its manifest names the
    //    flat file, and that must still be found where it always was.
    const QString legacyDir = root + QStringLiteral("/Legacy");
    QDir().mkpath(legacyDir);
    const QString legacy = legacyDir + QStringLiteral("/Old.sankotv");
    {
        QImage flat(960, 540, QImage::Format_ARGB32_Premultiplied);
        flat.fill(QColor(210, 180, 40));
        flat.save(legacyDir + QStringLiteral("/panel_s0_p0.png"), "PNG");
        QJsonObject panel{{QStringLiteral("pixmapFile"),
                           QStringLiteral("panel_s0_p0.png")}};
        QJsonObject scene{{QStringLiteral("panels"), QJsonArray{panel}}};
        QJsonObject manifest{{QStringLiteral("version"), 1},
                             {QStringLiteral("scenes"), QJsonArray{scene}}};
        QFile f(legacy);
        if (f.open(QIODevice::WriteOnly))
            f.write(QJsonDocument(manifest).toJson());
    }
    const QString legacyThumb = RecentProjects::thumbnailSource(legacy);
    check(QStringLiteral("(u) a pre-assets project still finds its flat "
                         "file, because its manifest says so"),
          QFileInfo(legacyThumb)
                  == QFileInfo(legacyDir + QStringLiteral("/panel_s0_p0.png"))
              && centre(legacyThumb) == QColor(210, 180, 40),
          legacyThumb);

    // 4. No panel yet, and no file at all.
    const QString empty = root + QStringLiteral("/Empty.sankotv");
    {
        QJsonObject manifest{{QStringLiteral("version"), 1},
                             {QStringLiteral("scenes"), QJsonArray()}};
        QFile f(empty);
        if (f.open(QIODevice::WriteOnly))
            f.write(QJsonDocument(manifest).toJson());
    }
    check(QStringLiteral("(u) a project with no panel yet has no thumbnail "
                         "(not a guessed one)"),
          RecentProjects::thumbnailSource(empty).isEmpty());
    check(QStringLiteral("(u) a project whose file is gone has none either"),
          RecentProjects::thumbnailSource(root + QStringLiteral("/Gone.sankotv"))
              .isEmpty());

    // 5. The answer is remembered until the project file changes: a list
    //    repaints on every hover and must not parse a manifest each time.
    const int readsBefore = RecentProjects::manifestReadsForTest();
    for (int i = 0; i < 200; ++i)
        RecentProjects::thumbnailSource(fresh);
    check(QStringLiteral("(u) 200 repeat lookups open the manifest ZERO more "
                         "times"),
          RecentProjects::manifestReadsForTest() == readsBefore,
          QStringLiteral("%1 extra read(s)")
              .arg(RecentProjects::manifestReadsForTest() - readsBefore));
    // ...and the control that the counter can move: change the file.
    {
        QJsonObject panel{{QStringLiteral("pixmapFile"),
                           QStringLiteral("Fresh_assets/panel_s0_p0.png")}};
        QJsonObject other{{QStringLiteral("pixmapFile"),
                           QStringLiteral("elsewhere/first.png")}};
        QJsonObject scene{{QStringLiteral("panels"), QJsonArray{other, panel}}};
        QJsonObject manifest{{QStringLiteral("version"), 1},
                             {QStringLiteral("scenes"), QJsonArray{scene}}};
        QFile f(fresh);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(manifest).toJson());
        f.close();
        // An explicit, different timestamp: two writes inside one clock
        // tick must not be what decides this check.
        if (f.open(QIODevice::ReadWrite)) {
            f.setFileTime(QDateTime::currentDateTime().addSecs(5),
                          QFileDevice::FileModificationTime);
            f.close();
        }
    }
    const QString moved = RecentProjects::thumbnailSource(fresh);
    check(QStringLiteral("(u) a CHANGED project file is read again, once, "
                         "and the new first panel is the answer"),
          RecentProjects::manifestReadsForTest() == readsBefore + 1
              && moved.endsWith(QStringLiteral("/elsewhere/first.png")),
          moved);
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


// ---- (v) EVERY way to a new project asks about unsaved work, once ---------
// The defect: File > New Project asked, then raised the Dashboard's signal;
// the Dashboard's own New Project button was wired straight to the dialog.
// The button is reachable with a project open (Back from the Script Editor),
// so it replaced an unsaved project without a word.
//
// The modal itself cannot be clicked here, so the ANSWER is supplied through
// setDiscardPromptForTest - but whether the question is ASKED, and how many
// times, is the real code, and that is what is counted. "Exactly once"
// matters as much as "at all": the obvious repair (guard both entrances
// where they stand) asks twice on the menu route.
void runNewProjectPromptPass(const QString &project)
{
    out() << "--- (v) a new project always asks about unsaved work ---"
          << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    check(QStringLiteral("(v) project opens"), window.loadProjectForTest(project));
    pump(400);
    const QString openPath = window.projectPathForTest();

    QPushButton *newButton = nullptr;
    if (auto *dashboard = window.findChild<DashboardPage *>())
        for (QPushButton *b : dashboard->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("New Project"))
                newButton = b;
    QAction *newAction = nullptr;
    for (QAction *a : window.findChildren<QAction *>())
        if (a->text() == QStringLiteral("New Project..."))
            newAction = a;
    check(QStringLiteral("(v) found the Dashboard's New Project button and "
                         "the File > New Project action"),
          newButton && newAction);
    if (!newButton || !newAction)
        return;

    // The dialog is modal: something has to notice it and close it, or a
    // dialog that opens when it should (or should not) hangs the gate.
    int dialogsSeen = 0;
    QTimer watcher;
    watcher.setInterval(60);
    QObject::connect(&watcher, &QTimer::timeout, [&dialogsSeen] {
        if (auto *dialog = qobject_cast<NewProjectDialog *>(
                QApplication::activeModalWidget())) {
            ++dialogsSeen;
            dialog->reject();
        }
    });
    watcher.start();

    // Positive control FIRST: with nothing to lose, the button reaches the
    // dialog and nothing is asked. This is what proves the watcher can see
    // a dialog, so that "no dialog appeared" below means something.
    window.markCleanForTest();
    window.setDiscardPromptForTest(
        [] { return MainWindow::DiscardAnswer::Cancel; });
    newButton->click();
    pump(200);
    check(QStringLiteral("(v) control: a CLEAN project goes straight to the "
                         "dialog, unasked"),
          dialogsSeen == 1 && window.discardPromptCountForTest() == 0,
          QStringLiteral("dialogs %1, prompts %2").arg(dialogsSeen)
              .arg(window.discardPromptCountForTest()));

    // Now make it dirty, by a change the undo stack never sees.
    window.applyProjectSettingsForTest(QStringLiteral("EditedForPrompt"), 30);
    pump(150);
    check(QStringLiteral("(v) control: the project is dirty"), window.isDirty());

    // The Dashboard button, answered Cancel: asked once, and NOTHING else.
    dialogsSeen = 0;
    window.setDiscardPromptForTest(
        [] { return MainWindow::DiscardAnswer::Cancel; });
    newButton->click();
    pump(200);
    check(QStringLiteral("(v) the Dashboard button ASKS when there is "
                         "unsaved work"),
          window.discardPromptCountForTest() == 1,
          QStringLiteral("asked %1 time(s)")
              .arg(window.discardPromptCountForTest()));
    check(QStringLiteral("(v) ...and Cancel stops there: no dialog, the "
                         "project still open and still dirty"),
          dialogsSeen == 0 && window.projectPathForTest() == openPath
              && window.isDirty(),
          QStringLiteral("dialogs %1").arg(dialogsSeen));

    // The Dashboard button, answered Discard: asked once, then the dialog.
    dialogsSeen = 0;
    window.setDiscardPromptForTest(
        [] { return MainWindow::DiscardAnswer::Discard; });
    newButton->click();
    pump(200);
    check(QStringLiteral("(v) answered Discard, the button asks ONCE and "
                         "then shows the dialog"),
          window.discardPromptCountForTest() == 1 && dialogsSeen == 1,
          QStringLiteral("asked %1, dialogs %2")
              .arg(window.discardPromptCountForTest()).arg(dialogsSeen));

    // File > New Project: the same single question, not two.
    dialogsSeen = 0;
    window.setDiscardPromptForTest(
        [] { return MainWindow::DiscardAnswer::Discard; });
    newAction->trigger();
    pump(200);
    check(QStringLiteral("(v) File > New Project asks exactly ONCE too (it "
                         "does not ask, then ask again at the dialog)"),
          window.discardPromptCountForTest() == 1 && dialogsSeen == 1,
          QStringLiteral("asked %1, dialogs %2")
              .arg(window.discardPromptCountForTest()).arg(dialogsSeen));
    check(QStringLiteral("(v) a dialog that was cancelled changed nothing: "
                         "same project, still dirty"),
          window.projectPathForTest() == openPath && window.isDirty());

    watcher.stop();
    window.setDiscardPromptForTest({});
    window.markCleanForTest();
    window.close();
    pump(300);
}


// ---- (n) override marks in the panel + reset restores stock ---------------
// The user-visible half of (b9): tuning a built-in marks its row
// (Modified, the teal dot state), reset clears the mark, and activating
// the preset afterwards applies STOCK values to the canvas.
void runOverrideMarkPass(const QString &scratch)
{
    out() << "--- (n) override marks + reset to stock ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/omark"),
                                         QStringLiteral("OMark"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(n) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *canvas = window.findChild<DrawingCanvas *>();
    auto *panel = window.findChild<brushlib::BrushLibraryPanel *>();
    auto *model = window.findChild<brushlib::BrushLibraryModel *>();
    if (!canvas || !panel || !model) {
        check(QStringLiteral("(n) found canvas, panel and model"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    const QString id = QStringLiteral("builtin/painting/gouache");
    panel->selectCategoryForTest(QStringLiteral("Painting"));
    pump(200);
    check(QStringLiteral("(n) control: no mark before any override"),
          panel->overrideMarkForTest(id) == 0,
          QStringLiteral("mark=%1").arg(panel->overrideMarkForTest(id)));

    ::Brush tuned = model->preset(id)->brush;
    tuned.setSpacing(0.02);
    check(QStringLiteral("(n) tuning the built-in succeeds"),
          model->updateBrush(id, tuned));
    pump(300); // model 'changed' -> rebuild -> row mark refresh
    check(QStringLiteral("(n) the row shows the MODIFIED mark"),
          panel->overrideMarkForTest(id) == 1,
          QStringLiteral("mark=%1").arg(panel->overrideMarkForTest(id)));

    check(QStringLiteral("(n) reset to stock succeeds"),
          model->resetBuiltinToStock(id));
    pump(300);
    check(QStringLiteral("(n) ...and the mark clears"),
          panel->overrideMarkForTest(id) == 0,
          QStringLiteral("mark=%1").arg(panel->overrideMarkForTest(id)));

    panel->activatePresetForTest(id);
    pump(300);
    check(QStringLiteral("(n) activating afterwards applies STOCK (the "
                         "tuned spacing is gone)"),
          qAbs(canvas->paintBrush().spacing() - 0.02) > 0.001,
          QStringLiteral("spacing=%1").arg(canvas->paintBrush().spacing()));

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (w) a filled accent button's states come from the theme --------------
// The defect: when amber was retired the FILLS turned purple through
// %ACCENT%, but each button's hover/pressed/disabled colours were literals
// beside it and stayed amber - so purple buttons flashed orange under the
// pointer on five pages. Read from the live widgets, so it is the
// stylesheet the app actually applies that is being checked.
void runAccentStatesPass()
{
    out() << "--- (w) accent button states come from the theme ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(700);

    const QStringList amber = {QStringLiteral("#ffb733"),
                               QStringLiteral("#e0991c"),
                               QStringLiteral("#5a4416"),
                               QStringLiteral("#997a3a")};
    int styled = 0, withThemeHover = 0;
    QStringList offenders;
    QString newProjectSheet;
    for (QWidget *w : window.findChildren<QWidget *>()) {
        const QString sheet = w->styleSheet();
        if (sheet.isEmpty())
            continue;
        ++styled;
        if (sheet.contains(SankoTheme::kAccentHoverHex, Qt::CaseInsensitive))
            ++withThemeHover;
        for (const QString &literal : amber)
            if (sheet.contains(literal, Qt::CaseInsensitive))
                offenders << QStringLiteral("%1 \"%2\" has %3")
                                 .arg(QString::fromLatin1(
                                          w->metaObject()->className()),
                                      w->property("text").toString(), literal);
        if (auto *button = qobject_cast<QPushButton *>(w))
            if (button->text() == QStringLiteral("New Project"))
                newProjectSheet = sheet;
    }
    // The control: the walk reaches styled widgets at all, and reaches the
    // filled buttons specifically - an empty walk would "find no amber" too.
    check(QStringLiteral("(w) control: the walk sees the app's stylesheets, "
                         "including several filled accent buttons"),
          styled > 20 && withThemeHover >= 3,
          QStringLiteral("%1 styled widgets, %2 with the theme hover")
              .arg(styled).arg(withThemeHover));
    check(QStringLiteral("(w) no widget carries a retired amber state colour"),
          offenders.isEmpty(), offenders.join(QStringLiteral("; ")));
    check(QStringLiteral("(w) the start page's New Project button hovers and "
                         "presses in the theme's colours"),
          newProjectSheet.contains(SankoTheme::kAccentHoverHex)
              && newProjectSheet.contains(SankoTheme::kAccentPressedHex),
          newProjectSheet.right(120));
    check(QStringLiteral("(w) no stylesheet is left with an unresolved "
                         "%TOKEN%"),
          !SankoTheme::themed("%ACCENT_HOVER% %ACCENT_PRESSED% "
                              "%ACCENT_DISABLED% %ACCENT_DISABLED_TEXT% "
                              "%ACCENT_LIGHT% %ACCENT_RGB% %ACCENT% %PURPLE%")
               .contains(QLatin1Char('%')));

    // The states are still the ACCENT: same hue family, hover lighter and
    // pressed darker than the fill. (Amber sits ~210 degrees away.)
    auto hueGap = [](const QColor &a, const QColor &b) {
        const int d = qAbs(a.hslHue() - b.hslHue());
        return qMin(d, 360 - d);
    };
    check(QStringLiteral("(w) hover and pressed are the accent's own hue, "
                         "lighter and darker than the fill"),
          hueGap(SankoTheme::kAccentHover, SankoTheme::kAccent) < 12
              && hueGap(SankoTheme::kAccentPressed, SankoTheme::kAccent) < 12
              && SankoTheme::kAccentHover.lightness()
                     > SankoTheme::kAccent.lightness()
              && SankoTheme::kAccentPressed.lightness()
                     < SankoTheme::kAccent.lightness(),
          QStringLiteral("hover %1, fill %2, pressed %3")
              .arg(SankoTheme::kAccentHoverHex, SankoTheme::kAccentHex,
                   SankoTheme::kAccentPressedHex));
    check(QStringLiteral("(w) control: the same measure calls amber a "
                         "different colour"),
          hueGap(QColor(0xff, 0xb7, 0x33), SankoTheme::kAccent) > 90);

    window.close();
    pump(300);
}

// ---- (o) the Tip Shape preview IS the engine's tip -------------------------
// The studio's Tip Shape panel (Figma 358:22) claims one definition: the
// image it shows is StrokeBuilder::shapedTipForStamp's output, not a
// description of it. This pass pins that claim byte-for-byte, proves the
// preview refreshes through the REAL edit path, and proves the render is
// FIXED-RESOLUTION — brush size cannot touch it (the retired thumbnail's
// replacement must never buy legibility with a full-size tip build).
void runTipShapePreviewPass(const QString &scratch)
{
    out() << "--- (o) Tip Shape preview = engine tip ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/tipprev"),
                                         QStringLiteral("TipPrev"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(o) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *studio = window.findChild<brushlib::BrushSettingsStudio *>();
    auto *model = window.findChild<brushlib::BrushLibraryModel *>();
    if (!studio || !model) {
        check(QStringLiteral("(o) found studio and model"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    // The hardness step below needs a PROCEDURAL tip: hardness is INERT
    // with a custom tip (HANDOFF, dead-lever list), so since Gouache
    // gained its scan (2026-09-23) the fixture is Round Brush - still
    // procedural, hardness 0.45 live. Gouache stays in the pass as the
    // control that pins the inert rule at the preview: a hardness edit
    // on a stamped built-in changes NOTHING, while a roundness edit on
    // the same session does (the positive control).
    {
        const QString stamped = QStringLiteral("builtin/painting/gouache");
        studio->openForPreset(stamped);
        pump(400);
        const QImage before = studio->tipPreviewImageForTest();
        studio->editSessionForTest([](::Brush &b) { b.setHardness(0.05); },
                                   true);
        pump(100);
        // E3 (2026-09-26): hardness is LIVE on custom tips. A stamped
        // built-in ships at 1.0 (identity); editing it re-renders, and
        // setting it back to 1.0 restores the preview byte-exact.
        check(QStringLiteral("(o) E3: a hardness edit on a STAMPED built-in "
                             "re-renders the preview (hardness is live on "
                             "custom tips)"),
              !before.isNull() && studio->tipPreviewImageForTest() != before);
        studio->editSessionForTest([](::Brush &b) { b.setHardness(1.0); },
                                   true);
        pump(100);
        check(QStringLiteral("(o) ...control: hardness back to 1.0 restores "
                             "it byte-identical (1.0 = identity)"),
              studio->tipPreviewImageForTest() == before);
        // openForPreset on a DIFFERENT preset while visible with a dirty
        // session asks "Discard unsaved changes?" - a modal that answers
        // Cancel headlessly and leaves the old session in place. Hiding
        // first is the prompt-free path (pass (q) does the same).
        studio->hide();
        pump(300);
    }
    const QString id = QStringLiteral("builtin/painting/round-brush");
    studio->openForPreset(id);
    pump(400);

    // The test's OWN engine render, from the same preset the session
    // copied at open. The preview's pixel size is read back rather than
    // assumed so the check holds at any devicePixelRatio.
    const auto engineTip = [](const ::Brush &brush, int px) {
        ::Brush copy = brush;
        StrokeBuilder shaper(QSize(px, px), copy,
                             /*rasterizePreview=*/false);
        StrokeStamp stamp;
        stamp.effectiveSize = px;
        stamp.effectiveHardness = copy.hardness();
        stamp.roundness = 1.0;
        return shaper.shapedTipForStamp(stamp);
    };

    const QImage shown = studio->tipPreviewImageForTest();
    check(QStringLiteral("(o) preview holds an engine-format render"),
          !shown.isNull() && shown.format() == QImage::Format_Grayscale8
              && shown.width() == shown.height());
    int covered = 0;
    for (int y = 0; y < shown.height(); ++y) {
        const uchar *row = shown.constScanLine(y);
        for (int x = 0; x < shown.width(); ++x)
            if (row[x] > 0)
                ++covered;
    }
    check(QStringLiteral("(o) positive control: the tip is actually there"),
          covered > 1000, QStringLiteral("covered=%1").arg(covered));
    check(QStringLiteral("(o) ONE DEFINITION: preview == the test's own "
                         "shapedTipForStamp, byte for byte"),
          !shown.isNull()
              && shown == engineTip(model->preset(id)->brush, shown.width()));

    // DISPLAY-SIZE LAYER in the studio: the Size row shows the session
    // brush's VISIBLE width; the preset keeps its engine size; the number
    // FOLLOWS an edit that changes the mark (nothing remembered).
    {
        const ::Brush stock = model->preset(id)->brush;
        const int label =
            brushlib::BrushWidthRatio::displaySize(stock, stock.size());
        check(QStringLiteral("(o) the studio's Size row shows Round Brush's "
                             "visible width over its untouched engine size"),
              qRound(studio->sizeRowValueForTest()) == label
                  && label < stock.size()
                  && studio->sessionEngineSizeForTest() == stock.size(),
              QStringLiteral("row=%1 label=%2 engine=%3")
                  .arg(studio->sizeRowValueForTest()).arg(label)
                  .arg(studio->sessionEngineSizeForTest()));
        studio->sizeRowUserSetForTest(60.0);
        pump(50);
        check(QStringLiteral("(o) setting the row to 60 stores the engine "
                             "size that DRAWS 60 (60 / ratio)"),
              studio->sessionEngineSizeForTest()
                      == brushlib::BrushWidthRatio::engineSize(stock, 60)
                  && studio->sessionEngineSizeForTest() > 60
                  && qRound(studio->sizeRowValueForTest()) == 60,
              QStringLiteral("engine=%1 row=%2")
                  .arg(studio->sessionEngineSizeForTest())
                  .arg(studio->sizeRowValueForTest()));
        const int engineAt60 = studio->sessionEngineSizeForTest();
        // Retune the rim: the engine size stays, the mark widens, and the
        // row must say so without anyone telling it to - and WITHOUT the
        // UI thread measuring: the row goes pending, the measurer answers
        // from its own thread, the row updates when the answer lands.
        brushlib::BrushWidthRatio::clearMemoryCacheForTest(); // force it
        studio->editSessionForTest([](::Brush &b) { b.setHardness(1.0); },
                                   true);
        check(QStringLiteral("(o) OFF THE UI THREAD: right after the edit "
                             "the row is PENDING (nothing measured here)"),
              studio->sizeRowPendingForTest(),
              QStringLiteral("pending=%1").arg(studio->sizeRowPendingForTest()));
        QElapsedTimer settle;
        settle.start();
        while (studio->sizeRowPendingForTest() && settle.elapsed() < 15000)
            pump(20);
        check(QStringLiteral("(o) ...the answer lands and the row is no "
                             "longer pending"),
              !studio->sizeRowPendingForTest());
        check(QStringLiteral("(o) ...measured on a thread that is NOT the "
                             "UI thread"),
              brushlib::BrushWidthRatio::lastMeasureThreadForTest()
                      != nullptr
                  && brushlib::BrushWidthRatio::lastMeasureThreadForTest()
                      != QThread::currentThread());
        check(QStringLiteral("(o) STALENESS: a retuned rim changes the "
                             "row's number with the engine size untouched"),
              studio->sessionEngineSizeForTest() == engineAt60
                  && qRound(studio->sizeRowValueForTest()) != 60,
              QStringLiteral("engine=%1 row=%2")
                  .arg(studio->sessionEngineSizeForTest())
                  .arg(studio->sizeRowValueForTest()));
        // Put the session back exactly as the following checks expect it.
        studio->editSessionForTest(
            [stock](::Brush &b) {
                b.setHardness(stock.hardness());
                b.setSize(stock.size());
            },
            true);
        pump(100);
        check(QStringLiteral("(o) control: the session is the stock preset "
                             "again, byte for byte"),
              studio->sessionEngineSizeForTest() == stock.size()
                  && studio->tipPreviewImageForTest() == shown);
    }

    // Real time through the REAL edit path: a hardness edit lands in the
    // preview, and the identity holds at the new state too.
    studio->editSessionForTest([](::Brush &b) { b.setHardness(0.05); },
                               true);
    pump(100);
    const QImage soft = studio->tipPreviewImageForTest();
    check(QStringLiteral("(o) a hardness edit re-renders the preview"),
          !soft.isNull() && soft != shown);
    ::Brush softBrush = model->preset(id)->brush;
    softBrush.setHardness(0.05);
    check(QStringLiteral("(o) ...and the identity holds at the new state"),
          soft == engineTip(softBrush, soft.width()));

    // FIXED RESOLUTION: brush size is not a tip-render input. If this
    // fails, someone has coupled the preview to brush size and a 5000
    // brush is paying for a full-size tip build per slider tick.
    studio->editSessionForTest([](::Brush &b) { b.setSize(5000); }, true);
    pump(100);
    check(QStringLiteral("(o) size 5000 changes NOTHING in the preview "
                         "(fixed render resolution)"),
          studio->tipPreviewImageForTest() == soft);

    // The static transform reaches the preview (roundness squashes the
    // disc; angle alone would be invisible on a disc, so it rides along
    // with roundness where it has something to rotate).
    studio->editSessionForTest(
        [](::Brush &b) {
            b.setTipRoundness(0.4);
            b.setTipAngle(30.0);
        },
        false);
    pump(100);
    const QImage squashed = studio->tipPreviewImageForTest();
    check(QStringLiteral("(o) tip roundness+angle reach the preview"),
          !squashed.isNull() && squashed != soft);

    // A custom tip reaches the preview, and the flips act on it. The
    // flip-back involution is the control that keeps both comparisons
    // honest: different after one flip, byte-identical after two.
    QImage mask(32, 32, QImage::Format_Grayscale8);
    mask.fill(0);
    {
        QPainter mp(&mask);
        mp.fillRect(2, 2, 12, 12, Qt::white); // one corner: asymmetric
        mp.fillRect(4, 18, 24, 8, QColor(128, 128, 128));
    }
    studio->editSessionForTest(
        [&mask](::Brush &b) {
            b.setCustomShape(mask);
            b.setTipAngle(0.0);
            b.setTipRoundness(1.0);
        },
        true);
    pump(100);
    const QImage custom = studio->tipPreviewImageForTest();
    check(QStringLiteral("(o) a loaded custom tip reaches the preview"),
          !custom.isNull() && custom != squashed);
    studio->editSessionForTest([](::Brush &b) { b.setTipFlipX(true); },
                               false);
    pump(100);
    const QImage flipped = studio->tipPreviewImageForTest();
    check(QStringLiteral("(o) Flip Tip X changes an asymmetric tip"),
          !flipped.isNull() && flipped != custom);
    studio->editSessionForTest([](::Brush &b) { b.setTipFlipX(false); },
                               false);
    pump(100);
    check(QStringLiteral("(o) ...and flipping back restores it byte-exact "
                         "(involution control)"),
          studio->tipPreviewImageForTest() == custom);

    // Session edits only — nothing was committed, so the model must still
    // hold the stock recipes (the pass may not leave an override behind).
    check(QStringLiteral("(o) the session never touched the model"),
          model->overrideState(id)
                  == brushlib::BrushLibraryModel::OverrideState::None
              && model->overrideState(QStringLiteral("builtin/painting/gouache"))
                  == brushlib::BrushLibraryModel::OverrideState::None);

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (x) the recorder's keys reach a REAL dialog --------------------------
// SankoDevRecorderTest proves the mechanism on a bare QDialog. This proves
// the wiring: the app's own New Project window, opened by the app's own
// button, with the recorder as MainWindow set it up. A dialog bug cannot be
// marked if the marker key dies the moment the dialog opens - which it did.
//
// QtGui's "does a shortcut claim this key?" entry point, as QTest uses it.
// Declared without defaults so a second section may declare it again.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject *o, ulong timestamp,
                                               int k, Qt::KeyboardModifiers mods,
                                               const QString &text, bool autorep,
                                               ushort count);
namespace recorderKeys {
// Shortcuts resolve against the ACTIVE window, and Windows may refuse focus
// to a test started from another program; what is under test is which
// widgets the shortcut is attached to, not the desktop's focus policy.
bool press(QWidget *target, int key)
{
    target->activateWindow();
    target->raise();
    pump(100);
    if (QApplication::activeWindow() != target) {
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(target);
        QT_WARNING_POP
        pump(50);
    }
    return qt_sendShortcutOverrideEvent(
        target, 0, key, Qt::ControlModifier | Qt::ShiftModifier, QString(),
        false, 1);
}

int markersIn(const QString &sessionDir)
{
    int markers = 0;
    QFile f(sessionDir + QStringLiteral("/events.jsonl"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        while (!f.atEnd())
            if (QJsonDocument::fromJson(f.readLine())
                    .object()
                    .value(QLatin1String("type"))
                    .toString()
                == QLatin1String("marker"))
                ++markers;
    return markers;
}
} // namespace recorderKeys

void runRecorderUnderModalPass()
{
    out() << "--- (x) the recorder's keys work inside the New Project "
             "dialog ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(700);
    auto *rec = devrec::Recorder::instance();

    QPushButton *newButton = nullptr;
    if (auto *dashboard = window.findChild<DashboardPage *>())
        for (QPushButton *b : dashboard->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral("New Project"))
                newButton = b;
    check(QStringLiteral("(x) found the New Project button"), newButton);
    if (!newButton)
        return;

    // Everything below happens INSIDE the dialog's own event loop.
    bool sawDialog = false, startedInside = false, markedInside = false;
    bool refusedWithout = true, stoppedInside = false;
    QString session;
    // Polls until the dialog is up, then runs ONCE: a single shot that fired
    // a moment early would leave the dialog open with nothing to close it.
    QTimer driver;
    driver.setInterval(80);
    QObject::connect(&driver, &QTimer::timeout, [&] {
        auto *dialog = qobject_cast<NewProjectDialog *>(
            QApplication::activeModalWidget());
        if (!dialog || sawDialog)
            return;
        sawDialog = true;
        startedInside = recorderKeys::press(dialog, Qt::Key_R)
            && rec->isRecording();
        pump(150);
        session = rec->sessionDir();
        markedInside = recorderKeys::press(dialog, Qt::Key_B);
        pump(150);
        // The control: detach the marker from the dialog and the same key,
        // sent the same way, must be claimed by nothing - the main window
        // is blocked, exactly as it was before the bridge existed.
        dialog->removeAction(rec->markAction());
        refusedWithout = !recorderKeys::press(dialog, Qt::Key_B);
        dialog->addAction(rec->markAction());
        pump(100);
        stoppedInside = recorderKeys::press(dialog, Qt::Key_R)
            && !rec->isRecording();
        dialog->reject();
    });
    driver.start();
    newButton->click(); // blocks until the driver rejects the dialog
    driver.stop();
    pump(300);
    if (rec->isRecording())
        rec->stopRecording(); // never leave a session open behind a failure

    check(QStringLiteral("(x) control: the New Project dialog opened"),
          sawDialog);
    check(QStringLiteral("(x) Ctrl+Shift+R STARTS a recording with the "
                         "dialog open"),
          startedInside);
    check(QStringLiteral("(x) Ctrl+Shift+B marks an issue with the dialog "
                         "open"),
          markedInside);
    check(QStringLiteral("(x) control: detached from the dialog, the same "
                         "key is claimed by nothing (the defect)"),
          refusedWithout);
    check(QStringLiteral("(x) Ctrl+Shift+R stops it again from inside"),
          stoppedInside);
    check(QStringLiteral("(x) the session holds exactly ONE marker - the "
                         "claimed press, not the refused one"),
          recorderKeys::markersIn(session) == 1,
          QStringLiteral("%1 marker(s) in %2")
              .arg(recorderKeys::markersIn(session)).arg(session));

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (p) the Grain Preview IS the engine's carve ---------------------------
// The Texture section's Grain Preview (Figma 359:47) claims the same "one
// definition" the Tip Shape panel does: its image is the engine's own
// coverage modulation — a neutral hardness-1 patch through the REAL stamp
// path with the grain fields kept — not a picture of the grain texture.
// This pass pins that byte-for-byte, proves grain (not the tip) is what
// the well measures via a depth-0 flatness control, drives every grain
// control through the real edit path, and proves the Load… workflow makes
// the imported image the active grain immediately.
void runGrainPreviewPass(const QString &scratch)
{
    out() << "--- (p) Grain Preview = engine carve ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/grainprev"),
                                         QStringLiteral("GrainPrev"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(p) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *studio = window.findChild<brushlib::BrushSettingsStudio *>();
    auto *model = window.findChild<brushlib::BrushLibraryModel *>();
    if (!studio || !model) {
        check(QStringLiteral("(p) found studio and model"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    const QString id = QStringLiteral("builtin/painting/gouache");
    studio->openForPreset(id);
    pump(400);

    // The test's OWN engine render: the widget's neutralization recipe
    // transcribed, so a drift in EITHER side breaks the identity.
    static constexpr int kPx = brushlib::StudioGrainPreview::kPatchPx;
    const auto enginePatch = [](const ::Brush &brush) {
        ::Brush copy = brush;
        copy.clearCustomShape();
        copy.setHardness(0.6); // SOFT, mirroring the widget: blend modes
                               // collapse to one value at full coverage
        copy.setSize(kPx);
        copy.setTipAngle(0.0);
        copy.setTipRoundness(1.0);
        copy.setTipFlipX(false);
        copy.setTipFlipY(false);
        copy.setSizeJitter(0.0);
        copy.setAngleJitter(0.0);
        copy.setRoundnessJitter(0.0);
        copy.setSpacingJitter(0.0);
        copy.setScatterAlong(0.0);
        copy.setScatterPerpendicular(0.0);
        copy.setScatterCount(1);
        copy.setNoise(0.0);
        copy.setWetEdges(0.0);
        copy.setOpacity(1.0);
        copy.setFlow(1.0);
        copy.setDualBrushEnabled(false);
        StrokeBuilder sb(QSize(kPx, kPx), copy,
                         /*rasterizePreview=*/true);
        sb.addRawPoint(QPointF(kPx * 0.5, kPx * 0.5));
        return sb.strokeMask();
    };
    const auto countBetween = [](const QImage &img, int lo, int hi) {
        int n = 0;
        for (int y = 0; y < img.height(); ++y) {
            const uchar *row = img.constScanLine(y);
            for (int x = 0; x < img.width(); ++x)
                if (row[x] >= lo && row[x] <= hi)
                    ++n;
        }
        return n;
    };

    const QImage shown = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) preview holds an engine-format render"),
          !shown.isNull() && shown.format() == QImage::Format_Grayscale8
              && shown.width() == kPx && shown.height() == kPx);
    check(QStringLiteral("(p) positive control: the patch is actually "
                         "there"),
          countBetween(shown, 1, 255) > 1000,
          QStringLiteral("covered=%1").arg(countBetween(shown, 1, 255)));
    check(QStringLiteral("(p) ONE DEFINITION: preview == the test's own "
                         "engine render, byte for byte"),
          !shown.isNull()
              && shown == enginePatch(model->preset(id)->brush));

    // Depth 0 through the REAL edit path: grain leaves, the falloff
    // stays, and the identity holds at the new state — so the difference
    // between the two images was grain and NOTHING else.
    studio->editSessionForTest([](::Brush &b) { b.setGrainDepth(0.0); },
                               false);
    pump(100);
    const QImage flat = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) Grain Depth reaches the preview"),
          !flat.isNull() && flat != shown);
    {
        ::Brush zeroBrush = model->preset(id)->brush;
        zeroBrush.setGrainDepth(0.0);
        check(QStringLiteral("(p) ...identity holds at depth 0 (the "
                             "difference WAS grain)"),
              flat == enginePatch(zeroBrush));
    }

    // Each grain control reaches the preview through the real edit path.
    studio->editSessionForTest(
        [](::Brush &b) {
            b.setGrainDepth(0.5);
            b.setGrainPreset(::Brush::GrainPreset::Paper);
        },
        false);
    pump(100);
    const QImage paper = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) preset switch reaches the preview"),
          !paper.isNull() && paper != flat);
    studio->editSessionForTest([](::Brush &b) { b.setGrainScale(23.0); },
                               false);
    pump(100);
    const QImage scaled = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) Grain Scale reaches the preview"),
          !scaled.isNull() && scaled != paper);
    studio->editSessionForTest(
        [](::Brush &b) { b.setGrainRotation(37.0); }, false);
    pump(100);
    const QImage rotated = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) Grain Rotation reaches the preview"),
          !rotated.isNull() && rotated != scaled);
    studio->editSessionForTest(
        [](::Brush &b) {
            b.setTextureBlendMode(::Brush::TextureBlendMode::Subtract);
        },
        false);
    pump(100);
    const QImage blended = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) Texture Blend mode reaches the preview"),
          !blended.isNull() && blended != rotated);

    // Load… workflow: an imported image is the ACTIVE grain immediately
    // (requirement 6) and the preview shows it.
    QImage customGrain(16, 16, QImage::Format_Grayscale8);
    for (int y = 0; y < 16; ++y) {
        uchar *row = customGrain.scanLine(y);
        for (int x = 0; x < 16; ++x)
            row[x] = uchar(x * 16 + y);
    }
    studio->editSessionForTest(
        [&customGrain](::Brush &b) { b.setCustomGrain(customGrain); },
        false);
    pump(100);
    const QImage custom = studio->grainPreviewImageForTest();
    check(QStringLiteral("(p) a loaded grain reaches the preview"),
          !custom.isNull() && custom != blended);
    {
        ::Brush expect = model->preset(id)->brush;
        expect.setGrainDepth(0.5);
        expect.setGrainScale(23.0);
        expect.setGrainRotation(37.0);
        expect.setTextureBlendMode(::Brush::TextureBlendMode::Subtract);
        expect.setCustomGrain(customGrain);
        // setCustomGrain flips the preset to Custom atomically (the
        // engine's own semantics), so the identity holding here proves
        // BOTH requirement 6 halves: the imported image is the active
        // grain, immediately, and the preview depicts exactly that.
        check(QStringLiteral("(p) ...ACTIVE immediately: identity holds "
                             "with the custom grain (preset == Custom)"),
              expect.grainPreset() == ::Brush::GrainPreset::Custom
                  && custom == enginePatch(expect));
    }

    // Brush size is not a patch input: the well is a fixed canvas window.
    studio->editSessionForTest([](::Brush &b) { b.setSize(5000); }, true);
    pump(100);
    check(QStringLiteral("(p) size 5000 changes NOTHING in the preview "
                         "(fixed canvas window)"),
          studio->grainPreviewImageForTest() == custom);

    check(QStringLiteral("(p) the session never touched the model"),
          model->overrideState(id)
              == brushlib::BrushLibraryModel::OverrideState::None);

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (y) the start window shows the recent projects -----------------------
// The three boxes on the start window were hardcoded placeholders, and the
// real list lived in the New Project dialog, checked once by a seam and
// never by the gate. Both halves are pinned here: what the page shows for
// 0, 1, 2, 3 and a full list, and that EVERYTHING the dialog's list did
// arrived with it - the first-panel thumbnail, the date, the middle-elided
// name, the dimmed missing project with its remove prompt, and a thumbnail
// cache that decodes once and never on a paint.
//
// Every "what is shown" below is read through the view's *At accessors,
// which return what paintEvent draws, computed by the same code.
namespace startWindow {

// A project saved through the REAL save path: one panel, one solid colour,
// so "whose picture is this" has a one-pixel answer.
QString saveSolid(const QString &folder, const QString &name,
                  const QColor &color, const QSize &size = QSize(960, 540))
{
    QDir().mkpath(folder);
    const QString path =
        folder + QLatin1Char('/') + name + QStringLiteral(".sankotv");
    Scene *scene = new Scene;
    scene->number = 1;
    Panel *panel = makeBlankPanel(size);
    panel->layers[1].image.fill(color);
    scene->panels.append(panel);
    ProjectIO::SaveData data;
    data.projectName = name;
    data.fps = 24;
    data.canvasSize = size;
    data.scenes = {scene};
    const ProjectIO::WriteResult w = ProjectIO::projectToJson(data, path);
    QFile f(path);
    if (w.ok && f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(w.root).toJson(QJsonDocument::Indented));
    f.close();
    delete scene;
    return w.ok ? path : QString();
}

struct Recent
{
    QString path;
    QString stamp; // ISO, as the store keeps it
};

// The store written RAW, in the order given (most recent first), with the
// dates given - so order and dates are the test's, not "now".
void setRecents(const QString &ini, const QVector<Recent> &list)
{
    QFile::remove(ini);
    QSettings s(ini, QSettings::IniFormat);
    s.beginWriteArray(QStringLiteral("recentProjects"), int(list.size()));
    for (int i = 0; i < list.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(QStringLiteral("path"), list.at(i).path);
        s.setValue(QStringLiteral("lastOpened"), list.at(i).stamp);
    }
    s.endArray();
    s.sync();
}

// Let every queued thumbnail decode (one per turn of the event loop).
void settle(RecentThumbnails *thumbnails)
{
    for (int guard = 0; thumbnails->busy() && guard < 600; ++guard)
        pump(10);
    pump(40);
}

QPixmap thumbOf(DashboardPage *page, int index)
{
    RecentProjectsView *view = page->recentsView();
    return page->thumbnails()->pixmap(
        view->pathAt(index), view->thumbPixelSize(view->kindAt(index)));
}

QColor thumbCentre(DashboardPage *page, int index)
{
    const QImage img = thumbOf(page, index).toImage();
    return img.isNull() ? QColor()
                        : img.pixelColor(img.width() / 2, img.height() / 2);
}

bool closeTo(const QColor &a, const QColor &b, int tolerance = 3)
{
    return a.isValid() && b.isValid()
        && qAbs(a.red() - b.red()) <= tolerance
        && qAbs(a.green() - b.green()) <= tolerance
        && qAbs(a.blue() - b.blue()) <= tolerance;
}

void click(QWidget *w, const QPoint &pressAt, const QPoint &releaseAt)
{
    sendMouse(w, QEvent::MouseButtonPress, pressAt, Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, releaseAt, Qt::LeftButton);
}

void key(QWidget *w, int k)
{
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &press);
}

} // namespace startWindow

void runStartWindowRecentsPass(const QString &scratch)
{
    using namespace startWindow;
    out() << "--- (y) the start window shows the recent projects ---"
          << Qt::endl;
    const QString root = scratch + QStringLiteral("/start_window");
    QDir(root).removeRecursively();
    QDir().mkpath(root);
    const QString ini = root + QStringLiteral("/recents.ini");
    RecentProjects::setSettingsOverride(ini);

    // ---- fixtures ---------------------------------------------------------
    const QColor cBig(200, 40, 40), cV1(40, 170, 60), cV2(60, 80, 210);
    const QColor cBoard(210, 180, 40), cBoardA(160, 60, 200);
    const QColor cStale(20, 20, 20), cRepaint(30, 200, 200);
    QVector<QColor> plainColours;
    QVector<QString> plain; // P1..P8, 960x540
    for (int i = 1; i <= 8; ++i) {
        plainColours.append(QColor::fromHsv((i * 41) % 360, 190, 215));
        plain.append(saveSolid(root + QStringLiteral("/P%1").arg(i),
                               QStringLiteral("P%1").arg(i),
                               plainColours.last()));
    }
    const QString big = saveSolid(root + QStringLiteral("/Big"),
                                  QStringLiteral("Big"), cBig,
                                  QSize(1920, 1080));
    const QString longStem = QStringLiteral(
        "Cyberpunk_Alley_Night_Chase_Sequence_Final_Storyboard_Director_Cut_");
    const QString v1 = saveSolid(root + QStringLiteral("/V1"),
                                 longStem + QStringLiteral("v1"), cV1);
    const QString v2 = saveSolid(root + QStringLiteral("/V2"),
                                 longStem + QStringLiteral("v2"), cV2);
    // Two projects in ONE folder beside a stale flat file from an older
    // save: the arrangement in which the old list showed the wrong picture.
    const QString sharedDir = root + QStringLiteral("/Shared");
    const QString board = saveSolid(sharedDir, QStringLiteral("Board"), cBoard);
    const QString boardA =
        saveSolid(sharedDir, QStringLiteral("Board_A"), cBoardA);
    {
        QImage stale(960, 540, QImage::Format_ARGB32_Premultiplied);
        stale.fill(cStale);
        stale.save(sharedDir + QStringLiteral("/panel_s0_p0.png"), "PNG");
    }
    // Created but never drawn in: a manifest with no scenes.
    const QString empty = root + QStringLiteral("/Empty/Empty.sankotv");
    QDir().mkpath(QFileInfo(empty).absolutePath());
    {
        QJsonObject manifest{{QStringLiteral("version"), 1},
                             {QStringLiteral("scenes"), QJsonArray()}};
        QFile f(empty);
        if (f.open(QIODevice::WriteOnly))
            f.write(QJsonDocument(manifest).toJson());
    }
    const QString gone = root + QStringLiteral("/Gone/Gone.sankotv");
    bool fixturesOk = !big.isEmpty() && !v1.isEmpty() && !v2.isEmpty()
        && !board.isEmpty() && !boardA.isEmpty() && QFileInfo::exists(empty)
        && !QFileInfo::exists(gone);
    for (const QString &p : plain)
        fixturesOk = fixturesOk && !p.isEmpty();
    check(QStringLiteral("(y) fixtures: thirteen projects through the real "
                         "save path, one never drawn in, one whose file is "
                         "gone"),
          fixturesOk);
    const QString older = QStringLiteral("2026-08-28T18:16:17");

    // ---- 0, 1, 2, 3 recents -------------------------------------------------
    setRecents(ini, {});
    MainWindow window;
    window.resize(1400, 880);
    window.show();
    pump(800);
    auto *page = window.findChild<DashboardPage *>();
    RecentProjectsView *view = page ? page->recentsView() : nullptr;
    check(QStringLiteral("(y) found the start window and its recents view"),
          page && view && window.onDashboardForTest());
    if (!page || !view)
        return;
    RecentThumbnails *thumbnails = page->thumbnails();

    check(QStringLiteral("(y) FIRST RUN, no recents: no cards at all, and "
                         "the hint says how to begin"),
          view->count() == 0 && !view->isVisible()
              && page->emptyHint()->isVisible()
              && page->emptyHint()->text()
                     == QStringLiteral("Create a new project to get started"));

    setRecents(ini, {{plain.at(5), older}});
    page->reloadRecents();
    pump(60);
    check(QStringLiteral("(y) ONE recent: one card, in the first slot; the "
                         "hint is gone (control: the view CAN be visible)"),
          view->count() == 1 && view->isVisible()
              && !page->emptyHint()->isVisible()
              && view->kindAt(0) == RecentProjectsView::Kind::Card
              && view->rectAt(0).topLeft() == QPoint(0, 0));

    setRecents(ini, {{plain.at(5), older}, {plain.at(6), older}});
    page->reloadRecents();
    pump(60);
    check(QStringLiteral("(y) TWO recents: two cards filling from the left, "
                         "the third slot empty"),
          view->count() == 2
              && view->rectAt(1).left()
                     == RecentProjectsView::kCardW
                            + RecentProjectsView::kCardGap
              && view->rectAt(2).isNull()
              && view->width() == RecentProjectsView::kViewW);

    setRecents(ini, {{plain.at(5), older},
                     {plain.at(6), older},
                     {plain.at(7), older}});
    page->reloadRecents();
    pump(60);
    check(QStringLiteral("(y) THREE recents: three cards and nothing beneath"),
          view->count() == 3 && view->height() == RecentProjectsView::kCardH
              && view->kindAt(2) == RecentProjectsView::Kind::Card);
    settle(thumbnails);

    // ---- a full list --------------------------------------------------------
    // Twelve written; the store's cap is ten. Laid out so every behaviour
    // has an entry: 0 Big (1920x1080), 1 long-name v1, 2 Board_A (shared
    // folder), then rows: 3 Board, 4 long-name v2, 5 Gone, 6 Empty, 7-9
    // plain. None of these was listed above, so nothing is decoded yet.
    const QVector<Recent> full = {
        {big, QStringLiteral("2026-09-30T19:10:29")},
        {v1, QStringLiteral("2026-09-28T19:29:51")},
        {boardA, older},
        {board, older},
        {v2, older},
        {gone, older},
        {empty, older},
        {plain.at(0), older},
        {plain.at(1), older},
        {plain.at(2), older},
        {plain.at(3), older},  // beyond the cap
        {plain.at(4), older}}; // beyond the cap
    setRecents(ini, full);
    const int decodesBeforeLoad = thumbnails->decodeCountForTest();
    page->reloadRecents(); // deliberately NOT pumped yet
    view->repaint();       // a paint, with nothing decoded

    int withPicture = 0;
    for (int i = 0; i < view->count(); ++i)
        withPicture += view->hasThumbnailAt(i) ? 1 : 0;
    check(QStringLiteral("(y) loading the list and painting it decodes "
                         "NOTHING: thumbnails are deferred, not done on the "
                         "paint"),
          thumbnails->decodeCountForTest() == decodesBeforeLoad
              && withPicture == 0 && thumbnails->busy(),
          QStringLiteral("%1 decoded, %2 shown")
              .arg(thumbnails->decodeCountForTest() - decodesBeforeLoad)
              .arg(withPicture));
    // The film glyph MEANS "no picture". A project whose picture is merely
    // on its way must not wear it for a frame and then lose it.
    int glyphsWhileWaiting = 0;
    for (int i = 0; i < view->count(); ++i)
        glyphsWhileWaiting += view->showsGlyphAt(i) ? 1 : 0;
    check(QStringLiteral("(y) while pictures are still queued, only the "
                         "MISSING project wears the no-picture glyph; the "
                         "wells that are waiting stay empty"),
          glyphsWhileWaiting == 1 && view->showsGlyphAt(5),
          QStringLiteral("%1 glyph(s)").arg(glyphsWhileWaiting));

    using Kind = RecentProjectsView::Kind;
    bool kindsOk = view->count() == RecentProjects::kCap;
    for (int i = 0; kindsOk && i < view->count(); ++i)
        kindsOk = view->kindAt(i) == (i < 3 ? Kind::Card : Kind::Row);
    check(QStringLiteral("(y) a FULL list: ten shown (the store's cap), the "
                         "first three as cards and the other seven as rows"),
          kindsOk, QStringLiteral("%1 shown").arg(view->count()));
    bool orderOk = true;
    for (int i = 0; i < view->count(); ++i)
        orderOk = orderOk && view->pathAt(i) == full.at(i).path;
    check(QStringLiteral("(y) ...in the store's order, most recent first"),
          orderOk);

    const int pitchX =
        RecentProjectsView::kCardW + RecentProjectsView::kCardGap;
    const int rowsTop =
        RecentProjectsView::kCardH + RecentProjectsView::kSectionGap;
    check(QStringLiteral("(y) the rows sit under the cards in the cards' "
                         "three columns, reading across then down"),
          view->rectAt(3).topLeft() == QPoint(0, rowsTop)
              && view->rectAt(4).topLeft() == QPoint(pitchX, rowsTop)
              && view->rectAt(5).topLeft() == QPoint(2 * pitchX, rowsTop)
              && view->rectAt(6).topLeft()
                     == QPoint(0, rowsTop + RecentProjectsView::kRowPitch)
              && view->rectAt(9).topLeft()
                     == QPoint(0, rowsTop + 2 * RecentProjectsView::kRowPitch));
    bool inside = true, apart = true;
    for (int i = 0; i < view->count(); ++i) {
        inside = inside && view->rect().contains(view->rectAt(i));
        for (int j = i + 1; j < view->count(); ++j)
            apart = apart && !view->rectAt(i).intersects(view->rectAt(j));
    }
    check(QStringLiteral("(y) every item is inside the view and no two "
                         "overlap"),
          inside && apart);

    // Names and dates.
    const QChar ellipsis(0x2026);
    const QString shownV1 = view->shownNameAt(1), shownV2 = view->shownNameAt(4);
    check(QStringLiteral("(y) a long name is elided in the MIDDLE on a card "
                         "and on a row, so v1 and v2 stay distinguishable"),
          shownV1.contains(ellipsis) && shownV2.contains(ellipsis)
              && shownV1.endsWith(QStringLiteral("v1"))
              && shownV2.endsWith(QStringLiteral("v2"))
              && shownV1.startsWith(QStringLiteral("Cyber"))
              && shownV2.startsWith(QStringLiteral("Cyber")),
          shownV1 + QStringLiteral(" | ") + shownV2);
    check(QStringLiteral("(y) control: a short name is shown whole"),
          view->shownNameAt(0) == QStringLiteral("Big")
              && view->shownNameAt(7) == QStringLiteral("P1"));
    check(QStringLiteral("(y) each shows when it was last opened"),
          view->shownDateAt(0) == QStringLiteral("Last opened: Sep 30, 2026")
              && view->shownDateAt(1)
                     == QStringLiteral("Last opened: Sep 28, 2026")
              && view->shownDateAt(9)
                     == QStringLiteral("Last opened: Aug 28, 2026"),
          view->shownDateAt(0));

    // The missing project.
    int missing = 0;
    for (int i = 0; i < view->count(); ++i)
        missing += view->missingAt(i) ? 1 : 0;
    check(QStringLiteral("(y) the project whose file is gone is still "
                         "listed, flagged to be drawn dimmed - and it is the "
                         "only one"),
          view->missingAt(5) && missing == 1 && view->pathAt(5) == gone);

    // ---- thumbnails ---------------------------------------------------------
    settle(thumbnails);
    const int decoded = thumbnails->decodeCountForTest() - decodesBeforeLoad;
    bool pictured = true;
    for (int i = 0; i < view->count(); ++i)
        pictured = pictured && view->hasThumbnailAt(i) == (i != 5 && i != 6);
    check(QStringLiteral("(y) once the loop has turned, every project with "
                         "artwork has its picture - decoded exactly once "
                         "each - and the missing and the never-drawn do not"),
          decoded == 8 && pictured && !thumbnails->busy(),
          QStringLiteral("%1 decodes").arg(decoded));
    int glyphsAfter = 0;
    for (int i = 0; i < view->count(); ++i)
        glyphsAfter += view->showsGlyphAt(i) ? 1 : 0;
    check(QStringLiteral("(y) ...and the glyph now marks exactly the two "
                         "with no picture: the missing project and the one "
                         "never drawn in (control: the count moved from 1)"),
          glyphsAfter == 2 && view->showsGlyphAt(5) && view->showsGlyphAt(6),
          QStringLiteral("%1 glyph(s)").arg(glyphsAfter));

    const QSize cardPx = view->thumbPixelSize(Kind::Card);
    const QSize rowPx = view->thumbPixelSize(Kind::Row);
    const QSize bigOnDisk =
        QImage(RecentProjects::thumbnailSource(big)).size();
    const QSize bigThumb = thumbOf(page, 0).size();
    const QSize rowThumb = thumbOf(page, 7).size();
    check(QStringLiteral("(y) a card's picture is decoded AT the size it is "
                         "drawn, not at the panel's (control: the panel on "
                         "disk is 1920x1080)"),
          bigOnDisk == QSize(1920, 1080) && bigThumb == cardPx,
          QStringLiteral("thumb %1x%2, card well %3x%4")
              .arg(bigThumb.width()).arg(bigThumb.height())
              .arg(cardPx.width()).arg(cardPx.height()));
    check(QStringLiteral("(y) a row's picture covers its small well and is "
                         "no bigger than that needs"),
          rowThumb.height() == rowPx.height()
              && rowThumb.width() >= rowPx.width()
              && rowThumb.width() <= rowPx.width() * 2,
          QStringLiteral("thumb %1x%2, row well %3x%4")
              .arg(rowThumb.width()).arg(rowThumb.height())
              .arg(rowPx.width()).arg(rowPx.height()));

    check(QStringLiteral("(y) each picture is that project's OWN first "
                         "panel"),
          closeTo(thumbCentre(page, 0), cBig) && closeTo(thumbCentre(page, 1), cV1)
              && closeTo(thumbCentre(page, 4), cV2)
              && closeTo(thumbCentre(page, 7), plainColours.at(0))
              && closeTo(thumbCentre(page, 9), plainColours.at(2)));
    check(QStringLiteral("(y) two projects sharing a folder show two "
                         "different pictures, and neither is the stale flat "
                         "file beside them (control: it is there)"),
          closeTo(thumbCentre(page, 2), cBoardA) && closeTo(thumbCentre(page, 3), cBoard)
              && !closeTo(thumbCentre(page, 2), cStale)
              && !closeTo(thumbCentre(page, 3), cStale)
              && QFileInfo::exists(sharedDir
                                   + QStringLiteral("/panel_s0_p0.png")));

    // A repaint storm - what hovering across the list is.
    const int beforeStorm = thumbnails->decodeCountForTest();
    for (int i = 0; i < 60; ++i) {
        sendMouse(view, QEvent::MouseMove,
                  view->rectAt(i % view->count()).center(), Qt::NoButton);
        view->repaint();
    }
    check(QStringLiteral("(y) sixty hover repaints decode ZERO more times"),
          thumbnails->decodeCountForTest() == beforeStorm);
    page->reloadRecents();
    settle(thumbnails);
    page->reloadRecents();
    settle(thumbnails);
    check(QStringLiteral("(y) re-reading an unchanged list decodes nothing "
                         "either"),
          thumbnails->decodeCountForTest() == beforeStorm);

    // ...and the control that a decode DOES happen when it should: save one
    // project again with a different first panel.
    saveSolid(root + QStringLiteral("/P1"), QStringLiteral("P1"), cRepaint);
    {
        QFile png(RecentProjects::thumbnailSource(plain.at(0)));
        if (png.open(QIODevice::ReadWrite)) {
            png.setFileTime(QDateTime::currentDateTime().addSecs(5),
                            QFileDevice::FileModificationTime);
            png.close();
        }
    }
    page->reloadRecents();
    const bool staleShownMeanwhile =
        closeTo(thumbCentre(page, 7), plainColours.at(0));
    settle(thumbnails);
    check(QStringLiteral("(y) a project saved again is decoded again, ONCE, "
                         "and shows its new first panel - the old one staying "
                         "up until the new one is ready"),
          thumbnails->decodeCountForTest() == beforeStorm + 1
              && closeTo(thumbCentre(page, 7), cRepaint) && staleShownMeanwhile,
          QStringLiteral("%1 extra decode(s)")
              .arg(thumbnails->decodeCountForTest() - beforeStorm));

    // ---- the missing project's remove prompt ------------------------------
    int asked = 0, opened = 0;
    QString askedAbout;
    bool answer = false;
    view->setRemovePrompt([&](const QString &path) {
        ++asked;
        askedAbout = path;
        return answer;
    });
    QObject::connect(view, &RecentProjectsView::openRequested, &window,
                     [&opened] { ++opened; });
    view->activate(5);
    pump(60);
    check(QStringLiteral("(y) clicking the missing project ASKS whether to "
                         "remove it, and No keeps it listed"),
          asked == 1 && askedAbout == gone && opened == 0
              && view->count() == 10 && view->missingAt(5));
    answer = true;
    view->activate(5);
    pump(60);
    bool stillStored = false;
    for (const RecentProjects::Entry &e : RecentProjects::entries())
        stillStored = stillStored || e.path == gone;
    check(QStringLiteral("(y) Yes removes it: from the page AND from the "
                         "store, and nothing was opened"),
          asked == 2 && opened == 0 && view->count() == 9 && !stillStored
              && view->pathAt(5) == empty);

    // ---- a click opens ------------------------------------------------------
    // (the list is now 0 Big, 1 v1, 2 Board_A | 3 Board, 4 v2, 5 Empty,
    //  6 P1, 7 P2, 8 P3)
    click(view, view->rectAt(0).center(), view->rectAt(1).center());
    pump(200);
    check(QStringLiteral("(y) control: pressing one card and letting go on "
                         "another opens NOTHING"),
          opened == 0 && window.onDashboardForTest()
              && window.projectPathForTest().isEmpty());
    click(view, view->rectAt(2).center(), view->rectAt(2).center());
    pump(500);
    check(QStringLiteral("(y) a single click on a CARD opens that project, "
                         "through the real open path, without asking "
                         "anything"),
          opened == 1 && asked == 2 && !window.onDashboardForTest()
              && window.projectPathForTest() == boardA,
          window.projectPathForTest());
    window.markCleanForTest();
    window.closeProjectForTest();
    pump(400);
    check(QStringLiteral("(y) back on the start window the list has been "
                         "re-read: the project just opened is now first"),
          window.onDashboardForTest() && view->pathAt(0) == boardA
              && view->count() == 9);
    int p2 = -1;
    for (int i = 0; i < view->count(); ++i)
        if (view->pathAt(i) == plain.at(1))
            p2 = i;
    check(QStringLiteral("(y) setup: P2 is one of the rows"),
          p2 >= 3 && view->kindAt(p2) == Kind::Row);
    click(view, view->rectAt(p2).center(), view->rectAt(p2).center());
    pump(500);
    check(QStringLiteral("(y) a single click on a ROW opens that project "
                         "too"),
          !window.onDashboardForTest()
              && window.projectPathForTest() == plain.at(1));

    // The card route is THE open path: with unsaved work it must ask before
    // replacing the project. The prompt is a modal; something has to see it
    // and dismiss it (dismissing is Cancel).
    window.applyProjectSettingsForTest(QStringLiteral("EditedOnTheWay"), 30);
    pump(150);
    QString promptSeen;
    QTimer watcher;
    watcher.setInterval(60);
    QObject::connect(&watcher, &QTimer::timeout, [&promptSeen] {
        if (auto *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget())) {
            promptSeen = box->windowTitle();
            box->close();
        }
    });
    watcher.start();
    view->activate(0);
    watcher.stop();
    pump(150);
    check(QStringLiteral("(y) opening a recent over UNSAVED work asks first, "
                         "and Cancel leaves the open project alone"),
          promptSeen == QStringLiteral("Unsaved Changes")
              && window.projectPathForTest() == plain.at(1) && window.isDirty(),
          promptSeen);
    window.markCleanForTest();
    window.closeProjectForTest();
    pump(400);

    // ---- the keyboard -------------------------------------------------------
    // Home, then one step right, then down a line: the second card, then
    // the row beneath it. Enter opens what the cursor is on.
    key(view, Qt::Key_Home);
    key(view, Qt::Key_Right);
    key(view, Qt::Key_Down);
    const QString underCursor = view->pathAt(4);
    key(view, Qt::Key_Return);
    pump(500);
    check(QStringLiteral("(y) Home and the arrow keys move a cursor through "
                         "the list and Enter opens the project under it"),
          !underCursor.isEmpty()
              && window.projectPathForTest() == underCursor,
          window.projectPathForTest());
    window.markCleanForTest();
    window.closeProjectForTest();
    pump(300);

    // ---- Open Project -------------------------------------------------------
    // The picker is native and cannot be driven, so: the button exists and
    // raises its signal (on a page of its own, where nothing answers by
    // opening a file dialog), and the folder the picker would start in.
    {
        DashboardPage standalone;
        int openClicks = 0, newClicks = 0;
        QObject::connect(&standalone, &DashboardPage::openProjectRequested,
                         [&openClicks] { ++openClicks; });
        QObject::connect(&standalone, &DashboardPage::newProjectRequested,
                         [&newClicks] { ++newClicks; });
        QStringList buttons;
        for (QPushButton *b : standalone.findChildren<QPushButton *>()) {
            buttons << b->text();
            b->click();
        }
        check(QStringLiteral("(y) the start window offers exactly two "
                             "buttons, Open Project and New Project, each "
                             "raising its own request"),
              buttons == QStringList{QStringLiteral("Open Project"),
                                     QStringLiteral("New Project")}
                  && openClicks == 1 && newClicks == 1,
              buttons.join(QStringLiteral(", ")));
        bool logo = false;
        for (QLabel *label : standalone.findChildren<QLabel *>())
            logo = logo || !label->pixmap().isNull()
                || label->text().contains(QStringLiteral("SANKO"),
                                          Qt::CaseInsensitive);
        check(QStringLiteral("(y) and no logo: no label on the page carries "
                             "an image or the product name"),
              !logo);
    }
    setRecents(ini, {{gone, older}, {board, older}, {big, older}});
    check(QStringLiteral("(y) Open Project starts beside the most recent "
                         "project that still EXISTS (the missing first entry "
                         "is skipped)"),
          QDir(window.openDialogStartDirForTest()) == QDir(sharedDir),
          window.openDialogStartDirForTest());
    setRecents(ini, {});
    const QString documentsSanko =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        + QStringLiteral("/SankoTV");
    const bool existedBefore = QDir(documentsSanko).exists();
    const QString fallback = window.openDialogStartDirForTest();
    check(QStringLiteral("(y) with no recents it starts in a folder that "
                         "exists, and asking CREATED nothing"),
          QDir(fallback).exists()
              && QDir(documentsSanko).exists() == existedBefore,
          fallback);

    view->setRemovePrompt({});
    window.markCleanForTest();
    window.close();
    pump(300);
    RecentProjects::setSettingsOverride(scratch
                                        + QStringLiteral("/recents.ini"));
}

// ---- (z) the start window's chrome: no menu bar, live keys -----------------
// The menu bar is hidden on the start window and back on every other page.
// A hidden menu bar takes its shortcuts with it (measured before this was
// built: Ctrl+N and the recorder's Ctrl+Shift+B both fired 0 times), so
// every shortcut-bearing action is attached to the window as well - and
// this is where that stays true. Keys are delivered the way a real key
// press is resolved (QtGui's shortcut entry point, as QTest uses it).
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject *o, ulong timestamp,
                                               int k, Qt::KeyboardModifiers mods,
                                               const QString &text, bool autorep,
                                               ushort count);
namespace startWindowKeys {
bool press(QWidget *target, int key, Qt::KeyboardModifiers mods)
{
    target->activateWindow();
    target->raise();
    pump(100);
    if (QApplication::activeWindow() != target) {
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(target);
        QT_WARNING_POP
        pump(50);
    }
    return qt_sendShortcutOverrideEvent(target, 0, key, mods, QString(),
                                        false, 1);
}

int markersIn(const QString &sessionDir)
{
    int markers = 0;
    QFile f(sessionDir + QStringLiteral("/events.jsonl"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        while (!f.atEnd())
            if (QJsonDocument::fromJson(f.readLine())
                    .object()
                    .value(QLatin1String("type"))
                    .toString()
                == QLatin1String("marker"))
                ++markers;
    return markers;
}
} // namespace startWindowKeys

void runStartWindowChromePass(const QString &scratch, const QString &project)
{
    using startWindowKeys::press;
    const Qt::KeyboardModifiers ctrl = Qt::ControlModifier;
    const Qt::KeyboardModifiers ctrlShift =
        Qt::ControlModifier | Qt::ShiftModifier;
    out() << "--- (z) the start window: no menu bar, live keys ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    auto *page = window.findChild<DashboardPage *>();
    auto *rec = devrec::Recorder::instance();
    QAction *newAction = nullptr, *openAction = nullptr;
    for (QAction *a : window.findChildren<QAction *>()) {
        if (a->text() == QStringLiteral("New Project..."))
            newAction = a;
        if (a->text() == QStringLiteral("Open Project..."))
            openAction = a;
    }
    check(QStringLiteral("(z) found the page and the File menu's New and "
                         "Open actions"),
          page && newAction && openAction);
    if (!page || !newAction || !openAction)
        return;

    // ---- the menu bar -------------------------------------------------------
    const bool hiddenAtStart = !window.menuBar()->isVisible()
        && page->mapTo(&window, QPoint(0, 0)).y() == 0;
    check(QStringLiteral("(z) on the start window there is NO menu bar: the "
                         "page begins at the very top of the window"),
          hiddenAtStart,
          QStringLiteral("page top at y=%1")
              .arg(page->mapTo(&window, QPoint(0, 0)).y()));

    // ---- keys with the bar hidden -------------------------------------------
    // New and Open end in a modal and a native picker, so their signals are
    // blocked: the key is still RESOLVED - claimed or not - and that is
    // the thing in question.
    newAction->blockSignals(true);
    openAction->blockSignals(true);
    check(QStringLiteral("(z) Ctrl+N is live with the menu bar hidden"),
          press(&window, Qt::Key_N, ctrl));
    check(QStringLiteral("(z) Ctrl+O is live with the menu bar hidden"),
          press(&window, Qt::Key_O, ctrl));
    // THE CONTROL: detach the action from the window and the same key is
    // dead - which is the hidden-menu defect itself, and proves that what
    // keeps the key alive is the attachment, not something incidental.
    window.removeAction(newAction);
    check(QStringLiteral("(z) control: detached from the window, Ctrl+N is "
                         "claimed by NOTHING while the bar is hidden"),
          !press(&window, Qt::Key_N, ctrl));
    window.addAction(newAction);
    check(QStringLiteral("(z) ...and re-attached, it is live again"),
          press(&window, Qt::Key_N, ctrl));

    // The recorder's two keys, for real (its output is under scratch).
    const bool started = press(&window, Qt::Key_R, ctrlShift)
        && rec->isRecording();
    pump(150);
    const QString sessionA = rec->sessionDir();
    const bool marked = press(&window, Qt::Key_B, ctrlShift);
    pump(150);
    const bool stopped = press(&window, Qt::Key_R, ctrlShift)
        && !rec->isRecording();
    pump(200);
    check(QStringLiteral("(z) Ctrl+Shift+R starts and stops a recording "
                         "from the start window"),
          started && stopped);
    check(QStringLiteral("(z) Ctrl+Shift+B marks an issue from the start "
                         "window: one press, one marker"),
          marked && startWindowKeys::markersIn(sessionA) == 1,
          QStringLiteral("%1 marker(s)")
              .arg(startWindowKeys::markersIn(sessionA)));

    // ---- the recorder's indicator -------------------------------------------
    QWidget *indicator = rec->indicatorWidget();
    check(QStringLiteral("(z) the recorder's indicator is in the start "
                         "window's header (it would have vanished with the "
                         "menu bar), visible and clickable"),
          page->headerAccessory() == indicator && indicator->isVisible()
              && indicator->window() == &window && rec->indicatorInteractive()
              && window.menuBar()->cornerWidget(Qt::TopRightCorner)
                     != indicator);
    sendMouse(indicator, QEvent::MouseButtonPress, indicator->rect().center(),
              Qt::LeftButton);
    pump(200);
    const bool clickStarted = rec->isRecording();
    sendMouse(indicator, QEvent::MouseButtonPress, indicator->rect().center(),
              Qt::LeftButton);
    pump(250);
    check(QStringLiteral("(z) a click on it starts a recording and a second "
                         "click stops it"),
          clickStarted && !rec->isRecording());

    // ---- every other page gets the bar back ---------------------------------
    check(QStringLiteral("(z) project opens"), window.loadProjectForTest(project));
    pump(500);
    check(QStringLiteral("(z) with a project open the menu bar is BACK "
                         "(control: the same reading that said hidden)"),
          window.menuBar()->isVisible() && window.menuBar()->height() > 0);
    indicator = rec->indicatorWidget();
    check(QStringLiteral("(z) the indicator is back in the menu bar's "
                         "corner, visible, and passive again"),
          window.menuBar()->cornerWidget(Qt::TopRightCorner) == indicator
              && indicator->isVisible() && !rec->indicatorInteractive()
              && page->headerAccessory() == nullptr);
    sendMouse(indicator, QEvent::MouseButtonPress, indicator->rect().center(),
              Qt::LeftButton);
    pump(150);
    check(QStringLiteral("(z) control: in the menu bar a click on it does "
                         "nothing"),
          !rec->isRecording());

    // Attached to the menu AND the window, a key must still fire ONCE.
    check(QStringLiteral("(z) Ctrl+N is still live with the bar shown"),
          press(&window, Qt::Key_N, ctrl));
    rec->startRecording();
    pump(150);
    const QString sessionB = rec->sessionDir();
    const bool markedShown = press(&window, Qt::Key_B, ctrlShift);
    pump(150);
    rec->stopRecording();
    pump(200);
    check(QStringLiteral("(z) with the bar shown one Ctrl+Shift+B is ONE "
                         "marker, not two (two attachments, one shortcut)"),
          markedShown && startWindowKeys::markersIn(sessionB) == 1,
          QStringLiteral("%1 marker(s)")
              .arg(startWindowKeys::markersIn(sessionB)));
    newAction->blockSignals(false);
    openAction->blockSignals(false);

    window.markCleanForTest();
    window.closeProjectForTest();
    pump(400);
    check(QStringLiteral("(z) closing the project returns to the start "
                         "window and hides the bar again, indicator and all"),
          window.onDashboardForTest() && !window.menuBar()->isVisible()
              && page->headerAccessory() == rec->indicatorWidget()
              && rec->indicatorInteractive());

    // ---- the New Project dialog ---------------------------------------------
    out() << "--- (z) the New Project dialog only creates ---" << Qt::endl;
    {
        NewProjectDialog dialog(&window);
        check(QStringLiteral("(z) the dialog is one column, 340 x 385"),
              dialog.size() == QSize(340, 385)
                  && dialog.size() == QSize(NewProjectDialog::kWidth,
                                            NewProjectDialog::kHeight));
        QStringList buttons;
        for (QPushButton *b : dialog.findChildren<QPushButton *>())
            buttons << b->text();
        buttons.sort();
        check(QStringLiteral("(z) its buttons are Browse, Cancel and Create "
                             "- there is no Open Project and no list of "
                             "recents in it"),
              buttons == QStringList{QStringLiteral("Browse..."),
                                     QStringLiteral("Cancel"),
                                     QStringLiteral("Create Project")}
                  && dialog.findChildren<QScrollArea *>().isEmpty(),
              buttons.join(QStringLiteral(", ")));
        // Everything sits inside the 18 px padding, nothing overlaps.
        const QRect content(18, 0, 304, 385);
        bool insideContent = true, noOverlap = true;
        const QList<QWidget *> kids =
            dialog.findChildren<QWidget *>(Qt::FindDirectChildrenOnly);
        for (int i = 0; i < kids.size(); ++i) {
            insideContent =
                insideContent && content.contains(kids.at(i)->geometry());
            for (int j = i + 1; j < kids.size(); ++j)
                noOverlap = noOverlap
                    && !kids.at(i)->geometry().intersects(
                           kids.at(j)->geometry());
        }
        check(QStringLiteral("(z) control: the layout walk sees the form's "
                             "controls"),
              kids.size() >= 9, QStringLiteral("%1 controls").arg(kids.size()));
        check(QStringLiteral("(z) every control is inside the padded column "
                             "and none overlaps another"),
              insideContent && noOverlap);
        check(QStringLiteral("(z) the footer is Cancel then Create, edge to "
                             "edge of the column"),
              dialog.cancelButton()->geometry().left() == 18
                  && dialog.createButton()->geometry().right() == 321
                  && dialog.cancelButton()->geometry().right()
                         < dialog.createButton()->geometry().left());
        check(QStringLiteral("(z) Save Location has the room the old column "
                             "did not (233 px, was 158)"),
              dialog.locationField()->width() == 233
                  && dialog.nameField()->width() == 304);

        dialog.show();
        pump(150);
        dialog.cancelButton()->click();
        pump(100);
        check(QStringLiteral("(z) Cancel closes it, having created nothing"),
              !dialog.isVisible() && dialog.result() == QDialog::Rejected
                  && dialog.mode() == NewProjectDialog::Mode::Cancelled);
    }
    {
        const QString where = scratch + QStringLiteral("/start_created");
        QDir(where).removeRecursively();
        QDir().mkpath(where);
        NewProjectDialog dialog(&window);
        dialog.nameField()->setText(QStringLiteral("MadeFromTheStartWindow"));
        dialog.locationField()->setText(QDir::toNativeSeparators(where));
        dialog.attemptCreate();
        const QString made = where + QStringLiteral(
            "/MadeFromTheStartWindow/MadeFromTheStartWindow.sankotv");
        const QVector<RecentProjects::Entry> recents = RecentProjects::entries();
        check(QStringLiteral("(z) Create still creates: the project file is "
                             "written and it is the newest recent"),
              dialog.mode() == NewProjectDialog::Mode::Created
                  && QFileInfo::exists(made) && !recents.isEmpty()
                  && QFileInfo(recents.first().path) == QFileInfo(made),
              dialog.validationReason());
    }

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (q) floating-bar suppression: PERMANENT lock ---------------------------
// Made permanent at the user's direction after the 2026-08-29 lost-bars
// regression: twice now a defect hid in a path only TEMPORARY seams ever
// covered (the studio seam never drove a modal, the modal seam never opened
// the studio), and twice it reached the user by hand. This section drives
// the COMPOSITION: the studio as suppression holder, the generic modal
// filter as a second holder, and the studio as that second holder's TARGET
// — the exact interleaving that ate the capture stack. Synthetic
// WindowBlocked/Unblocked events stand in for the real dialogs (Load…, the
// Done-failure warning): the filter keys on the event type alone, so the
// probe exercises the same code path with nothing to dismiss.
namespace {
QVector<FloatingToolWindow *> floatingBarsOf(MainWindow &window)
{
    QVector<FloatingToolWindow *> bars;
    const auto all = window.findChildren<FloatingToolWindow *>();
    for (FloatingToolWindow *w : all)
        if (!qobject_cast<brushlib::BrushSettingsStudio *>(w))
            bars.append(w);
    return bars;
}
int visibleIntentCount(const QVector<FloatingToolWindow *> &bars)
{
    int n = 0;
    for (FloatingToolWindow *w : bars)
        if (w->visibleIntent())
            ++n;
    return n;
}
} // namespace

void runSuppressionPass(const QString &scratch)
{
    out() << "--- (q) suppression lock: holder x target composition ---"
          << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/suppr"),
                                         QStringLiteral("Suppr"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(q) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *studio = window.findChild<brushlib::BrushSettingsStudio *>();
    const QVector<FloatingToolWindow *> bars = floatingBarsOf(window);
    check(QStringLiteral("(q) found the studio and the bars"),
          studio && !bars.isEmpty(),
          QStringLiteral("bars=%1").arg(bars.size()));
    if (!studio || bars.isEmpty()) {
        window.markCleanForTest();
        window.close();
        return;
    }
    const int baseline = visibleIntentCount(bars);
    check(QStringLiteral("(q) control: bars visible before anything"),
          baseline > 0, QStringLiteral("baseline=%1").arg(baseline));

    // Plain open/close — the path that always worked stays working.
    studio->openForPreset(QStringLiteral("builtin/painting/gouache"));
    pump(400);
    check(QStringLiteral("(q) opening the studio suppresses the bars"),
          visibleIntentCount(bars) == 0 && studio->isVisible());
    studio->hide();
    pump(400);
    check(QStringLiteral("(q) closing the studio restores them"),
          visibleIntentCount(bars) == baseline,
          QStringLiteral("intents=%1").arg(visibleIntentCount(bars)));

    // THE COMPOSITION: a modal fires while the studio is open. Before the
    // structural fix, the walk's hide of the studio fired its holder
    // restore mid-suppress and the true intents were consumed; the four
    // checks below each pin one leg of the corrected unwind.
    studio->openForPreset(QStringLiteral("builtin/painting/gouache"));
    pump(400);
    {
        QEvent blocked(QEvent::WindowBlocked);
        QCoreApplication::sendEvent(&window, &blocked);
    }
    pump(200);
    check(QStringLiteral("(q) during the modal the studio hides (it is a "
                         "TARGET of the generic suppress)"),
          !studio->isVisible());
    check(QStringLiteral("(q) ...and the bars STAY hidden (the mid-walk "
                         "reentrant restore is dead)"),
          visibleIntentCount(bars) == 0,
          QStringLiteral("intents=%1").arg(visibleIntentCount(bars)));
    {
        QEvent unblocked(QEvent::WindowUnblocked);
        QCoreApplication::sendEvent(&window, &unblocked);
    }
    pump(200);
    check(QStringLiteral("(q) after the modal the studio is back and the "
                         "bars are STILL suppressed (its capture "
                         "survived the modal)"),
          studio->isVisible() && visibleIntentCount(bars) == 0);
    studio->hide();
    pump(400);
    check(QStringLiteral("(q) THE REGRESSION: closing the studio after a "
                         "modal restores the bars"),
          visibleIntentCount(bars) == baseline,
          QStringLiteral("intents=%1").arg(visibleIntentCount(bars)));
    FloatingToolWindow::restoreFloatingBars(studio->anchorWidget());
    pump(200);
    check(QStringLiteral("(q) the stack is balanced: a further restore is "
                         "a no-op, not a recovery it should not need"),
          visibleIntentCount(bars) == baseline);

    // TRANSPARENCY PIN. What this protects, stated for whoever trips it:
    // the holder deliberately KEEPS firing on host-driven transitions —
    // a page switch reaches the machinery as a Hide event on the ANCHOR
    // (the stacked widget hides the storyboard page and the canvas with
    // it), the studio's effective hide transiently restores the bars'
    // captured intents, and its re-show re-captures them LIVE. That round
    // trip is the drift-proofing the original wiring established
    // (captures re-read intent, so a transient hide can never be
    // re-captured as a stale user choice). The structural fix silenced
    // holder actions ONLY for transitions the suppression walk itself
    // causes. If this check fails, someone has "simplified" the holder
    // into ignoring all non-user transitions — which trades away the
    // drift-proofing semantics, not just an implementation detail. Do not
    // re-baseline this; restore the cause-keyed behaviour. (The pin
    // drives the anchor's hide/show directly: the same code path a page
    // switch takes, without depending on the window manager the way a
    // minimize does.)
    studio->openForPreset(QStringLiteral("builtin/painting/gouache"));
    pump(400);
    check(QStringLiteral("(q) pin setup: studio open, bars suppressed"),
          visibleIntentCount(bars) == 0 && studio->isVisible());
    QWidget *anchor = studio->anchorWidget();
    anchor->hide(); // the page-switch transition, driven at the anchor
    pump(300);
    check(QStringLiteral("(q) TRANSPARENCY PIN: hiding the anchor (page "
                         "switch) transiently restores the bars' INTENTS "
                         "(see comment - the drift-proofing round trip "
                         "must survive)"),
          visibleIntentCount(bars) == baseline && studio->visibleIntent()
              && !studio->isVisible(),
          QStringLiteral("intents=%1").arg(visibleIntentCount(bars)));
    anchor->show();
    pump(300);
    check(QStringLiteral("(q) ...anchor return re-captures LIVE intents "
                         "and re-suppresses (the studio re-holds)"),
          visibleIntentCount(bars) == 0 && studio->isVisible());
    studio->hide();
    pump(400);
    check(QStringLiteral("(q) ...and the round trip ends with the bars "
                         "restored exactly"),
          visibleIntentCount(bars) == baseline);

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (r) the image-cap notices: told, never silent -------------------------
// The cap itself is engine behaviour (BrushLibrary b10); this drives the
// two moments the USER is told. (1) Import: loading an image the cap
// reduces shows a modal with the numbers - "your original file is
// unchanged" - and loading an under-cap image shows nothing. (2) The
// pre-cap re-save: pressing Done on a preset whose FILE still carries
// oversized images flags the rewrite instead of letting it be silent (the
// HB Pencil Variation rescue decision). Modals are dismissed by the
// harness's activeModalWidget timer, per the existing pattern.
void runImageCapNoticePass(const QString &scratch)
{
    out() << "--- (r) image-cap notices ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/capnote"),
                                         QStringLiteral("CapNote"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(r) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *studio = window.findChild<brushlib::BrushSettingsStudio *>();
    auto *model = window.findChild<brushlib::BrushLibraryModel *>();
    if (!studio || !model) {
        check(QStringLiteral("(r) found studio and model"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    const QString id = QStringLiteral("builtin/painting/gouache");
    studio->openForPreset(id);
    pump(400);

    const auto dismissSoon = [] {
        QTimer::singleShot(600, [] {
            if (QWidget *m = QApplication::activeModalWidget())
                m->close();
        });
    };

    // (1) an oversized import notices, with the numbers.
    QImage big(2500, 300, QImage::Format_Grayscale8);
    big.fill(120);
    dismissSoon();
    studio->applyLoadedGrainImage(big);
    pump(900);
    const QString notice = studio->lastImportNoticeForTest();
    check(QStringLiteral("(r) an oversized import shows the numbers and "
                         "the original-file reassurance"),
          notice.contains(QStringLiteral("2500"))
              && notice.contains(QStringLiteral("2048"))
              && notice.contains(QStringLiteral("original file is "
                                                "unchanged")),
          notice);

    // Control: an under-cap import is silent. (Named to dodge windows.h's
    // `small` macro.)
    QImage underCap(500, 400, QImage::Format_Grayscale8);
    underCap.fill(60);
    studio->applyLoadedGrainImage(underCap);
    pump(200);
    check(QStringLiteral("(r) control: an under-cap import shows NO "
                         "notice"),
          studio->lastImportNoticeForTest().isEmpty());

    // (2) the pre-cap re-save is flagged at Done, and the flag clears
    // after the write (the file now holds the capped form).
    model->setImagesCappedOnLoadForTest(id);
    check(QStringLiteral("(r) seam control: the stored preset reads as "
                         "pre-cap"),
          model->preset(id)->imagesCappedOnLoad);
    dismissSoon();
    studio->doneForTest(); // writes a gouache override in SCRATCH
    pump(900);
    check(QStringLiteral("(r) Done on a pre-cap preset says the rewrite "
                         "out loud"),
          studio->lastImportNoticeForTest().contains(
              QStringLiteral("reduced size")));
    check(QStringLiteral("(r) ...and the flag clears once the file holds "
                         "the capped form"),
          !model->preset(id)->imagesCappedOnLoad);
    // Leave the model as found: drop the override this Done created.
    check(QStringLiteral("(r) cleanup: the test's override resets to "
                         "stock"),
          model->resetBuiltinToStock(id));

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (s) identity colour: applies while active, restores after -------------
// Design (b), 2026-08-30: a preset with a non-black stored colour (Blue
// Pencil, Sanguine, Sepia; Chalk until 2026-09-05, when its white was
// removed for drawing invisibly on white paper) applies it while active;
// a black-ink preset restores the pre-adoption colour; an explicit pick
// while adopted wins and persists. The old behaviour adopted permanently
// - every pencil after Blue Pencil painted blue. The pass drives Blue
// Pencil and Sepia (two identity presets remain beyond them: Sanguine).
void runIdentityColorPass(const QString &scratch)
{
    out() << "--- (s) identity colour adopt/restore ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/idcol"),
                                         QStringLiteral("IdCol"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(s) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *canvas = window.findChild<DrawingCanvas *>();
    if (!canvas) {
        check(QStringLiteral("(s) found canvas"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    const ::Brush *blue = nullptr, *sepia = nullptr, *pencil2b = nullptr;
    const auto rosterS = brushlib::builtinRoster();
    for (const brushlib::BrushPreset &p : rosterS) {
        if (p.id == QStringLiteral("builtin/sketching/blue-pencil"))
            blue = &p.brush;
        if (p.id == QStringLiteral("builtin/drawing/sepia"))
            sepia = &p.brush;
        if (p.id == QStringLiteral("builtin/sketching/2b-pencil"))
            pencil2b = &p.brush;
    }
    check(QStringLiteral("(s) roster carries Blue, Sepia and 2B"),
          blue && sepia && pencil2b);
    if (!blue || !sepia || !pencil2b) {
        window.markCleanForTest();
        window.close();
        return;
    }
    const QColor red(200, 30, 30);
    canvas->setTool(DrawingCanvas::Brush);
    canvas->setColor(red);
    check(QStringLiteral("(s) control: the user's colour is set"),
          canvas->currentColorForTest() == red);

    canvas->setPaintBrush(*blue);
    check(QStringLiteral("(s) Blue Pencil applies its identity colour "
                         "while active"),
          canvas->currentColorForTest() == blue->color()
              && canvas->paintBrush().color() == blue->color());
    canvas->setPaintBrush(*pencil2b);
    check(QStringLiteral("(s) THE BUG: a black-ink preset RESTORES the "
                         "user's colour (every pencil after Blue used to "
                         "stay blue)"),
          canvas->currentColorForTest() == red
              && canvas->paintBrush().color() == red);

    // Identity -> identity -> black: the ORIGINAL user colour returns,
    // not the first identity colour.
    canvas->setPaintBrush(*blue);
    canvas->setPaintBrush(*sepia);
    check(QStringLiteral("(s) identity->identity shows the second "
                         "identity colour"),
          canvas->currentColorForTest() == sepia->color());
    canvas->setPaintBrush(*pencil2b);
    check(QStringLiteral("(s) ...and restoring skips the intermediate: "
                         "the ORIGINAL colour returns"),
          canvas->currentColorForTest() == red);

    // The override rule: an explicit pick while adopted wins and persists.
    const QColor green(30, 160, 60);
    canvas->setPaintBrush(*blue);
    canvas->setColor(green);
    canvas->setPaintBrush(*pencil2b);
    check(QStringLiteral("(s) an explicit pick while adopted WINS and "
                         "persists past the switch"),
          canvas->currentColorForTest() == green);

    // Eraser round trip while adopted: adoption survives, restore still
    // works afterwards.
    canvas->setPaintBrush(*blue);
    canvas->setTool(DrawingCanvas::Eraser);
    canvas->setTool(DrawingCanvas::Brush);
    check(QStringLiteral("(s) an eraser round-trip leaves the adopted "
                         "colour in place"),
          canvas->currentColorForTest() == blue->color());
    canvas->setPaintBrush(*pencil2b);
    check(QStringLiteral("(s) ...and the restore still lands after it"),
          canvas->currentColorForTest() == green);

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (m) the MIRRORED tool-scoped library, end to end ---------------------
// REWRITTEN AGAIN 2026-08-28 (mirror pass): the eraser scope now MIRRORS
// the brush categories - one definition, referenced twice - so the old
// assertions (eraser sidebar = {Eraser}, category stranding on scope flip)
// inverted. Pinned now: the mirror census through the REAL panel (every
// mirrorable preset appears under the eraser scope, every excluded one
// does not), the both-doors identity (the same preset activated through
// either door produces the same brush except eraseMode), scope following
// the tool with the CATEGORY SURVIVING the flip, activation never touching
// the tool, and the bar mirror.
void runEraserLibraryPass(const QString &scratch)
{
    out() << "--- (m) mirrored library, end to end ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/elib"),
                                         QStringLiteral("ELib"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(m) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *canvas = window.findChild<DrawingCanvas *>();
    auto *page = window.findChild<StoryboardPage *>();
    auto *panel = window.findChild<brushlib::BrushLibraryPanel *>();
    auto *model = window.findChild<brushlib::BrushLibraryModel *>();
    if (!canvas || !page || !panel || !model) {
        check(QStringLiteral("(m) found canvas, page, panel and model"),
              false);
        window.markCleanForTest();
        window.close();
        return;
    }
    using Scope = brushlib::BrushLibraryPanel::ToolScope;

    // --- the sidebar mirrors: same categories under both scopes ----------
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    const QStringList brushCats = panel->visibleCategoriesForTest();
    canvas->setTool(DrawingCanvas::Eraser);
    pump(200);
    check(QStringLiteral("(m) Eraser tool -> Eraser scope"),
          panel->toolScope() == Scope::Eraser);
    const QStringList eraserCats = panel->visibleCategoriesForTest();
    check(QStringLiteral("(m) the eraser sidebar MIRRORS the brush "
                         "categories"),
          eraserCats.contains(QStringLiteral("Sketching"))
              && eraserCats.contains(QStringLiteral("Painting"))
              && eraserCats.contains(QStringLiteral("Recent")),
          eraserCats.join(QStringLiteral(", ")));

    // --- THE MIRROR CENSUS through the real panel: every mirrorable
    // preset appears, every excluded one does not, per category.
    {
        bool allPresent = true, noneLeaked = true;
        int shown = 0, expected = 0;
        for (const QString &cat : brushlib::builtinCategories()) {
            panel->selectCategoryForTest(cat);
            const QStringList ids = panel->visiblePresetIdsForTest();
            shown += ids.size();
            for (const brushlib::BrushPreset *p : model->presetsIn(cat)) {
                const bool mirrorable = !p->brush.smudgeActive()
                    && !p->brush.dualBrushEnabled();
                if (mirrorable) {
                    ++expected;
                    allPresent = allPresent && ids.contains(p->id);
                } else {
                    noneLeaked = noneLeaked && !ids.contains(p->id);
                }
            }
        }
        // 56 -> 55 on 2026-09-28: Dry Brush became DUAL in the Painting
        // stamp pass (E5, the banded mask secondary), so it left the
        // mirror; (b8) in BrushLibraryTest carries the same 55 / 7 split.
        check(QStringLiteral("(m) every MIRRORABLE preset appears in the "
                             "eraser scope (55 across the categories)"),
              allPresent && shown == expected && expected == 55,
              QStringLiteral("shown=%1 expected=%2").arg(shown)
                  .arg(expected));
        check(QStringLiteral("(m) no EXCLUDED preset leaks through the "
                             "eraser door"),
              noneLeaked);
    }

    // --- both doors, one definition: same preset, same brush, except
    // eraseMode. (The eraser door copies the preset raw; the paint door's
    // documented colour-adoption rule makes byte-equality apply on the
    // ERASER side, which is the side the mirror added.)
    panel->selectCategoryForTest(QStringLiteral("Painting"));
    const brushlib::BrushPreset *gouache =
        model->preset(QStringLiteral("builtin/painting/gouache"));
    check(QStringLiteral("(m) control: Gouache exists"),
          gouache != nullptr);
    panel->activatePresetForTest(QStringLiteral("builtin/painting/gouache"));
    pump(300);
    check(QStringLiteral("(m) eraser-Gouache IS Gouache: byte-identical "
                         "except eraseMode"),
          [&] {
              if (!gouache)
                  return false;
              ::Brush cleared = canvas->eraserBrush();
              if (!cleared.eraseMode())
                  return false;
              cleared.setEraseMode(false);
              return brushlib::BrushPresetCodec::saveBrush(cleared)
                  == brushlib::BrushPresetCodec::saveBrush(gouache->brush);
          }());
    check(QStringLiteral("(m) activation did NOT touch the tool"),
          canvas->tool() == DrawingCanvas::Eraser);
    // DISPLAY-SIZE LAYER: the bar shows the eraser preset's VISIBLE width
    // (the erase variant has the brush's footprint, so the same ratio).
    check(QStringLiteral("(m) the bar mirrors eraser-Gouache (its visible "
                         "width)"),
          page->sizeCtlDisplayedSizeForTest()
              == brushlib::BrushWidthRatio::displaySize(
                     canvas->eraserBrush(), canvas->eraserBrush().size()),
          QStringLiteral("bar=%1 engine=%2")
              .arg(page->sizeCtlDisplayedSizeForTest())
              .arg(canvas->eraserBrush().size()));

    // --- the category SURVIVES the scope flip now (shared sidebar) -------
    check(QStringLiteral("(m) control: Painting is selected"),
          panel->currentCategoryForTest() == QStringLiteral("Painting"));
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    check(QStringLiteral("(m) back to Brush: scope follows"),
          panel->toolScope() == Scope::Brush);
    check(QStringLiteral("(m) the shared category SURVIVES the flip "
                         "(no stranding between mirrored scopes)"),
          panel->currentCategoryForTest() == QStringLiteral("Painting"),
          panel->currentCategoryForTest());

    // --- the same preset through the BRUSH door ---------------------------
    panel->activatePresetForTest(QStringLiteral("builtin/painting/gouache"));
    pump(300);
    check(QStringLiteral("(m) the brush door paints (eraseMode false, tool "
                         "untouched)"),
          canvas->tool() == DrawingCanvas::Brush
              && !canvas->paintBrush().eraseMode()
              && canvas->paintBrush().size() == gouache->brush.size());

    // --- both selections survived --------------------------------------
    check(QStringLiteral("(m) the eraser selection survived (still "
                         "eraser-Gouache)"),
          canvas->eraserBrush().eraseMode()
              && canvas->eraserBrush().size() == gouache->brush.size());

    // --- DISPLAY-SIZE LAYER: the library ROW's size label ----------------
    // The row prints the VISIBLE width (the worker measures the ratio with
    // the swatch); it used to print the engine size. Painting is showing.
    {
        QElapsedTimer wait;
        wait.start();
        const QString roundId = QStringLiteral("builtin/painting/round-brush");
        while (panel->rowSizeLabelForTest(roundId) < 0
               && wait.elapsed() < 15000)
            pump(50);
        const brushlib::BrushPreset *round = model->preset(roundId);
        check(QStringLiteral("(m) control: the Round Brush row exists and "
                             "has a label"),
              round && panel->rowSizeLabelForTest(roundId) > 0,
              QStringLiteral("label=%1")
                  .arg(panel->rowSizeLabelForTest(roundId)));
        if (round) {
            check(QStringLiteral("(m) the library row shows Round Brush's "
                                 "VISIBLE width, not its engine size"),
                  panel->rowSizeLabelForTest(roundId)
                          == brushlib::BrushWidthRatio::displaySize(
                                 round->brush, round->brush.size())
                      && panel->rowSizeLabelForTest(roundId)
                             < round->brush.size(),
                  QStringLiteral("row=%1 engine=%2")
                      .arg(panel->rowSizeLabelForTest(roundId))
                      .arg(round->brush.size()));
        }
        panel->selectCategoryForTest(QStringLiteral("Inking"));
        const QString splatterId = QStringLiteral("builtin/inking/splatter");
        wait.restart();
        while (panel->rowSizeLabelForTest(splatterId) < 0
               && wait.elapsed() < 15000)
            pump(50);
        const brushlib::BrushPreset *splatter = model->preset(splatterId);
        check(QStringLiteral("(m) an EXCLUDED scatter brush keeps its "
                             "droplet size as the row's number (Splatter)"),
              splatter
                  && panel->rowSizeLabelForTest(splatterId)
                         == splatter->brush.size(),
              QStringLiteral("row=%1").arg(
                  panel->rowSizeLabelForTest(splatterId)));
        panel->selectCategoryForTest(QStringLiteral("Painting"));
        pump(100);
    }

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
    // DISPLAY-SIZE LAYER (2026-09-29): the ENGINE holds the preset's size;
    // the BAR shows that size's visible width (engine x the brush's width
    // ratio). A default-constructed Brush is a SOFT round (hardness 0.75),
    // so its label is below its engine size - which also makes this a
    // control that the bar is not simply echoing the engine.
    auto labelFor = [&](int engine) {
        return brushlib::BrushWidthRatio::displaySize(canvas->paintBrush(),
                                                      engine);
    };
    check(QStringLiteral("(j) control: a 152 preset reaches the engine, and "
                         "the bar shows its visible width"),
          canvas->paintBrush().size() == 152
              && page->sizeCtlDisplayedSizeForTest() == labelFor(152),
          QStringLiteral("engine=%1 bar=%2 label=%3")
              .arg(canvas->paintBrush().size())
              .arg(page->sizeCtlDisplayedSizeForTest())
              .arg(labelFor(152)));
    check(QStringLiteral("(j) control: that soft brush's label is NOT its "
                         "engine size (the bar is not echoing the engine)"),
          labelFor(152) < 152 && labelFor(152) > 100,
          QStringLiteral("label=%1").arg(labelFor(152)));

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
    check(QStringLiteral("(j) ...and the BAR displays its visible width, "
                         "not a clamp (the old lie was 200)"),
          page->sizeCtlDisplayedSizeForTest() == labelFor(5000)
              && labelFor(5000) > 3000,
          QStringLiteral("bar=%1 label=%2")
              .arg(page->sizeCtlDisplayedSizeForTest())
              .arg(labelFor(5000)));

    // The Size CTL path itself: the bar's own setter spans the range.
    canvas->setBrushToolSize(5000);
    check(QStringLiteral("(j) setBrushToolSize(5000) holds in the engine"),
          canvas->paintBrush().size() == 5000);

    // The ERASER shares the 1..5000 log track since the erase composite
    // pass; the round-trip must still not disturb the brush's value.
    canvas->setTool(DrawingCanvas::Eraser);
    pump(200);
    const int eraserShown = page->sizeCtlDisplayedSizeForTest();
    check(QStringLiteral("(j) eraser mode shows an eraser-range value"),
          eraserShown >= 1 && eraserShown <= 5000,
          QStringLiteral("bar=%1").arg(eraserShown));
    canvas->setEraserSize(5000);
    check(QStringLiteral("(j) the eraser itself accepts 5000"),
          canvas->eraserSize() == 5000,
          QStringLiteral("eraser=%1").arg(canvas->eraserSize()));
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    check(QStringLiteral("(j) back to Brush: 5000 SURVIVED the eraser "
                         "round-trip"),
          page->sizeCtlDisplayedSizeForTest() == labelFor(5000)
              && canvas->paintBrush().size() == 5000,
          QStringLiteral("bar=%1 engine=%2")
              .arg(page->sizeCtlDisplayedSizeForTest())
              .arg(canvas->paintBrush().size()));

    // ---- THE DISPLAY-SIZE LAYER through the real bar ------------------
    // Every built-in that is relabelled: selecting it leaves the ENGINE at
    // the preset's own size (nothing the user approved draws differently)
    // while the bar shows the visible width; moving the bar the way the
    // user does sends the engine display / ratio; and the preset's own
    // label gives the engine exactly the preset's size back.
    {
        const auto rosterW = brushlib::builtinRoster();
        bool engineUntouched = true, barIsLabel = true, tickHonest = true,
             defaultBack = true;
        QString firstBad;
        int relabelled = 0;
        for (const brushlib::BrushPreset &p : rosterW) {
            if (p.brush.dualBrushEnabled() || p.brush.smudgeActive())
                continue; // own passes; the bar logic is identical
            const brushlib::WidthRatio r =
                brushlib::BrushWidthRatio::ratioFor(p.brush);
            canvas->setPaintBrush(p.brush);
            pump(20);
            const int engine0 = p.brush.size();
            const int label0 =
                brushlib::BrushWidthRatio::displaySize(p.brush, engine0);
            if (label0 != engine0)
                ++relabelled;
            if (canvas->paintBrush().size() != engine0) {
                engineUntouched = false;
                if (firstBad.isEmpty()) firstBad = p.id + QStringLiteral(" engine");
            }
            if (page->sizeCtlDisplayedSizeForTest() != label0) {
                barIsLabel = false;
                if (firstBad.isEmpty())
                    firstBad = QStringLiteral("%1 bar=%2 label=%3").arg(p.id)
                        .arg(page->sizeCtlDisplayedSizeForTest()).arg(label0);
            }
            page->sizeCtlUserSetSizeForTest(120); // a tick labelled 120
            const int want = brushlib::BrushWidthRatio::engineSizeWith(r, 120);
            if (canvas->paintBrush().size() != want
                || page->sizeCtlDisplayedSizeForTest() != 120) {
                tickHonest = false;
                if (firstBad.isEmpty())
                    firstBad = QStringLiteral("%1 tick120 engine=%2 want=%3")
                        .arg(p.id).arg(canvas->paintBrush().size()).arg(want);
            }
            page->sizeCtlUserSetSizeForTest(label0); // back to its label
            if (canvas->paintBrush().size() != engine0) {
                defaultBack = false;
                if (firstBad.isEmpty())
                    firstBad = QStringLiteral("%1 label %2 gave engine %3, "
                                              "default %4").arg(p.id)
                        .arg(label0).arg(canvas->paintBrush().size())
                        .arg(engine0);
            }
        }
        check(QStringLiteral("(j) DISPLAY LAYER: selecting a preset leaves "
                             "the ENGINE at the preset's own size"),
              engineUntouched, firstBad);
        check(QStringLiteral("(j) DISPLAY LAYER: the bar shows the visible "
                             "width of every preset selected"),
              barIsLabel, firstBad);
        check(QStringLiteral("(j) DISPLAY LAYER: control - presets really "
                             "are relabelled (not a roster of ratio 1.0)"),
              relabelled >= 30, QStringLiteral("relabelled=%1").arg(relabelled));
        check(QStringLiteral("(j) DISPLAY LAYER: a tick of 120 sends the "
                             "engine 120 / ratio and the bar reads 120"),
              tickHonest, firstBad);
        check(QStringLiteral("(j) DISPLAY LAYER: a preset's own label gives "
                             "the engine EXACTLY its default back"),
              defaultBack, firstBad);

        // The opacity multiplier must not move the size label.
        const ::Brush *round = nullptr;
        for (const brushlib::BrushPreset &p : rosterW)
            if (p.id == QStringLiteral("builtin/painting/round-brush"))
                round = &p.brush;
        if (round) {
            canvas->setPaintBrush(*round);
            pump(20);
            const int before = page->sizeCtlDisplayedSizeForTest();
            const int measuredBefore =
                brushlib::BrushWidthRatio::measureCountForTest();
            page->sizeCtlUserSetOpacityForTest(40); // the bar, as the user
            page->sizeCtlUserSetSizeForTest(120);
            page->sizeCtlUserSetSizeForTest(before);
            check(QStringLiteral("(j) DISPLAY LAYER: at opacity multiplier "
                                 "40 the preset's label still returns its "
                                 "default engine size"),
                  canvas->paintBrush().size() == round->size(),
                  QStringLiteral("engine=%1").arg(canvas->paintBrush().size()));
            check(QStringLiteral("(j) DISPLAY LAYER: ...and moving the "
                                 "opacity bar caused NO new measurement "
                                 "(the multiplier is divided back out, to "
                                 "the preset's exact key)"),
                  brushlib::BrushWidthRatio::measureCountForTest()
                      == measuredBefore,
                  QStringLiteral("measured %1 more").arg(
                      brushlib::BrushWidthRatio::measureCountForTest()
                          - measuredBefore));
            page->sizeCtlUserSetOpacityForTest(100);
        }
        // The stored size is the ENGINE size: what was set draws as before.
        ::Brush five;
        five.setSize(5000);
        canvas->setPaintBrush(five);
        pump(20);
    }

    // ---- MULTIPLIER semantics (2026-08-29, user-approved) --------------
    // The bar's opacity means "how much of the preset's own ceiling",
    // never an absolute the selection rewrites. Pinned here: selection
    // preserves the multiplier, the engine holds base x multiplier, the
    // COMPOUNDING rule (50% of a 0.55 preset = 0.275 - deliberately much
    // fainter than the old absolute 50%), and the safety property the
    // whole migration rests on: at 100 the product IS the preset, byte
    // for byte.
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    check(QStringLiteral("(j) control: the opacity bar reads the "
                         "multiplier, at its default 100"),
          page->sizeCtlDisplayedOpacityForTest() == 100,
          QStringLiteral("bar=%1")
              .arg(page->sizeCtlDisplayedOpacityForTest()));
    const ::Brush *hbStock = nullptr;
    const auto rosterJ = brushlib::builtinRoster();
    for (const brushlib::BrushPreset &p : rosterJ)
        if (p.id == QStringLiteral("builtin/sketching/hb-pencil"))
            hbStock = &p.brush;
    check(QStringLiteral("(j) control: HB Pencil exists with base 0.55"),
          hbStock && hbStock->opacity() == 0.55);
    if (hbStock) {
        canvas->setPaintBrush(*hbStock);
        pump(200);
        check(QStringLiteral("(j) selecting HB Pencil leaves the bar at "
                             "100 while the ENGINE holds the 0.55 base"),
              page->sizeCtlDisplayedOpacityForTest() == 100
                  && canvas->paintBrush().opacity() == 0.55,
              QStringLiteral("bar=%1 engine=%2")
                  .arg(page->sizeCtlDisplayedOpacityForTest())
                  .arg(canvas->paintBrush().opacity()));
        canvas->setBrushOpacity(50);
        check(QStringLiteral("(j) THE COMPOUNDING RULE: 50 percent of the "
                             "0.55 preset paints at 0.275"),
              qAbs(canvas->paintBrush().opacity() - 0.275) < 1e-9,
              QStringLiteral("engine=%1")
                  .arg(canvas->paintBrush().opacity()));
        canvas->setBrushOpacity(100);
        ::Brush colourNeutral = canvas->paintBrush();
        colourNeutral.setColor(hbStock->color());
        check(QStringLiteral("(j) at 100 the product IS the preset, byte "
                             "for byte (the migration's safety property)"),
              brushlib::BrushPresetCodec::saveBrush(colourNeutral)
                  == brushlib::BrushPresetCodec::saveBrush(*hbStock));
    }
    // Eraser symmetric: preset opacity is the base, the bar multiplies.
    const ::Brush *gouacheJ = nullptr;
    for (const brushlib::BrushPreset &p : rosterJ)
        if (p.id == QStringLiteral("builtin/painting/gouache"))
            gouacheJ = &p.brush;
    if (gouacheJ) {
        canvas->setEraserPreset(*gouacheJ); // base 0.95
        check(QStringLiteral("(j) eraser preset base survives at "
                             "multiplier 100"),
              canvas->eraserBrush().opacity() == gouacheJ->opacity());
        canvas->setEraserOpacity(50);
        check(QStringLiteral("(j) ...and the eraser bar compounds the "
                             "same way"),
              qAbs(canvas->eraserBrush().opacity()
                   - gouacheJ->opacity() * 0.5) < 1e-9,
              QStringLiteral("engine=%1")
                  .arg(canvas->eraserBrush().opacity()));
        canvas->setEraserOpacity(100);
    }

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

// ---- (ab) Reset Layout resets the Panel Strip too --------------------------
// View > Reset Layout cleared the dock controller's layout and re-applied its
// default, and never touched the strip: the strip is not a controller panel
// and keeps its own keys. A strip moved to the bottom, floated, stretched or
// hidden stayed there after "Reset Layout" and came back there on relaunch.
// Every reading below is taken first in the DEFAULT state, so "it is back"
// is compared with what the same reading said before anything was moved.
void runResetLayoutStripPass(const QString &project)
{
    out() << "--- (ab) Reset Layout resets the Panel Strip too ---" << Qt::endl;
    const QString keys = QStringLiteral("storyboard/panelStrip/v1/");
    {
        QSettings s = sankoSettings();
        s.remove(QStringLiteral("storyboard/panelStrip/v1")); // a first run
    }
    int defaultHeight = 0;
    {
        MainWindow window;
        window.resize(1400, 880);
        window.show();
        pump(800);
        check(QStringLiteral("(ab) project opens"),
              window.loadProjectForTest(project));
        pump(600);
        auto *storyboard = window.findChild<StoryboardPage *>();
        auto *host = storyboard
            ? storyboard->findChild<QMainWindow *>(
                  QStringLiteral("storyboardDockHost"))
            : nullptr;
        auto *strip = host
            ? host->findChild<QDockWidget *>(QStringLiteral("dockPanelStrip"))
            : nullptr;
        QAction *reset = nullptr, *save = nullptr;
        for (QAction *a : window.findChildren<QAction *>()) {
            if (a->text() == QStringLiteral("Reset Layout"))
                reset = a;
            if (a->text() == QStringLiteral("Save Layout"))
                save = a;
        }
        check(QStringLiteral("(ab) found the strip dock and the View menu's "
                             "Reset Layout and Save Layout"),
              host && strip && reset && save);
        if (!host || !strip || !reset || !save)
            return;

        auto isDefault = [&](QString *why) {
            const bool ok = host->dockWidgetArea(strip) == Qt::TopDockWidgetArea
                && !strip->isFloating() && strip->isVisible()
                && strip->toggleViewAction()->isChecked()
                && strip->widget()->height() == defaultHeight;
            *why = QStringLiteral("area %1, floating %2, visible %3, height %4")
                       .arg(int(host->dockWidgetArea(strip)))
                       .arg(strip->isFloating()).arg(strip->isVisible())
                       .arg(strip->widget()->height());
            return ok;
        };
        defaultHeight = strip->widget()->height();
        QString why;
        check(QStringLiteral("(ab) control: on a first run the strip is docked "
                             "at the TOP, visible, at its base height"),
              isDefault(&why) && defaultHeight == 159, why);

        // Move it to the bottom, stretch it, save, then hide it.
        host->addDockWidget(Qt::BottomDockWidgetArea, strip);
        pump(200);
        host->resizeDocks({strip}, {240}, Qt::Vertical);
        pump(300);
        save->trigger();
        const bool savedBottom =
            sankoSettings().value(keys + QStringLiteral("area")).toInt()
            == int(Qt::BottomDockWidgetArea);
        strip->toggleViewAction()->trigger(); // the user's own way to hide it
        pump(300);
        check(QStringLiteral("(ab) control: the strip really was moved to the "
                             "bottom, stretched, saved there, and hidden"),
              savedBottom && !isDefault(&why) && !strip->isVisible()
                  && host->dockWidgetArea(strip) == Qt::BottomDockWidgetArea,
              why);

        reset->trigger();
        pump(500);
        check(QStringLiteral("(ab) Reset Layout puts the strip back: top, "
                             "visible, base height"),
              isDefault(&why), why);
        check(QStringLiteral("(ab) ...and forgets where it had been saved"),
              !sankoSettings().contains(keys + QStringLiteral("area")));

        // Floating is the state a reset matters most for: a strip left on a
        // monitor that is no longer there.
        strip->setFloating(true);
        pump(300);
        const bool floated = strip->isFloating();
        reset->trigger();
        pump(500);
        check(QStringLiteral("(ab) a FLOATING strip is re-docked at the top "
                             "by Reset Layout (control: it was floating)"),
              floated && isDefault(&why), why);

        window.markCleanForTest();
        window.close();
        pump(300);
    }
    // What a relaunch restores is what Reset Layout left, not what had been
    // saved before it.
    {
        MainWindow window;
        window.resize(1400, 880);
        window.show();
        pump(800);
        window.loadProjectForTest(project);
        pump(600);
        auto *host = window.findChild<QMainWindow *>(
            QStringLiteral("storyboardDockHost"));
        auto *strip = host
            ? host->findChild<QDockWidget *>(QStringLiteral("dockPanelStrip"))
            : nullptr;
        check(QStringLiteral("(ab) after a relaunch the strip is still at the "
                             "top, visible, at its base height"),
              host && strip
                  && host->dockWidgetArea(strip) == Qt::TopDockWidgetArea
                  && !strip->isFloating() && strip->isVisible()
                  && strip->widget()->height() == defaultHeight,
              strip ? QStringLiteral("area %1, height %2")
                          .arg(int(host->dockWidgetArea(strip)))
                          .arg(strip->widget()->height())
                    : QStringLiteral("no strip"));
        window.markCleanForTest();
        window.close();
        pump(300);
    }
}

// ---- (ac) the Size bar fits the canvas -------------------------------------
// The floating Size bar was a fixed 46x574 and, unlike the managed toolbars,
// neither moved nor hid when the canvas was shorter than that: it hung out
// below the canvas, over whatever sat underneath (measured 2026-10-02: ~108
// px at the 1280x720 minimum window; 81 px on a maximised 1080p screen once
// a timeline sits under the canvas). It now shortens to fit, and is the
// Figma column exactly whenever that fits.
void runSizeBarFitPass(const QString &project)
{
    out() << "--- (ac) the Size bar fits the canvas ---" << Qt::endl;
    MainWindow window;
    window.resize(1400, 900);
    window.show();
    pump(800);
    check(QStringLiteral("(ac) project opens"),
          window.loadProjectForTest(project));
    pump(700);
    auto *storyboard = window.findChild<StoryboardPage *>();
    auto *canvas = window.findChild<DrawingCanvas *>();
    QWidget *bar = storyboard
        ? storyboard->findChild<QWidget *>(QStringLiteral("sizeCtlBar"))
        : nullptr;
    check(QStringLiteral("(ac) found the canvas and the Size bar, and the bar "
                         "is showing"),
          canvas && bar && bar->isVisible());
    if (!canvas || !bar)
        return;
    // The timeline under the canvas is folded away for this section: its
    // "room for the whole Figma column" case needs the canvas at its
    // tallest, which on this screen is a collapsed timeline. (With it open
    // the bar is the shortened one - section (ai) covers that.)
    auto *animatic = window.findChild<AnimaticPage *>();
    const bool wasCollapsed = animatic && animatic->isCollapsed();
    if (animatic)
        animatic->setCollapsed(true);
    pump(500);

    struct Reading
    {
        QSize bar;
        QVector<QRect> sliders; // top to bottom
        QRect flip;
        int above = 0, below = 0; // bar edge to canvas edge, px (negative = outside)
        QString text;
    };
    auto read = [&] {
        Reading r;
        r.bar = bar->size();
        for (QWidget *w :
             bar->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (qobject_cast<QPushButton *>(w))
                r.flip = w->geometry();
            else if (w->width() == 25)
                r.sliders.append(w->geometry());
        }
        std::sort(r.sliders.begin(), r.sliders.end(),
                  [](const QRect &x, const QRect &y) { return x.top() < y.top(); });
        const int canvasTop = canvas->mapToGlobal(QPoint(0, 0)).y();
        r.above = bar->frameGeometry().top() - canvasTop;
        r.below = canvasTop + canvas->height()
            - (bar->frameGeometry().top() + bar->height());
        r.text = QStringLiteral("canvas %1 px tall, bar %2x%3, %4 px inside "
                                "the top, %5 inside the bottom")
                     .arg(canvas->height()).arg(r.bar.width())
                     .arg(r.bar.height()).arg(r.above).arg(r.below);
        if (r.sliders.size() == 2)
            r.text += QStringLiteral("; sliders y %1 h %2 / y %3 h %4, Flip y %5")
                          .arg(r.sliders.at(0).top()).arg(r.sliders.at(0).height())
                          .arg(r.sliders.at(1).top()).arg(r.sliders.at(1).height())
                          .arg(r.flip.top());
        return r;
    };
    auto isFigma = [](const Reading &r) {
        return r.bar == QSize(46, 574) && r.sliders.size() == 2
            && r.sliders.at(0) == QRect(10, 25, 25, 220)
            && r.flip == QRect(8, 276, 30, 30)
            && r.sliders.at(1) == QRect(10, 337, 25, 220);
    };

    const Reading tall = read();
    check(QStringLiteral("(ac) with room for it the bar is the Figma column "
                         "EXACTLY: 46x574, sliders at 25 and 337, Flip at 276"),
          canvas->height() >= 574 + 8 && isFigma(tall), tall.text);
    check(QStringLiteral("(ac) ...and inside the canvas with its margin"),
          tall.above >= 4 && tall.below >= 4, tall.text);

    window.resize(1280, 720); // the smallest window the app allows
    pump(900);
    // (Not named `small`: windows.h defines that as a macro.)
    const Reading tight = read();
    check(QStringLiteral("(ac) control: at the minimum window the canvas "
                         "CANNOT hold a 574 px bar"),
          canvas->height() < 574 + 8, tight.text);
    check(QStringLiteral("(ac) at the minimum window the bar is INSIDE the "
                         "canvas, margin and all - it no longer hangs below"),
          tight.above >= 4 && tight.below >= 4
              && tight.bar.height() == canvas->height() - 8,
          tight.text);
    const bool ordered = tight.sliders.size() == 2
        && tight.sliders.at(0).top() > 0
        && tight.sliders.at(0).bottom() < tight.flip.top()
        && tight.flip.bottom() < tight.sliders.at(1).top()
        && tight.sliders.at(1).bottom() < tight.bar.height()
        && tight.sliders.at(0).height() == tight.sliders.at(1).height()
        && tight.sliders.at(0).height() >= 60 && tight.flip.size() == QSize(30, 30);
    check(QStringLiteral("(ac) ...with both sliders the same height, Flip "
                         "between them, nothing overlapping or cut off"),
          ordered, tight.text);

    // A shorter slider is still a whole slider: its two ends are its range.
    QWidget *sizeSlider = nullptr;
    for (QWidget *w :
         bar->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (w->width() == 25 && (!sizeSlider || w->y() < sizeSlider->y()))
            sizeSlider = w;
    const int before = storyboard->sizeCtlDisplayedSizeForTest();
    if (sizeSlider) {
        sendMouse(sizeSlider, QEvent::MouseButtonPress,
                  QPointF(12, sizeSlider->height() - 1), Qt::LeftButton);
        sendMouse(sizeSlider, QEvent::MouseButtonRelease,
                  QPointF(12, sizeSlider->height() - 1), Qt::LeftButton);
        pump(150);
        const int atBottom = storyboard->sizeCtlDisplayedSizeForTest();
        sendMouse(sizeSlider, QEvent::MouseButtonPress, QPointF(12, 1),
                  Qt::LeftButton);
        sendMouse(sizeSlider, QEvent::MouseButtonRelease, QPointF(12, 1),
                  Qt::LeftButton);
        pump(150);
        const int atTop = storyboard->sizeCtlDisplayedSizeForTest();
        check(QStringLiteral("(ac) the shortened size slider still reaches "
                             "both ends of its range"),
              atBottom >= 1 && atTop > atBottom * 100,
              QStringLiteral("bottom %1, top %2").arg(atBottom).arg(atTop));
        storyboard->sizeCtlUserSetSizeForTest(before); // leave it as found
        pump(150);
    }

    window.resize(1400, 900);
    pump(900);
    const Reading again = read();
    check(QStringLiteral("(ac) given the room back, it is the Figma column "
                         "again"),
          isFigma(again) && again.above >= 4 && again.below >= 4, again.text);

    if (animatic)
        animatic->setCollapsed(wasCollapsed); // as found: the state persists
    pump(300);
    window.markCleanForTest();
    window.close();
    pump(300);
}

// ======================= THE COMBINED WORKSPACE ============================
// The Animatic screen is gone: its timeline sits under the drawing canvas and
// plays in a preview laid over it. Sections (ad)-(ai) hold what that change
// had to get right, each one a risk named before it was built:
//   (ad) the strip and the timeline are ONE set of panels, and a selection
//        made in either selects exactly once;
//   (ae) the animatic never keeps a row for a panel that has been freed;
//   (af) playing or scrubbing changes nothing in the document or the canvas;
//   (ag) Space and friends belong to the timeline only under the pointer;
//   (ah) a timing change is undoable, in order with drawing;
//   (ai) the timeline's place, height and collapse persist and reset, and
//        the floating toolbars come back to the same places.
namespace workspace {

struct Rig
{
    MainWindow window;
    StoryboardPage *storyboard = nullptr;
    AnimaticPage *animatic = nullptr;
    AnimaticTimeline *timeline = nullptr;
    QWidget *surface = nullptr; // the timeline's painted canvas
    DrawingCanvas *canvas = nullptr;
    bool ok = false;

    explicit Rig(const QString &project, const QSize &size = QSize(1400, 900))
    {
        window.resize(size);
        window.show();
        pump(800);
        if (!window.loadProjectForTest(project))
            return;
        pump(700);
        storyboard = window.findChild<StoryboardPage *>();
        animatic = window.findChild<AnimaticPage *>();
        canvas = window.findChild<DrawingCanvas *>();
        timeline = animatic ? animatic->timelineForTest() : nullptr;
        surface = timeline ? timeline->surfaceForTest() : nullptr;
        ok = storyboard && animatic && canvas && timeline && surface;
        // Whose keys the timeline's are depends on the pointer, and this
        // machine has a real one, wherever its owner left it. From here the
        // page hears only the pointer events hover() delivers.
        if (storyboard)
            storyboard->setRealPointerIgnoredForTest(true);
    }
    ~Rig()
    {
        window.markCleanForTest();
        window.close();
        pump(300);
    }
};

QVector<QLabel *> thumbs(StoryboardPage *page)
{
    // The strip rebuilds by deleteLater-ing its old thumbnails. Inside the
    // app's event loop they are gone by the next turn; a test that calls a
    // page function directly has no loop above it for them to be deleted
    // from, so they would be counted here as if still on the strip.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVector<QLabel *> list =
        page->findChildren<QLabel *>(QStringLiteral("panelThumb")).toVector();
    std::sort(list.begin(), list.end(), [](QLabel *x, QLabel *y) {
        return x->property("panelIndex").toInt()
            < y->property("panelIndex").toInt();
    });
    return list;
}

// Which strip thumbnail wears the selection border (-1: none).
int selectedThumb(StoryboardPage *page)
{
    const QVector<QLabel *> list = thumbs(page);
    for (int i = 0; i < list.size(); ++i)
        if (list.at(i)->styleSheet().contains(QStringLiteral("3px solid")))
            return i;
    return -1;
}

void clickThumb(Rig &r, int index)
{
    const QVector<QLabel *> list = thumbs(r.storyboard);
    if (index < 0 || index >= list.size())
        return;
    const QPointF c(list.at(index)->width() / 2.0, list.at(index)->height() / 2.0);
    sendMouse(list.at(index), QEvent::MouseButtonPress, c, Qt::LeftButton);
    sendMouse(list.at(index), QEvent::MouseButtonRelease, c, Qt::LeftButton);
    pump(200);
}

void clickClip(Rig &r, int flat)
{
    const QPointF c = r.timeline->clipRectForTest(flat).center();
    sendMouse(r.surface, QEvent::MouseButtonPress, c, Qt::LeftButton);
    sendMouse(r.surface, QEvent::MouseButtonRelease, c, Qt::LeftButton);
    pump(200);
}

void rightClickClip(Rig &r, int flat)
{
    const QPoint c = r.timeline->clipRectForTest(flat).center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, c, r.surface->mapToGlobal(c));
    QCoreApplication::sendEvent(r.surface, &ev);
    pump(250);
}

QVector<int> durations(Rig &r)
{
    QVector<int> d;
    for (int i = 0; i < r.timeline->blockCountForTest(); ++i)
        d.append(r.timeline->blockDurationForTest(i));
    return d;
}

QString show(const QVector<int> &v)
{
    QStringList parts;
    for (int x : v)
        parts << QString::number(x);
    return parts.join(QLatin1Char(' '));
}

// Everything the two views say about themselves, for a failing check.
QString state(Rig &r)
{
    return QStringLiteral("clips %1 | rows %2, strip thumbs %3 (highlight "
                          "%4), selected: workspace %5 timeline %6, playhead "
                          "%7")
        .arg(show(durations(r))).arg(r.animatic->itemCountForTest())
        .arg(thumbs(r.storyboard).size()).arg(selectedThumb(r.storyboard))
        .arg(r.storyboard->selectedFlatIndexForTest())
        .arg(r.timeline->selectedPanelForTest())
        .arg(r.timeline->playheadPanelForTest());
}

// The pointer arriving over / leaving a widget, as Qt would announce it:
// the same Enter / Leave, sent the same way, through the page's own event
// filter. (The real cursor is not moved - this machine may be in use - and
// since 2026-10-03 the page does not listen to it either: with the real
// pointer resting over the test window its events arrived in between these
// and six checks failed on unchanged code.)
void hover(Rig &r, QWidget *w, bool over)
{
    if (over) {
        const QPointF p(6, 6);
        QEnterEvent ev(p, p, w->mapToGlobal(p.toPoint()));
        r.storyboard->sendPointerEventForTest(w, &ev);
    } else {
        QEvent ev(QEvent::Leave);
        r.storyboard->sendPointerEventForTest(w, &ev);
    }
    pump(60);
}

// True when a SHORTCUT claimed the key (the canvas's own key handling is
// not a shortcut, so "not claimed" is what leaves a key to the canvas).
bool keyClaimed(QWidget *target, int key,
                Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    const bool claimed = startWindowKeys::press(target, key, mods);
    pump(200);
    return claimed;
}

void stroke(DrawingCanvas *canvas, int row = 0)
{
    const QTransform t = canvas->viewTransformForTest();
    const qreal y = 160 + 40 * row;
    sendMouse(canvas, QEvent::MouseButtonPress, t.map(QPointF(200, y)),
              Qt::LeftButton);
    for (int i = 1; i <= 8; ++i)
        sendMouse(canvas, QEvent::MouseMove,
                  t.map(QPointF(200 + i * 10, y + i * 5)), Qt::LeftButton);
    sendMouse(canvas, QEvent::MouseButtonRelease, t.map(QPointF(280, y + 40)),
              Qt::LeftButton);
}

class PaintCount : public QObject
{
public:
    int paints = 0;
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};

// Where every visible floating tool window sits, relative to the canvas.
QStringList barPlaces(Rig &r)
{
    QStringList places;
    const QPoint origin = r.canvas->mapToGlobal(QPoint(0, 0));
    for (FloatingToolWindow *w : r.window.findChildren<FloatingToolWindow *>())
        if (w->isVisible())
            places << QStringLiteral("%1:%2,%3 %4x%5")
                          .arg(w->objectName().isEmpty()
                                   ? QString::fromLatin1(
                                         w->metaObject()->className())
                                   : w->objectName())
                          .arg(w->x() - origin.x()).arg(w->y() - origin.y())
                          .arg(w->width()).arg(w->height());
    places.sort();
    return places;
}

bool barsInsideCanvas(Rig &r, QString *why)
{
    const QRect canvas(r.canvas->mapToGlobal(QPoint(0, 0)), r.canvas->size());
    for (FloatingToolWindow *w : r.window.findChildren<FloatingToolWindow *>())
        if (w->isVisible() && !canvas.contains(w->frameGeometry())) {
            *why = QStringLiteral("%1 at %2,%3 %4x%5 is not inside the %6x%7 "
                                  "canvas")
                       .arg(QString::fromLatin1(w->metaObject()->className()))
                       .arg(w->x() - canvas.x()).arg(w->y() - canvas.y())
                       .arg(w->width()).arg(w->height()).arg(canvas.width())
                       .arg(canvas.height());
            return false;
        }
    return true;
}

} // namespace workspace

void runWorkspaceSyncPass(const QString &project)
{
    using namespace workspace;
    out() << "--- (ad) one workspace: the strip and the timeline are the "
             "same panels ---" << Qt::endl;
    Rig r(project);
    check(QStringLiteral("(ad) the project opened into the workspace, with "
                         "its timeline"),
          r.ok);
    if (!r.ok)
        return;

    // ---- no screen to go to ------------------------------------------------
    bool continueButton = false;
    for (QPushButton *b : r.window.findChildren<QPushButton *>())
        if (b->text().contains(QStringLiteral("Continue to Animatic")))
            continueButton = true;
    auto *stack = qobject_cast<QStackedWidget *>(r.window.centralWidget());
    bool animaticIsAPage = false;
    for (int i = 0; stack && i < stack->count(); ++i)
        if (stack->widget(i) == r.animatic)
            animaticIsAPage = true;
    check(QStringLiteral("(ad) there is no Animatic screen and no Continue "
                         "to Animatic button: the animatic is inside the "
                         "workspace, on screen"),
          !continueButton && stack && !animaticIsAPage
              && r.storyboard->isAncestorOf(r.animatic)
              && r.animatic->isVisible() && r.surface->isVisible());
    // The Generation page went the same way (Pass 3): no button leads to
    // it and the window has four screens - start, script, workspace,
    // consistency board. The same search still finds Export MP4, the one
    // button of the old Animatic screen that has not moved yet.
    bool generationButton = false, exportStillThere = false;
    for (QPushButton *b : r.window.findChildren<QPushButton *>()) {
        if (b->text().contains(QStringLiteral("Generation")))
            generationButton = true;
        if (b->text() == QStringLiteral("Export MP4"))
            exportStillThere = true;
    }
    check(QStringLiteral("(ad) there is no Generation screen and no button "
                         "to it: the window has four screens (control: the "
                         "same search finds Export MP4)"),
          !generationButton && exportStillThere && stack
              && stack->count() == 4,
          QStringLiteral("%1 screen(s)").arg(stack ? stack->count() : -1));
    auto *strip = r.storyboard->findChild<QDockWidget *>(
        QStringLiteral("dockPanelStrip"));
    const int stripY = strip ? strip->mapToGlobal(QPoint(0, 0)).y() : -1;
    const int canvasY = r.canvas->mapToGlobal(QPoint(0, 0)).y();
    const int timelineY = r.animatic->mapToGlobal(QPoint(0, 0)).y();
    check(QStringLiteral("(ad) top to bottom: Panel Strip, drawing canvas, "
                         "timeline"),
          strip && stripY < canvasY
              && canvasY + r.canvas->height() <= timelineY,
          QStringLiteral("strip y %1, canvas y %2..%3, timeline y %4")
              .arg(stripY).arg(canvasY).arg(canvasY + r.canvas->height())
              .arg(timelineY));

    // ---- fed on open, not on a visit ---------------------------------------
    check(QStringLiteral("(ad) the timeline holds every panel of every scene "
                         "the moment the project opens (2 scenes x 3)"),
          r.timeline->blockCountForTest() == 6
              && r.animatic->itemCountForTest() == 6
              && thumbs(r.storyboard).size() == 3,
          QStringLiteral("%1 clips, %2 rows, %3 strip thumbs")
              .arg(r.timeline->blockCountForTest())
              .arg(r.animatic->itemCountForTest())
              .arg(thumbs(r.storyboard).size()));
    check(QStringLiteral("(ad) ...with the durations the panels carry"),
          durations(r) == QVector<int>({2, 3, 4, 2, 3, 4}), show(durations(r)));
    check(QStringLiteral("(ad) the opening selection is the same panel in "
                         "both views, and the playhead is on it"),
          r.storyboard->selectedFlatIndexForTest() == 0
              && r.timeline->selectedPanelForTest() == 0
              && r.timeline->playheadPanelForTest() == 0
              && selectedThumb(r.storyboard) == 0);

    // ---- selection, both directions, ONCE each ------------------------------
    int calls = r.storyboard->selectPanelCallsForTest();
    clickThumb(r, 2);
    check(QStringLiteral("(ad) a STRIP click selects that panel in the "
                         "timeline and moves the playhead to it"),
          r.timeline->selectedPanelForTest() == 2
              && r.timeline->playheadPanelForTest() == 2
              && selectedThumb(r.storyboard) == 2);
    check(QStringLiteral("(ad) ...and selected exactly ONCE (the timeline "
                         "did not answer back)"),
          r.storyboard->selectPanelCallsForTest() == calls + 1,
          QStringLiteral("%1 selection(s)")
              .arg(r.storyboard->selectPanelCallsForTest() - calls));

    calls = r.storyboard->selectPanelCallsForTest();
    clickClip(r, 4); // scene 2, its second panel
    check(QStringLiteral("(ad) a TIMELINE click on a clip in another scene "
                         "selects it in the strip: the strip shows that "
                         "scene with that panel highlighted"),
          r.storyboard->selectedFlatIndexForTest() == 4
              && thumbs(r.storyboard).size() == 3
              && selectedThumb(r.storyboard) == 1
              && r.timeline->selectedPanelForTest() == 4,
          QStringLiteral("flat %1, strip highlight %2")
              .arg(r.storyboard->selectedFlatIndexForTest())
              .arg(selectedThumb(r.storyboard)));
    check(QStringLiteral("(ad) ...exactly ONCE, scene change included (not "
                         "panel 1 of the scene and then the one asked for)"),
          r.storyboard->selectPanelCallsForTest() == calls + 1,
          QStringLiteral("%1 selection(s)")
              .arg(r.storyboard->selectPanelCallsForTest() - calls));
    calls = r.storyboard->selectPanelCallsForTest();
    clickClip(r, 4);
    check(QStringLiteral("(ad) clicking the clip that is already selected "
                         "selects nothing again"),
          r.storyboard->selectPanelCallsForTest() == calls
              && r.storyboard->selectedFlatIndexForTest() == 4);

    // ---- add / duplicate / delete, from either view -------------------------
    r.storyboard->copySelectedPanel();
    r.storyboard->pastePanelAfterSelected(); // the strip side's own insert
    pump(250);
    check(QStringLiteral("(ad) a panel added on the STRIP side appears in "
                         "the timeline, where it was added, with its "
                         "duration"),
          durations(r) == QVector<int>({2, 3, 4, 2, 3, 3, 4})
              && thumbs(r.storyboard).size() == 4
              && r.timeline->selectedPanelForTest() == 5
              && r.animatic->itemCountForTest() == 7,
          state(r));

    QStringList menuSaw;
    QString trigger;
    r.storyboard->setClipMenuHookForTest([&](QMenu *menu) {
        menuSaw.clear();
        for (QAction *a : menu->actions())
            if (!a->isSeparator())
                menuSaw << a->text()
                        + (a->isEnabled() ? QString()
                                          : QStringLiteral(" (disabled)"));
        for (QAction *a : menu->actions())
            if (a->text() == trigger)
                a->trigger();
    });
    trigger = QStringLiteral("Duplicate Panel");
    rightClickClip(r, 3); // scene 2's first panel, which is NOT the selection
    check(QStringLiteral("(ad) right-clicking a clip selects it and offers "
                         "Add Panel After, Duplicate Panel, Delete Panel"),
          menuSaw == QStringList({QStringLiteral("Add Panel After"),
                                  QStringLiteral("Duplicate Panel"),
                                  QStringLiteral("Delete Panel")}),
          menuSaw.join(QStringLiteral(" | ")));
    check(QStringLiteral("(ad) Duplicate from the TIMELINE duplicates THAT "
                         "clip's panel, and the strip shows it"),
          durations(r) == QVector<int>({2, 3, 4, 2, 2, 3, 3, 4})
              && thumbs(r.storyboard).size() == 5
              && r.storyboard->selectedFlatIndexForTest() == 4
              && selectedThumb(r.storyboard) == 1,
          state(r));
    trigger = QStringLiteral("Add Panel After");
    rightClickClip(r, 4);
    check(QStringLiteral("(ad) Add Panel After from the timeline adds a new "
                         "panel after that clip, in both views"),
          r.timeline->blockCountForTest() == 9
              && thumbs(r.storyboard).size() == 6
              && r.storyboard->selectedFlatIndexForTest() == 5
              && r.timeline->blockDurationForTest(5) == 3,
          state(r));
    r.storyboard->cutSelectedPanel(); // the same removal Delete performs
    pump(250);
    check(QStringLiteral("(ad) removing a panel removes its clip"),
          durations(r) == QVector<int>({2, 3, 4, 2, 2, 3, 3, 4})
              && thumbs(r.storyboard).size() == 5
              && r.animatic->itemCountForTest() == 8,
          state(r));
    r.storyboard->setClipMenuHookForTest({});

    // ---- reorder, from either view, and undo --------------------------------
    clickClip(r, 2); // scene 1's last panel (4 s)
    const bool moved = keyClaimed(&r.window, Qt::Key_Left, Qt::ControlModifier);
    check(QStringLiteral("(ad) reordering on the STRIP side (Ctrl+Left) "
                         "reorders the clips"),
          moved && durations(r) == QVector<int>({2, 4, 3, 2, 2, 3, 3, 4})
              && r.timeline->selectedPanelForTest() == 1,
          state(r));
    {
        // Drag the first clip past the middle of the scene's last one.
        const QPointF from = r.timeline->clipRectForTest(0).center();
        const QPointF to(r.timeline->clipRectForTest(2).center().x() + 12,
                         from.y());
        sendMouse(r.surface, QEvent::MouseButtonPress, from, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, from + QPointF(12, 0),
                  Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, to, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseButtonRelease, to, Qt::LeftButton);
        pump(250);
    }
    check(QStringLiteral("(ad) dragging a clip on the TIMELINE reorders the "
                         "panels, and the strip follows (the moved panel "
                         "stays selected)"),
          durations(r) == QVector<int>({4, 3, 2, 2, 2, 3, 3, 4})
              && r.storyboard->selectedFlatIndexForTest() == 2
              && selectedThumb(r.storyboard) == 2,
          state(r));
    {
        // A press that does not travel is a click, not a reorder.
        const QVector<int> before = durations(r);
        const QPointF at = r.timeline->clipRectForTest(1).center();
        sendMouse(r.surface, QEvent::MouseButtonPress, at, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, at + QPointF(3, 0),
                  Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseButtonRelease, at + QPointF(3, 0),
                  Qt::LeftButton);
        pump(200);
        check(QStringLiteral("(ad) control: a press that moves 3 px is a "
                             "click - it selects and reorders nothing"),
              durations(r) == before
                  && r.storyboard->selectedFlatIndexForTest() == 1);
    }
    r.window.undoStackForTest()->undo();
    pump(200);
    const bool undoneOnce =
        durations(r) == QVector<int>({2, 4, 3, 2, 2, 3, 3, 4});
    r.window.undoStackForTest()->undo();
    pump(200);
    check(QStringLiteral("(ad) both reorders are ONE history: undo takes "
                         "back the timeline's, then the strip's"),
          undoneOnce && durations(r) == QVector<int>({2, 3, 4, 2, 2, 3, 3, 4}),
          state(r));

    // ---- thumbnails: one repaint, after the stroke ---------------------------
    pump(400);
    PaintCount paints;
    r.surface->installEventFilter(&paints);
    paints.paints = 0;
    stroke(r.canvas);
    QCoreApplication::processEvents();
    const int during = paints.paints;
    pump(700);
    check(QStringLiteral("(ad) the timeline does not repaint DURING a "
                         "stroke (measured before this was built: it must "
                         "cost drawing nothing)"),
          during == 0, QStringLiteral("%1 repaint(s)").arg(during));
    check(QStringLiteral("(ad) ...and repaints once the edit settles, so "
                         "its clip shows the new artwork (control: the "
                         "counter can see a repaint)"),
          paints.paints >= 1, QStringLiteral("%1 repaint(s)").arg(paints.paints));
    r.surface->removeEventFilter(&paints);
}

void runWorkspaceFreedPanelPass(const QString &project, const QString &other)
{
    using namespace workspace;
    out() << "--- (ae) the animatic never keeps a freed panel ---" << Qt::endl;
    Rig r(project);
    check(QStringLiteral("(ae) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    const int before = r.animatic->itemCountForTest();
    clickThumb(r, 1);
    r.storyboard->cutSelectedPanel(); // the undo command now owns the panel
    pump(250);
    // Dropping the history DELETES the removed panel for real. A row left
    // pointing at it would be read by everything below; a version that does
    // not rebuild its rows dies here instead of failing a comparison, so
    // the line above has already been flushed.
    r.window.undoStackForTest()->clear();
    pump(100);
    check(QStringLiteral("(ae) control: removing a panel removed its row "
                         "(the rows were rebuilt, not left alone)"),
          r.animatic->itemCountForTest() == before - 1
              && r.timeline->blockCountForTest() == before - 1,
          QStringLiteral("%1 -> %2").arg(before)
              .arg(r.animatic->itemCountForTest()));
    r.surface->repaint();
    r.animatic->togglePlay();
    pump(150);
    for (int i = 0; i < r.animatic->itemCountForTest() + 2; ++i) {
        r.animatic->advanceForTest(); // every row, through the end and round
        pump(30);
    }
    check(QStringLiteral("(ae) repainting the timeline and playing through "
                         "every panel after the removed one was freed is "
                         "safe"),
          true);

    // Opening another project while it PLAYS: the rows, the preview's
    // picture and the timer all refer to panels that are about to go.
    r.animatic->togglePlay();
    pump(150);
    const bool wasPlaying = r.animatic->isPlaying() && r.animatic->previewVisible();
    check(QStringLiteral("(ae) a second project opens while the first is "
                         "playing"),
          r.window.loadProjectForTest(other));
    pump(500);
    check(QStringLiteral("(ae) ...playback stopped, the preview is gone, and "
                         "the timeline holds the NEW project's panels "
                         "(control: it was playing)"),
          wasPlaying && !r.animatic->isPlaying() && !r.animatic->previewVisible()
              && r.animatic->itemCountForTest() == 2
              && r.timeline->blockCountForTest() == 2
              && r.timeline->selectedPanelForTest() == 0,
          QStringLiteral("%1 row(s)").arg(r.animatic->itemCountForTest()));
    r.animatic->togglePlay();
    pump(150);
    r.window.markCleanForTest();
    r.window.closeProjectForTest();
    pump(400);
    check(QStringLiteral("(ae) Close Project while playing leaves the "
                         "animatic empty and stopped"),
          !r.animatic->isPlaying() && r.animatic->itemCountForTest() == 0
              && r.timeline->blockCountForTest() == 0);
    r.window.newProjectForTest();
    pump(400);
    check(QStringLiteral("(ae) ...and so does New Project"),
          r.animatic->itemCountForTest() == 0
              && r.timeline->blockCountForTest() == 0);
}

void runWorkspacePreviewPass(const QString &project)
{
    using namespace workspace;
    out() << "--- (af) playing and scrubbing leave the drawing alone ---"
          << Qt::endl;
    Rig r(project);
    check(QStringLiteral("(af) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    QWidget *preview = r.animatic->previewSurface();
    r.canvas->selectAll();
    pump(300);
    const bool hadSelection = r.canvas->hasSelection();
    r.window.markCleanForTest();
    const int undoIndex = r.window.undoStackForTest()->index();
    const int calls = r.storyboard->selectPanelCallsForTest();
    check(QStringLiteral("(af) control: before Play there is no preview, "
                         "and the canvas holds a selection"),
          preview && !preview->isVisible() && !r.animatic->previewVisible()
              && hadSelection);

    r.animatic->togglePlay();
    pump(200);
    check(QStringLiteral("(af) Play shows the preview OVER the canvas - a "
                         "child of it, covering it exactly - with the first "
                         "panel's picture"),
          r.animatic->isPlaying() && r.animatic->previewVisible()
              && preview->isVisible() && preview->parentWidget() == r.canvas
              && preview->geometry() == r.canvas->rect()
              && r.animatic->previewHasPictureForTest()
              && r.animatic->previewCaptionForTest().contains(
                     QStringLiteral("Panel 1")),
          r.animatic->previewCaptionForTest());
    r.animatic->advanceForTest(); // the per-panel timer, without the wait
    pump(120);
    check(QStringLiteral("(af) playback advances the PLAYHEAD and the "
                         "preview..."),
          r.timeline->playheadPanelForTest() == 1
              && r.animatic->previewCaptionForTest().contains(
                     QStringLiteral("Panel 2")),
          r.animatic->previewCaptionForTest());
    check(QStringLiteral("(af) ...and NOT the selection: same panel in the "
                         "strip, in the timeline and on the canvas, and no "
                         "panel was selected along the way"),
          r.storyboard->selectedFlatIndexForTest() == 0
              && r.timeline->selectedPanelForTest() == 0
              && selectedThumb(r.storyboard) == 0
              && r.storyboard->selectPanelCallsForTest() == calls);
    check(QStringLiteral("(af) the canvas still holds its selection, the "
                         "project is still clean, and the undo history has "
                         "not moved"),
          r.canvas->hasSelection() && !r.window.isDirty()
              && r.window.undoStackForTest()->index() == undoIndex);

    // A pen or mouse over the preview must not draw on what is under it.
    sendMouse(preview, QEvent::MouseButtonPress, QPointF(240, 200),
              Qt::LeftButton);
    sendMouse(preview, QEvent::MouseMove, QPointF(300, 230), Qt::LeftButton);
    sendMouse(preview, QEvent::MouseButtonRelease, QPointF(300, 230),
              Qt::LeftButton);
    pump(500);
    check(QStringLiteral("(af) a click on the preview goes back to drawing: "
                         "playback stops, the preview is gone, the playhead "
                         "is back on the selected panel"),
          !r.animatic->isPlaying() && !r.animatic->previewVisible()
              && !preview->isVisible()
              && r.timeline->playheadPanelForTest() == 0);
    check(QStringLiteral("(af) ...and that press-drag-release drew NOTHING "
                         "on the canvas underneath"),
          !r.window.isDirty()
              && r.window.undoStackForTest()->index() == undoIndex);
    r.canvas->clearSelection();
    pump(100);
    stroke(r.canvas);
    pump(600);
    check(QStringLiteral("(af) control: the same gesture on the uncovered "
                         "canvas DOES draw (the check above can fail)"),
          r.window.isDirty()
              && r.window.undoStackForTest()->index() > undoIndex);

    // ---- scrubbing ----------------------------------------------------------
    r.window.markCleanForTest();
    const int calls2 = r.storyboard->selectPanelCallsForTest();
    {
        // Press in the ruler above the fifth clip and drag onto the fourth.
        const QPointF a(r.timeline->clipRectForTest(4).center().x(), 10);
        const QPointF b(r.timeline->clipRectForTest(3).center().x(), 10);
        sendMouse(r.surface, QEvent::MouseButtonPress, a, Qt::LeftButton);
        pump(100);
        const bool onFifth = r.timeline->playheadPanelForTest() == 4
            && r.animatic->previewVisible();
        sendMouse(r.surface, QEvent::MouseMove, b, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseButtonRelease, b, Qt::LeftButton);
        pump(200);
        check(QStringLiteral("(af) dragging the playhead PREVIEWS the panel "
                             "under it, panel by panel, and stays there on "
                             "release"),
              onFifth && r.animatic->previewVisible() && !r.animatic->isPlaying()
                  && r.timeline->playheadPanelForTest() == 3
                  && r.animatic->previewCaptionForTest().contains(
                         QStringLiteral("Scene 2")),
              r.animatic->previewCaptionForTest());
    }
    check(QStringLiteral("(af) ...without selecting it: the canvas, the "
                         "strip and the dirty flag are untouched"),
          r.storyboard->selectedFlatIndexForTest() == 0
              && r.storyboard->selectPanelCallsForTest() == calls2
              && !r.window.isDirty());
    clickThumb(r, 1);
    check(QStringLiteral("(af) selecting a panel leaves the preview and "
                         "puts the playhead on that panel"),
          !r.animatic->previewVisible()
              && r.timeline->playheadPanelForTest() == 1
              && r.storyboard->selectedFlatIndexForTest() == 1);

    // ---- the end of the film ------------------------------------------------
    r.animatic->togglePlay();
    pump(150);
    const bool playing = r.animatic->isPlaying();
    int advances = 0;
    while (r.animatic->isPlaying() && advances < 20) {
        r.animatic->advanceForTest(); // each panel's timer, without the wait
        ++advances;
        pump(30);
    }
    check(QStringLiteral("(af) playing off the end stops, goes back to "
                         "drawing, and parks the playhead on the selected "
                         "panel (control: it was playing)"),
          playing && advances == 5 && !r.animatic->isPlaying()
              && !r.animatic->previewVisible()
              && r.timeline->playheadPanelForTest() == 1,
          QStringLiteral("%1 advance(s); %2").arg(advances).arg(state(r)));

    // ---- leaving the workspace ----------------------------------------------
    // The old screen paused in its Back button. There is no Back now, and a
    // film left playing behind another screen would keep its audio running.
    r.animatic->togglePlay();
    pump(150);
    const bool playingBefore = r.animatic->isPlaying();
    auto *stack = qobject_cast<QStackedWidget *>(r.window.centralWidget());
    QPushButton *board = nullptr;
    for (QPushButton *b : r.storyboard->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Consistency Board"))
            board = b;
    if (board)
        board->click();
    pump(400);
    check(QStringLiteral("(af) going to another screen (the Consistency "
                         "Board) stops playback and drops the preview "
                         "(control: it was playing, and the screen did "
                         "change)"),
          playingBefore && board && stack
              && stack->currentWidget() != r.storyboard
              && !r.animatic->isPlaying() && !r.animatic->previewVisible());
    if (stack)
        stack->setCurrentWidget(r.storyboard);
    pump(500);

    // Undo changes the document, so it goes back to drawing first.
    r.animatic->scrubForTest(2);
    pump(150);
    const bool previewUp = r.animatic->previewVisible();
    const int indexBefore = r.window.undoStackForTest()->index();
    for (QAction *a : r.window.findChildren<QAction *>())
        if (a->text() == QStringLiteral("Undo"))
            a->trigger();
    pump(300);
    check(QStringLiteral("(af) Edit > Undo while previewing leaves the "
                         "preview before it undoes (control: the preview "
                         "was up, and something was undone)"),
          previewUp && !r.animatic->previewVisible()
              && r.window.undoStackForTest()->index() == indexBefore - 1);

    // The floating toolbar's Undo and Redo are the same door. They used to
    // call the canvas directly and skip this step - and the toolbars stay
    // visible over the preview, so the buttons are right there to click.
    auto *barUndo =
        r.window.findChild<QPushButton *>(QStringLiteral("toolbarUndo"));
    auto *barRedo =
        r.window.findChild<QPushButton *>(QStringLiteral("toolbarRedo"));
    check(QStringLiteral("(af) found the floating toolbar's Undo and Redo"),
          barUndo && barRedo);
    if (barUndo && barRedo) {
        const int at = r.window.undoStackForTest()->index();
        r.animatic->scrubForTest(2);
        pump(150);
        const bool upForRedo = r.animatic->previewVisible();
        barRedo->click();
        pump(300);
        const bool redoLeft = !r.animatic->previewVisible()
            && r.window.undoStackForTest()->index() == at + 1;
        r.animatic->scrubForTest(2);
        pump(150);
        const bool upForUndo = r.animatic->previewVisible();
        barUndo->click();
        pump(300);
        check(QStringLiteral("(af) the TOOLBAR's Redo and Undo leave the "
                             "preview before they act, like the Edit menu's "
                             "(control: the preview was up each time, and "
                             "each did redo / undo)"),
              upForRedo && redoLeft && upForUndo
                  && !r.animatic->previewVisible()
                  && r.window.undoStackForTest()->index() == at);
    }
}

void runWorkspaceKeysPass(const QString &project)
{
    using namespace workspace;
    out() << "--- (ag) Space belongs to the timeline only under the "
             "pointer ---" << Qt::endl;
    Rig r(project);
    check(QStringLiteral("(ag) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    QWidget *preview = r.animatic->previewSurface();
    r.canvas->setFocus();
    hover(r, r.animatic, false);
    hover(r, preview, false);

    // THE CONTROL FOR THE SEAM ITSELF: an Enter that does not come through
    // hover() - which is what the real mouse's would be - arms nothing.
    {
        const QPointF p(6, 6);
        QEnterEvent stray(p, p, r.animatic->mapToGlobal(p.toPoint()));
        QCoreApplication::sendEvent(r.animatic, &stray);
        pump(60);
        check(QStringLiteral("(ag) control: an Enter the test did not "
                             "deliver (the real mouse's would be one) arms "
                             "nothing - this section cannot depend on where "
                             "the pointer was left"),
              !r.storyboard->timelineKeysArmedForTest());
    }

    check(QStringLiteral("(ag) a click on the timeline does not take "
                         "keyboard focus from the canvas (the pan modifier "
                         "is a key event on the canvas)"),
          [&] {
              clickClip(r, 1);
              // The WINDOW's focus widget: the application-wide one is
              // null whenever this is not the active window, which is not
              // the test's to decide on a machine in use.
              return r.window.focusWidget() == r.canvas;
          }());
    check(QStringLiteral("(ag) pointer NOT over the timeline: Space is "
                         "claimed by no shortcut - it is the canvas's"),
          !r.storyboard->timelineKeysArmedForTest()
              && !keyClaimed(&r.window, Qt::Key_Space)
              && !r.animatic->isPlaying());
    hover(r, r.animatic, true);
    const bool armed = r.storyboard->timelineKeysArmedForTest();
    const bool claimed = keyClaimed(&r.window, Qt::Key_Space);
    check(QStringLiteral("(ag) pointer OVER the timeline: the same Space "
                         "plays (control: the same key, the same focus)"),
          armed && claimed && r.animatic->isPlaying()
              && r.window.focusWidget() == r.canvas);
    keyClaimed(&r.window, Qt::Key_Space);
    check(QStringLiteral("(ag) ...and Space again pauses, with the preview "
                         "still up"),
          !r.animatic->isPlaying() && r.animatic->previewVisible());
    keyClaimed(&r.window, Qt::Key_Escape);
    check(QStringLiteral("(ag) Escape over the timeline goes back to "
                         "drawing"),
          !r.animatic->previewVisible());

    // Arrows / Home / End navigate - they select, as a click would.
    int calls = r.storyboard->selectPanelCallsForTest();
    keyClaimed(&r.window, Qt::Key_Right);
    const bool right = r.storyboard->selectedFlatIndexForTest() == 2
        && r.storyboard->selectPanelCallsForTest() == calls + 1;
    keyClaimed(&r.window, Qt::Key_End);
    const bool end = r.storyboard->selectedFlatIndexForTest() == 5;
    keyClaimed(&r.window, Qt::Key_Home);
    const bool home = r.storyboard->selectedFlatIndexForTest() == 0;
    keyClaimed(&r.window, Qt::Key_Right);
    keyClaimed(&r.window, Qt::Key_Left);
    check(QStringLiteral("(ag) over the timeline, Right / End / Home / Left "
                         "select the next, last, first and previous panel"),
          right && end && home
              && r.storyboard->selectedFlatIndexForTest() == 0,
          QStringLiteral("right %1 end %2 home %3").arg(right).arg(end)
              .arg(home));

    // Typing wins over the pointer: a text field with focus keeps its keys.
    if (auto *notes = r.storyboard->findChild<QPlainTextEdit *>()) {
        // Shot Info is tabbed behind Scenes by default: bring it forward so
        // the field can really hold the focus.
        if (auto *shotInfo = r.storyboard->findChild<QDockWidget *>(
                QStringLiteral("dockShotInfo")))
            shotInfo->raise();
        pump(200);
        notes->setFocus();
        pump(150);
        const bool focused = r.window.focusWidget() == notes;
        const bool typed = !keyClaimed(notes, Qt::Key_Space);
        check(QStringLiteral("(ag) with the pointer over the timeline but a "
                             "text field focused, Space is the text "
                             "field's: nothing plays"),
              focused && typed && !r.animatic->isPlaying()
                  && r.storyboard->timelineKeysArmedForTest());
        r.canvas->setFocus();
        pump(100);
    }

    QShortcut *deleteKey = nullptr, *spaceKey = nullptr;
    for (QShortcut *s : r.storyboard->findChildren<QShortcut *>()) {
        if (s->key() == QKeySequence(Qt::Key_Delete))
            deleteKey = s;
        if (s->key() == QKeySequence(Qt::Key_Space))
            spaceKey = s;
    }
    const bool deleteOverTimeline = deleteKey && deleteKey->isEnabled();
    hover(r, r.animatic, false);
    check(QStringLiteral("(ag) the pointer leaves: every timeline key is "
                         "disarmed again (Delete was armed over it)"),
          deleteOverTimeline && deleteKey && !deleteKey->isEnabled()
              && spaceKey && !spaceKey->isEnabled()
              && !r.storyboard->timelineKeysArmedForTest()
              && !keyClaimed(&r.window, Qt::Key_Space)
              && !r.animatic->isPlaying());

    // Over the PREVIEW the transport keys work too - what is under the
    // pointer there is the film, not the canvas - but Delete does not.
    r.animatic->togglePlay();
    pump(150);
    hover(r, preview, false);
    const bool notYet = !r.storyboard->timelineKeysArmedForTest();
    hover(r, preview, true);
    const bool overPreview = r.storyboard->timelineKeysArmedForTest();
    const bool pausedThere = keyClaimed(&r.window, Qt::Key_Space)
        && !r.animatic->isPlaying();
    check(QStringLiteral("(ag) pointer over the PREVIEW: Space pauses the "
                         "film there too, and Delete stays disarmed"),
          notYet && overPreview && pausedThere && deleteKey
              && !deleteKey->isEnabled());
    keyClaimed(&r.window, Qt::Key_Escape);
    check(QStringLiteral("(ag) ...and once the preview is gone the keys go "
                         "back to the canvas without the pointer moving"),
          !r.animatic->previewVisible()
              && !r.storyboard->timelineKeysArmedForTest()
              && !keyClaimed(&r.window, Qt::Key_Space)
              && !r.animatic->isPlaying());
}

void runWorkspaceUndoPass(const QString &project)
{
    using namespace workspace;
    out() << "--- (ah) a timing change is undoable, in order with drawing "
             "---" << Qt::endl;
    Rig r(project);
    check(QStringLiteral("(ah) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    QUndoStack *undo = r.window.undoStackForTest();
    QAction *undoAction = nullptr, *redoAction = nullptr;
    for (QAction *a : r.window.findChildren<QAction *>()) {
        if (a->text() == QStringLiteral("Undo"))
            undoAction = a;
        if (a->text() == QStringLiteral("Redo"))
            redoAction = a;
    }
    stroke(r.canvas);
    pump(600);
    const int afterStroke = undo->index();
    check(QStringLiteral("(ah) control: a stroke is on the history, and the "
                         "Edit menu's Undo and Redo were found"),
          afterStroke >= 1 && undoAction && redoAction);
    if (!undoAction || !redoAction)
        return;

    // The real gesture: drag the selected clip's right edge two seconds out.
    r.window.markCleanForTest();
    auto dragEdge = [&](int flat, int toSeconds, bool andBack) {
        const QRect clip = r.timeline->clipRectForTest(flat);
        const int had = r.timeline->blockDurationForTest(flat);
        const double perSecond = double(clip.width()) / had;
        const QPointF edge(clip.left() + clip.width() - 2, clip.center().y());
        const QPointF out(clip.left() + perSecond * toSeconds, edge.y());
        sendMouse(r.surface, QEvent::MouseButtonPress, edge, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, (edge + out) / 2, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, out, Qt::LeftButton);
        if (andBack) {
            sendMouse(r.surface, QEvent::MouseMove, edge, Qt::LeftButton);
            sendMouse(r.surface, QEvent::MouseButtonRelease, edge, Qt::LeftButton);
        } else {
            sendMouse(r.surface, QEvent::MouseButtonRelease, out, Qt::LeftButton);
        }
        pump(250);
    };
    dragEdge(0, 4, false);
    check(QStringLiteral("(ah) dragging a clip's edge re-times its panel "
                         "(2 s -> 4 s) as ONE command, and marks the project "
                         "unsaved"),
          r.timeline->blockDurationForTest(0) == 4
              && undo->index() == afterStroke + 1
              && undo->undoText() == QStringLiteral("Change Panel Duration")
              && r.window.isDirty(),
          QStringLiteral("%1 s, history %2 -> %3, top \"%4\"")
              .arg(r.timeline->blockDurationForTest(0)).arg(afterStroke)
              .arg(undo->index()).arg(undo->undoText()));

    undoAction->trigger(); // Edit > Undo, the path Ctrl+Z takes
    pump(250);
    check(QStringLiteral("(ah) Ctrl+Z right after it takes back the TIMING "
                         "- the clip is 2 s again - and NOT the stroke "
                         "before it"),
          r.timeline->blockDurationForTest(0) == 2
              && undo->index() == afterStroke,
          QStringLiteral("%1 s, history at %2 (the stroke is %3)")
              .arg(r.timeline->blockDurationForTest(0)).arg(undo->index())
              .arg(afterStroke));
    redoAction->trigger();
    pump(250);
    const bool redone = r.timeline->blockDurationForTest(0) == 4;
    undoAction->trigger();
    undoAction->trigger();
    pump(300);
    check(QStringLiteral("(ah) redo re-times it; two undos then take back "
                         "the timing and the stroke, in that order"),
          redone && r.timeline->blockDurationForTest(0) == 2
              && undo->index() == afterStroke - 1);
    redoAction->trigger();
    pump(250);

    const int count = undo->count();
    dragEdge(0, 4, true);
    check(QStringLiteral("(ah) a drag that ends where it began changes "
                         "nothing and adds nothing to the history"),
          r.timeline->blockDurationForTest(0) == 2 && undo->count() == count);

    // Saved with the project, like any other edit.
    r.animatic->setPanelDurationForTest(1, 2, 9);
    pump(200);
    check(QStringLiteral("(ah) a re-timed panel in ANOTHER scene updates "
                         "its clip too (the timeline shows every scene)"),
          r.timeline->blockDurationForTest(5) == 9);
    r.animatic->togglePlay();
    pump(150);
    const bool playing = r.animatic->isPlaying();
    r.animatic->setPanelDurationForTest(0, 1, 6);
    pump(200);
    check(QStringLiteral("(ah) a timing change during playback stops "
                         "playback (control: it was playing)"),
          playing && !r.animatic->isPlaying()
              && r.timeline->blockDurationForTest(1) == 6);
    r.animatic->leavePreview();
}

void runWorkspaceLayoutPass(const QString &project)
{
    using namespace workspace;
    out() << "--- (ai) the timeline's height and collapse persist, and the "
             "toolbars come back ---" << Qt::endl;
    const QString keys = QStringLiteral("storyboard/timeline/v1/");
    {
        QSettings s = sankoSettings();
        s.remove(QStringLiteral("storyboard/timeline/v1")); // a first run
    }
    int openHeight = 0, draggedHeight = 0;
    {
        Rig r(project);
        check(QStringLiteral("(ai) the workspace opened"), r.ok);
        if (!r.ok)
            return;
        auto *splitter = r.storyboard->findChild<QSplitter *>(
            QStringLiteral("workspaceSplitter"));
        auto *fold = r.animatic->findChild<QPushButton *>(
            QStringLiteral("animaticCollapse"));
        auto *play = r.animatic->findChild<QPushButton *>(
            QStringLiteral("animaticPlay"));
        check(QStringLiteral("(ai) found the splitter, the fold button and "
                             "the play button"),
              splitter && fold && play);
        if (!splitter || !fold || !play)
            return;
        openHeight = r.animatic->height();
        check(QStringLiteral("(ai) on a FIRST run the timeline is open, at "
                             "its default height"),
              !r.animatic->isCollapsed() && r.timeline->isVisible()
                  && openHeight == r.animatic->defaultExpandedHeight(),
              QStringLiteral("%1 px (default %2)").arg(openHeight)
                  .arg(r.animatic->defaultExpandedHeight()));
        const int canvasOpen = r.canvas->height();
        const QStringList barsOpen = barPlaces(r);
        QString why;
        check(QStringLiteral("(ai) with it open every floating toolbar is "
                             "inside the canvas"),
              barsInsideCanvas(r, &why), why);

        fold->click();
        pump(500);
        const int canvasFolded = r.canvas->height();
        const QStringList barsFolded = barPlaces(r);
        check(QStringLiteral("(ai) collapsing leaves ONE row - the header, "
                             "transport and all - and gives the rest to the "
                             "canvas"),
              r.animatic->isCollapsed()
                  && r.animatic->height() == AnimaticPage::kHeaderHeight
                  && !r.timeline->isVisible() && play->isVisible()
                  && canvasFolded
                         == canvasOpen + openHeight - AnimaticPage::kHeaderHeight,
              QStringLiteral("section %1 px, canvas %2 -> %3")
                  .arg(r.animatic->height()).arg(canvasOpen).arg(canvasFolded));
        play->click();
        pump(200);
        const bool playsFolded = r.animatic->isPlaying()
            && r.animatic->previewVisible();
        play->click();
        r.animatic->leavePreview();
        pump(150);
        check(QStringLiteral("(ai) ...and Play still works collapsed"),
              playsFolded);
        fold->click();
        pump(500);
        check(QStringLiteral("(ai) expanding gives it back the height it "
                             "had"),
              !r.animatic->isCollapsed() && r.timeline->isVisible()
                  && r.animatic->height() == openHeight
                  && r.canvas->height() == canvasOpen,
              QStringLiteral("%1 px").arg(r.animatic->height()));

        // The toolbars: the same places every time, in both states.
        int moved = 0;
        for (int i = 0; i < 8; ++i) {
            fold->click();
            pump(350);
            if (barPlaces(r) != barsFolded)
                ++moved;
            fold->click();
            pump(350);
            if (barPlaces(r) != barsOpen)
                ++moved;
        }
        check(QStringLiteral("(ai) eight collapse/expand cycles: the "
                             "floating toolbars return to exactly the same "
                             "places every time (control: they do move "
                             "between the two states)"),
              moved == 0 && barsOpen != barsFolded && !barsOpen.isEmpty(),
              QStringLiteral("%1 mismatch(es); %2 bars").arg(moved)
                  .arg(barsOpen.size()));

        // Drag the handle up: the timeline grows, the shots track takes it.
        const int shotsBefore = r.timeline->shotsTrackHeightForTest();
        QSplitterHandle *handle = splitter->handle(1);
        const QPointF grip = handle->rect().center();
        // ONE move: the handle follows the pointer, so a second move given
        // in the handle's own coordinates would be measured from where the
        // first one left it.
        sendMouse(handle, QEvent::MouseButtonPress, grip, Qt::LeftButton);
        sendMouse(handle, QEvent::MouseMove, grip + QPointF(0, -60),
                  Qt::LeftButton);
        sendMouse(handle, QEvent::MouseButtonRelease, grip, Qt::LeftButton);
        pump(400);
        draggedHeight = r.animatic->height();
        check(QStringLiteral("(ai) dragging the handle up 60 px makes the "
                             "timeline 60 px taller, and the SHOTS track "
                             "takes all of it"),
              draggedHeight == openHeight + 60
                  && r.timeline->shotsTrackHeightForTest() == shotsBefore + 60
                  && r.canvas->height() == canvasOpen - 60,
              QStringLiteral("section %1 -> %2, shots track %3 -> %4")
                  .arg(openHeight).arg(draggedHeight).arg(shotsBefore)
                  .arg(r.timeline->shotsTrackHeightForTest()));
        check(QStringLiteral("(ai) ...and the toolbars are still inside the "
                             "shorter canvas"),
              barsInsideCanvas(r, &why), why);
    }
    {
        Rig r(project);
        if (!r.ok)
            return;
        check(QStringLiteral("(ai) after a relaunch the timeline is open at "
                             "the height it was dragged to"),
              !r.animatic->isCollapsed() && r.animatic->height() == draggedHeight,
              QStringLiteral("%1 px (dragged to %2)").arg(r.animatic->height())
                  .arg(draggedHeight));
        r.animatic->setCollapsed(true);
        pump(300);
    }
    {
        Rig r(project);
        if (!r.ok)
            return;
        check(QStringLiteral("(ai) left collapsed, it comes back collapsed"),
              r.animatic->isCollapsed()
                  && r.animatic->height() == AnimaticPage::kHeaderHeight
                  && !r.timeline->isVisible(),
              QStringLiteral("%1 px").arg(r.animatic->height()));
        r.animatic->setCollapsed(false);
        pump(500);
        check(QStringLiteral("(ai) ...and expands to the remembered height, "
                             "not the default"),
              r.animatic->height() == draggedHeight
                  && draggedHeight != openHeight,
              QStringLiteral("%1 px").arg(r.animatic->height()));

        r.animatic->setCollapsed(true);
        pump(300);
        QAction *reset = nullptr;
        for (QAction *a : r.window.findChildren<QAction *>())
            if (a->text() == QStringLiteral("Reset Layout"))
                reset = a;
        if (reset)
            reset->trigger();
        pump(600);
        check(QStringLiteral("(ai) Reset Layout opens it at the default "
                             "height and forgets the saved one"),
              reset && !r.animatic->isCollapsed()
                  && r.animatic->height() == openHeight
                  && !sankoSettings().contains(keys + QStringLiteral("height")),
              QStringLiteral("%1 px (default %2)").arg(r.animatic->height())
                  .arg(openHeight));
    }
}

// ===================== AUDIO: MENUS, UNDO, THE PLAIN BAR ===================
namespace audio {

// A real, playable WAV: 16-bit mono PCM silence of the given length.
bool writeWav(const QString &path, double seconds)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const quint32 rate = 11025;
    const QByteArray pcm(int(rate * seconds) * 2, '\0');
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("RIFF", 4);
    s << quint32(36 + pcm.size());
    s.writeRawData("WAVEfmt ", 8);
    s << quint32(16) << quint16(1) << quint16(1) << rate << quint32(rate * 2)
      << quint16(2) << quint16(16);
    s.writeRawData("data", 4);
    s << quint32(pcm.size());
    out.append(pcm);
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

// The media backend reads a file's length a moment after it is set.
bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (condition())
            return true;
        pump(50);
    }
    return condition();
}

QAction *editAction(MainWindow &window, const QString &text, int *index = nullptr)
{
    for (QAction *top : window.menuBar()->actions()) {
        QString title = top->text();
        title.remove(QLatin1Char('&'));
        if (!top->menu() || title != QStringLiteral("Edit"))
            continue;
        const QList<QAction *> actions = top->menu()->actions();
        for (int i = 0; i < actions.size(); ++i)
            if (actions.at(i)->text() == text) {
                if (index)
                    *index = i;
                return actions.at(i);
            }
    }
    return nullptr;
}

void rightClickAudioRow(workspace::Rig &r, int x = -1)
{
    const QRect row = r.timeline->audioRowForTest();
    const QPoint at(x >= 0 ? x : row.center().x(), row.center().y());
    QContextMenuEvent ev(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
    QCoreApplication::sendEvent(r.surface, &ev);
    pump(250);
}

} // namespace audio

void runAudioEntryPointsPass(const QString &project, const QString &scratch)
{
    using namespace workspace;
    using namespace audio;
    out() << "--- (aj) audio: the Edit menu, the track's own menu, and the "
             "plain bar ---" << Qt::endl;
    const QString wavA = scratch + QStringLiteral("/audio/first take.wav");
    const QString wavB = scratch + QStringLiteral("/audio/second.wav");
    check(QStringLiteral("(aj) fixture: two real WAV files written"),
          writeWav(wavA, 2.0) && writeWav(wavB, 3.0));

    // ---- no project: both entries are there, and both are off ---------------
    {
        MainWindow window;
        window.resize(1300, 850);
        window.show();
        pump(700);
        int importAt = -1, removeAt = -1;
        QAction *importAct =
            editAction(window, QStringLiteral("Import Audio..."), &importAt);
        QAction *removeAct =
            editAction(window, QStringLiteral("Remove Audio"), &removeAt);
        check(QStringLiteral("(aj) Edit has Import Audio... and Remove "
                             "Audio, side by side"),
              importAct && removeAct && removeAt == importAt + 1,
              QStringLiteral("positions %1 and %2").arg(importAt).arg(removeAt));
        check(QStringLiteral("(aj) with no project open both are disabled"),
              importAct && removeAct && !importAct->isEnabled()
                  && !removeAct->isEnabled());
        window.close();
        pump(300);
    }

    Rig r(project);
    check(QStringLiteral("(aj) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    QAction *importAct = editAction(r.window, QStringLiteral("Import Audio..."));
    QAction *removeAct = editAction(r.window, QStringLiteral("Remove Audio"));
    QUndoStack *undo = r.window.undoStackForTest();
    if (!importAct || !removeAct)
        return;
    check(QStringLiteral("(aj) with a project open Import is enabled, and "
                         "Remove is not until there is a track"),
          importAct->isEnabled() && !removeAct->isEnabled()
              && r.animatic->audioPath().isEmpty());

    bool audioButton = false, exportButton = false;
    for (QPushButton *b : r.window.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Import Audio")
            || b->text() == QStringLiteral("Remove Audio"))
            audioButton = true;
        if (b->text() == QStringLiteral("Export MP4"))
            exportButton = true;
    }
    check(QStringLiteral("(aj) the bottom bar no longer has Import Audio or "
                         "Remove Audio buttons (control: Export MP4, not yet "
                         "moved, is still found by the same search)"),
          !audioButton && exportButton);
    check(QStringLiteral("(aj) an empty audio track says how to fill it - "
                         "and no longer claims to accept a drop"),
          r.timeline->audioBarStateForTest() == QStringLiteral("none")
              && r.timeline->audioBarTextForTest()
                     == QStringLiteral("Right-click to import audio"),
          r.timeline->audioBarTextForTest());

    // ---- a cancelled dialog ---------------------------------------------------
    r.window.markCleanForTest();
    const int countBefore = undo->count();
    r.animatic->setAudioPickerForTest([] { return QString(); });
    importAct->trigger();
    pump(200);
    check(QStringLiteral("(aj) a cancelled Import changes nothing: no track, "
                         "not dirty, nothing added to the history"),
          r.animatic->audioPath().isEmpty() && !r.window.isDirty()
              && undo->count() == countBefore);

    // ---- Edit > Import Audio --------------------------------------------------
    r.animatic->setAudioPickerForTest([wavA] { return wavA; });
    importAct->trigger();
    pump(200);
    check(QStringLiteral("(aj) Edit > Import Audio sets the track, as ONE "
                         "command, and marks a clean project unsaved"),
          r.animatic->audioPath() == wavA && r.window.isDirty()
              && undo->count() == countBefore + 1
              && undo->undoText() == QStringLiteral("Import Audio"),
          QStringLiteral("track \"%1\", top \"%2\"")
              .arg(QFileInfo(r.animatic->audioPath()).fileName(),
                   undo->undoText()));
    const bool present = waitFor([&] {
        return r.timeline->audioBarStateForTest() == QStringLiteral("present");
    });
    check(QStringLiteral("(aj) the audio track shows a plain bar carrying "
                         "the file's name and its real length"),
          present && r.timeline->audioBarTextForTest()
                         == QStringLiteral("first take.wav | 0:02"),
          QStringLiteral("%1: %2").arg(r.timeline->audioBarStateForTest(),
                                       r.timeline->audioBarTextForTest()));
    check(QStringLiteral("(aj) ...and Remove Audio is now enabled"),
          removeAct->isEnabled());

    // ---- the audio track's own menu -------------------------------------------
    QStringList menuSaw;
    QString trigger;
    r.animatic->setAudioMenuHookForTest([&](QMenu *menu) {
        menuSaw.clear();
        for (QAction *a : menu->actions())
            if (!a->isSeparator())
                menuSaw << a->text()
                        + (a->isEnabled() ? QString()
                                          : QStringLiteral(" (disabled)"));
        for (QAction *a : menu->actions())
            if (a->text() == trigger && a->isEnabled())
                a->trigger();
    });
    const int calls = r.storyboard->selectPanelCallsForTest();
    const int selected = r.storyboard->selectedFlatIndexForTest();
    r.window.markCleanForTest();
    trigger = QStringLiteral("Remove Audio");
    rightClickAudioRow(r);
    check(QStringLiteral("(aj) right-clicking the audio track offers the "
                         "same two entries"),
          menuSaw == QStringList({QStringLiteral("Import Audio..."),
                                  QStringLiteral("Remove Audio")}),
          menuSaw.join(QStringLiteral(" | ")));
    check(QStringLiteral("(aj) Remove Audio from that menu removes the "
                         "track, as a command, and marks the project "
                         "unsaved"),
          r.animatic->audioPath().isEmpty() && r.window.isDirty()
              && undo->undoText() == QStringLiteral("Remove Audio")
              && r.timeline->audioBarStateForTest() == QStringLiteral("none")
              && !removeAct->isEnabled());
    check(QStringLiteral("(aj) ...and the right-click selected no panel"),
          r.storyboard->selectPanelCallsForTest() == calls
              && r.storyboard->selectedFlatIndexForTest() == selected);
    trigger = QStringLiteral("Import Audio...");
    r.animatic->setAudioPickerForTest([wavB] { return wavB; });
    rightClickAudioRow(r, 20); // on the row's LABEL: still the audio track
    check(QStringLiteral("(aj) with no track the menu shows Remove Audio "
                         "disabled; Import from it sets the track"),
          menuSaw == QStringList({QStringLiteral("Import Audio..."),
                                  QStringLiteral("Remove Audio (disabled)")})
              && r.animatic->audioPath() == wavB,
          menuSaw.join(QStringLiteral(" | ")));
    r.animatic->setAudioMenuHookForTest({});

    // ---- Edit > Remove Audio ---------------------------------------------------
    removeAct->trigger();
    pump(200);
    check(QStringLiteral("(aj) Edit > Remove Audio removes it"),
          r.animatic->audioPath().isEmpty());

    // ---- only in the workspace --------------------------------------------------
    auto *stack = qobject_cast<QStackedWidget *>(r.window.centralWidget());
    QWidget *elsewhere = nullptr;
    for (int i = 0; stack && i < stack->count(); ++i)
        if (qobject_cast<ConsistencyBoard *>(stack->widget(i)))
            elsewhere = stack->widget(i);
    if (stack && elsewhere) {
        stack->setCurrentWidget(elsewhere);
        pump(300);
        const bool offElsewhere = !importAct->isEnabled();
        stack->setCurrentWidget(r.storyboard);
        pump(400);
        check(QStringLiteral("(aj) on another screen Import Audio is "
                             "disabled, and enabled again back in the "
                             "workspace"),
              offElsewhere && importAct->isEnabled());
    }

    // ---- opening the dialog pauses the film -------------------------------------
    r.animatic->togglePlay();
    pump(150);
    const bool playing = r.animatic->isPlaying();
    r.animatic->setAudioPickerForTest([] { return QString(); });
    importAct->trigger();
    pump(200);
    check(QStringLiteral("(aj) opening the Import dialog pauses playback "
                         "(control: it was playing)"),
          playing && !r.animatic->isPlaying());
    r.animatic->leavePreview();
    r.animatic->setAudioPickerForTest({});
}

void runAudioUndoPass(const QString &project, const QString &scratch)
{
    using namespace workspace;
    using namespace audio;
    out() << "--- (ak) audio is undoable, in order with drawing ---" << Qt::endl;
    const QString wavA = scratch + QStringLiteral("/audio/first take.wav");
    const QString wavB = scratch + QStringLiteral("/audio/second.wav");
    const QString saved = scratch
        + QStringLiteral("/projects/WithAudio/WithAudio.sankotv");
    {
        Rig r(project);
        check(QStringLiteral("(ak) the workspace opened"), r.ok);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        QAction *importAct =
            editAction(r.window, QStringLiteral("Import Audio..."));
        QAction *removeAct = editAction(r.window, QStringLiteral("Remove Audio"));
        QAction *undoAct = editAction(r.window, QStringLiteral("Undo"));
        QAction *redoAct = editAction(r.window, QStringLiteral("Redo"));
        if (!importAct || !removeAct || !undoAct || !redoAct) {
            check(QStringLiteral("(ak) found the Edit actions"), false);
            return;
        }
        stroke(r.canvas);
        pump(600);
        const int afterStroke = undo->index();
        r.animatic->setAudioPickerForTest([wavA] { return wavA; });
        importAct->trigger();
        pump(200);
        const bool imported = r.animatic->audioPath() == wavA
            && undo->index() == afterStroke + 1;
        undoAct->trigger(); // Edit > Undo, the path Ctrl+Z takes
        pump(250);
        check(QStringLiteral("(ak) Ctrl+Z right after importing audio takes "
                             "back the IMPORT - no track - and not the stroke "
                             "before it (control: the import was on the "
                             "history, above the stroke)"),
              imported && afterStroke >= 1 && r.animatic->audioPath().isEmpty()
                  && undo->index() == afterStroke,
              QStringLiteral("history at %1 (the stroke is %2)")
                  .arg(undo->index()).arg(afterStroke));
        redoAct->trigger();
        pump(250);
        check(QStringLiteral("(ak) redo brings the track back"),
              r.animatic->audioPath() == wavA);

        r.animatic->setAudioPickerForTest([wavB] { return wavB; });
        importAct->trigger();
        pump(200);
        const bool replaced = r.animatic->audioPath() == wavB;
        undoAct->trigger();
        pump(250);
        check(QStringLiteral("(ak) undoing an import that REPLACED a track "
                             "restores the earlier track"),
              replaced && r.animatic->audioPath() == wavA);
        removeAct->trigger();
        pump(200);
        const bool removed = r.animatic->audioPath().isEmpty();
        undoAct->trigger();
        pump(250);
        check(QStringLiteral("(ak) undoing Remove Audio restores the track"),
              removed && r.animatic->audioPath() == wavA);

        // Undoing the import WHILE THE FILM PLAYS. The stack is undone
        // directly, which is what the command must survive on its own -
        // Edit > Undo and the toolbar's button both stop playback before
        // they get here.
        while (undo->index() > afterStroke + 1)
            undo->undo(); // back to: stroke, import A
        waitFor([&] {
            return r.timeline->audioBarStateForTest()
                == QStringLiteral("present");
        });
        r.animatic->togglePlay();
        pump(200);
        const bool playing = r.animatic->isPlaying()
            && r.animatic->audioPath() == wavA
            && undo->index() == afterStroke + 1;
        undo->undo();
        pump(250);
        check(QStringLiteral("(ak) undoing the import while playing stops "
                             "playback and removes the track; the stroke "
                             "stays (control: it was playing, with the "
                             "track)"),
              playing && !r.animatic->isPlaying()
                  && r.animatic->audioPath().isEmpty()
                  && r.timeline->audioBarStateForTest() == QStringLiteral("none")
                  && undo->index() == afterStroke);
        r.animatic->togglePlay();
        pump(150);
        r.animatic->advanceForTest();
        pump(100);
        check(QStringLiteral("(ak) ...and Play carries on afterwards, "
                             "without a track"),
              r.animatic->isPlaying());
        r.animatic->leavePreview();

        // Save it WITH a track, for the load check below.
        undo->redo();
        pump(200);
        QDir().mkpath(QFileInfo(saved).absolutePath());
        check(QStringLiteral("(ak) the project saves with its track"),
              r.animatic->audioPath() == wavA
                  && r.window.saveProjectForTest(saved) && !r.window.isDirty());
        r.animatic->setAudioPickerForTest({});
    }
    {
        Rig r(saved);
        if (!r.ok) {
            check(QStringLiteral("(ak) the saved project re-opens"), false);
            return;
        }
        QUndoStack *undo = r.window.undoStackForTest();
        check(QStringLiteral("(ak) a project LOADED with a track has the "
                             "track, is clean, and has nothing on its "
                             "history: a load is not an edit"),
              r.animatic->audioPath() == wavA && !r.window.isDirty()
                  && undo->count() == 0,
              QStringLiteral("%1 command(s)").arg(undo->count()));
        QAction *removeAct = editAction(r.window, QStringLiteral("Remove Audio"));
        check(QStringLiteral("(ak) ...and Remove Audio is enabled for it "
                             "straight away"),
              removeAct && removeAct->isEnabled());
        if (removeAct)
            removeAct->trigger();
        pump(200);
        check(QStringLiteral("(ak) control: removing it IS an edit - one "
                             "command, dirty"),
              undo->count() == 1 && r.window.isDirty());
    }
}

// ---- (al) a project whose audio file is missing ----------------------------
// The defect: the animatic adopted a project's audio path only if the file
// existed, and save writes whatever path the animatic holds. A project
// opened while its audio was unavailable opened clean, said nothing, and
// the next save erased the reference. Now the path is kept, the track is
// shown as missing, and it can be pointed at its file again.
void runAudioMissingPass(const QString &scratch)
{
    using namespace workspace;
    using namespace audio;
    out() << "--- (al) a missing audio file keeps its path, shows as missing, "
             "and can be located ---" << Qt::endl;
    const QString wavA = scratch + QStringLiteral("/audio/first take.wav");
    const QString wavB = scratch + QStringLiteral("/audio/second.wav");
    const QString parked = wavA + QStringLiteral(".moved");
    const QString saved = scratch
        + QStringLiteral("/projects/WithAudio/WithAudio.sankotv");
    auto manifestAudio = [&saved] {
        QFile f(saved);
        if (!f.open(QIODevice::ReadOnly))
            return QStringLiteral("<unreadable>");
        return QJsonDocument::fromJson(f.readAll())
            .object()
            .value(QStringLiteral("audioPath"))
            .toString();
    };

    // CONTROL FIRST, with the file present: the same readings say "present".
    {
        Rig r(saved);
        if (!r.ok) {
            check(QStringLiteral("(al) the project with a track opens"), false);
            return;
        }
        QAction *locate =
            editAction(r.window, QStringLiteral("Locate Audio File..."));
        const bool present = waitFor([&] {
            return r.timeline->audioBarStateForTest() == QStringLiteral("present");
        });
        check(QStringLiteral("(al) control: with the file where the project "
                             "says, the track is present and Locate Audio "
                             "File is disabled"),
              present && !r.animatic->audioMissing() && locate
                  && !locate->isEnabled() && manifestAudio() == wavA,
              r.timeline->audioBarTextForTest());
    }

    check(QStringLiteral("(al) fixture: the audio file is moved away"),
          QFile::rename(wavA, parked) && !QFileInfo::exists(wavA));
    {
        Rig r(saved);
        if (!r.ok) {
            check(QStringLiteral("(al) the project opens without its audio "
                                 "file"), false);
            return;
        }
        QUndoStack *undo = r.window.undoStackForTest();
        QAction *locate =
            editAction(r.window, QStringLiteral("Locate Audio File..."));
        QAction *removeAct = editAction(r.window, QStringLiteral("Remove Audio"));
        QAction *undoAct = editAction(r.window, QStringLiteral("Undo"));
        check(QStringLiteral("(al) it opens CLEAN and still holds the path "
                             "(it used to drop it here)"),
              !r.window.isDirty() && r.animatic->audioPath() == wavA
                  && r.animatic->audioMissing() && undo->count() == 0,
              QFileInfo(r.animatic->audioPath()).fileName());
        check(QStringLiteral("(al) the audio track says MISSING, with the "
                             "file's name and what to do"),
              r.timeline->audioBarStateForTest() == QStringLiteral("missing")
                  && r.timeline->audioBarTextForTest().contains(
                         QStringLiteral("MISSING"))
                  && r.timeline->audioBarTextForTest().contains(
                         QStringLiteral("first take.wav"))
                  && r.timeline->audioBarTextForTest().contains(
                         QStringLiteral("right-click to locate")),
              r.timeline->audioBarTextForTest());
        bool headerSays = false;
        for (QLabel *label : r.animatic->findChildren<QLabel *>())
            if (label->isVisible()
                && label->text().contains(QStringLiteral("first take.wav"))
                && label->text().contains(QStringLiteral("missing")))
                headerSays = true;
        r.animatic->setCollapsed(true);
        pump(400);
        bool headerSaysCollapsed = false;
        for (QLabel *label : r.animatic->findChildren<QLabel *>())
            if (label->isVisible()
                && label->text().contains(QStringLiteral("missing")))
                headerSaysCollapsed = true;
        r.animatic->setCollapsed(false);
        pump(400);
        check(QStringLiteral("(al) the header row says so too - and still "
                             "does with the timeline collapsed"),
              headerSays && headerSaysCollapsed);
        check(QStringLiteral("(al) Locate Audio File and Remove Audio are "
                             "both enabled for it"),
              locate && locate->isEnabled() && removeAct
                  && removeAct->isEnabled());

        QStringList menuSaw;
        r.animatic->setAudioMenuHookForTest([&](QMenu *menu) {
            menuSaw.clear();
            for (QAction *a : menu->actions())
                if (!a->isSeparator())
                    menuSaw << a->text();
        });
        rightClickAudioRow(r);
        r.animatic->setAudioMenuHookForTest({});
        check(QStringLiteral("(al) the track's right-click menu leads with "
                             "Locate Audio File..."),
              menuSaw == QStringList({QStringLiteral("Locate Audio File..."),
                                      QStringLiteral("Import Audio..."),
                                      QStringLiteral("Remove Audio")}),
              menuSaw.join(QStringLiteral(" | ")));

        // THE DEFECT ITSELF: an unrelated edit, a save - the path survives.
        r.animatic->setPanelDurationForTest(0, 0, 6);
        pump(200);
        check(QStringLiteral("(al) after an unrelated edit and a SAVE the "
                             "project file still names the audio (it used to "
                             "be written back empty)"),
              r.window.saveProjectForTest(saved) && manifestAudio() == wavA,
              QFileInfo(manifestAudio()).fileName());

        r.animatic->togglePlay();
        pump(150);
        r.animatic->advanceForTest();
        pump(100);
        check(QStringLiteral("(al) the film plays, silently, with the track "
                             "missing"),
              r.animatic->isPlaying());
        r.animatic->leavePreview();

        // ---- locate: a DIFFERENT file -----------------------------------------
        r.window.markCleanForTest();
        const int count = undo->count();
        r.animatic->setAudioPickerForTest([wavB] { return wavB; });
        if (locate)
            locate->trigger();
        pump(250);
        const bool relinked = waitFor([&] {
            return r.timeline->audioBarStateForTest() == QStringLiteral("present");
        });
        check(QStringLiteral("(al) Locate to another file re-points the "
                             "track: playable, ONE command, project "
                             "unsaved"),
              relinked && r.animatic->audioPath() == wavB
                  && !r.animatic->audioMissing() && r.window.isDirty()
                  && undo->count() == count + 1
                  && undo->undoText() == QStringLiteral("Locate Audio File")
                  && locate && !locate->isEnabled(),
              r.timeline->audioBarTextForTest());
        if (undoAct)
            undoAct->trigger();
        pump(250);
        check(QStringLiteral("(al) ...and undo returns to the missing track "
                             "at the old path"),
              r.animatic->audioPath() == wavA && r.animatic->audioMissing()
                  && r.timeline->audioBarStateForTest()
                         == QStringLiteral("missing"));

        // ---- locate: the SAME path, the file put back --------------------------
        const bool restored = QFile::rename(parked, wavA);
        r.window.markCleanForTest();
        const int countBefore = undo->count();
        r.animatic->setAudioPickerForTest([wavA] { return wavA; });
        if (locate)
            locate->trigger();
        pump(250);
        const bool back = waitFor([&] {
            return r.timeline->audioBarStateForTest() == QStringLiteral("present");
        });
        check(QStringLiteral("(al) with the file put back, Locate to the SAME "
                             "path just loads it: playable, NOT dirty, "
                             "nothing added to the history"),
              restored && back && !r.animatic->audioMissing()
                  && r.animatic->audioPath() == wavA && !r.window.isDirty()
                  && undo->count() == countBefore,
              r.timeline->audioBarTextForTest());
        r.animatic->setAudioPickerForTest({});
    }
}

// ---- (am) generation data survives open and save ---------------------------
// The Generation page is gone, and with it every screen that showed a take.
// The data is still in the model and still read and written by ProjectIO -
// request ids, statuses, prompts, timestamps, costs, the selected take - and
// THIS SECTION IS NOW THE ONLY THING THAT WILL NOTICE IF IT STOPS SURVIVING.
// Three kinds of panel, as old projects can hold them: one with three takes
// (video present / video missing / already failed, no path), one from
// before takes existed (a lone video path), one that was mid-generation.
namespace takes {

const char *const kFields[] = {"generationStatus", "generatedVideoPath",
                               "falRequestId", "takes", "selectedTakeId"};

QJsonArray panelsOf(const QString &manifest)
{
    QJsonArray all;
    QFile f(manifest);
    if (!f.open(QIODevice::ReadOnly))
        return all;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &s : root.value(QStringLiteral("scenes")).toArray())
        for (const QJsonValue &p :
             s.toObject().value(QStringLiteral("panels")).toArray())
            all.append(p);
    return all;
}

QJsonObject generationOf(const QJsonValue &panel)
{
    QJsonObject out;
    for (const char *key : kFields)
        out[QLatin1String(key)] = panel.toObject().value(QLatin1String(key));
    return out;
}

QString text(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Every generation field of every panel, file against file.
bool same(const QString &a, const QString &b, QString *why)
{
    const QJsonArray pa = panelsOf(a), pb = panelsOf(b);
    if (pa.isEmpty() || pa.size() != pb.size()) {
        *why = QStringLiteral("%1 vs %2 panels").arg(pa.size()).arg(pb.size());
        return false;
    }
    for (int i = 0; i < pa.size(); ++i)
        if (generationOf(pa.at(i)) != generationOf(pb.at(i))) {
            *why = QStringLiteral("panel %1: %2  ->  %3")
                       .arg(i)
                       .arg(text(generationOf(pa.at(i))),
                            text(generationOf(pb.at(i))));
            return false;
        }
    return true;
}

void touch(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write("not really a video");
}

} // namespace takes

void runTakesRoundTripPass(const QString &scratch)
{
    using namespace takes;
    out() << "--- (am) generation data survives open and save, with no "
             "screen left to show it ---" << Qt::endl;
    const QString dirA = scratch + QStringLiteral("/projects/Takes");
    const QString dirB = scratch + QStringLiteral("/projects/TakesCopy");
    QDir().mkpath(dirA);
    QDir().mkpath(dirB);
    const QString path = dirA + QStringLiteral("/Takes.sankotv");
    const QString original = dirA + QStringLiteral("/original.json");
    const QString firstSave = dirA + QStringLiteral("/first_save.json");
    touch(dirA + QStringLiteral("/take_one.mp4"));
    touch(dirA + QStringLiteral("/old_clip.mp4"));
    {
        Scene *scene = new Scene;
        scene->number = 1;
        scene->location = QStringLiteral("INT. TAKES");
        for (int i = 0; i < 3; ++i)
            scene->panels.append(makeBlankPanel(QSize(960, 540)));
        Panel *p0 = scene->panels.at(0);
        GeneratedTake present;
        present.id = QStringLiteral("take-1");
        present.videoPath = QStringLiteral("take_one.mp4");
        present.promptUsed = QStringLiteral("A slow push in on the door.");
        present.timestamp = QStringLiteral("2026-07-01 10:00:00");
        present.status = QStringLiteral("Complete");
        present.costEstimate = 0.05;
        GeneratedTake gone = present; // finished - its video is not here
        gone.id = QStringLiteral("take-2");
        gone.videoPath = QStringLiteral("take_two_MISSING.mp4");
        gone.promptUsed = QStringLiteral("Same, faster.");
        gone.costEstimate = 0.07;
        GeneratedTake failed = present; // failed at the time: no file at all
        failed.id = QStringLiteral("take-3");
        failed.videoPath.clear();
        failed.status = QStringLiteral("Failed");
        p0->takes = {present, gone, failed};
        p0->selectedTakeId = present.id;
        p0->generatedVideoPath = present.videoPath;
        p0->falRequestId = QStringLiteral("req-abc-123");
        p0->generationStatus = QStringLiteral("Complete");
        Panel *p1 = scene->panels.at(1); // from before takes existed
        p1->generatedVideoPath = QStringLiteral("old_clip.mp4");
        p1->falRequestId = QStringLiteral("req-old");
        p1->generationStatus = QStringLiteral("Complete");
        Panel *p2 = scene->panels.at(2); // was mid-generation when saved
        p2->falRequestId = QStringLiteral("req-live");
        p2->generationStatus = QStringLiteral("Generating");

        ProjectIO::SaveData data;
        data.projectName = QStringLiteral("Takes");
        data.fps = 24;
        data.canvasSize = QSize(960, 540);
        data.scenes = {scene};
        const ProjectIO::WriteResult written = ProjectIO::projectToJson(data, path);
        QFile f(path);
        const bool wrote = written.ok && f.open(QIODevice::WriteOnly)
            && f.write(QJsonDocument(written.root).toJson()) > 0;
        f.close();
        delete scene;
        check(QStringLiteral("(am) fixture: a project with three takes, a "
                             "pre-takes panel and a mid-generation panel"),
              wrote && QFile::copy(path, original)
                  && panelsOf(original).size() == 3);
    }

    // THE COMPARATOR'S OWN CONTROL: one changed status in one take, and it
    // says "different" - otherwise every PASS below could be a comparator
    // that sees nothing.
    {
        const QString tampered = dirA + QStringLiteral("/tampered.json");
        QFile in(original);
        in.open(QIODevice::ReadOnly);
        QByteArray bytes = in.readAll();
        // take-2's own "status" line (keys are written in alphabetical
        // order, so it follows that take's "id").
        const int line = bytes.indexOf("\"status\"", bytes.indexOf("take-2"));
        bytes.replace(line, bytes.indexOf('\n', line) - line,
                      "\"status\": \"Failed\",");
        QFile outFile(tampered);
        outFile.open(QIODevice::WriteOnly);
        outFile.write(bytes);
        outFile.close();
        QString why;
        check(QStringLiteral("(am) control: the comparator SEES one take's "
                             "status changing"),
              !same(original, tampered, &why) && why.contains(QStringLiteral("panel 0")),
              why.left(120));
    }

    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(700);
    QString why;
    check(QStringLiteral("(am) the project opens, clean"),
          window.loadProjectForTest(path) && !window.isDirty());
    pump(400);
    check(QStringLiteral("(am) ...and saves"), window.saveProjectForTest(path));
    QFile::copy(path, firstSave);

    const QJsonArray was = panelsOf(original), now = panelsOf(firstSave);
    const bool threeTakesKept = was.size() == 3 && now.size() == 3
        && generationOf(was.at(0)) == generationOf(now.at(0));
    check(QStringLiteral("(am) the panel with three takes is UNCHANGED by "
                         "open + save: ids, paths, prompts, timestamps, "
                         "costs, the selected take, the request id - and "
                         "every status, including the finished take whose "
                         "video is not on disk (it used to be rewritten "
                         "\"Failed\")"),
          threeTakesKept,
          threeTakesKept ? QString()
                         : text(generationOf(now.at(0))).left(400));
    check(QStringLiteral("(am) the mid-generation panel is unchanged: still "
                         "\"Generating\", request id intact"),
          now.size() == 3 && generationOf(was.at(2)) == generationOf(now.at(2))
              && now.at(2).toObject().value(QStringLiteral("falRequestId"))
                     .toString() == QStringLiteral("req-live"));
    const QJsonObject folded = now.size() == 3 ? now.at(1).toObject()
                                               : QJsonObject();
    const QJsonArray foldedTakes =
        folded.value(QStringLiteral("takes")).toArray();
    const QJsonObject onlyTake = foldedTakes.size() == 1
        ? foldedTakes.at(0).toObject()
        : QJsonObject();
    check(QStringLiteral("(am) the pre-takes panel keeps its path, status "
                         "and request id, and its lone video is folded into "
                         "exactly ONE take: Complete, that path, selected"),
          foldedTakes.size() == 1
              && onlyTake.value(QStringLiteral("status")).toString()
                     == QStringLiteral("Complete")
              && onlyTake.value(QStringLiteral("videoPath")).toString()
                     == QStringLiteral("old_clip.mp4")
              && !onlyTake.value(QStringLiteral("id")).toString().isEmpty()
              && folded.value(QStringLiteral("selectedTakeId")).toString()
                     == onlyTake.value(QStringLiteral("id")).toString()
              && folded.value(QStringLiteral("generatedVideoPath")).toString()
                     == QStringLiteral("old_clip.mp4")
              && folded.value(QStringLiteral("falRequestId")).toString()
                     == QStringLiteral("req-old")
              && folded.value(QStringLiteral("generationStatus")).toString()
                     == QStringLiteral("Complete"),
          text(generationOf(folded)).left(300));

    // ---- the fixed point ------------------------------------------------------
    window.loadProjectForTest(path);
    pump(300);
    window.saveProjectForTest(path);
    check(QStringLiteral("(am) a SECOND open + save changes nothing at all: "
                         "the first save is a fixed point"),
          same(firstSave, path, &why), why.left(300));

    // ---- every video gone -------------------------------------------------------
    QFile::remove(dirA + QStringLiteral("/take_one.mp4"));
    QFile::remove(dirA + QStringLiteral("/old_clip.mp4"));
    window.loadProjectForTest(path);
    pump(300);
    window.saveProjectForTest(path);
    check(QStringLiteral("(am) with EVERY video file deleted, open + save "
                         "still changes nothing: a take's status records "
                         "what happened, not what is on the disk today"),
          same(firstSave, path, &why), why.left(300));

    // ---- Save As: the videos are not carried, the record is ---------------------
    const QString copy = dirB + QStringLiteral("/TakesCopy.sankotv");
    check(QStringLiteral("(am) Save As into another folder writes the same "
                         "generation data"),
          window.saveProjectForTest(copy) && same(firstSave, copy, &why),
          why.left(300));
    window.loadProjectForTest(copy);
    pump(300);
    window.saveProjectForTest(copy);
    check(QStringLiteral("(am) ...and re-opening and saving that copy - "
                         "which has no video beside it - still changes "
                         "nothing (every take there used to become "
                         "\"Failed\")"),
          same(firstSave, copy, &why), why.left(300));

    window.markCleanForTest();
    window.close();
    pump(300);
}

// ---- (ap) the header's timecode counts in the project's frame rate ---------
void runTimecodeRatePass(const QString &project)
{
    using namespace workspace;
    out() << "--- (ap) the timecode's frames field uses the project's frame "
             "rate ---" << Qt::endl;
    Rig r(project);
    check(QStringLiteral("(ap) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    r.window.applyProjectSettingsForTest(r.window.projectNameForTest(), 60);
    pump(200);
    clickClip(r, 0);
    check(QStringLiteral("(ap) at rest on the first panel it reads zero"),
          r.animatic->timecodeTextForTest() == QStringLiteral("00:00:00:00"),
          r.animatic->timecodeTextForTest());
    r.animatic->togglePlay();
    QElapsedTimer t;
    t.start();
    while (r.animatic->elapsedMsInCurrentPanel() < 400 && t.elapsed() < 6000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    // Read in the same turn of the event loop, with nothing pumped between:
    // the label and the clock it was written from.
    const int elapsed = r.animatic->elapsedMsInCurrentPanel();
    const QString shown = r.animatic->timecodeTextForTest();
    r.animatic->leavePreview();
    pump(200);
    const int at60 = int(elapsed / (1000.0 / 60)) % 60;
    const int at24 = int(elapsed / (1000.0 / 24)) % 24;
    check(QStringLiteral("(ap) part-way through the first second of a 60 fps "
                         "project the frames field counts sixtieths (it "
                         "counted in 24ths whatever the project was)"),
          elapsed >= 400 && elapsed < 1000 && at60 != at24
              && shown == QStringLiteral("00:00:00:%1").arg(at60, 2, 10,
                                                            QLatin1Char('0')),
          QStringLiteral("%1 ms in: shows %2; at 60 fps that is frame %3, at "
                         "24 it would be %4").arg(elapsed).arg(shown).arg(at60)
              .arg(at24));
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
    // Snapshot the settings store (org SankoTV) AS THIS PROCESS SEES IT,
    // before anything runs. Constructing the two-argument form here is
    // DELIBERATE - it is the verification instrument for the store the app
    // must never touch while sankoSettings() is overridden. Read-only.
    // Whether that is the user's real store or a packaged app's private
    // copy of it is settled in section (l), which says so.
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
    RecentProjects::setSettingsOverride(scratch
                                        + QStringLiteral("/recents.ini"));

    // Constructing the New Project dialog creates its default Save Location
    // and probes it for writability - in the user's Documents, unless it is
    // told otherwise.
    QDir().mkpath(scratch + QStringLiteral("/new_project_default"));
    NewProjectDialog::setDefaultLocationForTest(
        scratch + QStringLiteral("/new_project_default"));

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
    runRecentsStorePass(scratch);
    runSaveFailurePass(scratch);
    runRecentThumbnailSourcePass(scratch);
    runSizeCtlAgreementPass(scratch);
    runNewProjectPromptPass(a);
    runEraserLibraryPass(scratch);
    runAccentStatesPass();
    runOverrideMarkPass(scratch);
    runRecorderUnderModalPass();
    runTipShapePreviewPass(scratch);
    runStartWindowRecentsPass(scratch);
    runGrainPreviewPass(scratch);
    runStartWindowChromePass(scratch, c);
    runSuppressionPass(scratch);
    runImageCapNoticePass(scratch);
    runIdentityColorPass(scratch);
    runProvenancePass(scratch);
    runResetLayoutStripPass(a);
    runSizeBarFitPass(a);
    {
        // Their own projects: the sections above have re-timed and resized
        // the shared ones, and these compare exact panel lists.
        const QString work = writeProject(projects, QStringLiteral("Workspace"),
                                          QSize(960, 540), 24, 2, 3);
        const QString second = writeProject(projects, QStringLiteral("WorkspaceB"),
                                            QSize(1280, 720), 30, 1, 2);
        runWorkspaceSyncPass(work);
        runWorkspaceFreedPanelPass(work, second);
        runWorkspacePreviewPass(work);
        runWorkspaceKeysPass(work);
        runWorkspaceUndoPass(work);
        runWorkspaceLayoutPass(work);
        runAudioEntryPointsPass(work, scratch);
        runAudioUndoPass(work, scratch);
        runAudioMissingPass(scratch);
    }
    runTakesRoundTripPass(scratch);
    runTimecodeRatePass(writeProject(projects, QStringLiteral("Timecode"),
                                     QSize(960, 540), 30, 2, 2));

    // ---- (aa) a test that cannot start says why and exits ----------------
    // An unattended gate must FAIL, not wait. Measured 2026-10-02: with its
    // platform plugin missing, a Release family blocked on Qt's own message
    // box with nothing on stderr - Qt raises that box whenever the process
    // has no console window, which is every launch that is not typed into a
    // terminal. tests/TestHarness.cpp (linked into every family) turns a
    // dialog raised before the application object exists into a printed
    // reason and exit code 3. This starts a second copy of THIS family
    // with a platform that does not exist - the same failure, on purpose -
    // and requires it to come back by itself.
    out() << "--- (aa) a family that cannot start exits with the reason ---"
          << Qt::endl;
    {
        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_QPA_PLATFORM"),
                   QStringLiteral("sanko_no_such_platform"));
        child.setProcessEnvironment(env);
        QElapsedTimer startup;
        startup.start();
        child.start(QCoreApplication::applicationFilePath(), QStringList());
        const bool returned = child.waitForFinished(20000);
        const qint64 took = startup.elapsed();
        if (!returned) {
            child.kill(); // it is sitting on a dialog: do not leave it there
            child.waitForFinished(5000);
        }
        const QString said =
            QString::fromLocal8Bit(child.readAllStandardError())
            + QString::fromLocal8Bit(child.readAllStandardOutput());
        check(QStringLiteral("(aa) it RETURNS on its own instead of waiting "
                             "on a dialog"),
              returned,
              returned ? QStringLiteral("%1 ms").arg(took)
                       : QStringLiteral("still running after 20 s - killed"));
        check(QStringLiteral("(aa) ...with a failing exit code"),
              returned && child.exitStatus() == QProcess::NormalExit
                  ? child.exitCode() != 0
                  : returned, // a crash-style exit is a failure code too
              QStringLiteral("exit %1").arg(child.exitCode()));
        check(QStringLiteral("(aa) ...and the reason on its output, naming "
                             "the platform plugin"),
              said.contains(QStringLiteral("no Qt platform plugin"),
                            Qt::CaseInsensitive),
              said.simplified().left(170));
        // The control that this exercised the failure and nothing else: the
        // copy never got as far as a single check of its own.
        check(QStringLiteral("(aa) control: the copy failed AT STARTUP (it "
                             "ran none of this family's checks)"),
              !said.contains(QStringLiteral("PASS ("))
                  && !said.contains(QStringLiteral("RESULT ")));
    }

    // ---- (l) sankoSettings: scratch lands, the real store does not ----
    // Both halves asserted: landing in scratch is only half the guarantee,
    // and the real-store half is the one that would have caught the probe
    // contamination (a redirect that silently failed while everything
    // appeared to work).
    out() << "--- (l) sankoSettings honours the scratch override ---"
          << Qt::endl;

    // WHICH STORE IS THIS RUN GUARDING? The labels below used to say "the
    // real store" unconditionally, and for a month that was not true of the
    // runs that mattered most: a process started from a packaged app (the
    // Claude desktop app is MSIX) sees a PRIVATE copy of the registry, so
    // the before/after snapshot compared that copy with itself. Nothing in
    // the process reveals this - see RealStoreView.h for what was tried -
    // so the user's real store is read through the operating system's own
    // registry provider and compared with what this process sees.
    //
    // Either way the snapshot comparison keeps its power as a statement
    // about the CODE: a stray write lands in whichever store this process
    // can reach and shows up in the "after" (it caught the DockController
    // bypass on 2026-08-29 from inside the package). What changes is what
    // may be claimed about the user's real store, and that is now said.
    const realstore::Verdict store = realstore::available()
        ? realstore::inspect(QStringLiteral("Software\\SankoTV\\SankoTV"))
        : realstore::Verdict();
    if (realstore::available()) {
        check(QStringLiteral("(l) control: the store this run guards was "
                             "IDENTIFIED - the user's real store was read "
                             "through the OS registry provider and compared "
                             "with what this process sees"),
              store.determined,
              store.determined
                  ? QStringLiteral("this process sees %1 value(s), the real "
                                   "store holds %2; %3 differ, %4 only in "
                                   "the real store, %5 only here")
                        .arg(store.directValues).arg(store.realValues)
                        .arg(store.differing).arg(store.onlyReal)
                        .arg(store.onlyDirect)
                  : store.error);
        out() << "  GUARDING: " << store.guarded() << Qt::endl;
    }
    const QString guarded = realstore::available()
        ? store.guarded()
        : QStringLiteral("the settings store");

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
        check(QStringLiteral("(l) ...and NOT in the settings store this "
                             "process can reach"),
              !real.contains(QStringLiteral("scratchProbe/sentinel")));
        check(QStringLiteral("(l) control: that store is readable and "
                             "non-empty (the comparison can see keys)"),
              !realStoreBefore.isEmpty(),
              QStringLiteral("%1 key(s)").arg(realStoreBefore.size()));
        QMap<QString, QVariant> realStoreAfter;
        for (const QString &k : real.allKeys())
            realStoreAfter.insert(k, real.value(k));
        check(QStringLiteral("(l) the ENTIRE family wrote NOTHING to the "
                             "settings store (keys and values identical "
                             "before and after) - guarding %1").arg(guarded),
              realStoreAfter == realStoreBefore,
              QStringLiteral("%1 -> %2 key(s)")
                  .arg(realStoreBefore.size()).arg(realStoreAfter.size()));
    }
    if (realstore::available() && store.determined) {
        // Said in so many words, because a PASS above reads the same in
        // both cases and means different things.
        out() << (store.privateCopy
                      ? "  NOTE (l): this run proves the CODE writes nothing "
                        "to the settings store. It says nothing about the "
                        "user's real store, which this process cannot "
                        "reach; run the gate outside the packaged app for "
                        "that."
                      : "  NOTE (l): this run compared the user's REAL "
                        "settings store before and after the family.")
              << Qt::endl;
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
