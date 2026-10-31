#pragma once

#include "DrawingCanvas.h" // DrawingCanvas::Tool (persisted selection mode)

#include <QHash>
#include <functional>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

class ZoomToolbar;
namespace brushlib {
class BrushLibraryModel;
class BrushLibraryPanel;
class BrushSettingsStudio;
}
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QJsonArray;
class QJsonObject;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QTimer;
class QUndoCommand;
class QUndoStack;
class QVBoxLayout;
class SankoSlider;
class SankoTipPopup;

class DockController;
class QDockWidget;
class QMainWindow;
class AnimaticPage;
class QMenu;
class QShortcut;
class QSplitter;

struct Panel;
struct Scene;
struct ConsistencyEntry;

// THE WORKSPACE: scene list (left), panel strip over the drawing canvas
// (centre), the animatic timeline under it, and shot info (right). Drawing,
// panels, timing and playback all happen here; there is no separate animatic
// screen. Scenes are passed in from the Script Editor or a project file.
//
// The strip and the timeline are two views of the same panels. This page
// owns the one selection and the one undo history; both views ask it to
// change things and are told the result (see selectPanel and the
// apply...ForUndo callbacks).
class StoryboardPage : public QWidget
{
    Q_OBJECT

public:
    explicit StoryboardPage(QWidget *parent = nullptr);
    ~StoryboardPage() override; // persists the dock layout

    // What the Size CTL bar's size slider is DISPLAYING (-1 before the bar
    // exists). Test seam for the bar<->engine agreement: the bar once
    // clamped its mirror to 200 while the studio could set 2048, so it
    // showed 200 whenever a large library brush was active — a display lie
    // nothing could observe from outside. The slider class is file-local,
    // so this is the narrowest honest window into it.
    int sizeCtlDisplayedOpacityForTest() const
    {
        return m_sizeCtlOpacityForTest ? m_sizeCtlOpacityForTest() : -1;
    }
    int sizeCtlDisplayedSizeForTest() const
    {
        return m_sizeCtlSizeForTest ? m_sizeCtlSizeForTest() : -1;
    }
    // DISPLAY-SIZE LAYER: moves the bar the way the user does - the value
    // is the DISPLAY number (visible width / a tick), and it runs the
    // slider's own onChanged, so the engine conversion is the real one.
    void sizeCtlUserSetSizeForTest(int displayValue)
    {
        if (m_sizeCtlUserSetSizeForTest)
            m_sizeCtlUserSetSizeForTest(displayValue);
    }
    // Same, for the opacity MULTIPLIER slider (percent).
    void sizeCtlUserSetOpacityForTest(int percent)
    {
        if (m_sizeCtlUserSetOpacityForTest)
            m_sizeCtlUserSetOpacityForTest(percent);
    }

    // Display the given scenes. Ownership stays with the caller (MainWindow);
    // this page only holds non-owning pointers.
    void loadScenes(const QVector<Scene *> &scenes);
    // Drop every non-owning pointer into the current scenes because they are
    // about to be destroyed. NOT the same as loadScenes({}), which leaves the
    // canvas holding its panel — see the implementation.
    void detachScenes();
    // Drop the copied panel. Closing a project does this; opening another
    // deliberately does NOT, because pasting a panel across projects is a
    // real workflow and the size guard refuses the mismatched case.
    void clearPanelClipboard();
    // The PROJECT's canvas size — the authority every new panel is created
    // at. Set by MainWindow at project create/load, before any panel can be
    // added. Existing panels carry their own size (pixels are the truth);
    // this member only shapes panels that do not exist yet.
    void setProjectCanvasSize(const QSize &size) { m_projectCanvasSize = size; }
    QSize projectCanvasSize() const { return m_projectCanvasSize; }

    // Read-only reference to the project's consistency entries (for future
    // prompt injection). Not displayed yet.
    void setConsistencyEntries(const QVector<ConsistencyEntry> *entries)
    {
        m_consistencyEntries = entries;
    }

    // Preferences > Camera plumbing: safe-area guide opacities in percent,
    // forwarded live to the drawing canvas.
    void setActionSafeMaskOpacity(int percent);
    void setTitleSafeMaskOpacity(int percent);

    // Edit-menu entry points: route to the CANVAS selection clipboard when a
    // selection exists (copy/cut) or the last copy came from the canvas
    // (paste); otherwise fall back to the panel-level clipboard below.
    void editCopy();
    void editCut();
    void editPaste();
    void editPasteInPlace();

    // Edit-menu undo/redo (Ctrl+Z / Ctrl+Y): the app-wide chronological
    // history — one shared QUndoStack covering drawing, selection, and panel
    // actions, same as the Brush-bar buttons.
    void editUndo();
    void editRedo();

    // Perspective guide settings, persisted in the project file (delegates
    // to the canvas's PerspectiveTool; refreshes the settings panel on load).
    QJsonObject perspectiveToJson() const;
    void perspectiveFromJson(const QJsonObject &object);

    // Bake any pending QuickShape vector (called before save/load, so the
    // temporary overlay is never silently dropped or left un-serialized).
    void commitQuickShape();
    void ensureBrushPixelsForSave();

