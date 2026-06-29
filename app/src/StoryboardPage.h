#pragma once

#include <QVector>
#include <QWidget>

class DrawingCanvas;
class QComboBox;
class QHBoxLayout;
class QJsonArray;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QVBoxLayout;

struct Panel;
struct Scene;
struct ConsistencyEntry;

// Third pipeline screen: scene list (left), panel grid + drawing canvas
// (center), and shot info (right). Scenes are passed in from the Script Editor.
class StoryboardPage : public QWidget
{
    Q_OBJECT

public:
    explicit StoryboardPage(QWidget *parent = nullptr);
    ~StoryboardPage() override = default;

    // Display the given scenes. Ownership stays with the caller (MainWindow);
    // this page only holds non-owning pointers.
    void loadScenes(const QVector<Scene *> &scenes);

    // Read-only reference to the project's consistency entries (for future
    // prompt injection). Not displayed yet.
    void setConsistencyEntries(const QVector<ConsistencyEntry> *entries)
    {
        m_consistencyEntries = entries;
    }

signals:
    void backRequested();
    void continueToAnimaticRequested(const QVector<Scene *> &scenes);
    void consistencyBoardRequested();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    QWidget *createLeftColumn();
    QWidget *createCenterColumn();
    QWidget *createRightColumn();
    QWidget *createToolbar();
    QWidget *createBottomBar();

    void rebuildSceneList();
    void rebuildPanelStrip();
    void updateSceneCardStyles();
    void updatePanelThumbStyles();

    void selectScene(int index);
    void selectPanel(int index);
    void addPanelToScene(int sceneIndex);

    void loadShotInfo();   // current panel -> right column widgets
    void saveShotInfo();   // right column widgets -> current panel
    void refreshCurrentThumb();

    Scene *currentScene() const;
    Panel *currentPanel() const;

    QVector<Scene *> m_scenes;
    const QVector<ConsistencyEntry> *m_consistencyEntries = nullptr; // read-only
    int m_currentScene = -1;
    int m_currentPanel = -1;

    // Left column.
    QVBoxLayout *m_sceneListLayout = nullptr;
    QVector<QWidget *> m_sceneCards;

    // Center column.
    QHBoxLayout *m_panelStripLayout = nullptr;
    QVector<QWidget *> m_panelThumbs;
    QVector<QLabel *> m_panelThumbImages;
    DrawingCanvas *m_canvas = nullptr;

    // Right column.
    QComboBox *m_shotType = nullptr;
    QComboBox *m_camera = nullptr;
    QComboBox *m_lens = nullptr;
    QLineEdit *m_mood = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QLabel *m_actionLabel = nullptr;

    bool m_loadingShotInfo = false;
};
