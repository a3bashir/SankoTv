#pragma once

#include "ProjectResize.h" // Outcome, returned by the resize helper
#include "camera/CameraModel.h" // SceneCamera, the margin step's

#include <QDateTime>
#include <QMainWindow>
#include <QMargins>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <functional>
#include <memory>

class DashboardPage;
class QUndoStack;
class ScriptEditorPage;
class StoryboardPage;
class AnimaticPage;
class ConsistencyBoard;
class QStackedWidget;
class QAction;
class QCloseEvent;
class QMenu;
class QJsonArray;
class QTimer;

struct Scene;
struct ConsistencyEntry;

namespace ProjectIO {
class ImageLedger;
}

namespace ProjectRecovery {
struct Pending;
}

struct RecoveryCopyRun; // a recovery copy in flight (MainWindow.cpp)
class QThreadPool;
class QUrl;

namespace sankoexport {
struct MovieResult;
struct FilesResult;
class MovieEncoder;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // UNSAVED CHANGES. True from the first change until the next save,
    // load, new or close. A pure query with no side effects, so the save
    // prompts can be tested as a DECISION rather than by driving a modal.
    bool isDirty() const { return m_dirty; }
    // Would a destructive transition (New / Open / Close / Exit) have to ask
    // about unsaved work first? Pure, so the prompt's DECISION is testable
    // without driving a modal.
    bool shouldPromptToSave() const { return isDirty(); }

    // How the artist answered the unsaved-changes prompt. Exposed so the
    // consequence of each answer can be asserted directly.
    enum class DiscardAnswer { Save, Discard, Cancel };
    // May the transition proceed, given that answer? Save consults whether
    // the save ACTUALLY HAPPENED: a save that failed or was cancelled must
    // cancel the transition, or the prompt that exists to prevent data loss
    // becomes the thing that causes it.
    bool mayDiscardAfterAnswer(DiscardAnswer answer);
    // Test hook: put the project back to clean so a check can prove ONE
    // change type marks it, rather than inheriting dirt from an earlier one.
    void markCleanForTest() { setClean(); }

