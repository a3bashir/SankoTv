#pragma once

#include "DrawingCanvas.h" // DrawingCanvas::Tool (persisted selection mode)

#include <QHash>
#include <functional>
#include <QPoint>
#include <QSet>
#include <QVector>
#include <QWidget>

class ZoomToolbar;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QJsonArray;
class QJsonObject;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QUndoStack;
class QVBoxLayout;
class SankoSlider;
class SankoTipPopup;

class DockController;
class QDockWidget;
class QMainWindow;

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
    ~StoryboardPage() override; // persists the dock layout

    // Display the given scenes. Ownership stays with the caller (MainWindow);
    // this page only holds non-owning pointers.
    void loadScenes(const QVector<Scene *> &scenes);

    // Read-only reference to the project's consistency entries (for future
    // prompt injection). Not displayed yet.
    void setConsistencyEntries(const QVector<ConsistencyEntry> *entries)
    {
        m_consistencyEntries = entries;
    }

    // Preferences > Camera plumbing: safe-area guide opacities in percent,
    // forwarded live to the drawing canvas.
    void setActionSafeMaskOpacity(int percent);
    void setTitleSafeMaskOpacity(int percent);

    // Edit-menu entry points: route to the CANVAS selection clipboard when a
    // selection exists (copy/cut) or the last copy came from the canvas
    // (paste); otherwise fall back to the panel-level clipboard below.
    void editCopy();
    void editCut();
    void editPaste();
    void editPasteInPlace();

    // Edit-menu undo/redo (Ctrl+Z / Ctrl+Y): the app-wide chronological
    // history — one shared QUndoStack covering drawing, selection, and panel
    // actions, same as the Brush-bar buttons.
    void editUndo();
    void editRedo();

    // Perspective guide settings, persisted in the project file (delegates
    // to the canvas's PerspectiveTool; refreshes the settings panel on load).
    QJsonObject perspectiveToJson() const;
    void perspectiveFromJson(const QJsonObject &object);

    // Bake any pending QuickShape vector (called before save/load, so the
    // temporary overlay is never silently dropped or left un-serialized).
    void commitQuickShape();

    // App-wide undo stack (owned by MainWindow); forwarded to the canvas.
    void setUndoStack(QUndoStack *stack);
    // Callbacks for the panel undo commands (see StoryboardPage.cpp): mutate
    // the scene's panel list and refresh the strip/selection.
    void applyPanelInsertForUndo(Scene *scene, int index, Panel *panel);
    Panel *applyPanelRemoveForUndo(Scene *scene, int index);
    void applyPanelMoveForUndo(Scene *scene, int from, int to);

    // Panel-level clipboard. Copy stores an owned deep copy; paste inserts a
    // fresh clone (new layer UUIDs) each time. Cut is blocked on a scene's
    // last panel, same rule as Delete.
    void copySelectedPanel();
    void cutSelectedPanel();
    void pastePanelAfterSelected(); // after the selected panel
    void pastePanelInPlace();       // at the copied-from position
    bool hasPanelClipboard() const { return m_panelClipboard != nullptr; }

