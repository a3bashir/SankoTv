#pragma once

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

class AnimaticPage;
class TimelineCanvas; // inner painted surface, defined in the .cpp
class QContextMenuEvent;
class QPushButton;
class QScrollBar;
class QSlider;
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
    // What the audio track is showing: "none", "loading", "present" or
    // "missing"; its text (the hint, "name | m:ss", or the missing label);
    // and where the AUDIO row is.
    QString audioBarStateForTest() const;
    QString audioBarTextForTest() const;
    QRect audioRowForTest() const;

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

public slots:
    void setScenes(const QVector<Scene *> &scenes);
    void setCurrentPanel(int flatIndex);  // the panel under the PLAYHEAD
    void setSelectedPanel(int flatIndex); // the panel on the drawing canvas
    void setPlaying(bool playing);
    void setLoopRegion(int startIndex, int endIndex);
    void setAudioLoaded(bool loaded, qint64 audioDurationMs);
    void setAudioMissing(bool missing); // the project names a file that is gone
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
        enum State { None, Loading, Present, Missing };
        State state = None;
        QString name;   // the file's name
        QString length; // "m:ss", Present only
        QString hint;   // None only
        QString label;  // Missing only: what the bar says
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
    void applyZoom(float z);
    void fitZoom();
    int snapFrame(int frame) const;
    QString timecode(int frame) const;

    // Toolbar handlers.
    void styleToolbarButtons();

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
    qint64 m_audioDurationMs = 0;
    AnimaticPage *m_host = nullptr;

    float m_zoom = 100.0f; // 100 = 1px per frame
    int m_scrollX = 0;     // content scroll offset in px
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
    QSlider *m_zoomSlider = nullptr;
    QPushButton *m_fitButton = nullptr;
    QPushButton *m_zoomInButton = nullptr;
    QPushButton *m_zoomOutButton = nullptr;
    QPushButton *m_framesButton = nullptr;
    QPushButton *m_secondsButton = nullptr;
    QPushButton *m_snapButton = nullptr;
};