    // PERMANENT test hooks for SankoProjectLifecycle. The project lifecycle
    // had no gate coverage at all because opening a project runs behind a
    // native file dialog and nothing in the suite constructed this window;
    // two use-after-frees hid in that gap. These expose the REAL paths —
    // not copies of them — so the family exercises what the menu does.
    bool loadProjectForTest(const QString &path) { return loadFromPath(path); }
    void newProjectForTest() { onNewProject(); }
    bool saveProjectForTest(const QString &path) { return saveToPath(path); }
    // What the last save did with each image: the files it wrote, and how
    // many it skipped because their files already held them.
    QStringList lastSaveWrittenForTest() const { return m_lastSaveWritten; }
    int lastSaveSkippedForTest() const { return m_lastSaveSkipped; }
    QStringList lastSaveRenamedForTest() const { return m_lastSaveRenamed; }
    // Clean Up Project Folder (2026-11-04) driven by the gate: `confirm`
    // answers its question; the count of files removed, -1 when it refused
    // (no project, or unsaved changes).
    int cleanUpProjectFolderForTest(bool confirm);
    int lastSaveEncodeThreadsForTest() const { return m_lastSaveEncodeThreads; }
    // The real slot the Project Settings dialog's applied() signal reaches.
    void applyProjectSettingsForTest(const QString &name, int fps)
    {
        applyProjectSettings(name, fps);
    }
    // Everything the resize workflow does AFTER its dialogs have been
    // answered — the part with no modal in it.
    void resizeProjectForTest(const QSize &newSize) { applyCanvasResize(newSize); }
    // THE STAGE MARGIN (Phase 2A): the margin change once its dialogs are
    // answered (the same swap a resize does, the frame kept), and the
    // project's margin, px each side.
    void setStageMarginForTest(const QSize &margin) { applyStageResizeInternal(
        QSize(m_canvasWidth - 2 * m_stageMargin.width() + 2 * margin.width(),
              m_canvasHeight - 2 * m_stageMargin.height() + 2 * margin.height()), margin); }
    QSize stageMarginForTest() const { return m_stageMargin; }
    // The margin as a rectangle (batch 6b-V): the undo step's own two calls.
    QVector<shotcam::SceneCamera> sceneCamerasForMargin() const;
    void chooseStageMarginForTest(const QSize &newMargin, bool allPanels) { chooseStageMargin(newMargin, allPanels); }
    // Project Settings > Stage Margin... as the button opens it (its dialog is modal:
    // the check answers it from a timer).
    void openStageMarginForTest() { onStageMargin(); }
    QVector<QMargins> panelMarginsForMargin() const;
    void chooseStageMargin(const QSize &newMargin, bool allPanels); // Set, or Apply to All
    void setMarginRect(const QSize &newMargin, const QVector<shotcam::SceneCamera> *cameras,
                       bool allPanels, const QVector<QMargins> *panelMargins);
    // Art outside the stage: how many panels hold some, and freeing it (one step).
    int panelsWithHiddenArt() const;
    void trimHiddenArt();
    QUndoStack *undoStackForTest() const { return m_undoStack; }
    // The locks (Part B): the Script Editor's re-parse, as its scenesReady
    // signal reaches this window - refused with a message while the SCENES
    // or SHOTS track is locked. The message box goes to the hook instead.
    bool applyScriptScenesForTest(const QJsonArray &scenes) { return onScenesReady(scenes); }
    void setLockMessageHookForTest(std::function<void(const QString &)> hook)
    {
        m_lockMessageHookForTest = std::move(hook);
    }
    QString lastLockRefusalForTest() const { return m_lastLockRefusal; }
    // The track an export would carry right now (the real decision the
    // movie request is built from): empty for none, missing or MUTED.
    QString exportAudioPathForTest() const { return exportAudioPath(); }
    void closeProjectForTest() { onCloseProject(); }
    // Opening with the menu bar's room taken only after the page is back (the
    // old order): the check's control for the dock that moved on every reopen.
    void setLateChromeForTest(bool late) { m_lateChromeForTest = late; }
    // The real answer-to-consequence mapping, without the modal in front.
    bool mayDiscardForTest(DiscardAnswer answer)
    {
        return mayDiscardAfterAnswer(answer);
    }
    // Answer the unsaved-changes prompt WITHOUT its modal, and count how
    // often it was asked. With this set the prompt's box is never shown;
    // everything around it - whether to ask, and what each answer then
    // allows - is the real code. An empty function restores the modal.
    void setDiscardPromptForTest(std::function<DiscardAnswer()> answer)
    {
        m_discardPromptForTest = std::move(answer);
        m_discardPromptCount = 0;
    }
    int discardPromptCountForTest() const { return m_discardPromptCount; }
    QString projectPathForTest() const { return m_currentProjectPath; }
    void rebuildRecentMenuForTest() { rebuildRecentMenu(); }
    QString projectNameForTest() const { return m_projectName; }
    int projectFpsForTest() const { return m_projectFps; }
    bool onDashboardForTest() const;
    // The size of the panel the canvas is currently showing; invalid when it
    // holds none. Reading it after a load is what proves the canvas followed
    // the project, and reading it after a teardown is what proves it let go.
    QSize activePanelSizeForTest() const;
    // Where the Open Project file picker would start (the picker itself is
    // native and cannot be driven).
    QString openDialogStartDirForTest() const { return openDialogStartDir(); }