signals:
    void panelClipboardChanged(bool available); // enables the paste actions
    void backRequested();
    void continueToAnimaticRequested(const QVector<Scene *> &scenes);
    void consistencyBoardRequested();
    void settingsRequested(); // Layers toolbar Settings button -> Preferences

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    QWidget *createLeftColumn();
    QWidget *createCenterColumn();
    QWidget *createPanelControls(); // fixed, non-scrolling column at the left of the strip
    QWidget *createRightColumn();
    QWidget *createLayerPanel();
    void createFloatingToolbar();   // Brush bar + extras bar (FloatingToolWindows)
    // Floating overlay panel: dock-style header (title + Close only),
    // draggable by the header, child of the canvas.
    QWidget *createFloatingPanel(const QString &title, QWidget *body);
    QWidget *createBrushSettings(); // floating panel shown while Brush is active
    QWidget *createCameraPanel();   // floating panel shown while Camera is active
    QWidget *createPerspectiveModifier(); // shown while Perspective is active
    QWidget *createShapesPanel();   // floating panel shown while Shapes is active
    QWidget *createBottomBar();
    void applyBrushPreset(int size, int opacityPct, int hardnessPct,
                          bool pressureSize, bool pressureOpacity);

    // Dockable panel plumbing (Qt Advanced Docking System hosts the panels).
    void installDockViewActions(); // dock toggles + Reset Layout in the View menu
    void applyDefaultDockLayout(); // restores the captured default ADS layout
    bool restoreDockState();       // true if a saved layout was applied
    void saveDockState();

    // Layer panel (docked, right of the canvas; Figma 7-70).
    void rebuildLayerPanel();     // rows from the current panel's layer stack
    void setActiveLayer(int index);
    // Photoshop-style selection: plain click = single, Shift = range from
    // the anchor, Ctrl = toggle. Selection indices live in m_layerSelection;
    // the ACTIVE layer (drawing target) is panel->activeLayerIndex.
    void layerRowClicked(int index, Qt::KeyboardModifiers modifiers);
    QVector<int> selectedLayers() const; // ascending, valid, non-background
    void layerAdd();              // blank raster layer above the active one
    void layerAddImage();         // file dialog -> new image-type layer
    void layerDeleteSelected();   // never deletes Background / the last layer
    void layerDuplicateSelected();
    void layerMergeSelected();    // multi: into one; single: merge down
    void layerClearSelected();    // wipe pixels to transparent
    void layerGroupSelected();    // flatten selection into one "Group" layer
    void layerSetColorTag(int index);
    void layerBeginRename(int index); // inline QLineEdit over the name label
    // Drag-reorder: move the given ascending source indices so the block
    // starts at insertAt (a gap index in the PRE-move list); relative order
    // of the moved layers is preserved.
    void layerMoveTo(const QVector<int> &sources, int insertAt);
    void refreshLayerCanvas();    // repaint canvas + panel thumbnail after a layer change

    void layerContextMenu(int index, const QPoint &globalPos);
    void startLayerDrag(int index); // QDrag carrying the selected indices
    // Shared core of Merge and Group: flatten the given ascending indices
    // into the lowest one (optionally renaming it).
    void mergeLayerIndices(const QVector<int> &indices, const QString &newName);

    // Reuse-across-panels (shared layers): reference the SAME layer data
    // from another panel. Instances share pixels via sharedId; edits made
    // anywhere propagate to every instance (syncSharedLayers), and the
    // project file stores the image once.
    void layerReuseInPanel(int index);
    // Dialog-free core: marks the source shared and appends a referencing
    // instance to `target`. False when the reference already exists there.
    bool reuseLayerInPanelCore(int index, Panel *target);
    void syncSharedLayers(Panel *source);
    // Reference counting on delete/merge: when only ONE instance of a
    // sharedId remains anywhere, it stops being shared.
    void releaseSharedIfLastInstance(const QString &sharedId);

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
    void updateLightTable(); // feed the neighbour pixmaps (red prev / green next)

    // Panel reordering (drag within the strip + keyboard).
    void beginPanelDrag();
    void updatePanelDrag(const QPoint &globalPos);
    void finishPanelDrag();
    void cancelPanelDrag();
    int dropTargetForX(const QPoint &globalPos) const;
    void movePanel(int from, int target); // target is an insertion index (0..N)
    void movePanelBy(int delta);           // keyboard: -1 left, +1 right

    void duplicatePanel();                 // copy current panel, insert after it
    static Panel *clonePanel(const Panel *source); // deep copy, fresh layer UUIDs
    void insertPanelClone(const Panel *panel, int insertAt,
                          const QString &text); // into the current scene (undoable)
    void importImageToPanel();             // file dialog -> canvas->importImage
    void updateDuplicateButton();          // enable panel-action buttons when a panel is selected

    Scene *currentScene() const;
    Panel *currentPanel() const;

    QVector<Scene *> m_scenes;
    const QVector<ConsistencyEntry> *m_consistencyEntries = nullptr; // read-only
    int m_currentScene = -1;
    int m_currentPanel = -1;

    // Edit-menu panel clipboard: owned deep copy plus the position it was
    // copied from (for Paste in Place).
    Panel *m_panelClipboard = nullptr;
    int m_clipboardSceneIndex = -1;
    int m_clipboardPanelIndex = -1;
    // Which clipboard the LAST copy/cut fed — paste routes to the same one.
    enum class ClipSource { None, Canvas, PanelLevel };
    ClipSource m_lastClipSource = ClipSource::None;

    // Native docking: an embedded (child-widget) QMainWindow hosts the stock
    // QDockWidget engine; the reusable DockController adds custom title
    // bars, previews, tab/split drops, collapse and persistence. The canvas
    // area is the host's central widget; Scenes, Layers, and Shot Info are
    // QDockWidgets around it.
    QMainWindow *m_dockHost = nullptr;
    DockController *m_dockController = nullptr;
    QDockWidget *m_layersDock = nullptr;
    QDockWidget *m_scenesDock = nullptr;
    QDockWidget *m_shotInfoDock = nullptr;

    // Left column.
    QVBoxLayout *m_sceneListLayout = nullptr;
    QVector<QWidget *> m_sceneCards;

    // Center column.
    QHBoxLayout *m_panelStripLayout = nullptr;
    QVector<QWidget *> m_panelThumbs;
    QVector<QLabel *> m_panelThumbImages;
    DrawingCanvas *m_canvas = nullptr;
    ZoomToolbar *m_zoomToolbar = nullptr; // custom-painted view controls
    QScrollArea *m_panelScroll = nullptr;
    QPushButton *m_importButton = nullptr;
    // Brush settings panel (visible only while the Brush tool is active).
    QWidget *m_brushPanel = nullptr;
    // Floating Size/Opacity toolbar (Figma 209:42): size + opacity sliders
    // and Fit Screen, snapping to the left/right canvas edge.
    QWidget *m_sizeCtlBar = nullptr;
    std::function<void(int)> m_setSizeCtl;    // toolbar sliders <- presets
    std::function<void(int)> m_setOpacityCtl;
    SankoSlider *m_brushOpacitySlider = nullptr;
    SankoSlider *m_brushHardnessSlider = nullptr;
    // Camera panel (visible only while the Camera tool is active).
    QWidget *m_cameraPanel = nullptr;
    // Figma "Floating Toolbar Layers" (node 173-36): dock toggles + tools.
    QWidget *m_layersToolbar = nullptr;
    // Perspective Modifier toolbar (visible only while Perspective is active).
    QWidget *m_perspModToolbar = nullptr;
    std::function<void()> m_syncPerspective; // toolbar controls <- canvas model
    // Shapes panel (visible only while the Shapes tool is active).
    QWidget *m_shapesPanel = nullptr;
    // Last-chosen selection mode: a plain click on the combined Selection
    // button re-activates it; hold/right-click opens the mode menu.
    DrawingCanvas::Tool m_selectionMode = DrawingCanvas::SelectRect;
    // Floating overlays: every bar/panel is a FloatingToolWindow (frameless
    // OS-composited tool window) anchored to the canvas — drag, clamping,
    // main-window follow, page-visibility mirroring, and position persistence
    // all live in that base class (and cover future panels automatically).
    QWidget *m_floatToolbar = nullptr;  // horizontal Brush/tools bar (Figma 33:110)
    SankoTipPopup *m_toolbarTip = nullptr; // ONE reused tooltip for the Brush bar
    QWidget *m_selModToolbar = nullptr;  // Selection Modifier bar (Figma 146:67)
    QWidget *m_moveModToolbar = nullptr; // Move Modifier bar (Figma 161:39)
    QWidget *m_bottomBar = nullptr;      // status bar; both mod bars sit 10px above
    QCheckBox *m_pressureSizeCheck = nullptr;
    QCheckBox *m_pressureOpacityCheck = nullptr;
    // Fixed control column (left of the panel strip).
    QPushButton *m_addPanelButton = nullptr;
    QPushButton *m_dupPanelButton = nullptr;
    QPushButton *m_clearPanelButton = nullptr; // clears the drawing, asks first
    QPushButton *m_deletePanelButton = nullptr;
    QPushButton *m_lightTableButton = nullptr; // toggles neighbour-panel ghosts

    QUndoStack *m_undoStack = nullptr; // app-wide history (owned by MainWindow)

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
    SankoSlider *m_layerOpacity = nullptr; // custom glowing opacity slider
                                           // (paints its own "NN%" label)
    QPushButton *m_layerDeleteButton = nullptr;
    QPushButton *m_layerMergeButton = nullptr;
    bool m_updatingLayerUi = false; // guards the opacity slider feedback loop
    // Photoshop-style selection state (indices into the current panel's
    // layer vector) + the Shift-range anchor. Rebuilt rows render selected
    // state; cleared on panel switches.
    QSet<int> m_layerSelection;
    int m_layerAnchor = -1;
    Panel *m_layerSelPanel = nullptr;   // selection resets on panel switches
    QWidget *m_layerListHost = nullptr; // drop target for row reordering

    // Right column.
    QComboBox *m_shotType = nullptr;
    QComboBox *m_camera = nullptr;
    QComboBox *m_lens = nullptr;
    QLineEdit *m_mood = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QLabel *m_actionLabel = nullptr;

    bool m_loadingShotInfo = false;
};
