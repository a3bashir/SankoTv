#pragma once

#include <QAudioDevice>
#include <QElapsedTimer>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include "TimelineTracks.h"
#include "camera/CameraModel.h"

#include <QTransform>
#include <QVariantMap>

#include <functional>

class AnimaticTimeline;
class QAction;
class QMenu;
class QToolButton;
class PanelDisplay;
class QAudioOutput;
class QHBoxLayout;
class QLabel;
class QMediaDevices;
class QMediaPlayer;
class QPushButton;
class QSlider;
class QSpacerItem;
class QTimer;

struct Panel;
struct Scene;

// THE ANIMATIC, as a section of the storyboard workspace (the name is from
// when it was the fourth pipeline screen; there is no such screen any more).
// It sits under the drawing canvas: one header row - fold, transport,
// timecode, loop, speed, volume - that stays when the section is collapsed,
// and the timeline below it. It plays the storyboard's panels in sequence
// with their durations, in a preview laid over the canvas.
//
// It reads the workspace's own Scene/Panel objects (no copies) and changes
// none of them: selecting, adding, removing, reordering and re-timing a
// panel are all REQUESTED from the workspace through the signals below,
// which owns the selection and the undo history, and come back as state
// through setSelectedPanel / refreshStructure / refreshTiming.
class AnimaticPage : public QWidget
{
    Q_OBJECT

public:
    explicit AnimaticPage(QWidget *parent = nullptr);
    ~AnimaticPage() override;

    static constexpr int kHeaderHeight = 40;

    // Adopt these scenes (and select nothing: the workspace selects next).
    // loadScenes({}) is how every Panel pointer held here is dropped before
    // the scenes are destroyed.
    void loadScenes(const QVector<Scene *> &scenes);
    // Same scenes, panels added / removed / reordered.
    void refreshStructure();
    // A panel's duration changed.
    void refreshTiming();
    // A panel's artwork changed (clip thumbnails re-read the shared mip).
    void refreshThumbnails();
    // The workspace's selection, as a flat index over all scenes (-1: none).
    // Moves the playhead there. Emits nothing.
    void setSelectedPanel(int flatIndex);

    // The preview is laid over this widget (the drawing canvas) while
    // playing or scrubbing; it never changes what the widget itself shows.
    void attachPreview(QWidget *canvasArea);
    bool previewVisible() const;
    void leavePreview(); // also stops playback
    QWidget *previewSurface() const; // the widget laid over the canvas

