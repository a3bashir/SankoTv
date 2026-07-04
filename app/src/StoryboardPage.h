#pragma once

#include <QPoint>
#include <QVector>
#include <QWidget>

class DrawingCanvas;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QJsonArray;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
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
    QWidget *createPanelControls(); // fixed, non-scrolling column at the left of the strip
    QWidget *createRightColumn();
    QWidget *createLayerPanel();
    QWidget *createToolbar();
    QWidget *createBrushSettings(); // side panel shown while the Brush tool is active
    QWidget *createBottomBar();
    void applyBrushPreset(int size, int opacityPct, int hardnessPct,
                          bool pressureSize, bool pressureOpacity);

    // Layer panel (docked, right of the canvas).
    void rebuildLayerPanel();     // rows from the current panel's layer stack
    void setActiveLayer(int index);
    void renameLayer(int index);
    void layerAdd();              // blank raster layer above the active one
    void layerAddImage();         // file dialog -> new image-type layer
    void layerDelete();           // blocked when only one layer remains
    void layerMergeDown();        // flatten active into the layer below
    void layerMove(int delta);    // +1 = toward front (up in the list)
    void refreshLayerCanvas();    // repaint canvas + panel thumbnail after a layer change

    void rebuildSceneList();
    void rebuildPanelStrip();
    void updateSceneCardStyles();
    void updatePanelThumbStyles();

    void selectScene(int index);
    void selectPanel(int index);
    void addPanelToScene(int sceneIndex);
    void addPanelAfterSelected();           // control column "+": insert after the selected panel
    void deleteSelectedPanel();             // control column trash: confirm, then delete (blocks the last)

    void loadShotInfo();   // current panel -> right column widgets
    void saveShotInfo();   // right column widgets -> current panel
    void refreshCurrentThumb();
    void updateOnionGhost(); // feed the previous panel's pixmap to the canvas

    // Panel reordering (drag within the strip + keyboard).
    void beginPanelDrag();
    void updatePanelDrag(const QPoint &globalPos);
    void finishPanelDrag();
    void cancelPanelDrag();
    int dropTargetForX(const QPoint &globalPos) const;
    void movePanel(int from, int target); // target is an insertion index (0..N)
    void movePanelBy(int delta);           // keyboard: -1 left, +1 right

    void duplicatePanel();                 // copy current panel, insert after it
    void importImageToPanel();             // file dialog -> canvas->importImage
    void updateDuplicateButton();          // enable panel-action buttons when a panel is selected

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
    QScrollArea *m_panelScroll = nullptr;
    QPushButton *m_onionButton = nullptr;
    QPushButton *m_importButton = nullptr;
    // Brush settings panel (visible only while the Brush tool is active).
    QWidget *m_brushPanel = nullptr;
    QSlider *m_brushSizeSlider = nullptr;
    QSlider *m_brushOpacitySlider = nullptr;
    QSlider *m_brushHardnessSlider = nullptr;
    QCheckBox *m_pressureSizeCheck = nullptr;
    QCheckBox *m_pressureOpacityCheck = nullptr;
    // Fixed control column (left of the panel strip).
    QPushButton *m_addPanelButton = nullptr;
    QPushButton *m_dupPanelButton = nullptr;
    QPushButton *m_deletePanelButton = nullptr;

    // Drag-reorder state.
    bool m_panelPressActive = false;
    bool m_panelDragging = false;
    int m_dragSourceIndex = -1;
    int m_dropTarget = -1;
    QPoint m_dragStartGlobal;
    QLabel *m_dragGhost = nullptr;        // semi-transparent thumbnail follows cursor
    QWidget *m_dropIndicator = nullptr;   // amber line between thumbnails

    // Layer panel.
    QVBoxLayout *m_layerListLayout = nullptr;
    QVector<QWidget *> m_layerRows;
    QSlider *m_layerOpacity = nullptr;
    QLabel *m_layerOpacityValue = nullptr;
    QPushButton *m_layerDeleteButton = nullptr;
    QPushButton *m_layerMergeButton = nullptr;
    bool m_updatingLayerUi = false; // guards the opacity slider feedback loop

    // Right column.
    QComboBox *m_shotType = nullptr;
    QComboBox *m_camera = nullptr;
    QComboBox *m_lens = nullptr;
    QLineEdit *m_mood = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QLabel *m_actionLabel = nullptr;

    bool m_loadingShotInfo = false;
};
