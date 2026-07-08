#pragma once

#include <QColor>
#include <QCursor>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
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
    // ShapeKind). SelectRect/SelectEllipse/Lasso define a mask on the
    // ACTIVE layer (marching ants); Move drags the selected pixels within
    // that layer. Camera is a non-drawing tool: selecting it only opens the
    // Camera overlay panel; canvas clicks do nothing.
    enum Tool { Brush, Eraser, Shapes, Fill, SelectRect, SelectEllipse, Lasso, Move, Camera };

    // Shapes-tool primitives. Rectangle/Triangle/Circle/Line commit on drag
    // release; Polygon collects clicked vertices until double-click/Enter
    // closes it (Esc cancels).
    enum ShapeKind { ShapeRectangle, ShapeTriangle, ShapeCircle, ShapeLine, ShapePolygon };

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

    // Light alignment grid (View > Grid), display-only like the overlays.
    void setGridEnabled(bool enabled);
    bool isGridEnabled() const { return m_grid; }

public slots:
    void setTool(Tool tool);
    void setColor(const QColor &color);
    void setBrushSize(int size);
    void undo();
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

    // Selection + canvas clipboard (ACTIVE layer only). Copy reads even a
    // locked layer (not an edit); cut/paste/move require an editable one.
    bool hasSelection() const { return !m_selPath.isEmpty(); }
    bool hasCanvasClipboard() const { return !m_clipImg.isNull(); }
    void copySelection();                 // selected pixels -> internal clipboard
    void cutSelection();                  // copy, then clear to transparent (undoable)
    void pasteClipboard(bool atOriginalPos); // floating paste; commit on click-away/Enter
    void clearSelection();                // Esc equivalent

signals:
    void contentChanged();
    void layersChanged(); // layer added/removed by the canvas (image import)

protected:
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
    QRect displayRect() const;          // where the canvas is drawn (zoom + pan applied)
    double scale() const;               // display px per canvas px (includes zoom)
    QPoint toCanvas(const QPoint &widgetPoint) const;
    void setZoom(double zoom, const QPointF &anchorScreen); // keeps anchor point fixed
    void resetView();                   // 100%, recentred
    void updateZoomUi();                // refresh the percentage button text
    void positionZoomControls();        // pin the -/%/+ buttons to the corner
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

    // Selection / floating pixels (move-lift or un-committed paste).
    // The selection path is rasterized ONCE into a hard-edged mask; lift,
    // clear, copy, and cut all index that same mask so their coverage is
    // pixel-identical (never the bounding box for ellipse/lasso).
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
    bool m_selDrag = false;        // dragging out a new selection
    QPointF m_selStartC;
    QPointF m_selCurrentC;
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
    // source, never re-transformed) and shows a bounding box; every
    // move/scale/rotate re-renders the preview from m_transformBuf through a
    // fresh QTransform, so there is no cumulative resampling. The layer is
    // written exactly once on commit (Enter); Esc restores m_layerBackup.
    enum XformMode { XNone, XMove, XRotate,
                     XScaleTL, XScaleTR, XScaleBL, XScaleBR,
                     XScaleT, XScaleB, XScaleL, XScaleR };
    bool m_xformActive = false;
    QImage m_transformBuf;         // pristine lifted pixels (m_moveSrcRect-sized)
    QPointF m_boxCenter;           // current box centre, canvas coords
    qreal m_boxW = 0.0;            // current box width/height, canvas px
    qreal m_boxH = 0.0;
    qreal m_boxAngle = 0.0;        // current box rotation, degrees
    XformMode m_xformMode = XNone; // active handle while dragging
    QPointF m_dragStartCanvas;     // pointer at press, canvas coords
    QPointF m_boxCenter0;          // box snapshot at press (transforms recompute from these)
    qreal m_boxW0 = 0.0, m_boxH0 = 0.0, m_boxAngle0 = 0.0;
    qreal m_rotStart0 = 0.0;       // pointer angle at a rotate press, radians
    QCursor m_rotateCursor;        // curved-arrow cursor for the rotation zones

    void beginTransform();         // lift selection -> box (source cleared on lift)
    void commitTransform();        // bake once (one undo), clear box
    void cancelTransform();        // restore m_layerBackup, discard transform
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
    QPushButton *m_zoomOutButton = nullptr;
    QPushButton *m_zoomResetButton = nullptr; // shows "100%", click resets view
    QPushButton *m_zoomInButton = nullptr;

    // Display-only overlays.
    bool m_grid = false;         // alignment grid, 40 canvas px (View > Grid)
    bool m_cameraFrame = true;   // 16:9 framing + dim outside (ON by default)
    bool m_safeArea = false;     // action safe, 5% inset
    bool m_titleSafe = false;    // title safe, 10% inset
    int m_actionSafeMaskPct = 50; // guide opacity, percent (persisted)
    int m_titleSafeMaskPct = 50;
};
