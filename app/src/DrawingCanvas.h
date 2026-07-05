#pragma once

#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QWidget>

struct Layer;
struct Panel;
class QDragEnterEvent;
class QDropEvent;
class QPushButton;

// Freehand drawing surface for a single storyboard panel. Composites the
// panel's layer stack scaled-to-fit (letterboxed) and edits the ACTIVE layer's
// QImage in place via mouse events.
class DrawingCanvas : public QWidget
{
    Q_OBJECT

public:
    // Brush (the stamp-based pressure engine) is the single drawing tool;
    // pen-like behaviour is a brush preset. Eraser/Line keep the classic
    // QPainter stroke path.
    enum Tool { Brush, Eraser, Line, Fill };

    explicit DrawingCanvas(QWidget *parent = nullptr);

    // Fixed 16:9 working resolution for every panel layer.
    static QSize canvasSize();

    void setActivePanel(Panel *panel);

    // Onion skin: a faint blue ghost of the previous panel, display-only.
    void setOnionSkinEnabled(bool enabled);
    bool isOnionSkinEnabled() const { return m_onionSkin; }
    // Pass the previous panel's flattened pixmap (or null to clear the ghost).
    void setPreviousPixmap(const QPixmap &previous);

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

signals:
    void contentChanged();
    void layersChanged(); // layer added/removed by the canvas (image import)

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
    bool m_previewLine = false;
    QPoint m_lineStart;      // widget coords
    QPoint m_lineCurrent;    // widget coords

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
    bool m_cameraFrame = true;   // 16:9 framing + dim outside (ON by default)
    bool m_safeArea = false;     // action safe, 5% inset
    bool m_titleSafe = false;    // title safe, 10% inset
};
