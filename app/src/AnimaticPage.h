#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class AnimaticTimeline;
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

// Fourth pipeline screen: plays the storyboard panels in sequence with
// per-panel durations. Reads Scene/Panel pointers directly (no copies).
class AnimaticPage : public QWidget
{
    Q_OBJECT

public:
    explicit AnimaticPage(QWidget *parent = nullptr);

    void loadScenes(const QVector<Scene *> &scenes);

    // Scratch audio path persistence (used by project save/load).
    QString audioPath() const;
    void setAudioPath(const QString &path); // loads silently if the file exists

    // Read by AnimaticTimeline when rendering the real-time playhead.
    int currentFlatIndex() const { return m_current; }
    int elapsedMsInCurrentPanel() const { return m_elapsedMsInCurrentPanel; }

signals:
    void backRequested();

private:
    struct Item
    {
        Scene *scene = nullptr;
        Panel *panel = nullptr;
        int sceneNumber = 0;
        int panelInScene = 0; // 1-based index within its scene
    };

    QWidget *createTopBar();
    QWidget *createDisplay();
    QWidget *createTimingStrip();
    QWidget *createControls();

    void showPanel(int index);
    void updateTotalLabel();
    QString formatTime(int seconds) const;

    void onDurationChanged(int sceneIndex, int panelIndex, int newDuration);
    void onPlayheadTick();

    void play();
    void pause();
    void togglePlay();
    void setPlaybackSpeed(float speed); // takes effect on the next panel
    void updateSpeedButtons();          // active = amber-filled, others outlined
    void scheduleTick();
    void advance();           // QTimer timeout
    void goFirst();
    void goLast();
    void goPrev();
    void goNext();
    void jumpTo(int index);

    void updateTimecodeLabel(); // HH:MM:SS:FF of the playhead

    void onExportMp4();

    void onImportAudio();
    void onRemoveAudio();
    void loadAudioFile(const QString &path);
    void updateAudioUi();
    bool hasAudio() const;
    qint64 offsetForPanel(int index) const; // ms before this panel

    void setLoopStart();
    void setLoopEnd();
    void clearLoop();
    void validateLoop();   // swap if reversed, clamp to range
    bool loopActive() const;
    void updateLoopUi();   // clear button + warning visibility

    QVector<Item> m_items;
    int m_current = -1;
    bool m_playing = false;

    PanelDisplay *m_display = nullptr;
    QLabel *m_caption = nullptr;

    AnimaticTimeline *m_timeline = nullptr;
    QVector<Scene *> m_scenes; // source scenes, for duration edits + timeline rebuilds

    QPushButton *m_playButton = nullptr;
    QPushButton *m_exportButton = nullptr;
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
    QString m_audioPath;
    QPushButton *m_removeAudioButton = nullptr;
    QLabel *m_audioLabel = nullptr;
    QSlider *m_volumeSlider = nullptr;

    // Loop region (session-only).
    int m_loopStartIndex = -1;
    int m_loopEndIndex = -1;
    QPushButton *m_clearLoopButton = nullptr;
    QLabel *m_loopWarningLabel = nullptr;
};
