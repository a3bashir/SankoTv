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
#include "MediaCopyProgress.h"
#include "ProjectIO.h"
#include "ProjectMedia.h"
#include "RecentProjects.h"
#include "SankoIcons.h"
#include "SankoTimelineTheme.h"
#include "SankoTheme.h"
#include "StoryboardModel.h"
#include "camera/ShotCamera.h"
#include "devrecorder/DevRecorder.h"
#include "export/MovieEncoder.h"
#include "export/Mp4Probe.h"
#include "export/ProjectExport.h"

#include "AnimaticTimeline.h"
#include "SankoNoteStyle.h"

#include <QAbstractButton>
#include <QHelpEvent>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QToolTip>
#include <memory>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QStyleOptionComboBox>
#include <QDataStream>
#include <QDockWidget>
#include <QEnterEvent>
#include <QMainWindow>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMenu>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QRandomGenerator>
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
#include <QRegularExpression>
#include <QTextStream>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QLineEdit>
#include <QToolButton>
#include <QtMath>
#include <cmath>

// Part D (2026-10-23): the zoom slider is gone from the header; the sections
// that set zoom through it use this stand-in with the slider's two calls,
// routed to the timeline's own zoom (the same applyZoom the field uses).
struct ZoomSetter
{
    AnimaticTimeline *tl = nullptr;
    void setValue(int v) { if (tl) tl->setZoomForTest(float(v)); }
    int value() const { return tl ? qRound(tl->zoomForTest()) : 0; }
};
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

    ::Brush tuned = model->preset(id)->brush();
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
              && shown == engineTip(model->preset(id)->brush(), shown.width()));

    // DISPLAY-SIZE LAYER in the studio: the Size row shows the session
    // brush's VISIBLE width; the preset keeps its engine size; the number
    // FOLLOWS an edit that changes the mark (nothing remembered).
    {
        const ::Brush stock = model->preset(id)->brush();
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
    ::Brush softBrush = model->preset(id)->brush();
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
              && shown == enginePatch(model->preset(id)->brush()));

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
        ::Brush zeroBrush = model->preset(id)->brush();
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
        ::Brush expect = model->preset(id)->brush();
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
            blue = &p.brush();
        if (p.id == QStringLiteral("builtin/drawing/sepia"))
            sepia = &p.brush();
        if (p.id == QStringLiteral("builtin/sketching/2b-pencil"))
            pencil2b = &p.brush();
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
// THE LIBRARY'S WORK ON OPEN AND SELECT (2026-10-24), through the real
// page. Opening the library, switching the tool with it open and selecting
// a brush froze the app for 1.4-2.7 s because every list rebuild hashed
// every tip on the UI thread (BrushLibrary (m1)/(m2) pin the memo and the
// keyed renderer). Pinned here END TO END, as work counts and never as
// time: a second open rebuilds, encodes, measures, renders and writes
// nothing; a tool-switch open is ONE rebuild; a selection writes only the
// Recent list and touches no preset file; a studio edit (the control) does
// write one file and render one swatch.
void runLibraryWorkCountPass(const QString &scratch)
{
    out() << "--- (bw) the library's work on open and select ---" << Qt::endl;
    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);
    const QString fixture = writeProject(scratch + QStringLiteral("/bwork"),
                                         QStringLiteral("BWork"),
                                         QSize(960, 540), 24, 1, 1);
    check(QStringLiteral("(bw) fixture opens"),
          window.loadProjectForTest(fixture));
    pump(500);
    auto *canvas = window.findChild<DrawingCanvas *>();
    auto *panel = window.findChild<brushlib::BrushLibraryPanel *>();
    auto *model = window.findChild<brushlib::BrushLibraryModel *>();
    if (!canvas || !panel || !model) {
        check(QStringLiteral("(bw) found canvas, panel and model"), false);
        window.markCleanForTest();
        window.close();
        return;
    }
    // This pass OPENS the library, which records "visible" (D1 intent)
    // and its geometry in the scratch settings; every later pass's page
    // would then restore it open - (ai) counted it as a fifth floating
    // bar the first time this ran. Put both keys back as they were.
    const QString visibleKey =
        QStringLiteral("storyboard/brushLibrary/v1/visible");
    const QString geoKey = QStringLiteral("storyboard/brushLibrary/v1/geo");
    const QVariant visibleBefore = sankoSettings().value(visibleKey);
    const QVariant geoBefore = sankoSettings().value(geoKey);
    auto restoreLibraryKeys = [&] {
        QSettings s = sankoSettings();
        if (visibleBefore.isValid())
            s.setValue(visibleKey, visibleBefore);
        else
            s.remove(visibleKey);
        if (geoBefore.isValid())
            s.setValue(geoKey, geoBefore);
        else
            s.remove(geoKey);
    };
    // The key warm-up started with the page; the counts below assume it
    // has finished (the app's user opens the library seconds after launch).
    int presets = 0;
    for (const QString &cat : brushlib::BrushLibraryModel::categories())
        presets += model->presetsIn(cat).size();
    presets += model->presetsIn(QStringLiteral("Imported")).size();
    presets += model->presetsIn(QStringLiteral("Eraser")).size();
    QElapsedTimer guard;
    guard.start();
    while (!model->keyWarmupDoneForTest() && guard.elapsed() < 180000)
        pump(50);
    check(QStringLiteral("(bw) the key warm-up finished with every "
                         "preset's keys stored"),
          model->keyWarmupDoneForTest()
              && model->keysKnownCountForTest() == presets,
          QStringLiteral("%1 of %2 known after %3 ms")
              .arg(model->keysKnownCountForTest())
              .arg(presets)
              .arg(guard.elapsed()));

    struct Work
    {
        int encodes, measures, renders, writes, rebuilds;
    };
    auto now = [&] {
        return Work{brushlib::BrushPresetCodec::imageEncodeCountForTest(),
                    brushlib::BrushWidthRatio::measureCountForTest(),
                    brushlib::BrushPreviewRenderer::renderCountForTest(),
                    brushlib::BrushPreviewRenderer::cacheWriteCountForTest(),
                    panel->rebuildCountForTest()};
    };
    auto delta = [](const Work &a, const Work &b) {
        return QStringLiteral("rebuilds +%1 encodes +%2 measures +%3 "
                              "renders +%4 writes +%5")
            .arg(b.rebuilds - a.rebuilds)
            .arg(b.encodes - a.encodes)
            .arg(b.measures - a.measures)
            .arg(b.renders - a.renders)
            .arg(b.writes - a.writes);
    };
    auto labelled = [&] {
        const QStringList ids = panel->visiblePresetIdsForTest();
        for (const QString &id : ids)
            if (panel->rowSizeLabelForTest(id) < 0)
                return false;
        return !ids.isEmpty();
    };
    auto settle = [&](int ms) {
        guard.restart();
        while (!labelled() && guard.elapsed() < ms)
            pump(50);
        return labelled();
    };

    // --- plain open, twice ------------------------------------------------
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    panel->selectCategoryForTest(QStringLiteral("Painting")); // 20 tips
    pump(200);
    panel->openAtDefault();
    pump(300);
    check(QStringLiteral("(bw) first open: every Painting row is labelled "
                         "(the worker delivered)"),
          settle(120000),
          QStringLiteral("%1 rows")
              .arg(panel->visiblePresetIdsForTest().size()));
    const Work a = now();
    panel->setVisible(false);
    pump(300);
    panel->openAtDefault();
    pump(300);
    settle(20000);
    const Work b = now();
    check(QStringLiteral("(bw) the second open of the same list rebuilds "
                         "nothing, encodes nothing, measures nothing, "
                         "renders nothing and writes nothing"),
          panel->isVisible() && labelled() && b.rebuilds == a.rebuilds
              && b.encodes == a.encodes && b.measures == a.measures
              && b.renders == a.renders && b.writes == a.writes,
          delta(a, b));

    // --- tool-switch open ---------------------------------------------------
    panel->setVisible(false);
    pump(200);
    const Work c = now();
    canvas->setTool(DrawingCanvas::Eraser); // scope follows: ONE rebuild
    pump(200);
    panel->openAtDefault();                 // the show finds it current
    pump(300);
    settle(120000);
    const Work d = now();
    check(QStringLiteral("(bw) a tool-switch open is exactly ONE rebuild "
                         "(the scope's), with no encode and no measure"),
          panel->toolScope() == brushlib::BrushLibraryPanel::ToolScope::Eraser
              && labelled() && d.rebuilds == c.rebuilds + 1
              && d.encodes == c.encodes && d.measures == c.measures,
          delta(c, d));
    panel->setVisible(false);
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    canvas->setTool(DrawingCanvas::Eraser);
    pump(200);
    const Work e = now();
    panel->openAtDefault();
    pump(300);
    settle(20000);
    const Work f = now();
    check(QStringLiteral("(bw) the eraser list seen before: the show "
                         "rebuilds, renders and writes nothing"),
          labelled() && f.rebuilds == e.rebuilds && f.renders == e.renders
              && f.writes == e.writes && f.encodes == e.encodes,
          delta(e, f));

    // --- selection ------------------------------------------------------------
    canvas->setTool(DrawingCanvas::Brush);
    pump(200);
    settle(20000);
    const QString ini = scratch + QStringLiteral("/sanko_settings.ini");
    auto iniSnapshot = [&] {
        QSettings s(ini, QSettings::IniFormat);
        s.sync();
        QMap<QString, QVariant> m;
        for (const QString &k : s.allKeys())
            m.insert(k, s.value(k));
        return m;
    };
    auto dirSnapshot = [&] {
        QStringList files;
        QDirIterator it(model->userPresetDir(), QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            files << QStringLiteral("%1|%2|%3")
                         .arg(fi.absoluteFilePath())
                         .arg(fi.size())
                         .arg(fi.lastModified().toMSecsSinceEpoch());
        }
        files.sort();
        return files;
    };
    const QString id = QStringLiteral("builtin/painting/gouache");
    const QMap<QString, QVariant> ini0 = iniSnapshot();
    const QStringList dir0 = dirSnapshot();
    const Work g = now();
    model->recordUsage(id);          // the row's click, part one
    panel->activatePresetForTest(id); // ...and part two (the page's lambda)
    pump(400);
    settle(20000);
    const Work h = now();
    const QMap<QString, QVariant> ini1 = iniSnapshot();
    const QStringList dir1 = dirSnapshot();
    QStringList changedKeys;
    for (auto it = ini1.cbegin(); it != ini1.cend(); ++it)
        if (!ini0.contains(it.key()) || ini0.value(it.key()) != it.value())
            changedKeys << it.key();
    for (auto it = ini0.cbegin(); it != ini0.cend(); ++it)
        if (!ini1.contains(it.key()))
            changedKeys << it.key();
    check(QStringLiteral("(bw) selecting a brush writes ONLY the Recent "
                         "list: one settings key changed, no preset file "
                         "touched, no cache write, no render, no encode"),
          changedKeys == QStringList{QStringLiteral("brushLibrary/v1/recent")}
              && dir1 == dir0 && h.writes == g.writes
              && h.renders == g.renders && h.encodes == g.encodes,
          QStringLiteral("changed keys: %1; files %2 -> %3; %4")
              .arg(changedKeys.join(QStringLiteral(", ")))
              .arg(dir0.size())
              .arg(dir1.size())
              .arg(delta(g, h)));
    // Positive control: a studio edit DOES write a file and render anew.
    // The edit is a tip NOBODY has had before (a fresh noise image), so
    // its swatch cannot be waiting in the disk cache from an earlier pass
    // or an earlier gate run: (n) tunes this same brush's spacing, and
    // the first run of this control found that edit's swatch cached.
    ::Brush tuned = model->preset(id)->brush();
    {
        QImage noise(64, 64, QImage::Format_Grayscale8);
        for (int y = 0; y < noise.height(); ++y) {
            uchar *row = noise.scanLine(y);
            for (int x = 0; x < noise.width(); ++x)
                row[x] = uchar(QRandomGenerator::global()->bounded(256));
        }
        tuned.setCustomShape(noise);
    }
    const Work i = now();
    check(QStringLiteral("(bw) control: the edit is accepted"),
          model->updateBrush(id, tuned));
    pump(400);
    settle(120000);
    const Work j = now();
    const QStringList dir2 = dirSnapshot();
    check(QStringLiteral("(bw) control: a studio edit writes exactly one "
                         "preset file, measures its width once and renders "
                         "exactly one new swatch"),
          dir2.size() == dir1.size() + 1 && j.renders == i.renders + 1
              && j.writes == i.writes + 1 && j.measures == i.measures + 1,
          QStringLiteral("files %1 -> %2; %3")
              .arg(dir1.size())
              .arg(dir2.size())
              .arg(delta(i, j)));
    model->resetBuiltinToStock(id);
    pump(300);
    panel->setVisible(false);
    pump(200);
    restoreLibraryKeys();
    window.markCleanForTest();
    window.close();
    pump(300);
}

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
                const bool mirrorable = !p->brush().smudgeActive()
                    && !p->brush().dualBrushEnabled();
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
                  == brushlib::BrushPresetCodec::saveBrush(gouache->brush());
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
              && canvas->paintBrush().size() == gouache->brush().size());

    // --- both selections survived --------------------------------------
    check(QStringLiteral("(m) the eraser selection survived (still "
                         "eraser-Gouache)"),
          canvas->eraserBrush().eraseMode()
              && canvas->eraserBrush().size() == gouache->brush().size());

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
                                 round->brush(), round->brush().size())
                      && panel->rowSizeLabelForTest(roundId)
                             < round->brush().size(),
                  QStringLiteral("row=%1 engine=%2")
                      .arg(panel->rowSizeLabelForTest(roundId))
                      .arg(round->brush().size()));
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
                         == splatter->brush().size(),
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
            if (p.brush().dualBrushEnabled() || p.brush().smudgeActive())
                continue; // own passes; the bar logic is identical
            const brushlib::WidthRatio r =
                brushlib::BrushWidthRatio::ratioFor(p.brush());
            canvas->setPaintBrush(p.brush());
            pump(20);
            const int engine0 = p.brush().size();
            const int label0 =
                brushlib::BrushWidthRatio::displaySize(p.brush(), engine0);
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
                round = &p.brush();
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
            hbStock = &p.brush();
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
            gouacheJ = &p.brush();
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

// ---- (sv) a save writes only the images that changed ----------------------
// The defect (2026-10-27): every Ctrl+S re-encoded every panel flatten and
// every layer as a PNG on the UI thread, changed or not - 54 images at
// 3840x2160 froze the window for 25 seconds, twice in a row with nothing
// edited in between. Now a save skips each image whose file already holds
// it (ProjectIO::ImageLedger) and encodes the rest on several threads.
//
// Skipping is the dangerous half: a skip that should have been a write
// leaves old pixels on disk under a manifest that says the work is saved.
// So every way an image can change is a check that it IS written - a
// stroke, an Undo, a file deleted or replaced behind the app's back, a
// Save As to a new folder - and every "nothing was written" is paired with
// a control proving the same measurement sees a write. The written list
// comes from the save itself and is cross-checked against the disk (a
// file's size and modification time), so neither can drift from the other.
void runIncrementalSavePass(const QString &scratch)
{
    out() << "--- (sv) a save writes only the images that changed ---" << Qt::endl;
    const QString dir = scratch + QStringLiteral("/incremental");
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);

    MainWindow window;
    window.resize(1300, 850);
    window.show();
    pump(800);

    // 1 scene x 3 panels, each a Background and one drawn layer: 9 images.
    const QString proj = writeProject(dir, QStringLiteral("Inc"), QSize(960, 540),
                                      24, 1, 3);
    const QString assets = QFileInfo(proj).absolutePath() + QStringLiteral("/Inc_assets");
    check(QStringLiteral("(sv) fixture opens"), window.loadProjectForTest(proj));
    pump(500);

    using Stamps = QMap<QString, QPair<qint64, QDateTime>>;
    auto stamps = [](const QString &folder) {
        Stamps m;
        for (const QFileInfo &fi : QDir(folder).entryInfoList(
                 {QStringLiteral("*.png")}, QDir::Files, QDir::Name))
            m.insert(fi.fileName(), {fi.size(), fi.lastModified()});
        return m;
    };
    // Files written since `before`, as the disk shows it.
    auto touched = [&stamps](const Stamps &before, const QString &folder) {
        QStringList names;
        const Stamps now = stamps(folder);
        for (auto it = now.cbegin(); it != now.cend(); ++it)
            if (!before.contains(it.key()) || before.value(it.key()) != it.value())
                names << it.key();
        return names; // sorted: QMap order
    };
    // Files written by the last save, as the save reports it.
    auto written = [&window] {
        QStringList names;
        for (const QString &f : window.lastSaveWrittenForTest())
            names << QFileInfo(f).fileName();
        names.sort();
        return names;
    };
    auto panelAt = [&window](int p) { return window.scenesForTest().at(0)->panels.at(p); };
    auto decoded = [](const QString &file) {
        return QImage(file).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };
    auto paint = [&window] {
        auto *canvas = window.findChild<DrawingCanvas *>();
        if (!canvas)
            return;
        const QTransform t = canvas->viewTransformForTest();
        sendMouse(canvas, QEvent::MouseButtonPress, t.map(QPointF(150, 130)),
                  Qt::LeftButton);
        for (int i = 1; i <= 8; ++i)
            sendMouse(canvas, QEvent::MouseMove,
                      t.map(QPointF(150 + i * 24, 130 + i * 17)), Qt::LeftButton);
        sendMouse(canvas, QEvent::MouseButtonRelease, t.map(QPointF(342, 266)),
                  Qt::LeftButton);
        pump(700);
    };
    auto save = [&window](const QString &path) {
        const bool ok = window.saveProjectForTest(path);
        pump(200);
        return ok;
    };
    const int total = stamps(assets).size();
    check(QStringLiteral("(sv) control: the fixture has 9 images on disk"),
          total == 9, QString::number(total));

    // --- (sv.1) nothing changed since opening: nothing is written --------
    Stamps before = stamps(assets);
    const bool saved1 = save(proj);
    check(QStringLiteral("(sv.1) a save with no changes since opening writes no "
                         "image"),
          saved1 && written().isEmpty() && touched(before, assets).isEmpty(),
          QStringLiteral("saved %1, written [%2], on disk [%3]")
              .arg(saved1)
              .arg(written().join(QStringLiteral(", ")),
                   touched(before, assets).join(QStringLiteral(", "))));
    check(QStringLiteral("(sv.1) CONTROL: ...because it found every image "
                         "already on disk, not because it skipped the images"),
          window.lastSaveSkippedForTest() == total,
          QStringLiteral("skipped %1 of %2").arg(window.lastSaveSkippedForTest()).arg(total));

    // --- (sv.2) one stroke: exactly that layer and its panel's flatten ---
    QVector<QVector<qint64>> keys;
    for (int p = 0; p < 3; ++p) {
        keys.append(QVector<qint64>());
        for (const Layer &layer : panelAt(p)->layers)
            keys.last().append(layer.image.cacheKey());
    }
    int strokeP = -1, strokeK = -1, changedLayers = 0;
    QImage preStroke;
    {
        // The stroke's layer, found by its pixels changing in memory - not
        // assumed - so the expectation below is the edit that happened.
        QVector<QImage> copies;
        for (const Layer &layer : panelAt(0)->layers)
            copies.append(layer.image.copy());
        paint();
        for (int p = 0; p < 3; ++p)
            for (int k = 0; k < panelAt(p)->layers.size(); ++k)
                if (panelAt(p)->layers.at(k).image.cacheKey() != keys.at(p).at(k)) {
                    ++changedLayers;
                    strokeP = p;
                    strokeK = k;
                }
        if (strokeP == 0 && strokeK >= 0)
            preStroke = copies.at(strokeK);
    }
    check(QStringLiteral("(sv.2) control: the stroke changed exactly one layer, "
                         "in memory"),
          changedLayers == 1 && strokeP == 0 && !preStroke.isNull()
              && panelAt(0)->layers.at(strokeK).image != preStroke,
          QStringLiteral("%1 layer(s) changed; panel %2 layer %3")
              .arg(changedLayers).arg(strokeP).arg(strokeK));
    const QString strokeFile =
        QStringLiteral("panel_s0_p%1_layer%2.png").arg(strokeP).arg(strokeK);
    const QString flatFile = QStringLiteral("panel_s0_p%1.png").arg(strokeP);
    QStringList expected{strokeFile, flatFile};
    expected.sort();
    const QImage onDiskBefore = decoded(assets + QStringLiteral("/") + strokeFile);
    before = stamps(assets);
    save(proj);
    check(QStringLiteral("(sv.2) a stroke on one layer rewrites exactly that "
                         "layer's image and its panel's flatten"),
          written() == expected && touched(before, assets) == expected,
          QStringLiteral("expected [%1], written [%2], on disk [%3]")
              .arg(expected.join(QStringLiteral(", ")),
                   written().join(QStringLiteral(", ")),
                   touched(before, assets).join(QStringLiteral(", "))));
    check(QStringLiteral("(sv.2) CONTROL: the rewritten file really holds the "
                         "stroke now"),
          decoded(assets + QStringLiteral("/") + strokeFile) != onDiskBefore
              && decoded(assets + QStringLiteral("/") + strokeFile)
                     == panelAt(strokeP)->layers.at(strokeK).image);

    // --- (sv.3) Save, Undo, save again: the image is rewritten -----------
    before = stamps(assets);
    save(proj);
    check(QStringLiteral("(sv.3) control: saving again with no Undo writes "
                         "nothing"),
          written().isEmpty() && touched(before, assets).isEmpty(),
          written().join(QStringLiteral(", ")));
    window.undoActionForTest()->trigger();
    pump(500);
    check(QStringLiteral("(sv.3) control: Undo put the pre-stroke pixels back "
                         "in memory"),
          panelAt(strokeP)->layers.at(strokeK).image == preStroke);
    before = stamps(assets);
    save(proj);
    check(QStringLiteral("(sv.3) save, Undo, save again: the layer and its "
                         "flatten are rewritten"),
          written() == expected && touched(before, assets) == expected,
          QStringLiteral("written [%1], on disk [%2]")
              .arg(written().join(QStringLiteral(", ")),
                   touched(before, assets).join(QStringLiteral(", "))));
    check(QStringLiteral("(sv.3) ...and the file holds the pre-stroke pixels "
                         "again"),
          decoded(assets + QStringLiteral("/") + strokeFile) == preStroke);

    // --- (sv.4) a PNG deleted behind the app's back is written again -----
    const QString victim = QStringLiteral("panel_s0_p2_layer1.png");
    QFile::remove(assets + QStringLiteral("/") + victim);
    check(QStringLiteral("(sv.4) control: the PNG is gone"),
          !QFileInfo::exists(assets + QStringLiteral("/") + victim));
    before = stamps(assets);
    save(proj);
    check(QStringLiteral("(sv.4) a deleted PNG is rewritten on the next save, "
                         "and nothing else"),
          written() == QStringList{victim} && touched(before, assets) == QStringList{victim},
          QStringLiteral("written [%1], on disk [%2]")
              .arg(written().join(QStringLiteral(", ")),
                   touched(before, assets).join(QStringLiteral(", "))));
    check(QStringLiteral("(sv.4) ...with the layer's pixels"),
          decoded(assets + QStringLiteral("/") + victim)
              == panelAt(2)->layers.at(1).image);

    // --- (sv.5) a PNG replaced by another program is written again -------
    const QString replaced = QStringLiteral("panel_s0_p1_layer1.png");
    {
        QImage other(960, 540, QImage::Format_ARGB32);
        other.fill(QColor(200, 30, 30));
        other.save(assets + QStringLiteral("/") + replaced, "PNG");
    }
    check(QStringLiteral("(sv.5) control: the file on disk is no longer the "
                         "layer"),
          decoded(assets + QStringLiteral("/") + replaced)
              != panelAt(1)->layers.at(1).image);
    save(proj);
    check(QStringLiteral("(sv.5) a PNG another program wrote over is rewritten "
                         "with the layer"),
          written() == QStringList{replaced}
              && decoded(assets + QStringLiteral("/") + replaced)
                     == panelAt(1)->layers.at(1).image,
          written().join(QStringLiteral(", ")));

    // --- (sv.6) Save As to a new folder writes every image ---------------
    const QString asPath = dir + QStringLiteral("/as/IncCopy.sankotv");
    const QString asAssets = dir + QStringLiteral("/as/IncCopy_assets");
    QDir().mkpath(dir + QStringLiteral("/as"));
    save(asPath);
    check(QStringLiteral("(sv.6) Save As to a new folder writes every image"),
          written().size() == total && stamps(asAssets).size() == total
              && window.lastSaveSkippedForTest() == 0,
          QStringLiteral("written %1, on disk %2, skipped %3 of %4")
              .arg(written().size()).arg(stamps(asAssets).size())
              .arg(window.lastSaveSkippedForTest()).arg(total));
    // (How many threads shared the encoding depends on how fast each image
    // finishes, so it is measured by the timing probe, not checked here.)
    // The same bytes the UI thread's QImage::save makes, image by image.
    {
        const QString ref = dir + QStringLiteral("/ref");
        QDir().mkpath(ref);
        auto bytes = [](const QString &file) {
            QFile f(file);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        int same = 0, compared = 0;
        QStringList differ;
        for (int p = 0; p < 3; ++p) {
            QVector<QPair<QString, QImage>> images;
            images.append({QStringLiteral("panel_s0_p%1.png").arg(p),
                           panelAt(p)->flattenedPixmap().toImage()});
            for (int k = 0; k < panelAt(p)->layers.size(); ++k)
                images.append({QStringLiteral("panel_s0_p%1_layer%2.png").arg(p).arg(k),
                               panelAt(p)->layers.at(k).image});
            for (const auto &[name, image] : images) {
                image.save(ref + QStringLiteral("/") + name, "PNG");
                ++compared;
                if (bytes(ref + QStringLiteral("/") + name)
                    == bytes(asAssets + QStringLiteral("/") + name))
                    ++same;
                else
                    differ << name;
            }
        }
        check(QStringLiteral("(sv.6) ...and every file is byte-identical to the "
                             "same image saved on this thread"),
              compared == total && same == total,
              QStringLiteral("%1 of %2 identical; differ: %3")
                  .arg(same).arg(compared).arg(differ.join(QStringLiteral(", "))));
        const QString probe = ref + QStringLiteral("/control.png");
        panelAt(0)->layers.at(1).image.save(probe, "PNG", 80);
        check(QStringLiteral("(sv.6) CONTROL: the byte comparison sees a "
                             "different encoding of the same image"),
              bytes(probe) != bytes(asAssets + QStringLiteral("/panel_s0_p0_layer1.png"))
                  && !bytes(probe).isEmpty());
    }
    before = stamps(asAssets);
    save(asPath);
    check(QStringLiteral("(sv.6) CONTROL: the next save to the new folder writes "
                         "nothing - Save As wrote everything because it had to"),
          written().isEmpty() && touched(before, asAssets).isEmpty()
              && window.lastSaveSkippedForTest() == total,
          written().join(QStringLiteral(", ")));

    // --- (sv.7) save, then reload: pixel-identical -----------------------
    paint(); // fresh work in memory, so the reload has something to prove
    QVector<QVector<QImage>> mem;
    for (int p = 0; p < 3; ++p) {
        mem.append(QVector<QImage>());
        for (const Layer &layer : panelAt(p)->layers)
            mem.last().append(layer.image.copy());
    }
    check(QStringLiteral("(sv.7) control: the save after the stroke succeeds"),
          save(asPath) && !written().isEmpty());
    window.markCleanForTest();
    check(QStringLiteral("(sv.7) the project reopens"),
          window.loadProjectForTest(asPath));
    pump(500);
    int layersSame = 0, layersCompared = 0;
    for (int p = 0; p < 3 && p < window.scenesForTest().at(0)->panels.size(); ++p)
        for (int k = 0; k < panelAt(p)->layers.size() && k < mem.at(p).size(); ++k) {
            ++layersCompared;
            if (panelAt(p)->layers.at(k).image == mem.at(p).at(k))
                ++layersSame;
        }
    check(QStringLiteral("(sv.7) save, then reload: every layer is "
                         "pixel-identical"),
          layersCompared == 6 && layersSame == 6,
          QStringLiteral("%1 of %2 identical").arg(layersSame).arg(layersCompared));
    {
        QImage nudged = mem.at(0).at(1).copy();
        nudged.setPixel(5, 5, nudged.pixel(5, 5) ^ 0x00010101u);
        check(QStringLiteral("(sv.7) CONTROL: the comparison sees a one-pixel "
                             "difference"),
              nudged != panelAt(0)->layers.at(1).image);
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

    // 1880 wide (1640 in Part B, 1400 before): the window's minimum width
    // is its content's since Part B (the timeline's header floor plus the
    // docks), and a narrower window is grown to it at once; with an audio
    // track the name must have room to stay whole, which Part C's centre
    // cluster (two 68 px gaps beside the pill, the speed dropdown) made
    // about 1860 of window. 1880 fits the 1920 test screen.
    explicit Rig(const QString &project, const QSize &size = QSize(1880, 900))
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
    // consistency board. The same search finds Consistency Board, a button
    // of the same bottom bar. (Until Pass 4 the control was Export MP4,
    // the last button of the old Animatic screen; it is File > Export now,
    // and section (an) proves it is gone from the bar.)
    bool generationButton = false, searchFindsButtons = false;
    for (QPushButton *b : r.window.findChildren<QPushButton *>()) {
        if (b->text().contains(QStringLiteral("Generation")))
            generationButton = true;
        if (b->text() == QStringLiteral("Consistency Board"))
            searchFindsButtons = true;
    }
    check(QStringLiteral("(ad) there is no Generation screen and no button "
                         "to it: the window has four screens (control: the "
                         "same search finds Consistency Board)"),
          !generationButton && searchFindsButtons && stack
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
    // Phase 1D (2026-10-29) added Quick Camera Move (a submenu) after them.
    check(QStringLiteral("(ad) right-clicking a clip selects it and offers "
                         "Add Panel After, Duplicate Panel, Delete Panel, Quick Camera Move"),
          menuSaw == QStringList({QStringLiteral("Add Panel After"),
                                  QStringLiteral("Duplicate Panel"),
                                  QStringLiteral("Delete Panel"),
                                  QStringLiteral("Quick Camera Move")}),
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

// Where a project keeps ITS OWN copy of an imported audio file: in its
// "<name>_assets" folder, under the file's own name (media policy,
// 2026-10-04). Import copies the file there and the track names the copy.
QString owned(const QString &projectFile, const QString &name)
{
    const QFileInfo project(projectFile);
    return QDir::cleanPath(project.absolutePath() + QStringLiteral("/")
                           + project.completeBaseName()
                           + QStringLiteral("_assets/") + name);
}

// Two paths to one file, as Windows compares names.
bool sameFile(const QString &a, const QString &b)
{
    return !a.isEmpty() && !b.isEmpty()
        && QDir::cleanPath(QFileInfo(a).absoluteFilePath())
               .compare(QDir::cleanPath(QFileInfo(b).absoluteFilePath()),
                        Qt::CaseInsensitive) == 0;
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
    // What the track names after an import: the project's own copy, not
    // the file that was picked (section (aq) is about that copy).
    const QString ownA = owned(project, QStringLiteral("first take.wav"));
    const QString ownB = owned(project, QStringLiteral("second.wav"));

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

    bool audioButton = false, searchFindsButtons = false;
    for (QPushButton *b : r.window.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Import Audio")
            || b->text() == QStringLiteral("Remove Audio"))
            audioButton = true;
        if (b->text() == QStringLiteral("Import Image"))
            searchFindsButtons = true;
    }
    check(QStringLiteral("(aj) the bottom bar no longer has Import Audio or "
                         "Remove Audio buttons (control: Import Image, in "
                         "the same bar, is found by the same search)"),
          !audioButton && searchFindsButtons);
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
          sameFile(r.animatic->audioPath(), ownA) && r.window.isDirty()
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
              && sameFile(r.animatic->audioPath(), ownB),
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
    const QString ownA = owned(project, QStringLiteral("first take.wav"));
    const QString ownB = owned(project, QStringLiteral("second.wav"));
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
        const bool imported = sameFile(r.animatic->audioPath(), ownA)
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
              sameFile(r.animatic->audioPath(), ownA));

        r.animatic->setAudioPickerForTest([wavB] { return wavB; });
        importAct->trigger();
        pump(200);
        const bool replaced = sameFile(r.animatic->audioPath(), ownB);
        undoAct->trigger();
        pump(250);
        check(QStringLiteral("(ak) undoing an import that REPLACED a track "
                             "restores the earlier track"),
              replaced && sameFile(r.animatic->audioPath(), ownA));
        removeAct->trigger();
        pump(200);
        const bool removed = r.animatic->audioPath().isEmpty();
        undoAct->trigger();
        pump(250);
        check(QStringLiteral("(ak) undoing Remove Audio restores the track"),
              removed && sameFile(r.animatic->audioPath(), ownA));

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
            && sameFile(r.animatic->audioPath(), ownA)
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
        // A save to ANOTHER project file: the new project gets its own copy
        // of the track and plays that from here on.
        check(QStringLiteral("(ak) the project saves with its track"),
              sameFile(r.animatic->audioPath(), ownA)
                  && r.window.saveProjectForTest(saved) && !r.window.isDirty()
                  && sameFile(r.animatic->audioPath(),
                              owned(saved, QStringLiteral("first take.wav"))),
              QDir::toNativeSeparators(r.animatic->audioPath()));
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
              sameFile(r.animatic->audioPath(),
                       owned(saved, QStringLiteral("first take.wav")))
                  && !r.animatic->audioLinked() && !r.window.isDirty()
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
//
// SINCE THE MEDIA POLICY (2026-10-04) THIS IS THE LINKED-TRACK CASE: a
// project that names its audio by an absolute path outside the project is
// what an OLD project is, and they keep working exactly as this section
// always required. The fixture is therefore written the way an old build
// wrote it (the manifest's audioPath is made absolute, below). A track the
// project OWNS, and what a missing one of those does, is section (aq).
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
    // The old project: (ak) saved this one with a track of its own, so the
    // manifest names it relative to itself. Rewrite that one value the way
    // every build before the media policy wrote it.
    {
        QFile f(saved);
        bool rewritten = false;
        if (f.open(QIODevice::ReadOnly)) {
            QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            const bool wasRelative = !QDir::isAbsolutePath(
                root.value(QStringLiteral("audioPath")).toString());
            root[QStringLiteral("audioPath")] = wavA;
            rewritten = wasRelative && f.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && f.write(QJsonDocument(root).toJson()) > 0;
            f.close(); // on disk before it is read back below
        }
        check(QStringLiteral("(al) fixture: the project is made an OLD one - "
                             "its manifest names the audio by absolute path, "
                             "outside the project (control: it was saved "
                             "naming its own copy, relative)"),
              rewritten && manifestAudio() == wavA);
    }

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
        // Part D: the header warns with an icon whose tooltip carries the message.
        const bool headerSays = r.animatic->audioWarningShownForTest()
            && r.animatic->audioWarningForTest().contains(QStringLiteral("first take.wav"))
            && r.animatic->audioWarningForTest().contains(QStringLiteral("not found"));
        r.animatic->setCollapsed(true);
        pump(400);
        bool headerSaysCollapsed = false;
        headerSaysCollapsed = r.animatic->audioWarningShownForTest()
            && r.animatic->audioWarningForTest().contains(QStringLiteral("not found"));
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
                             "track - at the project's OWN COPY of it, no "
                             "longer a link: playable, ONE command, project "
                             "unsaved"),
              relinked
                  && sameFile(r.animatic->audioPath(),
                              owned(saved, QStringLiteral("second.wav")))
                  && !r.animatic->audioLinked()
                  && !r.animatic->audioMissing() && r.window.isDirty()
                  && undo->count() == count + 1
                  && undo->undoText() == QStringLiteral("Locate Audio File")
                  && locate && !locate->isEnabled(),
              r.timeline->audioBarTextForTest());
        if (undoAct)
            undoAct->trigger();
        pump(250);
        check(QStringLiteral("(al) ...and undo returns to the missing track "
                             "at the old path, a link again"),
              r.animatic->audioPath() == wavA && r.animatic->audioMissing()
                  && r.animatic->audioLinked()
                  && r.timeline->audioBarStateForTest()
                         == QStringLiteral("missing"));

        // ---- locate: the SAME path, the file put back --------------------------
        // This used to "just load it" with no command. A linked track now
        // CONVERTS when it is located (the user's decision, 2026-10-04:
        // Locate is one of the two deliberate steps), so picking the file -
        // even where the project already says it is - copies it in and is
        // an edit. The no-command case belongs to a track the project
        // owns, and is checked in (aq).
        const bool restored = QFile::rename(parked, wavA);
        r.window.markCleanForTest();
        const int indexBefore = undo->index(); // (the undone Locate is above it)
        r.animatic->setAudioPickerForTest([wavA] { return wavA; });
        if (locate)
            locate->trigger();
        pump(250);
        const bool back = waitFor([&] {
            return r.timeline->audioBarStateForTest() == QStringLiteral("present");
        });
        check(QStringLiteral("(al) with the file put back, Locate to it "
                             "COPIES IT INTO THE PROJECT: playable, the "
                             "project's own now, ONE command, unsaved - and "
                             "the file outside is left where it was"),
              restored && back && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(),
                              owned(saved, QStringLiteral("first take.wav")))
                  && !r.animatic->audioLinked() && r.window.isDirty()
                  && undo->index() == indexBefore + 1
                  && undo->undoText() == QStringLiteral("Locate Audio File")
                  && QFileInfo::exists(wavA),
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

// ---- (an) (ao) File > Export ------------------------------------------------
// The old export wrote PNG frames into one fixed temp folder without checking
// a single write, encoded them at 24 fps whatever the project said, and let
// audio shorter than the film cut the film. None of that could be seen from
// inside the app: it reported "Export complete". So these sections never ask
// the export how it went - they open the FILE and count.
//
// AND THEY MEASURE THE SOUND. The first rebuilt export passed every check
// here while writing its sound track as HE-AAC at 16 kbit/s under a request
// for 192: the checks asked only whether a sound track existed and how long
// it was. The kind of AAC and the bit rate MEASURED from the file's bytes
// are asserted now, with an encoder that writes half the rate as the proof
// that the assertion can fail.
namespace exporting {

using sankoexport::MovieEncoder;
using sankoexport::MovieResult;
using sankoexport::MovieSpec;

int g_offers = 0, g_refusals = 0, g_dropped = 0;

// Stands in front of the real encoder and misbehaves on purpose. This is
// the MovieEncoder seam doing its second job: the first is letting another
// encoder replace Qt's.
class Meddler : public MovieEncoder
{
public:
    enum Fault { DropEveryTenth, RefuseEveryOther, HalfTheSoundBitRate };
    explicit Meddler(Fault fault)
        : m_fault(fault)
        , m_inner(sankoexport::createMovieEncoder(this))
    {
        connect(m_inner, &MovieEncoder::videoWanted, this, &MovieEncoder::videoWanted);
        connect(m_inner, &MovieEncoder::audioWanted, this, &MovieEncoder::audioWanted);
        connect(m_inner, &MovieEncoder::finished, this, &MovieEncoder::finished);
        g_offers = g_refusals = g_dropped = 0;
    }
    bool begin(const MovieSpec &spec, QString *error) override
    {
        MovieSpec handed = spec;
        // A perfectly good sound track - at half the rate that was asked.
        if (m_fault == HalfTheSoundBitRate)
            handed.audioBitRate = spec.audioBitRate / 2;
        return m_inner->begin(handed, error);
    }
    bool addFrame(const QImage &frame, int index) override
    {
        ++g_offers;
        if (m_fault == RefuseEveryOther && g_offers % 2 == 0) {
            ++g_refusals;
            QMetaObject::invokeMethod(this, &MovieEncoder::videoWanted,
                                      Qt::QueuedConnection);
            return false;
        }
        if (m_fault == DropEveryTenth && index % 10 == 9) {
            ++g_dropped;
            return true; // "accepted" - and thrown away
        }
        // Renumbered, so the file that results is a perfectly well-formed,
        // perfectly playable film that is simply missing frames.
        return m_inner->addFrame(frame, index - g_dropped);
    }
    bool endVideo() override { return m_inner->endVideo(); }
    bool addAudio(const QByteArray &pcm, qint64 first) override
    {
        return m_inner->addAudio(pcm, first);
    }
    bool endAudio() override { return m_inner->endAudio(); }
    void cancel() override { m_inner->cancel(); }

private:
    Fault m_fault;
    MovieEncoder *m_inner;
};

// No real encoder behind it at all.
class Fake : public MovieEncoder
{
public:
    enum Mode { ClaimsSuccessWritesNothing, FailsPartWay, NeverAnswers };
    explicit Fake(Mode mode) : m_mode(mode) {}
    bool begin(const MovieSpec &spec, QString *) override
    {
        if (m_mode == FailsPartWay) {
            QFile f(spec.path); // it had started writing when it failed
            if (f.open(QIODevice::WriteOnly))
                f.write("half a film");
        }
        QMetaObject::invokeMethod(this, &MovieEncoder::videoWanted,
                                  Qt::QueuedConnection);
        return true;
    }
    bool addFrame(const QImage &, int index) override
    {
        if (m_mode == NeverAnswers && index >= 5)
            return false; // refuses, and never asks again
        if (m_mode == FailsPartWay && index == 20)
            report(false, QStringLiteral("simulated: the disk is full"));
        return true;
    }
    bool endVideo() override
    {
        if (m_mode == ClaimsSuccessWritesNothing)
            report(true, QString());
        return true;
    }
    bool addAudio(const QByteArray &, qint64) override { return true; }
    bool endAudio() override { return true; }
    void cancel() override
    {
        if (m_mode != NeverAnswers)
            report(false, QString());
    }

private:
    void report(bool ok, const QString &error)
    {
        if (m_reported)
            return;
        m_reported = true;
        QMetaObject::invokeMethod(this, [this, ok, error] { emit finished(ok, error); },
                                  Qt::QueuedConnection);
    }
    Mode m_mode;
    bool m_reported = false;
};

QMenu *exportMenu(MainWindow &window)
{
    for (QAction *top : window.menuBar()->actions()) {
        QString title = top->text();
        title.remove(QLatin1Char('&'));
        if (!top->menu() || title != QStringLiteral("File"))
            continue;
        for (QAction *a : top->menu()->actions())
            if (a->menu() && a->text() == QStringLiteral("Export"))
                return a->menu();
    }
    return nullptr;
}

QStringList actionTexts(QMenu *menu)
{
    QStringList texts;
    if (menu)
        for (QAction *a : menu->actions())
            texts << a->text();
    return texts;
}

QStringList listing(const QString &folder)
{
    return QDir(folder).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                  QDir::Name);
}

QByteArray fileHash(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1);
}

QString describe(const MovieResult &r)
{
    return QStringLiteral("ok=%1 cancelled=%2 frames %3 of %4, video %5 s, "
                          "audio %6 (%7 s), %8 bytes; ")
               .arg(r.ok).arg(r.cancelled).arg(r.framesInFile).arg(r.framesExpected)
               .arg(r.videoSeconds, 0, 'f', 3)
               .arg(r.audioTrack
                        ? QStringLiteral("AAC type %1 at %2 kbit/s")
                              .arg(r.audioObjectType).arg(r.audioBitRate / 1000.0, 0, 'f', 1)
                        : QStringLiteral("no"))
               .arg(r.audioSeconds, 0, 'f', 3).arg(r.bytes)
        + r.error.simplified().left(180);
}

// The video track of a file, as the file itself says: frames, whether every
// frame lasts the same, and the frame rate that timing amounts to.
struct Seen
{
    bool readable = false;
    int frames = 0;
    bool constant = false;
    double fps = 0, seconds = 0;
    QByteArray codec;
    bool audio = false;
    QByteArray audioCodec;
    double audioSeconds = 0;
    int audioObjectType = 0; // 2 = AAC-LC, 5 = HE-AAC
    double audioKbps = 0;    // measured: the track's bytes over its length
    int audioRate = 0, audioChannels = 0;
    int colourMatrix = 0;    // 1 = the file says BT.709
    QString text() const
    {
        return QStringLiteral("%1 frame(s) of %2, %3 fps%4, %5 s, colour tag %6; "
                              "audio %7 %8 s%9")
            .arg(frames).arg(QString::fromLatin1(codec)).arg(fps, 0, 'f', 3)
            .arg(constant ? QString() : QStringLiteral(" (NOT constant)"))
            .arg(seconds, 0, 'f', 3).arg(colourMatrix)
            .arg(audio ? QString::fromLatin1(audioCodec) : QStringLiteral("none"))
            .arg(audioSeconds, 0, 'f', 3)
            .arg(audio ? QStringLiteral(", AAC object type %1, %2 kbit/s measured, "
                                        "%3 Hz, %4 ch")
                             .arg(audioObjectType).arg(audioKbps, 0, 'f', 1)
                             .arg(audioRate).arg(audioChannels)
                       : QString());
    }
    // The sound the export promises: AAC-LC, 192 kbit/s within 5 %, stereo
    // at 48 kHz.
    bool soundAsPromised() const
    {
        return audio && audioCodec == "mp4a" && audioObjectType == 2
            && qAbs(audioKbps - 192.0) < 9.6 && audioRate == 48000
            && audioChannels == 2;
    }
};

Seen look(const QString &path)
{
    Seen s;
    const sankoexport::Mp4Info info = sankoexport::readMp4(path);
    s.readable = info.readable;
    if (const sankoexport::Mp4Track *v = info.video()) {
        s.frames = int(v->samples);
        s.constant = v->timing.size() == 1;
        if (s.constant && v->timing.first().second > 0)
            s.fps = double(v->timescale) / v->timing.first().second;
        s.seconds = v->seconds();
        s.codec = v->codec;
        s.colourMatrix = v->colourMatrix;
    }
    if (const sankoexport::Mp4Track *a = info.audio()) {
        s.audio = true;
        s.audioCodec = a->codec;
        s.audioSeconds = a->seconds();
        s.audioObjectType = a->audioObjectType;
        s.audioKbps = a->bitsPerSecond() / 1000.0;
        s.audioRate = a->sampleRate;
        s.audioChannels = a->channels;
    }
    return s;
}

} // namespace exporting

void runExportMoviePass(const QString &project, const QString &scratch)
{
    using namespace workspace;
    using namespace exporting;
    out() << "--- (an) File > Export > MP4: the project's frame rate, the "
             "video's length, and every frame counted in the file ---"
          << Qt::endl;
    // Phase 1B added a separator and "Stills Show Full Artwork" (the user's
    // decision 3); a separator is a QAction with empty text, never disabled.
    const QStringList wanted = {QStringLiteral("MP4 Video..."),
                                QStringLiteral("PNG Images..."),
                                QStringLiteral("PDF Storyboard..."),
                                QString(),
                                QStringLiteral("Stills Show Full Artwork")};
    {
        MainWindow window;
        window.resize(1300, 850);
        window.show();
        pump(700);
        QMenu *menu = exportMenu(window);
        bool anyEnabled = false;
        int entries = 0;
        if (menu)
            for (QAction *a : menu->actions())
                if (!a->isSeparator()) {
                    ++entries;
                    anyEnabled = anyEnabled || a->isEnabled();
                }
        check(QStringLiteral("(an) File > Export offers MP4 Video, PNG Images "
                             "and PDF Storyboard, then Stills Show Full Artwork"),
              actionTexts(menu) == wanted,
              actionTexts(menu).join(QStringLiteral(" | ")));
        check(QStringLiteral("(an) with no project open all four entries are "
                             "disabled (the separator is not an entry)"),
              menu && entries == 4 && !anyEnabled);
        window.close();
        pump(300);
    }

    Rig r(project);
    check(QStringLiteral("(an) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    QMenu *menu = exportMenu(r.window);
    bool allEnabled = menu != nullptr;
    if (menu)
        for (QAction *a : menu->actions())
            allEnabled = allEnabled && a->isEnabled();
    check(QStringLiteral("(an) with a project open all four are enabled"),
          allEnabled);
    bool exportButton = false, searchFindsButtons = false;
    for (QPushButton *b : r.window.findChildren<QPushButton *>()) {
        if (b->text().contains(QStringLiteral("Export")))
            exportButton = true;
        if (b->text() == QStringLiteral("Import Image"))
            searchFindsButtons = true;
    }
    check(QStringLiteral("(an) the Export MP4 button is gone from the bottom "
                         "bar (control: the same search finds Import Image)"),
          !exportButton && searchFindsButtons);

    // ---- can this machine do it, and what is said when one cannot -----------
    check(QStringLiteral("(an) this machine can write an H.264 MP4 (the checks "
                         "below need it)"),
          r.window.mp4UnavailableReasonForTest().isEmpty(),
          r.window.mp4UnavailableReasonForTest().simplified().left(120));
    const QString pack = sankoexport::mediaFeaturePackMessage();
    r.window.setMp4UnavailableForTest(pack);
    check(QStringLiteral("(an) where Windows has no encoders the export says "
                         "so and names the Media Feature Pack and where to "
                         "add it (the wording and its route to the dialog; "
                         "the DETECTION cannot be exercised on this machine)"),
          r.window.mp4UnavailableReasonForTest() == pack
              && pack.contains(QStringLiteral("Media Feature Pack"))
              && pack.contains(QStringLiteral("Optional features"))
              && pack.contains(QStringLiteral("restart")));
    r.window.setMp4UnavailableForTest(QString());

    const QString dir = scratch + QStringLiteral("/export/mp4");
    QDir().mkpath(dir);
    const QString film = dir + QStringLiteral("/film.mp4");
    QUndoStack *undo = r.window.undoStackForTest();

    // ---- a silent film, through the real progress dialog ---------------------
    // The fixture is 10 seconds (2 + 3 + 2 + 3) at 30 fps.
    r.window.markCleanForTest();
    const int undoBefore = undo->count();
    const MovieResult first = r.window.exportMp4ForTest(film);
    Seen seen = look(film);
    check(QStringLiteral("(an) a silent film exports"),
          first.ok && QFile::exists(film), describe(first));
    check(QStringLiteral("(an) at the PROJECT's frame rate: 300 frames at a "
                         "constant 30 fps (the old export's fixed 24 would be "
                         "240)"),
          seen.frames == 300 && seen.constant && qAbs(seen.fps - 30.0) < 0.001,
          seen.text());
    check(QStringLiteral("(an) H.264 in an MP4, exactly as long as the panels "
                         "run, and no sound track"),
          seen.readable && seen.codec == "avc1"
              && qAbs(seen.seconds - 10.0) < 0.001 && !seen.audio,
          seen.text());
    check(QStringLiteral("(an) the file says what colour it is in: a BT.709 "
                         "tag (the first rebuilt export stored 1080p in the "
                         "older matrix and said nothing, so every player "
                         "showed it slightly wrong)"),
          seen.colourMatrix == 1 && first.colourTagged, seen.text());
    check(QStringLiteral("(an) what the export reports is what the file "
                         "holds - it read it back"),
          first.framesExpected == 300 && first.framesInFile == seen.frames
              && first.constantTiming && first.bytes == QFileInfo(film).size(),
          describe(first));
    check(QStringLiteral("(an) nothing else is left beside it: no frame "
                         "files, no partial file"),
          listing(dir) == QStringList{QStringLiteral("film.mp4")},
          listing(dir).join(QStringLiteral(", ")));
    check(QStringLiteral("(an) exporting is not an edit: the project stays "
                         "clean and the history is untouched"),
          !r.window.isDirty() && undo->count() == undoBefore);

    // ---- the frame rate follows the project -----------------------------------
    const QString name = r.window.projectNameForTest();
    r.window.applyProjectSettingsForTest(name, 24);
    pump(200);
    const MovieResult at24 = r.window.exportMp4ForTest(film);
    seen = look(film);
    check(QStringLiteral("(an) change the project to 24 fps and the same "
                         "film is 240 frames at 24 - written over the "
                         "earlier file"),
          at24.ok && seen.frames == 240 && seen.constant
              && qAbs(seen.fps - 24.0) < 0.001 && qAbs(seen.seconds - 10.0) < 0.001,
          seen.text() + QStringLiteral(" | ") + describe(at24));
    r.window.applyProjectSettingsForTest(name, 30);
    pump(200);

    // ---- audio: the video's length is the film's length ------------------------
    const QString shortWav = scratch + QStringLiteral("/export/audio/short.wav");
    const QString longWav = scratch + QStringLiteral("/export/audio/long.wav");
    check(QStringLiteral("(an) fixture: a 4 s and a 15 s WAV written"),
          audio::writeWav(shortWav, 4.0) && audio::writeWav(longWav, 15.0));
    r.animatic->setAudioPath(shortWav);
    pump(300);
    check(QStringLiteral("(an) control: with the audio file present there is "
                         "nothing to warn about"),
          r.window.mp4AudioWarningForTest().isEmpty() && !r.animatic->audioMissing());
    const QString shortFilm = dir + QStringLiteral("/short_audio.mp4");
    const MovieResult withShort = r.window.exportMp4ForTest(shortFilm);
    seen = look(shortFilm);
    check(QStringLiteral("(an) audio SHORTER than the film does not cut the "
                         "film: still 300 frames and 10 s, with a sound track "
                         "of about 4 s (the old -shortest stopped the film at "
                         "4 s)"),
          withShort.ok && seen.frames == 300 && qAbs(seen.seconds - 10.0) < 0.001
              && seen.audio && seen.audioSeconds > 3.8 && seen.audioSeconds < 4.3,
          seen.text() + QStringLiteral(" | ") + describe(withShort));
    check(QStringLiteral("(an) THE SOUND IS WHAT WAS PROMISED, measured from "
                         "the file: AAC-LC, 192 kbit/s, 48 kHz stereo (the "
                         "first rebuilt export wrote HE-AAC at 16 kbit/s and "
                         "every check here passed)"),
          seen.soundAsPromised() && withShort.audioObjectType == 2
              && qAbs(withShort.audioBitRate - 192000.0) < 9600.0,
          seen.text());
    r.animatic->setAudioPath(longWav);
    pump(300);
    const QString longFilm = dir + QStringLiteral("/long_audio.mp4");
    const MovieResult withLong = r.window.exportMp4ForTest(longFilm);
    seen = look(longFilm);
    check(QStringLiteral("(an) audio LONGER than the film ends with the "
                         "film: 300 frames, 10 s, and a sound track that "
                         "stops within a tenth of a second of the last frame "
                         "and never after it (15 s of audio went in)"),
          withLong.ok && seen.frames == 300 && qAbs(seen.seconds - 10.0) < 0.001
              && seen.audio && seen.audioSeconds > 9.9
              && seen.audioSeconds <= seen.seconds && seen.soundAsPromised(),
          seen.text() + QStringLiteral(" | ") + describe(withLong));
    // The control for the sound check: an encoder that does everything
    // right except the bit rate. Its file plays; the export must refuse it.
    {
        const QString halfPath = dir + QStringLiteral("/half_rate_sound.mp4");
        const QStringList beforeHalf = listing(dir);
        r.window.setMovieEncoderFactoryForTest(
            [] { return new Meddler(Meddler::HalfTheSoundBitRate); });
        const MovieResult half = r.window.exportMp4ForTest(halfPath);
        r.window.setMovieEncoderFactoryForTest({});
        check(QStringLiteral("(an) AN ENCODER THAT WRITES THE SOUND AT HALF "
                             "THE RATE and reports success is caught: the "
                             "export measures 96 kbit/s in the file, refuses "
                             "it and says why"),
              !half.ok && !half.cancelled && half.audioObjectType == 2
                  && qAbs(half.audioBitRate - 96000.0) < 4800.0
                  && half.error.contains(QStringLiteral("96 kbit/s"))
                  && half.error.contains(QStringLiteral("192"))
                  && !QFile::exists(halfPath) && listing(dir) == beforeHalf,
              describe(half));
    }

    // ---- a missing audio file is said, not skipped ------------------------------
    const QString gone = scratch + QStringLiteral("/export/audio/gone.wav");
    r.animatic->setAudioPath(gone);
    pump(300);
    const QString warning = r.window.mp4AudioWarningForTest();
    check(QStringLiteral("(an) a project whose audio file is missing: the "
                         "export says so, by name, before it asks for a file"),
          r.animatic->audioMissing() && warning.contains(QStringLiteral("gone.wav"))
              && warning.contains(QStringLiteral("without sound")),
          warning.simplified().left(140));
    const QString silentFilm = dir + QStringLiteral("/silent_after_warning.mp4");
    const MovieResult silent = r.window.exportMp4ForTest(silentFilm);
    seen = look(silentFilm);
    check(QStringLiteral("(an) ...and \"Export Without Sound\" gives exactly "
                         "that: the whole film, no sound track"),
          silent.ok && seen.frames == 300 && !seen.audio,
          seen.text() + QStringLiteral(" | ") + describe(silent));

    // ---- an audio file that is there but cannot be read --------------------------
    const QString bad = scratch + QStringLiteral("/export/audio/not_audio.wav");
    {
        QFile f(bad);
        if (f.open(QIODevice::WriteOnly))
            f.write(QByteArray(4096, 'x'));
    }
    r.animatic->setAudioPath(bad);
    pump(300);
    const QString badFilm = dir + QStringLiteral("/bad_audio.mp4");
    const QStringList before = listing(dir);
    const MovieResult withBad = r.window.exportMp4ForTest(badFilm);
    check(QStringLiteral("(an) an audio file that cannot be read FAILS the "
                         "export and names the file - it does not quietly "
                         "export a silent film"),
          !withBad.ok && !withBad.cancelled
              && withBad.error.contains(QStringLiteral("not_audio.wav"))
              && !QFile::exists(badFilm) && listing(dir) == before,
          describe(withBad));
    r.animatic->setAudioPath(QString());
    pump(300);

    // ---- cancel -------------------------------------------------------------------
    const QByteArray filmBefore = fileHash(film);
    r.window.setExportProgressHookForTest(
        [](int, int total) { return total <= 0; }); // Cancel, as soon as it starts
    const MovieResult cancelled = r.window.exportMp4ForTest(film);
    r.window.setExportProgressHookForTest({});
    check(QStringLiteral("(an) Cancel: no error, no new file, no partial "
                         "file - and the film already at that name is "
                         "untouched"),
          cancelled.cancelled && !cancelled.ok && cancelled.error.isEmpty()
              && !filmBefore.isEmpty() && fileHash(film) == filmBefore
              && listing(dir) == before,
          describe(cancelled) + QStringLiteral(" | ")
              + listing(dir).join(QStringLiteral(", ")));

    // ---- a film Windows cannot even start writing ---------------------------------
    const QString noFolder = scratch + QStringLiteral("/export/no_such_folder/film.mp4");
    const MovieResult unwritable = r.window.exportMp4ForTest(noFolder);
    check(QStringLiteral("(an) a film whose folder does not exist fails with "
                         "what Windows said, and creates nothing (control: "
                         "the same export into a real folder worked above)"),
          !unwritable.ok && !unwritable.cancelled
              && unwritable.error.contains(QStringLiteral("Windows could not"))
              && !QFileInfo::exists(QFileInfo(noFolder).absolutePath())
              && listing(dir) == before,
          describe(unwritable));

    // ---- THE READ-BACK, against encoders that misbehave ----------------------------
    auto exportWith = [&](const std::function<MovieEncoder *()> &factory,
                          const QString &path) {
        r.window.setMovieEncoderFactoryForTest(factory);
        const MovieResult result = r.window.exportMp4ForTest(path);
        r.window.setMovieEncoderFactoryForTest({});
        return result;
    };
    const MovieResult dropped = exportWith(
        [] { return new Meddler(Meddler::DropEveryTenth); }, film);
    check(QStringLiteral("(an) AN ENCODER THAT SILENTLY DROPS ONE FRAME IN "
                         "TEN and reports success is caught: the export "
                         "counts 270 of 300 in the file and refuses it"),
          !dropped.ok && g_dropped == 30 && dropped.framesInFile == 270
              && dropped.error.contains(QStringLiteral("270"))
              && dropped.error.contains(QStringLiteral("300")),
          describe(dropped));
    check(QStringLiteral("(an) ...the short file is not kept, and the good "
                         "film at that name is still the good film"),
          fileHash(film) == filmBefore && listing(dir) == before,
          listing(dir).join(QStringLiteral(", ")));
    const QString grudged = dir + QStringLiteral("/refused_frames.mp4");
    const MovieResult refused = exportWith(
        [] { return new Meddler(Meddler::RefuseEveryOther); }, grudged);
    seen = look(grudged);
    check(QStringLiteral("(an) a frame the encoder REFUSES is offered again, "
                         "not skipped: every other offer refused, and the "
                         "film still has all 300 frames"),
          refused.ok && g_refusals >= 150 && seen.frames == 300 && seen.constant,
          QStringLiteral("%1 refusal(s) in %2 offer(s); %3").arg(g_refusals)
              .arg(g_offers).arg(seen.text()));
    QFile::remove(grudged);
    const QString nowhere = dir + QStringLiteral("/claimed.mp4");
    const MovieResult claimed = exportWith(
        [] { return new Fake(Fake::ClaimsSuccessWritesNothing); }, nowhere);
    check(QStringLiteral("(an) an encoder that reports success having "
                         "written nothing: the export fails, and says the "
                         "file has no video"),
          !claimed.ok && claimed.error.contains(QStringLiteral("no video"))
              && !QFile::exists(nowhere) && listing(dir) == before,
          describe(claimed));
    const MovieResult failed = exportWith(
        [] { return new Fake(Fake::FailsPartWay); }, nowhere);
    check(QStringLiteral("(an) an encoder that fails part-way: its reason is "
                         "passed on, and the half-written file it left is "
                         "removed"),
          !failed.ok && !failed.cancelled
              && failed.error.contains(QStringLiteral("the disk is full"))
              && listing(dir) == before,
          describe(failed) + QStringLiteral(" | ")
              + listing(dir).join(QStringLiteral(", ")));
    r.window.setExportStallTimeoutForTest(1500);
    QElapsedTimer stallTimer;
    stallTimer.start();
    const MovieResult stalled = exportWith(
        [] { return new Fake(Fake::NeverAnswers); }, nowhere);
    r.window.setExportStallTimeoutForTest(0);
    check(QStringLiteral("(an) an encoder that stops answering does not hang "
                         "the app: the export gives up and says so"),
          !stalled.ok && stalled.error.contains(QStringLiteral("stopped making "
                                                               "progress"))
              && stallTimer.elapsed() < 15000 && listing(dir) == before,
          QStringLiteral("%1 ms; %2").arg(stallTimer.elapsed()).arg(describe(stalled)));
    const MovieResult after = r.window.exportMp4ForTest(film);
    seen = look(film);
    check(QStringLiteral("(an) control: after all of that the real encoder "
                         "still exports the whole film"),
          after.ok && seen.frames == 300, describe(after));
}

void runExportFilesPass(const QString &project, const QString &board,
                        const QString &scratch)
{
    using namespace workspace;
    using namespace exporting;
    out() << "--- (ao) File > Export > PNG and PDF: every write checked, "
             "every file whole or absent ---" << Qt::endl;
    {
        Rig r(project);
        check(QStringLiteral("(ao) the workspace opened"), r.ok);
        if (!r.ok)
            return;
        const QVector<sankoexport::ExportPanel> panels =
            sankoexport::collectPanels(r.window.scenesForTest());
        const QString dir = scratch + QStringLiteral("/export/png");
        QDir().mkpath(dir);
        check(QStringLiteral("(ao) control: an empty folder holds nothing a "
                             "PNG export would replace"),
              panels.size() == 4 && r.window.pngExportClashesForTest(dir).isEmpty());
        r.window.markCleanForTest();
        const sankoexport::FilesResult png = r.window.exportPngForTest(dir);
        const QStringList names = {QStringLiteral("Export_S01_P01.png"),
                                   QStringLiteral("Export_S01_P02.png"),
                                   QStringLiteral("Export_S02_P01.png"),
                                   QStringLiteral("Export_S02_P02.png")};
        check(QStringLiteral("(ao) PNG: one file per panel, named by project, "
                             "scene and panel, and nothing else in the folder"),
              png.ok && png.written.size() == 4 && listing(dir) == names,
              png.error + listing(dir).join(QStringLiteral(", ")));
        bool sizes = true, pixels = true;
        for (int i = 0; i < panels.size() && i < names.size(); ++i) {
            const QImage file(dir + QLatin1Char('/') + names.at(i));
            const QImage flat = panels.at(i).panel->flattenedPixmap().toImage();
            sizes = sizes && file.size() == QSize(960, 540);
            pixels = pixels
                && file.convertToFormat(QImage::Format_ARGB32)
                       == flat.convertToFormat(QImage::Format_ARGB32);
        }
        const QImage firstFile(dir + QLatin1Char('/') + names.at(0));
        const bool differs = panels.size() > 1
            && firstFile.convertToFormat(QImage::Format_ARGB32)
                   != panels.at(1).panel->flattenedPixmap().toImage()
                          .convertToFormat(QImage::Format_ARGB32);
        check(QStringLiteral("(ao) each PNG is at the project's size and is "
                             "pixel for pixel what its panel shows (control: "
                             "panel 1's file is NOT panel 2's picture)"),
              sizes && pixels && differs);
        check(QStringLiteral("(ao) a second export into that folder knows "
                             "which four files it would replace; and "
                             "exporting was not an edit"),
              r.window.pngExportClashesForTest(dir) == names && !r.window.isDirty(),
              r.window.pngExportClashesForTest(dir).join(QStringLiteral(", ")));

        // A write that cannot happen: the third panel's name is taken by a
        // FOLDER, so no file can be put there.
        const QString blocked = scratch + QStringLiteral("/export/png_blocked");
        QDir().mkpath(blocked + QStringLiteral("/Export_S02_P01.png"));
        const sankoexport::FilesResult stopped = r.window.exportPngForTest(blocked);
        check(QStringLiteral("(ao) a PNG that cannot be written STOPS the "
                             "export and names the panel (the old export "
                             "checked no write at all)"),
              !stopped.ok && !stopped.cancelled
                  && stopped.error.contains(QStringLiteral("Scene 2, panel 1")),
              stopped.error.simplified().left(160));
        check(QStringLiteral("(ao) ...the two files before it are complete "
                             "and reported, and no half-written file is left"),
              stopped.written.size() == 2
                  && listing(blocked)
                         == QStringList{names.at(0), names.at(1), names.at(2)}
                  && QFileInfo(blocked + QLatin1Char('/') + names.at(2)).isDir()
                  && !QImage(blocked + QLatin1Char('/') + names.at(1)).isNull(),
              listing(blocked).join(QStringLiteral(", ")));

        const QString partDir = scratch + QStringLiteral("/export/png_cancel");
        r.window.setExportProgressHookForTest(
            [](int done, int) { return done < 1; });
        const sankoexport::FilesResult part = r.window.exportPngForTest(partDir);
        r.window.setExportProgressHookForTest({});
        check(QStringLiteral("(ao) Cancel after the first PNG: one whole file, "
                             "and it says cancelled rather than failed"),
              part.cancelled && !part.ok && part.error.isEmpty()
                  && part.written.size() == 1
                  && listing(partDir) == QStringList{names.at(0)},
              listing(partDir).join(QStringLiteral(", ")));
        check(QStringLiteral("(ao) a project name a file name cannot hold is "
                             "made safe, not refused"),
              sankoexport::pngFileName(QStringLiteral("A:B/C*?"), panels.first())
                  == QStringLiteral("A_B_C___S01_P01.png"),
              sankoexport::pngFileName(QStringLiteral("A:B/C*?"), panels.first()));

        // The four-panel board: one page.
        const QString onePage = scratch + QStringLiteral("/export/pdf/four.pdf");
        QDir().mkpath(QFileInfo(onePage).absolutePath());
        const sankoexport::FilesResult four = r.window.exportPdfForTest(onePage);
        QFile f(onePage);
        const QString text = f.open(QIODevice::ReadOnly)
            ? QString::fromLatin1(f.readAll())
            : QString();
        const int pages = int(
            text.count(QRegularExpression(QStringLiteral("/Type\\s*/Page(?!s)"))));
        check(QStringLiteral("(ao) PDF: four panels are one page"),
              four.ok && text.startsWith(QStringLiteral("%PDF-")) && pages == 1,
              QStringLiteral("%1 page(s); %2").arg(pages).arg(four.error));
    }

    Rig r(board);
    check(QStringLiteral("(ao) the 13-panel board opened"), r.ok);
    if (!r.ok)
        return;
    const QString dir = scratch + QStringLiteral("/export/pdf");
    QDir().mkpath(dir);
    const QString pdfPath = dir + QStringLiteral("/board.pdf");
    const sankoexport::FilesResult pdf = r.window.exportPdfForTest(pdfPath);
    QFile f(pdfPath);
    const QString text = f.open(QIODevice::ReadOnly)
        ? QString::fromLatin1(f.readAll())
        : QString();
    const int pages =
        int(text.count(QRegularExpression(QStringLiteral("/Type\\s*/Page(?!s)"))));
    check(QStringLiteral("(ao) PDF: thirteen panels make three pages - six "
                         "to a page"),
          pdf.ok && text.startsWith(QStringLiteral("%PDF-")) && pages == 3,
          QStringLiteral("%1 page(s); %2").arg(pages).arg(pdf.error));
    const QRegularExpressionMatch box = QRegularExpression(
        QStringLiteral("/MediaBox\\s*\\[\\s*0\\s+0\\s+([0-9.]+)\\s+([0-9.]+)"))
        .match(text);
    check(QStringLiteral("(ao) ...each A4, landscape (842 x 595 points)"),
          box.hasMatch() && qAbs(box.captured(1).toDouble() - 841.9) < 1.0
              && qAbs(box.captured(2).toDouble() - 595.3) < 1.0,
          box.hasMatch() ? box.captured(0) : QStringLiteral("no MediaBox"));
    const sankoexport::FilesResult png =
        r.window.exportPngForTest(scratch + QStringLiteral("/export/png_board"));
    const QImage boardPng(scratch + QStringLiteral("/export/png_board/"
                                                   "Board_S01_P13.png"));
    check(QStringLiteral("(ao) PNG at a different project size: thirteen "
                         "files, each 1280 x 720"),
          png.ok && png.written.size() == 13 && boardPng.size() == QSize(1280, 720),
          QStringLiteral("%1 file(s), last %2x%3").arg(png.written.size())
              .arg(boardPng.width()).arg(boardPng.height()));

    const QString noFolder = scratch + QStringLiteral("/export/no_such_folder/x.pdf");
    const sankoexport::FilesResult lost = r.window.exportPdfForTest(noFolder);
    check(QStringLiteral("(ao) a PDF that cannot be written fails with the "
                         "file's name, and creates nothing"),
          !lost.ok && lost.error.contains(QStringLiteral("x.pdf"))
              && !QFile::exists(noFolder),
          lost.error.simplified().left(140));
    const QString halfPdf = dir + QStringLiteral("/cancelled.pdf");
    r.window.setExportProgressHookForTest([](int done, int) { return done < 7; });
    const sankoexport::FilesResult half = r.window.exportPdfForTest(halfPdf);
    r.window.setExportProgressHookForTest({});
    check(QStringLiteral("(ao) Cancel part-way through a PDF leaves no PDF "
                         "at all (control: the finished one is still there)"),
          half.cancelled && !QFile::exists(halfPdf) && QFile::exists(pdfPath),
          listing(dir).join(QStringLiteral(", ")));
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

// ---- (aq) the soundtrack travels with the project --------------------------
// The media policy (HANDOFF, adopted 2026-10-03, built for audio 2026-10-04):
// media a project owns lives in its "<name>_assets" folder, is named
// relative to the manifest, and is carried by Save As - as panel images
// are. Before it, the audio track was an absolute path to wherever the file
// happened to be, and a project moved, copied or handed to someone else
// arrived without its sound.
//
// "PLAYS" HERE IS MEASURED, NOT HEARD: the player loaded the file and read
// its real length (the bar is "present" and says that length), its position
// advances under Play, and an MP4 exported from the moved project has a
// sound track as long as the film. The gate has no ears.
namespace travel {

using workspace::Rig;

QString manifestText(const QString &project)
{
    QFile f(project);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                       : QStringLiteral("<unreadable>");
}

QString manifestAudio(const QString &project)
{
    QFile f(project);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("<unreadable>");
    return QJsonDocument::fromJson(f.readAll())
        .object()
        .value(QStringLiteral("audioPath"))
        .toString();
}

// Make a project an OLD one: its audio named the way every build before
// the policy wrote it.
bool setManifestAudio(const QString &project, const QString &value)
{
    QFile f(project);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    root[QStringLiteral("audioPath")] = value;
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && f.write(QJsonDocument(root).toJson()) > 0;
}

bool copyFolder(const QString &from, const QString &to)
{
    if (!QDir().mkpath(to))
        return false;
    const QDir dir(from);
    for (const QFileInfo &entry :
         dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString target = to + QStringLiteral("/") + entry.fileName();
        if (entry.isDir() ? !copyFolder(entry.absoluteFilePath(), target)
                          : !QFile::copy(entry.absoluteFilePath(), target))
            return false;
    }
    return true;
}

// Everything in an assets folder that is not a panel image: the audio
// files - and any half-written leftover, which is what this is for.
QStringList media(const QString &project)
{
    QStringList found;
    const QFileInfo info(project);
    const QDir dir(info.absolutePath() + QStringLiteral("/")
                   + info.completeBaseName() + QStringLiteral("_assets"));
    for (const QString &name :
         dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (!name.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
            found << name;
    return found;
}

// A silent WAV of the same length - and so the same SIZE - as another, with
// one sample that differs: what a re-export under the same name is.
bool writeWavVariant(const QString &path, double seconds)
{
    if (!audio::writeWav(path, seconds))
        return false;
    QFile f(path);
    return f.open(QIODevice::ReadWrite) && f.seek(200) && f.write("\x01", 1) == 1;
}

bool present(Rig &r)
{
    return audio::waitFor([&] {
        return r.timeline->audioBarStateForTest() == QStringLiteral("present");
    });
}

void pick(Rig &r, const QString &actionText, const QString &file)
{
    r.animatic->setAudioPickerForTest([file] { return file; });
    if (QAction *a = audio::editAction(r.window, actionText))
        a->trigger();
    pump(200);
    r.animatic->setAudioPickerForTest({});
}

bool plays(Rig &r, qint64 *reached)
{
    workspace::clickClip(r, 0);
    r.animatic->togglePlay();
    const bool moved = audio::waitFor([&] {
        return r.animatic->audioPlayingForTest()
            && r.animatic->audioPositionForTest() > 150;
    });
    *reached = r.animatic->audioPositionForTest();
    r.animatic->leavePreview();
    pump(200);
    return moved;
}

// A copy that FAILS warns, in a modal this run cannot click. This closes
// it and keeps what it said - so "it failed loudly" is a check on the
// words, and "it was cancelled quietly" is a check that there were none.
struct Warnings
{
    QTimer timer;
    QStringList texts;
    Warnings()
    {
        timer.setInterval(80);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal)
                return;
            if (auto *box = qobject_cast<QMessageBox *>(modal))
                texts << box->text();
            modal->close();
        });
        timer.start();
    }
};

} // namespace travel

void runAudioTravelsPass(const QString &scratch)
{
    using namespace travel;
    using audio::editAction;
    using audio::owned;
    using audio::sameFile;
    using audio::writeWav;
    using exporting::fileHash;
    out() << "--- (aq) the soundtrack travels with the project: copied in at "
             "import, named relative, carried by Save As ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/travel");
    QDir(base).removeRecursively();
    const QString src = base + QStringLiteral("/source/theme.wav");
    const QString other = base + QStringLiteral("/elsewhere/theme.wav");
    const QString voice = base + QStringLiteral("/source/voice.wav");
    const QSize size(960, 540);
    // 1 scene, 2 panels: 2 s + 3 s = a 5 second film, 120 frames at 24.
    const QString film = writeProject(base + QStringLiteral("/home"),
                                      QStringLiteral("Film"), size, 24, 1, 2);
    check(QStringLiteral("(aq) fixture: a project, a 6 s WAV, a different "
                         "WAV of the same name and the same size, and a "
                         "third"),
          QFileInfo::exists(film) && writeWav(src, 6.0)
              && writeWavVariant(other, 6.0) && writeWav(voice, 2.0)
              && QFileInfo(src).size() == QFileInfo(other).size()
              && fileHash(src) != fileHash(other));
    const QByteArray srcHash = fileHash(src);
    const QString themeName = QStringLiteral("theme.wav");

    // ---- import copies the file in; save names it relative -----------------
    {
        Rig r(film);
        check(QStringLiteral("(aq) the workspace opened"), r.ok);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        const QByteArray manifestBefore = fileHash(film);
        const QString own = owned(film, themeName);
        pick(r, QStringLiteral("Import Audio..."), src);
        check(QStringLiteral("(aq) IMPORT COPIES THE FILE INTO THE PROJECT: "
                             "the track is the copy in Film_assets, byte for "
                             "byte the file that was picked, which is left "
                             "where it was - one command, project unsaved"),
              sameFile(r.animatic->audioPath(), own) && !r.animatic->audioLinked()
                  && fileHash(own) == srcHash && !srcHash.isEmpty()
                  && fileHash(src) == srcHash
                  && undo->undoText() == QStringLiteral("Import Audio")
                  && r.window.isDirty(),
              QDir::toNativeSeparators(r.animatic->audioPath()));
        check(QStringLiteral("(aq) ...and the project file on disk has not "
                             "been touched by the import: what was last "
                             "saved is still exactly what was saved"),
              fileHash(film) == manifestBefore && manifestAudio(film).isEmpty());
        check(QStringLiteral("(aq) a fine track shows no warning in the header, and "
                             "the bar carries its name and real length (a track the "
                             "project owns is not marked)"),
              present(r) && !r.animatic->audioWarningShownForTest()
                  && r.timeline->audioBarTextForTest()
                         == QStringLiteral("theme.wav | 0:06"),
              r.animatic->audioWarningForTest() + QStringLiteral(" / ")
                  + r.timeline->audioBarTextForTest());
        const bool saved = r.window.saveProjectForTest(film);
        const QString text = manifestText(film);
        check(QStringLiteral("(aq) SAVE NAMES IT RELATIVE TO THE PROJECT "
                             "FILE - \"Film_assets/theme.wav\" - and no "
                             "absolute path appears anywhere in the manifest"),
              saved && manifestAudio(film) == QStringLiteral("Film_assets/theme.wav")
                  && !text.contains(base, Qt::CaseInsensitive)
                  && !text.contains(QDir::toNativeSeparators(base),
                                    Qt::CaseInsensitive)
                  && !text.contains(QStringLiteral(":/"))
                  && !text.contains(QStringLiteral(":\\")),
              manifestAudio(film));
        check(QStringLiteral("(aq) an ordinary save leaves the track where "
                             "it is and the project clean"),
              sameFile(r.animatic->audioPath(), own) && !r.window.isDirty()
                  && media(film) == QStringList({themeName}),
              media(film).join(QStringLiteral(", ")));
    }

    // ---- the file that was picked is gone ------------------------------------
    check(QStringLiteral("(aq) fixture: the file that was imported is "
                         "deleted"),
          QFile::remove(src) && !QFileInfo::exists(src));
    {
        Rig r(film);
        if (!r.ok) {
            check(QStringLiteral("(aq) the project re-opens"), false);
            return;
        }
        check(QStringLiteral("(aq) with the picked file DELETED the project "
                             "re-opens with its track: present, its own, "
                             "clean, nothing on the history"),
              present(r) && !r.animatic->audioMissing()
                  && !r.animatic->audioLinked() && !r.window.isDirty()
                  && r.window.undoStackForTest()->count() == 0,
              r.timeline->audioBarTextForTest());
    }

    // ---- THE FOLDER IS MOVED ---------------------------------------------------
    // A control first, because "it still plays" must be a result the check
    // could have failed: an OLD project, naming its audio by absolute path
    // - a file INSIDE its own folder, as favourably as an absolute path can
    // be placed - moved the same way, must lose it.
    const QString linkedHome = base + QStringLiteral("/home2");
    const QString linkedProject = writeProject(linkedHome, QStringLiteral("Linked"),
                                               size, 24, 1, 2);
    const QString linkedWav = linkedHome + QStringLiteral("/Linked/sound/theme.wav");
    check(QStringLiteral("(aq) fixture: an old project whose manifest names "
                         "an audio file in its own folder by absolute path"),
          writeWav(linkedWav, 6.0) && setManifestAudio(linkedProject, linkedWav));
    {
        Rig r(linkedProject);
        check(QStringLiteral("(aq) control: where it was made, the old "
                             "project plays its track"),
              r.ok && present(r) && r.animatic->audioLinked());
    }
    const QString movedHome = base + QStringLiteral("/moved");
    const QString movedLinkedHome = base + QStringLiteral("/moved2");
    check(QStringLiteral("(aq) both project folders are MOVED - renamed, so "
                         "nothing is left at the old place to be found by "
                         "accident"),
          QDir().rename(base + QStringLiteral("/home"), movedHome)
              && QDir().rename(linkedHome, movedLinkedHome)
              && !QFileInfo::exists(film) && !QFileInfo::exists(linkedWav));
    const QString movedFilm = movedHome + QStringLiteral("/Film/Film.sankotv");
    {
        Rig r(movedLinkedHome + QStringLiteral("/Linked/Linked.sankotv"));
        check(QStringLiteral("(aq) CONTROL: the moved OLD project has lost "
                             "its sound - missing, still naming the old "
                             "place (so the next check can fail)"),
              r.ok && r.animatic->audioMissing() && r.animatic->audioLinked()
                  && r.animatic->audioPath() == linkedWav
                  && r.timeline->audioBarStateForTest() == QStringLiteral("missing"),
              r.ok ? r.timeline->audioBarTextForTest() : QString());
    }
    {
        Rig r(movedFilm);
        if (!r.ok) {
            check(QStringLiteral("(aq) the moved project opens"), false);
            return;
        }
        check(QStringLiteral("(aq) THE MOVED PROJECT STILL HAS ITS "
                             "SOUNDTRACK: it opens clean, the track is the "
                             "file in the folder's NEW place, present, with "
                             "its real length"),
              present(r) && !r.window.isDirty() && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), owned(movedFilm, themeName))
                  && r.timeline->audioBarTextForTest()
                         == QStringLiteral("theme.wav | 0:06"),
              QDir::toNativeSeparators(r.animatic->audioPath()));
        qint64 reached = 0;
        const bool advanced = plays(r, &reached);
        check(QStringLiteral("(aq) ...and it PLAYS: under Play the audio "
                             "player is playing and its position advances"),
              advanced, QStringLiteral("reached %1 ms").arg(reached));
        const QString mp4 = base + QStringLiteral("/moved_film.mp4");
        const sankoexport::MovieResult made = r.window.exportMp4ForTest(mp4);
        const exporting::Seen seen = exporting::look(mp4);
        check(QStringLiteral("(aq) ...and an MP4 exported from the moved "
                             "project has its sound: 120 frames, 5 s, and a "
                             "sound track that runs the film's length (6 s "
                             "of audio went in)"),
              made.ok && seen.frames == 120 && seen.audio
                  && seen.audioSeconds > 4.9 && seen.audioSeconds <= seen.seconds
                  && seen.soundAsPromised(),
              seen.text() + QStringLiteral(" | ") + exporting::describe(made));
    }

    // ---- THE FOLDER IS COPIED, and the original deleted -------------------------
    const QString copiedHome = base + QStringLiteral("/copied");
    const QString copiedFilm = copiedHome + QStringLiteral("/Film/Film.sankotv");
    check(QStringLiteral("(aq) the project folder is COPIED and the "
                         "original deleted"),
          copyFolder(movedHome, copiedHome) && QDir(movedHome).removeRecursively()
              && !QFileInfo::exists(movedFilm));
    const QString cut2 = base + QStringLiteral("/saveas/Cut2.sankotv");
    const QString cut3 = base + QStringLiteral("/saveas2/Cut3.sankotv");
    QDir().mkpath(QFileInfo(cut2).absolutePath());
    QDir().mkpath(QFileInfo(cut3).absolutePath());
    {
        Rig r(copiedFilm);
        if (!r.ok) {
            check(QStringLiteral("(aq) the copied project opens"), false);
            return;
        }
        check(QStringLiteral("(aq) THE COPY HAS ITS SOUNDTRACK: clean, "
                             "present, the file in the copy's own folder"),
              present(r) && !r.window.isDirty() && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), owned(copiedFilm, themeName)),
              QDir::toNativeSeparators(r.animatic->audioPath()));

        // ---- Save As carries it ------------------------------------------------
        const QByteArray originalManifest = fileHash(copiedFilm);
        const bool savedAs = r.window.saveProjectForTest(cut2);
        check(QStringLiteral("(aq) SAVE AS CARRIES THE TRACK: the new "
                             "project has its own copy, identical, named "
                             "relative to ITS file (\"Cut2_assets/"
                             "theme.wav\")"),
              savedAs && fileHash(owned(cut2, themeName)) == srcHash
                  && manifestAudio(cut2) == QStringLiteral("Cut2_assets/theme.wav"),
              manifestAudio(cut2));
        check(QStringLiteral("(aq) ...the open project now plays the NEW "
                             "copy, and is clean - a Save As is not an edit"),
              present(r) && sameFile(r.animatic->audioPath(), owned(cut2, themeName))
                  && !r.window.isDirty(),
              QDir::toNativeSeparators(r.animatic->audioPath()));
        check(QStringLiteral("(aq) ...and the project it was saved FROM is "
                             "untouched: its manifest and its audio file are "
                             "byte for byte what they were"),
              fileHash(copiedFilm) == originalManifest
                  && fileHash(owned(copiedFilm, themeName)) == srcHash);
    }
    check(QStringLiteral("(aq) fixture: the Save As copy's folder is "
                         "deleted"),
          QDir(QFileInfo(cut2).absolutePath()).removeRecursively());
    {
        Rig r(copiedFilm);
        check(QStringLiteral("(aq) the ORIGINAL survives its Save As copy "
                             "being deleted: track present"),
              r.ok && present(r) && !r.animatic->audioMissing());
        if (!r.ok)
            return;
        check(QStringLiteral("(aq) fixture: saved as another project"),
              r.window.saveProjectForTest(cut3));
    }
    check(QStringLiteral("(aq) fixture: the original's folder is deleted"),
          QDir(copiedHome).removeRecursively() && !QFileInfo::exists(copiedFilm));

    // ---- a Save As whose copy cannot be made ------------------------------------
    const QString failed = base + QStringLiteral("/saveas2/Fail.sankotv");
    const QString cancelled = base + QStringLiteral("/saveas2/Stopped.sankotv");
    {
        Rig r(cut3);
        check(QStringLiteral("(aq) the SAVE AS COPY survives the original "
                             "being deleted: track present"),
              r.ok && present(r) && !r.animatic->audioMissing());
        if (!r.ok)
            return;
        r.animatic->setPanelDurationForTest(0, 0, 4); // unsaved work
        pump(200);
        const QString own3 = owned(cut3, themeName);
        bool ok = true;
        {
            Warnings warnings;
            ProjectMedia::setWriteLimitForTest(1000); // the disk "fills"
            ok = r.window.saveProjectForTest(failed);
            ProjectMedia::setWriteLimitForTest(-1);
            check(QStringLiteral("(aq) A SAVE AS WHOSE AUDIO COPY CANNOT BE "
                                 "MADE FAILS LOUDLY: it returns false and "
                                 "says the project was NOT saved, naming the "
                                 "audio file and why"),
                  !ok && warnings.texts.size() == 1
                      && warnings.texts.value(0).contains(QStringLiteral("NOT saved"))
                      && warnings.texts.value(0).contains(themeName)
                      && warnings.texts.value(0).contains(QStringLiteral("audio"))
                      && warnings.texts.value(0).contains(QStringLiteral("disk may be full")),
                  warnings.texts.join(QStringLiteral(" / ")).simplified().left(220));
        }
        check(QStringLiteral("(aq) ...no project file was written, nothing "
                             "half-copied was left, and the open project is "
                             "still the one it was: same file, same track, "
                             "still unsaved"),
              !QFileInfo::exists(failed) && media(failed).isEmpty()
                  && sameFile(r.window.projectPathForTest(), cut3)
                  && sameFile(r.animatic->audioPath(), own3) && r.window.isDirty(),
              media(failed).join(QStringLiteral(", ")));
        {
            Warnings warnings;
            MediaCopyProgress::setTapForTest([](qint64, qint64) { return false; });
            ok = r.window.saveProjectForTest(cancelled);
            MediaCopyProgress::setTapForTest({});
            check(QStringLiteral("(aq) CANCELLING the copy stops the Save As "
                                 "the same way - and says nothing, because "
                                 "the artist is the one who stopped it"),
                  !ok && warnings.texts.isEmpty() && !QFileInfo::exists(cancelled)
                      && media(cancelled).isEmpty()
                      && sameFile(r.window.projectPathForTest(), cut3)
                      && r.window.isDirty(),
                  warnings.texts.join(QStringLiteral(" / ")));
        }
        ok = r.window.saveProjectForTest(failed);
        check(QStringLiteral("(aq) control: with neither, the same Save As "
                             "succeeds and the copy is there"),
              ok && fileHash(owned(failed, themeName)) == srcHash
                  && !r.window.isDirty());
    }

    // ---- a project file renamed by hand ------------------------------------------
    const QString renamed = base + QStringLiteral("/saveas2/Renamed.sankotv");
    check(QStringLiteral("(aq) fixture: a project file is copied under "
                         "another name, in Explorer's way - its assets "
                         "folder keeps the old name"),
          QFile::copy(failed, renamed));
    {
        Rig r(renamed);
        check(QStringLiteral("(aq) a project file RENAMED BY HAND still "
                             "finds its track, in the folder with the old "
                             "name"),
              r.ok && present(r)
                  && sameFile(r.animatic->audioPath(), owned(failed, themeName)));
        if (!r.ok)
            return;
        const bool saved = r.window.saveProjectForTest(renamed);
        check(QStringLiteral("(aq) ...and its next save brings the track "
                             "into the folder of its NEW name, as it does "
                             "the images; the other project's file is "
                             "untouched"),
              saved && fileHash(owned(renamed, themeName)) == srcHash
                  && manifestAudio(renamed)
                         == QStringLiteral("Renamed_assets/theme.wav")
                  && sameFile(r.animatic->audioPath(), owned(renamed, themeName))
                  && fileHash(owned(failed, themeName)) == srcHash,
              manifestAudio(renamed));
    }

    // ---- names: the same file twice, two files of one name ------------------------
    const QString names = writeProject(base + QStringLiteral("/names"),
                                       QStringLiteral("Names"), size, 24, 1, 1);
    const QString second = QStringLiteral("theme (2).wav");
    check(QStringLiteral("(aq) fixture: the first WAV is written again"),
          writeWav(src, 6.0) && fileHash(src) == srcHash);
    {
        Rig r(names);
        check(QStringLiteral("(aq) a second project opened"), r.ok);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        pick(r, QStringLiteral("Import Audio..."), src);
        const QString own = owned(names, themeName);
        const bool first = sameFile(r.animatic->audioPath(), own)
            && media(names) == QStringList({themeName});
        r.window.markCleanForTest();
        const int count = undo->count();
        pick(r, QStringLiteral("Import Audio..."), src);
        check(QStringLiteral("(aq) THE SAME FILE IMPORTED AGAIN changes "
                             "nothing: no second copy, no command, not "
                             "unsaved (control: the first import made one "
                             "file)"),
              first && sameFile(r.animatic->audioPath(), own)
                  && media(names) == QStringList({themeName})
                  && undo->count() == count && !r.window.isDirty(),
              media(names).join(QStringLiteral(", ")));
        pick(r, QStringLiteral("Import Audio..."), other);
        check(QStringLiteral("(aq) A DIFFERENT FILE OF THE SAME NAME (and "
                             "the same size) arrives as \"theme (2).wav\": "
                             "the first file is not overwritten, the track "
                             "and its label are the new one, one command"),
              sameFile(r.animatic->audioPath(), owned(names, second))
                  && media(names) == QStringList({second, themeName})
                  && fileHash(own) == srcHash
                  && fileHash(owned(names, second)) == fileHash(other)
                  && present(r) && !r.animatic->audioWarningShownForTest()
                  && undo->count() == count + 1,
              media(names).join(QStringLiteral(", ")) + QStringLiteral(" / ")
                  + r.animatic->audioWarningForTest());
        pick(r, QStringLiteral("Import Audio..."), src);
        check(QStringLiteral("(aq) importing the first file again finds its "
                             "copy already there and uses it: the track "
                             "goes back to theme.wav and no third file is "
                             "made"),
              sameFile(r.animatic->audioPath(), own)
                  && media(names) == QStringList({second, themeName})
                  && undo->count() == count + 2);
        pick(r, QStringLiteral("Import Audio..."), owned(names, second));
        check(QStringLiteral("(aq) a file picked from INSIDE the project's "
                             "folder is used where it is"),
              sameFile(r.animatic->audioPath(), owned(names, second))
                  && media(names) == QStringList({second, themeName})
                  && undo->count() == count + 3);

        // ---- undo deletes nothing; redo plays with the sources gone -----------
        while (undo->canUndo())
            undo->undo();
        pump(250);
        check(QStringLiteral("(aq) UNDOING EVERY IMPORT removes the track "
                             "and DELETES NOTHING: both copied files are "
                             "still in the project's folder"),
              r.animatic->audioPath().isEmpty()
                  && media(names) == QStringList({second, themeName})
                  && fileHash(own) == srcHash,
              media(names).join(QStringLiteral(", ")));
        const bool sourcesGone = QFile::remove(src) && QFile::remove(other);
        while (undo->canRedo())
            undo->redo();
        pump(250);
        check(QStringLiteral("(aq) ...which is what lets REDO bring back a "
                             "track that plays after the files that were "
                             "picked have been deleted"),
              sourcesGone && present(r) && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), owned(names, second)),
              r.timeline->audioBarTextForTest());
        if (QAction *removeAct = editAction(r.window, QStringLiteral("Remove Audio")))
            removeAct->trigger();
        pump(200);
        check(QStringLiteral("(aq) Remove Audio removes the track from the "
                             "project and leaves its file in the folder"),
              r.animatic->audioPath().isEmpty()
                  && media(names) == QStringList({second, themeName}));

        // ---- an import whose copy cannot be made ---------------------------------
        r.window.markCleanForTest();
        const int before = undo->count();
        const int at = undo->index();
        {
            Warnings warnings;
            ProjectMedia::setWriteLimitForTest(1000);
            pick(r, QStringLiteral("Import Audio..."), voice);
            ProjectMedia::setWriteLimitForTest(-1);
            check(QStringLiteral("(aq) AN IMPORT WHOSE COPY FAILS imports "
                                 "nothing and says so, naming the file: no "
                                 "track, no command, not unsaved, and no "
                                 "partial file in the project's folder"),
                  r.animatic->audioPath().isEmpty() && undo->count() == before
                      && undo->index() == at && !r.window.isDirty()
                      && media(names) == QStringList({second, themeName})
                      && warnings.texts.size() == 1
                      && warnings.texts.value(0).contains(QStringLiteral("NOT copied"))
                      && warnings.texts.value(0).contains(QStringLiteral("voice.wav")),
                  warnings.texts.join(QStringLiteral(" / ")).simplified().left(200)
                      + QStringLiteral(" | ") + media(names).join(QStringLiteral(", ")));
        }
        {
            Warnings warnings;
            MediaCopyProgress::setTapForTest([](qint64, qint64) { return false; });
            pick(r, QStringLiteral("Import Audio..."), voice);
            MediaCopyProgress::setTapForTest({});
            check(QStringLiteral("(aq) a CANCELLED copy imports nothing "
                                 "either, leaves nothing, and says nothing"),
                  r.animatic->audioPath().isEmpty() && undo->index() == at
                      && !r.window.isDirty()
                      && media(names) == QStringList({second, themeName})
                      && warnings.texts.isEmpty(),
                  warnings.texts.join(QStringLiteral(" / ")));
        }
        pick(r, QStringLiteral("Import Audio..."), voice);
        check(QStringLiteral("(aq) control: with neither, the same import "
                             "succeeds"),
              sameFile(r.animatic->audioPath(),
                       owned(names, QStringLiteral("voice.wav")))
                  && undo->index() == at + 1);

        // ---- a project with no file yet -------------------------------------------
        // Not reachable through the app (New Project writes the file before
        // the workspace opens), so the animatic is TOLD the project has
        // none: the track must then name the file where it is, and the
        // first save must bring it in.
        if (QAction *removeAct = editAction(r.window, QStringLiteral("Remove Audio")))
            removeAct->trigger();
        pump(200);
        r.animatic->setProjectFile(QString());
        const QStringList mediaBefore = media(names);
        pick(r, QStringLiteral("Import Audio..."), voice);
        const bool held = sameFile(r.animatic->audioPath(), voice)
            && !r.animatic->audioLinked() && media(names) == mediaBefore;
        const QString firstSave = base + QStringLiteral("/first/First.sankotv");
        QDir().mkpath(QFileInfo(firstSave).absolutePath());
        const bool saved = r.window.saveProjectForTest(firstSave);
        check(QStringLiteral("(aq) A PROJECT WITH NO FILE YET: the import "
                             "names the file where it is (there is nowhere "
                             "to copy it), and the FIRST SAVE copies it in "
                             "and names it relative"),
              held && saved
                  && fileHash(owned(firstSave, QStringLiteral("voice.wav")))
                         == fileHash(voice)
                  && manifestAudio(firstSave)
                         == QStringLiteral("First_assets/voice.wav")
                  && sameFile(r.animatic->audioPath(),
                              owned(firstSave, QStringLiteral("voice.wav"))),
              manifestAudio(firstSave));
    }

    // ---- AN OLD PROJECT: a linked track ----------------------------------------------
    const QString shared = base + QStringLiteral("/shared/score.wav");
    const QString scoreName = QStringLiteral("score.wav");
    const QString old = writeProject(base + QStringLiteral("/old"),
                                     QStringLiteral("Old"), size, 24, 1, 2);
    const QString linkedText = QString::fromUtf8("score.wav \xE2\x80\x94 linked");
    check(QStringLiteral("(aq) fixture: an old project naming an audio file "
                         "outside its folder by absolute path"),
          writeWav(shared, 4.0) && setManifestAudio(old, shared)
              && manifestAudio(old) == shared);
    const QByteArray sharedHash = fileHash(shared);
    {
        Rig r(old);
        if (!r.ok) {
            check(QStringLiteral("(aq) the old project opens"), false);
            return;
        }
        QAction *copyIn =
            editAction(r.window, QStringLiteral("Copy Audio Into Project"));
        QAction *locate = editAction(r.window, QStringLiteral("Locate Audio File..."));
        check(QStringLiteral("(aq) AN OLD PROJECT LOADS AS IT ALWAYS DID: "
                             "clean, its track playing from the absolute "
                             "path, exactly as stored - a LINKED track"),
              present(r) && !r.window.isDirty() && r.animatic->audioLinked()
                  && r.animatic->audioPath() == shared
                  && r.window.undoStackForTest()->count() == 0);
        check(QStringLiteral("(aq) A LINKED TRACK SAYS SO WITHOUT HOVERING: "
                             "the bar reads \"score.wav - linked\" (and the header, which "
                             "warns only of a problem, shows nothing)"),
              !r.animatic->audioWarningShownForTest()
                  && r.timeline->audioBarTextForTest()
                         == linkedText + QStringLiteral(" | 0:04"),
              r.animatic->audioWarningForTest() + QStringLiteral(" / ")
                  + r.timeline->audioBarTextForTest());
        QStringList menuSaw;
        r.animatic->setAudioMenuHookForTest([&](QMenu *menu) {
            menuSaw.clear();
            for (QAction *a : menu->actions())
                if (!a->isSeparator())
                    menuSaw << a->text();
        });
        audio::rightClickAudioRow(r);
        r.animatic->setAudioMenuHookForTest({});
        check(QStringLiteral("(aq) Edit > Copy Audio Into Project is enabled "
                             "for it (Locate is not: nothing is missing), "
                             "and the track's own menu leads with it"),
              copyIn && copyIn->isEnabled() && locate && !locate->isEnabled()
                  && menuSaw == QStringList({QStringLiteral("Copy Audio Into Project"),
                                             QStringLiteral("Import Audio..."),
                                             QStringLiteral("Remove Audio")}),
              menuSaw.join(QStringLiteral(" | ")));
        r.animatic->setPanelDurationForTest(0, 0, 4);
        pump(200);
        const bool saved = r.window.saveProjectForTest(old);
        check(QStringLiteral("(aq) A SAVE DOES NOT CONVERT IT: the manifest "
                             "still names the absolute path, character for "
                             "character, and no audio file has appeared in "
                             "the project's folder (control: the search "
                             "that found no absolute path in a new "
                             "project's manifest finds this one)"),
              saved && manifestAudio(old) == shared && media(old).isEmpty()
                  && r.animatic->audioLinked()
                  && manifestText(old).contains(base, Qt::CaseInsensitive),
              manifestAudio(old));
    }
    {
        Rig r(old);
        if (!r.ok) {
            check(QStringLiteral("(aq) the old project re-opens"), false);
            return;
        }
        const QString oldCopy = base + QStringLiteral("/old/Old/OldCopy.sankotv");
        const bool again = r.window.saveProjectForTest(old);
        const bool fixedPoint = manifestAudio(old) == shared && media(old).isEmpty();
        const bool savedAs = r.window.saveProjectForTest(oldCopy);
        check(QStringLiteral("(aq) open + save again is a fixed point, and "
                             "SAVE AS keeps the link too: the copy names "
                             "the same absolute path and gets no audio file"),
              again && fixedPoint && savedAs && manifestAudio(oldCopy) == shared
                  && media(oldCopy).isEmpty() && r.animatic->audioLinked()
                  && r.animatic->audioPath() == shared,
              manifestAudio(oldCopy));
    }
    {
        Rig r(old);
        if (!r.ok) {
            check(QStringLiteral("(aq) the old project opens a third time"), false);
            return;
        }
        QUndoStack *undo = r.window.undoStackForTest();
        QAction *copyIn =
            editAction(r.window, QStringLiteral("Copy Audio Into Project"));
        const QString own = owned(old, scoreName);
        if (copyIn)
            copyIn->trigger();
        pump(250);
        check(QStringLiteral("(aq) COPY AUDIO INTO PROJECT converts it: the "
                             "track is the project's own copy, identical; "
                             "one command, unsaved; the label no longer "
                             "says linked and the action is off; the file "
                             "outside is left alone"),
              present(r) && sameFile(r.animatic->audioPath(), own)
                  && !r.animatic->audioLinked() && fileHash(own) == sharedHash
                  && undo->count() == 1
                  && undo->undoText() == QStringLiteral("Copy Audio Into Project")
                  && r.window.isDirty()
                  && !r.animatic->audioWarningShownForTest()
                  && copyIn && !copyIn->isEnabled() && fileHash(shared) == sharedHash,
              r.animatic->audioWarningForTest());
        undo->undo();
        pump(250);
        const bool linkedAgain = r.animatic->audioLinked()
            && r.animatic->audioPath() == shared
            && !r.animatic->audioWarningShownForTest()
            && media(old) == QStringList({scoreName});
        undo->redo();
        pump(250);
        check(QStringLiteral("(aq) undo returns to the linked track (the "
                             "copy stays in the folder); redo converts "
                             "again"),
              linkedAgain && sameFile(r.animatic->audioPath(), own)
                  && !r.animatic->audioLinked());
        const bool savedConverted = r.window.saveProjectForTest(old);
        const QString nowNamed = manifestAudio(old);
        check(QStringLiteral("(aq) saved, the converted project names its "
                             "own copy, relative"),
              savedConverted && nowNamed == QStringLiteral("Old_assets/score.wav"),
              nowNamed);
    }
    const QString oldMoved = base + QStringLiteral("/old_moved");
    check(QStringLiteral("(aq) fixture: the file outside is deleted and the "
                         "converted project's folder moved"),
          QFile::remove(shared)
              && QDir().rename(base + QStringLiteral("/old"), oldMoved));
    {
        Rig r(oldMoved + QStringLiteral("/Old/Old.sankotv"));
        check(QStringLiteral("(aq) ONCE CONVERTED, THE OLD PROJECT TRAVELS "
                             "TOO: moved, with the outside file gone, its "
                             "track is present and not linked"),
              r.ok && present(r) && !r.animatic->audioMissing()
                  && !r.animatic->audioLinked() && !r.window.isDirty());
    }

    // ---- a track the project owns, whose file has gone ----------------------------------
    const QString lost = writeProject(base + QStringLiteral("/lost"),
                                      QStringLiteral("Lost"), size, 24, 1, 2);
    const QString ownLost = owned(lost, themeName);
    const QString outside = base + QStringLiteral("/found/theme.wav");
    check(QStringLiteral("(aq) fixture: the WAV again, in another folder"),
          writeWav(outside, 6.0) && fileHash(outside) == srcHash);
    {
        Rig r(lost);
        if (!r.ok) {
            check(QStringLiteral("(aq) a third project opens"), false);
            return;
        }
        pick(r, QStringLiteral("Import Audio..."), outside);
        check(QStringLiteral("(aq) fixture: a project saved with a track of "
                             "its own"),
              r.window.saveProjectForTest(lost)
                  && manifestAudio(lost) == QStringLiteral("Lost_assets/theme.wav"));
    }
    check(QStringLiteral("(aq) fixture: its audio file is deleted from the "
                         "project's folder"),
          QFile::remove(ownLost));
    {
        Rig r(lost);
        if (!r.ok) {
            check(QStringLiteral("(aq) it opens without its audio file"), false);
            return;
        }
        QAction *locate = editAction(r.window, QStringLiteral("Locate Audio File..."));
        check(QStringLiteral("(aq) A MISSING TRACK OF THE PROJECT'S OWN "
                             "opens clean and is shown as missing, by name; "
                             "it still names its place in the project's "
                             "folder, and Locate is enabled"),
              !r.window.isDirty() && r.animatic->audioMissing()
                  && !r.animatic->audioLinked()
                  && sameFile(r.animatic->audioPath(), ownLost)
                  && r.timeline->audioBarStateForTest() == QStringLiteral("missing")
                  && r.timeline->audioBarTextForTest().contains(themeName)
                  && locate && locate->isEnabled(),
              r.timeline->audioBarTextForTest());
        r.animatic->setPanelDurationForTest(0, 0, 4);
        pump(200);
        const bool saved = r.window.saveProjectForTest(lost);
        const QString lostCopy = base + QStringLiteral("/lost/Lost/LostCopy.sankotv");
        bool savedAs = false;
        {
            Warnings warnings;
            savedAs = r.window.saveProjectForTest(lostCopy);
            check(QStringLiteral("(aq) a save keeps its reference, and a "
                                 "SAVE AS DOES NOT FAIL over a file that was "
                                 "not there to copy: it succeeds, silently, "
                                 "and the copy names the track as missing "
                                 "in its own folder"),
                  saved && manifestAudio(lost) == QStringLiteral("Lost_assets/theme.wav")
                      && savedAs && warnings.texts.isEmpty()
                      && manifestAudio(lostCopy)
                             == QStringLiteral("LostCopy_assets/theme.wav")
                      && r.animatic->audioMissing() && media(lostCopy).isEmpty(),
                  manifestAudio(lost) + QStringLiteral(" / ")
                      + manifestAudio(lostCopy));
        }
    }
    {
        Rig r(lost);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        const bool wasMissing = r.animatic->audioMissing();
        pick(r, QStringLiteral("Locate Audio File..."), outside);
        check(QStringLiteral("(aq) LOCATE, with a file of the same name "
                             "from elsewhere: it is copied in under the name "
                             "the project already says, so the document "
                             "does not change - playable, no command, not "
                             "unsaved (control: it was missing)"),
              wasMissing && present(r) && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), ownLost)
                  && fileHash(ownLost) == srcHash && undo->count() == 0
                  && !r.window.isDirty(),
              r.timeline->audioBarTextForTest());
    }
    check(QStringLiteral("(aq) fixture: deleted again"), QFile::remove(ownLost));
    {
        Rig r(lost);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        const bool wasMissing = r.animatic->audioMissing();
        const bool putBack = QFile::copy(outside, ownLost);
        pick(r, QStringLiteral("Locate Audio File..."), ownLost);
        check(QStringLiteral("(aq) LOCATE, the file PUT BACK where the "
                             "project says and picked there: it just loads "
                             "- no command, not unsaved"),
              wasMissing && putBack && present(r) && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), ownLost)
                  && undo->count() == 0 && !r.window.isDirty()
                  && media(lost) == QStringList({themeName}),
              media(lost).join(QStringLiteral(", ")));
    }
    check(QStringLiteral("(aq) fixture: deleted a third time"),
          QFile::remove(ownLost));
    {
        Rig r(lost);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        pick(r, QStringLiteral("Locate Audio File..."), voice);
        const bool repointed = present(r)
            && sameFile(r.animatic->audioPath(),
                        owned(lost, QStringLiteral("voice.wav")))
            && undo->count() == 1
            && undo->undoText() == QStringLiteral("Locate Audio File")
            && r.window.isDirty();
        undo->undo();
        pump(250);
        check(QStringLiteral("(aq) LOCATE, a file of ANOTHER name: the "
                             "project now says something else, so it is one "
                             "command and unsaved - and undo returns to the "
                             "missing track at the old name"),
              repointed && r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), ownLost),
              r.timeline->audioBarTextForTest());

        // ---- the progress window ---------------------------------------------------
        // No copy in this run lasts long enough to show it (a 6 s WAV is
        // 130 KB), so the window is driven the way a slow copy drives it:
        // the same reports, arriving over most of a second.
        auto shownDialog = []() -> QProgressDialog * {
            for (QWidget *w : QApplication::topLevelWidgets())
                if (auto *d = qobject_cast<QProgressDialog *>(w))
                    if (d->isVisible())
                        return d;
            return nullptr;
        };
        bool quickMadeNone = false;
        {
            MediaCopyProgress quick(&r.window, QStringLiteral("quick"));
            const ProjectMedia::Progress report = quick.callback();
            bool went = true;
            for (int i = 1; i <= 10; ++i)
                went = went && report(i * 100, 1000);
            pump(100);
            quickMadeNone = went && !shownDialog();
        }
        const QString saying = QStringLiteral("Copying theme.wav into the project...");
        bool appeared = false, modal = false, stopped = false;
        QString said;
        int value = -1;
        {
            MediaCopyProgress slow(&r.window, saying);
            const ProjectMedia::Progress report = slow.callback();
            bool went = report(1, 1000);
            const bool noneAtFirst = !shownDialog();
            QElapsedTimer t;
            t.start();
            while (went && t.elapsed() < 900) {
                QThread::msleep(40);
                went = report(qMin<qint64>(900, t.elapsed()), 1000);
            }
            if (QProgressDialog *dialog = shownDialog()) {
                appeared = went && noneAtFirst;
                modal = dialog->isModal();
                said = dialog->labelText();
                value = dialog->value();
                dialog->cancel(); // what pressing Cancel does
                stopped = !report(950, 1000);
            }
        }
        pump(100);
        check(QStringLiteral("(aq) THE PROGRESS WINDOW: a copy that is over "
                             "at once never makes one; a copy still running "
                             "after half a second shows one - modal, saying "
                             "what is being copied, part-way along - Cancel "
                             "stops the copy, and the window is gone when "
                             "the copy is"),
              quickMadeNone && appeared && modal && said == saying && value > 0
                  && value < 1000 && stopped && !shownDialog(),
              QStringLiteral("quick made none: %1; appeared: %2, modal: %3, "
                             "at %4 of 1000, \"%5\"; Cancel stopped it: %6")
                  .arg(quickMadeNone).arg(appeared).arg(modal).arg(value)
                  .arg(said).arg(stopped));
    }
}

// ---- (ar) a track that cannot be heard says why -----------------------------
// 2026-10-05: the user imported a WAV and heard nothing. The file was fine.
// SankoTV had been started before the speaker was connected, and Qt's audio
// output keeps the device that was the system default when it was made - so
// the film played, "successfully", into a device nobody could hear, and
// nothing anywhere said so. Looking for it turned up the other silences: a
// file the player cannot open was accepted at import and then sat on the
// track with its name and no bar, for ever. Silence with no explanation is
// the failure; each of these now says what it is.
void runAudioSaysWhyPass(const QString &scratch)
{
    using namespace travel;
    using audio::editAction;
    using audio::owned;
    using audio::sameFile;
    using audio::waitFor;
    using audio::writeWav;
    out() << "--- (ar) a track that cannot be heard says why: files that "
             "cannot be read, and where the sound goes ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/saywhy");
    QDir(base).removeRecursively();
    const QString good = base + QStringLiteral("/source/good.wav");
    const QString other = base + QStringLiteral("/source/second.wav");
    const QString notAudio = base + QStringLiteral("/source/not_audio.wav");
    const QString empty = base + QStringLiteral("/source/empty.wav");
    const QString project = writeProject(base + QStringLiteral("/p"),
                                         QStringLiteral("Say"), QSize(960, 540),
                                         24, 1, 2);
    bool wroteJunk = false;
    {
        QDir().mkpath(QFileInfo(notAudio).absolutePath());
        QFile f(notAudio);
        wroteJunk = f.open(QIODevice::WriteOnly)
            && f.write(QByteArray(4096, 'x')) == 4096;
    }
    check(QStringLiteral("(ar) fixture: a project, two real WAVs, a file "
                         "named .wav that is not audio, and a WAV with a "
                         "correct header and no sound in it"),
          QFileInfo::exists(project) && writeWav(good, 3.0) && writeWav(other, 2.0)
              && wroteJunk && writeWav(empty, 0.0));
    const QString ownGood = owned(project, QStringLiteral("good.wav"));
    {
        Rig r(project);
        check(QStringLiteral("(ar) the workspace opened"), r.ok);
        if (!r.ok)
            return;
        QUndoStack *undo = r.window.undoStackForTest();
        {
            Warnings warnings;
            pick(r, QStringLiteral("Import Audio..."), notAudio);
            check(QStringLiteral("(ar) A FILE THAT IS NOT AUDIO IS REFUSED AT "
                                 "IMPORT, in words: it names the file, says "
                                 "it was NOT imported and why - and nothing "
                                 "was copied, no track, no command, not "
                                 "unsaved"),
                  warnings.texts.size() == 1
                      && warnings.texts.value(0).contains(
                             QStringLiteral("cannot read this file as audio"))
                      && warnings.texts.value(0).contains(QStringLiteral("NOT imported"))
                      && warnings.texts.value(0).contains(QStringLiteral("not_audio.wav"))
                      && r.animatic->audioPath().isEmpty() && undo->count() == 0
                      && !r.window.isDirty() && media(project).isEmpty(),
                  warnings.texts.join(QStringLiteral(" / ")).simplified().left(260));
        }
        {
            Warnings warnings;
            pick(r, QStringLiteral("Import Audio..."), empty);
            check(QStringLiteral("(ar) so is a WAV with nothing in it: "
                                 "refused, by name, with a reason"),
                  warnings.texts.size() == 1
                      && warnings.texts.value(0).contains(QStringLiteral("NOT imported"))
                      && warnings.texts.value(0).contains(QStringLiteral("empty.wav"))
                      && r.animatic->audioPath().isEmpty() && undo->count() == 0
                      && media(project).isEmpty(),
                  warnings.texts.join(QStringLiteral(" / ")).simplified().left(260));
        }
        {
            Warnings warnings;
            pick(r, QStringLiteral("Import Audio..."), good);
            check(QStringLiteral("(ar) control: a real WAV is imported, "
                                 "without a word, and is not marked as "
                                 "anything"),
                  warnings.texts.isEmpty() && present(r)
                      && sameFile(r.animatic->audioPath(), ownGood)
                      && !r.animatic->audioUnplayable()
                      && !r.animatic->audioWarningShownForTest(),
                  r.animatic->audioWarningForTest());
        }
        check(QStringLiteral("(ar) fixture: saved with that track"),
              r.window.saveProjectForTest(project));
    }

    // ---- a track whose file is there and cannot be played ------------------------
    // What an old project can hold, or what a file becomes when something
    // else overwrites it: the project names it, it exists, it is not audio.
    {
        QFile f(ownGood);
        check(QStringLiteral("(ar) fixture: the project's audio file is "
                             "overwritten with something that is not audio"),
              f.open(QIODevice::WriteOnly | QIODevice::Truncate)
                  && f.write(QByteArray(4096, 'x')) == 4096);
    }
    {
        Rig r(project);
        if (!r.ok) {
            check(QStringLiteral("(ar) the project re-opens"), false);
            return;
        }
        QUndoStack *undo = r.window.undoStackForTest();
        const bool noticed = waitFor([&] { return r.animatic->audioUnplayable(); });
        check(QStringLiteral("(ar) A TRACK THAT CANNOT BE PLAYED SAYS SO: the "
                             "project opens clean, still naming the file; "
                             "the bar reads CANNOT BE PLAYED with its name "
                             "(it used to show the name and no bar, for "
                             "ever), and the header label says it too"),
              noticed && !r.window.isDirty() && !r.animatic->audioMissing()
                  && sameFile(r.animatic->audioPath(), ownGood)
                  && r.timeline->audioBarStateForTest() == QStringLiteral("unplayable")
                  && r.timeline->audioBarTextForTest().contains(
                         QStringLiteral("CANNOT BE PLAYED"))
                  && r.timeline->audioBarTextForTest().contains(QStringLiteral("good.wav"))
                  && r.animatic->audioWarningShownForTest()
                  && r.animatic->audioWarningForTest().contains(QStringLiteral("cannot play")),
              r.timeline->audioBarStateForTest() + QStringLiteral(": ")
                  + r.timeline->audioBarTextForTest() + QStringLiteral(" / ")
                  + r.animatic->audioWarningForTest());
        check(QStringLiteral("(ar) ...with the player's reason and the "
                             "file's place in the label's tooltip"),
              !r.animatic->audioProblem().isEmpty()
                  && r.animatic->audioWarningForTest().contains(
                         r.animatic->audioProblem())
                  && r.animatic->audioWarningForTest().contains(
                         QStringLiteral("good.wav")),
              r.animatic->audioProblem());
        r.animatic->togglePlay();
        pump(150);
        r.animatic->advanceForTest();
        pump(100);
        check(QStringLiteral("(ar) the film plays, without sound, and the "
                             "audio player is not asked to play it"),
              r.animatic->isPlaying() && !r.animatic->audioPlayingForTest());
        r.animatic->leavePreview();
        r.animatic->setPanelDurationForTest(0, 0, 4);
        pump(200);
        const bool saved = r.window.saveProjectForTest(project);
        const QString named = manifestAudio(project);
        check(QStringLiteral("(ar) it is a display state, not a rewrite: a "
                             "save still names the file"),
              saved && named == QStringLiteral("Say_assets/good.wav"), named);
        pick(r, QStringLiteral("Import Audio..."), other);
        const bool replaced = present(r) && !r.animatic->audioUnplayable()
            && !r.animatic->audioWarningShownForTest();
        undo->undo();
        pump(250);
        const bool again = waitFor([&] { return r.animatic->audioUnplayable(); });
        check(QStringLiteral("(ar) importing a file that plays clears it; "
                             "undo returns to the track that cannot be "
                             "played, and it says so again"),
              replaced && again
                  && r.timeline->audioBarStateForTest() == QStringLiteral("unplayable"),
              r.timeline->audioBarTextForTest());
        undo->redo();
        pump(250);

        // ---- WHERE THE SOUND GOES ------------------------------------------------
        // The gate cannot change this machine's default audio device, so it
        // answers "what is the default now?" in the system's place
        // (setDefaultAudioOutputForTest) - with real devices of this
        // machine, or with none. What the app does with the answer is the
        // real path.
        const bool playable = present(r);
        const QAudioDevice real = QMediaDevices::defaultAudioOutput();
        const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
        if (!real.isNull()) {
            check(QStringLiteral("(ar) the volume slider's tooltip names the "
                                 "device the sound goes to - the system's "
                                 "default"),
                  playable && !r.animatic->noAudioOutput()
                      && r.animatic->audioOutputName() == real.description()
                      && r.animatic->volumeToolTipForTest().contains(
                             QStringLiteral("Playing through: ") + real.description()),
                  r.animatic->volumeToolTipForTest().simplified());
        } else {
            out() << "  NOTE (ar) this machine has no audio output device: the "
                     "checks that name a device were not run" << Qt::endl;
        }
        r.animatic->setDefaultAudioOutputForTest([] { return QAudioDevice(); });
        r.animatic->syncAudioOutputForTest();
        pump(100);
        const QString noOutputLabel = r.animatic->audioWarningForTest();
        r.animatic->togglePlay();
        pump(150);
        const bool filmPlays = r.animatic->isPlaying();
        r.animatic->leavePreview();
        check(QStringLiteral("(ar) NO OUTPUT DEVICE AT ALL is said on the "
                             "track: \"second.wav - no audio output\", with "
                             "what to do in both tooltips - and the film "
                             "still plays"),
              r.animatic->noAudioOutput() && r.animatic->audioWarningShownForTest()
                  && noOutputLabel.contains(QStringLiteral("second.wav"))
                  && noOutputLabel.contains(QStringLiteral("No audio output device"))
                  && r.animatic->audioWarningForTest().contains(
                         QStringLiteral("No audio output device"))
                  && r.animatic->volumeToolTipForTest().contains(
                         QStringLiteral("No audio output device"))
                  && filmPlays,
              noOutputLabel);
        if (!real.isNull()) {
            r.animatic->setDefaultAudioOutputForTest([real] { return real; });
            r.animatic->syncAudioOutputForTest();
            pump(100);
            check(QStringLiteral("(ar) a device arriving is taken up: the "
                                 "label is the plain name again and the "
                                 "tooltip names the device"),
                  !r.animatic->noAudioOutput()
                      && !r.animatic->audioWarningShownForTest()
                      && r.animatic->audioOutputName() == real.description(),
                  r.animatic->audioWarningForTest());
            QAudioDevice another;
            for (const QAudioDevice &d : outputs)
                if (d != real && another.isNull())
                    another = d;
            if (!another.isNull()) {
                r.animatic->setDefaultAudioOutputForTest([another] { return another; });
                r.animatic->syncAudioOutputForTest();
                pump(100);
                const bool followed =
                    r.animatic->audioOutputName() == another.description()
                    && r.animatic->volumeToolTipForTest().contains(another.description());
                // Back to the first - and this time NOBODY REPORTS IT: only
                // Play asks.
                r.animatic->setDefaultAudioOutputForTest([real] { return real; });
                const bool notYet =
                    r.animatic->audioOutputName() == another.description();
                r.animatic->togglePlay();
                pump(200);
                const bool atPlay = r.animatic->audioOutputName() == real.description();
                r.animatic->leavePreview();
                check(QStringLiteral("(ar) THE OUTPUT FOLLOWS THE SYSTEM'S "
                                     "DEFAULT DEVICE: when the default becomes "
                                     "another device the app moves to it (Qt "
                                     "alone stays on the old one - that was "
                                     "the silence), and a change nobody "
                                     "reported is still picked up at Play"),
                      followed && notYet && atPlay,
                      QStringLiteral("\"%1\" then \"%2\"; followed: %3, picked "
                                     "up at Play: %4")
                          .arg(real.description(), another.description())
                          .arg(followed).arg(atPlay));
            } else {
                out() << "  NOTE (ar) only one audio output on this machine: "
                         "the two-device check was not run" << Qt::endl;
            }
        }
        r.animatic->setDefaultAudioOutputForTest({});
        r.animatic->syncAudioOutputForTest();
    }
}

// ---- (as) the picture keeps step with the sound ------------------------------
// Measured 2026-10-05 with the app's own AnimaticPage, never shown: after an
// import with the playhead on panel 5 the sound started from the beginning
// of the file (Qt drops a seek asked before the file has loaded); a pause
// in the middle of a panel restarted that panel's time while the sound
// carried on; the 0.5x / 2x speeds moved the picture only; and the picture
// started on Play while the sound took up to a second to begin. Fixed
// 2026-10-16 (HANDOFF "The picture keeps step with the sound"). The gate
// cannot hear; it reads the player's position and the film's clock, which
// is what the probe did too.
void runAudioKeepsStepPass(const QString &scratch)
{
    using namespace workspace;
    using audio::waitFor;
    using audio::writeWav;
    out() << "--- (as) the picture keeps step with the sound: a seek before the "
             "file has loaded, resume after pause, the speeds, the wait for "
             "the sound ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/keepstep");
    QDir(base).removeRecursively();
    const QString wav = base + QStringLiteral("/source/track.wav");
    // One scene, three panels of 2, 3 and 4 s: panel 3 begins 5 s in.
    const QString project = writeProject(base + QStringLiteral("/p"),
                                         QStringLiteral("Step"), QSize(960, 540),
                                         24, 1, 3);
    check(QStringLiteral("(as) fixture: a project of three panels and a 12 s "
                         "WAV"),
          QFileInfo::exists(project) && writeWav(wav, 12.0));
    Rig r(project);
    check(QStringLiteral("(as) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    AnimaticPage *page = r.animatic;
    auto started = [&] {
        return waitFor([&] { return !page->waitingForAudioForTest(); }, 3000);
    };

    // ---- a seek asked before the file has loaded ----------------------------------
    clickClip(r, 2);
    pump(300);
    travel::pick(r, QStringLiteral("Import Audio..."), wav);
    const bool kept = page->audioWantedMsForTest() == 5000
        || page->audioPositionForTest() == 5000;
    const bool applied = waitFor([&] {
        return page->audioWantedMsForTest() < 0 && page->audioPositionForTest() == 5000;
    });
    check(QStringLiteral("(as) IMPORT WITH THE PLAYHEAD ON PANEL 3: the seek "
                         "to 5 s is kept while the file loads and applied "
                         "when it has (Qt alone dropped it, and the sound "
                         "started from the beginning)"),
          kept && applied,
          QStringLiteral("wanted %1, position %2")
              .arg(page->audioWantedMsForTest()).arg(page->audioPositionForTest()));
    page->togglePlay();
    const bool waited = page->waitingForAudioForTest()
        && page->elapsedMsInCurrentPanel() == 0;
    const bool began = started();
    pump(250);
    const qint64 audioA = page->audioPositionForTest();
    const int filmA = page->elapsedMsInCurrentPanel();
    check(QStringLiteral("(as) PLAY: the picture's clock waits for the sound "
                         "to begin, then runs from the sound's own "
                         "position - a quarter of a second in, the two are "
                         "within 250 ms"),
          waited && began && page->isPlaying() && page->audioPlayingForTest()
              && audioA >= 5000 && qAbs((audioA - 5000) - filmA) < 250,
          QStringLiteral("sound at %1, film %2 ms into panel 3").arg(audioA).arg(filmA));

    // ---- pause in the middle of a panel, then resume -------------------------------
    pump(500);
    page->togglePlay(); // pause
    const int atPause = page->elapsedMsInCurrentPanel();
    const qint64 audioAtPause = page->audioPositionForTest();
    pump(400);
    const bool frozen = page->elapsedMsInCurrentPanel() == atPause
        && !page->audioPlayingForTest();
    page->togglePlay(); // resume
    const bool resumed = started();
    pump(250);
    const qint64 audioB = page->audioPositionForTest();
    const int filmB = page->elapsedMsInCurrentPanel();
    check(QStringLiteral("(as) PAUSE IN THE MIDDLE OF A PANEL, THEN PLAY: the "
                         "panel's clock freezes and CONTINUES from there - "
                         "not from zero - and the sound is with it"),
          atPause >= 600 && frozen && resumed && filmB >= atPause
              && qAbs((audioB - 5000) - filmB) < 250
              && qAbs(audioAtPause - 5000 - atPause) < 300,
          QStringLiteral("paused at %1 ms (sound %2); after resume film %3, sound %4")
              .arg(atPause).arg(audioAtPause).arg(filmB).arg(audioB));

    // ---- the speeds -------------------------------------------------------------------
    page->togglePlay(); // pause
    pump(200);
    // The speed dropdown's 2x item (Part C; it was a button): the same
    // setPlaybackSpeed path the button took.
    for (QAction *a : page->findChildren<QAction *>())
        if (a->objectName() == QStringLiteral("animaticSpeed2x"))
            a->trigger();
    const int before2x = page->elapsedMsInCurrentPanel();
    page->togglePlay();
    const bool began2x = started();
    pump(500);
    const int gained = page->elapsedMsInCurrentPanel() - before2x;
    check(QStringLiteral("(as) AT 2x THE SOUND IS GIVEN THE SPEED TOO: the "
                         "player's rate is 2, and the picture's clock gains "
                         "about a second in half a second"),
          began2x && qFuzzyCompare(page->audioRateForTest(), 2.0) && gained > 700
              && gained < 1400,
          QStringLiteral("rate %1, gained %2 ms").arg(page->audioRateForTest()).arg(gained));
    page->togglePlay(); // pause
    for (QPushButton *b : page->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("1x"))
            b->click();
    page->leavePreview();
    pump(300);

    // ---- a seek with the file loaded is immediate (control) -----------------------------
    clickClip(r, 1);
    pump(200);
    check(QStringLiteral("(as) control: with the file loaded, selecting panel "
                         "2 seeks the sound to 2 s at once, nothing pending"),
          page->audioWantedMsForTest() < 0 && page->audioPositionForTest() == 2000,
          QStringLiteral("position %1").arg(page->audioPositionForTest()));

    // ---- the cap on the wait -----------------------------------------------------------
    page->setAudioNeverBeginsForTest(true);
    page->setAudioStartCapForTest(300);
    page->togglePlay();
    pump(150);
    const bool stillWaiting = page->waitingForAudioForTest()
        && page->elapsedMsInCurrentPanel() == 0 && page->isPlaying();
    pump(450);
    const bool startedAnyway = !page->waitingForAudioForTest()
        && page->elapsedMsInCurrentPanel() > 0;
    page->setAudioNeverBeginsForTest(false);
    page->setAudioStartCapForTest(-1);
    page->leavePreview();
    pump(200);
    check(QStringLiteral("(as) THE WAIT HAS A CAP: a sound that never begins "
                         "holds the picture for the cap (300 ms here, a "
                         "second in the app) and then the picture starts "
                         "anyway"),
          stillWaiting && startedAnyway);

    // ---- no sound: nothing to wait for ---------------------------------------------------
    if (QAction *removeAct = audio::editAction(r.window, QStringLiteral("Remove Audio")))
        removeAct->trigger();
    pump(200);
    page->togglePlay();
    const bool immediate = !page->waitingForAudioForTest() && page->isPlaying();
    pump(200);
    check(QStringLiteral("(as) with no track the picture starts at once"),
          immediate && page->elapsedMsInCurrentPanel() > 0);
    page->leavePreview();
    pump(200);
}

// ---- (at) the timeline follows the playhead -----------------------------------
// 2026-10-16: during playback the playhead ran off the right edge of the
// timeline and the view stayed where it was (nothing in the timeline read
// the playhead to scroll). Now the view follows: freely until the playhead
// reaches a follow point two-thirds across, then the strip scrolls under it;
// a scroll the user made is never undone by following; a jump moves the
// view only if the playhead would be off-screen, and puts it a third across.
// Nothing here depends on the real pointer, the cursor or the active window:
// the playhead is moved with advanceForTest, the user's scroll with
// userScrollForTest, and the pointer is already shut out.
void runTimelineFollowsPass(const QString &scratch)
{
    using namespace workspace;
    using audio::waitFor;
    using audio::writeWav;
    out() << "--- (at) the timeline follows the playhead: the follow point, "
             "the user's scroll, the jump rule, the stopped case ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/follow");
    QDir(base).removeRecursively();
    const QString wav = base + QStringLiteral("/source/track.wav");
    // One scene of 24 panels (2, 3, 4 s cycling): a 72 s film, 6912 px at
    // the 400 % zoom, in a view about 1200 px wide.
    const QString project = writeProject(base + QStringLiteral("/p"),
                                         QStringLiteral("Follow"), QSize(960, 540),
                                         24, 1, 24);
    check(QStringLiteral("(at) fixture: a 72 s film of 24 panels and an 80 s WAV"),
          QFileInfo::exists(project) && writeWav(wav, 80.0));
    Rig r(project);
    check(QStringLiteral("(at) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    AnimaticPage *page = r.animatic;
    AnimaticTimeline *tl = r.timeline;
    travel::pick(r, QStringLiteral("Import Audio..."), wav);
    travel::present(r);
    // The zoom slider lives in the page's header since the redesign
    // (2026-10-18); it is still the timeline's, found by its name.
    ZoomSetter zoomSetter{r.timeline};
    ZoomSetter *zoom = &zoomSetter;
    QScrollBar *bar = tl->findChild<QScrollBar *>();
    if (zoom)
        zoom->setValue(400);
    pump(150);
    const int vw = tl->visibleWidthForTest();
    const int fp = tl->followPointForTest();
    const int labelCol = fp - vw * 2 / 3; // followPoint = labelCol + 2/3 of the view
    const int rightEdge = labelCol + vw;
    check(QStringLiteral("(at) at 400 % the film overflows the view and the "
                         "view starts at the beginning"),
          bar && bar->maximum() > 2000 && tl->scrollXForTest() == 0 && vw > 600,
          QStringLiteral("view %1 px, content beyond it %2 px, follow point at %3")
              .arg(vw).arg(bar ? bar->maximum() : -1).arg(fp));
    auto started = [&] {
        return waitFor([&] { return !page->waitingForAudioForTest(); }, 3000);
    };
    auto advanceTo = [&](int panel) {
        while (page->currentFlatIndex() < panel && page->isPlaying()) {
            page->advanceForTest();
            pump(40);
        }
        pump(60);
    };

    // ---- positive control: following off -----------------------------------------
    clickClip(r, 0);
    pump(200);
    tl->setFollowForTest(false);
    page->togglePlay();
    started();
    advanceTo(12);
    check(QStringLiteral("(at) CONTROL, following off: the playhead runs off "
                         "the right edge and the view does not move - so the "
                         "checks below can see the failure"),
          tl->playheadScreenXForTest() > rightEdge && tl->scrollXForTest() == 0,
          QStringLiteral("playhead at x %1, right edge %2, scroll %3")
              .arg(tl->playheadScreenXForTest()).arg(rightEdge).arg(tl->scrollXForTest()));
    page->leavePreview();
    pump(200);
    tl->setFollowForTest(true);

    // ---- following on ----------------------------------------------------------------
    clickClip(r, 0);
    pump(200);
    int requests = 0, scrubs = 0;
    QObject::connect(page, &AnimaticPage::panelSelectRequested, page, [&] { ++requests; });
    QObject::connect(tl, &AnimaticTimeline::playheadScrubbed, tl, [&] { ++scrubs; });
    const int selectCalls = r.storyboard->selectPanelCallsForTest();
    page->togglePlay();
    started();
    const bool timerRuns = tl->followTimerActiveForTest();
    QElapsedTimer clock;
    clock.start();
    const qint64 audioAtStart = page->audioPositionForTest();
    advanceTo(6); // 18 s = 1728 px: well past the follow point
    QList<int> held;
    for (int i = 0; i < 8; ++i) {
        pump(40);
        held << tl->playheadScreenXForTest();
    }
    bool holds = true;
    for (int x : held)
        holds = holds && qAbs(x - fp) <= 1;
    check(QStringLiteral("(at) FOLLOWING: the follow timer runs; once the "
                         "playhead reaches the follow point the view scrolls "
                         "and the playhead HOLDS there within 1 px (eight "
                         "readings)"),
          timerRuns && holds && tl->scrollXForTest() > 0,
          QStringLiteral("follow point %1; readings %2; scroll %3").arg(fp)
              .arg([&] { QStringList s; for (int x : held) s << QString::number(x); return s.join(QLatin1Char(',')); }())
              .arg(tl->scrollXForTest()));
    const qint64 audioNow = page->audioPositionForTest();
    const qint64 expected = audioAtStart + clock.elapsed();
    check(QStringLiteral("(at) scrolling issued NO SEEK (the sound ran on by "
                         "the clock, not to a panel), selected nothing and "
                         "scrubbed nothing"),
          qAbs(audioNow - expected) < 400 && requests == 0 && scrubs == 0
              && r.storyboard->selectPanelCallsForTest() == selectCalls
              && tl->selectedPanelForTest() == 0,
          QStringLiteral("sound at %1, by the clock %2; requests %3, scrubs %4")
              .arg(audioNow).arg(expected).arg(requests).arg(scrubs));
    advanceTo(23);
    // The last panel is 4 s = 384 px; the strip reaches its end when the
    // playhead is within a third of the view (349 px) of the content's end,
    // so let the last panel run for a moment.
    pump(1500);
    const int scrollAtEnd = tl->scrollXForTest();
    const int playheadAtEnd = tl->playheadScreenXForTest();
    page->advanceForTest(); // the last panel's time is up: the film ends
    pump(300);
    check(QStringLiteral("(at) near the end the strip STOPS at its end and "
                         "the playhead runs on past the follow point; the "
                         "end of the film stops the follow timer, and the "
                         "playhead's return to panel 1 - off-screen by then - "
                         "brings the view back to it"),
          bar && scrollAtEnd == bar->maximum() && playheadAtEnd > fp
              && !page->isPlaying() && !tl->followTimerActiveForTest()
              && page->currentFlatIndex() == 0 && tl->scrollXForTest() == 0
              && tl->playheadScreenXForTest() == labelCol,
          QStringLiteral("scroll at the end %1 of %2, playhead x %3; after: scroll %4, "
                         "playhead x %5")
              .arg(scrollAtEnd).arg(bar ? bar->maximum() : -1).arg(playheadAtEnd)
              .arg(tl->scrollXForTest()).arg(tl->playheadScreenXForTest()));

    // ---- the timer stops on Pause, a scrub, and Stop ------------------------------------
    page->togglePlay();
    started();
    const bool onPlay = tl->followTimerActiveForTest();
    page->togglePlay(); // pause
    pump(50);
    const bool offPause = !tl->followTimerActiveForTest();
    page->togglePlay();
    started();
    page->scrubForTest(3);
    pump(50);
    const bool offScrub = !tl->followTimerActiveForTest() && !page->isPlaying();
    page->togglePlay();
    started();
    page->leavePreview(); // Stop
    pump(50);
    const bool offStop = !tl->followTimerActiveForTest();
    check(QStringLiteral("(at) the follow timer runs on Play and is stopped "
                         "after Pause, a scrub, and Stop"),
          onPlay && offPause && offScrub && offStop);

    // ---- the user's scroll: ahead ---------------------------------------------------------
    clickClip(r, 0);
    pump(200);
    page->togglePlay();
    started();
    const int ahead = bar ? bar->maximum() / 2 : 0;
    tl->userScrollForTest(ahead); // the playhead is now far LEFT of the view
    advanceTo(2);
    const bool leftAlone = tl->scrollXForTest() == ahead;
    advanceTo(16); // 16 panels = 48 s = 4608 px: past the follow point of that view
    const bool resumed = tl->scrollXForTest() > ahead
        && qAbs(tl->playheadScreenXForTest() - fp) <= 1;
    check(QStringLiteral("(at) MY SCROLL AHEAD: the view stays where I put it "
                         "while the playhead is left of the follow point, "
                         "and following resumes when the playhead reaches it"),
          leftAlone && resumed,
          QStringLiteral("put at %1; after 2 panels %2; later %3, playhead x %4")
              .arg(ahead).arg(leftAlone ? ahead : tl->scrollXForTest())
              .arg(tl->scrollXForTest()).arg(tl->playheadScreenXForTest()));

    // ---- the user's scroll: back, playhead right of the follow point, in view ---------------
    const int following = tl->scrollXForTest();
    tl->userScrollForTest(following - 150); // the playhead sits 150 px right of the point
    const bool inViewAfter = tl->playheadScreenXForTest() > fp
        && tl->playheadScreenXForTest() <= rightEdge;
    advanceTo(page->currentFlatIndex() + 2);
    const bool heldInView = tl->scrollXForTest() == following - 150;
    page->togglePlay(); // pause: the hold stands
    pump(50);
    const bool heldThroughPause = tl->scrollXForTest() == following - 150;
    page->togglePlay(); // Play: the jump rule, applied in this call
    // Read NOW: once the sound begins the playhead moves on from the third
    // (Release read it 12 px along after waiting for the sound).
    const int jumpedTo = tl->playheadScreenXForTest();
    started();
    const bool jumped = qAbs(jumpedTo - (labelCol + vw / 3)) <= 1;
    check(QStringLiteral("(at) MY SCROLL BACK with the playhead right of the "
                         "follow point, in view: the view stays where I left "
                         "it - through two panels and a pause - and the next "
                         "Play puts the playhead a third across"),
          inViewAfter && heldInView && heldThroughPause && jumped,
          QStringLiteral("in view after the scroll %1; held %2 / %3; after Play "
                         "playhead x %4 (a third across is %5)")
              .arg(inViewAfter).arg(heldInView).arg(heldThroughPause)
              .arg(jumpedTo).arg(labelCol + vw / 3));

    // ---- the user's scroll: back, playhead OFF the right edge -------------------------------
    const int following2 = tl->scrollXForTest();
    tl->userScrollForTest(following2 - vw); // a whole view back: off the right edge
    const bool offRight = tl->playheadScreenXForTest() > rightEdge;
    advanceTo(page->currentFlatIndex() + 1);
    const bool heldOff = tl->scrollXForTest() == following2 - vw;
    page->togglePlay();
    pump(50);
    page->togglePlay();
    const int jumpedTo2 = tl->playheadScreenXForTest(); // read now, as above
    started();
    const bool jumped2 = qAbs(jumpedTo2 - (labelCol + vw / 3)) <= 1;
    check(QStringLiteral("(at) MY SCROLL BACK with the playhead off the right "
                         "edge: held until the next Play, which repositions"),
          offRight && heldOff && jumped2,
          QStringLiteral("off right %1, held %2, after Play playhead x %3")
              .arg(offRight).arg(heldOff).arg(jumpedTo2));
    page->leavePreview();
    pump(200);

    // ---- stopped: End, Home, the arrows, and a Stop that sends the playhead off-screen ---
    page->goFirst();
    pump(200);
    tl->setFollowForTest(false);
    page->goLast(); // through the workspace's selection, as the End key goes
    pump(200);
    const bool endOffScreenWithoutFollow = tl->playheadScreenXForTest() > rightEdge
        && tl->scrollXForTest() == 0;
    tl->setFollowForTest(true);
    page->goFirst();
    pump(200);
    page->goLast();
    pump(200);
    // The last panel starts 384 px before the content's end, and a third of
    // the view is 349 px: the view cannot put it a third across without
    // running past the content, so the jump clamps to the end of the strip
    // and the playhead is a little right of a third.
    const int endX = tl->playheadScreenXForTest();
    const bool endInView = endX >= labelCol + vw / 3 && endX <= rightEdge
        && bar && tl->scrollXForTest() == bar->maximum();
    check(QStringLiteral("(at) STOPPED: End on a long film brings the playhead "
                         "into view - the strip at its end, the playhead at a "
                         "third across or as near as the end allows (control: "
                         "with following off it stays off-screen)"),
          endOffScreenWithoutFollow && endInView,
          QStringLiteral("without following x %1; with it x %2")
              .arg(endOffScreenWithoutFollow).arg(tl->playheadScreenXForTest()));
    page->goFirst();
    pump(200);
    const bool homeShowsStart = tl->scrollXForTest() == 0
        && tl->playheadScreenXForTest() == labelCol;
    page->goNext();
    pump(200);
    const bool arrowLeavesView = tl->scrollXForTest() == 0
        && tl->playheadScreenXForTest() > labelCol
        && tl->playheadScreenXForTest() < rightEdge;
    page->goPrev();
    pump(200);
    const bool backLeavesView = tl->scrollXForTest() == 0;
    check(QStringLiteral("(at) Home shows the start; the arrows inside the "
                         "view leave the offset unchanged"),
          homeShowsStart && arrowLeavesView && backLeavesView);
    // Stop with the playhead's home off-screen: play far along, then Stop.
    page->togglePlay();
    started();
    advanceTo(14);
    const bool farAlong = tl->scrollXForTest() > vw;
    page->leavePreview(); // Stop: the playhead goes home to panel 1
    pump(200);
    check(QStringLiteral("(at) a Stop that returns the playhead to a panel "
                         "off-screen shows it"),
          farAlong && page->currentFlatIndex() == 0 && tl->scrollXForTest() == 0
              && tl->playheadScreenXForTest() == labelCol,
          QStringLiteral("scroll %1, playhead x %2").arg(tl->scrollXForTest())
              .arg(tl->playheadScreenXForTest()));

    // ---- the whole film fits -------------------------------------------------------------
    for (QPushButton *b : r.animatic->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Fit"))
            b->click();
    pump(150);
    const bool fits = bar && bar->maximum() == 0;
    page->togglePlay();
    started();
    advanceTo(20);
    // What the Dev Recorder's statePoll carries about the view, read while
    // it plays: the same numbers the checks above read.
    const QVariantMap rec = tl->devrecState();
    const int playheadXNow = tl->playheadScreenXForTest(); // read with the map
    // Does a map agree with the view? One function, so the positive control
    // below runs the same comparison over a map with one field changed.
    auto agrees = [&](const QVariantMap &m) {
        // Fit keeps the exact zoom (60.59 here) and shows the slider the
        // nearest whole number (61): within one of the slider.
        return qAbs(m.value(QStringLiteral("timeline.zoom")).toDouble()
                    - (zoom ? zoom->value() : -1)) < 1.0
            && m.value(QStringLiteral("timeline.scrollX")).toInt() == tl->scrollXForTest()
            && m.value(QStringLiteral("timeline.scrollMax")).toInt() == (bar ? bar->maximum() : -1)
            && m.value(QStringLiteral("timeline.visibleWidth")).toInt() == vw
            && m.value(QStringLiteral("timeline.playheadX")).toInt() == playheadXNow
            && m.value(QStringLiteral("timeline.playheadFrame")).toInt() > 0
            && m.value(QStringLiteral("timeline.followPoint")).toInt() == fp
            && m.value(QStringLiteral("timeline.playing")).toBool()
            && m.value(QStringLiteral("timeline.follow")).toBool()
            && m.value(QStringLiteral("timeline.followTimer")).toBool()
            && !m.value(QStringLiteral("timeline.viewHeld")).toBool();
    };
    check(QStringLiteral("(at) the Dev Recorder's state carries the view: "
                         "zoom, scroll offset and range, visible width, "
                         "playhead frame and x, follow point, playing and "
                         "following - and they agree with the view"),
          agrees(rec),
          QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(rec)).toJson(QJsonDocument::Compact)));
    // POSITIVE CONTROL: the same comparison must SEE a field that disagrees
    // - each of four fields changed by one, in turn, and the whole map with
    // the playhead's x off by a pixel.
    int blind = 0;
    for (const char *key : {"timeline.scrollX", "timeline.visibleWidth",
                            "timeline.playheadX", "timeline.followPoint"}) {
        QVariantMap altered = rec;
        altered.insert(QLatin1String(key), altered.value(QLatin1String(key)).toInt() + 1);
        if (agrees(altered))
            ++blind;
    }
    QVariantMap heldMap = rec;
    heldMap.insert(QStringLiteral("timeline.viewHeld"), true);
    if (agrees(heldMap))
        ++blind;
    check(QStringLiteral("(at) control: the same comparison FAILS a map in "
                         "which one field disagrees with the view by a pixel "
                         "(scroll, width, playhead x, follow point) or says "
                         "the view is held when it is not"),
          blind == 0, QStringLiteral("%1 altered map(s) passed").arg(blind));
    check(QStringLiteral("(at) when the whole film fits, nothing moves"),
          fits && tl->scrollXForTest() == 0 && tl->playheadScreenXForTest() < rightEdge,
          QStringLiteral("scroll range %1, scroll %2, playhead x %3, right edge %4, zoom %5")
              .arg(bar ? bar->maximum() : -1).arg(tl->scrollXForTest())
              .arg(tl->playheadScreenXForTest()).arg(rightEdge)
              .arg(zoom ? zoom->value() : -1));
    page->leavePreview();
    pump(200);
}

// ---- (au) "my scroll" is only what the user does to the scrollbar ------------
// 2026-10-17, found by the Dev Recorder's new timeline fields: zooming OUT
// during playback set viewHeld and stopped following. The zoom shrank the
// scroll range, Qt's scrollbar clamped its own value, and the clamp's
// valueChanged was read as the user's scroll. The rule the user approved:
// "my scroll" is a drag, the wheel over the bar, its arrow buttons, or a
// click on its track - and a change the program makes to the scrollbar
// must never set viewHeld. These checks drive each with synthetic events
// sent straight to the scrollbar (never the real pointer), and each
// programmatic path with the calls the app makes.
namespace scrollbar {

QStyleOptionSlider optionFor(QScrollBar *bar)
{
    QStyleOptionSlider opt;
    opt.initFrom(bar);
    opt.subControls = QStyle::SC_All;
    opt.activeSubControls = QStyle::SC_None;
    opt.orientation = bar->orientation();
    opt.minimum = bar->minimum();
    opt.maximum = bar->maximum();
    opt.sliderPosition = bar->sliderPosition();
    opt.sliderValue = bar->value();
    opt.singleStep = bar->singleStep();
    opt.pageStep = bar->pageStep();
    opt.upsideDown = bar->invertedAppearance();
    if (bar->orientation() == Qt::Horizontal)
        opt.state |= QStyle::State_Horizontal;
    return opt;
}

QRect partRect(QScrollBar *bar, QStyle::SubControl part)
{
    const QStyleOptionSlider opt = optionFor(bar);
    return bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt, part, bar);
}

// A real drag: press on the handle, move left by `px`, release.
void drag(QScrollBar *bar, int px)
{
    const QRect handle = partRect(bar, QStyle::SC_ScrollBarSlider);
    const QPointF start = QRectF(handle).center();
    sendMouse(bar, QEvent::MouseButtonPress, start, Qt::LeftButton);
    pump(30);
    sendMouse(bar, QEvent::MouseMove, start - QPointF(px / 2.0, 0), Qt::LeftButton);
    pump(30);
    sendMouse(bar, QEvent::MouseMove, start - QPointF(px, 0), Qt::LeftButton);
    pump(30);
    sendMouse(bar, QEvent::MouseButtonRelease, start - QPointF(px, 0), Qt::LeftButton);
    pump(60);
}

// The wheel over the bar, in the direction that scrolls LEFT. Measured on
// this horizontal bar: an event with BOTH axes set moved nothing (the two
// earlier readings of "+12" were following's own motion during the wait);
// Y alone at -120 moved the view RIGHT by 864 px over three notches; so
// +120 on Y, X at zero, is "left".
void wheelLeft(QScrollBar *bar, int notches)
{
    const QRect handle = partRect(bar, QStyle::SC_ScrollBarSlider);
    const QPointF at = QRectF(handle).center();
    for (int i = 0; i < notches; ++i) {
        QWheelEvent ev(at, bar->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(bar, &ev);
        pump(20);
    }
    pump(60);
}

// A click on a part of the bar (the track left of the handle, or the left
// arrow button if the style has one).
void click(QScrollBar *bar, const QPointF &at)
{
    sendMouse(bar, QEvent::MouseButtonPress, at, Qt::LeftButton);
    pump(30);
    sendMouse(bar, QEvent::MouseButtonRelease, at, Qt::LeftButton);
    pump(60);
}

} // namespace scrollbar

void runScrollbarOwnershipPass(const QString &scratch)
{
    using namespace workspace;
    using audio::waitFor;
    out() << "--- (au) \"my scroll\" is only what the user does to the scrollbar: "
             "programmatic changes never hold the view; a drag, the wheel, a track "
             "click do ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/scrollown");
    QDir(base).removeRecursively();
    const QString project = writeProject(base + QStringLiteral("/p"),
                                         QStringLiteral("Own"), QSize(960, 540),
                                         24, 1, 24);
    // 1400 wide, as this section was written: its follow-point arithmetic
    // (the playhead past the follow point after a zoom-out at 200 %) holds
    // for this width; the window grows to its content minimum (~1492).
    Rig r(project, QSize(1400, 900));
    check(QStringLiteral("(au) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    AnimaticPage *page = r.animatic;
    AnimaticTimeline *tl = r.timeline;
    // The zoom slider lives in the page's header since the redesign
    // (2026-10-18); it is still the timeline's, found by its name.
    ZoomSetter zoomSetter{r.timeline};
    ZoomSetter *zoom = &zoomSetter;
    QScrollBar *bar = tl->findChild<QScrollBar *>();
    if (!zoom || !bar) {
        check(QStringLiteral("(au) the timeline's zoom slider and scrollbar were found"),
              false);
        return;
    }
    auto started = [&] {
        return waitFor([&] { return !page->waitingForAudioForTest(); }, 3000);
    };
    auto fp = [&] { return tl->followPointForTest(); };
    auto labelCol = [&] { return fp() - tl->visibleWidthForTest() * 2 / 3; };
    auto rightEdge = [&] { return labelCol() + tl->visibleWidthForTest(); };
    auto inView = [&] {
        const int x = tl->playheadScreenXForTest();
        return x >= labelCol() && x <= rightEdge();
    };
    // Play from the start at 400 % and run until following holds the
    // playhead on the follow point. Every case begins from here.
    auto heldAtFollowPoint = [&]() -> bool {
        page->leavePreview();
        pump(150);
        zoom->setValue(400);
        pump(100);
        clickClip(r, 0);
        pump(150);
        page->togglePlay();
        if (!started())
            return false;
        while (page->currentFlatIndex() < 6 && page->isPlaying()) {
            page->advanceForTest();
            pump(40);
        }
        pump(80);
        return qAbs(tl->playheadScreenXForTest() - fp()) <= 1 && tl->scrollXForTest() > 0
            && page->isPlaying();
    };
    // What a programmatic change must leave: not held, the playhead in
    // view, and - while there is still room to scroll - on the follow
    // point once following has had a tick.
    auto settledRight = [&](QString *why) {
        pump(80);
        const bool room = bar->maximum() > 0 && tl->scrollXForTest() < bar->maximum();
        const bool onPoint = qAbs(tl->playheadScreenXForTest() - fp()) <= 1;
        const bool ok = !tl->viewHeldForTest() && inView() && (!room || onPoint);
        *why = QStringLiteral("held %1, playhead x %2 (follow point %3, right edge %4), "
                              "scroll %5/%6, zoom %7, playing %8")
                   .arg(tl->viewHeldForTest()).arg(tl->playheadScreenXForTest()).arg(fp())
                   .arg(rightEdge()).arg(tl->scrollXForTest()).arg(bar->maximum())
                   .arg(zoom->value()).arg(page->isPlaying());
        return ok && page->isPlaying();
    };
    QString why;

    // ---- programmatic changes during playback --------------------------------------
    check(QStringLiteral("(au) fixture: playing at 400 %, held on the follow point"),
          heldAtFollowPoint());
    zoom->setValue(200); // ZOOM OUT: the range shrinks, Qt clamps the value
    check(QStringLiteral("(au) ZOOM OUT during playback: not held, the playhead "
                         "in view and back on the follow point (the recording of "
                         "2026-10-17 showed this holding the view)"),
          settledRight(&why), why);
    zoom->setValue(300); // zoom in
    check(QStringLiteral("(au) zoom IN during playback: the same"), settledRight(&why), why);
    for (QPushButton *b : r.animatic->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Fit"))
            b->click();
    check(QStringLiteral("(au) FIT during playback: not held, playhead in view "
                         "(no room left to scroll)"),
          settledRight(&why), why);
    check(QStringLiteral("(au) fixture: held on the follow point again"), heldAtFollowPoint());
    const QSize before = r.window.size();
    r.window.resize(before.width() - 300, before.height()); // the view narrows
    pump(250);
    check(QStringLiteral("(au) a NARROWER TIMELINE during playback (window "
                         "resized; the splitter and collapse reach the same "
                         "canvasResized): not held, playhead in view, on the "
                         "new follow point"),
          settledRight(&why), why);
    r.window.resize(before);
    pump(250);
    check(QStringLiteral("(au) ...and back to the old width: the same"),
          settledRight(&why), why);
    // A panel BEFORE the playhead lengthened (the structure changes and
    // the range with it), and its undo.
    // A timing edit PAUSES the film before it reflows the blocks (measured:
    // playing was false after the command), so "during playback" is not
    // possible here. The user's rule for paused or stopped: a change of
    // size moves nothing - the reflow pushes the playhead off the right
    // edge (x 1368 of 1137, measured) and the view stays; the next Play
    // brings it back by the jump rule.
    QUndoStack *undo = r.window.undoStackForTest();
    const int undoCount = undo->count();
    const int scrollBeforeEdit = tl->scrollXForTest();
    page->setPanelDurationForTest(0, 1, 9);
    pump(80);
    auto stayedPut = [&](QString *w) {
        const bool ok = !tl->viewHeldForTest() && !page->isPlaying()
            && tl->scrollXForTest() == scrollBeforeEdit;
        *w = QStringLiteral("held %1, playhead x %2 (right edge %3), scroll %4 (was %5), "
                            "range %6, playing %7")
                 .arg(tl->viewHeldForTest()).arg(tl->playheadScreenXForTest())
                 .arg(rightEdge()).arg(tl->scrollXForTest()).arg(scrollBeforeEdit)
                 .arg(bar->maximum()).arg(page->isPlaying());
        return ok;
    };
    check(QStringLiteral("(au) a PANEL LENGTHENED by a timing command (which "
                         "pauses the film and rebuilds the blocks): PAUSED, A "
                         "CHANGE OF SIZE MOVES NOTHING - the view's offset is "
                         "unchanged even with the playhead pushed off the "
                         "right edge, and not held"),
          stayedPut(&why) && undo->count() == undoCount + 1
              && tl->playheadScreenXForTest() > rightEdge(),
          why);
    undo->undo();
    pump(80);
    check(QStringLiteral("(au) ...and its UNDO: the same, offset unchanged"),
          stayedPut(&why), why);
    page->togglePlay();
    const int afterPlay = tl->playheadScreenXForTest();
    started();
    check(QStringLiteral("(au) ...and the next Play brings the playhead back "
                         "by the jump rule (a third across) - it was in view "
                         "after the undo, so here nothing moves unless it was "
                         "off-screen"),
          afterPlay >= labelCol() && afterPlay <= rightEdge(),
          QStringLiteral("playhead x %1 at Play (view %2..%3)").arg(afterPlay)
              .arg(labelCol()).arg(rightEdge()));
    check(QStringLiteral("(au) fixture: held on the follow point"), heldAtFollowPoint());
    bar->setSliderPosition(qMax(0, bar->value() - 200)); // what a future programmer might write
    check(QStringLiteral("(au) a programmatic setSliderPosition during playback "
                         "(which Qt reports with the same signal as a wheel or a "
                         "drag) is NOT the user's: not held, and following takes "
                         "the playhead back to the follow point"),
          settledRight(&why), why);

    // ---- the user's actions, each a positive control for the above -----------------------
    check(QStringLiteral("(au) fixture: held on the follow point"), heldAtFollowPoint());
    scrollbar::drag(bar, 400);
    check(QStringLiteral("(au) A DRAG of the handle to the left holds the view: "
                         "viewHeld true, the playhead right of the follow point, "
                         "and the view does not come back while playing"),
          tl->viewHeldForTest() && tl->playheadScreenXForTest() > fp()
              && [&] { const int s = tl->scrollXForTest(); pump(300); return tl->scrollXForTest() == s; }(),
          QStringLiteral("held %1, playhead x %2, scroll %3").arg(tl->viewHeldForTest())
              .arg(tl->playheadScreenXForTest()).arg(tl->scrollXForTest()));
    // ---- a hold survives a change of size -------------------------------------------
    // Only Play, a scrub, a seek or a loop restart end a hold: zoom and
    // resize while held must leave viewHeld true and must not re-anchor.
    check(QStringLiteral("(au) fixture: held on the follow point"), heldAtFollowPoint());
    scrollbar::drag(bar, 400);
    const bool heldFirst = tl->viewHeldForTest();
    const int heldScroll = tl->scrollXForTest();
    zoom->setValue(200); // zoom out while held: the range shrinks
    pump(80);
    const bool heldAfterZoom = tl->viewHeldForTest();
    const int scrollAfterZoom = tl->scrollXForTest();
    const bool notAnchoredZoom = scrollAfterZoom == qMin(heldScroll, bar->maximum());
    r.window.resize(before.width() - 300, before.height());
    pump(250);
    const bool heldAfterResize = tl->viewHeldForTest();
    const int scrollAfterResize = tl->scrollXForTest();
    const bool notAnchoredResize = scrollAfterResize == qMin(scrollAfterZoom, bar->maximum());
    r.window.resize(before);
    pump(250);
    const bool heldAfterRestore = tl->viewHeldForTest();
    check(QStringLiteral("(au) A HOLD SURVIVES A CHANGE OF SIZE: after a drag "
                         "back, a zoom-out and a window resize leave viewHeld "
                         "true and move the offset only by the range's clamp - "
                         "no re-anchoring"),
          heldFirst && heldAfterZoom && notAnchoredZoom && heldAfterResize
              && notAnchoredResize && heldAfterRestore,
          QStringLiteral("held %1 -> zoom %2 (scroll %3 -> %4, max %5) -> resize %6 "
                         "(scroll %7) -> restore %8")
              .arg(heldFirst).arg(heldAfterZoom).arg(heldScroll).arg(scrollAfterZoom)
              .arg(bar->maximum()).arg(heldAfterResize).arg(scrollAfterResize)
              .arg(heldAfterRestore));
    check(QStringLiteral("(au) fixture: held on the follow point"), heldAtFollowPoint());
    const int beforeWheel = tl->scrollXForTest();
    scrollbar::wheelLeft(bar, 3);
    check(QStringLiteral("(au) THE WHEEL over the scrollbar, scrolling left, holds "
                         "the view (control: the wheel moved it)"),
          tl->scrollXForTest() < beforeWheel && tl->viewHeldForTest()
              && tl->playheadScreenXForTest() > fp(),
          QStringLiteral("scroll %1 -> %2, held %3").arg(beforeWheel)
              .arg(tl->scrollXForTest()).arg(tl->viewHeldForTest()));
    check(QStringLiteral("(au) fixture: held on the follow point"), heldAtFollowPoint());
    const QRect track = scrollbar::partRect(bar, QStyle::SC_ScrollBarSubPage);
    const int beforeTrack = tl->scrollXForTest();
    scrollbar::click(bar, QPointF(track.left() + 2, track.center().y()));
    check(QStringLiteral("(au) A CLICK ON THE TRACK left of the handle (a page "
                         "step back) holds the view (control: it moved)"),
          !track.isEmpty() && tl->scrollXForTest() < beforeTrack && tl->viewHeldForTest(),
          QStringLiteral("track %1x%2 at %3; scroll %4 -> %5, held %6").arg(track.width())
              .arg(track.height()).arg(track.left()).arg(beforeTrack)
              .arg(tl->scrollXForTest()).arg(tl->viewHeldForTest()));
    const QRect arrow = scrollbar::partRect(bar, QStyle::SC_ScrollBarSubLine);
    // The style gives add-line / sub-line "width: 0; height: 0". A rect a
    // few pixels wide is the style's placeholder, not a button a person
    // could press: say so rather than click the track beside it and call
    // it the arrow.
    if (arrow.width() < 8 || arrow.height() < 8) {
        out() << "  NOTE (au) this scrollbar style has NO ARROW BUTTONS (sub-line "
                 "rect " << arrow.width() << "x" << arrow.height() << " at x "
              << arrow.left() << "): the left-arrow check cannot be run and is not "
                 "pretended." << Qt::endl;
    } else {
        check(QStringLiteral("(au) fixture: held on the follow point"), heldAtFollowPoint());
        const int beforeArrow = tl->scrollXForTest();
        scrollbar::click(bar, QRectF(arrow).center());
        check(QStringLiteral("(au) THE LEFT ARROW BUTTON (the style gives it a "
                             "rect, see the detail) holds the view: a single "
                             "step back"),
              tl->scrollXForTest() < beforeArrow && tl->viewHeldForTest(),
              QStringLiteral("sub-line rect %1x%2 at x %3; scroll %4 -> %5, held %6")
                  .arg(arrow.width()).arg(arrow.height()).arg(arrow.left())
                  .arg(beforeArrow).arg(tl->scrollXForTest()).arg(tl->viewHeldForTest()));
    }
    page->leavePreview();
    pump(200);
}

// ---- (av) the wheel over the timeline ---------------------------------------
// 2026-10-17: in three recordings the user's wheel went over the tracks,
// where it did nothing. Now a plain or Shift+wheel there scrolls sideways
// (routed through the scrollbar, so it is "my scroll" for the hold rule),
// and Ctrl+wheel zooms around the pointer and never holds. Synthetic wheel
// events sent straight to the canvas; never the real pointer.
namespace {
void wheelOnCanvas(QWidget *canvas, const QPointF &at, int angleY, Qt::KeyboardModifiers mods)
{
    QWheelEvent ev(at, canvas->mapToGlobal(at.toPoint()), QPoint(), QPoint(0, angleY),
                   Qt::NoButton, mods, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(canvas, &ev);
    pump(40);
}
} // namespace

void runTimelineWheelPass(const QString &scratch)
{
    using namespace workspace;
    using audio::waitFor;
    out() << "--- (av) the wheel over the timeline: sideways by a named step as the "
             "user's scroll; Ctrl+wheel zooms around the pointer ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/wheel");
    QDir(base).removeRecursively();
    const QString project = writeProject(base + QStringLiteral("/p"),
                                         QStringLiteral("Wheel"), QSize(960, 540),
                                         24, 1, 24);
    Rig r(project);
    check(QStringLiteral("(av) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    AnimaticPage *page = r.animatic;
    AnimaticTimeline *tl = r.timeline;
    QWidget *canvas = tl->surfaceForTest();
    // The zoom slider lives in the page's header since the redesign
    // (2026-10-18); it is still the timeline's, found by its name.
    ZoomSetter zoomSetter{r.timeline};
    ZoomSetter *zoom = &zoomSetter;
    QScrollBar *bar = tl->findChild<QScrollBar *>();
    if (!zoom || !bar || !canvas) {
        check(QStringLiteral("(av) the timeline's parts were found"), false);
        return;
    }
    auto started = [&] {
        return waitFor([&] { return !page->waitingForAudioForTest(); }, 3000);
    };
    auto fp = [&] { return tl->followPointForTest(); };
    auto labelCol = [&] { return fp() - tl->visibleWidthForTest() * 2 / 3; };
    auto heldAtFollowPoint = [&]() -> bool {
        page->leavePreview();
        pump(150);
        zoom->setValue(400);
        pump(100);
        clickClip(r, 0);
        pump(150);
        page->togglePlay();
        if (!started())
            return false;
        while (page->currentFlatIndex() < 6 && page->isPlaying()) {
            page->advanceForTest();
            pump(40);
        }
        pump(80);
        return qAbs(tl->playheadScreenXForTest() - fp()) <= 1 && tl->scrollXForTest() > 0
            && page->isPlaying();
    };
    const QPointF overTracks(labelCol() + 300, canvas->height() / 2);

    // ---- scrolling back during playback holds -------------------------------------
    check(QStringLiteral("(av) fixture: playing at 400 %, held on the follow point"),
          heldAtFollowPoint());
    const int before = tl->scrollXForTest();
    wheelOnCanvas(canvas, overTracks, 120, Qt::NoModifier); // one notch up: toward the start
    const int afterOne = tl->scrollXForTest();
    wheelOnCanvas(canvas, overTracks, 120, Qt::NoModifier);
    wheelOnCanvas(canvas, overTracks, 120, Qt::ShiftModifier); // Shift+wheel: the same
    check(QStringLiteral("(av) THE WHEEL OVER THE TRACKS, up: the view scrolls "
                         "toward the start by one named step a notch (Shift+wheel "
                         "too), and - the playhead now right of the follow point - "
                         "the view is HELD"),
          before - afterOne > 0 && before - afterOne < tl->visibleWidthForTest() / 3
              && tl->scrollXForTest() == before - 3 * (before - afterOne)
              && tl->viewHeldForTest() && tl->playheadScreenXForTest() > fp(),
          QStringLiteral("scroll %1 -> %2 -> %3 (step %4 px), held %5, playhead x %6")
              .arg(before).arg(afterOne).arg(tl->scrollXForTest()).arg(before - afterOne)
              .arg(tl->viewHeldForTest()).arg(tl->playheadScreenXForTest()));
    const int heldAt = tl->scrollXForTest();
    pump(400);
    check(QStringLiteral("(av) ...and the view stays where the wheel put it "
                         "while the film plays on"),
          tl->scrollXForTest() == heldAt && page->isPlaying());

    // ---- scrolling ahead does not hold; following resumes at the point -----------------
    check(QStringLiteral("(av) fixture: held on the follow point"), heldAtFollowPoint());
    const int beforeAhead = tl->scrollXForTest();
    for (int i = 0; i < 3; ++i)
        wheelOnCanvas(canvas, overTracks, -120, Qt::NoModifier); // down: toward the end
    const int ahead = tl->scrollXForTest();
    const bool movedAhead = ahead > beforeAhead && !tl->viewHeldForTest()
        && tl->playheadScreenXForTest() < fp();
    while (page->currentFlatIndex() < 14 && page->isPlaying()) {
        page->advanceForTest();
        pump(40);
    }
    pump(80);
    check(QStringLiteral("(av) THE WHEEL DOWN scrolls ahead and does NOT hold; "
                         "following resumes when the playhead reaches the follow "
                         "point"),
          movedAhead && tl->scrollXForTest() > ahead
              && qAbs(tl->playheadScreenXForTest() - fp()) <= 1 && !tl->viewHeldForTest(),
          QStringLiteral("scroll %1 -> %2 (held %3) -> %4, playhead x %5")
              .arg(beforeAhead).arg(ahead).arg(tl->viewHeldForTest())
              .arg(tl->scrollXForTest()).arg(tl->playheadScreenXForTest()));
    page->leavePreview();
    pump(200);

    // ---- Ctrl+wheel zooms around the pointer ------------------------------------------
    zoom->setValue(200);
    pump(100);
    tl->userScrollForTest(600);
    pump(100);
    page->leavePreview();
    pump(100);
    const QPointF anchor(labelCol() + 500, canvas->height() / 2);
    const int ax = anchor.toPoint().x();
    const double contentBefore = tl->contentXAtForTest(ax);
    const double ppfBefore = tl->pxPerFrameForTest();
    const double zoomBefore = tl->zoomForTest();
    wheelOnCanvas(canvas, anchor, 120, Qt::ControlModifier); // one notch in
    const double contentAfter = tl->contentXAtForTest(ax);
    const double expectedAfter = contentBefore * (tl->pxPerFrameForTest() / ppfBefore);
    check(QStringLiteral("(av) CTRL+WHEEL zooms in around the pointer: the content "
                         "under the pointer stays within 1 px, the zoom slider is "
                         "in step, and viewHeld is false"),
          tl->zoomForTest() > zoomBefore && qAbs(contentAfter - expectedAfter) <= 1.0
              && zoom->value() == qRound(tl->zoomForTest()) && !tl->viewHeldForTest(),
          QStringLiteral("zoom %1 -> %2 (slider %3); content at x %4: %5 -> %6 (expected %7)")
              .arg(zoomBefore).arg(tl->zoomForTest()).arg(zoom->value()).arg(ax)
              .arg(contentBefore, 0, 'f', 1).arg(contentAfter, 0, 'f', 1)
              .arg(expectedAfter, 0, 'f', 1));
    const double contentBefore2 = tl->contentXAtForTest(ax);
    const double ppfBefore2 = tl->pxPerFrameForTest();
    wheelOnCanvas(canvas, anchor, -120, Qt::ControlModifier); // one notch out
    const double expectedAfter2 = contentBefore2 * (tl->pxPerFrameForTest() / ppfBefore2);
    check(QStringLiteral("(av) ...and out: the same"),
          qAbs(tl->contentXAtForTest(ax) - expectedAfter2) <= 1.0
              && zoom->value() == qRound(tl->zoomForTest()) && !tl->viewHeldForTest(),
          QStringLiteral("content %1 -> %2 (expected %3)").arg(contentBefore2, 0, 'f', 1)
              .arg(tl->contentXAtForTest(ax), 0, 'f', 1).arg(expectedAfter2, 0, 'f', 1));
    for (int i = 0; i < 40; ++i)
        wheelOnCanvas(canvas, anchor, 120, Qt::ControlModifier);
    const bool capped = qFuzzyCompare(tl->zoomForTest(), 400.0) && zoom->value() == 400;
    for (int i = 0; i < 60; ++i)
        wheelOnCanvas(canvas, anchor, -120, Qt::ControlModifier);
    check(QStringLiteral("(av) Ctrl+wheel respects the zoom limits: 400 % at the "
                         "top, 10 % at the bottom, the slider with it"),
          capped && qFuzzyCompare(tl->zoomForTest(), 10.0) && zoom->value() == 10
              && !tl->viewHeldForTest(),
          QStringLiteral("zoom %1, slider %2").arg(tl->zoomForTest()).arg(zoom->value()));

    // ---- the film fits: nothing moves, nothing breaks ---------------------------------
    for (QPushButton *b : r.animatic->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Fit"))
            b->click();
    pump(100);
    for (int i = 0; i < 4; ++i)
        wheelOnCanvas(canvas, overTracks, i % 2 ? 120 : -120, Qt::NoModifier);
    check(QStringLiteral("(av) when the film fits, the wheel over the tracks "
                         "moves nothing and holds nothing"),
          bar->maximum() == 0 && tl->scrollXForTest() == 0 && !tl->viewHeldForTest());

    // ---- positive control: the canvas's wheel handling off ----------------------------------
    check(QStringLiteral("(av) fixture: held on the follow point"), heldAtFollowPoint());
    tl->setCanvasWheelForTest(false);
    const int offBefore = tl->scrollXForTest();
    const double zoomOffBefore = tl->zoomForTest();
    wheelOnCanvas(canvas, overTracks, 120, Qt::NoModifier);
    wheelOnCanvas(canvas, overTracks, 120, Qt::ControlModifier);
    const int offAfter = tl->scrollXForTest();
    tl->setCanvasWheelForTest(true);
    check(QStringLiteral("(av) CONTROL, the canvas's wheel handling off: the same "
                         "events leave the offset, the hold and the zoom unchanged "
                         "(so the checks above could have failed)"),
          qAbs(offAfter - offBefore) <= 2 * 6 /* following's own motion in the wait */
              && !tl->viewHeldForTest() && qFuzzyCompare(tl->zoomForTest(), zoomOffBefore),
          QStringLiteral("scroll %1 -> %2, held %3, zoom %4").arg(offBefore).arg(offAfter)
              .arg(tl->viewHeldForTest()).arg(tl->zoomForTest()));
    page->leavePreview();
    pump(200);
}

// ---- (aw) the timeline's new look (Part A of the redesign) ---------------------
// 2026-10-18: the Figma redesign (Dasktop_UI_test, node 380:644). Checks
// that the look is the design's where a number can say so: every icon
// loads (with a wrong path as the control), the header's geometry, the
// merged zoom row and what collapses with the body, the label column, the
// ruler's label step, the clip layout and its hide order, the painted
// colours, and the give-way rule for a narrow window. Pixel reads come
// from the canvas's own grab, never from the screen.
void runTimelineLookPass(const QString &scratch)
{
    using namespace workspace;
    out() << "--- (aw) the timeline's new look: icons, header, label column, ruler, "
             "clips, colours, the give-way rule ---" << Qt::endl;

    // ---- every icon loads; a wrong path does not ------------------------------
    {
        const char *files[] = {"collapse_down", "collapse_up", "tr_first", "tr_prev",
                               "tr_play", "tr_pause", "tr_next", "tr_last", "loop_start",
                               "loop_end", "view_frames", "view_seconds", "view_snap",
                               "track_lock", "track_eye", "track_speaker"};
        QStringList missing, blank;
        for (const char *f : files) {
            const QString path = QStringLiteral(":/icons/timeline/%1.svg").arg(QLatin1String(f));
            const QIcon ic = SankoIcons::icon(path, QSize(16, 16));
            if (ic.isNull()) { missing << QLatin1String(f); continue; }
            const QImage img = ic.pixmap(16, 16).toImage();
            bool painted = false;
            for (int y = 0; y < img.height() && !painted; ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qAlpha(img.pixel(x, y)) > 0) { painted = true; break; }
            if (!painted)
                blank << QLatin1String(f);
        }
        check(QStringLiteral("(aw) EVERY TIMELINE ICON LOADS from the resources and paints "
                             "something (16 files)"),
              missing.isEmpty() && blank.isEmpty(),
              QStringLiteral("missing [%1] blank [%2]").arg(missing.join(' '), blank.join(' ')));
        check(QStringLiteral("(aw) control: a wrong path gives a null icon (so the check "
                             "above could have failed)"),
              SankoIcons::icon(QStringLiteral(":/icons/timeline/no_such_icon.svg"),
                               QSize(16, 16)).isNull(),
              QString());
    }

    const QString base = scratch + QStringLiteral("/look");
    QDir(base).removeRecursively();
    // The Figma sample: 60 fps, ten 3 s panels (the fourth 2 s), one scene.
    const QString project = writeProject(base + QStringLiteral("/p"), QStringLiteral("Look"),
                                         QSize(960, 540), 60, 1, 10);
    // A window wide enough for the whole header (its floor is about 1390):
    // 1800 on the 1920 test screen.
    Rig r(project, QSize(1800, 900));
    check(QStringLiteral("(aw) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    AnimaticPage *page = r.animatic;
    AnimaticTimeline *tl = r.timeline;
    QWidget *canvas = tl->surfaceForTest();
    // The fixture writes 2 / 3 / 4 s panels; the sample is ten of 3 s with
    // the fourth at 2 s.
    for (int i = 0; i < 10; ++i)
        page->setPanelDurationForTest(0, i, i == 3 ? 2 : 3);
    pump(200);
    auto widget = [&](const char *name) -> QWidget * {
        return page->findChild<QWidget *>(QLatin1String(name));
    };
    auto rectOf = [&](const char *name) -> QRect {
        QWidget *w = widget(name);
        return w ? QRect(w->mapTo(page, QPoint(0, 0)), w->size()) : QRect();
    };
    pump(300);

    // ---- the header: the design's geometry ---------------------------------------
    {
        const QRect collapse = rectOf("animaticCollapse"), fit = rectOf("timelineFit"),
                    minus = rectOf("timelineZoomOut"), field = rectOf("timelineZoomField"),
                    plus = rectOf("timelineZoomIn"), tc = rectOf("animaticTimecode"),
                    sep = rectOf("animaticTimecodeSeparator"), total = rectOf("animaticTotal"),
                    frames = rectOf("timelineFrames"), seconds = rectOf("timelineSeconds"),
                    snap = rectOf("timelineSnap"), pill = page->transportPillRectForTest();
        const int W = page->width();
        check(QStringLiteral("(aw) THE HEADER'S LEFT GROUP is the design's (2026-10-23, at y 6): "
                             "collapse 26x26 at (12,7); Fit 33x28 at (58,6); - 27x28 at 107; "
                             "the zoom field 46x28 at (137,6); + 27x28 at 186"),
              collapse == QRect(12, 7, 26, 26) && fit == QRect(58, 6, 33, 28)
                  && minus == QRect(107, 6, 27, 28) && field == QRect(137, 6, 46, 28)
                  && plus == QRect(186, 6, 27, 28),
              QStringLiteral("collapse %1,%2 %3x%4; fit %5,%6; minus %7,%8; field %9,%10 %11x%12; plus %13,%14")
                  .arg(collapse.x()).arg(collapse.y()).arg(collapse.width()).arg(collapse.height())
                  .arg(fit.x()).arg(fit.y()).arg(minus.x()).arg(minus.y()).arg(field.x()).arg(field.y())
                  .arg(field.width()).arg(field.height()).arg(plus.x()).arg(plus.y()));
        check(QStringLiteral("(aw) THE RIGHT GROUP ends 32 px before the edge: Snap 32x28 at "
                             "y 6, Seconds 14 px before it, Frames 14 px before that"),
              snap.right() == W - 32 - 1 && snap.size() == QSize(32, 28) && snap.y() == 6
                  && seconds.right() + 1 + 14 == snap.x() && frames.right() + 1 + 14 == seconds.x(),
              QStringLiteral("W %1 snap %2,%3 %4x%5 seconds %6 frames %7")
                  .arg(W).arg(snap.x()).arg(snap.y()).arg(snap.width()).arg(snap.height())
                  .arg(seconds.x()).arg(frames.x()));
        // The centre cluster (the design of 2026-10-23), measured from the pill:
        // the timecode box 132x40 ending 75 px before it, a 1x22 separator at
        // pill - 69 (y 9), a second at pill + 296, the total 331 px after the
        // pill's left edge, the speed dropdown 60x28 at y 6, 28 px after the total.
        const QRect sep2 = rectOf("animaticTotalSeparator"), speed = rectOf("animaticSpeed");
        check(QStringLiteral("(aw) THE CENTRE CLUSTER is the design's: the timecode 132x40 ends "
                             "75 px before the pill, the separators (1x22 at y 9) sit at pill-69 "
                             "and pill+296, the total starts at pill+331, the speed dropdown "
                             "60x28 at y 6 starts 28 px after the total"),
              tc.size() == QSize(132, 40) && tc.y() == 0 && tc.right() + 1 == pill.x() - 75
                  && sep == QRect(pill.x() - 69, 9, 1, 22) && sep2 == QRect(pill.x() + 296, 9, 1, 22)
                  && total.x() == pill.x() + 331 && total.height() == 40
                  && speed.size() == QSize(60, 28) && speed.y() == 6 && speed.x() == total.right() + 1 + 28,
              QStringLiteral("pill %1; tc %2,%3 %4x%5; sep %6,%7 %8x%9; sep2 %10,%11; total %12 (%13 wide); speed %14,%15 %16x%17")
                  .arg(pill.x()).arg(tc.x()).arg(tc.y()).arg(tc.width()).arg(tc.height())
                  .arg(sep.x()).arg(sep.y()).arg(sep.width()).arg(sep.height()).arg(sep2.x()).arg(sep2.y())
                  .arg(total.x()).arg(total.width()).arg(speed.x()).arg(speed.y()).arg(speed.width()).arg(speed.height()));
        const QRect left = rectOf("animaticHeaderLeft"), right = rectOf("animaticHeaderRight"),
                    cluster = rectOf("animaticCentre");
        const bool centred = pill.x() == (W - 228) / 2;
        const bool clear = cluster.left() >= left.right() + 1 + 16 && cluster.right() + 1 + 16 <= right.left();
        check(QStringLiteral("(aw) THE TRANSPORT PILL is 228x32 at y 4, centred on the window "
                             "when there is room; its cluster never within 16 px of either group"),
              pill.size() == QSize(228, 32) && pill.y() == 4 && clear
                  && (centred || cluster.right() + 1 + 16 == right.left()),
              QStringLiteral("pill %1,%2 %3x%4; cluster %5..%6; left group ends %7; right group starts %8; W %9")
                  .arg(pill.x()).arg(pill.y()).arg(pill.width()).arg(pill.height())
                  .arg(cluster.left()).arg(cluster.right()).arg(left.right()).arg(right.left()).arg(W));
        check(QStringLiteral("(aw) the Seconds button wears the checked fill and Frames does "
                             "not; Snap is checked by default (the states are unchanged)"),
              widget("timelineSeconds")->styleSheet().contains(QStringLiteral("#7c6ef6"))
                  && !widget("timelineFrames")->styleSheet().contains(QStringLiteral("background: #7c6ef6"))
                  && widget("timelineSnap")->styleSheet().contains(QStringLiteral("#7c6ef6")));
        check(QStringLiteral("(aw) THE TOTAL reads HH:MM:SS:FF in the project's rate: ten "
                             "panels of 3 s with one of 2 s at 60 fps = Total: 00:00:29:00"),
              qobject_cast<QLabel *>(widget("animaticTotal"))->text()
                  == QStringLiteral("Total: 00:00:29:00"),
              qobject_cast<QLabel *>(widget("animaticTotal"))->text());
        // The speed dropdown (the user's choice, 2026-10-23): three checkable
        // items, the current one checked, the third not clipped by the menu's
        // own height; choosing 2x is the same setPlaybackSpeed as before.
        {
            auto *btn = page->findChild<QToolButton *>(QStringLiteral("animaticSpeed"));
            auto *menu = page->findChild<QMenu *>(QStringLiteral("animaticSpeedMenu"));
            QStringList items;
            int checked = 0; QString checkedText;
            if (menu)
                for (QAction *a : menu->actions()) {
                    items << a->text();
                    if (a->isChecked()) { ++checked; checkedText = a->text(); }
                }
            const int menuH = menu ? menu->sizeHint().height() : 0;
            const QRect third = menu && menu->actions().size() == 3 ? menu->actionGeometry(menu->actions().at(2)) : QRect();
            const bool thirdFits = !third.isEmpty() && third.bottom() < menuH;
            QAction *twoX = nullptr;
            if (menu) for (QAction *a : menu->actions()) if (a->text() == QStringLiteral("2x")) twoX = a;
            if (twoX) twoX->trigger();
            pump(100);
            const QString totalAt2x = qobject_cast<QLabel *>(widget("animaticTotal"))->text();
            const QString buttonAt2x = btn ? btn->text() : QString();
            QAction *oneX = nullptr;
            if (menu) for (QAction *a : menu->actions()) if (a->text() == QStringLiteral("1x")) oneX = a;
            if (oneX) oneX->trigger();
            pump(100);
            check(QStringLiteral("(aw) THE SPEED DROPDOWN: 60x28, the menu lists 0.5x, 1x and 2x "
                             "with 1x checked, the third item inside the menu's height; choosing "
                             "2x shows 2x on the button and the Total's (2x) suffix (the same "
                             "speed path as before); 1x again restores it"),
                  btn && menu && items == QStringList({QStringLiteral("0.5x"), QStringLiteral("1x"), QStringLiteral("2x")})
                      && checked == 1 && checkedText == QStringLiteral("1x") && thirdFits
                      && buttonAt2x == QStringLiteral("2x") && totalAt2x == QStringLiteral("Total: 00:00:14:30 (2x)")
                      && btn->text() == QStringLiteral("1x")
                      && qobject_cast<QLabel *>(widget("animaticTotal"))->text() == QStringLiteral("Total: 00:00:29:00"),
                  QStringLiteral("items [%1] checked %2 '%3' third %4,%5 %6x%7 menu h %8; at 2x: button '%9' total '%10'; back: '%11'")
                      .arg(items.join(QStringLiteral(","))).arg(checked).arg(checkedText).arg(third.x()).arg(third.y())
                      .arg(third.width()).arg(third.height()).arg(menuH).arg(buttonAt2x).arg(totalAt2x)
                      .arg(btn ? btn->text() : QString()));
            // ---- THE ZOOM FIELD (Part D): shows the real zoom, whole percent ----
            QLineEdit *field = tl->zoomField();
            auto textNow = [&] { return tl->zoomFieldTextForTest(); };
            tl->setZoomForTest(100);
            pump(50);
            const bool rest = field && textNow() == QStringLiteral("100%") && field->isReadOnly()
                && field->focusPolicy() == Qt::NoFocus && !tl->zoomEditingForTest();
            qobject_cast<QPushButton *>(widget("timelineZoomIn"))->click();
            pump(50);
            const QString afterPlus = textNow();
            qobject_cast<QPushButton *>(widget("timelineZoomOut"))->click();
            qobject_cast<QPushButton *>(widget("timelineZoomOut"))->click();
            pump(50);
            const QString afterMinus = textNow();
            tl->setZoomForTest(133.4f); // any setter (Ctrl+wheel lands here too): rounded
            pump(50);
            const QString afterSet = textNow();
            qobject_cast<QPushButton *>(widget("timelineFit"))->click();
            pump(50);
            const QString afterFit = textNow(), fitWant = QStringLiteral("%1%").arg(qRound(tl->zoomForTest()));
            check(QStringLiteral("(aw) THE ZOOM FIELD shows the real zoom in whole percent: 100% at "
                                 "rest (read-only, no focus); + gives 125%, two - give 75%; a zoom of "
                                 "133.4 reads 133%; Fit reads Fit's own value rounded"),
                  rest && afterPlus == QStringLiteral("125%") && afterMinus == QStringLiteral("75%")
                      && afterSet == QStringLiteral("133%") && afterFit == fitWant,
                  QStringLiteral("rest %1 '%2' + '%3' -- '%4' set '%5' fit '%6' (want %7)")
                      .arg(rest).arg(textNow()).arg(afterPlus).arg(afterMinus).arg(afterSet).arg(afterFit).arg(fitWant));
            // Editing: double-click opens (begun through the same path), Enter
            // applies "150%" and "150"; out of range clamps; invalid restores;
            // Escape cancels; a lost focus (a click elsewhere) applies.
            auto pressKey = [&](QWidget *w, int key) {
                QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::sendEvent(w, &down);
                QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
                QCoreApplication::sendEvent(w, &up);
                pump(50);
            };
            tl->setZoomForTest(100);
            tl->beginZoomEditForTest();
            pump(50);
            const bool editing = tl->zoomEditingForTest() && field && !field->isReadOnly() && field->hasFocus()
                && field->selectedText() == field->text();
            field->setText(QStringLiteral("150%"));
            pressKey(field, Qt::Key_Return);
            const bool applied150 = !tl->zoomEditingForTest() && qFuzzyCompare(tl->zoomForTest(), 150.0)
                && textNow() == QStringLiteral("150%") && field->isReadOnly();
            tl->beginZoomEditForTest(); field->setText(QStringLiteral(" 80 ")); pressKey(field, Qt::Key_Return);
            const bool applied80 = qFuzzyCompare(tl->zoomForTest(), 80.0) && textNow() == QStringLiteral("80%");
            tl->beginZoomEditForTest(); field->setText(QStringLiteral("900")); pressKey(field, Qt::Key_Return);
            const bool clampedHigh = qFuzzyCompare(tl->zoomForTest(), 400.0) && textNow() == QStringLiteral("400%");
            tl->beginZoomEditForTest(); field->setText(QStringLiteral("3%")); pressKey(field, Qt::Key_Return);
            const bool clampedLow = qFuzzyCompare(tl->zoomForTest(), 10.0) && textNow() == QStringLiteral("10%");
            tl->beginZoomEditForTest(); field->setText(QStringLiteral("abc")); pressKey(field, Qt::Key_Return);
            const bool invalidRestored = qFuzzyCompare(tl->zoomForTest(), 10.0) && textNow() == QStringLiteral("10%");
            tl->beginZoomEditForTest(); field->setText(QStringLiteral("200")); pressKey(field, Qt::Key_Escape);
            const bool escaped = !tl->zoomEditingForTest() && qFuzzyCompare(tl->zoomForTest(), 10.0)
                && textNow() == QStringLiteral("10%") && field->isReadOnly();
            tl->beginZoomEditForTest(); field->setText(QStringLiteral("60")); field->clearFocus(); pump(50);
            const bool clickAway = !tl->zoomEditingForTest() && qFuzzyCompare(tl->zoomForTest(), 60.0)
                && textNow() == QStringLiteral("60%");
            check(QStringLiteral("(aw) EDITING THE ZOOM FIELD: double-click opens it with the text "
                                 "selected; Enter applies \"150%\" and \" 80 \"; 900 clamps to 400% and "
                                 "3% to 10%; \"abc\" restores the value; Escape cancels; a lost focus "
                                 "applies"),
                  editing && applied150 && applied80 && clampedHigh && clampedLow && invalidRestored
                      && escaped && clickAway,
                  QStringLiteral("editing %1 150 %2 80 %3 high %4 low %5 invalid %6 esc %7 away %8 (zoom %9 '%10')")
                      .arg(editing).arg(applied150).arg(applied80).arg(clampedHigh).arg(clampedLow)
                      .arg(invalidRestored).arg(escaped).arg(clickAway).arg(tl->zoomForTest()).arg(textNow()));
            // WHILE EDITING, EVERY KEY IS THE FIELD'S: Delete, Backspace, Space,
            // the arrows, Home and End change no panel, move no selection and
            // start no playback (the timeline's keys are armed - the pointer is
            // over it - and the SHOTS track is locked, so a Delete that did get
            // through would be refused and counted rather than ask a question).
            page->setTrackLocked(TimelineTrack::Shots, true);
            r.storyboard->clearLockRefusalForTest();
            hover(r, r.animatic, true);
            const bool armed = r.storyboard->timelineKeysArmedForTest();
            tl->beginZoomEditForTest();
            field->setText(QStringLiteral("123%"));
            field->deselect();
            field->setCursorPosition(4);
            pump(30);
            const int panels0 = page->itemCountForTest(), sel0 = page->selectedPanelForTest();
            const int ph0 = page->currentFlatIndex();
            // keyClaimed is true when a SHORTCUT took the key - the leak this
            // check exists to catch; then the real press goes to the field.
            bool claimed = false;
            for (int k : {Qt::Key_Backspace, Qt::Key_Delete, Qt::Key_Left, Qt::Key_Home, Qt::Key_End, Qt::Key_Space,
                          Qt::Key_Right, Qt::Key_Up, Qt::Key_Down}) {
                claimed = keyClaimed(field, k) || claimed;
                pressKey(field, k);
            }
            const QString typed = field->text();
            const bool fieldKept = !claimed && page->itemCountForTest() == panels0
                && page->selectedPanelForTest() == sel0 && page->currentFlatIndex() == ph0
                && !page->isPlaying() && r.storyboard->lastLockRefusalForTest().isEmpty()
                && tl->zoomEditingForTest() && typed != QStringLiteral("123%");
            pressKey(field, Qt::Key_Escape);
            // CONTROL: not editing, the same Space starts playback (the keys are
            // the timeline's again).
            keyClaimed(&r.window, Qt::Key_Space);
            pump(100);
            const bool spacePlays = page->isPlaying();
            page->leavePreview();
            hover(r, r.animatic, false);
            page->setTrackLocked(TimelineTrack::Shots, false);
            check(QStringLiteral("(aw) WHILE THE ZOOM FIELD IS BEING EDITED every key is the field's: "
                                 "Backspace, Delete, the arrows, Home, End and Space edit the text "
                                 "and change no panel, selection or playhead and start no playback; "
                                 "control: with the field at rest the same Space plays"),
                  armed && fieldKept && spacePlays,
                  QStringLiteral("armed %1 shortcut took a key %2 typed '%3' panels %4->%5 sel %6->%7 playing %8 refusal '%9' | control plays %10")
                      .arg(armed).arg(claimed).arg(typed).arg(panels0).arg(page->itemCountForTest()).arg(sel0)
                      .arg(page->selectedPanelForTest()).arg(page->isPlaying()).arg(r.storyboard->lastLockRefusalForTest()).arg(spacePlays));
            tl->setZoomForTest(70);
            pump(50);
        }
        // The heights with the scrollbar hidden (the film fits at zoom 70);
        // with it shown, the timeline's size hint grows by the bar's height,
        // as it always did.
        tl->setZoomForTest(70);
        pump(150);
        // Phase 1C (2026-10-28): the CAMERA row's 28 px is added to both -
        // 204 and 172 before it.
        check(QStringLiteral("(aw) the zoom row is gone: the canvas starts at y 40 of the "
                             "section, the expanded default is 232 and the minimum 200 (the "
                             "CAMERA row's 28 px on 204 / 172), the collapsed height stays 40"),
              canvas->mapTo(page, QPoint(0, 0)).y() == 40 && page->defaultExpandedHeight() == 232
                  && page->minimumExpandedHeight() == 200 && AnimaticPage::kHeaderHeight == 40,
              QStringLiteral("canvas y %1, default %2, minimum %3")
                  .arg(canvas->mapTo(page, QPoint(0, 0)).y()).arg(page->defaultExpandedHeight())
                  .arg(page->minimumExpandedHeight()));
    }

    // ---- collapsed: the zoom and view controls go with the body -------------------
    {
        page->setCollapsed(true);
        pump(200);
        const bool hidden = !widget("timelineZoomControls")->isVisible()
            && !widget("timelineViewControls")->isVisible() && widget("animaticTransport")->isVisible();
        page->setCollapsed(false);
        pump(200);
        check(QStringLiteral("(aw) COLLAPSED hides the zoom and view controls with the body "
                             "and keeps the transport; expanded shows them again"),
              hidden && widget("timelineZoomControls")->isVisible()
                  && widget("timelineViewControls")->isVisible());
    }

    // ---- the label column, the ruler step, the clip layout ------------------------
    {
        ZoomSetter zoomSetter{tl};
        ZoomSetter *zoom = &zoomSetter;
        zoom->setValue(70);
        pump(150);
        check(QStringLiteral("(aw) THE LABEL COLUMN is 134 px (the design of 2026-10-23) and the "
                             "ruler labels every 3 s at zoom 70 / 60 fps (42 px/s)"),
              AnimaticTimeline::labelColumnWidthForTest() == 134 && tl->rulerLabelStepForTest() == 3,
              QStringLiteral("column %1, step %2").arg(AnimaticTimeline::labelColumnWidthForTest())
                  .arg(tl->rulerLabelStepForTest()));
        const QVariantMap c0 = tl->clipPartsForTest(0), c3 = tl->clipPartsForTest(3);
        const QRect clip0 = tl->clipRectForTest(0);
        check(QStringLiteral("(aw) A 3 s CLIP at 42 px/s is 126 wide: body 124 (a 2 px gap), "
                             "thumbnail at (6,6) 112x42, number at (10,9), name at (8, bottom-20) "
                             "78 wide, pill 28x18 at (right-33, bottom-22); all shown"),
              c0.value(QStringLiteral("body")).toString()
                      == QStringLiteral("%1,%2 124x%3").arg(clip0.x()).arg(clip0.y()).arg(clip0.height())
                  && c0.value(QStringLiteral("thumb")).toString()
                      == QStringLiteral("%1,%2 112x42").arg(clip0.x() + 6).arg(clip0.y() + 6)
                  && c0.value(QStringLiteral("number")).toString().startsWith(
                      QStringLiteral("%1,%2 ").arg(clip0.x() + 10).arg(clip0.y() + 9))
                  && c0.value(QStringLiteral("name")).toString()
                      == QStringLiteral("%1,%2 78x15").arg(clip0.x() + 8).arg(clip0.bottom() - 20)
                  && c0.value(QStringLiteral("pill")).toString()
                      == QStringLiteral("%1,%2 28x18").arg(clip0.right() - 2 - 33).arg(clip0.bottom() - 22)
                  && c0.value(QStringLiteral("thumbShown")).toBool()
                  && c0.value(QStringLiteral("nameShown")).toBool()
                  && c0.value(QStringLiteral("pillShown")).toBool(),
              QStringLiteral("clip %1,%2 %3x%4; %5").arg(clip0.x()).arg(clip0.y()).arg(clip0.width())
                  .arg(clip0.height()).arg(QJsonDocument(QJsonObject::fromVariantMap(c0)).toJson(QJsonDocument::Compact)));
        check(QStringLiteral("(aw) the 2 s clip (84 px, body 82) hides its NAME and keeps the "
                             "thumbnail (70 wide) and the pill, as the design draws it"),
              c3.value(QStringLiteral("body")).toString().contains(QStringLiteral(" 82x"))
                  && !c3.value(QStringLiteral("nameShown")).toBool()
                  && c3.value(QStringLiteral("thumbShown")).toBool()
                  && c3.value(QStringLiteral("thumb")).toString().contains(QStringLiteral(" 70x42"))
                  && c3.value(QStringLiteral("pillShown")).toBool(),
              QJsonDocument(QJsonObject::fromVariantMap(c3)).toJson(QJsonDocument::Compact));
        // The hide order as the zoom drops: name, then thumbnail, then pill.
        int nameGone = -1, thumbGone = -1, pillGone = -1, numberAt10 = -1;
        for (int z = 400; z >= 10; --z) {
            zoom->setValue(z);
            const QVariantMap c = tl->clipPartsForTest(0);
            if (nameGone < 0 && !c.value(QStringLiteral("nameShown")).toBool()) nameGone = z;
            if (thumbGone < 0 && !c.value(QStringLiteral("thumbShown")).toBool()) thumbGone = z;
            if (pillGone < 0 && !c.value(QStringLiteral("pillShown")).toBool()) pillGone = z;
            if (z == 10) numberAt10 = tl->clipRectForTest(0).width();
        }
        check(QStringLiteral("(aw) AS A CLIP NARROWS the name hides first, then the thumbnail, "
                             "then the pill (the user's order), and at zoom 10 (18 px) only the "
                             "number is left"),
              nameGone > thumbGone && thumbGone > pillGone && pillGone > 0 && numberAt10 == 18,
              QStringLiteral("name hidden from zoom %1, thumbnail from %2, pill from %3; width at 10: %4")
                  .arg(nameGone).arg(thumbGone).arg(pillGone).arg(numberAt10));
        zoom->setValue(100);
        pump(100);
        // The step rule at other zooms: 24 px/s (zoom 100 at 24 fps) -> 5 s
        // is the old project's case; here at 60 fps zoom 100 = 60 px/s -> 2 s,
        // zoom 200 = 120 px/s -> 1 s, zoom 20 = 12 px/s -> 10 s.
        const int s100 = tl->rulerLabelStepForTest();
        zoom->setValue(200);
        const int s200 = tl->rulerLabelStepForTest();
        zoom->setValue(20);
        const int s20 = tl->rulerLabelStepForTest();
        zoom->setValue(70);
        pump(100);
        check(QStringLiteral("(aw) THE LABEL STEP is the smallest of 1,2,3,5,10,15,30,60 s at "
                             "least 120 px apart: 60 px/s -> 2 s, 120 px/s -> 1 s, 12 px/s -> 10 s"),
              s100 == 2 && s200 == 1 && s20 == 10,
              QStringLiteral("%1 %2 %3").arg(s100).arg(s200).arg(s20));
    }

    // ---- the painted colours, read from the canvas's own grab ----------------------
    {
        page->setSelectedPanel(3);
        pump(200);
        const QImage img = canvas->grab().toImage();
        auto px = [&](int x, int y) { return QColor(img.pixel(x, y)).name(); };
        const QRect c3 = tl->clipRectForTest(3);
        const int shotsY = 48 + tl->shotsTrackHeightForTest() / 2;
        const QString shots = px(canvas->width() - 20, shotsY), ruler = px(canvas->width() - 20, 4),
                      scenes = px(canvas->width() - 20, 36), column = px(60, 10), separator = px(134, 10),
                      afterSeparator = px(135, 10),
                      // the AUDIO row is under the CAMERA row since Phase 1C
                      audio = px(canvas->width() - 20, tl->audioRowForTest().y() + 4),
                      cameraRow = px(canvas->width() - 20, tl->cameraRowForTest().y() + 14),
                      selectedBorder = px(c3.x() + 1, c3.y() + c3.height() / 2),
                      // the body's own pixel: inside the border, under the thumbnail's
                      // bottom edge (6 + 42) and above the name line
                      body0 = px(tl->clipRectForTest(0).x() + 3, tl->clipRectForTest(0).y() + 51);
        check(QStringLiteral("(aw) THE TRACK COLOURS are the design's: shots #101115, ruler "
                             "#181a21, scenes #1c1e25, empty audio #181a21, label column #0d0d0d "
                             "with its #2a2a2a separator at x 134 and the ruler's content from x 135"),
              shots == QStringLiteral("#101115") && ruler == QStringLiteral("#181a21")
                  && scenes == QStringLiteral("#1c1e25") && audio == QStringLiteral("#181a21")
                  && column == QStringLiteral("#0d0d0d") && separator == QStringLiteral("#2a2a2a")
                  && afterSeparator == QStringLiteral("#181a21"),
              QStringLiteral("shots %1 ruler %2 scenes %3 audio %4 column %5 separator %6 x135 %7")
                  .arg(shots, ruler, scenes, audio, column, separator, afterSeparator));
        check(QStringLiteral("(aw) the CAMERA row (Phase 1C) is #15161c, between SHOTS and AUDIO"),
              cameraRow == QStringLiteral("#15161c")
                  && tl->cameraRowForTest().bottom() + 1 == tl->audioRowForTest().y(),
              cameraRow);
        check(QStringLiteral("(aw) THE SELECTED CLIP wears the 2 px #9787ff border and a plain "
                             "clip's body is #252330 (no gradient)"),
              selectedBorder == QStringLiteral("#9787ff") && body0 == QStringLiteral("#252330"),
              QStringLiteral("border %1 body %2").arg(selectedBorder, body0));
        page->setPlayheadForTest(6, 1684);
        pump(150);
        const QImage img2 = canvas->grab().toImage();
        const int phx = tl->playheadScreenXForTest();
        const QString ph = QColor(img2.pixel(phx, 40)).name();
        check(QStringLiteral("(aw) THE PLAYHEAD is #d5cbff and the timecode 00:00:18:41 at "
                             "1 s 41 f into the seventh panel"),
              ph == QStringLiteral("#d5cbff") && page->timecodeTextForTest() == QStringLiteral("00:00:18:41"),
              QStringLiteral("playhead %1 at x %2, timecode %3").arg(ph).arg(phx).arg(page->timecodeTextForTest()));
    }

    // ---- the give-way rule: the audio name shortens first, then the slider ---------
    {
        const QString wav = base + QStringLiteral("/sound/theme.wav");
        audio::writeWav(wav, 20.0);
        page->setAudioPath(wav, true);
        audio::waitFor([&] { return tl->audioBarStateForTest() == QStringLiteral("present"); }, 4000);
        pump(200);
        // THE FLOOR (Part D): nothing gives way any more - the zoom slider
        // and the audio name are gone from the header - so the floor is one
        // number, the same with and without a track, and the window refuses
        // below it. A fine track shows no warning in the header.
        auto groupRect = [&](const char *name) {
            return QRect(widget(name)->mapTo(page, QPoint(0, 0)), widget(name)->size());
        };
        // The right margin is the spacing's in force: 32 at the design's
        // full spacing, 16 compact (BEFORE SELLING 13).
        auto clearWith = [&](int margin) {
            const QRect cluster = groupRect("animaticCentre");
            const QRect left = groupRect("animaticHeaderLeft");
            const QRect right = groupRect("animaticHeaderRight");
            return cluster.left() >= left.right() + 1 + 16 && cluster.right() + 1 + 16 <= right.left()
                && right.right() + 1 + margin <= page->width();
        };
        auto clear = [&] { return clearWith(page->headerRightMarginForTest()); };
        const int floorWithAudio = page->headerMinimumWidthForTest();
        const int beside = r.window.width() - page->width();
        const bool noWarning = !page->audioWarningShownForTest();
        r.window.resize(floorWithAudio + beside + 400, 900);
        pump(400);
        const bool wideOk = clear() && !page->headerCompactForTest()
            && page->headerRightMarginForTest() == 32;
        r.window.resize(floorWithAudio + beside, 900);
        pump(400);
        const bool atFloor = page->width() == floorWithAudio && clear();
        const bool compactAtFloor = page->headerCompactForTest() && page->headerRightMarginForTest() == 16;
        const bool fullMarginFits = clearWith(32); // the control: it must not
        r.window.resize(floorWithAudio + beside - 150, 900);
        pump(400);
        const bool belowOk = page->width() >= floorWithAudio && clear();
        const int windowMin = r.window.minimumWidth();
        page->setAudioPath(QString(), false);
        pump(300);
        const int floorWithout = page->headerMinimumWidthForTest();
        check(QStringLiteral("(aw) THE FLOOR (Part D; compact below the full floor, BEFORE SELLING "
                             "13): one number with or without a track; wide (the design's spacing, "
                             "32 px right margin), at the floor and 150 px below it (compact, 16 px) "
                             "nothing overlaps and the section is never narrower than the floor; a "
                             "fine track shows no warning"),
              wideOk && atFloor && compactAtFloor && belowOk && floorWithout == floorWithAudio
                  && noWarning,
              QStringLiteral("floor %1 (without a track %2; full %3); wide %4 at %5 compact %6 below %7 (W %8, window min %9); warning shown %10")
                  .arg(floorWithAudio).arg(floorWithout).arg(page->headerFullFloorForTest())
                  .arg(wideOk).arg(atFloor).arg(compactAtFloor).arg(belowOk)
                  .arg(page->width()).arg(windowMin).arg(!noWarning));
        check(QStringLiteral("(aw) control: at the floor the full design's 32 px right margin "
                             "would not fit - the compact spacing is what makes it"),
              !fullMarginFits);
    }

    // ---- BEFORE SELLING 13: the window fits a 1366 px laptop in every header state ----
    {
        auto *loopStart = r.window.findChild<QPushButton *>(QStringLiteral("animaticLoopStart"));
        auto *loopEnd = r.window.findChild<QPushButton *>(QStringLiteral("animaticLoopEnd"));
        auto *loopClear = r.window.findChild<QPushButton *>(QStringLiteral("animaticLoopClear"));
        const QString hint = QStringLiteral("Set the other loop point to play the loop.");
        // The window at its narrowest: the minimum then is the one in force.
        auto windowMin = [&] {
            r.window.resize(600, 900);
            pump(400);
            return r.window.minimumWidth();
        };
        QStringList rows;
        int worst = 0, worstFull = 0;
        auto measure = [&](const QString &state) {
            const int w = windowMin();
            const int docks = w - page->headerMinimumWidthForTest();
            const int full = page->headerFullFloorForTest() + docks;
            rows << QStringLiteral("%1 %2 (full spacing %3)").arg(state).arg(w).arg(full);
            worst = qMax(worst, w);
            worstFull = qMax(worstFull, full);
            return w;
        };
        // (The clear button is hidden while one point is set; its click
        // clears either way.)
        if (loopClear)
            loopClear->click();
        pump(100);
        measure(QStringLiteral("plain"));
        // One point - the start first, then (cleared) the end first.
        page->clearNoteForTest();
        page->setPlayheadForTest(2, 0);
        if (loopStart)
            loopStart->click();
        pump(100);
        const QString startNote = page->lastNoteForTest();
        const QString startTip = loopStart ? loopStart->toolTip() : QString();
        measure(QStringLiteral("start only"));
        if (loopClear)
            loopClear->click();
        page->clearNoteForTest();
        page->setPlayheadForTest(4, 0);
        if (loopEnd)
            loopEnd->click();
        pump(100);
        const QString endNote = page->lastNoteForTest();
        const QString endTip = loopEnd ? loopEnd->toolTip() : QString();
        const bool noLabel = !r.window.findChild<QLabel *>(QStringLiteral("animaticLoopWarning"));
        measure(QStringLiteral("end only"));
        // Both: the clear-loop button shows.
        page->setPlayheadForTest(2, 0);
        if (loopStart)
            loopStart->click();
        pump(100);
        const bool clearShown = loopClear && loopClear->isVisible();
        const QString bothTip = loopStart ? loopStart->toolTip() : QString();
        measure(QStringLiteral("clear-loop"));
        // The audio warning, with the clear-loop button and without.
        page->setAudioPath(base + QStringLiteral("/sound/no_such_file.wav"), true);
        pump(400);
        const bool warningShown = page->audioWarningShownForTest();
        measure(QStringLiteral("warning + clear-loop"));
        if (loopClear)
            loopClear->click();
        pump(100);
        measure(QStringLiteral("warning"));
        page->setAudioPath(QString(), false);
        pump(300);
        check(QStringLiteral("(aw) BEFORE SELLING 13: THE WINDOW'S MINIMUM WIDTH IS AT MOST 1366 PX "
                             "in every header state - plain, one loop point (either first), the "
                             "clear-loop button, the audio warning, and the warning with the "
                             "clear-loop button - every dock open"),
              worst > 0 && worst <= 1366 && warningShown && clearShown && rows.size() == 6,
              QStringLiteral("worst %1; %2; warning shown %3, clear-loop shown %4")
                  .arg(worst).arg(rows.join(QStringLiteral("; "))).arg(warningShown).arg(clearShown));
        check(QStringLiteral("(aw) control: at the design's full spacing the same states would need "
                             "more than 1366 px (the check could fail)"),
              worstFull > 1366, QStringLiteral("full-spacing worst %1").arg(worstFull));
        check(QStringLiteral("(aw) THE LOOP HINT is a note at the pointer and the set button's "
                             "tooltip, \"Set the other loop point to play the loop.\", whichever "
                             "point is set first - and no longer a label in the header; with both "
                             "points set the tooltip is the button's own again"),
              startNote == hint && startTip == hint && endNote == hint && endTip == hint && noLabel
                  && bothTip == QStringLiteral("Set loop start to the selected panel"),
              QStringLiteral("start note '%1' tip '%2'; end note '%3' tip '%4'; label gone %5; both '%6'")
                  .arg(startNote, startTip, endNote, endTip).arg(noLabel).arg(bothTip));
    }
}

// ---- (ax) the tracks' lock, visibility and mute (Part B) --------------------
// The three tracks' lock / eye / speaker buttons over the label column.
// Every lock blocks every path in the lock table, each with its positive
// control (the same edit succeeds unlocked); a hidden track paints nothing
// and takes no press; mute reaches the player and the export; the seven
// flags round-trip through the manifest and an old project loads with the
// defaults; the toggles mark the project unsaved and push nothing on the
// undo stack; Undo / Redo of a locked track's command is refused with a
// message and the stack is left as it is. Synthetic events only.
// (cm) THE SHOT CAMERA, Phase 1A (2026-10-25; the user's rule): removing a
// panel that holds camera keys changes the scene's camera move, so it must
// not be silent - the history names the keys ("Delete Panel and 1 Camera
// Key") and the locks' non-modal note says the move changed. Through the
// real page and the real undo stack: Cut (no question) and Delete (its
// confirmation answered), the plural, undo bringing the key back at the
// same frame, and the control (a panel without keys: the plain text, no
// note). Part of the spec's C4.
void runCameraPanelRemovalPass(const QString &scratch)
{
    using namespace workspace;
    out() << "--- (cm) removing a panel that holds camera keys ---" << Qt::endl;
    const QString project = writeProject(scratch + QStringLiteral("/camdel"),
                                         QStringLiteral("CamDel"), QSize(960, 540),
                                         24, 1, 4);
    Rig r(project);
    check(QStringLiteral("(cm) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    Scene *scene = r.window.scenesForTest().value(0);
    QUndoStack *stack = r.window.undoStackForTest();
    if (!scene || scene->panels.size() < 4 || !stack) {
        check(QStringLiteral("(cm) found a scene of four panels and the history"), false);
        return;
    }
    shotcam::CameraState pushed = shotcam::neutral(QSize(960, 540));
    pushed.zoom = 1.4;
    scene->camera.keys = {shotcam::makeKey(*scene, 1, 10, 24, pushed),
                          shotcam::makeKey(*scene, 3, 0, 24,
                                           shotcam::neutral(QSize(960, 540)))};
    const double k1Frame = shotcam::keyFrame(*scene, scene->camera.keys.at(0), 24);
    auto lastText = [&] { return stack->index() > 0 ? stack->text(stack->index() - 1)
                                                    : QString(); };

    // Control: a panel without keys.
    clickThumb(r, 0);
    r.storyboard->clearNoteForTest();
    r.storyboard->cutSelectedPanel();
    pump(200);
    const QString plainText = lastText();
    const QString plainNote = r.storyboard->lastNoteForTest();
    stack->undo();
    pump(200);
    check(QStringLiteral("(cm) control: cutting a panel WITHOUT camera keys says "
                         "\"Cut Panel\" and shows no note"),
          plainText == QStringLiteral("Cut Panel") && plainNote.isEmpty(), plainText);

    // The panel holding the first key.
    clickThumb(r, 1);
    r.storyboard->clearNoteForTest();
    r.storyboard->cutSelectedPanel();
    pump(200);
    const QString cutText = lastText();
    const QString cutNote = r.storyboard->lastNoteForTest();
    const int liveAfterCut = shotcam::liveKeyCount(*scene);
    check(QStringLiteral("(cm) cutting the panel that holds a camera key says \"Cut "
                         "Panel and 1 Camera Key\", shows the note, and the key "
                         "leaves the move"),
          cutText == QStringLiteral("Cut Panel and 1 Camera Key")
              && cutNote.contains(QStringLiteral("camera move")) && liveAfterCut == 1,
          cutText + QStringLiteral(" | ") + cutNote);
    stack->undo();
    pump(200);
    check(QStringLiteral("(cm) undo brings the panel and its key back, at the same "
                         "frame"),
          shotcam::liveKeyCount(*scene) == 2
              && shotcam::keyFrame(*scene, scene->camera.keys.at(0), 24) == k1Frame);

    clickThumb(r, 1);
    r.storyboard->clearNoteForTest();
    r.storyboard->deleteSelectedPanelConfirmedForTest();
    pump(200);
    const QString deleteText = lastText();
    const QString deleteNote = r.storyboard->lastNoteForTest();
    stack->undo();
    pump(200);
    check(QStringLiteral("(cm) Delete says \"Delete Panel and 1 Camera Key\" with the "
                         "same note"),
          deleteText == QStringLiteral("Delete Panel and 1 Camera Key")
              && deleteNote == cutNote,
          deleteText);

    scene->camera.keys.append(shotcam::makeKey(*scene, 3, 30, 24, pushed));
    clickThumb(r, 3);
    r.storyboard->clearNoteForTest();
    r.storyboard->cutSelectedPanel();
    pump(200);
    const QString pluralText = lastText();
    const QString pluralNote = r.storyboard->lastNoteForTest();
    stack->undo();
    pump(200);
    check(QStringLiteral("(cm) a panel holding two keys says \"Cut Panel and 2 Camera "
                         "Keys\""),
          pluralText == QStringLiteral("Cut Panel and 2 Camera Keys")
              && pluralNote.contains(QStringLiteral("2 keys")),
          pluralText + QStringLiteral(" | ") + pluralNote);
    scene->camera.keys.clear();
}

// ---- (ct) the shot camera's canvas tool (Phase 1B, 2026-10-25) ------------
// The tool the artist frames a shot with, through the REAL paths: the
// Camera tool button, synthetic pointer / key / wheel events on the canvas
// (never the real pointer), the Camera bar's own switches and buttons.
// C3 (the canvas frame is frameQuad through the view), C5 (navigation input
// never makes a key), C6 (Animate off refuses with two or more keys), C9
// (every camera command undoes and redoes byte for byte), each with its
// positive control; and the bar, the clamp, the guides (on with every tool,
// inset from the camera, remembered), the note style, the strip outline,
// Stills Show Full Artwork, the 1080p hint.
namespace camtool {

void mouse(QWidget *w, QEvent::Type type, const QPointF &local, Qt::MouseButton b,
           Qt::MouseButtons held, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QMouseEvent ev(type, local, w->mapToGlobal(local), b, held, mods);
    QCoreApplication::sendEvent(w, &ev);
}

void drag(QWidget *w, const QPointF &from, const QPointF &to,
          Qt::KeyboardModifiers mods = Qt::NoModifier,
          Qt::MouseButton b = Qt::LeftButton)
{
    mouse(w, QEvent::MouseButtonPress, from, b, b, mods);
    for (int i = 1; i <= 6; ++i)
        mouse(w, QEvent::MouseMove, from + (to - from) * (i / 6.0), Qt::NoButton, b,
              mods);
    mouse(w, QEvent::MouseButtonRelease, to, b, Qt::NoButton, mods);
    pump(30);
}

void key(QWidget *w, QEvent::Type type, int k)
{
    QKeyEvent ev(type, k, Qt::NoModifier, k == Qt::Key_Space ? QStringLiteral(" ")
                                                             : QString());
    QCoreApplication::sendEvent(w, &ev);
}

void wheel(QWidget *w, const QPointF &at, Qt::KeyboardModifiers mods)
{
    QWheelEvent ev(at, w->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton,
                   mods, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &ev);
}

// A Camera-bar switch, pressed where the artist presses it.
void pressSwitch(QWidget *bar, const QString &name)
{
    QWidget *s = bar ? bar->findChild<QWidget *>(name) : nullptr;
    if (!s)
        return;
    const QPointF c(s->width() / 2.0, s->height() / 2.0);
    mouse(s, QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
    mouse(s, QEvent::MouseButtonRelease, c, Qt::LeftButton, Qt::NoButton);
    pump(30);
}

void clickBar(QWidget *bar, const QString &name)
{
    if (auto *b = bar ? bar->findChild<QAbstractButton *>(name) : nullptr)
        b->click();
    pump(30);
}

QByteArray json(const Scene *scene)
{
    return QJsonDocument(shotcam::toJson(*scene)).toJson(QJsonDocument::Compact);
}

bool samePoly(const QPolygonF &a, const QPolygonF &b, double tol = 1e-6)
{
    if (a.size() != b.size() || a.isEmpty())
        return false;
    for (int i = 0; i < a.size(); ++i)
        if (QLineF(a.at(i), b.at(i)).length() > tol)
            return false;
    return true;
}

QString polyText(const QPolygonF &p)
{
    QStringList s;
    for (const QPointF &q : p)
        s << QStringLiteral("%1,%2").arg(q.x(), 0, 'f', 2).arg(q.y(), 0, 'f', 2);
    return s.join(QLatin1Char(' '));
}

QPointF centre(const QPolygonF &q) { return (q.at(0) + q.at(2)) / 2.0; }

// Points along a frame-local edge (the safe-area insets): the camera's frame
// turned and placed, then the view.
QVector<QPointF> insetTopEdge(const shotcam::CameraState &s, const QSize &cs,
                              double inset, const QTransform &view)
{
    const double fw = cs.width() / s.zoom, fh = cs.height() / s.zoom;
    QTransform t;
    t.translate(s.cx, s.cy);
    t.rotate(s.rotation);
    QVector<QPointF> pts;
    for (int i = 2; i <= 8; ++i) {
        const double x = -fw / 2.0 + fw * inset + (fw * (1.0 - 2.0 * inset)) * (i / 10.0);
        pts.append(view.map(t.map(QPointF(x, -fh / 2.0 + fh * inset))));
    }
    return pts;
}

// How many of `pts` (widget px) show a changed pixel between the two grabs
// within one pixel.
int changedAt(const QImage &a, const QImage &b, const QVector<QPointF> &pts)
{
    const double dpr = a.devicePixelRatio();
    int n = 0;
    for (const QPointF &p : pts) {
        bool changed = false;
        for (int dy = -2; dy <= 2 && !changed; ++dy)
            for (int dx = -2; dx <= 2 && !changed; ++dx) {
                const QPoint q(qRound(p.x() * dpr) + dx, qRound(p.y() * dpr) + dy);
                if (a.rect().contains(q) && a.pixel(q) != b.pixel(q))
                    changed = true;
            }
        n += changed ? 1 : 0;
    }
    return n;
}

QImage grabCanvas(DrawingCanvas *c)
{
    c->repaint();
    return c->grab().toImage();
}

// Qt's own warnings while a check runs (the polish batch, 2026-10-27):
// collected, and passed on to whatever handler was there before.
QStringList g_qtWarnings;
QtMessageHandler g_previousHandler = nullptr;
void collectWarnings(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    if (type == QtWarningMsg)
        g_qtWarnings << msg;
    if (g_previousHandler)
        g_previousHandler(type, context, msg);
}
struct WarningCapture
{
    WarningCapture()
    {
        g_qtWarnings.clear();
        g_previousHandler = qInstallMessageHandler(collectWarnings);
    }
    ~WarningCapture() { qInstallMessageHandler(g_previousHandler); }
    int count(const QString &part) const
    {
        return int(g_qtWarnings.filter(part).size());
    }
};

// A pointer event at the WINDOW-SYSTEM level, as a real one arrives.
void wsMouse(QWindow *win, const QPointF &global, QEvent::Type type,
             Qt::MouseButtons buttons)
{
    const QPointF local = global - QPointF(win->mapToGlobal(QPoint(0, 0)));
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
        win, local, global, buttons, Qt::LeftButton, type);
}

} // namespace camtool

// ---- (cr) the CAMERA row on the timeline (Phase 1C, 2026-10-28) ------------
// The row between SHOTS and AUDIO: a diamond on every live key at its frame,
// segments with the move's ease (Hold dashed) never across a cut between
// scenes; click, drag (re-anchoring to the panel it lands in), the key's and
// the row's menus, Delete; the lock, the eye (the camera off EVERYWHERE) and
// Animate (the Camera bar's switch). Each check has a control proving the
// measurement can see the other outcome.
void runCameraRowPass(const QString &scratch)
{
    using namespace workspace;
    using namespace camtool;
    out() << "--- (cr) the CAMERA row on the timeline (Phase 1C) ---" << Qt::endl;
    const QSize cs(1920, 1080);
    // 2 scenes x 4 panels of 2, 3, 4, 2 s at 24 fps: 48, 72, 96, 48 frames;
    // the panels start at scene frames 0, 48, 120, 216 and a scene ends at 264.
    const QString project = writeProject(scratch + QStringLiteral("/camrow"),
                                         QStringLiteral("CamRow"), cs, 24, 2, 4);
    auto rig = std::make_unique<Rig>(project);
    Rig &r = *rig;
    check(QStringLiteral("(cr) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    Scene *s0 = r.window.scenesForTest().value(0);
    Scene *s1 = r.window.scenesForTest().value(1);
    QUndoStack *stack = r.window.undoStackForTest();
    QWidget *bar = r.storyboard->cameraBarForTest();
    if (!s0 || !s1 || s0->panels.size() < 4 || s1->panels.size() < 2 || !stack || !bar) {
        check(QStringLiteral("(cr) found the two scenes, the history and the Camera bar"), false);
        return;
    }
    // The keys, set as a load would: scene 0 at its frames 12 (Ease In/Out),
    // 72 (Linear), 168 (Hold) and 240 (Ease Out, the last); scene 1 at 0
    // and 72 (film frames 264 and 336).
    using shotcam::CameraState;
    using shotcam::Ease;
    {
        shotcam::SceneCamera c0, c1;
        c0.keys << shotcam::makeKey(*s0, 0, 12, 24, CameraState{960, 540, 1.2, 0}, Ease::EaseInOut)
                << shotcam::makeKey(*s0, 1, 24, 24, CameraState{900, 500, 1.5, 0}, Ease::Linear)
                << shotcam::makeKey(*s0, 2, 48, 24, CameraState{1000, 560, 2.0, 10}, Ease::Hold)
                << shotcam::makeKey(*s0, 3, 24, 24, CameraState{960, 540, 1.3, 0}, Ease::EaseOut);
        c1.keys << shotcam::makeKey(*s1, 0, 0, 24, CameraState{960, 540, 1.4, 0}, Ease::EaseInOut)
                << shotcam::makeKey(*s1, 1, 24, 24, CameraState{800, 450, 1.8, 0}, Ease::EaseInOut);
        r.storyboard->applyCameraForUndo(s0, c0);
        r.storyboard->applyCameraForUndo(s1, c1);
    }
    r.timeline->setZoomForTest(100); // 1 px a frame: content x = film frame
    pump(150);
    const int rowY = r.timeline->cameraRowForTest().y() + 14;
    auto keys = [&] { return r.timeline->cameraKeysForTest(); };
    auto keyMap = [&](int s, int k) {
        for (const QVariant &v : keys())
            if (v.toMap().value(QStringLiteral("scene")).toInt() == s
                && v.toMap().value(QStringLiteral("key")).toInt() == k)
                return v.toMap();
        return QVariantMap();
    };
    auto keyX = [&](int s, int k) {
        const QVariantMap m = keyMap(s, k);
        return m.isEmpty() ? -1000 : m.value(QStringLiteral("x")).toInt();
    };
    auto screenX = [&](double filmFrame) {
        return 135 + qRound(filmFrame * r.timeline->pxPerFrameForTest()
                            - r.timeline->scrollXForTest());
    };
    auto click = [&](int x) {
        mouse(r.surface, QEvent::MouseButtonPress, QPointF(x, rowY), Qt::LeftButton, Qt::LeftButton);
        mouse(r.surface, QEvent::MouseButtonRelease, QPointF(x, rowY), Qt::LeftButton, Qt::NoButton);
        pump(80);
    };
    auto dragKey = [&](int fromX, int toX) {
        drag(r.surface, QPointF(fromX, rowY), QPointF(toX, rowY));
        pump(80);
    };
    auto menuAt = [&](int x, const std::function<void(QMenu *)> &fn) {
        r.storyboard->setClipMenuHookForTest(fn);
        QContextMenuEvent ev(QContextMenuEvent::Mouse, QPoint(x, rowY),
                             r.surface->mapToGlobal(QPoint(x, rowY)));
        QCoreApplication::sendEvent(r.surface, &ev);
        r.storyboard->setClipMenuHookForTest({});
        pump(80);
    };
    auto selected = [&](int *s, int *k) { return r.animatic->selectedCameraKey(s, k); };
    auto lastText = [&] { return stack->index() > 0 ? stack->text(stack->index() - 1) : QString(); };
    auto liveKeys = [&](const Scene *s) { return shotcam::liveKeyCount(*s); };
    auto findAction = [](QMenu *m, const QString &name) {
        return m ? m->findChild<QAction *>(name) : nullptr;
    };
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");

    // --- (cr.1) the row: 28 px between SHOTS and AUDIO; the minimum 160 ---
    {
        const QRect cam = r.timeline->cameraRowForTest();
        const QRect audio = r.timeline->audioRowForTest();
        const int shotsBottom = 48 + r.timeline->shotsTrackHeightForTest();
        check(QStringLiteral("(cr.1) the CAMERA row is 28 px, right under SHOTS and right over "
                             "AUDIO; the timeline canvas's minimum is 160 (48 + 48 + 28 + 36)"),
              cam.y() == shotsBottom && cam.height() == 28 && audio.y() == cam.y() + 28
                  && r.surface->minimumHeight() == 160,
              QStringLiteral("camera y %1 h %2, shots end %3, audio y %4, minimum %5")
                  .arg(cam.y()).arg(cam.height()).arg(shotsBottom).arg(audio.y())
                  .arg(r.surface->minimumHeight()));
        check(QStringLiteral("(cr.1) control: the minimum is not the pre-camera 132"),
              r.surface->minimumHeight() != 132);
    }

    // --- (cr.2) every key where its frame is, at three zooms, two scrolls ---
    {
        auto expected = [&](int s, int k) {
            const Scene *sc = s == 0 ? s0 : s1;
            const double start = s == 0 ? 0.0 : 264.0;
            return start + shotcam::keyFrame(*sc, sc->camera.keys.at(k), 24);
        };
        const QVector<QPair<int, int>> all{{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 1}};
        QStringList bad;
        int checked = 0, controlSame = 0;
        for (float zoom : {100.0f, 400.0f, 60.0f}) {
            r.timeline->setZoomForTest(zoom);
            pump(60);
            for (int scroll : {0, 300}) {
                if (scroll > 0 && zoom != 400.0f)
                    continue; // the film fits the view below 400 %
                r.timeline->userScrollForTest(scroll);
                pump(40);
                for (const auto &sk : all) {
                    ++checked;
                    const int want = screenX(expected(sk.first, sk.second));
                    if (keyX(sk.first, sk.second) != want)
                        bad << QStringLiteral("z%1 s%2 key %3:%4 x %5 want %6")
                                   .arg(zoom).arg(scroll).arg(sk.first).arg(sk.second)
                                   .arg(keyX(sk.first, sk.second)).arg(want);
                    if (zoom == 400.0f
                        && keyX(sk.first, sk.second) == screenX(expected(sk.first, sk.second) + 1))
                        ++controlSame;
                }
            }
        }
        r.timeline->setZoomForTest(100);
        r.timeline->userScrollForTest(0);
        pump(80);
        check(QStringLiteral("(cr.2) each of the 6 keys is drawn at its frame's x, at zoom 100, "
                             "400 and 60, scrolled 0 and 300"),
              keys().size() == 6 && checked == 24 && bad.isEmpty(),
              QStringLiteral("%1 keys; %2").arg(keys().size()).arg(bad.join(QStringLiteral("; "))));
        check(QStringLiteral("(cr.2) control: at 400 % one frame later is a different x for every key"),
              controlSame == 0, QString::number(controlSame));
    }

    // --- (cr.13) the hover tooltip names the key ---------------------------
    check(QStringLiteral("(cr.13) a key's tooltip names it - Key 2, 00:00:03:00, panel 2, "
                         "Linear - and the scene's last key says \"last key\""),
          keyMap(0, 1).value(QStringLiteral("tip")).toString()
                  == QStringLiteral("Key 2") + dot + QStringLiteral("00:00:03:00") + dot
                         + QStringLiteral("panel 2") + dot + QStringLiteral("Linear")
              && keyMap(0, 3).value(QStringLiteral("tip")).toString().endsWith(QStringLiteral("last key")),
          keyMap(0, 1).value(QStringLiteral("tip")).toString() + QStringLiteral(" | ")
              + keyMap(0, 3).value(QStringLiteral("tip")).toString());

    // --- (cr.3) the segments --------------------------------------------------
    {
        auto segs = [&] { return r.timeline->cameraSegmentsForTest(); };
        QStringList seen;
        bool crossed = false;
        for (const QVariant &v : segs()) {
            const QVariantMap m = v.toMap();
            seen << QStringLiteral("%1:%2>%3 %4%5%6")
                        .arg(m.value(QStringLiteral("scene")).toInt())
                        .arg(m.value(QStringLiteral("from")).toInt())
                        .arg(m.value(QStringLiteral("to")).toInt())
                        .arg(m.value(QStringLiteral("ease")).toString())
                        .arg(m.value(QStringLiteral("dashed")).toBool() ? QStringLiteral(" dashed") : QString())
                        .arg(m.value(QStringLiteral("glyph")).toBool() ? QStringLiteral(" glyph") : QString());
            crossed = crossed || (m.value(QStringLiteral("scene")).toInt() == 0
                                  && m.value(QStringLiteral("from")).toInt() == 3);
        }
        const QStringList want{QStringLiteral("0:0>1 Ease In/Out glyph"),
                               QStringLiteral("0:1>2 Linear glyph"),
                               QStringLiteral("0:2>3 Hold dashed glyph"),
                               QStringLiteral("1:0>1 Ease In/Out glyph")};
        check(QStringLiteral("(cr.3) a segment between each two keys of a scene, named by the "
                             "first key's ease, Hold dashed; none from scene 1's last key to "
                             "scene 2's first"),
              seen == want && !crossed, seen.join(QStringLiteral(", ")));
        // CONTROL: the segment really reads the key - change its ease, and
        // the segment changes with it.
        shotcam::SceneCamera c0 = s0->camera;
        c0.keys[1].ease = Ease::EaseOut;
        r.storyboard->applyCameraForUndo(s0, c0);
        pump(40);
        const bool follows = segs().value(1).toMap().value(QStringLiteral("ease")).toString()
            == QStringLiteral("Ease Out");
        c0.keys[1].ease = Ease::Linear;
        r.storyboard->applyCameraForUndo(s0, c0);
        pump(40);
        check(QStringLiteral("(cr.3) control: changing key 2's ease changes its segment"), follows);
    }

    // --- (cr.4) a click selects the key and moves the playhead to it --------
    click(keyX(0, 1));
    {
        int s = -1, k = -1;
        const bool sel = selected(&s, &k);
        check(QStringLiteral("(cr.4) a click on key 2 selects it, selects its panel and puts the "
                             "playhead on its frame (panel 2, frame 24)"),
              sel && s == 0 && k == 1 && r.storyboard->selectedFlatIndexForTest() == 1
                  && r.animatic->playheadPanel() == s0->panels.at(1)
                  && r.animatic->playheadFrameInPanel() == 24
                  && keyMap(0, 1).value(QStringLiteral("selected")).toBool()
                  && keyMap(0, 1).value(QStringLiteral("atPlayhead")).toBool(),
              QStringLiteral("selected %1 (%2:%3), panel %4, frame %5")
                  .arg(sel).arg(s).arg(k).arg(r.storyboard->selectedFlatIndexForTest())
                  .arg(r.animatic->playheadFrameInPanel()));
        // (cr.12) the recorder sees it.
        const QVariantMap rec = r.timeline->devrecState();
        check(QStringLiteral("(cr.12) the Dev Recorder's statePoll has the CAMERA row's lock, eye, "
                             "Animate, drag and the selected key (\"0:1\")"),
              rec.contains(QStringLiteral("timeline.cameraLocked"))
                  && rec.contains(QStringLiteral("timeline.cameraVisible"))
                  && rec.contains(QStringLiteral("timeline.cameraAnimate"))
                  && rec.contains(QStringLiteral("timeline.cameraDrag"))
                  && rec.value(QStringLiteral("timeline.cameraKey")).toString() == QStringLiteral("0:1"),
              rec.value(QStringLiteral("timeline.cameraKey")).toString());
        click(255); // the row between key 2 and key 3, away from the playhead
        check(QStringLiteral("(cr.4) control: a click on the row off every key selects none"),
              !selected(nullptr, nullptr)
                  && r.timeline->devrecState().value(QStringLiteral("timeline.cameraKey")).toString().isEmpty());
    }

    // --- (cr.5) drags: across a cut, within a panel, the snap, onto a key ---
    {
        const QByteArray before = json(s0);
        const int count = stack->index(); // a position in the history, not its size
        dragKey(keyX(0, 1), screenX(130)); // panel 3 starts at 120: frame 10 of it
        const shotcam::CameraKey moved = s0->camera.keys.at(1);
        check(QStringLiteral("(cr.5) a key dragged across a cut is re-anchored to the panel it "
                             "lands in (panel 3, frame 10 of 96), as ONE command"),
              moved.panelUid == s0->panels.at(2)->uid.value() && qAbs(moved.at - 10.0 / 96) < 1e-9
                  && stack->index() == count + 1 && lastText() == QStringLiteral("Move Camera Key")
                  && moved.state == CameraState({900, 500, 1.5, 0}) && moved.ease == Ease::Linear,
              QStringLiteral("uid ok %1, at %2, commands +%3 (%4)")
                  .arg(moved.panelUid == s0->panels.at(2)->uid.value()).arg(moved.at)
                  .arg(stack->index() - count).arg(lastText()));
        r.window.undoActionForTest()->trigger();
        pump(80);
        check(QStringLiteral("(cr.5) Undo puts the camera back byte for byte"), json(s0) == before);
        dragKey(keyX(0, 1), screenX(80)); // inside panel 2
        const shotcam::CameraKey inside = s0->camera.keys.at(1);
        check(QStringLiteral("(cr.5) control: a drag inside one panel leaves the key on that panel "
                             "(panel 2, frame 32 of 72)"),
              inside.panelUid == s0->panels.at(1)->uid.value() && qAbs(inside.at - 32.0 / 72) < 1e-9,
              QString::number(inside.at));
        r.window.undoActionForTest()->trigger();
        pump(80);
        dragKey(keyX(0, 1), screenX(117)); // 3 px before the cut at 120
        const bool snapped = s0->camera.keys.at(1).panelUid == s0->panels.at(2)->uid.value()
            && s0->camera.keys.at(1).at == 0.0;
        r.window.undoActionForTest()->trigger();
        pump(80);
        // The Snap button lives in the page's header, not under the timeline.
        auto *snap = r.window.findChild<QPushButton *>(QStringLiteral("timelineSnap"));
        if (snap)
            snap->click(); // off
        dragKey(keyX(0, 1), screenX(117));
        const bool unsnapped = s0->camera.keys.at(1).panelUid == s0->panels.at(1)->uid.value()
            && qAbs(s0->camera.keys.at(1).at - 69.0 / 72) < 1e-9;
        r.window.undoActionForTest()->trigger();
        pump(80);
        if (snap)
            snap->click(); // on again
        check(QStringLiteral("(cr.5) with Snap on, a key dropped 3 px from a cut lands ON the cut "
                             "(the next panel's first frame)"),
              snapped);
        check(QStringLiteral("(cr.5) control: with Snap off the same drop stays 3 frames short"),
              unsnapped);
        const int before2 = stack->index();
        r.storyboard->clearNoteForTest();
        dragKey(keyX(0, 1), keyX(0, 2)); // onto key 3
        check(QStringLiteral("(cr.5) a key is never dropped onto another key: nothing changes "
                             "and a note says why"),
              json(s0) == before && stack->index() == before2
                  && r.storyboard->lastNoteForTest() == QStringLiteral("Another camera key is at this frame."),
              r.storyboard->lastNoteForTest());
    }

    // --- (cr.6) the key's menu and the row's --------------------------------
    {
        const int count = stack->index();
        QString header;
        bool easeOn = false;
        QStringList entries;
        QString checkedEase;
        menuAt(keyX(0, 1), [&](QMenu *m) {
            header = findAction(m, QStringLiteral("cameraKeyHeader"))
                ? findAction(m, QStringLiteral("cameraKeyHeader"))->text() : QString();
            QMenu *ease = m->findChild<QMenu *>(QStringLiteral("cameraKeyEase"));
            easeOn = ease && ease->menuAction()->isEnabled();
            if (ease)
                for (QAction *a : ease->actions()) {
                    entries << a->text();
                    if (a->isChecked())
                        checkedEase = a->text();
                }
            if (ease)
                for (QAction *a : ease->actions())
                    if (a->text() == QStringLiteral("Ease In"))
                        a->trigger();
        });
        check(QStringLiteral("(cr.6) the key's menu names it (Key 2, 00:00:03:00, panel 2) and "
                             "offers the five eases, its own checked; choosing Ease In is one "
                             "command"),
              header == QStringLiteral("Key 2") + dot + QStringLiteral("00:00:03:00") + dot
                            + QStringLiteral("panel 2")
                  && easeOn
                  && entries == QStringList({QStringLiteral("Linear"), QStringLiteral("Ease In"),
                                             QStringLiteral("Ease Out"), QStringLiteral("Ease In/Out"),
                                             QStringLiteral("Hold")})
                  && checkedEase == QStringLiteral("Linear")
                  && s0->camera.keys.at(1).ease == Ease::EaseIn && stack->index() == count + 1
                  && lastText() == QStringLiteral("Camera Ease"),
              QStringLiteral("%1 | %2 | checked %3 | +%4").arg(header, entries.join(QLatin1Char(',')),
                                                               checkedEase).arg(stack->index() - count));
        const int count2 = stack->index();
        menuAt(keyX(0, 1), [&](QMenu *m) {
            if (QMenu *ease = m->findChild<QMenu *>(QStringLiteral("cameraKeyEase")))
                for (QAction *a : ease->actions())
                    if (a->text() == QStringLiteral("Ease In"))
                        a->trigger(); // the key's own ease already
        });
        check(QStringLiteral("(cr.6) control: choosing the ease the key already has makes no command"),
              stack->index() == count2);
        // Copy on key 2, Paste on key 3.
        menuAt(keyX(0, 1), [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraCopyFraming")))
                a->trigger();
        });
        QString pasteText;
        bool pasteOn = false;
        menuAt(keyX(0, 2), [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraPasteFraming"))) {
                pasteText = a->text();
                pasteOn = a->isEnabled();
                a->trigger();
            }
        });
        check(QStringLiteral("(cr.6) Copy Framing on key 2, then Paste Framing (from key 2) on key "
                             "3: key 3 takes key 2's framing, one command"),
              r.storyboard->hasCameraFramingForTest() && pasteOn
                  && pasteText == QStringLiteral("Paste Framing (from key 2)")
                  && s0->camera.keys.at(2).state == s0->camera.keys.at(1).state
                  && lastText() == QStringLiteral("Paste Camera Framing"),
              pasteText);
        // The last key: no move after it, so no ease.
        QString lastHeader, lastEase;
        bool lastEaseOn = true;
        menuAt(keyX(0, 3), [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraKeyHeader")))
                lastHeader = a->text();
            if (QMenu *ease = m->findChild<QMenu *>(QStringLiteral("cameraKeyEase"))) {
                lastEaseOn = ease->menuAction()->isEnabled();
                lastEase = ease->menuAction()->text();
            }
        });
        check(QStringLiteral("(cr.6) on the scene's last key Ease is disabled (\"no move after this "
                             "key\") and the header says \"last key\""),
              !lastEaseOn && lastEase == QString::fromUtf8("Ease \xE2\x80\x94 no move after this key")
                  && lastHeader.endsWith(QStringLiteral("last key")),
              lastHeader + QStringLiteral(" | ") + lastEase);
        // Delete Key, and Undo.
        const QByteArray beforeDelete = json(s0);
        menuAt(keyX(0, 2), [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraDeleteKey")))
                a->trigger();
        });
        const bool deleted = liveKeys(s0) == 3 && lastText() == QStringLiteral("Delete Camera Key");
        r.window.undoActionForTest()->trigger();
        pump(80);
        check(QStringLiteral("(cr.6) Delete Key removes the key (one command); Undo brings the "
                             "camera back byte for byte"),
              deleted && json(s0) == beforeDelete);
        // The row's menu, off every key: Add Key at Playhead.
        // (Positions in the history, not its size: a push after an Undo
        // drops the redo entries.)
        r.animatic->setPlayheadFrameInPanel(2, 60); // scene frame 180
        pump(60);
        const int index3 = stack->index();
        QString addFree;
        bool addFreeOn = false;
        menuAt(255, [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraAddKeyAtPlayhead"))) {
                addFree = a->text();
                addFreeOn = a->isEnabled();
                a->trigger();
            }
        });
        const bool added = liveKeys(s0) == 5 && stack->index() == index3 + 1
            && shotcam::keyIndexAtFrame(*s0, 180, 24) >= 0
            && lastText() == QStringLiteral("Add Camera Key");
        const int index4 = stack->index();
        QString addSet;
        bool addSetOn = true;
        menuAt(255, [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraAddKeyAtPlayhead"))) {
                addSet = a->text();
                addSetOn = a->isEnabled();
                a->trigger();
            }
        });
        // The 1C hand test (2026-10-29): an enabled entry that did nothing.
        check(QStringLiteral("(cr.6) with a key already at the playhead the row's entry reads "
                             "\"Key Set at Playhead\" and is disabled, as the Camera bar's "
                             "\"Key set\""),
              addSet == QStringLiteral("Key Set at Playhead") && !addSetOn, addSet);
        check(QStringLiteral("(cr.6) control: with no key at the playhead it read \"Add Key at "
                             "Playhead\" and was enabled"),
              addFree == QStringLiteral("Add Key at Playhead") && addFreeOn, addFree);
        check(QStringLiteral("(cr.6) the row's menu adds a key at the playhead (scene frame 180), "
                             "one command"),
              added, QStringLiteral("%1 keys, +%2").arg(liveKeys(s0)).arg(stack->index() - index3));
        check(QStringLiteral("(cr.6) control: a second Add at the same frame adds nothing"),
              stack->index() == index4 && liveKeys(s0) == 5);
        r.window.undoActionForTest()->trigger(); // the added key
        pump(60);
        // Paste at a frame with no key and a different camera (scene frame
        // 30, inside the first move): a key with the copied framing.
        r.animatic->setPlayheadFrameInPanel(0, 30);
        pump(60);
        const int index5 = stack->index();
        menuAt(255, [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraPasteFramingAtPlayhead")))
                a->trigger();
        });
        const int at = shotcam::keyIndexAtFrame(*s0, 30, 24);
        check(QStringLiteral("(cr.6) Paste Framing at Playhead sets a key there with the copied "
                             "framing, one command"),
              at >= 0 && s0->camera.keys.at(at).state == CameraState({900, 500, 1.5, 0})
                  && stack->index() == index5 + 1
                  && lastText() == QStringLiteral("Paste Camera Framing"));
        const int index6 = stack->index();
        menuAt(255, [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraPasteFramingAtPlayhead")))
                a->trigger();
        });
        check(QStringLiteral("(cr.6) control: pasting the same framing again on that key is no "
                             "command"),
              stack->index() == index6);
        r.window.undoActionForTest()->trigger(); // the pasted key
        pump(80);
    }

    // --- (cr.7) Delete over the timeline takes the selected key -------------
    {
        click(keyX(0, 0));
        const int liveBefore = liveKeys(s0);
        r.storyboard->timelineDeleteForTest();
        pump(60);
        check(QStringLiteral("(cr.7) Delete with a key selected deletes the key, not its panel"),
              liveKeys(s0) == liveBefore - 1 && s0->panels.size() == 4
                  && lastText() == QStringLiteral("Delete Camera Key"));
        r.window.undoActionForTest()->trigger();
        pump(80);
        click(keyX(0, 0));
        const QPointF clip = r.timeline->clipRectForTest(2).center();
        mouse(r.surface, QEvent::MouseButtonPress, clip, Qt::LeftButton, Qt::LeftButton);
        mouse(r.surface, QEvent::MouseButtonRelease, clip, Qt::LeftButton, Qt::NoButton);
        pump(80);
        check(QStringLiteral("(cr.7) control: a press on a clip ends the key selection, so Delete "
                             "is the panel's again"),
              !selected(nullptr, nullptr));
    }

    // --- (cr.10) Animate: the row's record dot and the Camera bar's switch --
    {
        QToolButton *dotBtn = r.timeline->trackControlForTest(QStringLiteral("trackAnimateCamera"));
        const bool offBefore = dotBtn && !dotBtn->isChecked() && !r.storyboard->cameraAnimate()
            && !r.storyboard->cameraBarStateForTest().value(QStringLiteral("camAnimate.on")).toBool();
        if (dotBtn)
            dotBtn->click();
        pump(60);
        const bool onBoth = r.storyboard->cameraAnimate() && dotBtn && dotBtn->isChecked()
            && r.storyboard->cameraBarStateForTest().value(QStringLiteral("camAnimate.on")).toBool();
        pressSwitch(bar, QStringLiteral("camAnimate"));
        pump(60);
        const bool offAgain = !r.storyboard->cameraAnimate() && dotBtn && !dotBtn->isChecked();
        check(QStringLiteral("(cr.10) the CAMERA row's Animate turns the Camera bar's switch on, and "
                             "the bar's switch turns the row's off: one state"),
              onBoth && offAgain, QStringLiteral("on %1 off %2").arg(onBoth).arg(offAgain));
        check(QStringLiteral("(cr.10) control: both read off before the click"), offBefore);
    }

    // --- (cr.8) the CAMERA lock ------------------------------------------------
    {
        QToolButton *lock = r.timeline->trackControlForTest(QStringLiteral("trackLockCamera"));
        if (lock)
            lock->click();
        pump(60);
        const QByteArray before = json(s0);
        const int count = stack->index();
        r.timeline->clearRefusalForTest();
        dragKey(keyX(0, 1), screenX(130));
        const bool dragRefused = json(s0) == before && stack->index() == count
            && r.timeline->lastRefusalForTest() == QStringLiteral("CAMERA track is locked");
        QString header;
        bool easeOn = true, delOn = true, pasteOn = true, copyOn = false;
        menuAt(keyX(0, 1), [&](QMenu *m) {
            if (QAction *a = findAction(m, QStringLiteral("cameraKeyHeader")))
                header = a->text();
            if (QMenu *ease = m->findChild<QMenu *>(QStringLiteral("cameraKeyEase")))
                easeOn = ease->menuAction()->isEnabled();
            if (QAction *a = findAction(m, QStringLiteral("cameraDeleteKey")))
                delOn = a->isEnabled();
            if (QAction *a = findAction(m, QStringLiteral("cameraPasteFraming")))
                pasteOn = a->isEnabled();
            if (QAction *a = findAction(m, QStringLiteral("cameraCopyFraming")))
                copyOn = a->isEnabled();
        });
        r.storyboard->clearLockRefusalForTest();
        r.animatic->setPlayheadFrameInPanel(1, 5); // on no key
        pump(40);
        clickBar(bar, QStringLiteral("camAddKey"));
        const bool barRefused = stack->index() == count
            && r.storyboard->lastLockRefusalForTest() == QStringLiteral("CAMERA track is locked");
        r.storyboard->clearLockRefusalForTest();
        const int index = stack->index();
        r.window.undoActionForTest()->trigger(); // the top command is a camera edit
        pump(60);
        const bool undoRefused = stack->index() == index
            && r.storyboard->lastLockRefusalForTest() == QStringLiteral("CAMERA track is locked");
        check(QStringLiteral("(cr.8) CAMERA locked: a key drag is refused at the pointer (\"CAMERA "
                             "track is locked\") and nothing changes"),
              dragRefused, r.timeline->lastRefusalForTest());
        check(QStringLiteral("(cr.8) ...the key's menu says it is locked and every edit in it is "
                             "disabled (Copy stays)"),
              header == QStringLiteral("The CAMERA track is locked") && !easeOn && !delOn && !pasteOn
                  && copyOn,
              header);
        check(QStringLiteral("(cr.8) ...the Camera bar's Add Key and Undo of a camera edit are "
                             "refused too"),
              barRefused && undoRefused,
              QStringLiteral("bar %1 undo %2 (top: %3)").arg(barRefused).arg(undoRefused)
                  .arg(lastText()));
        check(QStringLiteral("(cr.8) ...and the row is drawn at 40 %"),
              qAbs(r.timeline->cameraRowOpacityForTest() - 0.4) < 1e-9);
        if (lock)
            lock->click(); // unlocked
        pump(60);
        dragKey(keyX(0, 1), screenX(130));
        const bool accepted = stack->index() == count + 1 && json(s0) != before;
        r.window.undoActionForTest()->trigger();
        pump(80);
        check(QStringLiteral("(cr.8) control: unlocked, the same drag is accepted (and the row is "
                             "opaque)"),
              accepted && r.timeline->cameraRowOpacityForTest() == 1.0);
    }

    // --- (cr.9) the eye: the camera off EVERYWHERE ------------------------------
    {
        QToolButton *eye = r.timeline->trackControlForTest(QStringLiteral("trackEyeCamera"));
        const QString pngDir = scratch + QStringLiteral("/camrow_png");
        auto pngOfPanel2 = [&](const QString &folder) {
            QDir(folder).removeRecursively();
            QDir().mkpath(folder);
            const sankoexport::FilesResult res = r.window.exportPngForTest(folder);
            for (const QString &f : res.written)
                if (f.endsWith(QStringLiteral("_S01_P02.png")))
                    return QImage(f).convertToFormat(QImage::Format_RGB32);
            return QImage();
        };
        const QImage flat = s0->panels.at(1)->flattenedPixmap().toImage()
                                .convertToFormat(QImage::Format_RGB32);
        auto state = [&](bool *clipNeutral, bool *previewNeutral, QString *warning, QImage *png,
                         const QString &folder) {
            *clipNeutral = shotcam::isNeutral(r.timeline->clipCameraForTest(1), cs);
            r.animatic->scrubForTest(1);
            pump(120);
            r.animatic->cameraTickForTest();
            *previewNeutral = r.animatic->previewCameraNeutralForTest();
            r.animatic->leavePreview();
            pump(80);
            *warning = r.window.mp4CameraWarningForTest();
            *png = pngOfPanel2(folder);
        };
        bool clipOn = true, previewOn = true, clipOff = false, previewOff = false;
        QString warnOn, warnOff;
        QImage pngOn, pngOff;
        state(&clipOn, &previewOn, &warnOn, &pngOn, pngDir + QStringLiteral("_on"));
        if (eye)
            eye->click(); // hidden
        pump(80);
        state(&clipOff, &previewOff, &warnOff, &pngOff, pngDir + QStringLiteral("_off"));
        r.animatic->setSelectedCameraKey(-1, -1);
        click(keyX(0, 1));
        const bool pressIgnored = !selected(nullptr, nullptr);
        check(QStringLiteral("(cr.9) CAMERA hidden: the SHOTS clip, the preview and the PNG export "
                             "show the whole panel, and the MP4 export warns first"),
              clipOff && previewOff && !pngOff.isNull() && pngOff == flat
                  && warnOff.contains(QStringLiteral("The CAMERA track is hidden")),
              QStringLiteral("clip %1 preview %2 png==flat %3 warning \"%4\"")
                  .arg(clipOff).arg(previewOff).arg(pngOff == flat).arg(warnOff.left(40)));
        check(QStringLiteral("(cr.9) ...the hidden row takes no press and is drawn at 40 %"),
              pressIgnored && qAbs(r.timeline->cameraRowOpacityForTest() - 0.4) < 1e-9);
        check(QStringLiteral("(cr.9) control: shown, the clip, the preview and the PNG are the "
                             "shot (not the whole panel) and there is no warning"),
              !clipOn && !previewOn && !pngOn.isNull() && pngOn != flat && warnOn.isEmpty(),
              QStringLiteral("clip %1 preview %2 png==flat %3").arg(clipOn).arg(previewOn)
                  .arg(pngOn == flat));
    }

    // --- (cr.11) the camera lock and eye are saved -----------------------------
    {
        const QString saved = scratch + QStringLiteral("/camrow_saved/CamRow.sankotv");
        QDir().mkpath(QFileInfo(saved).absolutePath());
        auto tracksJson = [&] {
            QFile f(saved);
            f.open(QIODevice::ReadOnly);
            return QJsonDocument::fromJson(f.readAll()).object()
                .value(QStringLiteral("tracks")).toObject();
        };
        // Shown and unlocked: the old JSON (no "camera").
        if (r.animatic->tracks().cameraLocked)
            r.animatic->setTrackLocked(TimelineTrack::Camera, false);
        r.animatic->setTrackVisible(TimelineTrack::Camera, true);
        r.window.saveProjectForTest(saved);
        pump(100);
        const bool plain = !tracksJson().contains(QStringLiteral("camera"));
        r.animatic->setTrackLocked(TimelineTrack::Camera, true);
        r.animatic->setTrackVisible(TimelineTrack::Camera, false);
        r.window.saveProjectForTest(saved);
        pump(100);
        const QJsonObject cam = tracksJson().value(QStringLiteral("camera")).toObject();
        const bool written = cam.value(QStringLiteral("locked")).toBool(false)
            && !cam.value(QStringLiteral("visible")).toBool(true);
        r.window.markCleanForTest();
        r.window.loadProjectForTest(project); // the fixture: no "camera" at all
        pump(300);
        const bool oldDefaults = !r.animatic->tracks().cameraLocked && r.animatic->tracks().cameraVisible;
        r.window.loadProjectForTest(saved);
        pump(300);
        const bool back = r.animatic->tracks().cameraLocked && !r.animatic->tracks().cameraVisible
            && r.timeline->trackControlForTest(QStringLiteral("trackLockCamera"))->isChecked()
            && r.timeline->trackControlForTest(QStringLiteral("trackEyeCamera"))->isChecked();
        check(QStringLiteral("(cr.11) the CAMERA lock and eye are saved under tracks.camera and come "
                             "back on load; a project without them opens unlocked and shown"),
              written && back && oldDefaults,
              QStringLiteral("written %1 back %2 defaults %3").arg(written).arg(back).arg(oldDefaults));
        check(QStringLiteral("(cr.11) control: unlocked and shown, the save writes no \"camera\" - "
                             "the JSON every older project has"),
              plain);
        r.animatic->setTrackLocked(TimelineTrack::Camera, false);
        r.animatic->setTrackVisible(TimelineTrack::Camera, true);
    }
}

// ---- (qm) the Quick Moves and the scene framing (Phase 1D, 2026-10-29) -----
// The popover (its preview changes nothing; a click is one command), the span
// rule (inside replaced, ends updated, outside untouched, the eases), moves
// chaining, Undo, the lock and the covered canvas, the three amounts kept
// between launches, the CAMERA row's and the clip's menus, and Use Last
// Framing / Continue Camera. Each check has a control.
void runQuickMovePass(const QString &scratch)
{
    using namespace workspace;
    using namespace camtool;
    out() << "--- (qm) Quick Moves and the scene framing (Phase 1D) ---" << Qt::endl;
    const QSize cs(1920, 1080);
    sankoSettings().remove(StoryboardPage::kCameraQuickKey + QStringLiteral("Zoom"));
    sankoSettings().remove(StoryboardPage::kCameraQuickKey + QStringLiteral("Move"));
    sankoSettings().remove(StoryboardPage::kCameraQuickKey + QStringLiteral("Turn"));
    // 2 scenes x 4 panels of 2, 3, 4, 2 s at 24 fps: panel starts 0, 48, 120,
    // 216; a scene is 264 frames.
    const QString project = writeProject(scratch + QStringLiteral("/quickmove"),
                                         QStringLiteral("Quick"), cs, 24, 2, 4);
    auto rig = std::make_unique<Rig>(project);
    Rig &r = *rig;
    check(QStringLiteral("(qm) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    Scene *s0 = r.window.scenesForTest().value(0);
    Scene *s1 = r.window.scenesForTest().value(1);
    QUndoStack *stack = r.window.undoStackForTest();
    QWidget *bar = r.storyboard->cameraBarForTest();
    QWidget *pop = r.storyboard->cameraQuickPopoverForTest();
    if (!s0 || !s1 || !stack || !bar || !pop) {
        check(QStringLiteral("(qm) found the scenes, the history, the bar and the popover"), false);
        return;
    }
    using shotcam::CameraState;
    using shotcam::Ease;
    using shotcam::QuickMove;
    auto lastText = [&] { return stack->index() > 0 ? stack->text(stack->index() - 1) : QString(); };
    auto keyAt = [&](const Scene *s, double f) { return shotcam::keyIndexAtFrame(*s, f, 24); };
    auto stateAt = [&](const Scene *s, double f) {
        const int k = keyAt(s, f);
        return k >= 0 ? s->camera.keys.at(k).state : CameraState{-1, -1, -1, -1};
    };
    // ("near" is a Windows macro, hence the name.)
    auto sameState = [](const CameraState &a, const CameraState &b) {
        return qAbs(a.cx - b.cx) < 0.01 && qAbs(a.cy - b.cy) < 0.01
            && qAbs(a.zoom - b.zoom) < 1e-9 && qAbs(a.rotation - b.rotation) < 1e-9;
    };
    auto setCam = [&](Scene *s, const QVector<shotcam::CameraKey> &keys) {
        shotcam::SceneCamera c;
        c.keys = keys;
        r.storyboard->applyCameraForUndo(s, c);
        pump(30);
    };
    auto menuHooked = [&](const std::function<void()> &open, const std::function<void(QMenu *)> &fn) {
        r.storyboard->setClipMenuHookForTest(fn);
        open();
        r.storyboard->setClipMenuHookForTest({});
        pump(60);
    };
    const CameraState z2{960, 540, 2.0, 0};
    clickThumb(r, 1); // the canvas on panel 2 (scene 1): the span starts at frame 48

    // --- (qm.1) the popover, its preview, the click -------------------------
    setCam(s0, {shotcam::makeKey(*s0, 0, 0, 24, z2)}); // a still 2x framing
    QToolButton *quickBtn = bar->findChild<QToolButton *>(QStringLiteral("camQuickMove"));
    if (quickBtn)
        quickBtn->click();
    pump(150);
    QToolButton *pushTile = pop->findChild<QToolButton *>(QStringLiteral("camQuickPushIn"));
    int tiles = 0;
    for (auto *t : pop->findChildren<QToolButton *>())
        tiles += t->objectName().startsWith(QStringLiteral("camQuick")) ? 1 : 0;
    const auto *zoomField = pop->findChild<QDoubleSpinBox *>(QStringLiteral("camQuickZoom"));
    const auto *moveField = pop->findChild<QSpinBox *>(QStringLiteral("camQuickMoveAmount"));
    const auto *turnField = pop->findChild<QSpinBox *>(QStringLiteral("camQuickTurn"));
    check(QStringLiteral("(qm.1) Quick Move opens the popover: eight moves and the three amounts "
                         "Zoom 1.25x, Move 25 %, Turn 10 deg"),
          pop->isVisible() && tiles == 8 && zoomField && moveField && turnField
              && qAbs(zoomField->value() - 1.25) < 1e-9 && moveField->value() == 25
              && turnField->value() == 10,
          QStringLiteral("visible %1 tiles %2").arg(pop->isVisible()).arg(tiles));
    const QByteArray beforePreview = json(s0);
    const int indexBefore = stack->index();
    if (pushTile) {
        QEnterEvent ev(QPointF(5, 5), QPointF(5, 5), pushTile->mapToGlobal(QPoint(5, 5)));
        QCoreApplication::sendEvent(pushTile, &ev);
    }
    pump(80);
    const QVariantMap prev = r.timeline->cameraPreviewForTest();
    const bool previewShown = prev.value(QStringLiteral("shown")).toBool()
        && !r.canvas->shotCameraPreviewEndForTest().isEmpty();
    const bool rowSpan = qAbs(prev.value(QStringLiteral("x1")).toInt() - prev.value(QStringLiteral("x0")).toInt()
                              - 72 * r.timeline->pxPerFrameForTest()) <= 1.0;
    check(QStringLiteral("(qm.1) hovering Push In previews it on the canvas and the CAMERA row "
                         "(the span: panel 2's 72 frames) and changes nothing"),
          previewShown && rowSpan && json(s0) == beforePreview && stack->index() == indexBefore,
          QStringLiteral("shown %1 span %2..%3").arg(previewShown)
              .arg(prev.value(QStringLiteral("x0")).toInt()).arg(prev.value(QStringLiteral("x1")).toInt()));
    if (pushTile) {
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(pushTile, &leave);
    }
    pump(60);
    check(QStringLiteral("(qm.1) control: leaving the move clears the preview"),
          !r.timeline->cameraPreviewForTest().value(QStringLiteral("shown")).toBool()
              && r.canvas->shotCameraPreviewEndForTest().isEmpty());
    if (pushTile)
        pushTile->click();
    pump(100);
    check(QStringLiteral("(qm.1) a click on Push In applies it as ONE command and closes the "
                         "popover: keys at the span's start (2x) and end (2.5x)"),
          !pop->isVisible() && stack->index() == indexBefore + 1
              && lastText() == QStringLiteral("Quick Camera Move")
              && sameState(stateAt(s0, 48), z2) && sameState(stateAt(s0, 120), CameraState{960, 540, 2.5, 0}),
          QStringLiteral("+%1 %2").arg(stack->index() - indexBefore).arg(lastText()));
    r.window.undoActionForTest()->trigger();
    pump(60);
    check(QStringLiteral("(qm.1) Undo puts the camera back byte for byte"), json(s0) == beforePreview);

    // --- (qm.2) the span rule ------------------------------------------------
    {
        r.storyboard->setQuickEase(Ease::Linear);
        setCam(s0, {shotcam::makeKey(*s0, 0, 12, 24, z2, Ease::EaseInOut),              // 12: outside
                    shotcam::makeKey(*s0, 1, 24, 24, CameraState{900, 500, 2.2, 0}),   // 72: inside
                    shotcam::makeKey(*s0, 2, 0, 24, CameraState{1000, 560, 2.4, 0}, Ease::Hold), // 120: on the end
                    shotcam::makeKey(*s0, 2, 80, 24, CameraState{960, 540, 3.0, 0})}); // 200: outside
        const CameraState startHere = shotcam::evaluateAtPanel(*s0, 1, 0, 24);
        r.storyboard->cameraQuickMove(QuickMove::PanRight);
        pump(60);
        const bool outsideKept = keyAt(s0, 12) >= 0 && sameState(stateAt(s0, 12), z2)
            && keyAt(s0, 200) >= 0 && sameState(stateAt(s0, 200), CameraState{960, 540, 3.0, 0});
        const bool insideGone = keyAt(s0, 72) < 0;
        const bool ends = keyAt(s0, 48) >= 0 && keyAt(s0, 120) >= 0
            && sameState(stateAt(s0, 48), startHere)
            && s0->camera.keys.at(keyAt(s0, 48)).ease == Ease::Linear
            && s0->camera.keys.at(keyAt(s0, 120)).ease == Ease::Hold;
        check(QStringLiteral("(qm.2) a move replaces the keys inside its span and the one on its "
                             "end, never the keys outside; the START is the camera as it was, "
                             "with the chosen ease (Linear); the END keeps the ease that was there "
                             "(Hold)"),
              outsideKept && insideGone && ends && shotcam::liveKeyCount(*s0) == 4,
              QStringLiteral("outside %1 inside gone %2 ends %3 keys %4").arg(outsideKept)
                  .arg(insideGone).arg(ends).arg(shotcam::liveKeyCount(*s0)));
        // Chaining: a Push In on panel 3 starts where the pan ended.
        const CameraState panEnd = stateAt(s0, 120);
        clickThumb(r, 2);
        r.storyboard->cameraQuickMove(QuickMove::PushIn);
        pump(60);
        check(QStringLiteral("(qm.2) moves chain: a Push In on the next panel starts on the pan's "
                             "end framing"),
              sameState(stateAt(s0, 120), panEnd) && keyAt(s0, 216) >= 0
                  && qAbs(stateAt(s0, 216).zoom - panEnd.zoom * 1.25) < 1e-9);
        r.storyboard->setQuickEase(Ease::EaseInOut);
        clickThumb(r, 1);
    }

    // --- (qm.3) the other spans ------------------------------------------------
    {
        setCam(s0, {});
        r.storyboard->setQuickSpan(StoryboardPage::QuickSpan::ToSceneEnd);
        r.storyboard->cameraQuickMove(QuickMove::PushIn);
        pump(60);
        const bool toEnd = keyAt(s0, 48) >= 0 && keyAt(s0, 264) >= 0 && shotcam::liveKeyCount(*s0) == 2;
        // The loop region: panels 3-4 of scene 1.
        r.storyboard->setQuickSpan(StoryboardPage::QuickSpan::LoopRegion);
        r.storyboard->clearNoteForTest();
        r.storyboard->cameraQuickMove(QuickMove::PushIn);
        const QString noLoop = r.storyboard->lastNoteForTest();
        r.animatic->setPlayheadForTest(2, 0);
        if (auto *b = r.window.findChild<QPushButton *>(QStringLiteral("animaticLoopStart")))
            b->click();
        r.animatic->setPlayheadForTest(3, 0);
        if (auto *b = r.window.findChild<QPushButton *>(QStringLiteral("animaticLoopEnd")))
            b->click();
        pump(60);
        setCam(s0, {});
        r.storyboard->cameraQuickMove(QuickMove::PushIn);
        pump(60);
        const bool loop = keyAt(s0, 120) >= 0 && keyAt(s0, 264) >= 0 && shotcam::liveKeyCount(*s0) == 2;
        r.storyboard->setQuickSpan(StoryboardPage::QuickSpan::ThisPanel);
        if (auto *b = r.window.findChild<QPushButton *>(QStringLiteral("animaticLoopClear")))
            b->click();
        check(QStringLiteral("(qm.3) To the scene's end spans panel 2 to the scene's end (48-264); "
                             "the loop region spans its panels (120-264)"),
              toEnd && loop, QStringLiteral("to end %1 loop %2").arg(toEnd).arg(loop));
        check(QStringLiteral("(qm.3) control: with no loop region the loop span says so and adds "
                             "nothing"),
              noLoop == QStringLiteral("Set a loop region inside one scene first."), noLoop);
    }

    // --- (qm.4) the lock and the covered canvas ---------------------------------
    {
        setCam(s0, {});
        r.animatic->setTrackLocked(TimelineTrack::Camera, true);
        r.storyboard->clearLockRefusalForTest();
        const int index = stack->index();
        r.storyboard->cameraQuickMove(QuickMove::PushIn);
        const bool refused = stack->index() == index
            && r.storyboard->lastLockRefusalForTest() == QStringLiteral("CAMERA track is locked");
        r.animatic->setTrackLocked(TimelineTrack::Camera, false);
        r.storyboard->cameraQuickMove(QuickMove::PushIn);
        const bool accepted = stack->index() == index + 1;
        r.window.undoActionForTest()->trigger();
        check(QStringLiteral("(qm.4) the CAMERA lock refuses a Quick Move with its message"), refused);
        check(QStringLiteral("(qm.4) control: unlocked, the same move is one command"), accepted);
        const bool enabledBefore =
            r.storyboard->cameraBarStateForTest().value(QStringLiteral("camQuickMove.enabled")).toBool();
        r.animatic->scrubForTest(1);
        pump(150);
        const bool coveredOff =
            !r.storyboard->cameraBarStateForTest().value(QStringLiteral("camQuickMove.enabled")).toBool();
        r.animatic->leavePreview();
        pump(100);
        check(QStringLiteral("(qm.4) with the preview over the canvas the Quick Move button is "
                             "disabled (and enabled without it)"),
              coveredOff && enabledBefore);
    }

    // --- (qm.5) the amounts, remembered between launches -----------------------
    {
        shotcam::QuickAmounts a;
        a.zoom = 1.5;
        a.move = 0.1;
        a.turn = 15.0;
        r.storyboard->setQuickAmounts(a);
        Rig second(project);
        const shotcam::QuickAmounts b = second.storyboard ? second.storyboard->quickAmounts()
                                                          : shotcam::QuickAmounts();
        check(QStringLiteral("(qm.5) Zoom, Move and Turn are remembered: a new window opens with "
                             "1.5x, 10 %, 15 deg"),
              second.ok && qAbs(b.zoom - 1.5) < 1e-9 && qAbs(b.move - 0.1) < 1e-9
                  && qAbs(b.turn - 15.0) < 1e-9,
              QStringLiteral("%1 %2 %3").arg(b.zoom).arg(b.move).arg(b.turn));
        r.storyboard->setQuickAmounts(shotcam::QuickAmounts());
        check(QStringLiteral("(qm.5) control: the defaults are 1.25x, 25 %, 10 deg"),
              qAbs(shotcam::QuickAmounts().zoom - 1.25) < 1e-9 && qAbs(shotcam::QuickAmounts().move - 0.25) < 1e-9);
    }

    // --- (qm.6) the menus ----------------------------------------------------------
    {
        setCam(s0, {});
        const int rowY = r.timeline->cameraRowForTest().y() + 14;
        const QRect clip2 = r.timeline->clipRectForTest(1);
        QStringList entries;
        bool triggered = false;
        menuHooked([&] {
            const QPoint at(clip2.center().x(), rowY);
            QContextMenuEvent ev(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
            QCoreApplication::sendEvent(r.surface, &ev);
        }, [&](QMenu *m) {
            if (QMenu *q = m->findChild<QMenu *>(QStringLiteral("cameraQuickMenu"))) {
                entries << q->title();
                for (QAction *a : q->actions())
                    entries << a->objectName();
                if (QAction *a = q->findChild<QAction *>(QStringLiteral("quickPanRight"))) {
                    a->trigger();
                    triggered = true;
                }
            }
        });
        const bool panned = triggered && lastText() == QStringLiteral("Quick Camera Move")
            && keyAt(s0, 48) >= 0 && keyAt(s0, 120) >= 0;
        check(QStringLiteral("(qm.6) the CAMERA row's menu off a key: Quick Move on Panel 2 with the "
                             "eight moves; Pan Right applies on that panel"),
              entries.value(0) == QStringLiteral("Quick Move on Panel 2") && entries.size() == 9 && panned,
              entries.join(QLatin1Char(',')));
        QString clipSub;
        int clipCount = 0;
        r.storyboard->setClipMenuHookForTest([&](QMenu *m) {
            for (QMenu *q : m->findChildren<QMenu *>(QStringLiteral("cameraQuickMenu"))) {
                clipSub = q->title();
                clipCount = q->actions().size();
            }
        });
        const QPoint c = clip2.center();
        QContextMenuEvent ev(QContextMenuEvent::Mouse, c, r.surface->mapToGlobal(c));
        QCoreApplication::sendEvent(r.surface, &ev);
        r.storyboard->setClipMenuHookForTest({});
        check(QStringLiteral("(qm.6) the SHOTS clip menu has Quick Camera Move with the eight moves"),
              clipSub == QStringLiteral("Quick Camera Move") && clipCount == 8, clipSub);
        r.window.undoActionForTest()->trigger();
        pump(60);
    }

    // --- (qm.7) Use Last Framing / Continue Camera -------------------------------
    {
        // Scene 1 ends on F (its last key); scene 2 moves P -> Q.
        const CameraState F{1100, 600, 2.0, 0};
        setCam(s0, {shotcam::makeKey(*s0, 0, 0, 24, z2), shotcam::makeKey(*s0, 3, 48, 24, F)});
        const CameraState P{900, 500, 1.5, 0}, Q{1000, 560, 1.8, 0};
        setCam(s1, {shotcam::makeKey(*s1, 1, 0, 24, P), shotcam::makeKey(*s1, 2, 48, 24, Q)});
        const QByteArray before = json(s1);
        const CameraState lastOf0 = shotcam::evaluateAtPanel(*s0, 3, 47, 24);
        QString useText, contText, s0Use, s0Cont;
        bool s0UseOn = true, s0ContOn = true;
        auto sceneMenu = [&](int i, const std::function<void(QMenu *)> &fn) {
            menuHooked([&] { r.storyboard->sceneMenuForTest(i); }, fn);
        };
        sceneMenu(1, [&](QMenu *m) {
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraUseLastFraming"))) {
                useText = a->text();
                a->trigger();
            }
        });
        const bool used = shotcam::liveKeyCount(*s1) == 1 && sameState(stateAt(s1, 0), lastOf0)
            && lastText() == QStringLiteral("Use Last Framing");
        r.window.undoActionForTest()->trigger();
        pump(60);
        const bool usedUndone = json(s1) == before;
        sceneMenu(1, [&](QMenu *m) {
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraContinue"))) {
                contText = a->text();
                a->trigger();
            }
        });
        const bool continued = shotcam::liveKeyCount(*s1) == 3 && sameState(stateAt(s1, 0), lastOf0)
            && keyAt(s1, 48) >= 0 && keyAt(s1, 168) >= 0 && lastText() == QStringLiteral("Continue Camera");
        r.window.undoActionForTest()->trigger();
        pump(60);
        sceneMenu(0, [&](QMenu *m) {
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraUseLastFraming"))) {
                s0Use = a->text();
                s0UseOn = a->isEnabled();
            }
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraContinue"))) {
                s0Cont = a->text();
                s0ContOn = a->isEnabled();
            }
        });
        check(QStringLiteral("(qm.7) \"Use Last Framing of Scene 1 (replaces 2 keys)\": scene 2 "
                             "holds scene 1's last framing, still; Undo restores it byte for byte"),
              useText == QStringLiteral("Use Last Framing of Scene 1 (replaces 2 keys)") && used
                  && usedUndone,
              useText);
        check(QStringLiteral("(qm.7) \"Continue Camera from Scene 1\": scene 2 starts on that "
                             "framing and keeps its own keys"),
              contText == QStringLiteral("Continue Camera from Scene 1") && continued, contText);
        check(QStringLiteral("(qm.7) on scene 1 both are disabled: no scene before it"),
              !s0UseOn && !s0ContOn
                  && s0Use == QString::fromUtf8("Use Last Framing \xE2\x80\x94 no scene before this one")
                  && s0Cont == QString::fromUtf8("Continue Camera \xE2\x80\x94 no scene before this one"),
              s0Use);
        setCam(s0, {});
        QString wholeText;
        sceneMenu(1, [&](QMenu *m) {
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraUseLastFraming")))
                wholeText = a->text();
        });
        check(QStringLiteral("(qm.7) control: when scene 1 has no camera the entry says \"(the whole "
                             "canvas)\""),
              wholeText == QStringLiteral("Use Last Framing of Scene 1 (the whole canvas)"), wholeText);
    }

    // --- (qm.8) the menus act on the panel they name, whatever Over says --------
    // The 1D hand test: Over left at "To the scene's end" made the row menu's
    // "Quick Move on Panel 2" run to the scene's last panel.
    {
        setCam(s0, {});
        r.storyboard->setQuickSpan(StoryboardPage::QuickSpan::ToSceneEnd);
        const int rowY = r.timeline->cameraRowForTest().y() + 14;
        auto pickFromMenu = [&](const QPoint &at) {
            bool picked = false;
            menuHooked([&] {
                QContextMenuEvent ev(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
                QCoreApplication::sendEvent(r.surface, &ev);
            }, [&](QMenu *m) {
                if (QMenu *q = m->findChild<QMenu *>(QStringLiteral("cameraQuickMenu")))
                    if (QAction *a = q->findChild<QAction *>(QStringLiteral("quickPanRight"))) {
                        a->trigger();
                        picked = true;
                    }
            });
            return picked;
        };
        // Panel 2 is frames 48-120, panel 3 120-216, the scene ends at 264.
        const bool rowPicked = pickFromMenu(QPoint(r.timeline->clipRectForTest(1).center().x(), rowY));
        const bool rowOnPanel = rowPicked && keyAt(s0, 48) >= 0 && keyAt(s0, 120) >= 0
            && keyAt(s0, 264) < 0 && shotcam::liveKeyCount(*s0) == 2;
        r.window.undoActionForTest()->trigger();
        pump(60);
        const bool clipPicked = pickFromMenu(r.timeline->clipRectForTest(2).center());
        const bool clipOnPanel = clipPicked && keyAt(s0, 120) >= 0 && keyAt(s0, 216) >= 0
            && keyAt(s0, 264) < 0 && shotcam::liveKeyCount(*s0) == 2;
        r.window.undoActionForTest()->trigger();
        pump(60);
        check(QStringLiteral("(qm.8) with Over at \"To the scene's end\", the CAMERA row's \"Quick "
                             "Move on Panel 2\" keys panel 2 only (48 / 120) and the clip menu's "
                             "panel 3 only (120 / 216)"),
              rowOnPanel && clipOnPanel,
              QStringLiteral("row %1 clip %2 keys %3").arg(rowOnPanel).arg(clipOnPanel)
                  .arg(shotcam::liveKeyCount(*s0)));
        // Control: the popover (the canvas on panel 2 - the clip's right-click
        // selected panel 3) does follow Over.
        clickThumb(r, 1);
        pump(60);
        r.storyboard->cameraQuickMove(QuickMove::PanRight);
        pump(60);
        const bool popoverToEnd = keyAt(s0, 48) >= 0 && keyAt(s0, 264) >= 0 && keyAt(s0, 120) < 0;
        r.window.undoActionForTest()->trigger();
        pump(60);
        check(QStringLiteral("(qm.8) control: the popover's own move with the same Over runs from "
                             "panel 2 to the scene's end (48 / 264)"),
              popoverToEnd);
        r.storyboard->setQuickSpan(StoryboardPage::QuickSpan::ThisPanel);
    }

    // --- (qm.9) Over shows its longest entry whole ------------------------------
    {
        auto *overC = pop->findChild<QComboBox *>(QStringLiteral("camQuickOver"));
        const QString longest = QStringLiteral("To the scene's end");
        auto textRoom = [&](int width) {
            QStyleOptionComboBox opt;
            opt.initFrom(overC);
            opt.rect = QRect(0, 0, width, qMax(overC->height(), overC->sizeHint().height()));
            opt.editable = false;
            opt.subControls = QStyle::SC_All;
            return overC->style()
                ->subControlRect(QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, overC)
                .width();
        };
        const int need = overC ? overC->fontMetrics().horizontalAdvance(longest) : 0;
        const int room = overC ? textRoom(overC->width()) : 0;
        check(QStringLiteral("(qm.9) the Over field has room for \"To the scene's end\" and 4 px to "
                             "spare"),
              overC && room >= need + 4, QStringLiteral("room %1 need %2").arg(room).arg(need));
        check(QStringLiteral("(qm.9) control: at the old 120 px it had not"),
              overC && textRoom(120) < need,
              QStringLiteral("room %1 need %2").arg(overC ? textRoom(120) : 0).arg(need));
    }

    // --- (qm.10) the amounts take the wheel only once clicked -------------------
    {
        if (quickBtn)
            quickBtn->click(); // the fields show the amounts (1.25x, 25 %, 10 deg)
        pump(60);
        pop->hide(); // closing disarms them
        pump(30);
        QList<QAbstractSpinBox *> boxes{
            pop->findChild<QAbstractSpinBox *>(QStringLiteral("camQuickZoom")),
            pop->findChild<QAbstractSpinBox *>(QStringLiteral("camQuickMoveAmount")),
            pop->findChild<QAbstractSpinBox *>(QStringLiteral("camQuickTurn"))};
        auto wheel = [](QAbstractSpinBox *b) {
            QWheelEvent ev(QPointF(10, 8), b->mapToGlobal(QPointF(10, 8)), QPoint(), QPoint(0, 120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(b, &ev);
        };
        const shotcam::QuickAmounts before = r.storyboard->quickAmounts();
        int found = 0;
        for (QAbstractSpinBox *b : boxes)
            if (b) {
                ++found;
                wheel(b);
            }
        const shotcam::QuickAmounts afterWheel = r.storyboard->quickAmounts();
        const bool unmoved = found == 3 && afterWheel.zoom == before.zoom
            && afterWheel.move == before.move && afterWheel.turn == before.turn;
        for (QAbstractSpinBox *b : boxes)
            if (b) {
                QWidget *text = b->findChild<QLineEdit *>();
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(4, 4), text->mapToGlobal(QPointF(4, 4)),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(text, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(4, 4),
                                    text->mapToGlobal(QPointF(4, 4)), Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QCoreApplication::sendEvent(text, &release);
                wheel(b);
            }
        const shotcam::QuickAmounts clicked = r.storyboard->quickAmounts();
        check(QStringLiteral("(qm.10) a wheel over Zoom, Move or Turn changes nothing until the field "
                             "is clicked"),
              unmoved, QStringLiteral("%1 %2 %3").arg(afterWheel.zoom).arg(afterWheel.move).arg(afterWheel.turn));
        check(QStringLiteral("(qm.10) control: clicked first, the same wheel steps each one (1.30x, "
                             "30 %, 11 deg)"),
              qAbs(clicked.zoom - 1.30) < 1e-9 && qAbs(clicked.move - 0.30) < 1e-9
                  && qAbs(clicked.turn - 11.0) < 1e-9,
              QStringLiteral("%1 %2 %3").arg(clicked.zoom).arg(clicked.move).arg(clicked.turn));
        r.storyboard->setQuickAmounts(shotcam::QuickAmounts());
    }

    // --- (qm.11) the Scenes dock's cards have the scene's camera menu -----------
    QFrame *cards[2] = {nullptr, nullptr};
    for (QFrame *f : r.window.findChildren<QFrame *>(QStringLiteral("sceneCard"))) {
        const int i = f->property("sceneIndex").toInt();
        if (i >= 0 && i < 2)
            cards[i] = f;
    }
    auto cardMenu = [&](int i, const std::function<void(QMenu *)> &fn) {
        menuHooked([&] {
            // On the card's label, as a hand does: it reaches the card.
            QWidget *at = cards[i] ? cards[i]->findChild<QLabel *>() : nullptr;
            if (!at)
                at = cards[i];
            if (!at)
                return;
            QContextMenuEvent ev(QContextMenuEvent::Mouse, QPoint(4, 4), at->mapToGlobal(QPoint(4, 4)));
            QCoreApplication::sendEvent(at, &ev);
        }, fn);
    };
    {
        const CameraState F{1100, 600, 2.0, 0};
        setCam(s0, {shotcam::makeKey(*s0, 0, 0, 24, z2), shotcam::makeKey(*s0, 3, 48, 24, F)});
        const CameraState P{900, 500, 1.5, 0};
        setCam(s1, {shotcam::makeKey(*s1, 1, 0, 24, P)});
        const QByteArray before = json(s1);
        const CameraState lastOf0 = shotcam::evaluateAtPanel(*s0, 3, 47, 24);
        QString useText, s0Use;
        bool s0UseOn = true;
        cardMenu(1, [&](QMenu *m) {
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraUseLastFraming"))) {
                useText = a->text();
                a->trigger();
            }
        });
        const bool used = shotcam::liveKeyCount(*s1) == 1 && sameState(stateAt(s1, 0), lastOf0)
            && lastText() == QStringLiteral("Use Last Framing");
        r.window.undoActionForTest()->trigger();
        pump(60);
        const bool undone = json(s1) == before;
        cardMenu(0, [&](QMenu *m) {
            if (QAction *a = m->findChild<QAction *>(QStringLiteral("cameraUseLastFraming"))) {
                s0Use = a->text();
                s0UseOn = a->isEnabled();
            }
        });
        check(QStringLiteral("(qm.11) right-clicking scene 2's card in the Scenes dock offers \"Use "
                             "Last Framing of Scene 1 (replaces 1 key)\": the same edit, one Undo"),
              cards[0] && cards[1] && useText == QStringLiteral("Use Last Framing of Scene 1 (replaces 1 key)")
                  && used && undone,
              useText);
        check(QStringLiteral("(qm.11) control: scene 1's card has it disabled - no scene before it"),
              !s0UseOn && s0Use == QString::fromUtf8("Use Last Framing \xE2\x80\x94 no scene before this one"),
              s0Use);
    }

    // --- (qm.12) every 1D menu entry is greyed while the CAMERA track is locked --
    {
        const int rowY = r.timeline->cameraRowForTest().y() + 14;
        const QRect clip6 = r.timeline->clipRectForTest(5); // scene 2's second panel
        auto surfaceMenu = [&](const QPoint &at, const std::function<void(QMenu *)> &fn) {
            menuHooked([&] {
                QContextMenuEvent ev(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
                QCoreApplication::sendEvent(r.surface, &ev);
            }, fn);
        };
        // name -> enabled, for the 1D entries each menu carries.
        auto survey = [&] {
            QMap<QString, bool> on;
            auto scan = [&](const QString &where, QMenu *m) {
                for (QMenu *q : m->findChildren<QMenu *>(QStringLiteral("cameraQuickMenu")))
                    on[where + QStringLiteral(".quick")] = q->menuAction()->isEnabled();
                for (const char *n : {"cameraUseLastFraming", "cameraContinue"})
                    if (QAction *a = m->findChild<QAction *>(QLatin1String(n)))
                        on[where + QLatin1Char('.') + QLatin1String(n)] = a->isEnabled();
            };
            surfaceMenu(QPoint(clip6.center().x(), rowY), [&](QMenu *m) { scan(QStringLiteral("row"), m); });
            surfaceMenu(clip6.center(), [&](QMenu *m) { scan(QStringLiteral("clip"), m); });
            surfaceMenu(QPoint(clip6.center().x(), clip6.top() - 12),
                        [&](QMenu *m) { scan(QStringLiteral("scenes"), m); });
            cardMenu(1, [&](QMenu *m) { scan(QStringLiteral("dock"), m); });
            return on;
        };
        const QStringList want{QStringLiteral("row.quick"), QStringLiteral("row.cameraUseLastFraming"),
                               QStringLiteral("row.cameraContinue"), QStringLiteral("clip.quick"),
                               QStringLiteral("scenes.cameraUseLastFraming"),
                               QStringLiteral("scenes.cameraContinue"),
                               QStringLiteral("dock.cameraUseLastFraming"),
                               QStringLiteral("dock.cameraContinue")};
        auto all = [&](const QMap<QString, bool> &on, bool value) {
            for (const QString &k : want)
                if (!on.contains(k) || on.value(k) != value)
                    return false;
            return true;
        };
        auto listed = [](const QMap<QString, bool> &on) {
            QStringList l;
            for (auto it = on.cbegin(); it != on.cend(); ++it)
                l << it.key() + QLatin1Char('=') + (it.value() ? QLatin1Char('1') : QLatin1Char('0'));
            return l.join(QLatin1Char(' '));
        };
        r.animatic->setTrackLocked(TimelineTrack::Camera, true);
        pump(30);
        const QMap<QString, bool> locked = survey();
        r.animatic->setTrackLocked(TimelineTrack::Camera, false);
        pump(30);
        const QMap<QString, bool> open = survey();
        check(QStringLiteral("(qm.12) CAMERA locked: Quick Move on the row and the clip, and Use Last "
                             "Framing / Continue on the row, the SCENES row and the Scenes dock, are "
                             "all greyed"),
              all(locked, false), listed(locked));
        check(QStringLiteral("(qm.12) control: unlocked, the same eight entries are enabled"),
              all(open, true), listed(open));
    }

    // --- (qm.13) a scene action with nothing to change says so ----------------
    // The round-2 hand test: a Continue on a scene that already started on
    // the previous scene's last framing did nothing, silently (clean-up
    // batch, 2026-10-30). Both scene actions now leave a note instead.
    {
        auto trigger = [&](int sceneIndex, const char *name) {
            menuHooked([&] { r.storyboard->sceneMenuForTest(sceneIndex); }, [&](QMenu *m) {
                if (QAction *a = m->findChild<QAction *>(QLatin1String(name)))
                    a->trigger();
            });
            pump(30);
        };
        const CameraState F{1100, 600, 2.0, 0};
        setCam(s0, {shotcam::makeKey(*s0, 0, 0, 24, F)});
        setCam(s1, {});
        r.storyboard->clearNoteForTest();
        const int i0 = stack->index();
        trigger(1, "cameraContinue");
        const bool firstMoved = stack->index() == i0 + 1 && lastText() == QStringLiteral("Continue Camera")
            && sameState(stateAt(s1, 0), F);
        const QString firstNote = r.storyboard->lastNoteForTest();
        const int i1 = stack->index();
        const QByteArray once = json(s1);
        trigger(1, "cameraContinue");
        const QString againNote = r.storyboard->lastNoteForTest();
        check(QStringLiteral("(qm.13) Continue on a scene that already starts on the previous scene's "
                             "last framing pushes nothing and says \"Scene 2 already starts on Scene 1's "
                             "last framing.\""),
              stack->index() == i1 && json(s1) == once
                  && againNote == QStringLiteral("Scene 2 already starts on Scene 1's last framing."),
              againNote);
        check(QStringLiteral("(qm.13) control: the first Continue is one \"Continue Camera\" edit and "
                             "leaves no note"),
              firstMoved && firstNote.isEmpty(), firstNote);

        // Use Last Framing: scene 1 has no camera and scene 2 no keys.
        setCam(s0, {});
        setCam(s1, {});
        r.storyboard->clearNoteForTest();
        const int i2 = stack->index();
        trigger(1, "cameraUseLastFraming");
        const QString wholeNote = r.storyboard->lastNoteForTest();
        check(QStringLiteral("(qm.13) Use Last Framing with nothing to change (scene 1 the whole canvas, "
                             "scene 2 no keys) pushes nothing and says \"Scene 2 already shows the "
                             "whole canvas.\""),
              stack->index() == i2 && shotcam::liveKeyCount(*s1) == 0
                  && wholeNote == QStringLiteral("Scene 2 already shows the whole canvas."),
              wholeNote);
        setCam(s1, {shotcam::makeKey(*s1, 1, 0, 24, F)});
        r.storyboard->clearNoteForTest();
        const int i3 = stack->index();
        trigger(1, "cameraUseLastFraming");
        const QString clearedNote = r.storyboard->lastNoteForTest();
        check(QStringLiteral("(qm.13) control: with a key on scene 2 the same entry clears it as one "
                             "\"Use Last Framing\" edit and leaves no note"),
              stack->index() == i3 + 1 && lastText() == QStringLiteral("Use Last Framing")
                  && shotcam::liveKeyCount(*s1) == 0 && clearedNote.isEmpty(),
              clearedNote);

        // Use Last Framing twice (the clean-up hand test ran it three times):
        // the second finds scene 2 already holding the framing.
        setCam(s0, {shotcam::makeKey(*s0, 0, 0, 24, F)});
        setCam(s1, {});
        r.storyboard->clearNoteForTest();
        const int i4 = stack->index();
        trigger(1, "cameraUseLastFraming");
        const bool usedOnce = stack->index() == i4 + 1 && lastText() == QStringLiteral("Use Last Framing")
            && sameState(stateAt(s1, 0), F);
        const QString onceNote = r.storyboard->lastNoteForTest();
        const int i5 = stack->index();
        const QByteArray held = json(s1);
        trigger(1, "cameraUseLastFraming");
        const QString holdNote = r.storyboard->lastNoteForTest();
        check(QStringLiteral("(qm.13) Use Last Framing on a scene that already holds that framing "
                             "pushes nothing and says \"Scene 2 already holds Scene 1's last "
                             "framing.\""),
              stack->index() == i5 && json(s1) == held
                  && holdNote == QStringLiteral("Scene 2 already holds Scene 1's last framing."),
              holdNote);
        check(QStringLiteral("(qm.13) control: the first Use Last Framing is one edit and leaves no "
                             "note"),
              usedOnce && onceNote.isEmpty(), onceNote);
    }
    setCam(s0, {});
    setCam(s1, {});
}

void runCameraToolPass(const QString &scratch)
{
    using namespace workspace;
    using namespace camtool;
    out() << "--- (ct) the shot camera's canvas tool (Phase 1B) ---" << Qt::endl;
    sankoSettings().remove(StoryboardPage::kCameraGuidesKey); // the default: off
    const QSize cs(1920, 1080); // sharp limit 1.0: neutral is sharp
    const QString project = writeProject(scratch + QStringLiteral("/camtool"),
                                         QStringLiteral("CamTool"), cs, 24, 2, 4);
    auto rig = std::make_unique<Rig>(project);
    Rig &r = *rig;
    check(QStringLiteral("(ct) the workspace opened"), r.ok);
    if (!r.ok)
        return;
    Scene *scene = r.window.scenesForTest().value(0);
    QUndoStack *stack = r.window.undoStackForTest();
    auto *camBtn = r.storyboard->findChild<QPushButton *>(QStringLiteral("toolCamera"));
    auto *brushBtn = r.storyboard->findChild<QPushButton *>(QStringLiteral("toolBrush"));
    QWidget *bar = r.storyboard->cameraBarForTest();
    if (!scene || scene->panels.size() < 4 || !stack || !camBtn || !brushBtn || !bar) {
        check(QStringLiteral("(ct) found the scene, the history, the tool buttons and "
                             "the Camera bar"), false);
        return;
    }
    QVector<QVariantMap> recorded;
    QObject::connect(r.storyboard, &StoryboardPage::shotCameraEdited, r.storyboard,
                     [&recorded](const QVariantMap &e) { recorded.append(e); });
    auto lastText = [&] { return stack->index() > 0 ? stack->text(stack->index() - 1)
                                                    : QString(); };
    auto bars = [&] { return r.storyboard->cameraBarStateForTest(); };
    auto place = [&] { // the whole canvas at half size, centred
        r.canvas->placeViewForTest(0.25, QPointF(cs.width() / 2.0, cs.height() / 2.0),
                                   QPointF(r.canvas->width() / 2.0,
                                           r.canvas->height() / 2.0));
        pump(30);
    };
    const QString dot = QStringLiteral("  ") + QChar(0x00B7) + QStringLiteral("  ");

    clickThumb(r, 1);
    camBtn->click();
    pump(200);
    place();
    check(QStringLiteral("(ct) the Camera tool button selects the tool and shows the "
                         "Camera bar; the mode chip says Animate is off"),
          r.canvas->tool() == DrawingCanvas::Camera && bar->isVisible()
              && r.canvas->shotCameraChipForTest()
                     == QStringLiteral("SHOT CAMERA") + dot + QStringLiteral("ANIMATE OFF"),
          r.canvas->shotCameraChipForTest());
    check(QStringLiteral("(ct) Stay inside artwork starts on, Animate off, the old "
                         "\"Camera Frame\" toggle is gone"),
          bars().value(QStringLiteral("camStayInside.on")).toBool()
              && !bars().value(QStringLiteral("camAnimate.on")).toBool()
              && [&] {
                     for (QAbstractButton *b :
                          r.window.findChildren<QAbstractButton *>())
                         if (b->text() == QStringLiteral("Camera Frame"))
                             return false;
                     return true;
                 }()
              && !r.canvas->isCameraFrameEnabled());

    // ---- Hover (found by the 1B capture: setTool's cancelShape() turned the
    // tracking back off, so no buttonless move ever reached the tool) --------
    {
        const QPolygonF q = r.canvas->shotCameraQuadForTest();
        const QPointF body = centre(q) + QPointF(0, (q.at(2).y() - q.at(0).y()) / 4.0);
        const QPointF away(3, 3);
        mouse(r.canvas, QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        pump(20);
        const QImage idle = grabCanvas(r.canvas);
        mouse(r.canvas, QEvent::MouseMove, body, Qt::NoButton, Qt::NoButton);
        pump(20);
        const QImage hovered = grabCanvas(r.canvas);
        mouse(r.canvas, QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        pump(20);
        const QImage left = grabCanvas(r.canvas);
        const QVector<QPointF> spot{body};
        check(QStringLiteral("(ct) hover: the Camera tool tracks the pointer, and a "
                             "buttonless move over the body tints it"),
              r.canvas->hasMouseTracking() && changedAt(idle, hovered, spot) == 1);
        check(QStringLiteral("(ct) hover control: moving away takes the tint off again"),
              changedAt(idle, left, spot) == 0 && changedAt(hovered, left, spot) == 1);
    }

    // ---- C3 on the canvas -------------------------------------------------
    {
        const QPolygonF drawn = r.canvas->shotCameraQuadForTest();
        const QPolygonF expect = r.canvas->viewTransformForTest().map(
            shotcam::frameQuad(shotcam::neutral(cs), cs));
        check(QStringLiteral("(ct) C3: the frame on the canvas IS frameQuad mapped "
                             "through the view"),
              samePoly(drawn, expect), polyText(drawn) + QStringLiteral(" vs ")
                                           + polyText(expect));
        r.canvas->setViewRotation(15.0);
        r.canvas->toggleFlipH();
        pump(30);
        const QPolygonF turned = r.canvas->shotCameraQuadForTest();
        const QPolygonF turnedExpect = r.canvas->viewTransformForTest().map(
            shotcam::frameQuad(shotcam::neutral(cs), cs));
        const QVector<QPointF> h = r.canvas->shotCameraHandlesForTest();
        check(QStringLiteral("(ct) C3 control: with the view turned 15 degrees and "
                             "flipped the frame turns with it (still frameQuad through "
                             "the NEW view, not the old quad), handles on its corners"),
              samePoly(turned, turnedExpect) && !samePoly(turned, drawn, 1.0)
                  && h.size() == 4 && samePoly(QPolygonF(h), turned),
              polyText(turned));
        // The grab zones are screen-sized: 7 px off a corner grabs it, 15 px
        // out along the diagonal turns, the middle moves, far away nothing.
        const QPointF c = centre(turned);
        const QPointF dir = (turned.at(0) - c) / QLineF(c, turned.at(0)).length();
        const int atCorner = r.canvas->shotCameraHitForTest(turned.at(0) + QPointF(7, -7) * 0.5);
        const int outside = r.canvas->shotCameraHitForTest(turned.at(0) + dir * 15.0);
        const int middle = r.canvas->shotCameraHitForTest(c);
        const int away = r.canvas->shotCameraHitForTest(turned.at(0) + dir * 60.0);
        check(QStringLiteral("(ct) under a turned, flipped view: a corner handle "
                             "grabs, 15 px outside it turns, the body moves, 60 px out "
                             "nothing"),
              atCorner == 1 && outside == 5 && middle == 0 && away == -1,
              QStringLiteral("%1 %2 %3 %4").arg(atCorner).arg(outside).arg(middle).arg(away));
        r.canvas->toggleFlipH();
        r.canvas->resetViewRotation();
        place();
    }

    // ---- C5: navigation never makes a key -----------------------------------
    {
        const int idx0 = stack->index();
        const QByteArray before = json(scene);
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        const QPointF pan0 = r.canvas->viewPanOffset();
        key(r.canvas, QEvent::KeyPress, Qt::Key_Space);
        drag(r.canvas, body, body + QPointF(60, 30));
        key(r.canvas, QEvent::KeyRelease, Qt::Key_Space);
        const QPointF pan1 = r.canvas->viewPanOffset();
        const QPointF body1 = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body1, body1 + QPointF(-40, 20), Qt::NoModifier, Qt::MiddleButton);
        const QPointF pan2 = r.canvas->viewPanOffset();
        const double zoom0 = r.canvas->viewZoom();
        wheel(r.canvas, centre(r.canvas->shotCameraQuadForTest()), Qt::ControlModifier);
        const double zoom1 = r.canvas->viewZoom();
        wheel(r.canvas, centre(r.canvas->shotCameraQuadForTest()), Qt::NoModifier);
        // The pen: the canvas leaves a tablet press with the Camera tool to
        // Qt's mouse synthesis (the same press as the mouse's, above).
        const QPointF pt = centre(r.canvas->shotCameraQuadForTest());
        QTabletEvent tab(QEvent::TabletPress, QPointingDevice::primaryPointingDevice(),
                         pt, r.canvas->mapToGlobal(pt), 0.5, 0, 0, 0, 0, 0,
                         Qt::NoModifier, Qt::LeftButton, Qt::LeftButton);
        QCoreApplication::sendEvent(r.canvas, &tab);
        pump(30);
        check(QStringLiteral("(ct) C5: Space+drag, middle drag, Ctrl+wheel, the plain "
                             "wheel and a pen press over the frame make no key and no "
                             "command"),
              stack->index() == idx0 && json(scene) == before
                  && shotcam::liveKeyCount(*scene) == 0
                  && !r.canvas->shotCameraDraggingForTest(),
              QString::fromUtf8(json(scene)));
        check(QStringLiteral("(ct) C5 control: those inputs reached the canvas - "
                             "Space+drag and the middle drag panned it, Ctrl+wheel "
                             "zoomed it"),
              pan1 != pan0 && pan2 != pan1 && zoom1 != zoom0,
              QStringLiteral("pan %1,%2 -> %3,%4 -> %5,%6 zoom %7 -> %8")
                  .arg(pan0.x()).arg(pan0.y()).arg(pan1.x()).arg(pan1.y())
                  .arg(pan2.x()).arg(pan2.y()).arg(zoom0).arg(zoom1));
        place();
    }

    // Animate on with no keys: the chip names the frame a drag would key.
    pressSwitch(bar, QStringLiteral("camAnimate"));
    const QString chipOn = r.canvas->shotCameraChipForTest();
    pressSwitch(bar, QStringLiteral("camAnimate"));
    check(QStringLiteral("(ct) the Animate switch: on, the chip says \"new key at\" the "
                         "playhead's timecode; off again"),
          chipOn == QStringLiteral("SHOT CAMERA") + dot + QStringLiteral("ANIMATE ON")
                        + dot + QStringLiteral("new key at 00:00:02:00")
              && !r.storyboard->cameraAnimate(),
          chipOn);

    // ---- C5's positive control, the single framing, C9 -------------------------
    auto c9 = [&](const QString &what, const QByteArray &before, const QByteArray &after) {
        stack->undo();
        pump(30);
        const QByteArray undone = json(scene);
        stack->redo();
        pump(30);
        const QByteArray redone = json(scene);
        check(QStringLiteral("(ct) C9 %1: undo gives the camera's JSON back byte for "
                             "byte, redo the edit's").arg(what),
              undone == before && redone == after,
              QString::fromUtf8(undone) + QStringLiteral(" | ") + QString::fromUtf8(redone));
        check(QStringLiteral("(ct) C9 %1 control: the edit changed the JSON").arg(what),
              before != after);
    };
    {
        const int idx0 = stack->index();
        const QByteArray before = json(scene);
        recorded.clear();
        const QPolygonF q = r.canvas->shotCameraQuadForTest();
        const QPointF h0 = r.canvas->shotCameraHandlesForTest().value(0);
        drag(r.canvas, h0, h0 + (centre(q) - h0) * 0.25);
        const QByteArray after = json(scene);
        const shotcam::CameraKey k = scene->camera.keys.value(0);
        check(QStringLiteral("(ct) C5 control: ONE drag of a corner with Animate off "
                             "and no keys adds one key - the scene's single framing, on "
                             "its first frame - and one command, \"Zoom Camera\""),
              stack->index() == idx0 + 1 && shotcam::liveKeyCount(*scene) == 1
                  && k.panelUid == scene->panels.at(0)->uid.value() && k.at == 0.0
                  && lastText() == QStringLiteral("Zoom Camera")
                  && qAbs(k.state.zoom - 4.0 / 3.0) < 0.02,
              lastText() + QStringLiteral(" zoom %1").arg(k.state.zoom));
        check(QStringLiteral("(ct) the edit is reported once for the Dev Recorder "
                             "(shotCamEdit), and the statePoll carries the tool's state"),
              recorded.size() == 1
                  && recorded.at(0).value(QStringLiteral("what")) == QStringLiteral("Zoom Camera")
                  && recorded.at(0).value(QStringLiteral("keysAfter")).toInt() == 1
                  && r.storyboard->shotCamToolDevrecState()
                         .value(QStringLiteral("shotCam.toolActive")).toBool());
        c9(QStringLiteral("(zoom)"), before, after);
        const QString soft = r.canvas->shotCameraChipForTest();
        check(QStringLiteral("(ct) past the sharp limit the chip says Soft and the zoom"),
              soft.endsWith(QStringLiteral("Soft 1.33") + QChar(0x00D7)), soft);
    }
    {
        const QByteArray before = json(scene);
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body, body + QPointF(30, 10));
        const QByteArray after = json(scene);
        check(QStringLiteral("(ct) Animate off with one key: a body drag MOVES the single "
                             "framing - still one key, on the scene's first frame"),
              shotcam::liveKeyCount(*scene) == 1
                  && scene->camera.keys.at(0).panelUid == scene->panels.at(0)->uid.value()
                  && lastText() == QStringLiteral("Move Camera") && before != after,
              lastText());
        c9(QStringLiteral("(move)"), before, after);
    }

    // ---- A click that closes a menu never reaches the canvas (2026-10-26) ----
    // Delivered at the WINDOW-SYSTEM level (QWindowSystemInterface), as a
    // real click is: while a popup is open Windows gives the press to the
    // popup, which closes and - unless stopped - has Qt replay the press to
    // the widget under it. The second 1B hand test lost a camera move that
    // way. Probe: build/_probe/camera1b/replayprobe.
    {
        QWindow *mainWin = r.window.windowHandle();
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        const QPointF g0 = QPointF(r.canvas->mapToGlobal(body.toPoint()));
        const QPointF g1 = g0 + QPointF(40, 0);
        auto ws = [&](QWindow *win, const QPointF &global, QEvent::Type type,
                      Qt::MouseButtons buttons) {
            const QPointF local = global - QPointF(win->mapToGlobal(QPoint(0, 0)));
            QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
                win, local, global, buttons, Qt::LeftButton, type);
        };
        const int idx0 = stack->index();
        const QByteArray before = json(scene);
        QMenu menu(r.canvas);
        menu.addAction(QStringLiteral("Show Grid"));
        menu.popup(r.canvas->mapToGlobal(QPoint(12, 12)));
        pump(120);
        const bool opened = menu.isVisible();
        ws(menu.windowHandle(), g0, QEvent::MouseButtonPress, Qt::LeftButton);
        pump(60);
        ws(mainWin, g1, QEvent::MouseMove, Qt::LeftButton);
        ws(mainWin, g1, QEvent::MouseButtonRelease, Qt::NoButton);
        pump(120);
        check(QStringLiteral("(ct) a click on the camera frame that closes a menu does "
                             "not reach the canvas: no edit, no undo step"),
              opened && !menu.isVisible() && stack->index() == idx0
                  && json(scene) == before && !r.canvas->shotCameraDraggingForTest(),
              lastText());
        ws(mainWin, g0, QEvent::MouseButtonPress, Qt::LeftButton);
        ws(mainWin, g1, QEvent::MouseMove, Qt::LeftButton);
        ws(mainWin, g1, QEvent::MouseButtonRelease, Qt::NoButton);
        pump(120);
        check(QStringLiteral("(ct) click-through control: the same click with no menu "
                             "open starts a drag - one \"Move Camera\""),
              stack->index() == idx0 + 1 && lastText() == QStringLiteral("Move Camera"),
              lastText());
        if (stack->index() == idx0 + 1) {
            stack->undo(); // back to the framing the checks below expect
            pump(30);
        }
    }

    // ---- The polish batch (2026-10-27) -------------------------------------
    // (pz1) A click that closes a menu over a FLOATING BAR does not press
    // the control under it - the third 1B hand test pressed Next Key that
    // way, and Qt's replay tried to activate the bar's window
    // ("requestActivate() ... WindowDoesNotAcceptFocus").
    {
        QWidget *sw = bar->findChild<QWidget *>(QStringLiteral("camAnimate"));
        QWindow *barWin = bar->windowHandle();
        const QPointF g = QPointF(sw->mapToGlobal(sw->rect().center()));
        const bool animate0 = r.storyboard->cameraAnimate();
        int activationWarnings = 0;
        bool closed = false;
        {
            WarningCapture warnings;
            QMenu menu(r.canvas);
            menu.addAction(QStringLiteral("Show Grid"));
            menu.popup(r.canvas->mapToGlobal(QPoint(12, 12)));
            pump(120);
            wsMouse(menu.windowHandle(), g, QEvent::MouseButtonPress, Qt::LeftButton);
            pump(60);
            wsMouse(barWin, g, QEvent::MouseButtonRelease, Qt::NoButton);
            pump(120);
            closed = !menu.isVisible();
            activationWarnings = warnings.count(QStringLiteral("requestActivate"));
        }
        check(QStringLiteral("(pz1) a click on a Camera-bar switch that closes a menu does "
                             "not press it, and Qt logs no activation warning"),
              closed && r.storyboard->cameraAnimate() == animate0 && activationWarnings == 0,
              QStringLiteral("animate %1 -> %2, %3 warning(s)").arg(animate0)
                  .arg(r.storyboard->cameraAnimate()).arg(activationWarnings));
        // Compared with the state just before THIS click (the first run, on
        // the unfixed code, had already flipped it with the replayed press).
        const bool beforeControl = r.storyboard->cameraAnimate();
        wsMouse(barWin, g, QEvent::MouseButtonPress, Qt::LeftButton);
        wsMouse(barWin, g, QEvent::MouseButtonRelease, Qt::NoButton);
        pump(80);
        const bool toggled = r.storyboard->cameraAnimate() != beforeControl;
        check(QStringLiteral("(pz1) control: the same click with no menu open presses the "
                             "switch"),
              toggled);
        r.storyboard->setCameraAnimate(animate0);
        QStringList unguarded;
        for (FloatingToolWindow *w : r.window.findChildren<FloatingToolWindow *>())
            if (!w->property("sankoNoClickThrough").toBool())
                unguarded << (w->objectName().isEmpty()
                                  ? QString::fromLatin1(w->metaObject()->className())
                                  : w->objectName());
        check(QStringLiteral("(pz1) every floating bar (Camera, Selection, Move, "
                             "Perspective, the toolbars) carries the guard"),
              unguarded.isEmpty(), unguarded.join(QLatin1String(", ")));
    }
    // (pz2) The Dev Recorder's words for Undo / Redo: the history's own text,
    // as Edit > Undo is about to do it.
    {
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body, body + QPointF(30, 0));
        const QString undoStep = r.window.recorderActionDetail(r.window.undoActionForTest())
                                     .value(QStringLiteral("step")).toString();
        stack->undo();
        pump(30);
        const QString redoStep = r.window.recorderActionDetail(r.window.redoActionForTest())
                                     .value(QStringLiteral("step")).toString();
        check(QStringLiteral("(pz2) Edit > Undo's recorder detail names the step it is "
                             "about to undo, and Redo's the step it would redo"),
              undoStep == QStringLiteral("Undo Move Camera")
                  && redoStep == QStringLiteral("Redo Move Camera"),
              undoStep + QStringLiteral(" | ") + redoStep);
        check(QStringLiteral("(pz2) control: any other action adds no detail"),
              r.window.recorderActionDetail(r.window.exportFullArtworkActionForTest())
                  .isEmpty());
    }
    // (pz3) The Perspective tool's hover: its handles grow under an idle
    // pointer - which needs mouse tracking, and setTool's cancelShape()
    // turned it off again (the same fault the Camera tool had).
    {
        r.canvas->setTool(DrawingCanvas::Perspective);
        pump(50);
        const int idxP = stack->index();
        const QPointF vp(r.canvas->width() * 0.3, r.canvas->height() * 0.5);
        mouse(r.canvas, QEvent::MouseButtonPress, vp, Qt::LeftButton, Qt::LeftButton);
        mouse(r.canvas, QEvent::MouseButtonRelease, vp, Qt::LeftButton, Qt::NoButton);
        pump(60);
        const QPointF away(4, 4);
        mouse(r.canvas, QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        pump(30);
        const QImage idle = grabCanvas(r.canvas);
        mouse(r.canvas, QEvent::MouseMove, vp, Qt::NoButton, Qt::NoButton);
        pump(30);
        const QImage hovered = grabCanvas(r.canvas);
        mouse(r.canvas, QEvent::MouseMove, away, Qt::NoButton, Qt::NoButton);
        pump(30);
        const QImage left = grabCanvas(r.canvas);
        // The handle's rim: a ring of points a few px out from its centre.
        QVector<QPointF> rim;
        for (int a = 0; a < 360; a += 30)
            rim << vp + QPointF(9 * std::cos(qDegreesToRadians(double(a))),
                                9 * std::sin(qDegreesToRadians(double(a))));
        const bool created = stack->index() == idxP + 1;
        const int grew = changedAt(idle, hovered, rim);
        check(QStringLiteral("(pz3) Perspective tool: it tracks the pointer, and an idle "
                             "pointer over a vanishing point grows its handle"),
              created && r.canvas->hasMouseTracking() && grew >= 3,
              QStringLiteral("vp %1 tracking %2 rim changed %3/12").arg(created)
                  .arg(r.canvas->hasMouseTracking()).arg(grew));
        check(QStringLiteral("(pz3) control: moving away shrinks it back"),
              changedAt(idle, left, rim) == 0,
              QStringLiteral("%1/12").arg(changedAt(idle, left, rim)));
        if (created) {
            stack->undo(); // the vanishing point goes
            pump(30);
        }
        r.canvas->setTool(DrawingCanvas::Camera);
        pump(50);
    }
    // (pz5) NO TOOL BUTTON ANYWHERE HAS A PIXEL FONT (clean-up batch 2,
    // 2026-10-30). The clean-up hand test logged the warning again, as the
    // pointer left a Quick Move tile - a tool button with NO menu. From the
    // Qt 6.11.1 source (probe build/_probe/cleanup2/askprobe): on every hover
    // QToolButton hit-tests its sub-controls; for a style-sheeted button the
    // menu area is sized by QStyleSheetStyle from the base style's
    // PM_MenuButtonIndicator (menu or not), and the Windows 11 style reads
    // the font's POINT size there - -1 for a pixel font. So the rule is every
    // tool button, not only those with a menu. Twelve had one (the eight
    // tiles, Quick Move, two panel close buttons, the size preset pill); all
    // now give it in points, pixel-identical at 96 dpi (probe pxpt). Placed
    // BEFORE (pz4): the style caches the answer per launch, and (pz4)'s
    // control asks it on purpose - so that control still warning below also
    // proves this sweep left nothing cached.
    {
        auto pixelFonted = [] {
            QStringList bad;
            for (QWidget *w : QApplication::allWidgets())
                if (auto *tb = qobject_cast<QToolButton *>(w)) {
                    tb->ensurePolished();
                    if (tb->font().pointSize() <= 0)
                        bad << QStringLiteral("%1/%2 (%3 px)")
                                   .arg(tb->parentWidget() ? tb->parentWidget()->objectName() : QString(),
                                        tb->objectName())
                                   .arg(tb->font().pixelSize());
                }
            return bad;
        };
        QWidget *pop = r.storyboard->cameraQuickPopoverForTest();
        int tiles = pop ? int(pop->findChildren<QToolButton *>().size()) : 0;
        const QStringList bad = pixelFonted();
        check(QStringLiteral("(pz5) NO TOOL BUTTON ANYWHERE IN THE APP has a font without a point "
                             "size (a pixel size) - the Quick Move popover's tiles included"),
              bad.isEmpty() && tiles >= 8,
              QStringLiteral("%1 tile(s); %2").arg(tiles).arg(bad.join(QStringLiteral("; "))));
        // Control: the census sees a tool button with the old pixel sheet.
        QStringList withOld;
        {
            auto *old = new QToolButton(&r.window);
            old->setObjectName(QStringLiteral("pz5OldPixelFont"));
            old->setStyleSheet(QStringLiteral(
                "QToolButton { border: 1px solid #2a2a2a; font-size: 11px; padding: 4px; }"));
            withOld = pixelFonted();
            delete old;
        }
        check(QStringLiteral("(pz5) control: a tool button given the old pixel font is found by the "
                             "same census"),
              withOld.size() == bad.size() + 1
                  && withOld.filter(QStringLiteral("pz5OldPixelFont")).size() == 1,
              withOld.join(QStringLiteral("; ")));
        // Live: the pointer over every tile and away, at the window-system
        // level, as in the recording.
        int tileWarnings = -1;
        QToolButton *quick = bar->findChild<QToolButton *>(QStringLiteral("camQuickMove"));
        if (pop && quick) {
            quick->click();
            pump(150);
            if (pop->isVisible()) {
                WarningCapture warnings;
                QWindow *pwin = pop->windowHandle();
                for (QToolButton *t : pop->findChildren<QToolButton *>()) {
                    const QPoint c = t->mapToGlobal(t->rect().center());
                    wsMouse(pwin, QPointF(c), QEvent::MouseMove, Qt::NoButton);
                    pump(30);
                    wsMouse(pwin, QPointF(c + QPoint(0, t->height())), QEvent::MouseMove, Qt::NoButton);
                    pump(30);
                }
                tileWarnings = warnings.count(QStringLiteral("setPointSize"));
            }
            pop->hide();
            pump(60);
        }
        check(QStringLiteral("(pz5) the pointer over each Quick Move tile and away logs no "
                             "\"QFont::setPointSize\" warning (that it could: (pz4)'s control below)"),
              tileWarnings == 0, QStringLiteral("%1 warning(s)").arg(tileWarnings));
    }

    // (pz4) No Qt warning when the pointer leaves the timeline's speed
    // button. The cause (probe build/_probe/polish/warnprobe): Qt 6.11's
    // Windows 11 style sizes a tool button's menu indicator from the font's
    // POINT size, and the speed button's style sheet gave it PIXELS (point
    // size -1) - "QFont::setPointSize: Point size <= 0 (-1)" on HoverLeave,
    // once per launch (the style caches the answer).
    {
        auto *speed = r.window.findChild<QToolButton *>(QStringLiteral("animaticSpeed"));
        QWindow *win = r.window.windowHandle();
        auto sweep = [&](QWidget *over) {
            const QPoint c = over->mapToGlobal(over->rect().center());
            for (int dy = -90; dy <= 90; dy += 6)
                wsMouse(win, QPointF(c + QPoint(0, dy)), QEvent::MouseMove, Qt::NoButton);
            wsMouse(win, QPointF(c), QEvent::MouseMove, Qt::NoButton);
            pump(120);
            wsMouse(win, QPointF(c + QPoint(160, 120)), QEvent::MouseMove, Qt::NoButton);
            pump(120);
        };
        int speedWarnings = -1;
        if (speed) {
            WarningCapture warnings;
            sweep(speed);
            speedWarnings = warnings.count(QStringLiteral("setPointSize"));
        }
        check(QStringLiteral("(pz4) the pointer over the speed button and away: no "
                             "\"QFont::setPointSize\" warning, and its font has a point "
                             "size"),
              speed && speedWarnings == 0 && speed->font().pointSize() > 0,
              QStringLiteral("%1 warning(s), pointSize %2").arg(speedWarnings)
                  .arg(speed ? speed->font().pointSize() : -99));
        // Control: a tool button with the OLD sheet (font in pixels) and a
        // menu, swept the same way, does warn - so the check can see it (and
        // nothing had cached the bad answer before it).
        int oldWarnings = 0;
        {
            auto *old = new QToolButton(&r.window);
            old->setText(QStringLiteral("1x"));
            old->setPopupMode(QToolButton::InstantPopup);
            old->setStyleSheet(QStringLiteral(
                "QToolButton { font-size: 11px; padding: 0 30px 0 6px; }"
                "QToolButton:hover { color: #7c6ef6; }"
                "QToolButton::menu-indicator { image: none; width: 0px; height: 0px; }"));
            auto *m = new QMenu(old);
            m->addAction(QStringLiteral("2x"));
            old->setMenu(m);
            old->setGeometry(r.window.width() / 2, 60, 60, 28);
            old->show();
            old->raise();
            pump(60);
            WarningCapture warnings;
            sweep(old);
            oldWarnings = warnings.count(QStringLiteral("setPointSize"));
            delete old;
        }
        check(QStringLiteral("(pz4) control: a tool button with the old pixel font and a "
                             "menu, swept the same way, does warn"),
              oldWarnings >= 1, QStringLiteral("%1 warning(s)").arg(oldWarnings));
    }

    // ---- Animate on: a key at the playhead; the rotate zone; Shift -------------
    pressSwitch(bar, QStringLiteral("camAnimate"));
    {
        const QByteArray before = json(scene);
        const QPolygonF q = r.canvas->shotCameraQuadForTest();
        const QPointF c = centre(q);
        const QPointF dir = (q.at(0) - c) / QLineF(c, q.at(0)).length();
        const QPointF from = q.at(0) + dir * 12.0;
        QTransform turn;
        turn.translate(c.x(), c.y());
        turn.rotate(20.0);
        turn.translate(-c.x(), -c.y());
        drag(r.canvas, from, turn.map(from), Qt::ShiftModifier);
        const QByteArray after = json(scene);
        const shotcam::CameraState now = r.storyboard->shotCameraView().state;
        bool inside = true;
        for (const QPointF &p : shotcam::frameQuad(now, cs))
            inside = inside && p.x() >= -1e-6 && p.y() >= -1e-6
                && p.x() <= cs.width() + 1e-6 && p.y() <= cs.height() + 1e-6;
        check(QStringLiteral("(ct) Animate on: a drag in the rotate zone with Shift adds "
                             "a key AT THE PLAYHEAD, turned 15 degrees (20 snapped), and "
                             "Stay inside keeps every corner on the artwork"),
              shotcam::liveKeyCount(*scene) == 2 && now.rotation == 15.0 && inside
                  && lastText() == QStringLiteral("Rotate Camera"),
              QStringLiteral("rot %1 zoom %2 %3").arg(now.rotation).arg(now.zoom)
                  .arg(lastText()));
        c9(QStringLiteral("(rotate)"), before, after);
    }

    // ---- C6: Animate off with two keys refuses --------------------------------
    {
        pressSwitch(bar, QStringLiteral("camAnimate")); // off
        const int idx0 = stack->index();
        const QByteArray before = json(scene);
        r.storyboard->clearNoteForTest();
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body, body + QPointF(-25, 0));
        check(QStringLiteral("(ct) C6: Animate off with two keys - a frame drag is "
                             "refused: no command, the keys unchanged, and the note says "
                             "why"),
              stack->index() == idx0 && json(scene) == before
                  && r.storyboard->lastNoteForTest()
                         == QStringLiteral("Turn on Animate to change the camera at "
                                           "this point."),
              r.storyboard->lastNoteForTest());
        pressSwitch(bar, QStringLiteral("camAnimate")); // on
        drag(r.canvas, body, body + QPointF(-25, 0));
        check(QStringLiteral("(ct) C6 control: the same drag with Animate on is taken - "
                             "one command that updates the key at the playhead"),
              stack->index() == idx0 + 1 && json(scene) != before
                  && shotcam::liveKeyCount(*scene) == 2
                  && lastText() == QStringLiteral("Move Camera"),
              lastText());
    }

    // ---- The bar's keys: Add / Delete, ease, previous / next ------------------
    {
        const int flat = r.storyboard->selectedFlatIndexForTest();
        r.animatic->setPlayheadFrameInPanel(flat, 12);
        pump(30);
        const QVariantMap b0 = bars();
        check(QStringLiteral("(ct) between keys the bar offers Add Key, no Delete, a "
                             "previous key and no next, and the ease of the segment"),
              b0.value(QStringLiteral("camAddKey.text")) == QStringLiteral("Add Key")
                  && !b0.value(QStringLiteral("camDeleteKey.enabled")).toBool()
                  && b0.value(QStringLiteral("camPrevKey.enabled")).toBool()
                  && !b0.value(QStringLiteral("camNextKey.enabled")).toBool()
                  && b0.value(QStringLiteral("camEase.text")) == QStringLiteral("Ease In/Out"),
              b0.value(QStringLiteral("camEase.text")).toString());
        const shotcam::CameraState shot0 = r.storyboard->shotCameraView().state;
        QByteArray before = json(scene);
        clickBar(bar, QStringLiteral("camAddKey"));
        QByteArray after = json(scene);
        const shotcam::CameraState shot1 = r.storyboard->shotCameraView().state;
        check(QStringLiteral("(ct) Add Key adds a key at the playhead without moving the "
                             "shot; the button then reads \"Key set\""),
              shotcam::liveKeyCount(*scene) == 3 && shot0 == shot1
                  && lastText() == QStringLiteral("Add Camera Key")
                  && bars().value(QStringLiteral("camAddKey.text"))
                         == QStringLiteral("Key set")
                  && bars().value(QStringLiteral("camDeleteKey.enabled")).toBool(),
              lastText());
        c9(QStringLiteral("(Add Key)"), before, after);
        const QVariantMap addEvent = recorded.isEmpty() ? QVariantMap() : recorded.last();

        // THE SCENE'S LAST KEY (hand-test follow-up, 2026-10-25): the key
        // just added is the last; its ease would move nothing.
        auto *easeField = bar->findChild<QAbstractButton *>(QStringLiteral("camEase"));
        // What the hover shows: the ToolTip event a hover sends, delivered to
        // the control (disabled or not), with the reveal effects off so the
        // tooltip is up at once and nothing real can decide it. A PLACEHOLDER
        // is shown first: hideText() only starts a hide timer, so a control
        // with no tooltip would otherwise read the previous label (the first
        // run of the last-key control did exactly that).
        auto hoverNote = [&](QWidget *w) {
            if (!w)
                return QStringLiteral("<no control>");
            const bool roll = QApplication::isEffectEnabled(Qt::UI_AnimateTooltip);
            const bool fade = QApplication::isEffectEnabled(Qt::UI_FadeTooltip);
            QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
            QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
            const QString placeholder = QStringLiteral("(no tooltip shown)");
            QToolTip::showText(w->mapToGlobal(QPoint(4, 4)), placeholder, w);
            QHelpEvent ev(QEvent::ToolTip, QPoint(4, 4), w->mapToGlobal(QPoint(4, 4)));
            QCoreApplication::sendEvent(w, &ev);
            QString shown = QToolTip::isVisible() ? QToolTip::text() : QString();
            if (shown == placeholder)
                shown.clear();
            QToolTip::hideText();
            QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, roll);
            QApplication::setEffectEnabled(Qt::UI_FadeTooltip, fade);
            return shown;
        };
        {
            const bool disabledOnLast = easeField && !easeField->isEnabled();
            const QString lastNote = hoverNote(easeField);
            const int idxLast = stack->index();
            r.storyboard->cameraSetEase(shotcam::Ease::Linear); // the backstop
            check(QStringLiteral("(ct) on the scene's last key the Ease field is "
                                 "disabled, its hover says \"No move after this key\", "
                                 "and no ease can be set there"),
                  disabledOnLast && lastNote == StoryboardPage::cameraLastKeyNote()
                      && stack->index() == idxLast,
                  lastNote);
            clickBar(bar, QStringLiteral("camPrevKey")); // the key with a move after it
            const QString midNote = hoverNote(easeField);
            check(QStringLiteral("(ct) last-key control: on a key with a move after it "
                                 "the Ease field is enabled and says nothing"),
                  easeField && easeField->isEnabled() && midNote.isEmpty()
                      && r.animatic->playheadFrameInPanel() == 0,
                  midNote);
        }

        before = json(scene);
        auto *menu = bar->findChild<QMenu *>(QStringLiteral("camEaseMenu"));
        QAction *hold = nullptr;
        for (QAction *a : menu ? menu->actions() : QList<QAction *>())
            if (a->text() == QStringLiteral("Hold"))
                hold = a;
        if (hold)
            hold->trigger();
        pump(30);
        after = json(scene);
        check(QStringLiteral("(ct) the ease menu sets the key's ease, one command, and "
                             "the field and menu show it"),
              hold && lastText() == QStringLiteral("Camera Ease")
                  && bars().value(QStringLiteral("camEase.text")) == QStringLiteral("Hold")
                  && hold->isChecked(),
              lastText());
        c9(QStringLiteral("(ease)"), before, after);
        const QVariantMap easeEvent = recorded.isEmpty() ? QVariantMap() : recorded.last();

        clickBar(bar, QStringLiteral("camNextKey")); // back to the key Add Key made
        before = json(scene);
        clickBar(bar, QStringLiteral("camDeleteKey"));
        after = json(scene);
        check(QStringLiteral("(ct) Delete Key removes the key at the playhead"),
              shotcam::liveKeyCount(*scene) == 2
                  && lastText() == QStringLiteral("Delete Camera Key"));
        c9(QStringLiteral("(Delete Key)"), before, after);
        const QVariantMap deleteEvent = recorded.isEmpty() ? QVariantMap() : recorded.last();
        auto counts = [](const QVariantMap &e) {
            return QStringLiteral("%1 %2->%3").arg(e.value(QStringLiteral("what")).toString())
                .arg(e.value(QStringLiteral("keysBefore")).toInt())
                .arg(e.value(QStringLiteral("keysAfter")).toInt());
        };
        check(QStringLiteral("(ct) the recorder's shotCamEdit counts the keys BEFORE a "
                             "button edit: Add Key 2 -> 3, Delete Key 3 -> 2"),
              counts(addEvent) == QStringLiteral("Add Camera Key 2->3")
                  && counts(deleteEvent) == QStringLiteral("Delete Camera Key 3->2"),
              counts(addEvent) + QStringLiteral(" | ") + counts(deleteEvent));
        check(QStringLiteral("(ct) recorder control: an ease edit, which adds no key, "
                             "logs the same count before and after (3 -> 3)"),
              counts(easeEvent) == QStringLiteral("Camera Ease 3->3"), counts(easeEvent));

        clickBar(bar, QStringLiteral("camPrevKey"));
        const int f1 = r.animatic->playheadFrameInPanel();
        const int s1 = r.storyboard->selectedFlatIndexForTest();
        clickBar(bar, QStringLiteral("camPrevKey"));
        const int s2 = r.storyboard->selectedFlatIndexForTest();
        const int f2 = r.animatic->playheadFrameInPanel();
        clickBar(bar, QStringLiteral("camNextKey"));
        const int s3 = r.storyboard->selectedFlatIndexForTest();
        check(QStringLiteral("(ct) previous / next key move the playhead (and the canvas) "
                             "from key to key, across panels, without the preview"),
              f1 == 0 && s1 == flat && s2 == flat - 1 && f2 == 0 && s3 == flat
                  && !r.animatic->previewVisible(),
              QStringLiteral("%1/%2 %3/%4 %5").arg(s1).arg(f1).arg(s2).arg(f2).arg(s3));

        // THE PREVIEW OVER THE CANVAS (hand-test follow-up, 2026-10-25).
        const QStringList names{QStringLiteral("camAnimate"), QStringLiteral("camPrevKey"),
                                QStringLiteral("camAddKey"), QStringLiteral("camDeleteKey"),
                                QStringLiteral("camNextKey"), QStringLiteral("camEase"),
                                QStringLiteral("camStayInside"), QStringLiteral("camSafeArea"),
                                QStringLiteral("camTitleSafe")};
        auto control = [&](const QString &n) { return bar->findChild<QWidget *>(n); };
        // Play, then pause: the preview stays up over the canvas. (A scrub
        // opened it here until 2026-10-26; with the Camera tool a scrub now
        // stays on the canvas - see "the Camera tool's scrub" below.)
        r.animatic->togglePlay();
        pump(80);
        r.animatic->togglePlay();
        pump(30);
        // The bar's body while covered: SOLID, so the dimmed controls read
        // over a bright picture (2026-10-26); the control is the canvas back.
        auto barAlpha = [&] {
            const QImage img = bar->grab().toImage();
            return img.isNull() ? -1
                : qAlpha(img.pixel(qRound(6 * img.devicePixelRatio()), img.height() / 2));
        };
        const int coveredAlpha = barAlpha();
        QStringList on;
        for (const QString &n : names)
            if (!control(n) || control(n)->isEnabled())
                on << n;
        const QString addNote = hoverNote(control(QStringLiteral("camAddKey")));
        const QString switchNote = hoverNote(control(QStringLiteral("camAnimate")));
        const int idxCovered = stack->index();
        const bool animateBefore = r.storyboard->cameraAnimate();
        pressSwitch(bar, QStringLiteral("camAnimate"));
        clickBar(bar, QStringLiteral("camPrevKey"));
        check(QStringLiteral("(ct) with the preview over the canvas every Camera bar "
                             "control is disabled, the hover says \"Close the preview to "
                             "edit the camera\", and a click changes nothing"),
              r.animatic->previewVisible() && on.isEmpty()
                  && addNote == StoryboardPage::cameraPreviewNote()
                  && switchNote == StoryboardPage::cameraPreviewNote()
                  && stack->index() == idxCovered
                  && r.storyboard->cameraAnimate() == animateBefore,
              on.join(QLatin1Char(',')) + QStringLiteral(" | ") + addNote);
        r.animatic->leavePreview();
        pump(30);
        // Play moved the playhead a frame or two: back onto the key at the
        // panel's start, where the checks below expect it.
        r.animatic->setPlayheadFrameInPanel(flat, 0);
        pump(30);
        QStringList off;
        for (const QString &n : {QStringLiteral("camAnimate"), QStringLiteral("camPrevKey"),
                                 QStringLiteral("camAddKey"), QStringLiteral("camStayInside"),
                                 QStringLiteral("camSafeArea"), QStringLiteral("camTitleSafe")})
            if (!control(n) || !control(n)->isEnabled())
                off << n;
        const QString prevNote = hoverNote(control(QStringLiteral("camPrevKey")));
        check(QStringLiteral("(ct) preview control: with the canvas back the controls "
                             "work again and their own tooltips return"),
              !r.animatic->previewVisible() && off.isEmpty()
                  && prevNote == QStringLiteral("Previous camera key"),
              off.join(QLatin1Char(',')) + QStringLiteral(" | ") + prevNote);
        const int canvasAlpha = barAlpha();
        check(QStringLiteral("(ct) while the preview covers the canvas the Camera bar's "
                             "body is SOLID, so its dimmed controls stay readable"),
              coveredAlpha == 255, QStringLiteral("alpha %1").arg(coveredAlpha));
        check(QStringLiteral("(ct) solid-bar control: with the canvas back the body is "
                             "translucent again (0.65)"),
              canvasAlpha == 166, QStringLiteral("alpha %1").arg(canvasAlpha));
    }

    // ---- Stay inside artwork: the clamp, and its control -----------------------
    {
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body, body + QPointF(-2000, 0));
        const shotcam::CameraState kept = r.storyboard->shotCameraView().state;
        auto onCanvas = [&](const shotcam::CameraState &s) {
            for (const QPointF &p : shotcam::frameQuad(s, cs))
                if (p.x() < -1e-6 || p.y() < -1e-6 || p.x() > cs.width() + 1e-6
                    || p.y() > cs.height() + 1e-6)
                    return false;
            return true;
        };
        pressSwitch(bar, QStringLiteral("camStayInside")); // off
        const QPointF body2 = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body2, body2 + QPointF(-2000, 0));
        const shotcam::CameraState free = r.storyboard->shotCameraView().state;
        stack->undo(); // the frame back on the artwork for what follows
        pump(30);
        pressSwitch(bar, QStringLiteral("camStayInside")); // on again
        check(QStringLiteral("(ct) Stay inside artwork: a drag far off the canvas keeps "
                             "the turned frame on the artwork"),
              onCanvas(kept), QStringLiteral("cx %1").arg(kept.cx));
        check(QStringLiteral("(ct) Stay inside control: switched off, the same drag "
                             "takes the frame off the artwork"),
              !onCanvas(free), QStringLiteral("cx %1").arg(free.cx));
    }

    // ---- The strip's frame outline -----------------------------------------
    {
        auto amberNear = [&](const QImage &img, const QPointF &p) {
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const QPoint q(qRound(p.x()) + dx, qRound(p.y()) + dy);
                    if (!img.rect().contains(q))
                        continue;
                    const QColor c = img.pixelColor(q);
                    if (c.red() > 200 && c.green() > 120 && c.green() < 200 && c.blue() < 110)
                        return true;
                }
            return false;
        };
        auto outlinePoint = [&](const QImage &img) {
            // The thumb's fit (16:9 into the 16:9 well: the whole well).
            const shotcam::CameraState s = shotcam::evaluateAtPanel(*scene, 0, 0, 24);
            const QPolygonF q = shotcam::frameQuad(s, cs);
            const QPointF mid = (q.at(0) + q.at(1)) / 2.0;
            return QPointF(mid.x() * img.width() / cs.width(),
                           mid.y() * img.height() / cs.height());
        };
        const QImage withKeys = thumbs(r.storyboard).value(0)->pixmap().toImage();
        const QPointF p = outlinePoint(withKeys);
        const shotcam::SceneCamera saved = scene->camera;
        r.storyboard->applyCameraForUndo(scene, shotcam::SceneCamera());
        const QImage without = thumbs(r.storyboard).value(0)->pixmap().toImage();
        r.storyboard->applyCameraForUndo(scene, saved);
        check(QStringLiteral("(ct) the strip thumbnail draws the whole artwork with the "
                             "camera's frame outlined (amber: past the sharp limit)"),
              amberNear(withKeys, p), QStringLiteral("at %1,%2").arg(p.x()).arg(p.y()));
        check(QStringLiteral("(ct) strip control: the scene without keys has no outline "
                             "there"),
              !amberNear(without, p));
    }

    // ---- The guides: on with every tool, inset from the camera ------------------
    {
        pressSwitch(bar, QStringLiteral("camSafeArea"));
        check(QStringLiteral("(ct) the Safe Area switch turns the guide on and remembers "
                             "it as the one app setting"),
              r.canvas->isSafeAreaEnabled() && !r.canvas->isTitleSafeEnabled()
                  && sankoSettings().value(StoryboardPage::kCameraGuidesKey).toInt() == 1);
        brushBtn->setChecked(true);
        pump(100);
        const shotcam::CameraState s = r.storyboard->shotCameraView().state;
        const QTransform view = r.canvas->viewTransformForTest();
        const QImage on = grabCanvas(r.canvas);
        r.storyboard->setCameraGuides(false, false);
        const QImage off = grabCanvas(r.canvas);
        const QVector<QPointF> camLine = insetTopEdge(s, cs, 0.05, view);
        const QVector<QPointF> canvasLine =
            insetTopEdge(shotcam::neutral(cs), cs, 0.05, view);
        const int onCam = changedAt(off, on, camLine);
        const int onCanvas = changedAt(off, on, canvasLine);
        check(QStringLiteral("(ct) with the Brush tool the Camera bar is gone and the "
                             "guide STAYS; switched off, its pixels go"),
              r.canvas->tool() == DrawingCanvas::Brush && !bar->isVisible()
                  && onCam >= 6,
              QStringLiteral("camera line %1/7").arg(onCam));
        check(QStringLiteral("(ct) with a pushed-in key the guide is inset from the "
                             "CAMERA's frame: nothing changes along the canvas's own "
                             "inset line"),
              onCanvas <= 2 && !shotcam::isNeutral(s, cs),
              QStringLiteral("canvas line %1/7").arg(onCanvas));
        // Control: a scene without keys puts it back on the canvas's inset.
        const shotcam::SceneCamera saved = scene->camera;
        r.storyboard->applyCameraForUndo(scene, shotcam::SceneCamera());
        const QImage plainOff = grabCanvas(r.canvas);
        r.storyboard->setCameraGuides(true, false);
        const QImage plainOn = grabCanvas(r.canvas);
        r.storyboard->applyCameraForUndo(scene, saved);
        check(QStringLiteral("(ct) guides control: with a neutral camera the guide is the "
                             "canvas's own 5 % inset"),
              changedAt(plainOff, plainOn, canvasLine) >= 6,
              QStringLiteral("%1/7").arg(changedAt(plainOff, plainOn, canvasLine)));
        // Safe Area stays on: remembered for the next window.
    }

    // ---- The note style ---------------------------------------------------
    {
        // Windows reveals a tooltip with a roll or fade effect: until it ends
        // the QTipLabel is not visible and there is nothing to grab (the
        // first run read an invalid colour, #000000, for both the check and
        // its control). With the effects off, showText shows the label at
        // once and it is grabbed before any event - so neither time nor the
        // real pointer can decide the result. Restored after.
        const bool rollWas = QApplication::isEffectEnabled(Qt::UI_AnimateTooltip);
        const bool fadeWas = QApplication::isEffectEnabled(Qt::UI_FadeTooltip);
        QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
        QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
        auto tipImage = [&](const QString &text) {
            QToolTip::showText(r.canvas->mapToGlobal(QPoint(120, 120)), text, r.canvas);
            QWidget *tip = nullptr;
            for (QWidget *w : QApplication::topLevelWidgets())
                if (w->inherits("QTipLabel") && w->isVisible())
                    tip = w;
            return tip ? tip->grab().toImage() : QImage();
        };
        const QImage dark = tipImage(QStringLiteral("Turn on Animate to change the "
                                                    "camera at this point."));
        auto fillOf = [](const QImage &img) {
            return img.isNull() ? QColor()
                                : img.pixelColor(img.width() - qRound(4 * img.devicePixelRatio()),
                                                 img.height() / 2);
        };
        const QColor fill = fillOf(dark);
        const QString sheet = qApp->styleSheet();
        check(QStringLiteral("(ct) notes and tooltips take the dark Sanko style "
                             "(#161616), applied once"),
              fill == QColor(0x16, 0x16, 0x16)
                  && sheet.count(QStringLiteral("QToolTip{")) == 1,
              fill.name());
        QString plain = sheet;
        plain.remove(sankoNoteStyle());
        qApp->setStyleSheet(plain);
        QToolTip::hideText();
        const QColor plainFill = fillOf(tipImage(QStringLiteral("Plain note")));
        qApp->setStyleSheet(sheet);
        QToolTip::hideText();
        check(QStringLiteral("(ct) note style control: without the style the tooltip is "
                             "not #161616"),
              plainFill.isValid() && plainFill != QColor(0x16, 0x16, 0x16),
              plainFill.name());
        QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, rollWas);
        QApplication::setEffectEnabled(Qt::UI_FadeTooltip, fadeWas);
    }

    // ---- The 1080p hint in the New Project dialog -------------------------------
    {
        NewProjectDialog dialog(&r.window);
        const QString hint = dialog.canvasHintForTest();
        const QImage with = dialog.grab().toImage();
        dialog.presetDropdown()->choose(2); // 4K
        const QString none = dialog.canvasHintForTest();
        const QImage without = dialog.grab().toImage();
        const double dpr = with.devicePixelRatio();
        int differ = 0;
        for (int y = qRound(155 * dpr); y < qRound(167 * dpr); ++y)
            for (int x = qRound(180 * dpr); x < qRound(322 * dpr); ++x)
                differ += with.pixel(x, y) != without.pixel(x, y) ? 1 : 0;
        check(QStringLiteral("(ct) the New Project dialog's 1080p preset carries the "
                             "camera hint (soft push-ins, 4K recommended), drawn on the "
                             "Canvas Preset line"),
              hint.contains(QStringLiteral("soft")) && hint.contains(QStringLiteral("4K"))
                  && differ > 20,
              hint + QStringLiteral(" (%1 px)").arg(differ));
        check(QStringLiteral("(ct) hint control: the 4K preset shows no hint"),
              none.isEmpty());
    }

    // ---- Stills Show Full Artwork ------------------------------------------
    {
        QAction *full = r.window.exportFullArtworkActionForTest();
        const QString root = scratch + QStringLiteral("/camtool_png");
        auto exportTo = [&](const QString &name) {
            const QString dir = root + QStringLiteral("/") + name;
            QDir(dir).removeRecursively();
            QDir().mkpath(dir);
            r.window.exportPngForTest(dir);
            QStringList files = QDir(dir).entryList(QDir::Files, QDir::Name);
            QFile f(dir + QStringLiteral("/") + files.value(0));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
        };
        r.window.setExportProgressHookForTest([](int, int) { return true; });
        const QByteArray shot = exportTo(QStringLiteral("shot"));
        if (full)
            full->setChecked(true);
        const QByteArray whole = exportTo(QStringLiteral("whole"));
        if (full)
            full->setChecked(false);
        const shotcam::SceneCamera saved = scene->camera;
        r.storyboard->applyCameraForUndo(scene, shotcam::SceneCamera());
        const QByteArray noCamera = exportTo(QStringLiteral("nocamera"));
        r.storyboard->applyCameraForUndo(scene, saved);
        r.window.setExportProgressHookForTest({});
        check(QStringLiteral("(ct) File > Export > Stills Show Full Artwork: the PNG is "
                             "the whole panel, byte for byte the picture without a "
                             "camera"),
              full && full->isCheckable() && !whole.isEmpty() && whole == noCamera);
        check(QStringLiteral("(ct) Full Artwork control: unchecked, the still is the "
                             "camera's shot"),
              !shot.isEmpty() && shot != noCamera);
    }

    // ---- The Camera tool's scrub, and the chip when soft (2026-10-26) --------
    // With the Camera tool a ruler drag selects the panel under the playhead
    // and stays on the canvas at the exact frame; a drag then keys THAT
    // frame. Panel 2 is 3 s (72 frames at 24 fps): its middle, frame 36, is
    // scene frame 48 + 36 = 84 - 00:00:03:12 of the film.
    {
        camBtn->click();
        pump(150);
        auto rulerDrag = [&](int flatPanel, double frameIn) {
            const double ppf = r.timeline->pxPerFrameForTest();
            const QPointF start(r.timeline->clipRectForTest(0).center().x(), 10);
            const QPointF at(r.timeline->clipRectForTest(flatPanel).left()
                                 + (frameIn + 0.25) * ppf, 10);
            mouse(r.surface, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(r.surface, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
            mouse(r.surface, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
            pump(150);
        };
        if (!r.storyboard->cameraAnimate())
            pressSwitch(bar, QStringLiteral("camAnimate"));
        rulerDrag(1, 36);
        const bool onCanvas = !r.animatic->previewVisible()
            && r.storyboard->selectedFlatIndexForTest() == 1
            && r.animatic->playheadFrameInPanel() == 36;
        const QString chip = r.canvas->shotCameraChipForTest();
        // To the RIGHT: the Stay-inside check above left this framing at the
        // artwork's left edge, where a leftward drag is clamped to no change.
        const QPointF body = centre(r.canvas->shotCameraQuadForTest());
        drag(r.canvas, body, body + QPointF(40, 0));
        const int keyAt84 = shotcam::keyIndexAtFrame(*scene, 84.0, 24);
        check(QStringLiteral("(ct) with the Camera tool a ruler drag to the middle of "
                             "panel 2 stays on the canvas, selects that panel at frame 36, "
                             "and a drag with Animate on keys THAT frame (84)"),
              onCanvas && keyAt84 >= 0 && lastText() == QStringLiteral("Move Camera"),
              QStringLiteral("preview=%1 sel=%2 frame=%3 key=%4")
                  .arg(r.animatic->previewVisible())
                  .arg(r.storyboard->selectedFlatIndexForTest())
                  .arg(r.animatic->playheadFrameInPanel()).arg(keyAt84));
        // The chip, soft (1080p, zoom past 1.0) with Animate on: the frame
        // the drag keys AND "Soft", both.
        const QString soft = QStringLiteral("Soft ");
        check(QStringLiteral("(ct) soft with Animate on, the chip names the key's frame "
                             "AND says Soft: \"... ANIMATE ON  new key at 00:00:03:12  "
                             "Soft ...\""),
              chip.contains(QStringLiteral("ANIMATE ON")) && chip.contains(
                  QStringLiteral("new key at 00:00:03:12")) && chip.contains(soft)
                  && chip.indexOf(QStringLiteral("new key at")) < chip.indexOf(soft),
              chip);
        pressSwitch(bar, QStringLiteral("camAnimate")); // off
        const QString offChip = r.canvas->shotCameraChipForTest();
        check(QStringLiteral("(ct) chip control: with Animate off the soft chip names no "
                             "key frame"),
              offChip.contains(soft) && !offChip.contains(QStringLiteral("key at")),
              offChip);
        // Control: the Brush tool's scrub still previews.
        brushBtn->setChecked(true);
        pump(100);
        rulerDrag(2, 10);
        const bool brushPreviews = r.animatic->previewVisible();
        r.animatic->leavePreview();
        pump(50);
        check(QStringLiteral("(ct) scrub control: with the Brush tool the same ruler "
                             "drag opens the preview, as before"),
              brushPreviews);
    }

    rig.reset(); // the window closes; the guide setting stays
    {
        Rig again(project);
        check(QStringLiteral("(ct) the guides are remembered: a new window opens with "
                             "Safe Area on and Title Safe off"),
              again.ok && again.canvas->isSafeAreaEnabled()
                  && !again.canvas->isTitleSafeEnabled());
        again.storyboard->setCameraGuides(false, false);
    }
    {
        Rig third(project);
        check(QStringLiteral("(ct) guide setting control: switched off, the next window "
                             "opens with both off"),
              third.ok && !third.canvas->isSafeAreaEnabled()
                  && !third.canvas->isTitleSafeEnabled()
                  && sankoSettings().value(StoryboardPage::kCameraGuidesKey).toInt() == 0);
    }
    sankoSettings().remove(StoryboardPage::kCameraGuidesKey);
}

void runTrackLocksPass(const QString &scratch)
{
    using namespace workspace;
    out() << "--- (ax) the tracks' lock, visibility and mute (Part B) ---" << Qt::endl;
    const QString base = scratch + QStringLiteral("/track_locks");
    QDir(base).removeRecursively();
    // Two scenes of three panels; durations 2, 3, 4 s (writeProject's
    // 2 + p % 3), so a reorder shows in the clips' durations.
    const QString project = writeProject(base, QStringLiteral("Locks"), QSize(960, 540), 24, 2, 3);
    Rig r(project);
    check(QStringLiteral("(ax) the rig: the workspace with the timeline"), r.ok);
    if (!r.ok)
        return;
    AnimaticPage *page = r.animatic;
    AnimaticTimeline *tl = r.timeline;
    StoryboardPage *sb = r.storyboard;
    QUndoStack *stack = r.window.undoStackForTest();
    auto control = [&](const char *name) { return tl->trackControlForTest(QLatin1String(name)); };
    auto clickControl = [&](const char *name) {
        if (QToolButton *b = control(name))
            b->click(); // the button's own click: checked flips, clicked(bool) fires
        pump(150);
    };
    auto panels = [&] { return page->itemCountForTest(); };
    auto durations = [&] {
        QStringList d;
        for (int i = 0; i < tl->blockCountForTest(); ++i)
            d << QString::number(tl->blockDurationForTest(i));
        return d.join(QLatin1Char(','));
    };
    QStringList messages; // what the lock hooks were handed
    sb->setLockMessageHookForTest([&](const QString &m) { messages << m; });
    r.window.setLockMessageHookForTest([&](const QString &m) { messages << m; });

    // ---- the controls, and an old project's defaults ----
    {
        // Phase 1C (2026-10-28): the CAMERA row's lock, Animate (at the
        // speaker's x 32) and eye join the seven.
        const char *names[] = {"trackLockScenes", "trackEyeScenes", "trackLockShots",
                               "trackEyeShots", "trackLockAudio", "trackEyeAudio",
                               "trackMuteAudio", "trackLockCamera", "trackAnimateCamera",
                               "trackEyeCamera"};
        int found = 0, unchecked = 0, placed = 0;
        QStringList where;
        const QRect audioRow = tl->audioRowForTest();
        const QRect cameraRow = tl->cameraRowForTest();
        for (const char *n : names) {
            QToolButton *b = control(n);
            if (!b)
                continue;
            ++found;
            if (!b->isChecked())
                ++unchecked;
            const QString name = QLatin1String(n);
            // The icon's x: 8 for the lock, 32 for the speaker, 56 for the
            // eye (the button is 2 px larger each side); the icon centred in
            // its row.
            const int iconX = b->x() + 2;
            const int wantX = name.startsWith(QStringLiteral("trackLock")) ? 8
                : (name.startsWith(QStringLiteral("trackMute"))
                   || name.startsWith(QStringLiteral("trackAnimate"))) ? 32 : 56;
            // SHOTS ends where the CAMERA row begins (Phase 1C); it ended at
            // the AUDIO row before.
            const int rowY = name.endsWith(QStringLiteral("Scenes")) ? 24
                : name.endsWith(QStringLiteral("Shots")) ? 48
                : name.endsWith(QStringLiteral("Camera")) ? cameraRow.y() : audioRow.y();
            const int rowH = name.endsWith(QStringLiteral("Scenes")) ? 24
                : name.endsWith(QStringLiteral("Shots")) ? tl->shotsTrackHeightForTest()
                : name.endsWith(QStringLiteral("Camera")) ? cameraRow.height() : audioRow.height();
            const int wantY = rowY + (rowH - 16) / 2;
            if (iconX == wantX && b->y() + 2 == wantY && b->isVisible())
                ++placed;
            where << QStringLiteral("%1 %2,%3").arg(name).arg(b->x()).arg(b->y());
        }
        const TimelineTracks t = page->tracks();
        check(QStringLiteral("(ax) THE TEN CONTROLS are real buttons over the label column "
                             "(lock at x 8, speaker at 32 on AUDIO and Animate at 32 on CAMERA, "
                             "eye at 56, 16 px icons centred in their rows), all unchecked; a "
                             "project saved without a tracks key loads unlocked, visible and "
                             "unmuted"),
              found == 10 && unchecked == 10 && placed == 10 && t == TimelineTracks(),
              QStringLiteral("found %1 unchecked %2 placed %3 | %4 | audio row %5")
                  .arg(found).arg(unchecked).arg(placed).arg(where.join(QStringLiteral("; ")))
                  .arg(QStringLiteral("%1,%2 %3x%4").arg(audioRow.x()).arg(audioRow.y()).arg(audioRow.width()).arg(audioRow.height())));
    }

    // ---- the icon states: a lock "on" is the accent; hidden and muted read as "off" (dimmed) ----
    {
        using namespace SankoTimelineTheme;
        auto maxAlpha = [](const QPixmap &pm, QColor *colourAtMax) {
            const QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            int best = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.alpha() > best) { best = c.alpha(); if (colourAtMax) *colourAtMax = c; }
                }
            return best;
        };
        QStringList detail;
        bool ok = true;
        for (const char *n : {"trackLockScenes", "trackLockShots", "trackLockAudio"}) {
            QToolButton *b = control(n);
            QColor on;
            const int aOn = b ? maxAlpha(b->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::On), &on) : 0;
            const int aOff = b ? maxAlpha(b->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::Off), nullptr) : 0;
            const bool accent = aOn >= 200 && aOff >= 200 && on.red() == kTrackControlActive.red()
                && on.green() == kTrackControlActive.green() && on.blue() == kTrackControlActive.blue();
            ok = ok && accent;
            detail << QStringLiteral("%1 on %2 (%3) off %4").arg(QLatin1String(n)).arg(aOn).arg(on.name()).arg(aOff);
        }
        for (const char *n : {"trackEyeScenes", "trackEyeShots", "trackEyeAudio"}) {
            QToolButton *b = control(n);
            const int aOn = b ? maxAlpha(b->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::On), nullptr) : 0;
            const int aOff = b ? maxAlpha(b->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::Off), nullptr) : 0;
            // A thin glyph's antialiased strokes may never reach 255 (the eye
            // peaks at 246 at 16 px): "opaque" is at least 200, and the
            // dimmed one at most 40 % of it.
            const bool dimmed = aOff >= 200 && aOn > 0 && aOn * 10 <= aOff * 4;
            ok = ok && dimmed;
            detail << QStringLiteral("%1 on %2 off %3").arg(QLatin1String(n)).arg(aOn).arg(aOff);
        }
        {
            QToolButton *b = control("trackMuteAudio");
            const QPixmap on = b ? b->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::On) : QPixmap();
            const QPixmap off = b ? b->icon().pixmap(QSize(16, 16), QIcon::Normal, QIcon::Off) : QPixmap();
            const int aOn = maxAlpha(on, nullptr), aOff = maxAlpha(off, nullptr);
            const bool mutedGlyph = aOn >= 200 && aOff >= 200 && on.toImage() != off.toImage();
            ok = ok && mutedGlyph;
            detail << QStringLiteral("trackMuteAudio on %1 off %2 differ %3 (the muted glyph, Part D)")
                          .arg(aOn).arg(aOff).arg(on.toImage() != off.toImage());
        }
        check(QStringLiteral("(ax) ICON STATES: a lock's checked icon is the accent at full "
                             "opacity; the eyes' checked icons are the grey at 35 % (at most "
                             "40 % of their unchecked opacity) and their unchecked icons opaque; "
                             "the speaker's checked icon is the muted glyph, opaque and its own"),
              ok, detail.join(QStringLiteral("; ")));
    }

    // ---- locked reads as locked: a closed padlock (clean-up batch, 2026-10-30) ----
    {
        using namespace SankoTimelineTheme;
        const QSize box(16, 16);
        auto render = [&](const char *file, qreal scale, const QColor &c) {
            return SankoIcons::renderSvg(QStringLiteral(":/icons/timeline/%1.svg").arg(QLatin1String(file)),
                                         box, scale, c)
                .toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        };
        QStringList bad;
        for (const char *n : {"trackLockScenes", "trackLockShots", "trackLockAudio", "trackLockCamera"}) {
            QToolButton *b = control(n);
            for (qreal scale : {1.0, 2.0}) {
                const QImage on = b ? b->icon().pixmap(box, scale, QIcon::Normal, QIcon::On).toImage()
                                          .convertToFormat(QImage::Format_ARGB32_Premultiplied) : QImage();
                const QImage off = b ? b->icon().pixmap(box, scale, QIcon::Normal, QIcon::Off).toImage()
                                           .convertToFormat(QImage::Format_ARGB32_Premultiplied) : QImage();
                if (on != render("track_lock_closed", scale, kTrackControlActive)
                    || off != render("track_lock", scale, kTrackControl))
                    bad << QStringLiteral("%1@%2x").arg(QLatin1String(n)).arg(scale);
            }
        }
        // The shape itself, at 2x: the shackle's right leg between the open
        // glyph's end (y 4.8 plus its round cap) and the body (y 6.5) - pixel
        // (21, 12) - is ink on the closed padlock and clear on the open one.
        const int aClosed = qAlpha(render("track_lock_closed", 2.0, kTrackControlActive).pixel(21, 12));
        const int aOpen = qAlpha(render("track_lock", 2.0, kTrackControlActive).pixel(21, 12));
        check(QStringLiteral("(ax) LOCKED IS A CLOSED PADLOCK: on all four tracks, at 1x and 2x, the "
                             "lock's checked icon is track_lock_closed.svg in the accent and its "
                             "unchecked icon track_lock.svg in the column grey"),
              bad.isEmpty(), bad.join(QLatin1Char(' ')));
        check(QStringLiteral("(ax) control: the two glyphs differ where the shackle closes - at 2x, "
                             "pixel (21, 12) is ink on the closed padlock and clear on the open one"),
              aClosed >= 200 && aOpen < 16,
              QStringLiteral("closed %1 open %2").arg(aClosed).arg(aOpen));
    }

    // ---- the Layers panel takes the same lock and eye (one lock and one eye
    //      across SankoTV, approved 2026-10-30) ----
    {
        auto img = [](const QIcon &ic, qreal scale, QIcon::State st) {
            return ic.pixmap(QSize(16, 16), scale, QIcon::Normal, st).toImage()
                .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        };
        auto sameIcon = [&](const QIcon &a, const QIcon &b) {
            for (qreal s : {1.0, 2.0})
                for (QIcon::State st : {QIcon::Off, QIcon::On})
                    if (img(a, s, st) != img(b, s, st))
                        return false;
            return true;
        };
        // The NEWEST match: a rebuild deleteLater()s the old rows, and with no
        // event loop running (pump is processEvents) deferred deletes never
        // run - the old rows stay, earlier in the child list.
        auto rowButton = [&](const char *name, int layerIndex) -> QToolButton * {
            QToolButton *found = nullptr;
            for (QToolButton *b : sb->findChildren<QToolButton *>(QLatin1String(name)))
                if (b->parentWidget() && b->parentWidget()->property("layerIndex").toInt() == layerIndex)
                    found = b;
            return found;
        };
        QToolButton *trackLock = control("trackLockShots");
        QToolButton *trackEye = control("trackEyeShots");
        const QList<QToolButton *> locks = sb->findChildren<QToolButton *>(QStringLiteral("layerLock"));
        const QList<QToolButton *> eyes = sb->findChildren<QToolButton *>(QStringLiteral("layerEye"));
        QStringList bad;
        auto alike = [&](QToolButton *b, QToolButton *ref, const QString &what) {
            if (!b || !ref) {
                bad << what + QStringLiteral(" missing");
                return;
            }
            if (b->size() != ref->size() || b->iconSize() != ref->iconSize() || !b->isCheckable()
                || b->styleSheet() != ref->styleSheet() || b->cursor().shape() != ref->cursor().shape()
                || b->focusPolicy() != ref->focusPolicy() || !sameIcon(b->icon(), ref->icon()))
                bad << QStringLiteral("%1 %2x%3").arg(what).arg(b->width()).arg(b->height());
        };
        for (QToolButton *b : locks)
            alike(b, trackLock, QStringLiteral("lock"));
        for (QToolButton *b : eyes)
            alike(b, trackEye, QStringLiteral("eye"));
        check(QStringLiteral("(ax) THE LAYERS PANEL'S LOCK AND EYE ARE THE TIMELINE'S: every layer "
                             "row's lock and eye is the track's button - 20 px, a 16 px icon, "
                             "checkable, the same sheet (the hover square), cursor and focus - with "
                             "the same icons at 1x and 2x, checked and unchecked"),
              !locks.isEmpty() && locks.size() == eyes.size() && bad.isEmpty(),
              QStringLiteral("%1 lock(s), %2 eye(s); %3").arg(locks.size()).arg(eyes.size())
                  .arg(bad.join(QStringLiteral("; "))));
        check(QStringLiteral("(ax) control: the same comparison tells a lock from an eye (a row's "
                             "lock against the track's eye fails it)"),
              !locks.isEmpty() && trackEye && !sameIcon(locks.first()->icon(), trackEye->icon()));

        // A click is still the layer's Undo step; the rebuilt row shows it.
        Panel *p = sb->currentPanelForTest();
        const int idx = locks.isEmpty() || !locks.first()->parentWidget()
            ? -1 : locks.first()->parentWidget()->property("layerIndex").toInt();
        QString detail;
        bool lockOk = false, eyeOk = false;
        // The stack must end as it began - empty (track toggles are not
        // commands): the checks below count its entries.
        const int count0 = stack->count();
        if (p && idx >= 0 && idx < p->layers.size() && !p->layers.at(idx).locked
            && p->layers.at(idx).visible && count0 == 0) {
            auto lastStep = [&] { return stack->index() > 0 ? stack->text(stack->index() - 1) : QString(); };
            const int i0 = stack->index();
            const bool before = rowButton("layerLock", idx)->isChecked();
            rowButton("layerLock", idx)->click();
            pump(120);
            QToolButton *after = rowButton("layerLock", idx);
            const bool locked = p->layers.at(idx).locked && after && after->isChecked()
                && stack->index() == i0 + 1 && lastStep() == QStringLiteral("Toggle Layer Lock");
            stack->undo();
            pump(120);
            QToolButton *undone = rowButton("layerLock", idx);
            lockOk = !before && locked && !p->layers.at(idx).locked && undone && !undone->isChecked();
            const int i1 = stack->index();
            rowButton("layerEye", idx)->click();
            pump(120);
            QToolButton *hid = rowButton("layerEye", idx);
            const bool hidden = !p->layers.at(idx).visible && hid && hid->isChecked()
                && stack->index() == i1 + 1 && lastStep() == QStringLiteral("Toggle Layer Visibility");
            stack->undo();
            pump(120);
            QToolButton *shown = rowButton("layerEye", idx);
            eyeOk = hidden && p->layers.at(idx).visible && shown && !shown->isChecked();
            detail = QStringLiteral("layer %1: lock [before %2 locked-step %3 undone-ok %4] eye "
                                    "[hidden-step %5 shown-ok %6]")
                         .arg(idx).arg(before).arg(locked).arg(lockOk).arg(hidden).arg(eyeOk);
            stack->clear(); // back to empty, as it began
            pump(60);
        } else {
            detail = QStringLiteral("no unlocked visible layer row (%1), stack %2").arg(idx).arg(count0);
        }
        check(QStringLiteral("(ax) LAYERS: a click on a row's lock locks the layer as one \"Toggle "
                             "Layer Lock\" step and the rebuilt lock is checked (the closed padlock); "
                             "the eye hides it as one \"Toggle Layer Visibility\" step and the rebuilt "
                             "eye is checked (35 %); Undo reverses each, unchecked again"),
              lockOk && eyeOk, detail);
    }

    // ---- a toggle: state, the button, unsaved, and nothing on the stack ----
    r.window.markCleanForTest();
    const int n0 = stack->count();
    clickControl("trackLockShots");
    check(QStringLiteral("(ax) CLICKING THE SHOTS LOCK locks the track: tracks() says so, "
                         "the button is checked, the project is unsaved, and the undo stack "
                         "is untouched (a toggle is not a command)"),
          page->tracks().shotsLocked && control("trackLockShots")->isChecked()
              && r.window.isDirty() && stack->count() == n0,
          QStringLiteral("locked %1 checked %2 dirty %3 stack %4 -> %5")
              .arg(page->tracks().shotsLocked).arg(control("trackLockShots")->isChecked())
              .arg(r.window.isDirty()).arg(n0).arg(stack->count()));

    // ---- the SHOTS lock, path by path, each with its control ----
    QPushButton *addBtn = nullptr, *dupBtn = nullptr, *delBtn = nullptr;
    for (QPushButton *b : sb->findChildren<QPushButton *>()) {
        if (b->toolTip().startsWith(QStringLiteral("<b>Add Panel</b>")))
            addBtn = b;
        else if (b->toolTip().startsWith(QStringLiteral("<b>Duplicate Panel</b>")))
            dupBtn = b;
        else if (b->toolTip().startsWith(QStringLiteral("<b>Delete Panel</b>")))
            delBtn = b;
    }
    check(QStringLiteral("(ax) locked: the strip's Add, Duplicate and Delete buttons are "
                         "disabled"),
          addBtn && dupBtn && delBtn && !addBtn->isEnabled() && !dupBtn->isEnabled()
              && !delBtn->isEnabled(),
          QStringLiteral("found %1 %2 %3 enabled %4 %5 %6")
              .arg(addBtn != nullptr).arg(dupBtn != nullptr).arg(delBtn != nullptr)
              .arg(addBtn && addBtn->isEnabled()).arg(dupBtn && dupBtn->isEnabled())
              .arg(delBtn && delBtn->isEnabled()));
    // The clip menu: the three entries disabled and suffixed.
    {
        QStringList items;
        sb->setClipMenuHookForTest([&](QMenu *menu) {
            for (QAction *a : menu->actions())
                if (!a->isSeparator())
                    items << QStringLiteral("%1[%2]").arg(a->text()).arg(a->isEnabled());
        });
        rightClickClip(r, 0);
        sb->setClipMenuHookForTest({});
        // Phase 1D: Quick Camera Move follows them, enabled - a camera
        // edit is the CAMERA lock's, not the SHOTS lock's.
        check(QStringLiteral("(ax) locked: the clip's menu shows Add Panel After, Duplicate "
                             "Panel and Delete Panel disabled and suffixed \"(locked)\"; Quick "
                             "Camera Move (the CAMERA track's) stays enabled"),
              items.size() == 4 && items.at(0) == QStringLiteral("Add Panel After (locked)[0]")
                  && items.at(1) == QStringLiteral("Duplicate Panel (locked)[0]")
                  && items.at(2) == QStringLiteral("Delete Panel (locked)[0]")
                  && items.at(3) == QStringLiteral("Quick Camera Move[1]"),
              items.join(QStringLiteral(" | ")));
    }
    // Cut, Paste (Copy allowed), the Delete key over the timeline, Ctrl+Right.
    {
        const int before = panels();
        const QString order = durations();
        clickClip(r, 1);
        sb->clearLockRefusalForTest();
        sb->editCopy();
        const bool copied = sb->hasPanelClipboard() && sb->lastLockRefusalForTest().isEmpty();
        sb->editCut();
        const bool cutRefused = panels() == before
            && sb->lastLockRefusalForTest() == QStringLiteral("SHOTS track is locked");
        sb->clearLockRefusalForTest();
        sb->editPaste();
        const bool pasteRefused = panels() == before
            && sb->lastLockRefusalForTest() == QStringLiteral("SHOTS track is locked");
        sb->clearLockRefusalForTest();
        hover(r, r.animatic, true); // arms the timeline's keys (Delete among them)
        const bool armed = sb->timelineKeysArmedForTest();
        keyClaimed(&r.window, Qt::Key_Delete);
        const bool deleteRefused = armed && panels() == before
            && sb->lastLockRefusalForTest() == QStringLiteral("SHOTS track is locked");
        sb->clearLockRefusalForTest();
        startWindowKeys::press(&r.window, Qt::Key_Right, Qt::ControlModifier);
        pump(200);
        const bool moveRefused = durations() == order
            && sb->lastLockRefusalForTest() == QStringLiteral("SHOTS track is locked");
        hover(r, r.animatic, false);
        check(QStringLiteral("(ax) locked: Copy is allowed; Cut, Paste, the Delete key over "
                             "the timeline (no question asked) and Ctrl+Right are refused "
                             "with \"SHOTS track is locked\" and change nothing"),
              copied && cutRefused && pasteRefused && deleteRefused && moveRefused
                  && stack->count() == n0,
              QStringLiteral("copied %1 cut %2 paste %3 delete %4 move %5 panels %6 stack %7")
                  .arg(copied).arg(cutRefused).arg(pasteRefused).arg(deleteRefused)
                  .arg(moveRefused).arg(panels()).arg(stack->count()));
    }
    // The timeline's own paths: a trim and a reorder, locked then unlocked.
    auto trim = [&](int flat, int dx) {
        clickClip(r, flat);
        const QRect c = tl->clipRectForTest(flat);
        const QPointF edge(c.right(), c.center().y());
        tl->clearRefusalForTest();
        sendMouse(r.surface, QEvent::MouseButtonPress, edge, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, edge + QPointF(dx, 0), Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseButtonRelease, edge + QPointF(dx, 0), Qt::LeftButton);
        pump(200);
    };
    auto reorder = [&](int flat, int toFlat) {
        const QPointF from = tl->clipRectForTest(flat).center();
        const QPointF to(tl->clipRectForTest(toFlat).right() - 2,
                         tl->clipRectForTest(toFlat).center().y());
        sendMouse(r.surface, QEvent::MouseButtonPress, from, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, from + QPointF(12, 0), Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseMove, to, Qt::LeftButton);
        sendMouse(r.surface, QEvent::MouseButtonRelease, to, Qt::LeftButton);
        pump(200);
    };
    {
        const int d0 = tl->blockDurationForTest(0);
        const QString order = durations();
        trim(0, 90); // at zoom 100, 24 fps: 90 px = nearly 4 s more
        const bool trimRefused = tl->blockDurationForTest(0) == d0
            && tl->lastRefusalForTest() == QStringLiteral("SHOTS track is locked")
            && stack->count() == n0;
        reorder(0, 1);
        const bool reorderRefused = durations() == order && stack->count() == n0
            && page->selectedPanelForTest() == 0; // the press still selected
        check(QStringLiteral("(ax) locked: a trim drag on the selected clip's edge is refused "
                             "with the message at the pointer, and a drag of a clip onto "
                             "another selects it and moves nothing"),
              trimRefused && reorderRefused,
              QStringLiteral("trim: duration %1 -> %2 refusal '%3'; reorder: %4 -> %5 selected %6 stack %7")
                  .arg(d0).arg(tl->blockDurationForTest(0)).arg(tl->lastRefusalForTest())
                  .arg(order).arg(durations()).arg(page->selectedPanelForTest()).arg(stack->count()));
        // CONTROL: unlocked, the same two drags change the track.
        clickControl("trackLockShots");
        trim(0, 90);
        const bool trimmed = tl->blockDurationForTest(0) > d0 && stack->count() == n0 + 1;
        const int d0b = tl->blockDurationForTest(0);
        reorder(0, 1);
        const bool reordered = durations() != order && stack->count() == n0 + 2;
        check(QStringLiteral("(ax) CONTROL: unlocked, the same trim lengthens the clip and the "
                             "same drag reorders, one command each"),
              trimmed && reordered && !page->tracks().shotsLocked,
              QStringLiteral("duration %1 -> %2, order %3 -> %4, stack %5 -> %6")
                  .arg(d0).arg(d0b).arg(order).arg(durations()).arg(n0).arg(stack->count()));
        // Back to the start through the history (unlocked: allowed).
        sb->editUndo();
        sb->editUndo();
        pump(200);
    }
    // Unlocked: the keys, the buttons, the menu work; then Undo / Redo of
    // a SHOTS command is refused while locked and works unlocked.
    {
        const int before = panels();
        clickClip(r, 1);
        addBtn->click();
        pump(200);
        const bool added = panels() == before + 1 && stack->count() == n0 + 1
            && addBtn->isEnabled() && dupBtn->isEnabled() && delBtn->isEnabled();
        clickControl("trackLockShots");
        messages.clear();
        const int index = stack->index();
        sb->editUndo();
        pump(200);
        const bool undoRefused = stack->index() == index && panels() == before + 1
            && messages.size() == 1 && messages.first() == QStringLiteral("SHOTS track is locked");
        clickControl("trackLockShots");
        sb->editUndo();
        pump(200);
        const bool undone = stack->index() == index - 1 && panels() == before;
        clickControl("trackLockShots");
        messages.clear();
        sb->editRedo();
        pump(200);
        const bool redoRefused = stack->index() == index - 1 && panels() == before
            && messages.size() == 1 && messages.first().contains(QStringLiteral("SHOTS track"));
        clickControl("trackLockShots");
        sb->editRedo();
        pump(200);
        const bool redone = stack->index() == index && panels() == before + 1;
        sb->editUndo();
        pump(200);
        check(QStringLiteral("(ax) UNDO / REDO of a SHOTS command (Add Panel) is refused with "
                             "the same non-modal message as every other refusal while the "
                             "track is locked, the stack left as it is; unlocked, the same "
                             "Undo and Redo work"),
              added && undoRefused && undone && redoRefused && redone,
              QStringLiteral("added %1 undoRefused %2 undone %3 redoRefused %4 redone %5 | %6")
                  .arg(added).arg(undoRefused).arg(undone).arg(redoRefused).arg(redone)
                  .arg(messages.join(QStringLiteral(" / "))));
    }

    // ---- a pending QuickShape: the first Undo cancels it, lock or no lock ----
    // (the user's rule: drawing is never locked, so the shape goes before
    // the lock looks at the history). The shape is made pending through
    // the session the canvas owns: a circle, then a hold until it is
    // recognised, then the release that leaves it pending.
    {
        addBtn->click(); // unlocked: a SHOTS command next on the stack
        pump(200);
        clickControl("trackLockShots");
        const int index = stack->index();
        quickshape::QuickShapeSession *qs = r.canvas->quickShape();
        qs->setHoldDelayMs(20);
        qs->setMorphDurationMs(1);
        const QPointF c(300, 300);
        qs->pointerPress(c + QPointF(80, 0));
        for (int i = 1; i <= 36; ++i) {
            const double a = i * 2.0 * 3.14159265 / 36.0;
            qs->pointerMove(c + QPointF(80 * std::cos(a), 80 * std::sin(a)));
            pump(4);
        }
        pump(400); // the hold
        const bool recognised = qs->hasActiveShape();
        qs->pointerRelease(c + QPointF(80, 0));
        pump(100);
        const bool pending = qs->hasActiveShape();
        messages.clear();
        sb->editUndo();
        pump(200);
        const bool cancelled = !qs->hasActiveShape() && stack->index() == index && messages.isEmpty();
        sb->editUndo();
        pump(200);
        const bool thenRefused = stack->index() == index && messages.size() == 1;
        clickControl("trackLockShots");
        sb->editUndo(); // the Add Panel, unlocked
        pump(200);
        check(QStringLiteral("(ax) UNDO WITH A PENDING QUICKSHAPE cancels the shape first, "
                             "lock or no lock (drawing is never locked), touching no history "
                             "and saying nothing; the next Undo is the lock's refusal; "
                             "unlocked, the Undo after that takes the panel back"),
              recognised && pending && cancelled && thenRefused && stack->index() == index - 1,
              QStringLiteral("recognised %1 pending %2 cancelled %3 thenRefused %4 index %5 -> %6 | %7")
                  .arg(recognised).arg(pending).arg(cancelled).arg(thenRefused).arg(index)
                  .arg(stack->index()).arg(messages.join(QStringLiteral(" / "))));
    }

    // ---- the AUDIO lock ----
    {
        const QString wav = base + QStringLiteral("/sound/theme.wav");
        audio::writeWav(wav, 6.0);
        // The stack's INDEX, not its count: the block above ended with an
        // Undo, and a push after an Undo drops the redo branch.
        const int before = stack->index();
        emit page->audioChangeRequested(wav, true, QStringLiteral("Import Audio"));
        audio::waitFor([&] { return tl->audioBarStateForTest() == QStringLiteral("present"); }, 4000);
        pump(200);
        auto isWav = [&] { return QFileInfo(page->audioPath()).fileName() == QStringLiteral("theme.wav"); };
        const bool imported = isWav() && stack->index() == before + 1;
        clickControl("trackLockAudio");
        // The track's own menu, and the Edit menu's entries.
        QStringList items;
        page->setAudioMenuHookForTest([&](QMenu *menu) {
            for (QAction *a : menu->actions())
                if (!a->isSeparator())
                    items << QStringLiteral("%1[%2]").arg(a->text()).arg(a->isEnabled());
        });
        const QRect row = tl->audioRowForTest();
        const QPoint at(AnimaticTimeline::labelColumnWidthForTest() + 40, row.center().y());
        QContextMenuEvent ev(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
        QCoreApplication::sendEvent(r.surface, &ev);
        pump(200);
        page->setAudioMenuHookForTest({});
        bool menuLocked = !items.isEmpty();
        for (const QString &i : items)
            menuLocked = menuLocked && i.endsWith(QStringLiteral(" (locked)[0]"));
        QAction *importAct = nullptr, *removeAct = nullptr;
        for (QAction *a : r.window.findChildren<QAction *>()) {
            if (a->text().startsWith(QStringLiteral("Import Audio")))
                importAct = a;
            else if (a->text().startsWith(QStringLiteral("Remove Audio")))
                removeAct = a;
        }
        const bool editMenuLocked = importAct && removeAct && !importAct->isEnabled()
            && !removeAct->isEnabled()
            && importAct->text() == QStringLiteral("Import Audio... (locked)")
            && removeAct->text() == QStringLiteral("Remove Audio (locked)");
        page->removeAudio();
        pump(200);
        const bool removeRefused = isWav() && stack->index() == before + 1;
        messages.clear();
        const int index = stack->index();
        sb->editUndo();
        pump(200);
        const bool undoRefused = stack->index() == index && isWav()
            && messages.size() == 1 && messages.first() == QStringLiteral("AUDIO track is locked");
        check(QStringLiteral("(ax) THE AUDIO LOCK: the track's menu and the Edit menu show "
                             "Import / Remove Audio disabled and suffixed \"(locked)\", Remove "
                             "Audio does nothing, and Undo of the import is refused with a "
                             "message"),
              imported && menuLocked && editMenuLocked && removeRefused && undoRefused,
              QStringLiteral("imported %1 menu [%2] edit menu %3 (%4 / %5) remove %6 undo %7 | %8")
                  .arg(imported).arg(items.join(QStringLiteral(", "))).arg(editMenuLocked)
                  .arg(importAct ? importAct->text() : QStringLiteral("-"))
                  .arg(removeAct ? removeAct->text() : QStringLiteral("-"))
                  .arg(removeRefused).arg(undoRefused).arg(messages.join(QStringLiteral(" / "))));
        // CONTROL: unlocked, the entries come back whole and the Undo works.
        clickControl("trackLockAudio");
        const bool entriesBack = importAct && importAct->isEnabled()
            && importAct->text() == QStringLiteral("Import Audio...") && removeAct
            && removeAct->isEnabled() && removeAct->text() == QStringLiteral("Remove Audio");
        sb->editUndo();
        pump(300);
        const bool undone = page->audioPath().isEmpty() && stack->index() == index - 1;
        sb->editRedo();
        audio::waitFor([&] { return tl->audioBarStateForTest() == QStringLiteral("present"); }, 4000);
        pump(200);
        check(QStringLiteral("(ax) CONTROL: unlocked, the entries are whole and enabled again "
                             "and the same Undo removes the track (redone for what follows)"),
              entriesBack && undone && isWav(),
              QStringLiteral("entries %1 undone %2 path '%3'")
                  .arg(entriesBack).arg(undone).arg(page->audioPath()));
    }

    // ---- hidden tracks: paint nothing, take no press ----
    {
        using namespace SankoTimelineTheme;
        auto pixel = [&](const QPoint &at) {
            return r.surface->grab().toImage().pixelColor(at).name();
        };
        const QRect clip0 = tl->clipRectForTest(0);
        const QPoint onClip = clip0.center();
        const QPoint onScene(clip0.center().x(), 24 + 12);
        const QString clipShown = pixel(onClip);
        const QString sceneShown = pixel(onScene);
        clickControl("trackEyeShots");
        clickControl("trackEyeScenes");
        pump(200);
        const QString clipHidden = pixel(onClip);
        const QString sceneHidden = pixel(onScene);
        const int selectedBefore = page->selectedPanelForTest();
        clickClip(r, selectedBefore == 0 ? 1 : 0); // a press on a hidden clip selects nothing
        const int selectedWhileHidden = page->selectedPanelForTest();
        // The audio row: hidden, its menu does not open.
        clickControl("trackEyeAudio");
        int menus = 0;
        page->setAudioMenuHookForTest([&](QMenu *) { ++menus; });
        const QRect row = tl->audioRowForTest();
        const QPoint at(AnimaticTimeline::labelColumnWidthForTest() + 40, row.center().y());
        QContextMenuEvent ev(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
        QCoreApplication::sendEvent(r.surface, &ev);
        pump(150);
        const int menusHidden = menus;
        clickControl("trackEyeAudio");
        QContextMenuEvent ev2(QContextMenuEvent::Mouse, at, r.surface->mapToGlobal(at));
        QCoreApplication::sendEvent(r.surface, &ev2);
        pump(150);
        page->setAudioMenuHookForTest({});
        check(QStringLiteral("(ax) HIDDEN TRACKS paint nothing: the clip's pixel is the shots "
                             "track's background and the scene block's pixel the scene "
                             "track's; a press on a hidden clip selects nothing; the hidden "
                             "audio row opens no menu - and each returns when shown"),
              clipShown != kPageBg.name() && clipHidden == kPageBg.name()
                  && sceneShown == kSceneBlock.name() && sceneHidden == kHeaderBg.name()
                  && selectedWhileHidden == selectedBefore && !page->tracks().shotsVisible
                  && !page->tracks().scenesVisible && menusHidden == 0 && menus == 1,
              QStringLiteral("clip %1 -> %2, scene %3 -> %4, selected %5, audio menus hidden %6 shown %7")
                  .arg(clipShown).arg(clipHidden).arg(sceneShown).arg(sceneHidden)
                  .arg(selectedWhileHidden).arg(menusHidden).arg(menus));
        clickControl("trackEyeShots");
        clickControl("trackEyeScenes");
        pump(200);
        const QString clipBack = pixel(onClip); // read before the press re-selects it
        const QString sceneBack = pixel(onScene);
        clickClip(r, 0);
        check(QStringLiteral("(ax) CONTROL: shown again, the clip and the scene block paint as "
                             "before and the press selects the clip"),
              clipBack == clipShown && sceneBack == sceneShown
                  && page->selectedPanelForTest() == 0,
              QStringLiteral("clip %1 scene %2 selected %3")
                  .arg(clipBack).arg(sceneBack).arg(page->selectedPanelForTest()));
    }

    // ---- THE VOLUME (Part D): the speaker opens a popup slider in the label
    // column; the value reaches the player at once, perceptually; 0 % mutes
    // (the muted glyph, no audio in the export); Escape, the speaker and a
    // press elsewhere close it; the AUDIO lock leaves it alone; a change
    // marks the project unsaved and pushes nothing. ----
    {
        QToolButton *speaker = control("trackMuteAudio");
        QSlider *popup = tl->volumePopupForTest();
        const bool hiddenAtRest = popup && !popup->isVisible();
        r.window.markCleanForTest();
        const int n0 = stack->count();
        clickControl("trackMuteAudio");
        const QRect pr = popup ? popup->geometry() : QRect();
        const QRect audioRow = tl->audioRowForTest();
        const QRect lockR = control("trackLockAudio")->geometry(), eyeR = control("trackEyeAudio")->geometry();
        const int iconTop = audioRow.y() + (audioRow.height() - 16) / 2;
        const bool placed = popup && popup->isVisible() && pr.x() == 34 && pr.width() == 10
            && pr.bottom() + 1 == iconTop && pr.height() == qMin(128, iconTop - 10) && pr.top() >= 10
            && pr.right() < eyeR.left() && pr.left() > lockR.right() && !popup->invertedAppearance()
            && popup->hasFocus();
        // The expected gain is the user's formula, written here and not read
        // from the app: gain = (v / 100) ^ (5/3), -10.0 dB at 50 %.
        const double expect50 = std::pow(0.5, 5.0 / 3.0);
        const double expect25 = std::pow(0.25, 5.0 / 3.0), expect75 = std::pow(0.75, 5.0 / 3.0);
        popup->setValue(50);
        pump(60);
        const double gain50 = page->audioGainForExport();
        const bool live50 = page->audioVolumeForTest() == 50 && qAbs(gain50 - expect50) < 1e-6
            && !page->audioMutedForTest() && r.window.isDirty() && stack->count() == n0
            && !speaker->isChecked() && !r.window.exportAudioPathForTest().isEmpty()
            && gain50 > 0.0 && gain50 < 1.0;
        popup->setValue(25);
        pump(30);
        const double gain25 = page->audioGainForExport();
        popup->setValue(75);
        pump(30);
        const double gain75 = page->audioGainForExport();
        const bool monotonic = qAbs(gain25 - expect25) < 1e-6 && qAbs(gain75 - expect75) < 1e-6
            && gain25 < gain50 && gain50 < gain75 && gain75 < 1.0;
        popup->setValue(0);
        pump(60);
        const bool muted0 = page->audioMutedForTest() && page->audioVolumeForTest() == 0 && speaker->isChecked()
            && r.window.exportAudioPathForTest().isEmpty() && qFuzzyIsNull(page->audioGainForExport());
        clickControl("trackMuteAudio"); // the speaker closes it
        const bool closedBySpeaker = !popup->isVisible();
        clickControl("trackMuteAudio");
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(popup, &esc);
        pump(60);
        const bool closedByEscape = !popup->isVisible();
        clickControl("trackMuteAudio");
        clickClip(r, 0); // a press on the canvas, outside it
        const bool closedByPress = !popup->isVisible();
        // The AUDIO lock does not lock the volume.
        clickControl("trackLockAudio");
        clickControl("trackMuteAudio");
        popup->setValue(100);
        pump(60);
        const bool lockLeavesIt = speaker->isEnabled() && popup->isEnabled() && page->audioVolumeForTest() == 100
            && !page->audioMutedForTest() && !speaker->isChecked();
        clickControl("trackMuteAudio");
        clickControl("trackLockAudio");
        check(QStringLiteral("(ax) THE VOLUME POPUP: hidden at rest; the speaker opens it at x 34 of "
                             "the column, 10 wide, rising 128 px (or less) from the speaker's icon, "
                             "clear of the lock and the eye, focused; 50 % reaches the player at once "
                             "as the perceptual gain, unsaved, nothing on the stack; 0 % mutes "
                             "(muted glyph, no audio in the export); the speaker, Escape and a press "
                             "elsewhere close it; the AUDIO lock leaves it alone"),
              hiddenAtRest && placed && live50 && monotonic && muted0 && closedBySpeaker && closedByEscape && closedByPress
                  && lockLeavesIt,
              QStringLiteral("rest %1 placed %2 (%3,%4 %5x%6; icon top %7; lock..%8 eye %9..) live50 %10 (gains 25/50/75 %% = %11 / %12 / %13 linear) "
                             "muted0 %14 closed: speaker %15 esc %16 press %17; lock leaves it %18")
                  .arg(hiddenAtRest).arg(placed).arg(pr.x()).arg(pr.y()).arg(pr.width()).arg(pr.height()).arg(iconTop)
                  .arg(lockR.right()).arg(eyeR.left()).arg(live50).arg(gain25, 0, 'f', 3).arg(gain50, 0, 'f', 3).arg(gain75, 0, 'f', 3)
                  .arg(muted0).arg(closedBySpeaker).arg(closedByEscape).arg(closedByPress).arg(lockLeavesIt));
        // DURING PLAYBACK: the volume changes without a restart - the film
        // keeps playing and the sound with it.
        page->setSelectedPanel(0);
        page->togglePlay();
        audio::waitFor([&] { return page->audioPlayingForTest(); }, 3000);
        pump(300);
        const bool playingBefore = page->isPlaying() && page->audioPlayingForTest();
        page->setAudioVolume(30);
        pump(300);
        const bool playingAfter = page->isPlaying() && page->audioPlayingForTest() && page->audioVolumeForTest() == 30
            && qAbs(page->audioGainForExport() - std::pow(0.3, 5.0 / 3.0)) < 1e-6;
        page->togglePlay();
        page->leavePreview();
        pump(200);
        page->setAudioVolume(100);
        check(QStringLiteral("(ax) A VOLUME CHANGE DURING PLAYBACK is applied at once: the film and "
                             "the sound keep playing, the player at the new gain"),
              playingBefore && playingAfter,
              QStringLiteral("before %1 after %2 volume %3").arg(playingBefore).arg(playingAfter).arg(page->audioVolumeForTest()));
    }

    // ---- save -> load round trip of the seven flags; New resets them ----
    {
        TimelineTracks all;
        all.scenesLocked = all.shotsLocked = all.audioLocked = true;
        all.scenesVisible = all.shotsVisible = all.audioVisible = false;
        all.audioVolume = 0;
        page->setTracks(all);
        const bool saved = r.window.saveProjectForTest(project);
        page->setTracks(TimelineTracks());
        const bool reloaded = r.window.loadProjectForTest(project);
        pump(500);
        const bool back = page->tracks() == all && tl->trackControlForTest(QStringLiteral("trackLockShots"))->isChecked()
            && tl->trackControlForTest(QStringLiteral("trackEyeAudio"))->isChecked()
            && tl->trackControlForTest(QStringLiteral("trackMuteAudio"))->isChecked()
            && page->audioMutedForTest();
        QFile f(project);
        f.open(QIODevice::ReadOnly);
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonObject tracks = root.value(QStringLiteral("tracks")).toObject();
        const bool written = tracks.value(QStringLiteral("shots")).toObject().value(QStringLiteral("locked")).toBool()
            && !tracks.value(QStringLiteral("audio")).toObject().value(QStringLiteral("visible")).toBool(true)
            && tracks.value(QStringLiteral("audio")).toObject().value(QStringLiteral("volume")).toInt(-1) == 0;
        check(QStringLiteral("(ax) ALL SEVEN FLAGS round-trip through the manifest's tracks "
                             "key (saved, read back on load, the buttons and the player "
                             "following); the volume is written as \"volume\""),
              saved && reloaded && back && written,
              QStringLiteral("saved %1 reloaded %2 back %3 written %4").arg(saved).arg(reloaded).arg(back).arg(written));
        // OLD PROJECTS (the user's rule): one saved as muted (Part B) opens at
        // 0 %; one without a volume opens at 100 %.
        auto rewriteAudioTracks = [&](std::function<void(QJsonObject &)> edit) {
            QFile rf(project);
            rf.open(QIODevice::ReadOnly);
            QJsonObject rootNow = QJsonDocument::fromJson(rf.readAll()).object();
            rf.close();
            QJsonObject tr = rootNow.value(QStringLiteral("tracks")).toObject();
            QJsonObject au = tr.value(QStringLiteral("audio")).toObject();
            edit(au);
            tr[QStringLiteral("audio")] = au;
            rootNow[QStringLiteral("tracks")] = tr;
            QFile wf(project);
            wf.open(QIODevice::WriteOnly | QIODevice::Truncate);
            wf.write(QJsonDocument(rootNow).toJson(QJsonDocument::Indented));
            wf.close();
        };
        rewriteAudioTracks([](QJsonObject &au) { au.remove(QStringLiteral("volume")); au[QStringLiteral("muted")] = true; });
        page->setTracks(TimelineTracks());
        const bool mutedOld = r.window.loadProjectForTest(project) && page->audioVolumeForTest() == 0 && page->audioMutedForTest();
        rewriteAudioTracks([](QJsonObject &au) { au.remove(QStringLiteral("volume")); au.remove(QStringLiteral("muted")); });
        page->setTracks(TimelineTracks());
        const bool plainOld = r.window.loadProjectForTest(project) && page->audioVolumeForTest() == 100 && !page->audioMutedForTest();
        check(QStringLiteral("(ax) OLD PROJECTS: one saved as muted opens at 0 %; one without a "
                             "volume opens at 100 %"),
              mutedOld && plainOld,
              QStringLiteral("muted-old %1 plain-old %2 volume now %3").arg(mutedOld).arg(plainOld).arg(page->audioVolumeForTest()));
    }
    // The SCENES lock refuses the Script Editor's re-parse; unlocked it applies.
    {
        TimelineTracks scenesOnly;
        scenesOnly.scenesLocked = true;
        page->setTracks(scenesOnly);
        QJsonArray scenes;
        QJsonObject one;
        one[QStringLiteral("scene_number")] = 1;
        one[QStringLiteral("location")] = QStringLiteral("INT. ROOM");
        scenes.append(one);
        const int before = panels();
        messages.clear();
        const bool refused = !r.window.applyScriptScenesForTest(scenes) && panels() == before
            && messages.size() == 1 && messages.first().contains(QStringLiteral("SCENES track"));
        page->setTracks(TimelineTracks());
        const bool applied = r.window.applyScriptScenesForTest(scenes);
        pump(300);
        check(QStringLiteral("(ax) THE SCENES LOCK refuses the Script Editor's re-parse with a "
                             "message and the project is unchanged; unlocked, the same scenes "
                             "apply"),
              refused && applied,
              QStringLiteral("refused %1 applied %2 panels %3 -> %4 | %5")
                  .arg(refused).arg(applied).arg(before).arg(panels())
                  .arg(messages.join(QStringLiteral(" / "))));
    }
    sb->setLockMessageHookForTest({});
    r.window.setLockMessageHookForTest({});
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
    runIncrementalSavePass(scratch);
    runRecentThumbnailSourcePass(scratch);
    runSizeCtlAgreementPass(scratch);
    runNewProjectPromptPass(a);
    runEraserLibraryPass(scratch);
    runLibraryWorkCountPass(scratch);
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
    {
        // 10 seconds at 30 fps (so that a fixed 24 shows), and a board long
        // enough to need three PDF pages.
        const QString film = writeProject(projects, QStringLiteral("Export"),
                                          QSize(960, 540), 30, 2, 2);
        const QString board = writeProject(projects, QStringLiteral("Board"),
                                           QSize(1280, 720), 24, 1, 13);
        runExportMoviePass(film, scratch);
        runExportFilesPass(film, board, scratch);
    }
    runAudioTravelsPass(scratch);
    runAudioSaysWhyPass(scratch);
    runAudioKeepsStepPass(scratch);
    runTimelineFollowsPass(scratch);
    runScrollbarOwnershipPass(scratch);
    runTimelineWheelPass(scratch);
    runTimelineLookPass(scratch);
    runTrackLocksPass(scratch);
    runCameraPanelRemovalPass(scratch);
    runCameraToolPass(scratch);
    runCameraRowPass(scratch);
    runQuickMovePass(scratch);

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