    // App-wide undo stack (owned by MainWindow); forwarded to the canvas.
    void setUndoStack(QUndoStack *stack);
    Panel *makeProjectPanel() const; // blank panel at the project size
    // Callbacks for the panel undo commands (see StoryboardPage.cpp): mutate
    // the scene's panel list and refresh the strip/selection.
    void applyPanelInsertForUndo(Scene *scene, int index, Panel *panel);
    Panel *applyPanelRemoveForUndo(Scene *scene, int index);
    void applyPanelMoveForUndo(Scene *scene, int from, int to);
    // A panel's duration, set by the undoable command a timeline drag
    // pushes (and by its undo): writes the panel and re-times the timeline.
    void applyPanelDurationForUndo(Panel *panel, int seconds);
    // The scratch audio track, set by the undoable command Import / Remove
    // Audio pushes (and by its undo): empty = no track; linked = an old
    // project's file outside the project (AnimaticPage::audioLinked).
    void applyAudioPathForUndo(const QString &path, bool linked);
    // A scene's shot camera, set by the undoable command every camera edit
    // pushes (a canvas drag, Add / Delete Key, the ease) and by its undo:
    // the keys, then the canvas, the preview, the SHOTS clips, the strip.
    void applyCameraForUndo(Scene *scene, const shotcam::SceneCamera &camera);

    // THE SHOT CAMERA BAR (Phase 1B, Figma 1B-4): the Camera tool's
    // Modifier bar. Animate (session), previous key / Add Key / Delete Key /
    // next key, the ease of the segment at the playhead, Stay inside artwork
    // (session, on by default), and the Safe Area / Title Safe guides -
    // which stay on with every tool and are remembered across launches as
    // ONE app setting (kCameraGuidesKey, bit 1 Safe Area, bit 2 Title Safe).
    // EDIT RULES: Animate off with two or more keys refuses a frame drag
    // with a note; Animate off with none or one sets the scene's single
    // framing (one key on its first frame); Animate on updates the key at
    // the playhead or adds one there. Every edit is one undo command.
    static const QString kCameraGuidesKey;
    // The bar's hover notes (follow-up to the 1B hand test, 2026-10-25):
    // every control while the preview covers the canvas, and the Ease field
    // on (or past) the scene's last key.
    static QString cameraPreviewNote();
    static QString cameraLastKeyNote();
    bool cameraAnimate() const { return m_cameraAnimate; }
    void setCameraAnimate(bool on);
    bool cameraStayInside() const { return m_cameraStayInside; }
    void setCameraStayInside(bool on);
    void setCameraGuides(bool safeArea, bool titleSafe); // also remembered
    void cameraAddKey();
    void cameraDeleteKey();
    void cameraStepKey(int direction); // -1 previous, +1 next
    void cameraSetEase(shotcam::Ease ease);
    // What the canvas is shown (DrawingCanvas::setShotCameraProvider).
    DrawingCanvas::ShotCameraView shotCameraView() const;
    // The bar's state, for the gate: which controls are enabled and what
    // they read ("addKey" "Add Key" / "Key set", "ease" text, ...).
    QVariantMap cameraBarStateForTest() const;
    QWidget *cameraBarForTest() const { return m_cameraPanel; }

    // THE QUICK MOVES (Phase 1D, 2026-10-29; Figma 1D-1 to 1D-5). The
    // Camera bar's Quick Move popover, the CAMERA row's and the SHOTS clip's
    // menus. A move is two keys over its SPAN - this panel, from it to the
    // scene's end, or the loop region (inside one scene): START the camera
    // as it already is at the span's first frame (shotcam::quickMove may
    // give way, decisions 3 and 4), END that framing moved; the START key
    // takes the chosen ease, the END key the ease the move through it had.
    // Keys inside the span are replaced, keys on its ends updated, keys
    // outside never touched. One "Quick Camera Move" command; the CAMERA
    // lock refuses it. The three amounts are remembered between launches
    // (kCameraQuickKey + "Zoom" / "Move" / "Turn"); the span and the ease
    // for the session.
    enum class QuickSpan { ThisPanel, ToSceneEnd, LoopRegion };
    static const QString kCameraQuickKey;
    shotcam::QuickAmounts quickAmounts() const { return m_quickAmounts; }
    void setQuickAmounts(const shotcam::QuickAmounts &amounts); // also remembered
    QuickSpan quickSpan() const { return m_quickSpan; }
    void setQuickSpan(QuickSpan span) { m_quickSpan = span; }
    shotcam::Ease quickEase() const { return m_quickEase; }
    void setQuickEase(shotcam::Ease ease) { m_quickEase = ease; }
    void cameraQuickMove(shotcam::QuickMove move);             // at the canvas's panel
    void cameraQuickMoveOnPanel(shotcam::QuickMove move, int flatPanel);
    void showQuickPreview(shotcam::QuickMove move); // the popover's hover
    void clearQuickPreview();
    QWidget *cameraQuickPopoverForTest() const { return m_quickPopover; }
    // USE LAST FRAMING / CONTINUE CAMERA (Figma 1D-4): the previous scene's
    // camera at its last frame as this scene's single framing (its own keys
    // replaced), or on a key at this scene's first frame (its keys kept).
    void cameraUseLastFraming(int sceneIndex);
    void cameraContinue(int sceneIndex);
    // The SCENES track's menu, handed to the menu hook in the gate.
    void sceneMenuForTest(int sceneIndex) { onSceneMenu(sceneIndex, QPoint()); }
    // For the Dev Recorder's statePoll: shotCam.animate, shotCam.toolActive.
    QVariantMap shotCamToolDevrecState() const;
    // What Edit > Undo / Redo would do now, in the history's words ("Undo
    // Move Camera"); the Dev Recorder logs it with each Undo / Redo action.
    QString undoStepText() const;
    QString redoStepText() const;

