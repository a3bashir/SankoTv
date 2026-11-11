#pragma once

#include <QElapsedTimer>
#include <QHash>
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
class TimelineCanvas; // inner painted surface, defined in the .cpp
class QContextMenuEvent;
class QIcon;
class QPushButton;
class QScrollBar;
class QTimer;
class QLabel;
class QLineEdit;
class QSlider;
class QToolButton;
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
    // clamped to the zoom's 10..400; invalid text restores the value.
    QLineEdit *zoomField() const { return m_zoomField; }
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
    // The selected key: false when none (or it has gone).
    bool selectedCameraKey(int *sceneIndex, int *keyIndex) const;
    void setSelectedCameraKey(int sceneIndex, int keyIndex); // -1, -1: none
    // The ease glyph (the row's pill and the key menu's Ease entries).
    static QIcon easeGlyphIcon(int ease);
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
    // Batch 6d: the amber chip of a clip whose art the camera leaves, pressed.
    void artExitChipClicked(int flatPanelIndex, const QPoint &globalPos);
    void cameraAnimateToggled(bool on);
    // Right-click on the SCENES track (Phase 1D): that scene's menu.
    void sceneContextMenuRequested(int sceneIndex, const QPoint &globalPos);
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
        bool dashed = false;        // Hold
        bool glyph = false;         // long enough for the ease pill
        bool accent = false;        // starts at the selected key
    };
    QVector<CameraSegment> cameraSegments(const QVector<CameraMark> &marks) const;
    bool cameraKeyAt(const QPoint &pos, CameraMark *mark) const;
    bool inCameraRow(const QPoint &pos) const;
    int sceneStartFrame(int sceneIndex) const; // its first block's start
    int sceneEndFrame(int sceneIndex) const;   // its last block's end
    QString cameraKeyTip(const CameraMark &mark) const;
    void paintCameraRow(QPainter &p);
    void paintPinLines(QPainter &p); // the pinned keys' dotted lines through the AUDIO row
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
    struct ClipLayout
    {
        QRect body, thumb, number, name, pill;
        bool thumbShown = false, nameShown = false, pillShown = false;
    };
    // pillText sizes the pill: the design's 28 px, wider when its text
    // ("1s 12f", "120f") needs it (2026-11-06).
    ClipLayout clipLayout(const QRect &r, const QString &pillText = QString()) const;

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
    // The CAMERA row's selection, hover and drag (scene, key indices).
    int m_camSelScene = -1, m_camSelKey = -1;
    int m_camHoverScene = -1, m_camHoverKey = -1;
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
    enum class Drag { None, ResizeRight, ResizeLeft, Roll, Playhead, Move, CameraKey };
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
