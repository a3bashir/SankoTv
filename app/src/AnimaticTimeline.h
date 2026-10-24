#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

#include "TimelineTracks.h"

class AnimaticPage;
class TimelineCanvas; // inner painted surface, defined in the .cpp
class QContextMenuEvent;
class QPushButton;
class QScrollBar;
class QTimer;
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
    int blockDurationForTest(int flatIndex) const
    {
        return flatIndex >= 0 && flatIndex < m_blocks.size()
            ? m_blocks.at(flatIndex).duration
            : -1;
    }
    int selectedPanelForTest() const { return m_selected; }
    int playheadPanelForTest() const { return m_current; }
    int shotsTrackHeightForTest() const { return panelTrackH(); }
    // What the audio track is showing: "none", "loading", "present",
    // "missing" or "unplayable"; its text (the hint, "name | m:ss", or the
    // missing / cannot-be-played label);
    // and where the AUDIO row is.
    QString audioBarStateForTest() const;
    QString audioBarTextForTest() const;
    QRect audioRowForTest() const;
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
    void durationChanged(int sceneIndex, int panelIndex, int newDurationSeconds);
    // A clip was dragged to a new place in its scene. `flatGap` is the block
    // it should land in front of (one past the scene's last block = the end).
    void panelMoveRequested(int flatIndex, int flatGap);
    // Right-click on a clip (already requested as the selection).
    void clipContextMenuRequested(int flatPanelIndex, const QPoint &globalPos);
    // Right-click anywhere on the AUDIO row.
    void audioContextMenuRequested(const QPoint &globalPos);
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
        int duration = 1;   // seconds
        int startFrame = 0; // cumulative frames before this clip
        int frames = 24;    // duration * fps
        bool sceneStart = false;
    };

    // Canvas hooks (called by TimelineCanvas).
    void renderCanvas(QPainter &p);
    void canvasMousePress(QMouseEvent *e);
    void canvasMouseMove(QMouseEvent *e);
    void canvasMouseRelease(QMouseEvent *e);
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
    int audioTrackY() const;
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
    ClipLayout clipLayout(const QRect &r) const;

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
    QToolButton *m_lockButtons[3] = {nullptr, nullptr, nullptr}; // SCENES, SHOTS, AUDIO
    QToolButton *m_eyeButtons[3] = {nullptr, nullptr, nullptr};
    QToolButton *m_muteButton = nullptr;
    QString m_lastRefusal;

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
    enum class Drag { None, ResizeRight, ResizeLeft, Playhead, Move };
    Drag m_drag = Drag::None;
    int m_scrubIndex = -1;     // last panel announced during a playhead drag
    int m_moveFrom = -1;       // clip pressed (flat index)
    int m_moveGap = -1;        // gap it would drop into (flat index)
    bool m_moveActive = false; // the press has travelled far enough to drag
    QPoint m_movePress;
    int m_resizeIndex = -1;
    int m_dragDuration = 0;
    int m_dragLeftFrames = -1;  // visual-only left trim preview
    int m_dragPlayheadFrame = -1;

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
