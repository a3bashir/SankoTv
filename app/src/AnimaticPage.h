#pragma once

#include <QPoint>
#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class AnimaticTimeline;
class QMenu;
class PanelDisplay;
class QAudioOutput;
class QHBoxLayout;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QSlider;
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

    // The buttons that have not yet moved to the menus (see the .cpp); the
    // workspace places this in its bottom bar.
    QWidget *legacyActions() const { return m_legacyActions; }

    // Transport, for the workspace's pointer-routed keys.
    void togglePlay();
    void goFirst();
    void goLast();
    void goPrev();
    void goNext();
    bool isPlaying() const { return m_playing; }

    // Scratch audio path persistence (used by project save/load). The path
    // is the DOCUMENT's: it is kept, and saved, whether or not its file can
    // be found right now. audioMissing() is what the disk says about it.
    QString audioPath() const;
    void setAudioPath(const QString &path); // silent: a load is not an edit
    bool audioMissing() const { return m_audioMissing; }
    // Where the Locate dialog starts when the stored path's own folder is
    // gone too: the project's folder.
    void setFallbackFolder(const QString &folder) { m_fallbackFolder = folder; }

    // The audio track's two edits, for the Edit menu and the track's own
    // right-click menu. Both only REQUEST (audioChangeRequested): the
    // workspace makes the change an undoable command and calls
    // applyAudioPath when it runs, is undone, or is redone.
    void importAudio(); // asks for a file
    void removeAudio();
    void locateAudio(); // a missing track: point it at its file again
    void applyAudioPath(const QString &path); // pauses playback first

    // Project frame rate (New Project dialog / project file): forwarded to
    // the timeline, which derives every frame count from it.
    void setFps(int fps);

    // Read by AnimaticTimeline when rendering the real-time playhead.
    int currentFlatIndex() const { return m_current; }
    int elapsedMsInCurrentPanel() const { return m_elapsedMsInCurrentPanel; }

signals:
    void generationRequested();
    // The track changed, by any route (an edit, an undo, a project load):
    // the menus enable Remove Audio from this. NOT a statement about the
    // document - nothing here says "unsaved" any more; an audio edit is a
    // command on the undo stack, and the stack is what marks the project
    // dirty. (This class used to emit documentChanged for it.)
    void audioStateChanged();
    // --- requests to the workspace (user input only; never from a slot) ---
    // newPath empty = remove the track. commandText names it in the history.
    void audioChangeRequested(const QString &newPath, const QString &commandText);
    void panelSelectRequested(int flatIndex);
    void durationChangeRequested(int sceneIndex, int panelIndex, int seconds);
    void panelMoveRequested(int flatIndex, int flatGap);
    void clipContextMenuRequested(int flatIndex, const QPoint &globalPos);
    void collapsedChanged(bool collapsed);

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
    QString previewCaptionForTest() const;
    bool previewHasPictureForTest() const;
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

    QWidget *createLegacyActions();
    QWidget *createHeader();
    QWidget *createTimingStrip();

    void rebuildItems();
    void movePlayheadTo(int index); // playhead + timeline + timecode + preview
    void seekTo(int index);         // ...plus the running tick and the audio
    void onScrubbed(int index);     // playhead dragged: preview, select nothing
    void enterPreview();
    void showPreviewFrame();
    void updateTotalLabel();
    QString formatTime(int seconds) const;

    void onDurationChanged(int sceneIndex, int panelIndex, int newDuration);
    void onPlayheadTick();

    void play();
    void pause();
    void setPlaybackSpeed(float speed); // takes effect on the next panel
    void updateSpeedButtons();          // active = amber-filled, others outlined
    void scheduleTick();
    void advance();           // QTimer timeout

    void updateTimecodeLabel(); // HH:MM:SS:FF of the playhead

    void onExportMp4();

    void installAudio(const QString &path); // set or clear the track, silently
    void showAudioMenu(const QPoint &globalPos);
    void updateAudioUi();
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
    QWidget *m_legacyActions = nullptr;
    QWidget *m_body = nullptr;          // everything below the header row
    QPushButton *m_collapseButton = nullptr;
    bool m_collapsed = false;

    AnimaticTimeline *m_timeline = nullptr;
    QVector<Scene *> m_scenes; // source scenes, for duration edits + timeline rebuilds

    QPushButton *m_playButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_generationButton = nullptr;
    QLabel *m_totalLabel = nullptr;
    QLabel *m_timecodeLabel = nullptr;
    QTimer *m_timer = nullptr;

    // Real-time playhead tracking.
    QTimer *m_playheadTimer = nullptr;
    int m_elapsedMsInCurrentPanel = 0;

    // Playback speed (session-only): 0.5, 1.0, or 2.0.
    float m_playbackSpeed = 1.0f;
    QPushButton *m_speedHalfButton = nullptr;
    QPushButton *m_speed1xButton = nullptr;
    QPushButton *m_speed2xButton = nullptr;

    // Audio.
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    QString m_audioPath;          // what the project says
    bool m_audioMissing = false;  // ...and its file is not there
    QString m_fallbackFolder;
    QLabel *m_audioLabel = nullptr;
    std::function<QString()> m_audioPickerForTest;
    std::function<void(QMenu *)> m_audioMenuHookForTest;
    QSlider *m_volumeSlider = nullptr;

    // Loop region (session-only).
    int m_loopStartIndex = -1;
    int m_loopEndIndex = -1;
    QPushButton *m_clearLoopButton = nullptr;
    QLabel *m_loopWarningLabel = nullptr;
};
