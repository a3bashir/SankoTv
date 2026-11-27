#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <functional>
#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include "AudioPeaks.h"
#include "TimelineTracks.h"
#include "camera/CameraModel.h"

class AnimaticPage;
class QAbstractButton;
class TimelineCanvas; // inner painted surface, defined in the .cpp
class QContextMenuEvent;
class QIcon;
class QPainter;
class QPushButton;
class QScrollBar;
class QTimer;
class QLabel;
class QLineEdit;
class QSlider;
class QToolButton;
struct Layer;
struct LayerPixels;
struct Panel;
struct Scene;

// Professional NLE-style timeline, shown under the drawing canvas in the
// storyboard workspace. A zoom toolbar sits above a multi-track canvas
// (timecode ruler, scene track, panel/shot clips, audio) with a fixed left
// label column and a horizontally scrollable, zoomable canvas. Its height is
// the user's: the shots track takes whatever the other tracks leave.
//
// It is a second VIEW of the storyboard's own panels, never a second list of
// them: blocks hold scene/panel indices into the workspace's scenes, and
// every change to what is selected, added, removed or moved is REQUESTED from
// the workspace (the signals below) and arrives back as state (the slots).
class AnimaticTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit AnimaticTimeline(QWidget *parent = nullptr);

    // Test hooks: the painted surface (to send it real mouse events), where
    // a clip is on it, and what the view currently holds.
    QWidget *surfaceForTest() const;
    QRect clipRectForTest(int flatIndex) const { return clipRect(flatIndex); }
    // Batch F1: a scene's block, the lock before its name (drawn or not), its end line's
    // x (-1: not drawn), and the hover text of the block and of the lock.
    QRect sceneRectForTest(int sceneIndex) const { return sceneRectOnScreen(sceneIndex); }
    // SCENES + SHOTS (Figma 621:716): the block as drawn, inside its span (above: the span).
    QRect sceneBlockRectForTest(int sceneIndex) const { return sceneBlockRect(sceneIndex); }
    QRect sceneLockRectForTest(int sceneIndex) const { return sceneLockGlyphRect(sceneIndex); }
    int sceneEndLineXForTest(int sceneIndex) const;
    QString sceneTipForTest(int sceneIndex, bool overLock) const
    {
        return overLock ? sceneLockTip(sceneIndex) : sceneEndTip(sceneIndex);
    }
    int blockCountForTest() const { return int(m_blocks.size()); }
    // In whole SECONDS, as every check written before 2026-11-06 reads it:
    // -1 when the clip is not a whole number of seconds (or not there).
    // Frames, the length itself: the next.
    int blockDurationForTest(int flatIndex) const
    {
        if (flatIndex < 0 || flatIndex >= m_blocks.size())
            return -1;
        const int f = m_blocks.at(flatIndex).frames, rate = qMax(1, m_fps);
        return f % rate == 0 ? f / rate : -1;
    }
    int blockFramesForTest(int flatIndex) const
    {
        return flatIndex >= 0 && flatIndex < m_blocks.size() ? m_blocks.at(flatIndex).frames : -1;
    }
    int blockStartForTest(int flatIndex) const
    {
        return flatIndex >= 0 && flatIndex < m_blocks.size() ? m_blocks.at(flatIndex).startFrame : -1;
    }
    // The clip's duration pill as drawn: its text and where it is (empty
    // when the clip is too narrow to show one), and the chip a drag shows.
    QString pillTextForTest(int flatIndex) const;
    QRect pillRectForTest(int flatIndex) const;
    QString dragChipForTest() const { return m_dragChip; }
    // THE TYPING FIELD (2026-11-06, Figma DUR-1): opened by a double-click
    // on a clip's duration pill or the clip menu's "Set Duration..."; the
    // panel it is for (flat index; -1 = closed), the widget itself, and
    // the "= 1s 12f at 24 fps" beside the number.
    void openDurationField(int flatIndex);
    int durationFieldPanelForTest() const;
    QWidget *durationFieldForTest() const;
    QString durationFieldEqualsForTest() const;
    int selectedPanelForTest() const { return m_selected; }
    int playheadPanelForTest() const { return m_current; }
    // A clip, the playhead or a camera key under the pointer right now: the
    // recovery copy (2026-11-04) waits for the drag to end.
    bool dragInProgress() const { return m_drag != Drag::None || m_camDragActive; }
    int shotsTrackHeightForTest() const { return panelTrackH(); }
    // What the audio track is showing: "none", "loading", "present",
    // "missing" or "unplayable"; its text (the hint, "name | m:ss", or the
    // missing / cannot-be-played label);
    // and where the AUDIO row is.
    QString audioBarStateForTest() const;
    QString audioBarTextForTest() const;
    QRect audioRowForTest() const;
    // THE WAVEFORM (Phase 2D): what the row shows of the sound - "none",
    // "reading", "ready" or "plain" (the decoder could not read it); whether
    // the name and length chips are hidden (the pointer over the row); and
    // the columns as drawn, {x, top, bottom} each, screen px.
    QString audioWaveformStateForTest() const;
    bool audioChipsHiddenForTest() const;
    QVariantList waveformColumnsForTest() const;
    // The view's following of the playhead (see followTick): where the
    // view is, how wide, where the playhead is drawn, the follow point,
    // whether the follow timer runs; a switch to turn following off (the
    // gate's positive control); and a scroll made "by the user", as any
    // scrollbar change that following did not make counts.
    int scrollXForTest() const { return m_scrollX; }
    int visibleWidthForTest() const { return visibleWidth(); }
    int playheadScreenXForTest() const;
    int followPointForTest() const;
    bool followTimerActiveForTest() const;
    bool viewHeldForTest() const { return m_viewHeld; }
    // The wheel over the canvas: its switch (the gate's positive control),
    // the zoom as a number, and where a screen x is in content space.
    void setCanvasWheelForTest(bool on) { m_canvasWheel = on; }
    double zoomForTest() const { return m_zoom; }
    double contentXAtForTest(int screenX) const { return screenXToContent(screenX); }
    double pxPerFrameForTest() const { return pxPerFrame(); }
    void setFollowForTest(bool follow) { m_follow = follow; }
    void userScrollForTest(int scrollX);
    // For the Dev Recorder's statePoll (2026-10-16): the view's zoom,
    // scroll offset and visible width, the playhead's frame, and the
    // following state - what a recording of "the playhead left the view"
    // needed and did not have.
    QVariantMap devrecState() const;

    // The timeline reads the live elapsed time / current index from the page
    // when rendering the playhead (see updatePlayhead()).
    void setHost(AnimaticPage *host);

    // Project frame rate (24 unless the project says otherwise). Set before
    // loadScenes/setBlocks — per-block frame counts derive from it.
    void setFps(int fps);
    int fps() const { return m_fps; }
    // The stage margin (Phase 2A): px each side; the clips show the frame.
    void setStageMargin(const QSize &margin);
    // Test hook: the DERIVED total (sum of per-block duration*fps), so a
    // test can prove frame counts re-derive after setFps rather than just
    // reading the stored rate back.
    int totalFramesForTest() const { return totalFrames(); }

    // THE REDESIGN (Figma 380:644, 2026-10-18): the zoom row is gone. Its
    // controls still belong to the timeline - the same buttons, the same
    // slider, the same handlers - but they live in two containers the
    // header row above takes: Fit, -, the slider and + on the left, and the
    // Frames / Seconds / Snap icons on the right. The page hides both with
    // the body when the section is collapsed (the design shows no collapsed
    // state; the user's decision).
    QWidget *zoomControls() const { return m_zoomControls; }
    QWidget *viewControls() const { return m_viewControls; }
    // THE ZOOM FIELD (Part D, 2026-10-23; the user's design: [ - ] [ 100% ]
    // [ + ]): the current zoom in whole percent; double-click to edit, Enter
    // applies, Escape cancels, a click elsewhere applies; "150" or "150%",
    // clamped to the zoom's 10..1600 (400 until batch C); invalid text restores the value.
    QLineEdit *zoomField() const { return m_zoomField; }
    // THE ZOOM FROM THE BUTTONS AND THE KEYS (batch C). - and + step along a ladder of
    // sqrt(2) whose rungs are 100 times a power of two every second step (..., 50, 71, 100,
    // 141, 200, 283, 400, 566, 800, 1131, 1600; 10 below 12.5); Ctrl+= and Ctrl+- over the
    // timeline do the same, Ctrl+0 is Fit, Ctrl+1 is 100 % (1 px a frame).
    void stepZoom(int direction);
    static float zoomStepFrom(float zoom, int direction);
    void fitZoomToView() { fitZoom(); }
    void zoomToActual() { applyZoom(100.0f); }
    void setZoomForTest(float zoom) { applyZoom(zoom); }
    QString zoomFieldTextForTest() const;
    bool zoomEditingForTest() const { return m_zoomEditing; }
    void beginZoomEditForTest() { beginZoomEdit(); }
    // THE VOLUME POPUP (Part D): the Audio track's speaker opens it.
    QSlider *volumePopupForTest() const { return m_volumePopup; }
    void setVolumeTooltip(const QString &tip);
    // Test hooks for the look: the ruler's label step in seconds at the
    // current zoom, and where the parts of a clip are drawn (body,
    // thumbnail, number, name, pill - with which of them are shown), as
    // "x,y,wxh" strings in canvas coordinates.
    int rulerLabelStepForTest() const { return rulerLabelStep(); }
    // Where a film frame is on screen, and the ruler's bottom edge (its ticks hang
    // from it): the gate reads the frame ticks of batch C there.
    int frameScreenXForTest(int frame) const { return contentXToScreen(frame * pxPerFrame()); }
    int rulerBottomForTest() const;
    QVariantMap clipPartsForTest(int flatIndex) const;
    // The camera a SHOTS clip's thumbnail is drawn with (Phase 1A): the
    // shot camera at the panel's first frame.
    shotcam::CameraState clipCameraForTest(int flatIndex) const;
    static int labelColumnWidthForTest();
    // The tracks' lock / eye / speaker buttons over the label column
    // ("trackLockScenes", "trackEyeShots", "trackMuteAudio", ...), for the
    // gate to press and read; the host's tracks() is what they show.
    QToolButton *trackControlForTest(const QString &objectName) const;
    // What a press on the painted canvas was refused for, last ("SHOTS
    // track is locked"), empty when nothing was refused since the reset.
    QString lastRefusalForTest() const { return m_lastRefusal; }
    void clearRefusalForTest() { m_lastRefusal.clear(); }
    // Where a refusal is said (batch I-1b): the workspace's note, in the status bar since round
    // 1, as a refusal (amber); no sink, a tooltip at the pointer as before.
    void setNoteSink(std::function<void(const QString &, bool warning)> sink) { m_noteSink = std::move(sink); }

    // The host's track state, pushed in whenever it changes (a toggle or a
    // load): the buttons show it, hidden tracks paint empty and take no
    // press, a locked SHOTS track takes no trim or reorder.
    void setTracks(const TimelineTracks &tracks);

    // THE CAMERA ROW (Phase 1C, 2026-10-28; Figma proposals 1C-1 to 1C-5).
    // A 28 px row between SHOTS and AUDIO: a diamond on each live camera
    // key at its frame, a 2 px bar between consecutive keys of a scene with
    // the move's ease in a small pill (Hold dashed), never across a cut
    // between scenes. A press on a key selects it and asks for the playhead
    // to go there (cameraKeyActivated); a drag moves it in time, one
    // request on release (cameraKeyMoveRequested); right-click asks for
    // the key's menu or, off a key, the row's. The edits themselves are
    // the workspace's (StoryboardPage), so they are its undo commands and
    // its lock refusals. Locked or hidden, the row is drawn at 40 %; a
    // locked row selects but does not drag, a hidden one takes no press.
    // The label column holds lock, ANIMATE (a record dot: the canvas Camera
    // bar's switch, the same state) and eye.
    void setCameraAnimate(bool on); // what the Animate button shows
    bool cameraAnimateShown() const { return m_cameraAnimate; }
    // A press at `globalPos` delivered to `receiver` lands on the LAYER row (batch I-1b round 2:
    // any other press hands the edit keys back).
    bool pressOnLayerRow(const QObject *receiver, const QPoint &globalPos) const;
    // ...or on the timecode row, the ruler and its label: it moves the playhead, and the LAYER
    // row keeps the edit keys through that (the user's answer after hand test I1b-3) ...
    bool pressOnTimecodeRow(const QObject *receiver, const QPoint &globalPos) const;
    // ...or is the copy of a press on the rows that Qt hands on to the rows' parents (a pen's
    // press the rows did not take, before the mouse press Qt makes of it): not a press anywhere.
    bool pressPassedOnFromRows(const QObject *receiver, const QPoint &globalPos) const;
    // The selected key: false when none (or it has gone).
    bool selectedCameraKey(int *sceneIndex, int *keyIndex) const;
    void setSelectedCameraKey(int sceneIndex, int keyIndex); // -1, -1: none
    // The ease glyph (the row's pill and the key menu's Ease entries).
    static QIcon easeGlyphIcon(int ease);
    // The glyph's curve alone, in `color` and `width`, fitted to `box` as the glyph's 18 x 12
    // (batch E, Figma E-1: the five eases' buttons in the popover and the key's menu).
    static void paintEaseCurve(QPainter &p, const QRectF &box, int ease, const QColor &color, qreal width);
    // A QUICK MOVE under the pointer (Phase 1D, Figma 1D-2): the two keys it
    // would add, hollow and dashed, joined by a dashed bar, and the keys it
    // would replace crossed out. Film frames; nothing is changed.
    void setCameraPreview(int sceneIndex, double startFrame, double endFrame,
                          const QVector<int> &replacedKeys);
    void clearCameraPreview();
    // ("shown", "scene", "x0", "x1", "replaced" as "s:k" strings)
    QVariantMap cameraPreviewForTest() const;
    // Test hooks: the row; each key as the row draws it ("scene", "key",
    // "x" on the canvas, "frame" in the film, "selected", "atPlayhead",
    // "hover"); each segment ("scene", "from", "to", "ease", "dashed",
    // "glyph", "accent", "x0", "x1"); the row's dimming.
    QRect cameraRowForTest() const;
    // THE PART OF A MOVE THAT LEAVES A CLIP'S ART (batch 6d, Figma MARGIN-2): the
    // clip's frames past its stage, in amber on the CAMERA row, and its chip.
    int artExitFramesForTest(int flatIndex) const;
    QMargins artExitGrowForTest(int flatIndex) const;
    QRect artExitChipRectForTest(int flatIndex) const;
    QString artExitChipTextForTest(int flatIndex) const;
    // GROW-1: what the chip shows at this zoom - the sentence, the count ("72 f") or
    // the marker ("!"); "over" when the marker is centred over a clip too narrow for it.
    QString artExitChipLabelForTest(int flatIndex) const;
    QVariantList cameraKeysForTest() const;
    QVariantList cameraSegmentsForTest() const;
    double cameraRowOpacityForTest() const;

    // THE LAYER ROW (batch I-1, 2026-11-24; Figma I-1 693:755, option A, approved): a 28 px
    // row under CAMERA, there while any layer of the project has motion kept (no project
    // without one changes). It shows THE SELECTED LAYER's keys - the active layer of the
    // panel on the canvas, named in the label column ("LAYER · Boat") - as the CAMERA row
    // shows the camera's: a diamond on each key at its frame, a bar between consecutive keys
    // with the move's ease mark (Hold dashed); and, dim, a small mark on every key of every
    // other animated layer, so the panels with motion are seen. Animate off: the layer's
    // keys greyed, kept. A key pressed: the playhead goes there (and, a dim one, its panel
    // and layer are chosen); dragged: moved in its panel, on release; its ease mark: the
    // five eases; right-click: its menu. The edits are the workspace's, as the camera's.
    void refreshLayerRow();
    bool layerRowShown() const { return m_layerRowShown; }
    QRect layerRowForTest() const;
    QString layerRowLabelForTest() const { return layerRowLabel(); }
    // Each key as drawn: "flat", "layer", "key", "x", "frame" (film), "own" (the selected
    // layer's; else dim), "greyed" (Animate off), "atPlayhead"; each bar: "flat", "layer",
    // "from", "to", "ease", "dashed", "glyph", "small", "lineOnly", "markRect", "x0", "x1".
    QVariantList layerKeysForTest() const;
    QVariantList layerSegmentsForTest() const;

    // THE DRAWINGS LANE (batch I-2, 2026-11-26; Figma I-2 727:757, approved: "two lanes in a
    // 46 px row, keys above, drawings below"). While the selected layer has a run of drawings
    // the LAYER row is 46 px: the keys' lane above, exactly as built, and under it the
    // drawings as BLOCKS, each as long as it is shown - the last to the panel's end - in the
    // layer keys' colour, quietly (a tint, the cuts a little stronger, the block under the
    // playhead the strong one; with the onion skin on, the blocks it ghosts in its two
    // colours). The pointer's lane decides what a press takes: in the drawings' lane a block
    // (the playhead to its first frame; it is then the selected thing on the row, for Del),
    // or a cut between two blocks - dragged, the later drawings move with it; with Alt, the
    // cut alone - and a right-click asks for the lane's menu. The + at the row's left is New
    // Drawing. The edits are the workspace's (StoryboardPage), one request on release. The
    // SHOTS track gives the row its 18 px (never under 30 px itself).
    void setSelectedDrawing(int drawingIndex); // the block drawn as selected; -1 none
    void setOnionSkinShown(bool on);
    QRect drawingsLaneForTest() const;
    // Each block as drawn: "index", "start", "end" (frames of the panel; end exclusive),
    // "x0", "x1", "shown" (under the playhead), "selected", "tint" ("#rrggbb" its colour),
    // "strength" (its fill's opacity), "label".
    QVariantList drawingBlocksForTest() const;
    QAbstractButton *newDrawingButtonForTest() const;
    // SINCE HAND TEST I2-1 (2026-11-26, the user's notes 5 and 7). The lane is 41 px (the row
    // 71), its bands 3 px, to the user's Figma "Proposal drawing size" (737:872); a block with
    // nothing drawn on it is grey ("blank") until something is; the block under the pointer is
    // a little lighter ("hovered"). ZOOM TO FRAMES - the small button beside the +, and a
    // double-click on the lane - zooms so the lane's shortest drawing is 32 px wide (never
    // past 1600 %, 16 px a frame; never out), with the playhead in the middle of the view.
    // And every row inside its bounds: the canvas is never shorter than its rows (rowsMinH),
    // and a SHOTS clip's thumbnail keeps its picture's shape inside the clip.
    void zoomToFrames();
    QAbstractButton *zoomToFramesButtonForTest() const;
    int rowsMinimumForTest() const { return rowsMinH(); }
    // Each row as laid out, "y,h": "timecode", "scenes", "shots", "camera", "layer", "audio";
    // and "canvas", the painted surface's height.
    QVariantMap rowsForTest() const;

