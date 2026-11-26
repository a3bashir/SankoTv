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
class StageClear;
class FloatingToolWindow;
class ColorPanel;
class QAbstractButton;
class LayerDepthPopover;
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
class QKeyEvent;
class QPointerEvent;
class QSinglePointEvent;
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
class SankoTipFrame;

class DockController;
class LayerFolder;
class QDockWidget;
class QMainWindow;
class AnimaticPage;
class QMenu;
class QShortcut;
class QSplitter;

struct Panel;
struct Scene;
struct ConsistencyEntry;

// THE NOTE IN THE STATUS BAR (batch I-1b round 1, 2026-11-25; the user's change after hand
// test I1b-1, reference statusbar_place.png and notes_1-4.png). The bottom bar's space between
// Import Image and "Recovery copy saved" says the canvas's note (DrawingCanvas::noteBarText):
// plain text in the bar's own font and grey, a thin vertical separator either side - "| Drawing
// view - depth shows with the Camera tool |" - centred in the space. A refusal or a warning is
// in amber. Too long for the space, it ends with "..." and its tooltip says it whole (and
// what the note's tip adds: the camera's passed layers). Paints nothing when there is no note.
class StatusNoteBar : public QWidget
{
public:
    explicit StatusNoteBar(QWidget *parent = nullptr);
    // `amberFrom`: a grey note whose end is a warning (the Camera tool's "Soft 1.43x", batch
    // I-1b round 2) - amber from that character on; -1 none.
    void setNote(const QString &text, bool warning, const QString &tip = QString(), int amberFrom = -1);
    QString text() const { return m_text; }
    bool warning() const { return m_warning; }
    QString shownText() const;       // as painted: elided with "..." when too long
    QString amberShownText() const;  // the part of shownText painted amber ("" none, or all of it)
    QColor textColor() const;
    QRect textRect() const;          // where the words are painted, in the bar's px
    QRect separatorRect(int side) const; // 0 the left separator, 1 the right
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    static constexpr int kSeparatorGap = 12; // between a separator and the words
    static constexpr int kSeparatorHeight = 16;

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QFont noteFont() const;
    void refreshTip();
    QString m_text;
    bool m_warning = false;
    QString m_tip;
    int m_amberFrom = -1;
};

