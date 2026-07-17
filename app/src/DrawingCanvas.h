#pragma once

#include "PerspectiveTool.h"

#include <QuickShape/quickshape_session.h>

#include <QJsonObject>

#include <QColor>
#include <QCursor>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QSet>
#include <QTransform>
#include <QPoint>
#include <QPointF>
#include <QVector>
#include <QWidget>

struct Layer;
struct Panel;
class QDragEnterEvent;
class QUndoStack;
class QDropEvent;
class QPushButton;
class QSlider;
class QTimer;

// Freehand drawing surface for a single storyboard panel. Composites the
// panel's layer stack scaled-to-fit (letterboxed) and edits the ACTIVE layer's
// QImage in place via mouse events.
class DrawingCanvas : public QWidget
{
    Q_OBJECT

public:
    // Brush (the stamp-based pressure engine) is the single drawing tool;
    // pen-like behaviour is a brush preset. Eraser keeps the classic
    // QPainter stroke path. Shapes stamps geometric primitives (see
    // ShapeKind). SelectRect/SelectEllipse/Lasso/SelectPoly define a mask on
    // the ACTIVE layer (marching ants); Move drags the selected pixels within
    // that layer. Camera is a non-drawing tool: selecting it only opens the
    // Camera overlay panel; canvas clicks do nothing.
    //   Lasso     = freehand: drag to trace the selection boundary.
    //   SelectPoly = polygon:  click to drop vertices, double-click/Enter to
    //                close them into the selection (Esc cancels).
    enum Tool { Brush, Eraser, Shapes, Fill, SelectRect, SelectEllipse, Lasso,
                SelectPoly, Move, Camera, Perspective };

    // Shapes-tool primitives. Rectangle/Triangle/Circle/Line commit on drag
    // release; Polygon collects clicked vertices until double-click/Enter
    // closes it (Esc cancels).
    enum ShapeKind { ShapeRectangle, ShapeTriangle, ShapeCircle, ShapeLine, ShapePolygon };

    // How a freshly-drawn selection shape combines with the existing one; the
    // Selection Modifier toolbar's Add / Remove buttons drive this.
    enum SelectionOp { SelReplace, SelAdd, SelSubtract };

    // Move tool transform-box interaction mode; the Move Modifier toolbar's
    // Pivot Point / Skew / Distort / Perspective / Warp buttons drive this.
    // Default = the classic move/scale/rotate box. Switching modes keeps the
    // same live transform session (commit/cancel ends it).
    enum XformUiMode { XformDefault, XformPivot, XformSkew, XformDistort,
                       XformPerspective, XformWarp };

    explicit DrawingCanvas(QWidget *parent = nullptr);

    // Fixed 16:9 working resolution for every panel layer.
    static QSize canvasSize();

    void setActivePanel(Panel *panel);

    // Onion skin: a faint blue ghost of the previous panel, display-only.
    void setOnionSkinEnabled(bool enabled);
    bool isOnionSkinEnabled() const { return m_onionSkin; }
    // Pass the previous panel's flattened pixmap (or null to clear the ghost).
    void setPreviousPixmap(const QPixmap &previous);

    // Light table: fuller onion skin showing neighbouring panels behind the
    // current drawing — previous tinted red, next tinted green. Display-only
    // (never written to any layer, flattenedPixmap, export, or save).
    void setLightTableEnabled(bool enabled);
    bool isLightTableEnabled() const { return m_lightTable; }
    // Raw flattened pixmaps of the previous / next panel (null = none). Tinted
    // ghosts are built internally.
    void setLightTablePixmaps(const QPixmap &previous, const QPixmap &next);

    // Import an image file as a NEW image-type layer above the active layer
    // (scaled to fit the canvas, transparent padding). Returns true on success.
    // Shared by the button, Ctrl+I shortcut, and file drop.
    bool importImage(const QString &filePath);

