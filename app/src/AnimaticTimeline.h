#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class AnimaticPage;
class TimelineCanvas; // inner painted surface, defined in the .cpp
class QPushButton;
class QScrollBar;
class QSlider;
struct Panel;
struct Scene;

// Professional NLE-style timeline for the Animatic screen. A zoom toolbar sits
// above a multi-track canvas (timecode ruler, scene track, panel/shot clips,
// audio waveform, and reserved camera/markers tracks) with a fixed left label
// column and a horizontally scrollable, zoomable canvas.
class AnimaticTimeline : public QWidget
{
    Q_OBJECT

public:
    explicit AnimaticTimeline(QWidget *parent = nullptr);

    // The timeline reads the live elapsed time / current index from the page
    // when rendering the playhead (see updatePlayhead()).
    void setHost(AnimaticPage *host);

public slots:
    void setScenes(const QVector<Scene *> &scenes);
    void setCurrentPanel(int flatIndex);
    void setPlaying(bool playing);
    void setLoopRegion(int startIndex, int endIndex);
    void setAudioLoaded(bool loaded, qint64 audioDurationMs);
    void updatePlayhead();

signals:
    void panelSeekRequested(int flatPanelIndex);
    void durationChanged(int sceneIndex, int panelIndex, int newDurationSeconds);
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
    int m_current = -1;
    bool m_playing = false;
    int m_loopStart = -1;
    int m_loopEnd = -1;
    bool m_audioLoaded = false;
    qint64 m_audioDurationMs = 0;
    AnimaticPage *m_host = nullptr;

    float m_zoom = 100.0f; // 100 = 1px per frame
    int m_scrollX = 0;     // content scroll offset in px
    bool m_framesMode = false; // false = timecode, true = frame numbers
    bool m_snap = true;

    // Interaction.
    int m_hoverIndex = -1;
    enum class Drag { None, ResizeRight, ResizeLeft, Playhead };
    Drag m_drag = Drag::None;
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
