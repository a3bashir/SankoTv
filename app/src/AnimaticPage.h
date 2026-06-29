#pragma once

#include <QVector>
#include <QWidget>

class PanelDisplay;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QSpinBox;
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

signals:
    void backRequested();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

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

    void rebuildTimingStrip();
    void showPanel(int index);
    void updateStripHighlight();
    void updateTotalLabel();
    QString formatTime(int seconds) const;

    void play();
    void pause();
    void togglePlay();
    void scheduleTick();
    void advance();           // QTimer timeout
    void goFirst();
    void goLast();
    void jumpTo(int index);

    void onExportMp4();

    QVector<Item> m_items;
    int m_current = -1;
    bool m_playing = false;

    PanelDisplay *m_display = nullptr;
    QLabel *m_caption = nullptr;

    QHBoxLayout *m_stripLayout = nullptr;
    QVector<QLabel *> m_thumbs;
    QVector<QSpinBox *> m_spins;

    QPushButton *m_playButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QLabel *m_totalLabel = nullptr;
    QTimer *m_timer = nullptr;
};