    // Collapsed = the header row only.
    bool isCollapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed);
    int minimumExpandedHeight() const;
    int defaultExpandedHeight() const;

    // THE TRACKS' LOCK / VISIBILITY / MUTE STATE (TimelineTracks.h). The
    // three setters are the user's toggles: they change the state, apply
    // it (the timeline's buttons and painting, the player's mute) and say
    // tracksChanged, which marks the project unsaved - they are not undo
    // commands. setTracks is the silent form for a project load.
    const TimelineTracks &tracks() const { return m_tracks; }
    void setTracks(const TimelineTracks &tracks);
    void setTrackLocked(TimelineTrack track, bool locked);
    void setTrackVisible(TimelineTrack track, bool visible);
    void setAudioVolume(int percent); // 0..100 on a perceptual scale; 0 mutes (Part D)
    // THE HEADER ROW (the redesign, Figma 380:644): collapse, Fit / - /
    // slider / +, a transport pill centred on the window, the timecode, a
    // separator, the total, the speed buttons, the audio label and volume,
    // the loop buttons, Frames / Seconds / Snap. What gives way when the
    // window is narrow: the audio file name shortens first, then the zoom
    // slider narrows to 80 px; below that the header's minimum width holds
    // the window, so nothing overlaps and no button is cut. Test hooks:
    // that minimum, the pill's place, the slider's width, the label's text
    // as drawn.
    int headerMinimumWidthForTest() const;
    // BEFORE SELLING 13 (2026-10-31): the floor at the design's full spacing
    // (the header is compact below it) and whether it is compact now.
    int headerFullFloorForTest() const { return m_headerFullFloor; }
    bool headerCompactForTest() const { return m_headerCompact; }
    int headerRightMarginForTest() const;
    // The loop hint is a note at the pointer now (not a header label).
    QString lastNoteForTest() const { return m_lastNote; }
    void clearNoteForTest() { m_lastNote.clear(); }
    void setNotesAtPointerForTest(bool shown) { m_notesAtPointer = shown; }
    QRect transportPillRectForTest() const;
    bool audioMutedForTest() const; // what the player itself reports
    int audioVolumeForTest() const { return m_tracks.audioVolume; }
    double audioGainForExport() const; // the volume as the export's linear gain
    // The header's audio warning (Part D): its message while shown, empty
    // when the audio is fine.
    QString audioWarningForTest() const;
    bool audioWarningShownForTest() const;

    // Transport, for the workspace's pointer-routed keys.
    void togglePlay();
    void goFirst();
    void goLast();
    void goPrev();
    void goNext();
    bool isPlaying() const { return m_playing; }

    // The scratch audio track (used by project save/load). audioPath() is
    // WHERE THE TRACK'S FILE IS - absolute, empty for no track - and is the
    // DOCUMENT's: kept, and saved, whether or not the file can be found
    // right now. audioMissing() is what the disk says about it.
    //
    // A track is one of two kinds (media policy, HANDOFF 2026-10-04):
    // - the project's OWN: a copy in its "<name>_assets" folder, which the
    //   manifest names relative to itself, so it travels with the folder;
    // - LINKED (audioLinked()): an old project's absolute path to a file
    //   outside the project. It plays, it is saved back exactly as it was,
    //   and it is the one that loses its sound when the folder moves -
    //   which is why the track says "linked" where it can be seen.
    QString audioPath() const;
    bool audioLinked() const { return m_audioLinked; }
    void setAudioPath(const QString &path, bool linked = false); // silent: a load is not an edit
    bool audioMissing() const { return m_audioMissing; }
    // The file is THERE and the player cannot play it (it could not be
    // opened as audio). Like missing, a display state: the track is kept
    // and saved, shown in the warning colour with the reason, and the film
    // plays without sound. audioProblem() is why, in the player's words.
    bool audioUnplayable() const { return m_audioUnplayable; }
    QString audioProblem() const { return m_audioProblem; }
    // WHERE THE SOUND GOES. Qt's QAudioOutput takes the system's default
    // device once, when it is made, and keeps it: started before the
    // speaker was connected, the app played into a device nobody could hear,
    // with no error anywhere (measured 2026-10-05). So the output FOLLOWS
    // the system default - when Windows reports a device change, and again
    // at every Play. noAudioOutput() = Windows has nothing to play through,
    // which the track says; audioOutputName() names the device in use (the
    // volume slider's tooltip shows it).
    bool noAudioOutput() const { return m_noAudioOutput; }
    QString audioOutputName() const;
    // The project's file, empty while it has none: what decides where an
    // imported file is copied to, and where the Locate dialog starts when
    // the stored path's own folder is gone.
    void setProjectFile(const QString &path) { m_projectFile = path; }
    // After a save: the project's own copy of the track is HERE now (a Save
    // As made a new one). Not an edit; nothing happens if it already is.
    void relocateAudio(const QString &path);

    // The audio track's edits, for the Edit menu and the track's own
    // right-click menu. All only REQUEST (audioChangeRequested): the
    // workspace makes the change an undoable command and calls
    // applyAudioPath when it runs, is undone, or is redone. Import, Locate
    // and Copy Into Project COPY THE FILE INTO THE PROJECT FIRST
    // (ProjectMedia::adopt) and request the copy; a copy that fails or is
    // cancelled requests nothing.
    void importAudio(); // asks for a file
    void removeAudio();
    void locateAudio(); // a missing track: point it at its file again
    void copyAudioIntoProject(); // a linked track becomes the project's own
    void applyAudioPath(const QString &path, bool linked); // pauses playback first

    // Project frame rate (New Project dialog / project file): forwarded to
    // the timeline, which derives every frame count from it.
    void setFps(int fps);

    // Read by AnimaticTimeline when rendering the real-time playhead.
    int currentFlatIndex() const { return m_current; }
    int elapsedMsInCurrentPanel() const { return m_elapsedMsInCurrentPanel; }
    // The same, read LIVE from the panel's wall clock while the film runs -
    // for the timeline's own 60 Hz follow (2026-10-16), which would
    // otherwise repaint the 20 Hz value three times over. Const, reads
    // only; while paused or waiting for the sound it is the frozen value.
    int liveElapsedMsInCurrentPanel() const;

    // THE SHOT CAMERA AT THE PLAYHEAD (Phase 1A, 2026-10-25): the film's own
    // clock (the value above, live while playing - the clock that already
    // starts from the sound's position and follows the speed), quantised
    // to the frame it falls in exactly as the timeline's playhead and the
    // MP4 count it. The preview draws with this; nothing else is asked.
    shotcam::CameraState cameraAtPlayhead() const;
    // For the Dev Recorder's statePoll: "shotCam." keys (the recorder's own
    // "camera" events are the canvas VIEW).
    QVariantMap shotCamDevrecState() const;

    // THE PLAYHEAD, FOR THE CANVAS'S CAMERA TOOL (Phase 1B). Which panel it
    // is in (null: none), the frame inside that panel (the same rounding as
    // cameraAtPlayhead), the project's rate, and the header's timecode.
    // setPlayheadFrameInPanel puts it on a frame of a panel WITHOUT the
    // preview (the Camera bar's previous / next key: the artist keeps
    // looking at the canvas). refreshCamera: a key changed - the preview
    // and the SHOTS clips redraw. playheadMoved is said whenever the
    // playhead's panel or frame changes while the film is not running, and
    // once when it stops.
    Panel *playheadPanel() const;
    int playheadFrameInPanel() const;
    int projectFps() const;
    QString playheadTimecode() const;
    void setPlayheadFrameInPanel(int flatIndex, int frameInPanel);
    void refreshCamera();
    // What the CAMERA row's Animate shows (the workspace's state, Phase 1C).
    void setCameraAnimate(bool on);
    // The CAMERA row's selected key (scene and key indices; false: none).
    bool selectedCameraKey(int *sceneIndex, int *keyIndex) const;
    void setSelectedCameraKey(int sceneIndex, int keyIndex);
    // The Quick Move preview on the CAMERA row (Phase 1D; film frames).
    void setCameraPreview(int sceneIndex, double startFrame, double endFrame,
                          const QVector<int> &replacedKeys);
    void clearCameraPreview();
    // The loop region's panels (flat indices); false when none is set.
    bool loopRegion(int *startFlat, int *endFlat) const;
    // THE CAMERA TOOL'S SCRUB (2026-10-26): on, a ruler click or drag
    // selects the panel under the playhead and stays on the canvas at the
    // exact frame instead of opening the preview. The workspace turns it on
    // with the Camera tool and off with every other; Play is unchanged.
    void setScrubShowsCanvas(bool on);
    bool scrubShowsCanvas() const { return m_scrubShowsCanvas; }

    // THE PICTURE KEEPS STEP WITH THE SOUND (sync pass, HANDOFF 2026-10-16).
    // Four rules, each the fix for a measured fault:
    // - a seek asked of the player before its file has loaded is dropped
    //   by Qt, so seeks are REMEMBERED and applied when it loads, and Play
    //   checks the position once more before it starts (seekAudio);
    // - Play RESUMES a panel from where it was paused - the panel's clock
    //   is a wall clock that freezes on pause, not a dwell restarted from
    //   zero - so the picture continues from where the sound does;
    // - the 0.5x / 2x speeds are given to the player too (setPlaybackRate;
    //   the pitch changes with them);
    // - THE FILM'S CLOCK WAITS FOR THE SOUND TO BEGIN: Play starts the
    //   player and the panel timers start when its position first moves
    //   from where it was asked to start, with the picture's clock set from
    //   the player's own position at that moment. A cap (one second) starts
    //   the picture anyway.

