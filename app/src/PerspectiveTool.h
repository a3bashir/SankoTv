#pragma once

#include <QColor>
#include <QPointF>
#include <QRectF>

class QJsonObject;
class QPainter;
class QTransform;

// Adjustable 1/2/3-point perspective guides with optional stroke snapping.
//
// Pure model + geometry: DrawingCanvas owns one instance, renders the guides
// as a DISPLAY-ONLY overlay in canvas space (never into any layer, never in
// flattenedPixmap or exports), routes handle drags to it while the
// Perspective tool is active, and asks it to constrain stroke points while
// Snap is on. The settings panel (StoryboardPage) and project save/load
// (MainWindow) talk to the same instance.
//
// Geometry lives in CANVAS coordinates; vanishing points may sit far outside
// the canvas bounds. Snapping receives canvas-space points (already through
// the inverse view transform), so it is correct at any zoom/rotation/flip.
class PerspectiveTool
{
public:
    enum Mode { OnePoint = 1, TwoPoint = 2, ThreePoint = 3 };
    enum Handle { HandleNone, HandleHorizon, HandleVp0, HandleVp1, HandleVp2 };

    void reset(const QSizeF &canvas);

    // --- settings -----------------------------------------------------------
    Mode mode() const { return m_mode; }
    void setMode(Mode mode) { m_mode = mode; }
    bool isVisible() const { return m_visible; }
    void setVisible(bool on) { m_visible = on; }
    bool snapEnabled() const { return m_snap; }
    void setSnapEnabled(bool on) { m_snap = on; }
    int density() const { return m_density; }
    void setDensity(int lines) { m_density = qBound(2, lines, 72); }
    QColor color() const { return m_color; }
    void setColor(const QColor &color) { m_color = color; }
    qreal opacity() const { return m_opacity; }
    void setOpacity(qreal opacity) { m_opacity = qBound(0.05, opacity, 1.0); }
    qreal thickness() const { return m_thickness; }
    void setThickness(qreal px) { m_thickness = qBound(0.5, px, 8.0); }

    // --- geometry ------------------------------------------------------------
    int vanishingPointCount() const { return int(m_mode); }
    QPointF vanishingPoint(int index) const;
    qreal horizonY() const { return m_horizonY; }
    void setHorizonY(qreal y) { m_horizonY = y; }
    void setVanishingPoint(int index, const QPointF &canvasPos);

    // --- rendering (painter already in CANVAS space) --------------------------
    void paintGuides(QPainter &p, const QRectF &canvasRect) const;
    // Editing handles, WIDGET space (constant screen size; may be off-canvas).
    void paintHandles(QPainter &p, const QTransform &canvasToWidget) const;

    // --- interaction -----------------------------------------------------------
    Handle hitTest(const QPointF &widgetPos, const QTransform &canvasToWidget) const;
    void dragHandle(Handle handle, const QPointF &canvasPos);

    // --- snapping ---------------------------------------------------------------
    // Stroke assist: beginStroke() anchors at the stroke start and clears the
    // ray lock; snapPoint() locks onto the nearest VP ray once the stroke has
    // direction and projects every later point onto it. Returns the input
    // unchanged while snapping is off or no ray applies.
    void beginStroke(const QPointF &anchorCanvas);
    QPointF snapPoint(const QPointF &canvasPos);
    // Stateless variant for two-point tools (shape Line): projects onto the
    // nearest VP ray through the given anchor.
    QPointF snapToRay(const QPointF &anchorCanvas, const QPointF &canvasPos) const;

    // --- persistence -------------------------------------------------------------
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &object);

private:
    int nearestRayVp(const QPointF &anchor, const QPointF &pos) const;
    static QPointF projectOntoLine(const QPointF &a, const QPointF &b,
                                   const QPointF &p);

    Mode m_mode = TwoPoint;
    bool m_visible = false;
    bool m_snap = false;
    qreal m_horizonY = 216.0;
    qreal m_vpX[2] = {144.0, 816.0}; // the horizon VPs (y == m_horizonY)
    QPointF m_zenith{480.0, 864.0};  // 3-point third VP (above or below)
    int m_density = 12;
    QColor m_color{0x4d, 0x9f, 0xff};
    qreal m_opacity = 0.45;
    qreal m_thickness = 1.0;

    // Stroke-assist state.
    QPointF m_strokeAnchor;
    int m_lockedVp = -1;
};