public slots:
    void setScenes(const QVector<Scene *> &scenes);
    void setCurrentPanel(int flatIndex);  // the panel under the PLAYHEAD
    void setSelectedPanel(int flatIndex); // the panel on the drawing canvas
    void setPlaying(bool playing);
    void setLoopRegion(int startIndex, int endIndex);
    void setAudioLoaded(bool loaded, qint64 audioDurationMs);
    void setAudioMissing(bool missing); // the project names a file that is gone
    void setAudioUnplayable(bool unplayable); // ...or one that cannot be played
    void updatePlayhead();
    void refreshThumbnails(); // a panel's artwork changed: repaint the clips

signals:
    // The user pressed a clip: select that panel and put the playhead on it.
    void panelSeekRequested(int flatPanelIndex);
    // The user is dragging the playhead: show that panel, select nothing.
    void playheadScrubbed(int flatPanelIndex);
    // ...and the frame inside it, whenever that changes (Phase 1A: the
    // scrub is frame-accurate; the camera is seen frame by frame).
    void playheadScrubbedTo(int flatPanelIndex, int frameInPanel);
    // The playhead's drag began (the press) and ended (the release) - batch I-1b: while
    // it lasts the workspace leaves the Move tool's box for its end.
    void playheadDragStarted();
    void playheadDragEnded();
    // A panel's new length in FRAMES (2026-11-06; it was whole seconds):
    // the right edge, the left edge (a ripple: the clip keeps its start and
    // later clips move up) or the typing field.
    void durationChanged(int sceneIndex, int panelIndex, int newFrames);
    // Alt-drag on the cut between two clips: a ROLL - both change, nothing
    // after them moves.
    void rollRequested(int sceneA, int panelA, int framesA, int sceneB, int panelB, int framesB);
    // A clip was dragged to a new place in its scene. `flatGap` is the block
    // it should land in front of (one past the scene's last block = the end).
    void panelMoveRequested(int flatIndex, int flatGap);
    // Right-click on a clip (already requested as the selection).
    void clipContextMenuRequested(int flatPanelIndex, const QPoint &globalPos);
    // Right-click anywhere on the AUDIO row.
    void audioContextMenuRequested(const QPoint &globalPos);
    // THE CAMERA ROW (see setCameraAnimate). A key pressed (left or right):
    // select its panel and put the playhead on it. A key dragged to another
    // frame of its scene (counted from the scene's first frame). Right-click
    // on a key, and on the row off any key. The Animate button clicked.
    void cameraKeyActivated(int sceneIndex, int keyIndex);
    void cameraKeyMoveRequested(int sceneIndex, int keyIndex, int sceneFrame);
    void cameraKeyContextMenuRequested(int sceneIndex, int keyIndex,
                                       const QPoint &globalPos);
    void cameraRowContextMenuRequested(int flatPanelIndex, const QPoint &globalPos);
    // Batch E (Figma E-1): a tap on a move's ease mark - or, under 16 px, on its line - asks
    // for the five eases of that move (the key it starts from), by the mark's place on screen.
    void cameraEaseMarkClicked(int sceneIndex, int keyIndex, const QRect &globalMarkRect);
    // Batch 6d: the amber chip of a clip whose art the camera leaves, pressed.
    void artExitChipClicked(int flatPanelIndex, const QPoint &globalPos);
    void cameraAnimateToggled(bool on);
    // THE LAYER ROW (batch I-1; see refreshLayerRow).
    void layerKeyActivated(int flatPanelIndex, int layerIndex, int keyIndex);
    void layerKeyMoveRequested(int flatPanelIndex, int layerIndex, int keyIndex, int frameInPanel);
    void layerKeyContextMenuRequested(int flatPanelIndex, int layerIndex, int keyIndex, const QPoint &globalPos);
    void layerEaseMarkClicked(int flatPanelIndex, int layerIndex, int keyIndex, const QRect &globalMarkRect);
    // THE LAYER ROW TAKES THE EDIT KEYS (batch I-1b round 2, the user's note 5): any press on the
    // row - on a key (its indices), or on its empty track or an ease mark (keyIndex -1) - makes it
    // the last thing pressed, so Ctrl+C, X and V are its; a right-click off a key asks for the
    // row's own menu (Paste Key at the Playhead).
    void layerRowPressed(int flatPanelIndex, int layerIndex, int keyIndex);
    void layerRowContextMenuRequested(const QPoint &globalPos);
    // THE DRAWINGS LANE (batch I-2; see setSelectedDrawing): the + pressed; a block pressed;
    // a cut dragged to a frame of the panel (`alone`: Alt was down), on release; a right-click
    // on a block (its index) - each of the selected layer of the panel on the canvas.
    void newDrawingRequested();
    // The rows need another least height (a LAYER row came or went, or its drawings lane):
    // whoever sizes the timeline's section gives it the room (2026-11-26).
    void rowsMinimumChanged();
    void drawingBlockPressed(int layerIndex, int drawingIndex);
    void drawingCutMoveRequested(int layerIndex, int drawingIndex, int startFrameInPanel, bool alone);
    void drawingsLaneContextMenuRequested(int layerIndex, int drawingIndex, const QPoint &globalPos);
    // Right-click on the SCENES track (Phase 1D): that scene's menu.
    void sceneContextMenuRequested(int sceneIndex, const QPoint &globalPos);
    // A press on a scene's lock on the SCENES row (batch F1): lock or unlock its length.
    void sceneLengthLockToggleRequested(int sceneIndex);
    void zoomChanged(float zoomLevel); // internal use