    // Viewport overlays (display-only, never in flattenedPixmap()).
    void setCameraFrameEnabled(bool enabled);
    void setSafeAreaEnabled(bool enabled);
    void setTitleSafeEnabled(bool enabled);
    bool isCameraFrameEnabled() const { return m_cameraFrame; }
    bool isSafeAreaEnabled() const { return m_safeArea; }
    bool isTitleSafeEnabled() const { return m_titleSafe; }

    // Safe-area guide opacities in percent (Preferences > Camera). Defaults
    // are loaded from QSettings("SankoTV","SankoTV") in the constructor.
    void setActionSafeMaskOpacity(int percent); // action-safe amber
    void setTitleSafeMaskOpacity(int percent);  // title-safe blue

    // Canvas VIEW transforms (display only; never touch layer pixels/export).
    void setViewZoom(double zoom);        // 0.25..4, centred on the view
    void setViewRotation(double degrees); // -180..180, around the view centre
    void toggleFlipH();                   // horizontal flip of the view
    void resetViewRotation();             // rotation back to 0
    double viewZoom() const { return m_zoom; }
    double viewRotation() const { return m_viewRotation; }
    bool viewFlipH() const { return m_viewFlipH; }

    // Light alignment grid (View > Grid), display-only like the overlays.
    void setGridEnabled(bool enabled);
    bool isGridEnabled() const { return m_grid; }

    // Perspective guides + snap (display-only overlay; the settings panel and
    // project save/load talk to the same instance). Call update() after edits.
    PerspectiveTool *perspective() { return &m_perspective; }

    // QuickShape: hold-to-shape assist for the Brush tool. The session sees
    // the same canvas-space samples the brush engine paints; a recognized
    // shape becomes a temporary vector overlay that is only rasterized by
    // replaying it through the normal brush engine (one undo entry).
    void setQuickShapeEnabled(bool enabled);
    bool quickShapeEnabled() const { return m_quickShapeEnabled; }
    quickshape::QuickShapeSession *quickShape() { return &m_quickShape; }
    void commitQuickShape(); // resolve any pending temporary shape (lifecycle)
    void cancelQuickShape(); // explicit cancel: discard the temporary vector

public slots:
    void setTool(Tool tool);
    void setColor(const QColor &color);
    void setBrushSize(int size);
    void undo();   // app-wide history (Ctrl+Z / toolbar Undo)
    void redo();   // app-wide history (Ctrl+Y / toolbar Redo)
    void clearCanvas();

    // Brush engine settings (the stamp-based Brush tool only).
    void setBrushToolSize(int px);       // 1..200, canvas pixels
    void setBrushOpacity(int percent);   // 0..100
    void setBrushHardness(int percent);  // 0..100 (100 = sharp edge)
    void setPressureToSize(bool on);
    void setPressureToOpacity(bool on);

    // Shapes tool settings (Shapes options panel).
    void setShapeKind(ShapeKind kind);   // cancels any in-progress shape
    void setShapeStrokeWidth(int px);    // 1..100, canvas pixels
    void setShapeFill(bool on);          // filled with the current colour

    void setSelectionOp(SelectionOp op);
    SelectionOp selectionOp() const { return m_selOp; }

    // Move Modifier toolbar: transform-box interaction mode (see XformUiMode).
    void setXformUiMode(XformUiMode mode);
    XformUiMode xformUiMode() const { return m_xformUiMode; }

    // Selection Modifier "Move": while on, dragging with a selection tool
    // translates ONLY the selection outline (marching ants) — artwork pixels
    // are never lifted or altered.
    void setSelectionOutlineMove(bool on);
    bool selectionOutlineMove() const { return m_selOutlineMove; }

    // Selection + canvas clipboard (ACTIVE layer only). Copy reads even a
    // locked layer (not an edit); cut/paste/move require an editable one.
    bool hasSelection() const { return !m_selectionPath.isEmpty(); }
    bool hasCanvasClipboard() const { return !m_clipImg.isNull(); }
    void copySelection();                 // selected pixels -> internal clipboard
    void cutSelection();                  // copy, then clear to transparent (undoable)
    void pasteClipboard(bool atOriginalPos); // floating paste; commit on click-away/Enter
    void clearSelection();                // Esc equivalent
    void deselect();                      // clearSelection + selection-history entry
    void selectAll();                     // select the whole canvas
    void invertSelection();               // canvas minus current selection