    // The animatic section under the canvas (timeline + transport). Created
    // and owned here; MainWindow reaches it for the project's audio path and
    // frame rate.
    AnimaticPage *animatic() const { return m_animatic; }

    // Test hooks for the combined workspace.
    // How many times a panel has been selected: one user action must select
    // exactly once, whichever view it came from (a feedback loop between
    // the strip and the timeline would show up here as more).
    int selectPanelCallsForTest() const { return m_selectPanelCalls; }
    int selectedFlatIndexForTest() const { return currentFlatIndex(); }
    // Whether Space / arrows / Home / End / Delete currently belong to the
    // timeline (the pointer is over it) rather than to the canvas.
    bool timelineKeysArmedForTest() const { return m_timelineKeysArmed; }
    // The gate's pointer (see the .cpp): hear only the Enter / Leave events
    // the test delivers through sendPointerEventForTest, never the real
    // mouse, so the result cannot depend on where the pointer was left.
    void setRealPointerIgnoredForTest(bool ignored);
    void sendPointerEventForTest(QWidget *target, QEvent *event);
    // The clip context menu, handed to the test instead of being shown -
    // and the CAMERA row's key and row menus (Phase 1C), by the same hook.
    void setClipMenuHookForTest(std::function<void(QMenu *)> hook)
    {
        m_clipMenuHookForTest = std::move(hook);
    }
    // Delete over the timeline (the shortcut's own path): a selected camera
    // key if there is one, else the selected panel (with its question).
    void timelineDeleteForTest() { timelineDelete(); }
    bool hasCameraFramingForTest() const { return m_hasCameraFraming; }
    // A locked track's refusals (Part B). The message box an Undo / Redo
    // of a locked track's command shows is handed to the test instead;
    // the last refusal of any kind (key, menu, button, history) is kept.
    void setLockMessageHookForTest(std::function<void(const QString &)> hook)
    {
        m_lockMessageHookForTest = std::move(hook);
    }
    QString lastLockRefusalForTest() const { return m_lastLockRefusal; }
    void clearLockRefusalForTest() { m_lastLockRefusal.clear(); }
    // The last non-modal NOTE (the locks' tooltip, not a refusal): today
    // the one a panel removal shows when it took camera keys with it.
    QString lastNoteForTest() const { return m_lastNote; }
    void clearNoteForTest() { m_lastNote.clear(); }
    Panel *currentPanelForTest() const { return currentPanel(); }
    // Delete Panel without its confirmation box (the gate cannot answer
    // it); everything after the question is the real path.
    void deleteSelectedPanelConfirmedForTest();
    // Layer-stack undo: restore a panel's whole layer vector + active index
    // (QImage handles are implicitly shared, so snapshots are cheap), then
    // refresh the canvas/panel/thumb UI.
    void applyLayerStackForUndo(Panel *panel, const QVector<Layer> &layers,
                                int activeIndex);

    // Panel-level clipboard. Copy stores an owned deep copy; paste inserts a
    // fresh clone (new layer UUIDs) each time. Cut is blocked on a scene's
    // last panel, same rule as Delete.
    void copySelectedPanel();
    void cutSelectedPanel();
    void pastePanelAfterSelected(); // after the selected panel
    void pastePanelInPlace();       // at the copied-from position
    bool hasPanelClipboard() const { return m_panelClipboard != nullptr; }
    // Would pasting the clipboard panel leave this project holding panels
    // of two different sizes? The refusal's decision, without its dialog —
    // exposed so the gate can assert it directly.
    bool pasteWouldMixSizes() const;

    // Is the user mid-edit? A project resize refuses rather than committing
    // it; `what` names the edit for the message.
    bool hasActiveEdit(QString *what = nullptr) const;
    // Everything on the storyboard side that a completed canvas resize must
    // update or clear. Called AFTER the panels have been swapped, with the
    // centre offset that was applied.
    void applyProjectResize(const QSize &newSize, const QPoint &offset);

signals:
    void panelClipboardChanged(bool available); // enables the paste actions
    // A change to the PROJECT's contents that does NOT go through the undo
    // stack (the window watches that stack separately for everything that
    // does). Today: the Shot Info fields, which are written straight to the
    // panel. Anything added here that mutates the model silently must emit
    // this, or the project can be closed with the change unsaved.
    void documentChanged();
    void backRequested();
    void consistencyBoardRequested();
    void settingsRequested(); // Layers toolbar Settings button -> Preferences
    // A camera edit was made (for the Dev Recorder's shotCamEdit event):
    // what, on which scene frame, and the camera before and after.
    void shotCameraEdited(const QVariantMap &event);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    QWidget *createLeftColumn();
    QWidget *createCenterColumn();
    QWidget *createPanelControls(); // fixed, non-scrolling column at the left of the strip
    QWidget *createRightColumn();
    QWidget *createLayerPanel();
    void createFloatingToolbar();   // Brush bar + extras bar (FloatingToolWindows)
    // Floating overlay panel: dock-style header (title + Close only),
    // draggable by the header, child of the canvas.
    QWidget *createFloatingPanel(const QString &title, QWidget *body);
    QWidget *createCameraPanel();   // floating panel shown while Camera is active
    QWidget *createPerspectiveModifier(); // shown while Perspective is active
    QWidget *createShapesPanel();   // floating panel shown while Shapes is active
    QWidget *createBottomBar();

