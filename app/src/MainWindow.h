#pragma once

#include "ProjectResize.h" // Outcome, returned by the resize helper

#include <QMainWindow>
#include <QSize>
#include <QString>
#include <QVector>

class DashboardPage;
class QUndoStack;
class ScriptEditorPage;
class StoryboardPage;
class AnimaticPage;
class ConsistencyBoard;
class GenerationPage;
class QStackedWidget;
class QAction;
class QCloseEvent;
class QMenu;
class QJsonArray;

struct Scene;
struct ConsistencyEntry;

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
    // The real slot the Project Settings dialog's applied() signal reaches.
    void applyProjectSettingsForTest(const QString &name, int fps)
    {
        applyProjectSettings(name, fps);
    }
    // Everything the resize workflow does AFTER its dialogs have been
    // answered — the part with no modal in it.
    void resizeProjectForTest(const QSize &newSize) { applyCanvasResize(newSize); }
    QUndoStack *undoStackForTest() const { return m_undoStack; }
    void closeProjectForTest() { onCloseProject(); }
    // The real answer-to-consequence mapping, without the modal in front.
    bool mayDiscardForTest(DiscardAnswer answer)
    {
        return mayDiscardAfterAnswer(answer);
    }
    QString projectPathForTest() const { return m_currentProjectPath; }
    QString projectNameForTest() const { return m_projectName; }
    int projectFpsForTest() const { return m_projectFps; }
    bool onDashboardForTest() const;
    // The size of the panel the canvas is currently showing; invalid when it
    // holds none. Reading it after a load is what proves the canvas followed
    // the project, and reading it after a teardown is what proves it let go.
    QSize activePanelSizeForTest() const;

protected:
    // THE single unsaved-changes gate. Every way of closing this window —
    // the X button, Alt+F4, the taskbar, a Windows shutdown, and File > Exit
    // (which does nothing but call close()) — arrives here. A prompt on the
    // menu with silent data loss from the X button would be worse than no
    // prompt at all.
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenuBar();
    void updateSaveActions();
    void updateTitle();
    void freeScenes();
    void buildScenesFromJson(const QJsonArray &scenes);

    void onNewProject();
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
    void openProject(const QString &path); // File > Open AND Open Recent
    void rebuildRecentMenu();

    void onProjectSettings();
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
    void applyProjectSettings(const QString &projectName, int fps);
    bool saveToPath(const QString &path);
    bool loadFromPath(const QString &path);

    QStackedWidget *m_stack = nullptr;
    QUndoStack *m_undoStack = nullptr; // ONE app-wide chronological history
    DashboardPage *m_dashboard = nullptr;
    ScriptEditorPage *m_scriptEditor = nullptr;
    StoryboardPage *m_storyboard = nullptr;
    AnimaticPage *m_animatic = nullptr;
    ConsistencyBoard *m_consistencyBoard = nullptr;
    GenerationPage *m_generation = nullptr;

    // MainWindow owns the scene/panel objects; pages hold non-owning pointers.
    QVector<Scene *> m_scenes;
    QVector<ConsistencyEntry> m_consistencyEntries;
    QString m_currentProjectPath;
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
    // Unsaved changes. Set by every document mutation, cleared only by
    // save/load/new/close. Not derived from the undo stack's clean state:
    // that stack has a 60-command limit (so its clean index can fall off
    // the bottom and never return), it carries selection changes that are
    // not document changes, and several real edits — shot info, panel
    // durations, frame rate, project name, canvas resize — never reach it
    // at all.
    bool m_dirty = false;

    QAction *m_saveAct = nullptr;
    QAction *m_saveAsAct = nullptr;
    QAction *m_projectSettingsAct = nullptr;
    QAction *m_closeProjectAct = nullptr;
    QMenu *m_recentMenu = nullptr;
    // Edit-menu panel clipboard actions (enabled once a panel is copied).
    QAction *m_pastePanelAct = nullptr;
    QAction *m_pastePanelInPlaceAct = nullptr;
};