    // App-wide undo plumbing. The shared QUndoStack (owned by MainWindow)
    // receives every mutating action as a QUndoCommand; the commands call
    // back into these apply methods.
    void setUndoStack(QUndoStack *stack) { m_undoStack = stack; }
    void applyLayerRegionForUndo(Panel *panel, const QString &layerId,
                                 const QRect &region, const QImage &pixels);
    void applySelectionPathForUndo(const QPainterPath &path);

    // Perspective undo plumbing: every completed perspective gesture (VP
    // create/move/delete, settings edit) pushes ONE command holding full
    // before/after model snapshots. begin/end bracket toolbar slider drags.
    void applyPerspectiveForUndo(const QJsonObject &state);
    void beginPerspectiveEdit();
    void endPerspectiveEdit(const QString &text);

signals:
    void contentChanged();
    void layersChanged(); // layer added/removed by the canvas (image import)
    void viewZoomChanged(double zoom); // wheel/pan zoom -> sync the zoom slider
    // Committing a Warp resets the box to the default move/scale/rotate mode;
    // the Move Modifier toolbar listens and unchecks its mode buttons.
    void xformUiModeReset();
    // An undo/redo command rewrote this panel's layer data; the page
    // regenerates that panel's thumbnail from the committed model.
    void panelEdited(Panel *panel);
    // A perspective VP was created, selected, or removed: the Perspective
    // Modifier toolbar re-syncs its per-VP sliders.
    void perspectiveEdited();

protected:
    bool eventFilter(QObject *object, QEvent *event) override; // toolbar grip drag
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override; // closes a polygon
    void tabletEvent(QTabletEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QRect displayRect() const;          // axis-aligned zoom+pan rect (pre-rotation)
    double scale() const;               // display px per canvas px (includes zoom)
    QTransform viewTransform() const;   // canvas -> widget (zoom+pan+rotate+flip)
    QPoint toCanvas(const QPoint &widgetPoint) const;
    void setZoom(double zoom, const QPointF &anchorScreen); // keeps anchor point fixed
    void resetView();                   // 100%, recentred
    int penWidth() const;               // brush size mapped into canvas space
    Layer *editableActiveLayer() const; // active layer if it accepts strokes, else nullptr
    // App-wide undo: snapshot the active layer before a mutating operation,
    // then diff and push ONE region-limited DrawingCommand when it finishes.
    void beginLayerEdit();
    void finalizeLayerEdit(const QString &text,
                           const QImage &beforeOverride = QImage());
    void drawSegment(const QPoint &from, const QPoint &to, const QColor &color);
    void floodFill(const QPoint &seed);

    // Shapes tool: preview geometry lives in canvas coords until committed.
    void paintShapeGeometry(QPainter &painter, bool closePolygon) const;
    void commitDragShape(); // rectangle/triangle/circle/line, on release
    void commitPolygon();   // on double-click/Enter; needs >= 3 vertices
    void cancelShape();     // clears drag + polygon state, no artifacts
    void closePolygonSelection(); // SelectPoly: vertices -> selection mask
    QPainterPath combinedSelection(const QPainterPath &shape) const; // Replace/Add/Sub

    // Selection rasterisation. The selection itself is VECTOR geometry
    // (m_selectionPath); a grayscale ANTIALIASED coverage mask is generated
    // from it only when a pixel operation needs one (brush, erase, fill,
    // lift, clear, copy/cut). cachedSelectionMask() is the full-canvas mask,
    // rebuilt lazily ONLY when the path has changed — a stroke of hundreds of
    // dabs rasterises the path exactly once; selectionMask() crops it.
    // DestinationIn with the mask KEEPS exactly the masked coverage and
    // DestinationOut CLEARS exactly the complement — alpha + (1-alpha)
    // partitions the pixels exactly, so cut/lift edges stay clean.
    const QImage &cachedSelectionMask() const;
    QImage selectionMask(const QRect &boundingRect) const;
    // Clamp used only to place a fresh PASTE fully on-canvas (positioning,
    // not clipping).
    QPointF clampFloatDelta(const QPointF &delta) const;
    // Move tool, deferred-commit model: the layer is written exactly ONCE,
    // on mouse up. beginMoveDrag copies the masked pixels + snapshots the
    // whole layer; commitMoveDrag rebuilds from that snapshot (clear source,
    // paste at the final offset) and pushes ONE undo entry.
    void beginMoveDrag(const QPointF &grabCanvasPt);
    void commitMoveDrag();
    void commitFloating();      // paste-floating commit (click-away/Enter)
    void cancelFloatingPaste(); // Esc on a floating paste: discard, no artifacts
    QRectF floatBounds() const; // floating image bounds in canvas coords
    void updateAntsTimer();     // marching ants animate only while needed

    // Stamp-based brush stroke pipeline (mouse pressure = 1.0; tablet = real).
    QPointF toCanvasF(const QPointF &widgetPoint) const; // float, unclamped
    void beginBrushStroke(const QPointF &canvasPt, qreal pressure);
    void moveBrushStroke(const QPointF &canvasPt, qreal pressure);
    void endBrushStroke(const QString &undoText = QStringLiteral("Brush Stroke"));
    void stampDab(const QPointF &center, qreal pressure);

    Panel *m_panel = nullptr;
    Tool m_tool = Brush;
    QColor m_color = Qt::black;
    int m_brushSize = 4;

    bool m_drawing = false;
    QPoint m_lastCanvas;     // last freehand point, canvas coords

    // Shapes tool state. Preview-only until committed to the active layer.
    ShapeKind m_shapeKind = ShapeRectangle;
    int m_shapeStroke = 4;         // stroke width, canvas px (1..100)
    bool m_shapeFill = false;      // OFF = outline only
    bool m_shapeDrag = false;      // drag-defined shape in progress
    QPointF m_shapeStartC;         // canvas coords
    QPointF m_shapeCurrentC;       // canvas coords (drag end / polygon rubber)
    QVector<QPointF> m_polygonPts; // in-progress polygon vertices, canvas coords

    // Selection state: resolution-independent VECTOR geometry on the ACTIVE
    // layer (canvas coords). Rasterised lazily via cachedSelectionMask().
    QPainterPath m_selectionPath;        // empty = no selection
    mutable QImage m_selMaskCache;       // AA coverage, canvas-sized, lazy
    mutable QPainterPath m_selMaskPath;  // the path the cache was built from
    QPainterPath m_selBase;        // selection before the current Add/Sub drag
    SelectionOp m_selOp = SelReplace;
    bool m_selDrag = false;        // dragging out a new selection
    QPointF m_selStartC;
    QPointF m_selCurrentC;
    // Selection Modifier "Move" (outline only, never pixels).
    bool m_selOutlineMove = false; // the toolbar's Move mode is on
    bool m_selOutlineDrag = false; // dragging the outline right now
    QPointF m_selOutlineStartC;    // canvas point where the drag grabbed
    QPainterPath m_selOutlineBase; // m_selectionPath at drag start (translated live)
    // Selection changes push SelectionCommands onto the shared stack.
    void recordSelectionChange(const QPainterPath &before);
    QPainterPath m_selGestureBase; // selection at gesture start (drag/polygon)
    // Shared app-wide undo stack (owned by MainWindow) + the in-flight layer
    // edit snapshot captured by beginLayerEdit().
    QUndoStack *m_undoStack = nullptr;
    Panel *m_editPanel = nullptr;
    QString m_editLayerId;
    QImage m_editBefore;
    QVector<QPointF> m_lassoPts;   // freehand outline while dragging
    QTimer *m_antsTimer = nullptr; // marching-ants animation
    int m_antsPhase = 0;

    // Floating pixels: a move drag (commits on release) or an un-committed
    // paste (commits on click-away/Enter). Display-only until committed.
    bool m_floatActive = false;
    bool m_floatFromPaste = false; // paste floats until click-away; move doesn't
    bool m_floatDragging = false;
    QImage m_floatImg;             // complete masked pixels (never cropped)
    QPointF m_floatPos;            // top-left, canvas coords
    QPointF m_floatDelta;          // drag offset since lift/paste
    QPointF m_floatGrabC;          // canvas point where the drag grabbed
    QPointF m_floatGrabDelta;      // m_floatDelta at grab time

    // Move tool deferred-commit state. During the drag the layer is NEVER
    // written; mouse up rebuilds it once from m_layerBackup.
    bool m_moveActive = false;     // between Move mouse-down and mouse-up
    QImage m_layerBackup;          // pristine full copy of the layer at move start
    QImage m_moveMask;             // selection mask, bounding-rect-local
    QRect m_moveSrcRect;           // selection bounding rect at move start

    // Non-destructive transform box (Move tool). Activating Move with a
    // selection lifts the masked pixels into m_transformBuf (the PRISTINE
    // source, never re-transformed) and shows a bounding box; every edit
    // re-renders the preview from m_transformBuf through a fresh QTransform,
    // so there is no cumulative resampling. The layer is written exactly once
    // on commit (Enter); Esc restores m_layerBackup.
    //
    // The box is a free QUAD (corners TL,TR,BR,BL in canvas coords) mapped
    // from the source rect with QTransform::quadToQuad — affine for
    // move/scale/rotate/skew, projective for distort/perspective. m_pivot is
    // the rotation/scale origin (Pivot Point mode relocates it; until then it
    // tracks the quad centre).
    enum XformMode { XNone, XMove, XRotate, XPivot, XWarpPt, XWarpBox,
                     XScaleTL, XScaleTR, XScaleBL, XScaleBR,
                     XScaleT, XScaleB, XScaleL, XScaleR };
    bool m_xformActive = false;
    bool m_xformAutoSel = false;   // box selection was synthesized from the
                                   // layer's artwork bounds (no user selection)
    QImage m_transformBuf;         // pristine lifted pixels (m_moveSrcRect-sized)
    QPolygonF m_quad;              // current box corners TL,TR,BR,BL, canvas coords
    QPointF m_pivot;               // rotate/scale origin (Pivot Point mode)
    bool m_pivotCustom = false;    // user moved the pivot off the box centre
    XformUiMode m_xformUiMode = XformDefault;
    XformMode m_xformMode = XNone; // active handle while dragging
    QPointF m_dragStartCanvas;     // pointer at press, canvas coords
    QPolygonF m_quad0;             // quad snapshot at press (drags recompute from it)
    QPointF m_pivot0;              // pivot snapshot at press
    qreal m_rotStart0 = 0.0;       // pointer angle at a rotate press, radians
    QCursor m_rotateCursors[4];    // curved-arrow cursors, oriented per corner
                                   // (TL, TR, BR, BL — each bends around its
                                   // own corner like Photoshop's)
    mutable int m_rotateCorner = 0; // corner whose rotate zone is hovered
    QPointF pivotPoint() const;    // m_pivot when custom, else the quad centre

    // Warp mode: an IRREGULAR mesh of draggable control points that smoothly
    // deform the buffer. Each point pairs a fixed SOURCE position (buffer
    // coords) with a draggable DESTINATION (canvas coords). The deformation
    // is a THIN-PLATE SPLINE interpolating the control points — globally
    // smooth (C1+), so clean line art stays smooth with no polygon faceting.
    // Rendering samples the TPS over a dense subdivided grid (fine cells:
    // sub-pixel deviation) drawn as seam-free micro-triangles. Seeded as a
    // kWarpGrid lattice; Ctrl+click adds/removes points (the 4 source-corner
    // anchors are protected). Quad edits in the other modes carry every
    // destination along through the incremental quad transform. All guides
    // are display-only overlays and never bake into the layer.
    struct WarpPt {
        QPointF src; // buffer coords, fixed after creation
        QPointF dst; // canvas coords, dragged by the user
    };
    static constexpr int kWarpGrid = 4;
    QVector<WarpPt> m_warp;        // control points
    QVector<WarpPt> m_warp0;       // snapshot at press
    QSet<int> m_warpSel;           // selected control points (multi-select)
    bool m_warpDirty = false;      // a control point was moved: render the mesh
    mutable int m_warpIdx = -1;    // control point under the cursor / dragged
    int m_warpHoverIdx = -1;       // hovered point (drawn slightly enlarged)
    bool m_warpMarquee = false;    // rubber-band point selection in progress
    QPointF m_marqueeStartW;       // marquee corners, WIDGET coords
    QPointF m_marqueeEndW;
    // Thin-plate-spline solve of src -> dst (weights per dimension, laid out
    // [w0..wN-1, a0, ax, ay]); recomputed on every mesh change.
    QVector<qreal> m_tpsX, m_tpsY;
    bool m_tpsValid = false;
    void solveWarpTps();                          // fit the spline to m_warp
    QPointF warpMap(const QPointF &src) const;    // evaluate the spline
    int warpPointAt(const QPointF &widgetPos) const;
    void addWarpPointAt(const QPointF &widgetPos);   // Ctrl+click on the mesh
    void removeWarpPoint(int index);                 // Ctrl+click on a point
    // Piecewise render of the TPS at the given source-space cell size (finer
    // for the commit than the interactive preview).
    void paintWarpedBuffer(QPainter &p, qreal cellPx) const;
    qreal luminanceBehind(const QPointF &canvasPt) const; // pivot contrast

    void liftDefaultTransformBox(); // Move tool: box around selection/artwork
    void beginTransform();         // lift selection -> box (source cleared on lift)
    // Bake once (one undo). relift: Photoshop-style — while the Move tool
    // stays active the box does not disappear; it RESETS to a fresh default
    // axis-aligned box around the committed artwork. setTool passes false
    // when the user is leaving Move.
    void commitTransform(bool relift = true);
    // Restore m_layerBackup exactly. relift (Esc): keep a fresh default box
    // up while the Move tool remains active.
    void cancelTransform(bool relift = false);
    QTransform boxTransform() const;                 // buffer -> canvas placement
    QVector<QPointF> boxHandlesCanvas() const;       // 8 handles, canvas coords
    XformMode hitTestBox(const QPointF &widgetPos) const;
    void applyXformDrag(const QPointF &canvasPos, bool proportional);
    void updateXformCursor(XformMode mode);

    // Visual debug: dump the move-pipeline stages to C:\SankoTv\app\debug\.
    // Off in shipped builds; flip to true to re-inspect the move stages.
    bool m_debugMove = false;
    void dumpMoveDebug(const QString &name, const QImage &img, bool checker) const;

    // Canvas clipboard (Edit menu Copy/Cut/Paste on the selection).
    QImage m_clipImg;              // copied pixels, tight bounding rect
    QPointF m_clipPos;             // canvas position they came from

    // Stroke-level selection masking: while a selection is active, the live
    // brush/eraser stroke accumulates UNMASKED in a scratch buffer and the
    // cached antialiased mask is applied ONCE (composited each repaint for
    // the preview, baked into the layer on release). The mask is therefore a
    // CEILING the stroke can never exceed — overlapping dabs cannot saturate
    // the soft boundary back to a hard edge (Photoshop semantics).
    enum StrokeMaskMode { StrokeMaskNone, StrokeMaskPaint, StrokeMaskErase };
    StrokeMaskMode m_strokeMask = StrokeMaskNone;
    QImage m_strokeBuf; // canvas-sized scratch holding the live stroke

    // Brush engine state. Defaults mirror the initial settings-panel values.
    int m_brushToolSize = 25;        // dab diameter, canvas px
    double m_brushToolOpacity = 1.0; // 0..1
    double m_brushHardness = 0.8;    // 0..1
    bool m_pressureToSize = true;
    bool m_pressureToOpacity = false;
    bool m_brushStroke = false;      // brush stroke in progress
    QPointF m_lastBrushPt;           // canvas coords, float
    qreal m_lastBrushPressure = 1.0;
    double m_stampResidual = 0.0;    // distance travelled since the last dab

    bool m_onionSkin = false;
    QPixmap m_ghost;         // precomputed blue-tinted ghost (display only)

    // Light table: red-tinted previous + green-tinted next ghosts, drawn on
    // the paper behind the current drawing layers (display only, session state).
    bool m_lightTable = false;
    QPixmap m_ltPrevGhost;   // previous panel, tinted red
    QPixmap m_ltNextGhost;   // next panel, tinted green
    void drawLightTable(QPainter &painter, const QRect &d) const;

    // Viewport: zoom + pan.
    double m_zoom = 1.0;         // 0.25 .. 4.0
    QPointF m_panOffset;         // screen-px offset from the centred position
    bool m_spaceHeld = false;    // spacebar pan modifier
    bool m_panning = false;      // mid-drag (space+left or middle button)
    QPoint m_panStartScreen;
    QPointF m_panStartOffset;

    // Canvas VIEW transforms (display only — the layer pixels, flattenedPixmap,
    // save, and export are never rotated/flipped/zoomed). Applied as one
    // QTransform in paintEvent; mouse coords invert it (see viewTransform()).
    double m_viewRotation = 0.0; // degrees, -180..180
    bool m_viewFlipH = false;    // horizontal flip
    // Canvas View Controls toolbar (grip, zoom slider, flip, rotate slider,
    // reset rotation), floating over the canvas.
    QWidget *m_viewToolbar = nullptr;
    QWidget *m_viewGrip = nullptr;
    QSlider *m_zoomSlider = nullptr;
    QSlider *m_rotateSlider = nullptr;
    bool m_syncingViewUi = false; // guards slider<->engine feedback
    bool m_viewDragging = false;  // dragging the toolbar by its grip
    QPoint m_viewDragStart;       // global cursor at grip press
    QPoint m_viewToolbarStart;    // toolbar pos at grip press
    void buildViewToolbar();
    void positionViewToolbar();
    void syncViewToolbar();       // push engine state into the sliders

    // Perspective guides (display-only; geometry in canvas coords).
    PerspectiveTool m_perspective;
    int m_perspHandle = -1; // VP index being dragged (-1: none)

    // QuickShape session state (recognition math lives in the kit; the canvas
    // owns input feeding, the rough-stroke rollback, overlay painting, and
    // the brush-engine replay).
    quickshape::QuickShapeSession m_quickShape;
    QPainterPath m_quickShapeOverlay; // corrected vector, canvas coords
    bool m_quickShapeEnabled = true;
    qreal quickShapeDwellRadius() const;  // ~8 screen px in canvas units
    void discardRoughStroke();            // roll the pending edit back
    void replayQuickShape(const quickshape::QuickShapeCommit &commit);
    int m_perspHover = -1;  // VP handle under the idle cursor (grows on hover)
    // One undo command per completed gesture: snapshot at press, push at
    // release (or at begin/endPerspectiveEdit for toolbar edits).
    QJsonObject m_perspBefore;
    bool m_perspGesture = false;
    QString m_perspGestureText;
    void pushPerspectiveCommand(const QJsonObject &before, const QString &text);

    // Display-only overlays.
    bool m_grid = false;         // alignment grid, 40 canvas px (View > Grid)
    bool m_cameraFrame = true;   // 16:9 framing + dim outside (ON by default)
    bool m_safeArea = false;     // action safe, 5% inset
    bool m_titleSafe = false;    // title safe, 10% inset
    int m_actionSafeMaskPct = 50; // guide opacity, percent (persisted)
    int m_titleSafeMaskPct = 50;
};