    // Dockable panel plumbing (Qt Advanced Docking System hosts the panels).
    void installDockViewActions(); // dock toggles + Reset Layout in the View menu
    void applyDefaultDockLayout(); // restores the captured default ADS layout
    bool restoreDockState();       // true if a saved layout was applied
    void saveDockState();

    // Layer panel (docked, right of the canvas; Figma 7-70).
    void rebuildLayerPanel();     // rows from the current panel's layer stack
    void setActiveLayer(int index);
    // Photoshop-style selection: plain click = single, Shift = range from
    // the anchor, Ctrl = toggle. Selection indices live in m_layerSelection;
    // the ACTIVE layer (drawing target) is panel->activeLayerIndex.
    void layerRowClicked(int index, Qt::KeyboardModifiers modifiers);
    QVector<int> selectedLayers() const; // ascending, valid, non-background
    void layerAdd();              // blank raster layer above the active one
    void layerAddImage();         // file dialog -> new image-type layer
    void layerDeleteSelected();   // never deletes Background / the last layer
    void layerDuplicateSelected();
    void layerMergeSelected();    // multi: into one; single: merge down
    void layerClearSelected();    // wipe pixels to transparent
    void layerGroupSelected();    // flatten selection into one "Group" layer
    void layerSetColorTag(int index);
    void layerBeginRename(int index); // inline QLineEdit over the name label
    // Drag-reorder: move the given ascending source indices so the block
    // starts at insertAt (a gap index in the PRE-move list); relative order
    // of the moved layers is preserved.
    void layerMoveTo(const QVector<int> &sources, int insertAt);
    void refreshLayerCanvas();    // repaint canvas + panel thumbnail after a layer change

    void layerContextMenu(int index, const QPoint &globalPos);
    void startLayerDrag(int index); // QDrag carrying the selected indices
    // Merge core: flatten the given ascending indices into the lowest one
    // (optionally renaming it).
    void mergeLayerIndices(const QVector<int> &indices, const QString &newName);
    // Real deep copy into another panel; a group row copies its whole folder.
    void layerDuplicateToPanel(int index);
    bool duplicateLayerToPanelCore(int index, Panel *target);
    void layerUngroup(int groupIndex);     // dissolve a folder, keep members
    // Selection expanded so any selected group row brings its whole member
    // block along (delete/duplicate/move act on blocks).
    QVector<int> expandGroupBlocks(const QVector<int> &indices) const;
    // Every layer operation funnels its undo entry through here: snapshot
    // taken BEFORE the mutation, current state captured as "after".
    void pushLayerCommand(Panel *panel, const QVector<Layer> &before,
                          int beforeActive, const QString &text);
    // Delete confirmation (item: Delete/Cancel + "don't ask again" for
    // EMPTY layers only). True = go ahead.
    bool confirmLayerDelete(const QVector<int> &indices);
    void updateActiveLayerThumb(); // live row-thumbnail refresh while drawing
    // Selection-only refresh: restyle the EXISTING rows in place (slider,
    // buttons, canvas mirror included) — no widget teardown, so clicking
    // rows in a deep stack neither flickers nor re-scales every thumbnail.
    void updateLayerSelectionUi();
    // Panel-strip thumbnail: refreshCurrentThumb() debounces the expensive
    // flatten+scale behind a short timer (thumbnails settle after changes);
    // ...Now() does the actual work; flush runs a pending refresh early
    // (before panel switches).
    void refreshCurrentThumbNow();
    void flushThumbRefresh();

    void rebuildSceneList();
    void rebuildPanelStrip();
    // --- Panel Strip dock (top/bottom, resizable, floatable) --------------
    void setupPanelStripDock();     // wrap the strip bar in its QDockWidget
    void onPanelStripResized();     // live label rescale + debounced regen
    // Title-bar-less drag: a press on any BACKGROUND area of the strip
    // undocks/drags/docks it (top/bottom preview) or floats it; presses on
    // interactive children (thumbnails, buttons, scrollbars) pass through.
    bool stripBackgroundEvent(QObject *object, QEvent *event);
    bool stripHitIsInteractive(const QPoint &globalPos) const;
    void beginStripWindowDrag(const QPoint &globalPos);
    void updateStripWindowDrag(const QPoint &globalPos);
    void finishStripWindowDrag(const QPoint &globalPos);
    Qt::DockWidgetArea stripDropZone(const QPoint &globalPos) const;
    // Proportional scaling: ONE factor (strip height / 159, clamped 1..2)
    // drives buttons, icons, fonts, spacing, margins, and thumbnails.
    void applyStripScale();
    void applyPanelNumStyle(class QLabel *num) const;
    // Bucketed, cached, DPR-aware SVG rasterization for the strip buttons
    // (dprOverride > 0 forces a DPR — used by the HiDPI tests).
    QPixmap stripIconPixmap(const QString &svg, const QSize &logical,
                            qreal dprOverride = 0.0);
    // Re-render every strip thumbnail at the current thumb size and DPR
    // (dprOverride > 0 forces a specific DPR — used by the HiDPI tests).
    void regenerateStripThumbs(qreal dprOverride = 0.0);
    QPixmap stripThumbPixmap(Panel *panel, qreal dprOverride = 0.0) const;
    void savePanelStripState();     // versioned QSettings keys (v1)
    void restorePanelStripState();  // area/height/floating/geometry + clamp
    void resetPanelStripToDefault(); // Reset Layout: top, visible, 159 px
    void updateSceneCardStyles();
    void updatePanelThumbStyles();