// THE WORKSPACE: scene list (left), panel strip over the drawing canvas
// (centre), the animatic timeline under it, and shot info (right). Drawing,
// panels, timing and playback all happen here; there is no separate animatic
// screen. Scenes are passed in from the Script Editor or a project file.
//
// The strip and the timeline are two views of the same panels. This page
// owns the one selection and the one undo history; both views ask it to
// change things and are told the result (see selectPanel and the
// apply...ForUndo callbacks).
// A step from outside the page that the SHOTS lock also holds (Q8, batch 6c): giving
// every panel a stage margin.
class ShotsTrackStep
{
public:
    virtual ~ShotsTrackStep() = default;
    virtual bool changesPanels() const = 0;
};

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
    LayerFolder *layerFolder() const { return m_layerFolder; }
    // The panel on the canvas gets its full images again (a margin changed).
    void reopenCurrentPanel();
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
    // THE STAGE MARGIN (Phase 2A): px each side of the shot (0 x 0: none),
    // a project value like the canvas size. The canvas draws the frame and
    // veils the margin, the strip outlines the frame, the camera's limits
    // and Quick Moves measure against the frame and the stage.
    void setStageMargin(const QSize &margin);
    QSize stageMargin() const { return m_stageMargin; }
    shotcam::Stage stageFor(const Panel *panel) const;
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
    // SYMMETRY (batch H, 2026-11-22): the switch and the axis's place, saved with the
    // project as "symmetry" (empty - nothing written - when off and in the middle); and the
    // switch as the artist sets it, from the tools bar's button or Shift+S.
    QJsonObject symmetryToJson() const;
    void symmetryFromJson(const QJsonObject &object);
    void setSymmetryByUser(bool on);
    void symmetryKey();

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
    // A panel's length, set by the undoable command a timeline drag or the
    // typing field pushes (and by its undo): frames at a rate (2026-11-06:
    // it was whole seconds), written to the panel; re-times the timeline.
    void applyPanelDurationForUndo(Panel *panel, int frames, int rate);
    // A frame-rate change (Project Settings) changed no panel - each keeps
    // its time - but a length that is not a whole number of frames at the
    // new rate is rounded: the note names those panels (Q3, 2026-11-06).
    // Empty when none is.
    QString frameRateNote(int oldFps, int newFps) const;
    void sayFrameRateNote(int oldFps, int newFps);
    // The Shot Info panel's Duration row (2026-11-06): the number, and the
    // "= 1s 12f at 24 fps" beside it.
    QLineEdit *shotInfoFramesForTest() const { return m_shotFrames; }
    QString shotInfoFramesEqualsForTest() const;
    // The scratch audio track, set by the undoable command Import / Remove
    // Audio pushes (and by its undo): empty = no track; linked = an old
    // project's file outside the project (AnimaticPage::audioLinked).
    void applyAudioPathForUndo(const QString &path, bool linked);
    // A scene's shot camera, set by the undoable command every camera edit
    // pushes (a canvas drag, Add / Delete Key, the ease) and by its undo:
    // the keys, then the canvas, the preview, the SHOTS clips, the strip.
    void applyCameraForUndo(Scene *scene, const shotcam::SceneCamera &camera);
    // THE SCENE LENGTH LOCK (batch F1, Figma F-1): set by the lock's command and its
    // undo; the timeline redraws the scene's lock, baseline and end line.
    void applySceneLengthLockForUndo(Scene *scene, bool locked);
    // THE SCENE'S COLOUR (batch E, Figma E-1): "#RRGGBB", or empty for Auto; one undo step
    // ("Change Scene Color") from the scene's menu - on the SCENES row and on its card.
    void applySceneColorForUndo(Scene *scene, const QString &tag);
    void setSceneColor(int sceneIndex, const QString &tag);
    // The test's answer to the colour picker Custom... opens (an invalid colour: cancelled).
    void setSceneColorPickForTest(std::function<QColor(const QColor &)> pick) { m_sceneColorPickForTest = std::move(pick); }
    void openSceneMenuForTest(int sceneIndex) { onSceneMenu(sceneIndex, QPoint()); }
    // Lock or unlock a scene's length (the scene's menu, the lock on the SCENES row):
    // one undo step, and a note.
    void toggleSceneLengthLock(int sceneIndex);
    // BATCH F2. Lock / unlock every scene's length (the Edit menu and the scene's menu):
    // one step for the scenes it changes. Whether any scene is (un)locked, for the menus.
    void setAllSceneLengthsLocked(bool lock);
    bool anySceneLengthLocked(bool locked) const;
    // End the scene at the playhead: its last panel trimmed or lengthened, one step, the
    // scenes after it moving (as a drag of that panel's right edge). The blocker: empty
    // when it can, else why not ("the scene's length is locked", "the playhead is before
    // its last panel", "it ends there"); *lastFrames: the last panel's new length.
    void endSceneAtPlayhead(int sceneIndex);
    QString endSceneAtPlayheadBlocker(int sceneIndex, int *lastFrames = nullptr) const;
    int playheadFilmFrame() const; // -1: none

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
    // STAGE MODE (batch 6c, Figma MARGIN-1): this panel's stage margin, edited on
    // the canvas. The Camera bar's Stage button enters it and the Stage bar stands
    // in for the Camera bar. Esc leaves and cancels; Enter or Apply commits ONE
    // undo step, "Stage Margin (Panel 3)". Nothing changes until then.
    void enterStageMode();
    void leaveStageMode(bool apply);
    bool stageModeOn() const { return m_stageModeOn; }
    // The panel's margins, each side, and what the undo step does: no pixel moves.
    QMargins panelMargins(const Panel *panel) const;
    void applyPanelMargin(Panel *panel, const QMargins &margin);
    QWidget *stageBarForTest() const { return m_stagePanel; }
    void updateOnionGhostForTest() { updateOnionGhost(); }
    void addPanelForTest() { addPanelAfterSelected(); }
    QWidget *moveModToolbarForTest() const { return m_moveModToolbar; } // batch F1's chip check
    // The floating bar at the top centre (Layers, Perspective, Scenes, ...): batch I-1b's note
    // slot sits 8 px below it.
    void duplicateSelectedPanelForTest() { duplicatePanel(); }
    // Batch I-1's checks: a Layers row pressed as the artist presses it; the footer's Merge.
    void layerRowClickedForTest(int index) { layerRowClicked(index, Qt::NoModifier); }
    void layerMergeSelectedForTest() { layerMergeSelected(); }
    QVariantMap stageBarStateForTest() const;
    // The project's margin (for new panels) moved, and no panel with it.
    void projectMarginMoved(const QSize &newSize, const QPoint &offset);
    // GROW TO FIT THIS MOVE (batch 6d, Figma MARGIN-2): the offer's words for a panel
    // whose art the camera leaves (empty: it does not), and the step that gives it.
    QString growToFitText(int sceneIndex, int panelIndex, QMargins *grow = nullptr) const;
    void showQuickPreviewForTest(shotcam::QuickMove move) { showQuickPreview(move); }
    QPixmap stripThumbForTest(Panel *panel, qreal dpr) const { return stripThumbPixmap(panel, dpr); }
    void clearQuickPreviewForTest() { clearQuickPreview(); }
    void growPanelToFit(int sceneIndex, int panelIndex);
    // GROW ALL / START THE MOVE HERE (batch E, Figma E-1): the move a panel's art exit
    // belongs to - the live keys around its first frame off the art (-1 where the framing is
    // held before the first key or after the last) - and the panels of it whose art it
    // leaves. The offers' words (empty: not offered) and the steps that give them.
    struct MoveExits
    {
        int fromKey = -1, toKey = -1;     // key indices; -1 at a held framing's open end
        double fromFrame = 0, toFrame = 0; // scene frames of the span
        QVector<int> panels;               // the scene's panels it leaves, in order
    };
    bool moveExitsAt(int sceneIndex, int panelIndex, MoveExits *out) const;
    QString growAllToFitText(int sceneIndex, int panelIndex) const;
    QString startMoveHereText(int sceneIndex, int panelIndex, int *startPanel = nullptr) const;
    void growAllToFit(int sceneIndex, int panelIndex);
    void startMoveHere(int sceneIndex, int panelIndex);
    // Giving every panel a margin (Q8): true, and says so, when the SHOTS track is locked.
    bool refuseMarginChange() { return refuseShotsEdit(); }
    // The Camera bar widened by `px` (0: its own width), placed again: the
    // control of the fit check at the narrowest window (Phase 2E).
    void setCameraBarExtraWidthForTest(int px);

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
    // For the Dev Recorder's statePoll: shotCam.animate, shotCam.toolActive,
    // preview.stageClear.
    QVariantMap shotCamToolDevrecState() const;
    // CLEAR THE STAGE while the film plays (StageClear.h): the setting, and
    // the gate's seams - the state, the faded bars, no fade, the idle timer
    // fired by hand.
    void setClearStageOnPlay(bool on);
    bool clearStageOnPlay() const;
    bool stageClearedForTest() const;
    QVector<QWidget *> stageFadedBarsForTest() const;
    void setStageFadeMsForTest(int ms);
    void fireStageIdleForTest();
    bool stageIdleArmedForTest() const;
    bool stripSteppedAsideForTest() const { return m_stripSteppedAside; }
    // The panels the undo history still holds, by id (Clean Up Project
    // Folder keeps their files; 2026-11-04).
    QSet<QString> panelIdsHeldByHistory() const;
    // THE RECOVERY COPY (2026-11-04): is the artist in the middle of
    // something - a stroke or Quick Shape, a selection, transform or camera
    // drag on the canvas, a panel drag on the strip, a drag on the timeline,
    // the film playing? The copy waits for it to end.
    bool actionInProgress() const;
    // "Recovery copy saved 19:42", dimmed at the right of the bottom bar;
    // empty clears it.
    // (Saving step 2, 2026-11-23; Figma S-1) The same place says the save's word:
    // `lit` - a dot in the accent colour before it ("Saving..."); `failed` - the dot
    // and the words in red ("NOT saved"). recoveryStatusForTest: the words alone.
    void setRecoveryStatus(const QString &text, bool lit = false, bool failed = false);
    QString recoveryStatusForTest() const;
    // The status bar's note (batch I-1b round 1): the widget, and its neighbours in the bar.
    StatusNoteBar *noteBarForTest() const { return m_noteBar; }
    QWidget *recoveryStatusLabelForTest() const;
    QWidget *importButtonForTest() const;
    // DEPTH (Phase 2B): the Depth popover for layer `index` of the current
    // panel, as its badge or Depth... opens it (the paper and a folder's
    // member open nothing); the popover open now, or null.
    void openLayerDepthForTest(int index) { layerOpenDepth(index, QPoint(0, 0)); }
    LayerDepthPopover *depthPopoverForTest() const;
    // The Layers panel's context menu, to read its items (the gate's menu hook).
    void layerContextMenuForTest(int index) { layerContextMenu(index, QPoint(0, 0)); }
    void rebuildLayerPanelForTest() { rebuildLayerPanel(); }
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
    // Whether Space and Esc are the film's just now (batch I-1b round 2, after hand test
    // I1b-3): the film is up and no text field is being typed in - wherever the pointer is.
    bool filmKeysArmedForTest() const { return m_filmKeysArmed; }
    // The canvas's keys go where the pointer is (the trial of 2026-11-09): Space,
    // Enter and Esc pressed while the pointer is over the canvas are the canvas's,
    // whatever holds the keyboard. Called for every key press and release in the
    // application (CanvasKeyRouter); true when the canvas took the key.
    bool routeCanvasKey(QObject *receiver, QKeyEvent *event);
    // TAB HIDES OR SHOWS THE FLOATING TOOLBARS (batch C, Photoshop's Tab): wherever the
    // pointer is, in this page's window, unless a text field has the keyboard. Called
    // before routeCanvasKey; true when it took the key (its release and repeats too,
    // so Tab never walks the keyboard focus here).
    bool routeToolbarsKey(QObject *receiver, QKeyEvent *event);

    // THE FOUR FLOATING TOOLBARS (batch C; Figma C-2): View > Toolbars ticks each one
    // (remembered), and Tab hides all four for the session (not remembered); a toolbar
    // shows when it is ticked and Tab has not hidden them.
    enum class Toolbar { Tools, Layers, Zoom, SizeSliders };
    bool toolbarShown(Toolbar bar) const;
    void setToolbarShown(Toolbar bar, bool shown);
    bool toolbarsHiddenByTab() const { return m_toolbarsTabHidden; }
    void setToolbarsHiddenByTab(bool hidden);
    QWidget *toolbarWindowForTest(Toolbar bar) const { return toolbarWindow(bar); }
    static QString toolbarKey(Toolbar bar);    // the ticks' settings
    static const QString kCameraMoveArrowsKey; // View > Camera Move Arrows' setting
    // VIEW > DRAWING OVERLAYS (the user's point 3, 2026-11-21; DrawingCanvas::DrawingOverlay):
    // each entry's setting - the ticks are the app's, the same for every project - its text
    // in the submenu, and its action's name ("viewOverlay...").
    static QString drawingOverlayKey(DrawingCanvas::DrawingOverlay which);
    static QString drawingOverlayText(DrawingCanvas::DrawingOverlay which);
    static QString drawingOverlayActionName(DrawingCanvas::DrawingOverlay which);
    // THE PANEL STRIP'S BACKGROUND (STRIP-1, approved 2026-11-18): the colour behind the panels
    // in the strip - its ground and the cells behind the thumbnails (on a light ground an
    // unselected cell's edge a shade darker) - on screen only: never a panel, a thumbnail's
    // picture, the film, an export or the project. Remembered by the app for every project, not
    // an undo step. Presets and Custom... in View > Panel Strip Background and on a right-click
    // on the strip's empty ground; floating, the strip's own window takes it too.
    QColor panelStripBackground() const { return m_stripGround; }
    void setPanelStripBackground(const QColor &color);
    void addPanelStripBackgroundMenu(QMenu *parent); // the submenu, wherever it is offered
    static const QString kPanelStripBackgroundKey;
    QWidget *panelStripBarForTest() const { return m_panelStripBar; }
    QWidget *panelStripGroundForTest() const { return m_stripContainer; }

    // THE TOOL KEYS (batch C, the user's choice): B Brush, E Eraser, G Fill, V Move, L the
    // freehand selection, M the rectangle selection while the film is paused (M marks a
    // camera key while it plays, as before). As a press on the tool's button, less what
    // the press also opens (the Brush and Eraser buttons open the Library). I waits for
    // the eyedropper (batch D).
    void toolKey(DrawingCanvas::Tool tool);
    // THE EYEDROPPER (batch D; the user's D-2): the Colors panel's button and the I key arm
    // ONE pick (again: off); Alt held with the Brush picks while it is held (the Alt key,
    // reported here by CanvasKeyRouter wherever it went). A pick gives its colour to the
    // brush, the swatch and the Colors panel, and never hides the panel.
    void toggleColorPick();
    void noteAltKey(QObject *receiver, bool down);
    // FLIP H / V (batch D): the Move bar's buttons and Edit > Flip Horizontal / Vertical -
    // part of the open Move box (DrawingCanvas::flipTransform).
    void editFlipHorizontal();
    void editFlipVertical();
    bool canFlipTransform() const;
    // THE COLORS PANEL'S RULES (batch D; D-1's, as the Library's; no pin): anchored under
    // (or over) the swatch each time it opens; a click on the swatch opens and closes it;
    // it hides at the first stroke, when another tool is picked and at a press on another
    // toolbar button; whether it was open is remembered; View > Colors ticks with it.
    void setColorsPanelShown(bool shown); // the user's choice: remembered
    QWidget *colorsPanelForTest() const;
    QScrollArea *colorsScrollForTest() const { return m_colorsScroll; }
    QAbstractButton *colorPickButtonForTest() const { return m_colorPickButton; }
    QAbstractButton *colorSwatchForTest() const { return m_colorSwatch; }
    static const QString kColorsPanelKey; // whether the Colors panel was open
    static const QString kLastPanelKey;   // "library" or "colors": the one of the two shown last
    static QString panelToRestoreAtLaunch(); // "library", "colors" or none (D-3: one at launch)
    // [ AND ] (batch C): the active Brush's or Eraser's size one step down or up, as
    // Photoshop steps it - 1 px under 10, 10 under 100, 25 under 200, 50 under 300, 100
    // under 1000, then 250 - through the size slider's own path (the pill shows it, the
    // size is remembered). False when nothing changed (another tool, at a limit).
    bool stepToolSize(int direction);
    static int bracketSizeStep(int size, int direction);
    // ...and where the pointer is comes from every pen and mouse move and press as
    // well as the canvas's Enter / Leave (CanvasKeyRouter again; never takes the event).
    void notePointerAt(QObject *receiver, QPointerEvent *event);
    // A click outside the Brush Library or the Colors panel closes it (2026-11-21): every
    // press and release in the application, at the widget it is for (see the .cpp).
    void notePanelClick(QObject *receiver, QSinglePointEvent *event);
    bool pointerOverCanvasForTest() const { return m_pointerOverCanvas; }
    // Under the gate the real pointer is not heard; the moves of the pen with this
    // unique id are (sent at the window-system level, as a real pen's). -1: none.
    void setPointerDeviceForTest(qint64 uniqueId);
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
    // Batch E (Figma E-1): the five eases' popover a tap on a move's mark opens, while open.
    QWidget *easePopoverForTest() const { return m_easePopover.data(); }
    Panel *currentPanelForTest() const { return currentPanel(); }
    // The strip's parallel thumbnails (2026-10-31): how many were built off
    // the UI thread since the last reset.
    static int stripThumbsBuiltOffUiForTest();
    static void resetStripThumbCountForTest();
    // Delete Panel without its confirmation box (the gate cannot answer
    // it); everything after the question is the real path.
    void deleteSelectedPanelConfirmedForTest();
    // Layer-stack undo: restore a panel's whole layer vector + active index
    // (QImage handles are implicitly shared, so snapshots are cheap), then
    // refresh the canvas/panel/thumb UI.
    void applyLayerStackForUndo(Panel *panel, const QVector<Layer> &layers,
                                int activeIndex);

    // --- LAYER MOTION (batch I-1, 2026-11-24; Figma I-1 693:755, approved) -----------------
    // The playhead's place in the panel on the canvas (what the canvas poses at).
    DrawingCanvas::MotionView motionView() const;
    // The Layers panel's Animate diamond on row `index`: on, off (its keys kept, unused).
    void layerToggleAnimate(int index);
    QAbstractButton *layerAnimateButtonForTest(int index) const;
    // A key at the playhead on `layerIndex` of the panel on the canvas, with `pose` - set or
    // replaced, one undo step `text`. The first key gives the layer its pivot (`pivot`,
    // stage px: the centre of its art). What the Move box and the opacity slider write.
    void setLayerKeyAtPlayhead(int layerIndex, const shotcam::LayerPose &pose, const QPointF &pivot,
                               const QString &text);
    // The LAYER row's requests (batch I-1): the playhead to a key (its panel and layer
    // chosen); a key moved in its panel; its menu (Delete Key); its move's five eases.
    void layerGoToKey(int flat, int layerIndex, int keyIndex);
    void layerMoveKey(int flat, int layerIndex, int keyIndex, int frameInPanel);
    void layerDeleteKey(int flat, int layerIndex, int keyIndex,
                        const QString &text = QStringLiteral("Delete Layer Key"));
    void layerSetKeyEase(int flat, int layerIndex, int keyIndex, shotcam::Ease ease);
    void onLayerKeyMenu(int flat, int layerIndex, int keyIndex, const QPoint &globalPos);
    void onLayerRowMenu(const QPoint &globalPos); // off a key: Paste Key at the Playhead alone
    void onLayerEaseMark(int flat, int layerIndex, int keyIndex, const QRect &globalMarkRect);
    // COPY, CUT AND PASTE OF LAYER KEYS (batch I-1b round 2, 2026-11-25; the user's note 5 and
    // answer 4, Figma I-1b 701:755 section 4). While the LAYER row is the last thing pressed,
    // Ctrl+C, Ctrl+X, Ctrl+V and Delete are its: a key pressed there is the selected one; a
    // press on its empty track chooses the row with no key, for Paste. Any other press - the
    // canvas, a clip, another row, a panel - gives the keys back their jobs (the canvas
    // selection, else the panel), so Ctrl+X never cuts a panel while a key is selected, nor a
    // key once the artist has moved on. Copy keeps the key ready across panels and scenes until
    // another is copied. Paste goes to the layer selected in the Layers panel, in the panel under
    // the playhead, at the playhead, and keeps the PLACE: the pasted move is d2 = (pivot1 + d1) -
    // pivot2 (frame space - the scene's canvas, whatever each panel's camera frames), turn, size,
    // opacity and ease as copied; a layer with no keys gets Animate on and its pivot at the centre
    // of its art (the first key's rule); a key already at the playhead is replaced. Each cut or
    // paste is one undo step. Delete takes the selected key, one step, never the panel.
    void copyLayerKey();
    void cutLayerKey();
    void pasteLayerKey();
    bool layerRowFocusedForTest() const { return m_layerKeyFocus.row; }
    int layerRowFocusKeyForTest() const { return m_layerKeyFocus.row ? m_layerKeyFocus.key : -1; }
    bool layerKeyCopiedForTest() const { return m_layerKeyClip.has; }
    // Edit > Paste works for a key now: the LAYER row is the last thing pressed and a key is copied.
    bool layerKeyPasteReady() const { return m_layerKeyFocus.row && m_layerKeyClip.has; }
    // Any press: on the LAYER row it changes nothing here (the row says what it chose); anywhere
    // else - but a menu - it hands the edit keys back (CanvasKeyRouter).
    void noteLayerRowPress(QObject *receiver, QEvent *event);
    // The keys' colours changed (Edit > Preferences > Keyframes): everything that shows a key.
    void keyColorsChanged();
    // Everything that shows a layer's motion, after a key or the playhead moved.
    void refreshLayerMotionUi();
    // The Dev Recorder's layerMotion event for a step just made on `layerIndex` of `panel`.
    void reportLayerMotion(const QString &what, const Panel *panel, int layerIndex);
    // The key of `layer` (of the panel on the canvas) on the playhead's frame, or -1.
    int layerKeyAtPlayhead(const Panel &panel, const Layer &layer) const;
    // Writes the key at the playhead in place (no undo step: the caller's).
    bool writeLayerKeyAtPlayhead(Panel *panel, int layerIndex, const shotcam::LayerPose &pose, const QPointF &pivot);
    // The opacity slider's value for the active layer: its keyed opacity at the playhead
    // when it animates.
    int activeLayerOpacityPercent(const Panel *panel) const;

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
    void layerKeyPasteChanged(); // layerKeyPasteReady may have changed: Edit > Paste follows
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
    // A layer's depth was changed: one "Layer Depth" step (the Dev
    // Recorder's layerDepth event): the layer, its depth before and after.
    void layerDepthEdited(const QVariantMap &event);
    // A layer's motion was changed (batch I-1): one step - its name, the layer, the panel, the
    // playhead's frame, Animate, and every key (the Dev Recorder's layerMotion event).
    void layerMotionEdited(const QVariantMap &event);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    // The View entries that act on the canvas are on only while the page shows (batch C).
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QWidget *createLeftColumn();
    QWidget *createCenterColumn();
    QWidget *createPanelControls(); // fixed, non-scrolling column at the left of the strip
    QWidget *createRightColumn();
    QWidget *createLayerPanel();
    void createFloatingToolbar();   // Brush bar + extras bar (FloatingToolWindows)
    // Floating overlay panel: dock-style header (title + Close only),
    // draggable by the header, child of the canvas.
    // persistPosition false: no saved spot - the caller places it (the Colors panel, batch D).
    QWidget *createFloatingPanel(const QString &title, QWidget *body);
    QWidget *createCameraPanel();   // floating panel shown while Camera is active
    QWidget *createStagePanel();    // the Stage bar, in its place during Stage mode
    void setStageLive(const QMargins &margins, int activeSide);
    void applyStageField(int side, int value);
    QRect neighbourOnStage(const Panel *here, const Panel *neighbour) const;
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
    void fitLayerRows();          // narrow a row's thumbnail only if it must, to fit the list
    void setActiveLayer(int index);
    // Photoshop-style selection: plain click = single, Shift = range from
    // the anchor, Ctrl = toggle. Selection indices live in m_layerSelection;
    // the ACTIVE layer (drawing target) is panel->activeLayerIndex.
    void layerRowClicked(int index, Qt::KeyboardModifiers modifiers);
    QVector<int> selectedLayers() const; // ascending, valid, non-background
    void layerAdd();              // blank raster layer above the active one
    int insertLayerAboveActive(Panel *panel); // Add Layer's insertion (no history)
    void pasteCanvasOnNewLayer(bool atOriginalPos); // Edit > Paste: a new layer, one step
    void layerAddImage();         // file dialog -> new image-type layer
    void layerDeleteSelected();   // never deletes Background / the last layer
    void layerDuplicateSelected();
    void layerMergeSelected();    // multi: into one; single: merge down
    void layerClearSelected();    // wipe pixels to transparent
    void layerGroupSelected();    // put the selection in a new folder ("Group N")
    void layerSetColorTag(int index);
    void layerBeginRename(int index); // inline QLineEdit over the name label
    // Drag-reorder: move the given ascending source indices so the block
    // starts at insertAt (a gap index in the PRE-move list); relative order
    // of the moved layers is preserved.
    void layerMoveTo(const QVector<int> &sources, int insertAt);
    void refreshLayerCanvas();    // repaint canvas + panel thumbnail after a layer change

    void layerContextMenu(int index, const QPoint &globalPos);
    // The Depth popover (Phase 2B, Figma 2B-2) under `globalAt`: live while
    // the slider moves, one "Layer Depth" undo step per release or Enter.
    void layerOpenDepth(int index, const QPoint &globalAt);
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
    void warmStripThumbs(const QVector<Panel *> &panels); // stale thumbs, in parallel
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
    void onDurationChangeRequested(int sceneIndex, int panelIndex, int frames);
    void onRollRequested(int sceneA, int panelA, int framesA, int sceneB, int panelB, int framesB);
    void applyShotInfoFrames(); // the Duration row's number, as a request
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
    // THE LAYER ROW'S HOLD ON THE EDIT KEYS and the copied layer key (batch I-1b round 2; see
    // copyLayerKey). The key: its flat panel, layer and key index (-1: the row, no key).
    struct LayerKeyFocus
    {
        bool row = false;
        int flat = -1, layer = -1, key = -1;
    };
    LayerKeyFocus m_layerKeyFocus;
    void setLayerKeyFocus(bool row, int flat = -1, int layer = -1, int key = -1);
    // The selected key, if it is still there.
    bool focusedLayerKey(Panel **panel, int *flat, int *layer, int *key) const;
    // The copied key: as it was (its time is not used), and where its move put the art's centre
    // (frame space: the pivot plus its move) - the place a paste keeps.
    struct LayerKeyClip
    {
        bool has = false;
        shotcam::LayerKey key;
        QPointF place;
    };
    LayerKeyClip m_layerKeyClip;

    // Native docking: an embedded (child-widget) QMainWindow hosts the stock
    // QDockWidget engine; the reusable DockController adds custom title
    // bars, previews, tab/split drops, collapse and persistence. The canvas
    // area is the host's central widget; Scenes, Layers, and Shot Info are
    // QDockWidgets around it.
    QMainWindow *m_dockHost = nullptr;
    DockController *m_dockController = nullptr;
    LayerFolder *m_layerFolder = nullptr; // the panels at rest (batch 6b-V)
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
    QColor m_stripGround{0x1a, 0x1a, 0x1a}; // the strip's background (STRIP-1)
    QColor stripCellEdge() const;          // an unselected cell's 1 px edge on that ground
    void applyPanelStripBackground();      // the ground, the cells, the letterbox
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
    QSize m_stageMargin = QSize(0, 0); // Phase 2A: px each side; none
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
    QPointer<LayerDepthPopover> m_depthPopover; // the open Depth popover (Phase 2B)
    QPointer<QWidget> m_easePopover; // the five eases on a move's mark (batch E), while open
    std::function<QColor(const QColor &)> m_sceneColorPickForTest; // Custom...'s picker, in the gate
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
    // The Colors panel (batch D): the window, its body, the swatch that anchors and toggles
    // it, its Eyedropper button, View > Colors.
    FloatingToolWindow *m_colorsPanel = nullptr;
    ColorPanel *m_colorPanelBody = nullptr;
    QAbstractButton *m_colorSwatch = nullptr;
    QAbstractButton *m_colorPickButton = nullptr;
    QAbstractButton *m_symmetryButton = nullptr; // the tools bar's Symmetry switch (batch H)
    QAction *m_colorsViewAction = nullptr;
    QScrollArea *m_colorsScroll = nullptr; // the body's frame: scrolls only when cut short
    // A click outside, a menu entry, a tool key, the other panel opening: closed, and
    // remembered as closed (2026-11-21).
    void closeColorsPanel();
    void closeBrushLibrary();
    // The click-away rule's (notePanelClick): the press being looked at - its time and place,
    // so its copies are looked at once - and the button it began on, if it did.
    void closePanelsClickedOutside(QWidget *at, const QSinglePointEvent *event);
    bool m_panelPressSeen = false;
    quint64 m_panelPressTime = 0;
    QPointF m_panelPressAt;
    QPointer<QWidget> m_panelPressButton;
    void anchorColorsPanel();   // under / over the swatch, as the Library under the Brush
    void fitColorsPanel();      // never over the tools bar: cut to the room on its side
    // Batch C: the toolbars' View entries and Tab's state; the View entries that act on
    // the canvas (on only while this page shows, so their keys stay free elsewhere -
    // the Script Editor's text); the selection-mode picker and the size step, made
    // where their buttons and slider are.
    QWidget *toolbarWindow(Toolbar bar) const;
    void applyToolbarVisibility();
    bool m_toolbarsTabHidden = false;
    QAction *m_toolbarActs[4] = {};
    QAction *m_hideAllToolbarsAct = nullptr;
    QAction *m_cameraArrowsAct = nullptr;
    QList<QAction *> m_canvasViewActs;
    std::function<void(DrawingCanvas::Tool)> m_pickSelectionMode;
    std::function<bool(int)> m_stepToolSize;
    bool brushStudioUnderCursor() const; // Ctrl+Z routing (studio-local undo)
    void undoDocument(); // Edit > Undo AND the toolbar's Undo button
    void redoDocument();
    void refreshBrushDirtyState();       // white dot + Reset chip on the row
    QScrollArea *m_panelScroll = nullptr;
    QPushButton *m_importButton = nullptr;
    QLabel *m_recoveryStatus = nullptr; // the recovery copy's word (2026-11-04)
    StatusNoteBar *m_noteBar = nullptr; // the note, between Import Image and it (batch I-1b round 1)
    QString m_recoveryStatusWords;      // what it says, without the dot's markup
    // Floating Size CTL toolbar (Figma 209:42): size + opacity sliders and
    // Flip, snapping to the left/right canvas edge. (Brush Options is gone;
    // its pressure toggles were removed deliberately, and hardness is
    // edited in the Studio — its per-tool persistence lives on under
    // toolCtl/brush/hardness. See HANDOFF.md.)
    QWidget *m_sizeCtlBar = nullptr;
    // The Camera bar (visible only while the Camera tool is active).
    QWidget *m_cameraPanel = nullptr;
    int m_cameraBarExtraW = 0;       // the fit check's seam
    QWidget *m_stagePanel = nullptr;
    std::function<void()> m_syncStageBar;
    bool m_stageModeOn = false;
    bool m_stageLinked = false;
    QMargins m_stageStart;           // the panel's margins as the session began
    QMargins m_stageLive;            // what is proposed
    int m_stageSide = -1;            // 0 left, 1 top, 2 right, 3 bottom: the outlined field
    qint64 m_stageRestBytes = 0;     // every other panel's layers, as they rest
    QMetaObject::Connection m_stageUndoWatch;
    QVector<QShortcut *> m_stageKeys;
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
    // PINNED KEYS (Phase 2E, 2026-11-01; the Phase 2 spec's 5, D16-D21).
    // A key pinned to the sound holds its film TIME, not its panel: the
    // Camera bar's Pin toggle (the key at the playhead), the key menu's
    // Pin to Sound / Unpin, M while the film plays (a pinned key at the
    // playing frame). A STRANDED key - pinned where its scene no longer
    // reaches - is kept and drawn, has its own menu, and is live again
    // when the scene covers its time. A structural panel edit names the
    // keys it strands or brings back in its undo text and a note (5.3).
    void cameraTogglePin();
    void cameraPinKey(int sceneIndex, int keyIndex, bool pin);
    void cameraMarkKey();
    bool cameraKeyStranded(int sceneIndex, int keyIndex) const;
    void onStrandedKeyMenu(int sceneIndex, int keyIndex, const QPoint &globalPos);
    void cameraUnpinStranded(int sceneIndex, int keyIndex);
    void cameraDeleteStranded(int sceneIndex, int keyIndex);
    int pinnedAmong(const Scene *scene, const QVector<int> &keys) const;
    static QString pinnedReplacedText(int pinned); // D20: " (replaces 1 pinned key)"
    QVector<QVector<int>> filmLayout() const;      // frames per panel, per scene
    int strandedIn(const QVector<QVector<int>> &layout, int sceneIndex) const;
    QString strandingSuffix(const QVector<QVector<int>> &after, QString *note) const;
    // What kind of insert (batch F1: a locked scene's note says which).
    enum class InsertKind { Add, Duplicate, Paste };
    // False when a locked scene refused it (the panel is then deleted, and a note says why).
    bool pushInsertPanel(Scene *scene, int index, Panel *panel, const QString &text,
                         InsertKind kind = InsertKind::Add);
    void pushMovePanel(Scene *scene, int from, int to);
    // A locked scene's keys once its panels are `after` (uid, frames at the project's
    // rate), each kept on its frame (batch F1, the user's decision 4).
    shotcam::SceneCamera lockedSceneCamera(const Scene *scene, const QVector<shotcam::PanelSpan> &after) const;
    // A locked scene's typed duration (batch F1, decision 6): it takes from the next
    // panel (the previous one for the last), and stops at what that one can give.
    void lockedDurationChange(int sceneIndex, int panelIndex, int frames);
    int m_cameraBarNaturalW = 0; // the bar's own width (the fit check's seam adds to it)
    StageClear *m_stageClear = nullptr; // the bars fade while the film plays
    static const QString kClearStageKey;
    // THE COMPACT BAR AND THE STRIP STEPPING ASIDE (2026-11-03, Figma PB-1
    // / PB-3): while the stage is playing the timeline section is its 36 px
    // compact bar and the panel strip an 8 px handle; on pause both return
    // exactly - the section to the height and state it had (open or
    // collapsed), the strip to its size. The compact bar's chevron opens
    // the timeline without pausing (suppressed until the film stops).
    void applyCompact(bool on);
    void stripStepAside(bool on);
    bool m_compactActive = false;
    bool m_compactSuppressed = false;
    bool m_stripSteppedAside = false;
    int m_stripMinH = 0, m_stripMaxH = 0;
    int m_stripSavedDockH = 0; // the dock's height before stepping aside, given back exactly
    QVector<QPointer<QWidget>> m_stripHidden;
    bool cameraKeyAtPlayhead() const; // the row menu's "Key Set at Playhead"
    bool deleteSelectedCameraKey(); // false: no key selected (Delete is the panel's)
    QString cameraKeyHeader(int sceneIndex, int keyIndex, bool *last) const;
    void onCameraKeyMenu(int sceneIndex, int keyIndex, const QPoint &globalPos);
    void onCameraRowMenu(int flatPanel, const QPoint &globalPos);
    // Batch E (Figma E-1): a tap on a move's ease mark - the five eases there, in a popover.
    void onCameraEaseMark(int sceneIndex, int keyIndex, const QRect &globalMarkRect);
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
        QString note; // Phase 2C (Q7): the START gave way - by how much, and why
    };
    // *span*: the popover's Over; the CAMERA row's and the clip's menus act
    // on the panel they name, so they pass ThisPanel (the 1D hand test).
    bool quickMovePlan(shotcam::QuickMove move, int sceneIndex, int panelIndex, QuickSpan span,
                       QuickPlan *plan, QString *why) const;
    void applyQuickPlan(const QuickPlan &plan);
    QString quickGiveWayNote(shotcam::QuickMove move, const shotcam::QuickFrames &f,
                             const shotcam::Stage &stage) const;
    QString quickSpanText(int sceneIndex, int panelIndex) const; // the popover's title
    QString quickLeavesText(const QuickPlan &plan) const;       // batch 6d: the panels it would leave
    void addQuickMoveEntries(QMenu *menu, const QString &title, int flatPanel);
    void addSceneCameraEntries(QMenu *menu, int sceneIndex);
    // Batch E: "Grow all N panels to fit this move" and "Start the move at Panel N (...)"
    // after a Grow entry, in the chip's, the CAMERA row's and the clip's menus alike; greyed
    // with why on a locked SHOTS (Grow all) or CAMERA (Start) track.
    void addMoveFitEntries(QMenu *menu, int sceneIndex, int panelIndex);
    QMargins grownMargins(const Panel *panel, const QMargins &grow) const; // capped as Grow is
    void onSceneMenu(int sceneIndex, const QPoint &globalPos);
    shotcam::CameraState lastFramingOf(int sceneIndex, bool *whole) const;
    shotcam::CameraState heldOnFirstPanel(int sceneIndex, const shotcam::CameraState &framing,
                                          QString *note) const; // batch 6d
    int sceneFilmStart(int sceneIndex) const; // frames before the scene
    int filmStartOf(const Scene *scene) const; // the same, for a scene pointer (0 if not ours)
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
    // Batch I-1b (note 3): the Move bar shows with the Move tool, but not on an animated
    // layer with Animate on (refreshMoveBar).
    bool m_moveToolOn = false;
    void refreshMoveBar();
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
        // THE FILM'S OWN KEYS (the user's answer after hand test I1b-3, 2026-11-26): Space and
        // Esc. While the film is up - playing, or paused in the preview - they are armed
        // wherever the pointer is in the window (the preview, the timeline, the bottom bar, a
        // panel, the pen lifted away), except while a text field is being typed in.
        bool filmKey = false;
    };
    QVector<TimelineKey> m_timelineKeys;
    bool typingInTextField() const;          // a text or number field has the keyboard
    bool m_pointerOverTimeline = false;
    bool m_pointerOverPreview = false;
    // The pointer has left the window (batch I-1b round 2, the user's answer after hand test
    // I1b-1): a pen lifted out of reach is one - Qt sends Leave up to the window. While the
    // preview is up it counts as over the preview, so Esc and Space still stop the film.
    bool m_pointerLeftWindow = false;
    bool m_pointerOverCanvas = false;        // the canvas's keys are the canvas's (routeCanvasKey)
    bool m_spaceDownAway = false;            // a Space pressed off the canvas, still down
    qint64 m_testPointerDevice = -1;         // the gate's pen (setPointerDeviceForTest)
    void pointerArrivedOverCanvas();         // ...is the canvas's once the pointer comes
    bool m_timelineKeysArmed = false;
    bool m_filmKeysArmed = false;            // Space and Esc are the film's (updateTimelineKeys)
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
    // A note that is not a refusal, said in the status bar in grey (batch I-1b: one place for
    // every note - the status bar since round 1; it was a tooltip at the pointer).
    void sayNote(const QString &message);
    // A refusal or a warning - something did not happen, or happened otherwise than asked:
    // the same place, in amber (batch I-1b round 1).
    void sayWarning(const QString &message);
    // A press on a locked or hidden layer (batch G, answer 3): why, in the status bar in amber
    // for kRefusalNoteMs (batch I-1b; it was by the pointer, a window of its own).
    void sayEditRefused(const QString &why, const QPoint &globalPos);
    static constexpr int kRefusalNoteMs = 2500;
    // Remove the selected panel with this history text ("Delete Panel",
    // "Cut Panel"); when the panel held camera keys the text names them
    // and a note says the scene's camera move changes (the user's rule,
    // 2026-10-25: a deletion must not change a move silently).
    void removeSelectedPanel(const QString &baseText);
    // The same in a locked scene (batch F1, decision 2): its time goes to the panel
    // before it (the next, if it was the first), and its keys stay on their frames.
    void removeSelectedPanelLocked(const QString &baseText);
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
    QLineEdit *m_shotFrames = nullptr;  // Duration, in frames (2026-11-06)
    QLabel *m_shotFramesEquals = nullptr;
    QLabel *m_actionLabel = nullptr;

    bool m_loadingShotInfo = false;
};