    // FILE > EXPORT WITHOUT ITS DIALOGS. Each is everything the menu entry
    // does after the artist has answered the file dialog: the same job, the
    // same checks, the same result the message box is written from.
    sankoexport::MovieResult exportMp4ForTest(const QString &path);
    sankoexport::FilesResult exportPngForTest(const QString &folder);
    sankoexport::FilesResult exportPdfForTest(const QString &path);
    // File > Export > Stills Show Full Artwork (Phase 1B), as the menu has it.
    QAction *exportFullArtworkActionForTest() const { return m_exportFullArtworkAct; }
    // The Dev Recorder's words for an action (2026-10-27): Edit > Undo and
    // Redo name their step, "Undo Move Camera"; other actions add nothing.
    QVariantMap recorderActionDetail(QAction *action) const;
    QAction *undoActionForTest() const { return m_undoAct; }
    QAction *redoActionForTest() const { return m_redoAct; }
    // The two things MP4 export says BEFORE the file dialog: that it cannot
    // run here at all (empty when it can), and that the project's audio file
    // is missing (empty when there is no track or the file is there).
    QString mp4UnavailableReasonForTest() const { return mp4UnavailableReason(); }
    QString mp4AudioWarningForTest() const { return mp4AudioWarning(); }
    QString mp4CameraWarningForTest() const { return mp4CameraWarning(); }
    // Stand in for "this machine has no encoder" (an N edition of Windows
    // cannot be had on a developer's machine). Null string = ask the system.
    void setMp4UnavailableForTest(const QString &reason)
    {
        m_mp4UnavailableForTest = reason;
    }
    // Names in the folder that a PNG export would replace.
    QStringList pngExportClashesForTest(const QString &folder) const
    {
        return pngExportClashes(folder);
    }
    // Replace the encoder (the interface exists so that it can be).
    void setMovieEncoderFactoryForTest(
        std::function<sankoexport::MovieEncoder *()> factory)
    {
        m_movieEncoderFactoryForTest = std::move(factory);
    }
    // In place of the progress dialog: called with (done, total); return
    // false to press Cancel.
    void setExportProgressHookForTest(std::function<bool(int, int)> hook)
    {
        m_exportProgressHookForTest = std::move(hook);
    }
    void setExportStallTimeoutForTest(int ms) { m_exportStallMsForTest = ms; }
    // The project's own scenes, to compare an export with what it exported.
    const QVector<Scene *> &scenesForTest() const { return m_scenes; }
    // CONTROLLED FOLDER ACCESS (2026-10-31): in place of the "Windows
    // blocked SankoTV" box - given its title and text, return true for its
    // Save As / Export to Another Folder button; and in place of Save As's
    // file dialog, the path it would have returned.
    void setBlockedPromptForTest(std::function<bool(const QString &, const QString &)> prompt)
    {
        m_blockedPromptForTest = std::move(prompt);
    }
    void setSaveAsPathForTest(std::function<QString()> picker)
    {
        m_saveAsPathForTest = std::move(picker);
    }
    // The last failure message a save, export or recording showed (either
    // kind: the plain "Windows blocked" one or the write's own).
    QString lastWriteFailureForTest() const { return m_lastWriteFailure; }