    // panelIndex: which panel of that scene to land on (the timeline selects
    // across scenes, and going through panel 0 first would switch the canvas
    // twice).
    void selectScene(int index, int panelIndex = 0);
    void selectPanel(int index);

    // --- The animatic section (timeline under the canvas) -----------------
    void setupAnimaticSection(QWidget *centerColumn, QWidget *bottomBar,
                              class QVBoxLayout *centralLayout);
    int currentFlatIndex() const; // the selection over ALL scenes, or -1
    bool flatToScenePanel(int flat, int *sceneIndex, int *panelIndex) const;
    void selectFlatPanel(int flat);          // a timeline / transport request
    void onDurationChangeRequested(int sceneIndex, int panelIndex, int seconds);
    void onTimelineMoveRequested(int flat, int flatGap);
    void onAudioChangeRequested(const QString &newPath, bool linked,
                                const QString &commandText);
    void onClipContextMenu(int flat, const QPoint &globalPos);
    void applyTimelineCollapsed(bool collapsed);
    void applyTimelineHeight();              // the pending expanded height
    void saveTimelineState();
    void restoreTimelineState();
    void resetTimelineToDefault();           // Reset Layout: open, default height
    void updateTimelineKeys();               // arm/disarm by where the pointer is
    void addPanelToScene(int sceneIndex);
    void addPanelAfterSelected();           // control column "+": insert after the selected panel
    void deleteSelectedPanel();             // control column trash: confirm, then delete (blocks the last)

    void loadShotInfo();   // current panel -> right column widgets
    void saveShotInfo();   // right column widgets -> current panel
    void refreshCurrentThumb();
    void updateOnionGhost(); // feed the previous panel's pixmap to the canvas
    void updateLightTable(); // feed the neighbour pixmaps (red prev / green next)

    // Panel reordering (drag within the strip + keyboard).
    void beginPanelDrag();
    void updatePanelDrag(const QPoint &globalPos);
    void finishPanelDrag();
    void cancelPanelDrag();
    int dropTargetForX(const QPoint &globalPos) const;
    void movePanel(int from, int target); // target is an insertion index (0..N)
    void movePanelBy(int delta);           // keyboard: -1 left, +1 right

    void duplicatePanel();                 // copy current panel, insert after it
    static Panel *clonePanel(const Panel *source); // deep copy, fresh layer UUIDs
    // true = the paste was refused (and the user told why) because the
    // clipboard panel's size differs from this project's. Paste paths only:
    // duplicate deliberately does not consult it.
    bool refuseMismatchedPaste();
    void insertPanelClone(const Panel *panel, int insertAt,
                          const QString &text); // into the current scene (undoable)
    void importImageToPanel();             // file dialog -> canvas->importImage
    void updateDuplicateButton();          // enable panel-action buttons when a panel is selected

    Scene *currentScene() const;
    Panel *currentPanel() const;

    QVector<Scene *> m_scenes;
    const QVector<ConsistencyEntry> *m_consistencyEntries = nullptr; // read-only
    int m_currentScene = -1;
    int m_currentPanel = -1;

    // Edit-menu panel clipboard: owned deep copy plus the position it was
    // copied from (for Paste in Place).
    Panel *m_panelClipboard = nullptr;
    int m_clipboardSceneIndex = -1;
    int m_clipboardPanelIndex = -1;
    // Which clipboard the LAST copy/cut fed — paste routes to the same one.
    enum class ClipSource { None, Canvas, PanelLevel };
    ClipSource m_lastClipSource = ClipSource::None;

    // Native docking: an embedded (child-widget) QMainWindow hosts the stock
    // QDockWidget engine; the reusable DockController adds custom title
    // bars, previews, tab/split drops, collapse and persistence. The canvas
    // area is the host's central widget; Scenes, Layers, and Shot Info are
    // QDockWidgets around it.
    QMainWindow *m_dockHost = nullptr;
    DockController *m_dockController = nullptr;
    QDockWidget *m_layersDock = nullptr;
    QDockWidget *m_scenesDock = nullptr;
    QDockWidget *m_shotInfoDock = nullptr;

    // Left column.
    QVBoxLayout *m_sceneListLayout = nullptr;
    QVector<QWidget *> m_sceneCards;

