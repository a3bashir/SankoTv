#pragma once

#include "DrawingCanvas.h" // DrawingCanvas::Tool (persisted selection mode)

#include <QHash>
#include <functional>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QWidget>

class ZoomToolbar;
namespace brushlib {
class BrushLibraryModel;
class BrushLibraryPanel;
class BrushSettingsStudio;
}
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
class QTimer;
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
    // Drop every non-owning pointer into the current scenes because they are
    // about to be destroyed. NOT the same as loadScenes({}), which leaves the
    // canvas holding its panel — see the implementation.
    void detachScenes();
    // The PROJECT's canvas size — the authority every new panel is created
    // at. Set by MainWindow at project create/load, before any panel can be
    // added. Existing panels carry their own size (pixels are the truth);
    // this member only shapes panels that do not exist yet.
    void setProjectCanvasSize(const QSize &size) { m_projectCanvasSize = size; }
    QSize projectCanvasSize() const { return m_projectCanvasSize; }

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
    void ensureBrushPixelsForSave();

    // App-wide undo stack (owned by MainWindow); forwarded to the canvas.
    void setUndoStack(QUndoStack *stack);
    Panel *makeProjectPanel() const; // blank panel at the project size
    // Callbacks for the panel undo commands (see StoryboardPage.cpp): mutate
    // the scene's panel list and refresh the strip/selection.
    void applyPanelInsertForUndo(Scene *scene, int index, Panel *panel);
    Panel *applyPanelRemoveForUndo(Scene *scene, int index);
    void applyPanelMoveForUndo(Scene *scene, int from, int to);
    // Layer-stack undo: restore a panel's whole layer vector + active index
    // (QImage handles are implicitly shared, so snapshots are cheap), then
    // refresh the canvas/panel/thumb UI.
    void applyLayerStackForUndo(Panel *panel, const QVector<Layer> &layers,
                                int activeIndex);

    // Panel-level clipboard. Copy stores an owned deep copy; paste inserts a
    // fresh clone (new layer UUIDs) each time. Cut is blocked on a scene's
    // last panel, same rule as Delete.
    void copySelectedPanel();
    void cutSelectedPanel();
    void pastePanelAfterSelected(); // after the selected panel
    void pastePanelInPlace();       // at the copied-from position
    bool hasPanelClipboard() const { return m_panelClipboard != nullptr; }
    // Would pasting the clipboard panel leave this project holding panels
    // of two different sizes? The refusal's decision, without its dialog —
    // exposed so the gate can assert it directly.
    bool pasteWouldMixSizes() const;

    // Is the user mid-edit? A project resize refuses rather than committing
    // it; `what` names the edit for the message.
    bool hasActiveEdit(QString *what = nullptr) const;
    // Everything on the storyboard side that a completed canvas resize must
    // update or clear. Called AFTER the panels have been swapped, with the
    // centre offset that was applied.
    void applyProjectResize(const QSize &newSize, const QPoint &offset);

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
    QWidget *createCameraPanel();   // floating panel shown while Camera is active
    QWidget *createPerspectiveModifier(); // shown while Perspective is active
    QWidget *createShapesPanel();   // floating panel shown while Shapes is active
    QWidget *createBottomBar();

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
    // Merge core: flatten the given ascending indices into the lowest one
    // (optionally renaming it).
    void mergeLayerIndices(const QVector<int> &indices, const QString &newName);
    // Real deep copy into another panel; a group row copies its whole folder.
    void layerDuplicateToPanel(int index);
    bool duplicateLayerToPanelCore(int index, Panel *target);
    void layerUngroup(int groupIndex);     // dissolve a folder, keep members
    // Selection expanded so any selected group row brings its whole member
    // block along (delete/duplicate/move act on blocks).
    QVector<int> expandGroupBlocks(const QVector<int> &indices) const;
    // Every layer operation funnels its undo entry through here: snapshot
    // taken BEFORE the mutation, current state captured as "after".
    void pushLayerCommand(Panel *panel, const QVector<Layer> &before,
                          int beforeActive, const QString &text);
    // Delete confirmation (item: Delete/Cancel + "don't ask again" for
    // EMPTY layers only). True = go ahead.
    bool confirmLayerDelete(const QVector<int> &indices);
    void updateActiveLayerThumb(); // live row-thumbnail refresh while drawing
    // Selection-only refresh: restyle the EXISTING rows in place (slider,
    // buttons, canvas mirror included) — no widget teardown, so clicking
    // rows in a deep stack neither flickers nor re-scales every thumbnail.
    void updateLayerSelectionUi();
    // Panel-strip thumbnail: refreshCurrentThumb() debounces the expensive
    // flatten+scale behind a short timer (thumbnails settle after changes);
    // ...Now() does the actual work; flush runs a pending refresh early
    // (before panel switches).
    void refreshCurrentThumbNow();
    void flushThumbRefresh();

    void rebuildSceneList();
    void rebuildPanelStrip();
    // --- Panel Strip dock (top/bottom, resizable, floatable) --------------
    void setupPanelStripDock();     // wrap the strip bar in its QDockWidget
    void onPanelStripResized();     // live label rescale + debounced regen
    // Title-bar-less drag: a press on any BACKGROUND area of the strip
    // undocks/drags/docks it (top/bottom preview) or floats it; presses on
    // interactive children (thumbnails, buttons, scrollbars) pass through.
    bool stripBackgroundEvent(QObject *object, QEvent *event);
    bool stripHitIsInteractive(const QPoint &globalPos) const;
    void beginStripWindowDrag(const QPoint &globalPos);
    void updateStripWindowDrag(const QPoint &globalPos);
    void finishStripWindowDrag(const QPoint &globalPos);
    Qt::DockWidgetArea stripDropZone(const QPoint &globalPos) const;
    // Proportional scaling: ONE factor (strip height / 159, clamped 1..2)
    // drives buttons, icons, fonts, spacing, margins, and thumbnails.
    void applyStripScale();
    void applyPanelNumStyle(class QLabel *num) const;
    // Bucketed, cached, DPR-aware SVG rasterization for the strip buttons
    // (dprOverride > 0 forces a DPR — used by the HiDPI tests).
    QPixmap stripIconPixmap(const QString &svg, const QSize &logical,
                            qreal dprOverride = 0.0);
    // Re-render every strip thumbnail at the current thumb size and DPR
    // (dprOverride > 0 forces a specific DPR — used by the HiDPI tests).
    void regenerateStripThumbs(qreal dprOverride = 0.0);
    QPixmap stripThumbPixmap(Panel *panel, qreal dprOverride = 0.0) const;
    void savePanelStripState();     // versioned QSettings keys (v1)
    void restorePanelStripState();  // area/height/floating/geometry + clamp
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
    // true = the paste was refused (and the user told why) because the
    // clipboard panel's size differs from this project's. Paste paths only:
    // duplicate deliberately does not consult it.
    bool refuseMismatchedPaste();
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
    // Panel Strip dock: top/bottom only, height-resizable, floatable to any
    // monitor. Thumbnails scale with the strip height (m_thumbW/H replace the
    // old fixed 160x90); a resize drag rescales the existing pixmaps live and
    // regenerates crisp ones ONCE through m_stripRegenTimer.
    QDockWidget *m_panelStripDock = nullptr;
    QWidget *m_panelStripBar = nullptr;
    Qt::DockWidgetArea m_stripLastArea = Qt::TopDockWidgetArea;
    // The strip's LOGICAL visibility — what the user last chose. Qt unchecks
    // a dock's toggleViewAction on ANY hide event, including the ancestor
    // hide when another stack page becomes current, so the action's checked
    // state is NOT a record of user intent and must never be persisted.
    // The View-menu toggle is the strip's only deliberate show/hide control
    // (it has no close button), so only its triggered() updates this.
    bool m_stripUserVisible = true;
    QTimer *m_stripRegenTimer = nullptr;
    int m_stripRegenCount = 0; // regen passes (verified: 1 per settled drag)
    // Invalid until MainWindow supplies it; makeProjectPanel() refuses to
    // create a panel without it — never a 960x540 stand-in.
    QSize m_projectCanvasSize;
    int m_thumbW = 160;
    int m_thumbH = 90; // 16:9
    // Title-bar-less window drag state (background press-drag on the strip).
    QWidget *m_stripContainer = nullptr; // thumbnail row inside the scroll
    // Dock-target preview. A plain themed QWidget, NOT a QRubberBand:
    // the stock band paints from the system palette (the OS accent).
    QWidget *m_stripPreview = nullptr; // top/bottom dock-target preview
    bool m_stripDragCandidate = false;
    bool m_stripDragging = false;
    QPoint m_stripPressGlobal;
    QPoint m_stripDragOffset;
    // Proportional scale (1.0 at the 159px base strip, clamped to 2.0) and
    // the scaled controls: buttons registered with their base metrics.
    double m_stripScale = 1.0;
    struct StripCtl
    {
        QPushButton *button;
        QSize baseButton;
        QSize baseIcon;
        QString svg;
    };
    QVector<StripCtl> m_stripCtls;
    QWidget *m_stripCtlColumn = nullptr;
    QVBoxLayout *m_stripCtlLayout = nullptr;
    QHBoxLayout *m_stripBarLayout = nullptr;
    QHash<QString, QPixmap> m_stripIconCache; // svg|WxH@dpr -> pixmap
    int m_iconRasterCount = 0; // seam metric: bucketed rasterizations
    DrawingCanvas *m_canvas = nullptr;
    // The top-level window, watched for WindowBlocked/WindowUnblocked so the
    // floating tool windows hide while any modal dialog is open (they are
    // Qt::Tool windows owned by it and would otherwise paint over the modal).
    QPointer<QWidget> m_modalWatchWindow;
    ZoomToolbar *m_zoomToolbar = nullptr; // custom-painted view controls
    brushlib::BrushLibraryModel *m_brushLibModel = nullptr;
    brushlib::BrushLibraryPanel *m_brushLibPanel = nullptr;
    brushlib::BrushSettingsStudio *m_brushStudio = nullptr; // Figma 274:23
    QString m_activeBrushPresetId; // last library selection (dirty tracking)
    QPushButton *m_brushToolButton = nullptr; // Library anchor + open toggle
    QAction *m_brushLibViewAction = nullptr; // View menu toggle
    bool brushStudioUnderCursor() const; // Ctrl+Z routing (studio-local undo)
    void refreshBrushDirtyState();       // white dot + Reset chip on the row
    QScrollArea *m_panelScroll = nullptr;
    QPushButton *m_importButton = nullptr;
    // Floating Size CTL toolbar (Figma 209:42): size + opacity sliders and
    // Flip, snapping to the left/right canvas edge. (Brush Options is gone;
    // its pressure toggles were removed deliberately, and hardness is
    // edited in the Studio — its per-tool persistence lives on under
    // toolCtl/brush/hardness. See HANDOFF.md.)
    QWidget *m_sizeCtlBar = nullptr;
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
    QTimer *m_thumbTimer = nullptr; // debounces panel-thumbnail regeneration
    std::function<void(int)> m_syncLayerOpacity; // <0 disables; else sets %
    SankoSlider *m_layerOpacity = nullptr; // (unused; kept for ABI stability)
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