    // AUTO-SAVE AND CRASH RECOVERY (2026-11-04, batch 3), driven by the gate
    // without its timer or its dialog: the interval (minutes, 0 = off), one
    // tick of the timer (true when a copy was written), whether a tick found
    // the artist mid-action and is waiting, what the last copy wrote and
    // when, the manifest step made to fail (the previous copy must stay
    // whole), the crash prompt's answer in place of its dialog, and the real
    // File > Open path that asks it.
    enum class RecoveryAnswer { OpenCopy, OpenSaved, ShowFolder, Cancel };
    void setRecoveryMinutesForTest(int minutes) { setRecoveryMinutes(minutes); }
    int recoveryMinutesForTest() const { return m_recoveryMinutes; }
    int recoveryTimerIntervalForTest() const; // ms; 0 when not armed
    // One tick, and the copy it starts waited for (2026-11-05: the copy runs
    // on worker threads): true when a copy's manifest was written.
    bool fireRecoveryTickForTest();
    // One tick, NOT waited for: true when a copy started.
    bool startRecoveryTickForTest() { return onRecoveryTick(); }
    // Events processed until no copy is in flight (at most maxMs).
    bool waitForRecoveryCopyForTest(int maxMs = 180000);
    bool recoveryInFlightForTest() const { return m_recoveryRun != nullptr; }
    // While set, a copy that starts waits at its beginning on its worker;
    // clearing it releases the one in flight.
    void setRecoveryHoldForTest(bool hold);
    // Where the last copy's work ran: its steps (each flatten composited,
    // each image encoded, the manifest written) and how many of them ran on
    // the UI thread; the same for the last Save; the tick's own time on the
    // UI thread.
    int lastRecoveryStepsForTest() const { return m_lastRecoverySteps; }
    int lastRecoveryUiStepsForTest() const { return m_lastRecoveryUiSteps; }
    int lastSaveStepsForTest() const { return m_lastSaveSteps; }
    int lastSaveUiStepsForTest() const { return m_lastSaveUiSteps; }
    qint64 lastRecoveryUiMsForTest() const { return m_lastRecoveryUiMs; }
    // "Show both in the folder" in place of the desktop's file browser.
    void setOpenFolderForTest(std::function<void(const QUrl &)> open)
    {
        m_openFolderForTest = std::move(open);
    }
    bool recoveryPendingForTest() const { return m_recoveryPending; }
    QStringList lastRecoveryWrittenForTest() const { return m_lastRecoveryWritten; }
    QDateTime lastRecoveryAtForTest() const { return m_lastRecoveryAt; }
    void setRecoveryFailAtManifestForTest(bool fail) { m_recoveryFailAtManifestForTest = fail; }
    void setRecoveryPromptForTest(std::function<RecoveryAnswer()> answer)
    {
        m_recoveryPromptForTest = std::move(answer);
        m_recoveryPromptCount = 0;
    }
    int recoveryPromptCountForTest() const { return m_recoveryPromptCount; }
    void openProjectForTest(const QString &path) { openProject(path); }
    QString recoveredFromForTest() const { return m_recoveredFrom; }
    QString savedIndicatorForTest() const;  // the Script Editor's words
    QString recoveryStatusForTest() const;  // the bottom bar's words
    int discardRecoveryCopyForTest(bool confirm); // 1 removed, 0 not
    QString sessionIdForTest() const { return m_sessionId; }
    // The recovered document's replace question (2026-11-05) answered in
    // place of its box: given the file and whether it is the project the
    // work was recovered from, true to replace. Counted.
    void setReplacePromptForTest(std::function<bool(const QString &, bool)> answer)
    {
        m_replacePromptForTest = std::move(answer);
        m_replacePromptCount = 0;
    }
    int replacePromptCountForTest() const { return m_replacePromptCount; }

protected:
    // THE single unsaved-changes gate. Every way of closing this window —
    // the X button, Alt+F4, the taskbar, a Windows shutdown, and File > Exit
    // (which does nothing but call close()) — arrives here. A prompt on the
    // menu with silent data loss from the X button would be worse than no
    // prompt at all.
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenuBar();
    // Attach every shortcut-bearing menu action to the window as well, so
    // the shortcuts outlive a hidden menu bar. See the definition.
    void keepMenuShortcutsAlive();
    // Show or hide what surrounds the current page: no menu bar on the
    // start window, and the recorder's indicator moved to where it can
    // still be seen.
    void updateChromeForPage();
    void updateChromeFor(const QWidget *page);
    void updateSaveActions();
    void updateAudioActions(); // Edit > Import Audio / Remove Audio
    void updateExportActions(); // File > Export: needs at least one panel
    void updateTitle();

    // File > Export. The on... functions ask (which file, replace these?)
    // and report; the run... functions do the export and are what the test
    // hooks call.
    void onExportMp4();
    void onExportPng();
    void onExportPdf();
    sankoexport::MovieResult runMovieExport(const QString &path);
    sankoexport::FilesResult runPngExport(const QString &folder);
    sankoexport::FilesResult runPdfExport(const QString &path);
    QString mp4UnavailableReason() const;
    QString mp4AudioWarning() const;
    QString mp4CameraWarning() const; // the CAMERA track hidden (Phase 1C)
    bool exportCameraOff() const;
    QString exportAudioPath() const; // the track the film carries; empty: silent
    QStringList pngExportClashes(const QString &folder) const;
    QString exportStartDir() const;
    void freeScenes();
    void buildScenesFromJson(const QJsonArray &scenes);
    bool onScenesReady(const QJsonArray &scenes); // false: refused by a lock

    void onNewProject();
    // THE entrance to a new project: asks about unsaved work, then shows
    // the New Project window. The Dashboard's button and File > New Project
    // both come through here, so neither can skip the question.
    void requestNewProject();
    // Shows the New Project window and applies whatever it returns.
    void runNewProjectDialog();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
    void onPreferences(); // Edit > Preferences... (category list + settings pane)
    // File > Project Settings... — settings of the CURRENTLY OPEN project
    // (name, frame rate; canvas facts read-only). Pending until Apply/OK.
    // Unsaved-changes bookkeeping. markDirty() is deliberately cheap and
    // idempotent: it is called from a great many places.
    void markDirty();
    void setClean();