signals:
    // The track changed, by any route (an edit, an undo, a project load):
    // the menus enable Remove Audio from this. NOT a statement about the
    // document - nothing here says "unsaved" any more; an audio edit is a
    // command on the undo stack, and the stack is what marks the project
    // dirty. (This class used to emit documentChanged for it.)
    void audioStateChanged();
    // --- requests to the workspace (user input only; never from a slot) ---
    // newPath empty = remove the track. commandText names it in the history.
    void audioChangeRequested(const QString &newPath, bool linked,
                              const QString &commandText);
    void panelSelectRequested(int flatIndex);
    void durationChangeRequested(int sceneIndex, int panelIndex, int seconds);
    void panelMoveRequested(int flatIndex, int flatGap);
    void clipContextMenuRequested(int flatIndex, const QPoint &globalPos);
    void collapsedChanged(bool collapsed);
    // A lock, visibility or mute toggle by the user (never a load): the
    // window marks the project unsaved and the pages refresh what the
    // locks gate.
    void tracksChanged();
    // The playhead's panel or frame changed (see playheadPanel above).
    void playheadMoved();
    // The preview came over the canvas (true) or went (false). The Camera
    // bar is disabled while it covers the canvas (Phase 1B follow-up).
    void previewShownChanged(bool shown);
    // The CAMERA row's requests (Phase 1C; AnimaticTimeline's signals of
    // the same names), relayed to the workspace, which owns the edits.
    void cameraKeyActivated(int sceneIndex, int keyIndex);
    void cameraKeyMoveRequested(int sceneIndex, int keyIndex, int sceneFrame);
    void cameraKeyContextMenuRequested(int sceneIndex, int keyIndex,
                                       const QPoint &globalPos);
    void cameraRowContextMenuRequested(int flatPanelIndex, const QPoint &globalPos);
    void cameraAnimateToggled(bool on);
    void sceneContextMenuRequested(int sceneIndex, const QPoint &globalPos); // Phase 1D

