#pragma once

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
                SelectPoly, Move, Camera };

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

public slots:
    void setTool(Tool tool);
    void setColor(const QColor &color);
    void setBrushSize(int size);
    void undo();   // drawing history (Ctrl+Z / toolbar Undo)
    void redo();   // drawing history (Ctrl+Y / toolbar Redo)
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
    bool hasSelection() const { return !m_selPath.isEmpty(); }
    bool hasCanvasClipboard() const { return !m_clipImg.isNull(); }
    void copySelection();                 // selected pixels -> internal clipboard
    void cutSelection();                  // copy, then clear to transparent (undoable)
    void pasteClipboard(bool atOriginalPos); // floating paste; commit on click-away/Enter
    void clearSelection();                // Esc equivalent
    void deselect();                      // clearSelection + selection-history entry
    void selectAll();                     // select the whole canvas
    void invertSelection();               // canvas minus current selection

    // SELECTION history — fully separate from the drawing undo/redo above.
    // Records selection-region changes only (new/adjusted outlines, select
    // all, inverse, deselect, outline moves); never touches layer pixels.
    void undoSelection();
    void redoSelection();

signals:
    void contentChanged();
    void layersChanged(); // layer added/removed by the canvas (image import)
    void viewZoomChanged(double zoom); // wheel/pan zoom -> sync the zoom slider

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
    void pushUndo();
    void drawSegment(const QPoint &from, const QPoint &to, const QColor &color);
    void floodFill(const QPoint &seed);

    // Shapes tool: preview geometry lives in canvas coords until committed.
    void paintShapeGeometry(QPainter &painter, bool closePolygon) const;
    void commitDragShape(); // rectangle/triangle/circle/line, on release
    void commitPolygon();   // on double-click/Enter; needs >= 3 vertices
    void cancelShape();     // clears drag + polygon state, no artifacts
    void closePolygonSelection(); // SelectPoly: vertices -> selection mask
    QPainterPath combinedSelection(const QPainterPath &shape) const; // Replace/Add/Sub

    // Selection / floating pixels (move-lift or un-committed paste).
    // The selection path is rasterized ONCE into a mask; lift, clear, copy,
    // and cut all index that same mask so their coverage is pixel-identical
    // (never the bounding box for ellipse/lasso). Hard-edged by default;
    // antialiased=true gives a soft 1px coverage falloff (the transform box
    // uses it so moved artwork keeps clean edges). Either way DestinationIn
    // (keep) and DestinationOut (clear) with the SAME mask partition the
    // pixels exactly: alpha + (1-alpha) sums back to the original.
    QImage selectionMask(const QRect &boundingRect, bool antialiased = false) const;
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
    void endBrushStroke();
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

    // Selection state (mask on the ACTIVE layer, canvas coords).
    QPainterPath m_selPath;        // empty = no selection
    QPainterPath m_selBase;        // selection before the current Add/Sub drag
    SelectionOp m_selOp = SelReplace;
    bool m_selDrag = false;        // dragging out a new selection
    QPointF m_selStartC;
    QPointF m_selCurrentC;
    // Selection Modifier "Move" (outline only, never pixels).
    bool m_selOutlineMove = false; // the toolbar's Move mode is on
    bool m_selOutlineDrag = false; // dragging the outline right now
    QPointF m_selOutlineStartC;    // canvas point where the drag grabbed
    QPainterPath m_selOutlineBase; // m_selPath at drag start (translated live)
    // SELECTION history (separate from the drawing undo/redo, which lives on
    // the Panel). One entry = the selection region before a committed change.
    void recordSelectionChange(const QPainterPath &before);
    QVector<QPainterPath> m_selUndoStack;
    QVector<QPainterPath> m_selRedoStack;
    QPainterPath m_selGestureBase; // selection at gesture start (drag/polygon)
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
    QCursor m_rotateCursor;        // curved-arrow cursor for the rotation zones
    QPointF pivotPoint() const;    // m_pivot when custom, else the quad centre

    // Warp mode: an IRREGULAR mesh of draggable control points that locally
    // deform the buffer. Each point pairs a fixed SOURCE position (buffer
    // coords) with a draggable DESTINATION (canvas coords); the points are
    // Delaunay-triangulated in source space (topology is drag-invariant) and
    // rendered as piecewise affine triangles. Seeded as a kWarpGrid lattice;
    // Ctrl+click adds/removes points (the 4 source-corner anchors are
    // protected so the triangulation always covers the whole buffer). Quad
    // edits in the other modes carry every destination along through the
    // incremental quad transform. All guides are display-only overlays.
    struct WarpPt {
        QPointF src; // buffer coords, fixed after creation
        QPointF dst; // canvas coords, dragged by the user
    };
    static constexpr int kWarpGrid = 4;
    QVector<WarpPt> m_warp;        // control points
    QVector<WarpPt> m_warp0;       // snapshot at press
    QVector<int> m_warpTris;       // index triples into m_warp (src Delaunay)
    QSet<int> m_warpSel;           // selected control points (multi-select)
    bool m_warpDirty = false;      // a control point was moved: render the mesh
    mutable int m_warpIdx = -1;    // control point under the cursor / dragged
    bool m_warpMarquee = false;    // rubber-band point selection in progress
    QPointF m_marqueeStartW;       // marquee corners, WIDGET coords
    QPointF m_marqueeEndW;
    void rebuildWarpTriangulation();              // src-space Delaunay
    int warpPointAt(const QPointF &widgetPos) const;
    void addWarpPointAt(const QPointF &widgetPos);   // Ctrl+click on the mesh
    void removeWarpPoint(int index);                 // Ctrl+click on a point
    void paintWarpedBuffer(QPainter &p) const;    // piecewise triangles
    qreal luminanceBehind(const QPointF &canvasPt) const; // pivot contrast

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

    // Display-only overlays.
    bool m_grid = false;         // alignment grid, 40 canvas px (View > Grid)
    bool m_cameraFrame = true;   // 16:9 framing + dim outside (ON by default)
    bool m_safeArea = false;     // action safe, 5% inset
    bool m_titleSafe = false;    // title safe, 10% inset
    int m_actionSafeMaskPct = 50; // guide opacity, percent (persisted)
    int m_titleSafeMaskPct = 50;
};