    // THE teardown. New, Open and Close all use it; they differ only in what
    // they do afterwards, never in what they take down.
    enum class ClipboardPolicy {
        Keep,  // New / Open: pasting a panel into the next project is a real
               // workflow, and the paste guard refuses mismatched sizes
        Clear  // Close: there is no project to paste into, and the clipboard
               // would hold artwork from a project deliberately closed
    };
    void resetProjectState(ClipboardPolicy clipboards);

    // Shows the unsaved-changes prompt when there is something to lose.
    // Returns false ONLY when the transition must not proceed.
    bool confirmDiscardChanges(const QString &actionDescription);
    // Save, reporting whether it actually happened (Save As can be
    // cancelled, and a write can fail). onSaveProject returns void, which is
    // exactly the hole this closes.
    bool saveForPrompt();

    void onCloseProject();
    QString openDialogStartDir() const;
    // File > Open, Open Recent, AND the start window's cards and rows.
    void openProject(const QString &path);
    void rebuildRecentMenu();
    // ...once the current event is over: never under a click on its items.
    void rebuildRecentMenuLater();

    void onProjectSettings();
    void onCleanUpProjectFolder();
    int cleanUpProjectFolder(std::function<bool(int, qint64)> confirm, QString *why);

    // --- Auto-save and crash recovery (2026-11-04) ---------------------------
    // Every m_recoveryMinutes, while the project has unsaved changes and the
    // artist is between actions, a full copy goes to the project's recovery
    // folder (ProjectRecovery) through the same ProjectIO path as a Save -
    // never to the project's own files. The session lock marks the project
    // open; a lock left behind plus a copy newer than the file is a crash,
    // and openProject offers the copy.
    void setRecoveryMinutes(int minutes);
    void armRecoveryTimer();
    bool onRecoveryTick();       // a copy when due; waits for a held action
    bool startRecoveryCopy();    // true: a copy is now in flight
    void finishRecoveryRun(quint64 serial); // its result, on the UI thread
    // Before anything that ends what the copy is a copy of (a Save, a close,
    // an open, a discard, the window going): the copy in flight is stopped
    // (cancel) or allowed to finish, and waited for.
    void finishRecoveryCopy(bool cancel);
    bool actionInProgress() const;
    QString recoveryBasePath() const; // the project, or the one recovered from
    RecoveryAnswer askRecovery(const QString &path, const ProjectRecovery::Pending &pending);
    void openRecoveryCopy(const QString &original, const QString &copyPath);
    bool confirmReplaceForRecovered(const QString &path); // true: replace
    void onDiscardRecoveryCopy();
    int discardRecoveryCopy(std::function<bool()> confirm, QString *why);
    void updateSavedIndicator(); // the Script Editor's dot and words
    QString buildStamp() const;
    // Resize Project...: the whole confirmed, non-undoable canvas-only
    // resize workflow — in-flight refusal, size prompt, memory precheck,
    // save prompt, confirm, per-panel swap, state reset.
    void onResizeProject(const QSize &currentSize);
    // The resize itself, once every dialog has been answered: swap the
    // panels and put the project's state right. Split out of
    // onResizeProject so the operation can be exercised without driving
    // four modal dialogs, and so there is ONE definition of what a resize
    // does rather than a test-shaped copy of it.
    void applyCanvasResize(const QSize &newSize);
    ProjectResize::Outcome applyCanvasResizeInternal(const QSize &newSize);
    // Stage Margin... (Phase 2A): the same workflow as a resize - in-flight
    // refusal, the margin dialog, memory precheck, save prompt, confirm -
    // with the shot kept and the stage grown or shrunk around it.
    void onStageMargin();
    void onTrimHiddenArt();
    // The swap for both: the canvas to newSize with newMargin px each side;
    // camera keys keep framing the same artwork (Stage remap).
    ProjectResize::Outcome applyStageResizeInternal(const QSize &newSize,
                                                    const QSize &newMargin);
    // The margin into every page that draws or measures the shot.
    void pushStageMargin();
    // The project's margin as each side of a panel's stage (batch 6a): what
    // a new panel gets, and every panel after a margin change.
    QMargins projectMargin() const;
    void applyProjectSettings(const QString &projectName, int fps);
    bool saveToPath(const QString &path);
    // A save, export or recording that failed: when Windows blocked it in a
    // protected folder, the plain explanation and an offer to write
    // elsewhere (true when taken); otherwise the caller's own message.
    bool sayWriteFailed(const QString &title, const QString &lead, const QString &target,
                        const QString &doing, const QString &generic,
                        const QString &elsewhereLabel, bool criticalIcon);
    void saySaveFailed(const QString &target, const QString &generic);