    // Center column.
    QHBoxLayout *m_panelStripLayout = nullptr;
    QVector<QWidget *> m_panelThumbs;
    QVector<QLabel *> m_panelThumbImages;
    // Panel Strip dock: top/bottom only, height-resizable, floatable to any
    // monitor. Thumbnails scale with the strip height (m_thumbW/H replace the
    // old fixed 160x90); a resize drag rescales the existing pixmaps live and
    // regenerates crisp ones ONCE through m_stripRegenTimer.
    QDockWidget *m_panelStripDock = nullptr;
    QWidget *m_panelStripBar = nullptr;
    Qt::DockWidgetArea m_stripLastArea = Qt::TopDockWidgetArea;
    // The strip's LOGICAL visibility — what the user last chose. Qt unchecks
    // a dock's toggleViewAction on ANY hide event, including the ancestor
    // hide when another stack page becomes current, so the action's checked
    // state is NOT a record of user intent and must never be persisted.
    // The View-menu toggle is the strip's only deliberate show/hide control
    // (it has no close button), so only its triggered() updates this.
    bool m_stripUserVisible = true;
    QTimer *m_stripRegenTimer = nullptr;
    int m_stripRegenCount = 0; // regen passes (verified: 1 per settled drag)
    // Invalid until MainWindow supplies it; makeProjectPanel() refuses to
    // create a panel without it — never a 960x540 stand-in.
    QSize m_projectCanvasSize;
    int m_thumbW = 160;
    int m_thumbH = 90; // 16:9
    // Title-bar-less window drag state (background press-drag on the strip).
    QWidget *m_stripContainer = nullptr; // thumbnail row inside the scroll
    // Dock-target preview. A plain themed QWidget, NOT a QRubberBand:
    // the stock band paints from the system palette (the OS accent).
    QWidget *m_stripPreview = nullptr; // top/bottom dock-target preview
    bool m_stripDragCandidate = false;
    bool m_stripDragging = false;
    QPoint m_stripPressGlobal;
    QPoint m_stripDragOffset;
    // Proportional scale (1.0 at the 159px base strip, clamped to 2.0) and
    // the scaled controls: buttons registered with their base metrics.
    double m_stripScale = 1.0;
    struct StripCtl
    {
        QPushButton *button;
        QSize baseButton;
        QSize baseIcon;
        QString svg;
    };
    QVector<StripCtl> m_stripCtls;
    QWidget *m_stripCtlColumn = nullptr;
    QVBoxLayout *m_stripCtlLayout = nullptr;
    QHBoxLayout *m_stripBarLayout = nullptr;
    QHash<QString, QPixmap> m_stripIconCache; // svg|WxH@dpr -> pixmap
    int m_iconRasterCount = 0; // seam metric: bucketed rasterizations
    DrawingCanvas *m_canvas = nullptr;
    // The top-level window, watched for WindowBlocked/WindowUnblocked so the
    // floating tool windows hide while any modal dialog is open (they are
    // Qt::Tool windows owned by it and would otherwise paint over the modal).
    QPointer<QWidget> m_modalWatchWindow;
    ZoomToolbar *m_zoomToolbar = nullptr; // custom-painted view controls
    brushlib::BrushLibraryModel *m_brushLibModel = nullptr;
    brushlib::BrushLibraryPanel *m_brushLibPanel = nullptr;
    brushlib::BrushSettingsStudio *m_brushStudio = nullptr; // Figma 274:23
    QString m_activeBrushPresetId; // last library selection (dirty tracking)
    QString m_activeEraserPresetId; // the ERASER's own selection (v1: each
                                    // tool remembers its preset; switching
                                    // tools changes neither)
    std::function<void()> m_syncEraserCtl; // Size CTL <- eraser preset
    QPushButton *m_brushToolButton = nullptr; // Library anchor + open toggle
    QPushButton *m_eraserToolButton = nullptr; // eraser-scope anchor
    QAction *m_brushLibViewAction = nullptr; // View menu toggle
    bool brushStudioUnderCursor() const; // Ctrl+Z routing (studio-local undo)
    void undoDocument(); // Edit > Undo AND the toolbar's Undo button
    void redoDocument();
    void refreshBrushDirtyState();       // white dot + Reset chip on the row
    QScrollArea *m_panelScroll = nullptr;
    QPushButton *m_importButton = nullptr;
    // Floating Size CTL toolbar (Figma 209:42): size + opacity sliders and
    // Flip, snapping to the left/right canvas edge. (Brush Options is gone;
    // its pressure toggles were removed deliberately, and hardness is
    // edited in the Studio — its per-tool persistence lives on under
    // toolCtl/brush/hardness. See HANDOFF.md.)
    QWidget *m_sizeCtlBar = nullptr;
    // The Camera bar (visible only while the Camera tool is active).
    QWidget *m_cameraPanel = nullptr;
    std::function<void()> m_syncCameraBar; // bar controls <- scene / playhead
    bool m_cameraAnimate = false;    // session; off at every launch
    bool m_cameraStayInside = true;  // session; on at every launch
    // Where a camera edit lands: the canvas's panel, at the playhead's
    // frame when the playhead is in it (its first frame otherwise).
    bool cameraTarget(Scene **scene, int *panelIndex, int *frameInPanel) const;
    double cameraSceneFrame(const Scene *scene, int panelIndex,
                            int frameInPanel) const;
    int cameraFps() const;
    bool cameraCovered() const; // the preview is over the canvas
    void onShotCameraEdit(const shotcam::CameraState &state, int kind);
    void pushCameraEdit(Scene *scene, const shotcam::SceneCamera &before,
                        const shotcam::SceneCamera &after, const QString &text);
    // THE CAMERA ROW (Phase 1C). The CAMERA lock refuses every key edit
    // (row, menu, Delete, the Camera bar and the canvas tool, Undo / Redo).
    // A key pressed on the row: its panel selected, the playhead on it. A
    // key dragged: re-anchored to the panel it lands in. The key's menu and
    // the row's (Figma 1C-4): Ease, Copy / Paste Framing, Delete Key; Add
    // Key / Paste Framing at Playhead. Each edit is one CameraEditCommand.
    bool cameraLocked() const;
    bool refuseCameraEdit(); // true (and says so) when locked
    void timelineDelete();
    bool cameraKeyLive(int sceneIndex, int keyIndex) const;
    void cameraPlayheadTo(int sceneIndex, double sceneFrame);
    void cameraGoToKey(int sceneIndex, int keyIndex);
    void cameraMoveKey(int sceneIndex, int keyIndex, int sceneFrame);
    void cameraSetKeyEase(int sceneIndex, int keyIndex, shotcam::Ease ease);
    void cameraDeleteKeyAt(int sceneIndex, int keyIndex);
    void cameraCopyFraming(int sceneIndex, int keyIndex);
    void cameraPasteFraming(int sceneIndex, int keyIndex);
    void cameraAddKeyAtPlayhead(bool paste);
    bool cameraKeyAtPlayhead() const; // the row menu's "Key Set at Playhead"
    bool deleteSelectedCameraKey(); // false: no key selected (Delete is the panel's)
    QString cameraKeyHeader(int sceneIndex, int keyIndex, bool *last) const;
    void onCameraKeyMenu(int sceneIndex, int keyIndex, const QPoint &globalPos);
    void onCameraRowMenu(int flatPanel, const QPoint &globalPos);
    // Phase 1D. A quick move worked out (not applied): the span, START /
    // END, the camera after it, and the live keys it replaces. False (and
    // *why* set) when there is nothing to do.
    struct QuickPlan
    {
        int sceneIndex = -1;
        int startPanel = 0, endPanel = 0;
        double startFrame = 0, endFrame = 0; // in the scene
        shotcam::QuickFrames frames;
        shotcam::SceneCamera after;
        QVector<int> replaced;
    };
    // *span*: the popover's Over; the CAMERA row's and the clip's menus act
    // on the panel they name, so they pass ThisPanel (the 1D hand test).
    bool quickMovePlan(shotcam::QuickMove move, int sceneIndex, int panelIndex, QuickSpan span,
                       QuickPlan *plan, QString *why) const;
    void applyQuickPlan(const QuickPlan &plan);
    QString quickSpanText(int sceneIndex, int panelIndex) const; // the popover's title
    void addQuickMoveEntries(QMenu *menu, const QString &title, int flatPanel);
    void addSceneCameraEntries(QMenu *menu, int sceneIndex);
    void onSceneMenu(int sceneIndex, const QPoint &globalPos);
    shotcam::CameraState lastFramingOf(int sceneIndex, bool *whole) const;
    int sceneFilmStart(int sceneIndex) const; // frames before the scene
    shotcam::QuickAmounts m_quickAmounts;      // loaded with the guides
    QuickSpan m_quickSpan = QuickSpan::ThisPanel;
    shotcam::Ease m_quickEase = shotcam::Ease::EaseInOut;
    QWidget *m_quickPopover = nullptr;
    bool m_quickPreviewValid = false; // the hover preview, for the canvas
    int m_quickPreviewScene = -1;
    shotcam::CameraState m_quickPreviewStart, m_quickPreviewEnd;
    bool m_hasCameraFraming = false;   // Copy Framing's clipboard (session)
    shotcam::CameraState m_cameraFraming;
    QString m_cameraFramingSource;     // "key 3": named by Paste
    void refreshCameraViews(Scene *scene); // canvas, preview, clips, strip, bar
    void loadCameraGuides();               // the remembered setting
    // Figma "Floating Toolbar Layers" (node 173-36): dock toggles + tools.
    QWidget *m_layersToolbar = nullptr;
    // Perspective Modifier toolbar (visible only while Perspective is active).
    QWidget *m_perspModToolbar = nullptr;
    std::function<void()> m_syncPerspective; // toolbar controls <- canvas model
    std::function<int()> m_sizeCtlSizeForTest; // reads the (file-local) slider
    std::function<void(int)> m_sizeCtlUserSetSizeForTest; // user-style set
    std::function<void(int)> m_sizeCtlUserSetOpacityForTest;
    std::function<int()> m_sizeCtlOpacityForTest; // the bar MULTIPLIER, %
    // Shapes panel (visible only while the Shapes tool is active).
    QWidget *m_shapesPanel = nullptr;
    // Last-chosen selection mode: a plain click on the combined Selection
    // button re-activates it; hold/right-click opens the mode menu.
    DrawingCanvas::Tool m_selectionMode = DrawingCanvas::SelectRect;
    // Floating overlays: every bar/panel is a FloatingToolWindow (frameless
    // OS-composited tool window) anchored to the canvas — drag, clamping,
    // main-window follow, page-visibility mirroring, and position persistence
    // all live in that base class (and cover future panels automatically).
    QWidget *m_floatToolbar = nullptr;  // horizontal Brush/tools bar (Figma 33:110)
    SankoTipPopup *m_toolbarTip = nullptr; // ONE reused tooltip for the Brush bar
    QWidget *m_selModToolbar = nullptr;  // Selection Modifier bar (Figma 146:67)
    QWidget *m_moveModToolbar = nullptr; // Move Modifier bar (Figma 161:39)
    QWidget *m_bottomBar = nullptr;      // status bar; both mod bars sit 10px above
    // Fixed control column (left of the panel strip).
    QPushButton *m_addPanelButton = nullptr;
    QPushButton *m_dupPanelButton = nullptr;
    QPushButton *m_clearPanelButton = nullptr; // clears the drawing, asks first
    QPushButton *m_deletePanelButton = nullptr;
    QPushButton *m_lightTableButton = nullptr; // toggles neighbour-panel ghosts