public:
    // Test hooks. The duration one is the REAL request the timeline drag
    // makes; the rest read state, or stand in for a timer that would
    // otherwise take a panel's whole duration to fire.
    void setPanelDurationForTest(int sceneIndex, int panelIndex, int duration)
    {
        onDurationChanged(sceneIndex, panelIndex, duration);
    }
    AnimaticTimeline *timelineForTest() const { return m_timeline; }
    int selectedPanelForTest() const { return m_selected; }
    int itemCountForTest() const { return int(m_items.size()); }
    void advanceForTest() { advance(); }
    void scrubForTest(int flatIndex) { onScrubbed(flatIndex); }
    // The frame-accurate scrub (Phase 1A): what a playhead drag inside a
    // panel now requests.
    void scrubToForTest(int flatIndex, int frameInPanel)
    {
        onScrubbedTo(flatIndex, frameInPanel);
    }
    // What the preview draws with: its camera, whether it took the neutral
    // (pre-camera) path, and the transform it maps the panel through; the
    // 60 Hz camera tick, run once; whether its timer runs.
    shotcam::CameraState previewCameraForTest() const;
    bool previewCameraNeutralForTest() const;
    QTransform previewTransformForTest() const;
    void cameraTickForTest() { onCameraTick(); }
    bool cameraTimerActiveForTest() const;
    // Put the playhead inside a panel without playing: the accuracy capture
    // (2026-10-18) needs 00:00:18:41 exactly, which no navigation key gives.
    void setPlayheadForTest(int flatIndex, int elapsedMs);
    QString previewCaptionForTest() const;
    bool previewHasPictureForTest() const;
    QString timecodeTextForTest() const; // the header's HH:MM:SS:FF
    // The audio player's own position (ms) and whether it is playing: what
    // "the soundtrack plays" can be measured by. The gate cannot hear.
    qint64 audioPositionForTest() const;
    bool audioPlayingForTest() const;
    QString volumeToolTipForTest() const;
    // The gate cannot change the machine's default audio device: this
    // answers "what is the default output now?" in the system's place, and
    // syncAudioOutputForTest is what a device-change report from Windows
    // runs. Everything after the question is the real path.
    void setDefaultAudioOutputForTest(std::function<QAudioDevice()> provider)
    {
        m_defaultOutputForTest = std::move(provider);
    }
    void syncAudioOutputForTest() { syncAudioOutput(); }
    // The film clock's wait for the sound: its cap, and a switch that makes
    // the sound "never begin" so the cap is what starts the picture.
    void setAudioStartCapForTest(int ms) { m_audioStartCapMsForTest = ms; }
    void setAudioNeverBeginsForTest(bool never) { m_audioNeverBeginsForTest = never; }
    bool waitingForAudioForTest() const { return m_waitingForAudio; }
    qreal audioRateForTest() const;      // the player's playback rate
    qint64 audioWantedMsForTest() const { return m_audioWantedMs; } // a seek still pending
    // The file dialog cannot run under a test: this answers in its place.
    // Everything after the dialog is the real path.
    void setAudioPickerForTest(std::function<QString()> picker)
    {
        m_audioPickerForTest = std::move(picker);
    }
    // The audio track's menu, handed to the test instead of being shown.
    void setAudioMenuHookForTest(std::function<void(QMenu *)> hook)
    {
        m_audioMenuHookForTest = std::move(hook);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void hideEvent(QHideEvent *event) override; // leaving the workspace stops playback

private:
    struct Item
    {
        Scene *scene = nullptr;
        Panel *panel = nullptr;
        int sceneNumber = 0;
        int panelInScene = 0; // 1-based index within its scene
    };

    QWidget *createHeader();
    QWidget *createTimingStrip();
    void layoutHeader(); // the give-way rule and the pill's place
    void applyHeaderSpacing(bool compact); // BEFORE SELLING 13
    void sayNote(const QString &message);  // the dark note at the pointer

    void rebuildItems();
    void movePlayheadTo(int index); // playhead + timeline + timecode + preview
    void seekTo(int index);         // ...plus the running tick and the audio
    void onScrubbed(int index);     // playhead dragged: preview, select nothing
    void onScrubbedTo(int index, int frameInPanel); // ...to this frame of it
    void updatePreviewCamera(); // the preview's camera from cameraAtPlayhead()
    void onCameraTick();        // 60 Hz while playing: a move is smooth
    void enterPreview();
    void showPreviewFrame();
    void updateTotalLabel();
    QString formatTime(int seconds) const;

    void onDurationChanged(int sceneIndex, int panelIndex, int newDuration);
    void onPlayheadTick();

    void play();
    void pause();
    void seekAudio(qint64 ms);           // remembered until the file has loaded
    void onAudioPosition(qint64 ms);     // the player's position moved
    void startFilmClock(qint64 audioMs); // the picture begins (-1: no sound to follow)
    qint64 filmPositionMs() const;       // where the film is: panel offset + elapsed
    void setPlaybackSpeed(float speed); // takes effect on the next panel
    void updateSpeedControl();          // the dropdown's text and its checked item
    void scheduleTick();
    void advance();           // QTimer timeout

    void updateTimecodeLabel(); // HH:MM:SS:FF of the playhead

    void installAudio(const QString &path, bool linked); // set or clear the track, silently
    // Copy a picked file into the project (with progress; says why if it
    // cannot). False = nothing to request: failed, or cancelled.
    bool adoptIntoProject(const QString &picked, const QString &title,
                          QString *file);
    // Can a picked file be read as audio at all (AudioFileCheck)? Says why
    // not, and returns false: nothing is copied, nothing is requested.
    bool readableAsAudio(const QString &picked, const QString &title);
    void syncAudioOutput();                      // follow the system default
    void onPlayerProblem(const QString &reason); // the player could not open it
    void showAudioMenu(const QPoint &globalPos);
    void updateAudioUi();
    void applyTracks(); // push m_tracks to the timeline and the player
    bool hasAudio() const;
    qint64 offsetForPanel(int index) const; // ms before this panel

    void setLoopStart();
    void setLoopEnd();
    void clearLoop();
    void validateLoop();   // swap if reversed, clamp to range
    bool loopActive() const;
    void updateLoopUi();   // clear button + warning visibility

    QVector<Item> m_items; // derived: one row per panel, in play order
    int m_current = -1;    // the PLAYHEAD's panel
    int m_selected = -1;   // the workspace's selected panel (pushed in)
    bool m_playing = false;

    PanelDisplay *m_display = nullptr;  // the preview, a child of m_previewHost
    QPointer<QWidget> m_previewHost;    // the drawing canvas
    bool m_previewShown = false;
    QWidget *m_body = nullptr;          // everything below the header row
    QPushButton *m_collapseButton = nullptr;
    bool m_collapsed = false;
    // The header row's parts (see createHeader / layoutHeader).
    QWidget *m_header = nullptr;
    QWidget *m_leftGroup = nullptr;     // collapse + the timeline's zoom controls
    QWidget *m_rightGroup = nullptr;    // timecode .. view controls
    QWidget *m_transportPill = nullptr; // placed by layoutHeader, not the layout
    QWidget *m_centreGroup = nullptr;   // timecode | pill | total | speed, placed by layoutHeader
    QWidget *m_loopGroup = nullptr;
    QWidget *m_warningGroup = nullptr;      // the audio warning icon and its 20 px gap (Part D)
    QToolButton *m_audioWarning = nullptr;
    QWidget *m_timecodeSeparator = nullptr; // between the timecode and the pill
    QWidget *m_totalSeparator = nullptr;    // between the pill and the total
    TimelineTracks m_tracks;

    AnimaticTimeline *m_timeline = nullptr;
    QVector<Scene *> m_scenes; // source scenes, for duration edits + timeline rebuilds

    QPushButton *m_playButton = nullptr;
    QLabel *m_totalLabel = nullptr;
    QLabel *m_timecodeLabel = nullptr;
    QTimer *m_timer = nullptr;

    // Real-time playhead tracking. The elapsed time within the panel is a
    // WALL CLOCK (m_panelClock, scaled by the speed the panel was started
    // at) on top of a base - the time already spent when it was last
    // started, which is what a resume continues from.
    QTimer *m_playheadTimer = nullptr;
    // The shot camera's own tick while playing (16 ms): the playhead's 20 Hz
    // would show a camera move in steps. Does nothing in a scene without
    // keys (the camera there is neutral and the preview never repaints).
    QTimer *m_cameraTimer = nullptr;
    int m_elapsedMsInCurrentPanel = 0;
    QElapsedTimer m_panelClock;
    int m_panelBaseMs = 0;
    float m_panelSpeed = 1.0f;
    // The wait for the sound to begin (see the class comment).
    bool m_waitingForAudio = false;
    qint64 m_audioStartFromMs = 0; // where the player was asked to start
    QTimer *m_audioStartCap = nullptr;
    qint64 m_audioWantedMs = -1; // a seek the player could not take yet
    int m_audioStartCapMsForTest = -1;
    bool m_audioNeverBeginsForTest = false;

    // Playback speed (session-only): 0.5, 1.0, or 2.0.
    float m_playbackSpeed = 1.0f;
    // The speed dropdown (the user's choice, 2026-10-23): one 60 x 28 button
    // showing the current speed, a menu of the same three speeds.
    QToolButton *m_speedButton = nullptr;
    QMenu *m_speedMenu = nullptr;
    QAction *m_speedActions[3] = {nullptr, nullptr, nullptr}; // 0.5x, 1x, 2x

    // Audio.
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QString m_audioPath;          // what the project says
    bool m_audioLinked = false;   // ...as an old project's outside file
    bool m_audioMissing = false;  // ...and its file is not there
    bool m_audioUnplayable = false; // ...or is there and cannot be played
    QString m_audioProblem;         // ......and why
    bool m_noAudioOutput = false; // the system has no output device
    QMediaDevices *m_mediaDevices = nullptr;
    std::function<QAudioDevice()> m_defaultOutputForTest;
    QString m_projectFile;        // empty: the project has no file yet
    std::function<QString()> m_audioPickerForTest;
    std::function<void(QMenu *)> m_audioMenuHookForTest;

    bool m_scrubShowsCanvas = false; // the Camera tool's scrub (above)

    // Loop region (session-only).
    int m_loopStartIndex = -1;
    int m_loopEndIndex = -1;
    QPushButton *m_clearLoopButton = nullptr;
    QPushButton *m_loopStartButton = nullptr;
    QPushButton *m_loopEndButton = nullptr;

    // The header's compact spacing (BEFORE SELLING 13): the design's gaps
    // shrink, and the total drops its word, only below the full floor.
    QSpacerItem *m_pillGapBefore = nullptr; // 68 / 24
    QSpacerItem *m_pillGapAfter = nullptr;  // 68 / 24
    QSpacerItem *m_totalGap = nullptr;      // 34 / 16
    QSpacerItem *m_speedGap = nullptr;      // 28 / 12
    QSpacerItem *m_viewGap = nullptr;       // 31 / 16 (loop group to the view controls)
    bool m_headerCompact = false;
    int m_headerFullFloor = 0;
    QString m_totalTime; // the total without its word
    QString m_lastNote;
    bool m_notesAtPointer = true;
};