    // THE DIRTY FLAG IS WHAT STANDS BETWEEN THE ARTIST AND CLOSING THE APP
    // ON WORK THAT NEVER REACHED DISK, so "never clear it for a save that
    // did not fully happen" must not be a rule a future edit can forget.
    //
    // SaveCompleted is a token with a PRIVATE constructor whose only friend
    // is saveToPath. markSavedTo() — which adopts the path, records the
    // recent entry and calls setClean() — cannot be called without one, and
    // the single place that can make one is the tail of saveToPath, after
    // every image write and the manifest commit have been checked. Anywhere
    // else in this class, clearing the dirty flag after a partial save is
    // not a mistake to be caught in review: there is nothing to pass, and
    // the code does not compile.
    class SaveCompleted
    {
        // USER-PROVIDED on purpose, not "= default". Under C++17 a class
        // whose only constructor is defaulted-in-class is still an
        // AGGREGATE, and SaveCompleted{} would then be aggregate
        // initialisation — which does not check constructor access, so the
        // token could be forged anywhere and the guarantee would be
        // decorative. A negative compile test caught exactly that: the
        // first version of this class compiled a deliberate forgery in
        // onSaveProject without complaint. Writing the body makes the
        // constructor user-provided, the class a non-aggregate, and the
        // access check real.
        SaveCompleted() {}
        friend bool MainWindow::saveToPath(const QString &path);
    };
    void markSavedTo(const QString &path, SaveCompleted);

    // asRecoveryCopy: the file is a crash's recovery copy of m_recoveredFrom
    // - loaded as an UNSAVED document with no file of its own, not recorded
    // in the recents.
    bool loadFromPath(const QString &path, bool asRecoveryCopy = false);

    QStackedWidget *m_stack = nullptr;
    QUndoStack *m_undoStack = nullptr; // ONE app-wide chronological history
    DashboardPage *m_dashboard = nullptr;
    ScriptEditorPage *m_scriptEditor = nullptr;
    StoryboardPage *m_storyboard = nullptr;
    AnimaticPage *m_animatic = nullptr;
    ConsistencyBoard *m_consistencyBoard = nullptr;