    QUndoStack *m_undoStack = nullptr; // app-wide history (owned by MainWindow)

    // The animatic section: a splitter pane under the canvas column, NOT a
    // dock (it has one place, and a dock would have had to share the bottom
    // area with a strip the user may dock there). Collapsed = its header
    // row; expanded, its height is the user's and is remembered.
    AnimaticPage *m_animatic = nullptr;
    QSplitter *m_workSplitter = nullptr;
    int m_timelineHeight = 0;           // expanded height to apply / restore
    bool m_timelineHeightPending = false; // waiting for the splitter to have a size
    // Keys that belong to the timeline only while the pointer is over it
    // (or over the preview it is playing): disabled otherwise, so Space on
    // the canvas is still the pan modifier.
    struct TimelineKey
    {
        QShortcut *shortcut;
        bool overPreviewToo;
    };
    QVector<TimelineKey> m_timelineKeys;
    bool m_pointerOverTimeline = false;
    bool m_pointerOverPreview = false;
    bool m_timelineKeysArmed = false;
    bool m_ignoreRealPointerForTest = false; // the gate: test events only
    bool m_inTestPointerEvent = false;       // ...and this is one of them
    int m_selectPanelCalls = 0;
    std::function<void(QMenu *)> m_clipMenuHookForTest;
    // The SHOTS lock (Part B): every path that changes the track's content
    // asks refuseShotsEdit() first; the history door asks
    // refuseLockedHistory() about the command it is about to undo or redo.
    bool shotsLocked() const;
    bool refuseShotsEdit();
    bool refuseLockedHistory(const QUndoCommand *command);
    void sayLocked(const QString &message, bool asBox);
    // The same non-modal tooltip for a note that is not a refusal.
    void sayNote(const QString &message);
    // Remove the selected panel with this history text ("Delete Panel",
    // "Cut Panel"); when the panel held camera keys the text names them
    // and a note says the scene's camera move changes (the user's rule,
    // 2026-10-25: a deletion must not change a move silently).
    void removeSelectedPanel(const QString &baseText);
    QString m_lastNote;
    std::function<void(const QString &)> m_lockMessageHookForTest;
    QString m_lastLockRefusal;

