#pragma once

#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QWidget>

struct Panel;
class QDragEnterEvent;
class QDropEvent;

// Freehand drawing surface for a single storyboard panel. Renders the panel's
// QPixmap scaled-to-fit (letterboxed) and edits it in place via mouse events.
class DrawingCanvas : public QWidget
{
    Q_OBJECT

public:
    enum Tool { Brush, Eraser, Line, Fill };

    explicit DrawingCanvas(QWidget *parent = nullptr);

    // Fixed 16:9 working resolution for every panel pixmap.
    static QSize canvasSize();

    void setActivePanel(Panel *panel);

    // Onion skin: a faint blue ghost of the previous panel, display-only.
    void setOnionSkinEnabled(bool enabled);
    bool isOnionSkinEnabled() const { return m_onionSkin; }
    // Pass the previous panel's pixmap (or a null pixmap to clear the ghost).
    void setPreviousPixmap(const QPixmap &previous);

    // Load an image file and paint it (scaled-to-fit, black padding) into the
    // current panel. Warns before replacing a non-blank drawing. Returns true
    // if the panel was changed. Shared by the button, shortcut, and drop.
    bool importImage(const QString &filePath);

public slots:
    void setTool(Tool tool);
    void setColor(const QColor &color);
    void setBrushSize(int size);
    void undo();
    void clearCanvas();

signals:
    void contentChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QRect displayRect() const;          // where the pixmap is drawn in the widget
    double scale() const;               // display px per pixmap px
    QPoint toPixmap(const QPoint &widgetPoint) const;
    int penWidth() const;               // brush size mapped into pixmap space
    void pushUndo();
    void drawSegment(const QPoint &from, const QPoint &to, const QColor &color);
    void floodFill(const QPoint &seed);

    Panel *m_panel = nullptr;
    Tool m_tool = Brush;
    QColor m_color = Qt::black;
    int m_brushSize = 4;

    bool m_drawing = false;
    QPoint m_lastPixmap;     // last freehand point, pixmap coords
    bool m_previewLine = false;
    QPoint m_lineStart;      // widget coords
    QPoint m_lineCurrent;    // widget coords

    bool m_onionSkin = false;
    QPixmap m_ghost;         // precomputed blue-tinted ghost (display only)
};