    // MainWindow owns the scene/panel objects; pages hold non-owning pointers.
    QVector<Scene *> m_scenes;
    QVector<ConsistencyEntry> m_consistencyEntries;
    QString m_currentProjectPath;
    // What each image file this window has read or written now holds, so a
    // save writes only the images that changed (ProjectIO::ImageLedger).
    // Keyed by file, so it outlives any one project: an entry for another
    // project's file stays a true statement about that file (its size and
    // time catch any change), and nothing ever needs clearing.
    std::unique_ptr<ProjectIO::ImageLedger> m_imageLedger;
    QStringList m_lastSaveWritten;
    int m_lastSaveSkipped = 0;
    QStringList m_lastSaveRenamed; // images only renamed to their id names
    QAction *m_cleanUpAct = nullptr; // File > Clean Up Project Folder...
    int m_lastSaveEncodeThreads = 0;
    // Auto-save and crash recovery (2026-11-04).
    QTimer *m_recoveryTimer = nullptr;  // every m_recoveryMinutes
    QTimer *m_recoveryRetry = nullptr;  // while a tick waits for a held action
    int m_recoveryMinutes = 2;          // 0 = off; the setting recovery/minutes
    bool m_recoveryPending = false;
    std::unique_ptr<ProjectIO::ImageLedger> m_recoveryLedger; // the copy's files
    QString m_sessionId;                // this process's, in the lock and the header
    QString m_recoveredFrom;            // the project a recovered document came from
    QStringList m_lastRecoveryWritten;
    QDateTime m_lastRecoveryAt;
    bool m_recoveryFailAtManifestForTest = false;
    std::function<RecoveryAnswer()> m_recoveryPromptForTest;
    int m_recoveryPromptCount = 0;
    std::function<bool(const QString &, bool)> m_replacePromptForTest;
    int m_replacePromptCount = 0;
    QAction *m_discardRecoveryAct = nullptr; // File > Discard Recovery Copy
    // The copy off the UI thread (2026-11-05).
    std::unique_ptr<RecoveryCopyRun> m_recoveryRun; // the copy in flight
    QThreadPool *m_recoveryPool = nullptr;          // its worker (one)
    quint64 m_recoverySerial = 0;
    bool m_lastRecoveryCommitted = false;
    bool m_recoveryHoldForTest = false;
    int m_lastRecoverySteps = 0;
    int m_lastRecoveryUiSteps = 0;
    int m_lastSaveSteps = 0;
    int m_lastSaveUiSteps = 0;
    qint64 m_lastRecoveryUiMs = 0;
    Qt::HANDLE m_uiThread = nullptr; // the thread this window lives on
    std::function<void(const QUrl &)> m_openFolderForTest;
    QString m_projectName = QStringLiteral("Untitled Project");
    // Project properties. fps drives the animatic timeline; canvasWidth/
    // Height are the project's REAL canvas resolution — the single source
    // of truth that shapes every layer at create/load. At runtime the
    // panels' own pixels are the authority (Panel::canvasSize()); on load
    // these are RECONCILED to the pixels (pixels win, the user is told of
    // any disagreement, the manifest is corrected on save). The 960x540
    // initialisers are only the pre-project idle values; every create/load
    // path overwrites them before a panel can exist.
    int m_projectFps = 24;
    int m_canvasWidth = 960;
    int m_canvasHeight = 540;
    // THE STAGE MARGIN (Phase 2A): px each side of the shot; the canvas
    // above is the stage. 0 x 0 = none, every project made before 2A.
    QSize m_stageMargin = QSize(0, 0);
    // Unsaved changes. Set by every document mutation, cleared only by
    // save/load/new/close. Not derived from the undo stack's clean state:
    // that stack has a 60-command limit (so its clean index can fall off
    // the bottom and never return), it carries selection changes that are
    // not document changes, and several real edits — shot info, panel
    // durations, frame rate, project name, canvas resize — never reach it
    // at all.
    bool m_dirty = false;
    std::function<DiscardAnswer()> m_discardPromptForTest;
    bool m_lateChromeForTest = false;
    std::function<bool(const QString &, const QString &)> m_blockedPromptForTest;
    std::function<QString()> m_saveAsPathForTest;
    QString m_lastWriteFailure;
    std::function<void(const QString &)> m_lockMessageHookForTest;
    QString m_lastLockRefusal;
    int m_discardPromptCount = 0;

    QAction *m_saveAct = nullptr;
    QAction *m_clearStageAct = nullptr; // View > Clear the Stage While the Film Plays
    QAction *m_saveAsAct = nullptr;
    QAction *m_projectSettingsAct = nullptr;
    QAction *m_trimHiddenAct = nullptr;
    QAction *m_closeProjectAct = nullptr;
    QMenu *m_recentMenu = nullptr;
    // Edit-menu panel clipboard actions (enabled once a panel is copied).
    QAction *m_pastePanelAct = nullptr;
    QAction *m_pastePanelInPlaceAct = nullptr;
    // Edit-menu audio actions (the workspace page, with a project open).
    QAction *m_importAudioAct = nullptr;
    QAction *m_removeAudioAct = nullptr;
    QAction *m_locateAudioAct = nullptr; // only while the track is missing
    QAction *m_copyAudioAct = nullptr;   // only for a linked track that is there
    // File > Export.
    QMenu *m_exportMenu = nullptr;
    QAction *m_exportMp4Act = nullptr;
    QAction *m_exportPngAct = nullptr;
    QAction *m_exportPdfAct = nullptr;
    QAction *m_exportFullArtworkAct = nullptr; // checkable; stills = whole panel
    QAction *m_undoAct = nullptr; // Edit > Undo / Redo (the recorder names their step)
    QAction *m_redoAct = nullptr;
    bool stillsFullArtwork() const;
    bool m_exporting = false; // one export at a time
    QString m_mp4UnavailableForTest; // null = ask the system
    std::function<sankoexport::MovieEncoder *()> m_movieEncoderFactoryForTest;
    std::function<bool(int, int)> m_exportProgressHookForTest;
    int m_exportStallMsForTest = 0;
};