    // Drag-reorder state.
    bool m_panelPressActive = false;
    bool m_panelDragging = false;
    int m_dragSourceIndex = -1;
    int m_dropTarget = -1;
    QPoint m_dragStartGlobal;
    QLabel *m_dragGhost = nullptr;        // semi-transparent thumbnail follows cursor
    QWidget *m_dropIndicator = nullptr;   // amber line between thumbnails

    // Layer panel.
    QVBoxLayout *m_layerListLayout = nullptr;
    QVector<QWidget *> m_layerRows;
    QTimer *m_thumbTimer = nullptr; // debounces panel-thumbnail regeneration
    std::function<void(int)> m_syncLayerOpacity; // <0 disables; else sets %
    SankoSlider *m_layerOpacity = nullptr; // (unused; kept for ABI stability)
                                           // (paints its own "NN%" label)
    QPushButton *m_layerDeleteButton = nullptr;
    QPushButton *m_layerMergeButton = nullptr;
    bool m_updatingLayerUi = false; // guards the opacity slider feedback loop
    // Photoshop-style selection state (indices into the current panel's
    // layer vector) + the Shift-range anchor. Rebuilt rows render selected
    // state; cleared on panel switches.
    QSet<int> m_layerSelection;
    int m_layerAnchor = -1;
    Panel *m_layerSelPanel = nullptr;   // selection resets on panel switches
    QWidget *m_layerListHost = nullptr; // drop target for row reordering

    // Right column.
    QComboBox *m_shotType = nullptr;
    QComboBox *m_camera = nullptr;
    QComboBox *m_lens = nullptr;
    QLineEdit *m_mood = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QLabel *m_actionLabel = nullptr;

    bool m_loadingShotInfo = false;
};