private:
    friend class TimelineCanvas;

    struct Block
    {
        int flatIndex = 0;
        int sceneIndex = 0;
        int panelIndex = 0;
        int sceneNumber = 0;
        QString sceneName;
        int startFrame = 0; // cumulative frames before this clip
        int frames = 24;    // its length at the timeline's rate (shotcam::panelFrames)
        bool sceneStart = false;
    };
    // The duration pill's text for a clip: "1s 12f" in the Seconds view,
    // "36f" in the Frames view (Q5).
    QString pillText(const Block &b) const;
    // The flat index of the clip drawn under a screen x: a clip narrower
    // than its drawn minimum (kMinClipPx) wins over the clip it overlaps.
    int blockAtScreenX(int screenX) const;
    // The typing field's parts (a child of the window, over the timeline).
    void closeDurationField(bool apply);
    void positionDurationField();

    // Canvas hooks (called by TimelineCanvas).
    void renderCanvas(QPainter &p);
    void canvasMousePress(QMouseEvent *e);
    void canvasMouseMove(QMouseEvent *e);
    void canvasMouseRelease(QMouseEvent *e);
    void canvasMouseDoubleClick(QMouseEvent *e); // on a clip's pill: the typing field
    void canvasLeave();
    void canvasResized();
    void canvasContextMenu(QContextMenuEvent *e);
    // THE WHEEL OVER THE TRACKS, RULER AND LABEL COLUMN (2026-10-17, the
    // user's decision after three recordings in which the wheel went
    // there and did nothing): plain and Shift+wheel scroll sideways by
    // kWheelNotchPx a notch, routed through the scrollbar as the user's
    // own scroll (so it counts for the hold rule); Ctrl+wheel zooms around
    // the pointer, keeps the zoom slider in step, and never holds. When
    // the film fits, nothing moves.
    void canvasWheel(QWheelEvent *e);

    // Track geometry: the shots track is whatever the canvas height leaves
    // after the ruler, the scene track and the audio track.
    int tracksH() const;
    int rowsMinH() const;       // the least the rows need (2026-11-26)
    void syncMinimumHeight();   // the canvas's minimum follows them
    int panelTrackH() const;
    int cameraTrackY() const;
    int audioTrackY() const;

    // The CAMERA row's keys as drawn: one per live key, in film frames,
    // sorted by scene then frame. While a key is dragged it is drawn (and
    // its segments are) at the frame the drag has reached.
    struct CameraMark
    {
        int sceneIndex = 0;
        int keyIndex = 0;
        double frame = 0.0; // in the film
        int x = 0;          // on the canvas
        int ease = 0;       // shotcam::Ease
        bool pinned = false;   // Phase 2E: pinned to the sound (the blue diamond)
        bool stranded = false; // ...and outside its scene (hollow grey; strandedMarks)
    };
    QVector<CameraMark> cameraMarks() const;
    // The STRANDED pinned keys (Phase 2E, D17): drawn hollow at their time,
    // never a segment, never numbered among the live keys, never dragged.
    QVector<CameraMark> strandedMarks() const;
    // The bars between consecutive keys of a scene, as drawn: what the
    // painter draws and the gate reads, from one place.
    struct CameraSegment
    {
        int sceneIndex = 0;
        int fromKey = 0, toKey = 0; // key indices; the ease is fromKey's
        int ease = 0;
        qreal x0 = 0, x1 = 0;       // the bar, between the diamonds
        qreal markX = 0;            // the ease mark's centre (2026-11-26: clear of every key)
        bool dashed = false;        // Hold
        bool glyph = false;         // long enough for the ease pill (40 px or more)
        bool smallMark = false;     // 16 to 40 px: the small mark (batch E, Figma E-1; not
                                    // `small`: windows.h makes that a macro)
        bool lineOnly = false;      // under 16 px: no mark - a tap on the line itself
        bool accent = false;        // starts at the selected key
    };
    QVector<CameraSegment> cameraSegments(const QVector<CameraMark> &marks) const;
    // The ease mark under a press (batch E): the pill, the small mark, or a short move's
    // line; `markRect` gets what was pressed, in canvas coordinates.
    bool easeMarkAt(const QPoint &pos, CameraSegment *segment, QRectF *markRect) const;
    QRectF easeMarkRect(const CameraSegment &s) const;
    bool cameraKeyAt(const QPoint &pos, CameraMark *mark) const;
    bool inCameraRow(const QPoint &pos) const;
    int sceneStartFrame(int sceneIndex) const; // its first block's start
    int sceneEndFrame(int sceneIndex) const;   // its last block's end
    // THE SCENE LENGTH LOCK (batch F1, Figma F-1). Whether a scene's length is locked;
    // its block on screen; the scene under a screen x on the SCENES row (-1: none); the
    // lock before its name, 8 x 9 px, the lock and the name centred together; the
    // hover texts; and the lock itself, painted.
    bool sceneLocked(int sceneIndex) const;
    QRect sceneRectOnScreen(int sceneIndex) const;
    int sceneAtScreenX(int x) const;
    QFont sceneNameFont() const;
    QString sceneNameOf(int sceneIndex) const;
    QRect sceneLockGlyphRect(int sceneIndex) const;
    QString sceneEndTip(int sceneIndex) const;
    QString sceneLockTip(int sceneIndex) const;
    static void paintSceneLock(QPainter &p, const QRect &glyph, const QColor &colour);
    QString cameraKeyTip(const CameraMark &mark) const;
    // What Qt's tooltip says over a point of the canvas (batch C): a camera key's, an
    // amber chip's, the SCENES row's; empty elsewhere. And the amber chip's own.
    QString hoverTipAt(const QPoint &pos) const;
    QString artExitChipHoverText(int flatIndex) const;
    void paintCameraRow(QPainter &p);
    void paintPinLines(QPainter &p); // the pinned keys' dotted lines through the AUDIO row
    // THE LAYER ROW (batch I-1).
    int layerTrackY() const;
    int layerRowH() const { return m_layerRowShown ? (m_layerRowRun ? kLayerRowRunH : kLayerRowH) : 0; }
    static constexpr int kLayerRowH = 28;
    // THE DRAWINGS LANE (batch I-2): the row's height with it, the blocks' top and height
    // inside the row, where the keys' lane ends for the pointer, and the SHOTS track's floor
    // while the row is that tall.
    // (THE LANE'S SIZE since hand test I2-1, 2026-11-26 - the user's Figma "Proposal drawing
    // size", node 737:872: 41 px high, so the row is 71; it was 16 and 46. Its bands 3 px - the
    // proposal's 2.56 on whole pixels - where they were 1.)
    static constexpr int kLayerRowRunH = 71;
    static constexpr int kDrawingsLaneY = 25;
    static constexpr int kDrawingsLaneH = 41;
    static constexpr int kDrawingsBandH = 3;
    static constexpr int kKeysLaneH = 24;
    static constexpr int kPanelRunMinH = 30;
    struct DrawingBlock
    {
        int index = 0;
        int start = 0, end = 0; // frames of the panel, the project's rate; end exclusive
        qreal x0 = 0, x1 = 0;
        bool shown = false, selected = false, last = false;
        bool blank = false;   // nothing drawn on it yet: grey until something is
        bool hovered = false; // the pointer is over it
        QColor tint;
        qreal strength = 0.22;
    };
    // The selected layer of the panel on the canvas, when it has a run: its place and block.
    const Layer *runLayer(int *layerIndex = nullptr, int *flat = nullptr) const;
    // The run's starts in the project's frames, with a dragged cut where the drag has it.
    QVector<int> runStarts(const Layer &layer) const;
    QVector<DrawingBlock> drawingBlocks() const;
    QRect drawingsLaneRect() const; // on the canvas; empty without a run
    bool inDrawingsLane(const QPoint &pos) const;
    int drawingBlockAt(const QPoint &pos) const; // -1: none
    int drawingCutAt(const QPoint &pos) const;   // the drawing that STARTS at the cut under the pointer; -1
    QString drawingBlockLabel(const DrawingBlock &block) const;
    void paintDrawingsLane(QPainter &p);
    void layoutNewDrawingButton();
    // NOTHING DRAWN ON IT (2026-11-26, the user's note 7): asked of the drawing's pixels once
    // for each state of them and kept by their identity (the timeline repaints often).
    bool drawingBlank(const LayerPixels &px) const;
    mutable QHash<qint64, bool> m_blankByKey;
    int m_hoverDrawing = -1; // the block under the pointer, -1: none
    QToolButton *m_zoomFramesButton = nullptr;
    bool m_layerRowRun = false;
    bool m_onionShown = false;
    int m_selectedDrawing = -1;
    int m_cutDragDrawing = -1, m_cutDragLayer = -1;
    int m_cutDragFrom = 0, m_cutDragFrame = 0; // frames of the panel
    bool m_cutDragAlone = false, m_cutDragActive = false;
    QToolButton *m_newDrawingButton = nullptr;
    struct LayerMark
    {
        int flat = 0, layer = 0, key = 0;
        double frame = 0.0; // in the film
        int x = 0;
        int ease = 0;
        bool own = false;    // the selected layer's
        bool greyed = false; // its Animate is off
    };
    struct LayerSegment
    {
        int flat = 0, layer = 0, fromKey = 0, toKey = 0, ease = 0;
        qreal x0 = 0, x1 = 0;
        qreal markX = 0; // the ease mark's centre: the move's, or beside a key that is in the way
        bool dashed = false, glyph = false, smallMark = false, lineOnly = false, greyed = false;
    };
    QVector<LayerMark> layerMarks() const;
    QVector<LayerSegment> layerSegments(const QVector<LayerMark> &marks) const;
    QRectF layerEaseMarkRect(const LayerSegment &s) const;
    bool inLayerRow(const QPoint &pos) const;
    bool layerKeyAt(const QPoint &pos, LayerMark *mark) const;
    bool layerEaseMarkAt(const QPoint &pos, LayerSegment *segment, QRectF *markRect) const;
    // A drawn ease mark (the pill or the small mark) is under the point: a press there opens
    // the eases and takes no key (2026-11-26). The LAYER row's, and the CAMERA row's.
    bool onLayerEaseMark(const QPoint &pos) const;
    bool onCameraEaseMark(const QPoint &pos) const;
    QString layerRowLabel() const;
    void paintLayerRow(QPainter &p);
    bool computeLayerRowShown() const;
    bool m_layerRowShown = false;
    int m_layerDragFlat = -1, m_layerDragLayer = -1, m_layerDragKey = -1;
    int m_layerDragFrame = -1, m_layerDragFromFrame = -1; // film frames
    bool m_layerDragActive = false;
    QPoint m_layerPress;
    QRect clipRect(int flatIndex) const; // on the canvas; empty if no such clip
    int moveGapAt(int screenX) const;

    // The audio track's one description, shared by the painter and the
    // gate. Loading = a track is set and its length is not known yet.
    struct AudioBar
    {
        enum State { None, Loading, Present, Missing, Unplayable };
        State state = None;
        QString name;   // the file's name
        QString length; // "m:ss", Present only
        QString hint;   // None only
        QString label;  // Missing and Unplayable: what the bar says
        QRect rect;     // Present and Missing, on the canvas
    };
    AudioBar audioBar() const;
    // The waveform's columns (Phase 2D): for every screen column of the
    // visible bar, the lowest and highest peak in the column's time span by
    // the ruler's own x mapping - one function, drawn by paint and read by
    // the checks (wf2). Empty unless the peaks are in.
    struct WaveColumn
    {
        int x = 0;
        double top = 0.0;
        double bottom = 0.0;
    };
    QVector<WaveColumn> waveformColumns(const AudioBar &bar) const;
    AudioPeaks::State audioWaveformState() const;
    bool m_audioHover = false; // the pointer is over the AUDIO row: the chips hide
    // Batch F1: the scene under the pointer on the SCENES row (its lock shows, faint, on
    // an unlocked one), whether it is over that lock, and whether a length drag is a
    // locked scene's roll (its chip says so).
    int m_sceneHover = -1;
    bool m_sceneLockHover = false;
    bool m_lockedRoll = false;

    // Geometry / model helpers.
    void rebuildBlocks();
    int totalFrames() const;
    int totalSeconds() const;
    double pxPerFrame() const;
    double pxPerSecond() const;
    double contentWidthPx() const;
    int visibleWidth() const;        // canvas width minus the label column
    int contentXToScreen(double contentX) const;
    double screenXToContent(int screenX) const;
    int frameAtScreenX(int screenX) const;
    int blockAtFrame(int frame) const;
    double playheadContentX() const; // px, in content space
    int playheadFrame() const;
    void updateScrollRange();
    // THE VIEW FOLLOWS THE PLAYHEAD (2026-10-16; HANDOFF "The timeline
    // follows the playhead"). While the film plays, followTick runs at
    // 60 Hz: it reads the panel's elapsed time live from the host ONCE,
    // keeps it for the paint that follows (so scroll and playhead come
    // from one value and the playhead holds still at the follow point),
    // and, once the playhead has reached the follow point two-thirds across
    // the view, scrolls so it stays there; the scroll stops at the end of
    // the content and the playhead runs on. A scroll the user made is
    // never undone by following (m_viewHeld). A jump - a change of panel
    // that is not playback stepping to the next one, or Play - moves the
    // view only if the playhead would be off-screen, and then puts it a
    // third of the way across.
    void followTick();
    void setScrollX(int scrollX);       // by following: not a user scroll
    // "MY SCROLL" IS ONLY WHAT THE USER DOES TO THE SCROLLBAR (the user's
    // rule, 2026-10-17): a drag of its handle, the wheel over it, its arrow
    // buttons, a click on its track. The scrollbar reports those through
    // actionTriggered - but it reports a programmatic setSliderPosition the
    // same way (SliderMove), and a range change (zoom out) clamps the value
    // with valueChanged alone. So: valueChanged is never read as the
    // user's; an action is the user's when it is a step or page action, or
    // a SliderMove while the handle is pressed (a drag) or while a wheel
    // event is being delivered to the bar (the event filter). The first
    // version read valueChanged, and the clamp of a zoom-out held the view
    // - the Dev Recorder showed it.
    void onScrollbarAction(int action);
    void onUserScroll(int scrollX);     // the user's scroll, to this offset
    bool eventFilter(QObject *watched, QEvent *event) override;
    int followPoint() const;            // screen x, two-thirds across
    void jumpIfOffScreen();             // the jump rule
    bool playheadInView(int *screenX = nullptr) const;
    void applyZoom(float z);
    void fitZoom();
    int snapFrame(int frame) const;
    QString timecode(int frame) const;
    // The ruler's label step: the smallest of 1, 2, 3, 5, 10, 15, 30, 60 s
    // whose pitch is at least 120 px (the user's rule, 2026-10-18).
    int rulerLabelStep() const;
    // Where a clip's parts go for a body of this rect (the design's layout
    // and its hide order as the clip narrows: the name first, then the
    // thumbnail, then the pill; the number stays).
    // SCENES + SHOTS (2026-11-18, Figma 621:716): a scene's clips touch, sharing one line
    // at each cut; the scene's outer corners are round; between two scenes a 3 px gap is
    // taken from inside their spans - the cut's own column and one each side of it - so no
    // clip leaves its frames. first / last: the clip opens / closes its scene; startInset /
    // endInset: the px its body gives up there; radius: the scene's outer corners.
    struct ClipEdges
    {
        bool first = true, last = true;
        int startInset = 0, endInset = 0;
        qreal radius = 0;
    };
    ClipEdges clipEdges(int flatIndex) const;
    struct ClipLayout
    {
        QRect body, thumb, number, name, pill;
        bool thumbShown = false, nameShown = false, pillShown = false;
        ClipEdges edges; // the body's place in its scene (above)
    };
    // The layout of clip flatIndex drawn in rect r (its frame span, or kMinClipPx wide);
    // its pill sized to its text: the design's 28 px, wider when its text ("1s 12f",
    // "120f") needs it (2026-11-06).
    ClipLayout clipLayout(int flatIndex, const QRect &r) const;
    // The shape its thumbnail keeps (2026-11-26): the panel's frame.
    QSizeF clipPictureSize(int flatIndex) const;
    // A scene's block on the SCENES row as drawn: its span less its share of the gaps.
    QRect sceneBlockRect(int sceneIndex) const;

    // Toolbar handlers.
    void styleToolbarButtons();

    // THE TRACK CONTROLS (Part B): real buttons parented to the canvas, at
    // the design's positions over the label column (the lock at x 8, the
    // speaker at 32 on AUDIO, the eye at 56; 16 px icons). Built once,
    // placed again whenever the rows move (the shots track takes the spare
    // height), restyled from m_tracks (checked = locked / hidden / muted,
    // the icon in the accent).
    void buildTrackControls();
    void layoutTrackControls();
    void styleTrackControls();
    void refuse(const QString &why, const QPoint &globalPos);
    // The zoom field and the volume popup (Part D).
    void updateZoomField();
    void beginZoomEdit();
    void endZoomEdit(bool apply);
    void toggleVolumePopup();
    void layoutVolumePopup();

    // Data + state.
    QVector<Scene *> m_scenes;
    QVector<Block> m_blocks;
    int m_fps = 24; // project frame rate; every frame count derives from it
    QSize m_stageMargin = QSize(0, 0); // Phase 2A: px each side; none
    // Batch 6d: each scene's clips' art exits, kept while what shapes them is the
    // same (its camera, its panels' lengths, stages and art, the rate).
    struct ArtExits
    {
        QByteArray shape;
        QVector<int> frames, first, last; // per panel of the scene
        QVector<QMargins> grow;
    };
    mutable QHash<const Scene *, ArtExits> m_artExits;
    mutable bool m_artExitsTrusted = false; // true only inside a paint or a hit test, after its check
    int artExitChipAt(const QPoint &pos) const; // the clip whose chip is there, or -1
    const ArtExits *artExitsOf(int sceneIndex) const;
    bool artExitOf(int flatIndex, int *frames, int *first, int *last, QMargins *grow) const;
    QString artExitChipText(int flatIndex) const;
    // Empty only when the clip leaves no art (or SHOTS is hidden): at any zoom the chip
    // steps down rather than going (GROW-1). `label` gets what it shows; `over` is
    // set when the marker is centred over a clip too narrow to hold it.
    QRect artExitChipRect(int flatIndex, QString *label = nullptr, bool *over = nullptr) const;
    int m_chipHover = -1; // the clip whose chip the pointer is over (its tooltip shown), or -1
    int m_current = -1;  // playhead panel (flat index)
    int m_selected = -1; // selected panel (flat index): accent + trim handles
    bool m_playing = false;
    int m_loopStart = -1;
    int m_loopEnd = -1;
    bool m_audioLoaded = false;
    bool m_audioMissing = false;
    bool m_audioUnplayable = false;
    qint64 m_audioDurationMs = 0;
    AnimaticPage *m_host = nullptr;
    TimelineTracks m_tracks;
    // SCENES, SHOTS, AUDIO, CAMERA.
    QToolButton *m_lockButtons[4] = {nullptr, nullptr, nullptr, nullptr};
    QToolButton *m_eyeButtons[4] = {nullptr, nullptr, nullptr, nullptr};
    QToolButton *m_muteButton = nullptr;
    QToolButton *m_animateButton = nullptr; // the CAMERA row's record dot
    bool m_cameraAnimate = false;
    QString m_lastRefusal;
    std::function<void(const QString &, bool)> m_noteSink; // setNoteSink (batch I-1b)
    // The CAMERA row's selection, hover and drag (scene, key indices).
    int m_camSelScene = -1, m_camSelKey = -1;
    int m_camHoverScene = -1, m_camHoverKey = -1;
    int m_easeHoverScene = -1, m_easeHoverKey = -1; // the ease mark under the pointer (batch E)
    int m_camDragScene = -1, m_camDragKey = -1;
    int m_camDragFrame = -1;     // film frame the drag has reached
    int m_camDragFromFrame = -1; // ...and where the key was
    bool m_camDragActive = false;
    QPoint m_camPress;
    // The Quick Move preview (setCameraPreview).
    bool m_camPreview = false;
    int m_camPreviewScene = -1;
    double m_camPreviewStart = 0, m_camPreviewEnd = 0;
    QVector<int> m_camPreviewReplaced;

    float m_zoom = 100.0f; // 100 = 1px per frame
    int m_scrollX = 0;     // content scroll offset in px
    // Following (see followTick).
    QTimer *m_followTimer = nullptr;
    bool m_follow = true;        // off only for the gate's positive control
    bool m_wheelOnScrollbar = false; // a wheel event is being delivered to the bar
    bool m_canvasWheel = true;       // off only for the gate's positive control
    double m_wheelRemainder = 0;     // a high-resolution wheel's part-notches
    bool m_following = false;     // the strip is scrolling under the playhead:
                                  // a geometry change (zoom, width, panels)
                                  // keeps it on the follow point
    bool m_viewHeld = false;      // the user scrolled with the playhead right
                                  // of the follow point: the view is theirs
                                  // until the next Play, scrub, seek or loop
    int m_liveElapsedMs = -1;     // the host's elapsed time read this tick
                                  // (-1: read the host's plain value)
    bool m_framesMode = false; // false = timecode, true = frame numbers
    bool m_snap = true;

    // Interaction.
    int m_hoverIndex = -1;
    enum class Drag { None, ResizeRight, ResizeLeft, Roll, Playhead, Move, CameraKey, LayerKey, Pan, DrawingCut };
    // PANNING (batch I-1b, the user's note 8): Ctrl + a left drag (mouse or pen) or a middle
    // drag, anywhere on the rows or the ruler, moves the view left and right with the
    // pointer - through the scrollbar, as the user's scroll - and nothing else.
    double m_panStartGlobalX = 0.0;
    int m_panStartScroll = 0;
    bool m_panScrolling = false; // a pan is moving the scrollbar: the user's scroll
    static bool panPress(const QMouseEvent *e);
    Drag m_drag = Drag::None;
    int m_scrubIndex = -1;     // last panel announced during a playhead drag
    int m_scrubFrameSent = -1; // last frame announced (playheadScrubbedTo)
    void emitScrubFrame();
    int m_moveFrom = -1;       // clip pressed (flat index)
    int m_moveGap = -1;        // gap it would drop into (flat index)
    bool m_moveActive = false; // the press has travelled far enough to drag
    QPoint m_movePress;
    // A LENGTH DRAG (2026-11-06, batch 5): the clip (and for a roll the
    // clip after it), the lengths it had, the lengths the drag has reached
    // (drawn: rebuildBlocks reads m_dragFramesFor), where the pointer was
    // pressed (content x) and where the moving edge started (a frame, drawn
    // dashed), and the chip at the pointer.
    int m_resizeIndex = -1;
    int m_rollNext = -1;
    int m_dragFromFrames = 0, m_dragFromFramesNext = 0;
    QHash<int, int> m_dragFramesFor;
    double m_dragPressX = 0.0;
    int m_dragOriginFrame = -1;
    QString m_dragChip;
    void showDragChip(const QPoint &globalPos);
    // A CHIP THAT FOLLOWS THE POINTER through a drag - the playhead's timecode, a
    // clip's length, a camera key's time (see followChip): one tooltip, kept.
    void followChip(const QPoint &globalPos, const QString &text);
    void hideChip();
    QElapsedTimer m_chipRoll;   // since the chip's tip was last asked for anew
    QString m_chipWaitingText;  // the newest text and place, asked for while it appeared
    QPoint m_chipWaitingPos;
    QTimer *m_chipFlush = nullptr;
    int m_dragPlayheadFrame = -1;
    // The typing field (see openDurationField).
    QWidget *m_durField = nullptr;
    QLineEdit *m_durEdit = nullptr;
    QLabel *m_durTitle = nullptr;
    QLabel *m_durEquals = nullptr;
    int m_durFlat = -1;

    // Widgets.
    TimelineCanvas *m_canvas = nullptr;
    QScrollBar *m_hScroll = nullptr;
    QWidget *m_zoomControls = nullptr; // Fit, -, field, +: shown in the header
    QWidget *m_viewControls = nullptr; // Frames, Seconds, Snap: likewise
    QLineEdit *m_zoomField = nullptr;  // the percentage, editable on double-click
    bool m_zoomEditing = false;
    QString m_zoomTextBeforeEdit;
    QSlider *m_volumePopup = nullptr;  // vertical, a child of the canvas, hidden until the speaker opens it
    bool m_syncingVolume = false;      // setValue from the host, not the user
    QPushButton *m_fitButton = nullptr;
    QPushButton *m_zoomInButton = nullptr;
    QPushButton *m_zoomOutButton = nullptr;
    QPushButton *m_framesButton = nullptr;
    QPushButton *m_secondsButton = nullptr;
    QPushButton *m_snapButton = nullptr;
};
