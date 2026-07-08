#include "DrawingCanvas.h"

#include "StoryboardModel.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineF>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPolygonF>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QStack>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>
#include <Qt>

#include <cmath>
#include <utility>

namespace {

// Build a display-only ghost: previous strokes become semi-transparent blue
// (#4d9fff) and the white paper becomes fully transparent, so compositing it
// over the current panel shows a faint blue ghost without washing the canvas.
QPixmap buildGhost(const QPixmap &previous)
{
    if (previous.isNull())
        return QPixmap();
    QImage img = previous.toImage().convertToFormat(QImage::Format_ARGB32);
    const int w = img.width();
    const int h = img.height();
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int alpha = 255 - qGray(line[x]); // dark strokes opaque, white clear
            line[x] = qRgba(0x4d, 0x9f, 0xff, alpha);
        }
    }
    return QPixmap::fromImage(img);
}

// Light-table ghost: like buildGhost but with an arbitrary tint — the panel's
// dark art becomes `tint` (alpha by darkness), white paper becomes transparent.
QPixmap buildTintedGhost(const QPixmap &flattened, const QColor &tint)
{
    if (flattened.isNull())
        return QPixmap();
    QImage img = flattened.toImage().convertToFormat(QImage::Format_ARGB32);
    const int w = img.width();
    const int h = img.height();
    const int tr = tint.red(), tg = tint.green(), tb = tint.blue();
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int alpha = 255 - qGray(line[x]); // dark art opaque, white clear
            line[x] = qRgba(tr, tg, tb, alpha);
        }
    }
    return QPixmap::fromImage(img);
}

bool isImagePath(const QString &path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QLatin1String(".png")) || lower.endsWith(QLatin1String(".jpg"))
        || lower.endsWith(QLatin1String(".jpeg")) || lower.endsWith(QLatin1String(".webp"));
}

bool firstImageUrl(const QMimeData *mime, QString *outPath)
{
    if (!mime || !mime->hasUrls())
        return false;
    const QList<QUrl> urls = mime->urls();
    if (urls.isEmpty() || !urls.first().isLocalFile())
        return false;
    const QString path = urls.first().toLocalFile();
    if (!isImagePath(path))
        return false;
    if (outPath)
        *outPath = path;
    return true;
}

} // namespace

DrawingCanvas::DrawingCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(false);
    setTabletTracking(true); // deliver stylus events (with pressure) to tabletEvent
    setCursor(Qt::CrossCursor);
    setMinimumHeight(220);
    setAcceptDrops(true); // import images by dropping files onto the canvas
    setFocusPolicy(Qt::ClickFocus); // needed for the spacebar pan modifier

    // Persisted safe-area guide opacities (Preferences > Camera).
    const QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    m_actionSafeMaskPct =
        qBound(0, settings.value(QStringLiteral("camera/actionSafeOpacity"), 50).toInt(), 100);
    m_titleSafeMaskPct =
        qBound(0, settings.value(QStringLiteral("camera/titleSafeOpacity"), 50).toInt(), 100);

    // Canvas View Controls toolbar (zoom slider, flip, rotate slider, reset
    // rotation) — a floating bar over the canvas, replacing the old -/%/+.
    buildViewToolbar();

    // Marching ants: advance the dash offset while a selection or floating
    // paste is on screen (updateAntsTimer() starts/stops it).
    m_antsTimer = new QTimer(this);
    m_antsTimer->setInterval(150);
    connect(m_antsTimer, &QTimer::timeout, this, [this] {
        m_antsPhase = (m_antsPhase + 1) % 8;
        update();
    });

    // Rotate cursor for the transform box's rotation zones (a curved arrow).
    {
        QPixmap pm(24, 24);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(Qt::white, 2.4);
        pen.setCapStyle(Qt::RoundCap);
        for (int pass = 0; pass < 2; ++pass) {   // white halo, then black stroke
            pen.setColor(pass ? QColor(20, 20, 20) : Qt::white);
            pen.setWidthF(pass ? 1.4 : 2.6);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(5, 5, 14, 14), 40 * 16, 260 * 16); // ~C-shaped arc
            p.setPen(Qt::NoPen);
            p.setBrush(pass ? QColor(20, 20, 20) : Qt::white);
            p.drawPolygon(QPolygonF({QPointF(18.5, 5.5), QPointF(22.0, 8.5),
                                     QPointF(16.5, 9.5)}));       // arrowhead
        }
        p.end();
        m_rotateCursor = QCursor(pm, 12, 12);
    }
}

QSize DrawingCanvas::canvasSize()
{
    return QSize(960, 540); // 16:9
}

void DrawingCanvas::setActivePanel(Panel *panel)
{
    if (m_xformActive)
        commitTransform(); // finalise onto the CURRENT layer before switching
    m_panel = panel;
    m_drawing = false;
    cancelShape(); // preview state never carries across panels
    // A floating paste or in-progress move is discarded (committing to a
    // DIFFERENT panel's layer would be wrong; the move never wrote to the
    // old layer anyway) and the selection mask never carries across panels.
    m_floatActive = false;
    m_floatDragging = false;
    m_moveActive = false;
    m_layerBackup = QImage();
    m_moveMask = QImage();
    m_floatImg = QImage();
    m_floatDelta = QPointF();
    clearSelection();
    update();
}

void DrawingCanvas::setOnionSkinEnabled(bool enabled)
{
    m_onionSkin = enabled;
    update();
}

void DrawingCanvas::setPreviousPixmap(const QPixmap &previous)
{
    m_ghost = buildGhost(previous); // null pixmap -> empty ghost
    update();
}

void DrawingCanvas::setLightTableEnabled(bool enabled)
{
    m_lightTable = enabled;
    update();
}

void DrawingCanvas::setLightTablePixmaps(const QPixmap &previous, const QPixmap &next)
{
    m_ltPrevGhost = buildTintedGhost(previous, QColor(0xff, 0x4d, 0x4d)); // red = previous
    m_ltNextGhost = buildTintedGhost(next, QColor(0x4d, 0xff, 0x91));     // green = next
    update();
}

// Draw the neighbour ghosts at 35% each (previous red under next green).
void DrawingCanvas::drawLightTable(QPainter &painter, const QRect &d) const
{
    painter.setOpacity(0.35);
    if (!m_ltPrevGhost.isNull())
        painter.drawPixmap(d, m_ltPrevGhost);
    if (!m_ltNextGhost.isNull())
        painter.drawPixmap(d, m_ltNextGhost);
    painter.setOpacity(1.0);
}

void DrawingCanvas::setCameraFrameEnabled(bool enabled)
{
    m_cameraFrame = enabled;
    update();
}

void DrawingCanvas::setSafeAreaEnabled(bool enabled)
{
    m_safeArea = enabled;
    update();
}

void DrawingCanvas::setTitleSafeEnabled(bool enabled)
{
    m_titleSafe = enabled;
    update();
}

void DrawingCanvas::setGridEnabled(bool enabled)
{
    m_grid = enabled;
    update();
}

void DrawingCanvas::setActionSafeMaskOpacity(int percent)
{
    m_actionSafeMaskPct = qBound(0, percent, 100);
    update();
}

void DrawingCanvas::setTitleSafeMaskOpacity(int percent)
{
    m_titleSafeMaskPct = qBound(0, percent, 100);
    update();
}

// The active layer, but only when it can legally take strokes: locked layers
// ignore input entirely (no cursor change), hidden layers can't be drawn on
// (the stroke would be invisible), and a panel with no layers draws nowhere.
Layer *DrawingCanvas::editableActiveLayer() const
{
    if (!m_panel)
        return nullptr;
    Layer *layer = m_panel->activeLayer();
    if (!layer || layer->locked || !layer->visible)
        return nullptr;
    return layer;
}

bool DrawingCanvas::importImage(const QString &filePath)
{
    if (!m_panel)
        return false;

    QImage loaded(filePath);
    if (loaded.isNull()) {
        QMessageBox::warning(this, QStringLiteral("Import Image"),
                             QStringLiteral("Could not load the selected image."));
        return false;
    }

    // The import becomes a NEW image-type layer above the active one — the
    // existing drawing is never overwritten (delete the layer to discard it).
    QImage content = makeLayerImage(); // transparent padding around the fit
    {
        QPainter painter(&content);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = loaded.size();
        target.scale(canvasSize(), Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(content.rect().center());
        painter.drawImage(r, loaded);
    }

    Layer layer = makeRasterLayer(QFileInfo(filePath).completeBaseName());
    layer.type = QStringLiteral("image");
    layer.image = content;

    const int insertAt = qBound(0, m_panel->activeLayerIndex + 1, m_panel->layers.size());
    m_panel->layers.insert(insertAt, layer);
    m_panel->activeLayerIndex = insertAt;

    update();
    emit layersChanged();  // Layer panel rebuilds its rows
    emit contentChanged(); // refreshes the panel thumbnail
    return true;
}

void DrawingCanvas::setTool(Tool tool)
{
    commitFloating(); // an un-committed paste lands before the tool changes
    if (m_xformActive && tool != Move)
        commitTransform(); // leaving Move finalises the transform box
    m_tool = tool;
    cancelShape(); // an in-progress shape never survives a tool switch
    // Activating Move with a live selection lifts it into the transform box.
    if (tool == Move && !m_xformActive && !m_selPath.isEmpty())
        beginTransform();
    update(); // the selection itself DOES survive (Select -> Move flow)
}

void DrawingCanvas::setShapeKind(ShapeKind kind)
{
    if (m_shapeKind != kind)
        cancelShape();
    m_shapeKind = kind;
}

void DrawingCanvas::setShapeStrokeWidth(int px)
{
    m_shapeStroke = qBound(1, px, 100);
    update(); // a live preview follows the new width
}

void DrawingCanvas::setShapeFill(bool on)
{
    m_shapeFill = on;
    update();
}

void DrawingCanvas::setColor(const QColor &color)
{
    m_color = color;
}

void DrawingCanvas::setBrushSize(int size)
{
    m_brushSize = qBound(1, size, 20);
}

void DrawingCanvas::setBrushToolSize(int px)
{
    m_brushToolSize = qBound(1, px, 200);
}

void DrawingCanvas::setBrushOpacity(int percent)
{
    m_brushToolOpacity = qBound(0, percent, 100) / 100.0;
}

void DrawingCanvas::setBrushHardness(int percent)
{
    m_brushHardness = qBound(0, percent, 100) / 100.0;
}

void DrawingCanvas::setPressureToSize(bool on)
{
    m_pressureToSize = on;
}

void DrawingCanvas::setPressureToOpacity(bool on)
{
    m_pressureToOpacity = on;
}

// The letterbox fit is the zoom=1.0 baseline; m_zoom scales it and m_panOffset
// shifts it. Because toCanvas()/scale() derive from this rect, EVERY mouse
// press/move/release converts screen -> image coordinates through the same
// zoom+pan mapping, so strokes land under the cursor at any zoom level.
QRect DrawingCanvas::displayRect() const
{
    const QSize cs = canvasSize();
    const double fit = qMin(width() / double(cs.width()), height() / double(cs.height()));
    const double s = fit * m_zoom;
    const int w = int(cs.width() * s);
    const int h = int(cs.height() * s);
    return QRect(qRound((width() - w) / 2.0 + m_panOffset.x()),
                 qRound((height() - h) / 2.0 + m_panOffset.y()), w, h);
}

void DrawingCanvas::setZoom(double zoom, const QPointF &anchorScreen)
{
    zoom = qBound(0.25, zoom, 4.0);
    if (qFuzzyCompare(zoom, m_zoom)) {
        syncViewToolbar();
        return;
    }

    const QSize cs = canvasSize();
    const double fit = qMin(width() / double(cs.width()), height() / double(cs.height()));

    // Canvas-space point currently under the anchor (floating, unclamped).
    const QRect before = displayRect();
    const double sOld = fit * m_zoom;
    const QPointF canvasPt((anchorScreen.x() - before.x()) / sOld,
                           (anchorScreen.y() - before.y()) / sOld);

    m_zoom = zoom;

    // Solve the pan offset so that canvasPt stays exactly under the anchor.
    const double sNew = fit * m_zoom;
    const double w = cs.width() * sNew;
    const double h = cs.height() * sNew;
    m_panOffset = QPointF(anchorScreen.x() - canvasPt.x() * sNew - (width() - w) / 2.0,
                          anchorScreen.y() - canvasPt.y() * sNew - (height() - h) / 2.0);

    syncViewToolbar();
    emit viewZoomChanged(m_zoom);
    update();
}

void DrawingCanvas::resetView()
{
    m_zoom = 1.0;
    m_panOffset = QPointF(0, 0);
    m_viewRotation = 0.0;
    m_viewFlipH = false;
    syncViewToolbar();
    emit viewZoomChanged(m_zoom);
    update();
}

void DrawingCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionViewToolbar();
}

// --- Canvas View Controls (display-only view transforms) --------------------

void DrawingCanvas::setViewZoom(double zoom)
{
    setZoom(zoom, QPointF(width() / 2.0, height() / 2.0)); // centred on the view
}

void DrawingCanvas::setViewRotation(double degrees)
{
    m_viewRotation = qBound(-180.0, degrees, 180.0);
    update();
}

void DrawingCanvas::toggleFlipH()
{
    m_viewFlipH = !m_viewFlipH;
    update();
}

void DrawingCanvas::resetViewRotation()
{
    m_viewRotation = 0.0; // does not touch zoom or flip
    if (m_rotateSlider) {
        const QSignalBlocker block(m_rotateSlider);
        m_rotateSlider->setValue(0);
    }
    update();
}

namespace {
constexpr double kZoomMin = 0.25, kZoomMax = 4.0, kZoomSteps = 1000.0;
// Log map so 0 -> 0.25x, mid -> 1x, max -> 4x.
double sliderToZoom(int v)
{
    return kZoomMin * std::pow(kZoomMax / kZoomMin, v / kZoomSteps);
}
int zoomToSlider(double z)
{
    z = qBound(kZoomMin, z, kZoomMax);
    return int(std::round(kZoomSteps * std::log(z / kZoomMin) / std::log(kZoomMax / kZoomMin)));
}
} // namespace

void DrawingCanvas::syncViewToolbar()
{
    if (m_zoomSlider) {
        const QSignalBlocker block(m_zoomSlider);
        m_zoomSlider->setValue(zoomToSlider(m_zoom));
    }
}

void DrawingCanvas::positionViewToolbar()
{
    if (!m_viewToolbar)
        return;
    const int x = (width() - m_viewToolbar->width()) / 2;      // bottom-centre
    const int y = height() - m_viewToolbar->height() - 12;
    m_viewToolbar->move(qMax(6, x), qMax(6, y));
    m_viewToolbar->raise();
}

void DrawingCanvas::buildViewToolbar()
{
    m_viewToolbar = new QWidget(this);
    m_viewToolbar->setObjectName(QStringLiteral("viewToolbar"));
    m_viewToolbar->setAttribute(Qt::WA_StyledBackground, true);
    m_viewToolbar->setFixedHeight(42);
    m_viewToolbar->setStyleSheet(QStringLiteral(
        "QWidget#viewToolbar { background-color: #212121; border: 1px solid #1a1a1a;"
        " border-radius: 12px; }"));

    QHBoxLayout *row = new QHBoxLayout(m_viewToolbar);
    row->setContentsMargins(10, 0, 12, 0);
    row->setSpacing(11);

    // Grip: 3 columns x 2 rows of 3x3 dots (#6a6a6a), draggable.
    QLabel *gripLabel = new QLabel;
    QPixmap grip(QSize(22, 28));
    grip.setDevicePixelRatio(2.0);
    grip.fill(Qt::transparent);
    {
        QPainter p(&grip);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x6a, 0x6a, 0x6a));
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                p.fillRect(QRectF(1 + c * 5.0, 3 + r * 6.0, 3, 3), QColor(0x6a, 0x6a, 0x6a));
    }
    gripLabel->setPixmap(grip);
    gripLabel->setFixedWidth(13);
    gripLabel->setCursor(Qt::OpenHandCursor);
    gripLabel->setToolTip(QStringLiteral("Drag to move"));
    gripLabel->installEventFilter(this);
    m_viewGrip = gripLabel;
    row->addWidget(gripLabel);

    const QString labelCss = QStringLiteral(
        "color: #cccccc; font-family: 'Inter'; font-size: 11px; font-weight: 600;"
        " background: transparent; border: none;");
    const QString sliderCss = QStringLiteral(
        "QSlider::groove:horizontal { height: 6px; background: #333333; border-radius: 1px; }"
        "QSlider::sub-page:horizontal { background: #4b82b6; border-radius: 1px; }"
        "QSlider::add-page:horizontal { background: #333333; border-radius: 1px; }"
        "QSlider::handle:horizontal { width: 20px; height: 6px; background: #b3b3b3;"
        " border-radius: 1px; margin: 0; }");
    auto divider = [] {
        QFrame *f = new QFrame;
        f->setFixedSize(1, 20);
        f->setStyleSheet(QStringLiteral("background-color: #4d4d4d; border: none;"));
        return f;
    };

    QLabel *zoomLabel = new QLabel(QStringLiteral("Zoom"));
    zoomLabel->setStyleSheet(labelCss);
    row->addWidget(zoomLabel);

    m_zoomSlider = new QSlider(Qt::Horizontal);
    m_zoomSlider->setFixedSize(112, 16);
    m_zoomSlider->setRange(0, int(kZoomSteps));
    m_zoomSlider->setValue(zoomToSlider(m_zoom));
    m_zoomSlider->setStyleSheet(sliderCss);
    m_zoomSlider->setToolTip(QStringLiteral("Zoom the view (0.25x – 4x)"));
    connect(m_zoomSlider, &QSlider::valueChanged, this,
            [this](int v) { setViewZoom(sliderToZoom(v)); });
    row->addWidget(m_zoomSlider);

    row->addWidget(divider());

    QToolButton *flip = new QToolButton;
    flip->setCheckable(true);
    flip->setCursor(Qt::PointingHandCursor);
    flip->setFixedSize(24, 24);
    flip->setIcon(QIcon(QStringLiteral(":/icons/flip.svg")));
    flip->setIconSize(QSize(21, 17));
    flip->setToolTip(QStringLiteral("Flip the view horizontally"));
    flip->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; border-radius: 4px; }"
        "QToolButton:hover { background-color: #2e2e2e; }"
        "QToolButton:checked { background-color: #4b82b6; }"));
    connect(flip, &QToolButton::toggled, this, [this](bool on) {
        if (on != m_viewFlipH)
            toggleFlipH();
    });
    row->addWidget(flip);

    row->addWidget(divider());

    QLabel *rotLabel = new QLabel(QStringLiteral("Rotate"));
    rotLabel->setStyleSheet(labelCss);
    row->addWidget(rotLabel);

    m_rotateSlider = new QSlider(Qt::Horizontal);
    m_rotateSlider->setFixedSize(112, 16);
    m_rotateSlider->setRange(-180, 180);
    m_rotateSlider->setValue(0);
    m_rotateSlider->setStyleSheet(sliderCss);
    m_rotateSlider->setToolTip(QStringLiteral("Rotate the view (-180° – 180°)"));
    connect(m_rotateSlider, &QSlider::valueChanged, this,
            [this](int v) { setViewRotation(v); });
    row->addWidget(m_rotateSlider);

    QToolButton *reset = new QToolButton;
    reset->setCursor(Qt::PointingHandCursor);
    reset->setFixedSize(21, 21);
    reset->setIcon(QIcon(QStringLiteral(":/icons/resetrotation.svg")));
    reset->setIconSize(QSize(21, 21));
    reset->setToolTip(QStringLiteral("Reset rotation to 0°"));
    reset->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; border-radius: 4px; }"
        "QToolButton:hover { background-color: #2e2e2e; }"));
    connect(reset, &QToolButton::clicked, this, [this] { resetViewRotation(); });
    row->addWidget(reset);

    m_viewToolbar->adjustSize();
    m_viewToolbar->raise();
}

double DrawingCanvas::scale() const
{
    const int w = displayRect().width();
    return w > 0 ? w / double(canvasSize().width()) : 1.0;
}

// The full canvas->widget mapping: zoom+pan (displayRect) then rotation and
// horizontal flip about the view centre. Every draw and every mouse mapping
// goes through this (or its inverse), so strokes land under the cursor at any
// zoom / rotation / flip. Display only — layer pixels never change.
QTransform DrawingCanvas::viewTransform() const
{
    const QRect d = displayRect();
    const double s = scale();
    const QPointF c = QRectF(d).center();

    QTransform base; // canvas -> axis-aligned zoom+pan rect (widget space)
    base.translate(d.left(), d.top());
    base.scale(s, s);

    QTransform rf; // rotate + flip about the view centre, in widget space
    rf.translate(c.x(), c.y());
    rf.rotate(m_viewRotation);
    if (m_viewFlipH)
        rf.scale(-1.0, 1.0);
    rf.translate(-c.x(), -c.y());

    return base * rf; // canvas -> d -> rotated/flipped widget
}

QPoint DrawingCanvas::toCanvas(const QPoint &widgetPoint) const
{
    const QSize cs = canvasSize();
    const QPointF p = viewTransform().inverted().map(QPointF(widgetPoint));
    return QPoint(qBound(0, int(p.x()), cs.width() - 1),
                  qBound(0, int(p.y()), cs.height() - 1));
}

int DrawingCanvas::penWidth() const
{
    return qMax(1, qRound(m_brushSize / scale()));
}

// Float variant for the brush engine: sub-pixel dab placement, no clamping
// (dabs partially outside the image are clipped by QPainter automatically).
QPointF DrawingCanvas::toCanvasF(const QPointF &widgetPoint) const
{
    return viewTransform().inverted().map(widgetPoint); // inverse zoom+pan+rotate+flip
}

// Snapshot the ACTIVE layer's pixels (keyed by layer id, so undo still lands
// on the right layer after reorders or deletions of other layers).
void DrawingCanvas::pushUndo()
{
    if (!m_panel)
        return;
    Layer *layer = m_panel->activeLayer();
    if (!layer)
        return;
    m_panel->undoStack.append({layer->id, layer->image.copy()});
    while (m_panel->undoStack.size() > 20)
        m_panel->undoStack.removeFirst();
}

void DrawingCanvas::drawSegment(const QPoint &from, const QPoint &to, const QColor &color)
{
    Layer *layer = editableActiveLayer();
    if (!layer)
        return;
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Eraser carves the layer back to TRANSPARENT (revealing layers below /
    // the white paper) — painting white would smear over lower layers.
    if (m_tool == Eraser)
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
    QPen pen(color);
    pen.setWidth(penWidth());
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawLine(from, to);
    painter.end();
    update();
}

// Draw the current in-progress shape in CANVAS coordinates. The preview
// paints through the display transform and the commit paints straight into
// the layer image, so both render identical geometry. closePolygon: a
// committed polygon closes (and may fill); the preview stays an open
// polyline with a rubber segment to the cursor.
void DrawingCanvas::paintShapeGeometry(QPainter &painter, bool closePolygon) const
{
    QPen pen(m_color, qMax(1, m_shapeStroke));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(m_shapeFill ? QBrush(m_color) : Qt::NoBrush);

    const QRectF box = QRectF(m_shapeStartC, m_shapeCurrentC).normalized();
    switch (m_shapeKind) {
    case ShapeRectangle:
        painter.drawRect(box);
        break;
    case ShapeCircle:
        painter.drawEllipse(box); // drag defines the bounding box
        break;
    case ShapeTriangle: {
        QPolygonF triangle; // isosceles: apex centred on the box's top edge
        triangle << QPointF(box.center().x(), box.top())
                 << QPointF(box.right(), box.bottom())
                 << QPointF(box.left(), box.bottom());
        painter.drawPolygon(triangle);
        break;
    }
    case ShapeLine:
        painter.drawLine(m_shapeStartC, m_shapeCurrentC);
        break;
    case ShapePolygon:
        if (closePolygon) {
            painter.drawPolygon(QPolygonF(m_polygonPts));
        } else if (!m_polygonPts.isEmpty()) {
            painter.setBrush(Qt::NoBrush); // fill applies only once closed
            QPolygonF open(m_polygonPts);
            open << m_shapeCurrentC; // rubber segment to the cursor
            painter.drawPolyline(open);
        }
        break;
    }
}

void DrawingCanvas::commitDragShape()
{
    m_shapeDrag = false;
    Layer *layer = editableActiveLayer();
    if (!layer) {
        update();
        return;
    }
    const QRectF box = QRectF(m_shapeStartC, m_shapeCurrentC).normalized();
    const bool needsArea = (m_shapeKind == ShapeRectangle || m_shapeKind == ShapeCircle
                            || m_shapeKind == ShapeTriangle);
    if (needsArea && box.width() < 1.0 && box.height() < 1.0) {
        update(); // degenerate click: nothing to draw, no undo entry
        return;
    }
    pushUndo();
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintShapeGeometry(painter, true);
    painter.end();
    update();
    emit contentChanged();
}

void DrawingCanvas::commitPolygon()
{
    // Drop consecutive duplicate vertices — the closing double-click adds
    // one at the same spot via its own press before this runs.
    QVector<QPointF> pts;
    for (const QPointF &pt : std::as_const(m_polygonPts))
        if (pts.isEmpty() || QLineF(pts.last(), pt).length() >= 1.0)
            pts.append(pt);
    m_polygonPts = pts;

    Layer *layer = editableActiveLayer();
    if (!layer || m_polygonPts.size() < 3) {
        cancelShape(); // not enough vertices for a shape: no artifacts
        return;
    }
    pushUndo();
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintShapeGeometry(painter, true);
    painter.end();
    cancelShape();
    emit contentChanged();
}

void DrawingCanvas::cancelShape()
{
    m_shapeDrag = false;
    m_polygonPts.clear();
    setMouseTracking(false); // hover tracking only while a polygon is open
    update();
}

// --- Selection + canvas clipboard (ACTIVE layer only) ------------------------

void DrawingCanvas::clearSelection()
{
    if (m_xformActive)
        cancelTransform(); // deselecting mid-transform discards it (restores backup)
    m_selPath = QPainterPath();
    m_selDrag = false;
    m_lassoPts.clear();
    updateAntsTimer();
    update();
}

void DrawingCanvas::updateAntsTimer()
{
    const bool needed = !m_selPath.isEmpty() || m_selDrag || m_floatActive;
    if (needed && !m_antsTimer->isActive())
        m_antsTimer->start();
    else if (!needed && m_antsTimer->isActive())
        m_antsTimer->stop();
}

QRectF DrawingCanvas::floatBounds() const
{
    return QRectF(m_floatPos + m_floatDelta, QSizeF(m_floatImg.size()));
}

// The floating buffer always originated on-canvas, so it always FITS
// on-canvas: clamp its position so no part ever hangs past an edge. Without
// this, the commit's drawImage() clips at the layer bounds and any
// overhanging pixels are permanently destroyed — the "eaten corners" bug.
// Applied to the drag preview AND the commit, so preview == commit.
QPointF DrawingCanvas::clampFloatDelta(const QPointF &delta) const
{
    if (m_floatImg.isNull())
        return delta;
    const QSize cs = canvasSize();
    const QPointF target = m_floatPos + delta;
    const qreal maxX = qMax<qreal>(0.0, cs.width() - m_floatImg.width());
    const qreal maxY = qMax<qreal>(0.0, cs.height() - m_floatImg.height());
    const QPointF clamped(qBound<qreal>(0.0, target.x(), maxX),
                          qBound<qreal>(0.0, target.y(), maxY));
    return clamped - m_floatPos;
}

// The selection path rasterized once into a hard-edged binary mask (alpha
// strictly 0 or 255), in boundingRect-local coordinates. DestinationIn with
// it KEEPS exactly the masked pixels; DestinationOut CLEARS exactly the same
// set — the two partition the rect with no off-by-one.
QImage DrawingCanvas::selectionMask(const QRect &boundingRect) const
{
    QImage mask(boundingRect.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.translate(-boundingRect.topLeft());
    painter.fillPath(m_selPath, Qt::white);
    return mask;
}

// Copy is not an edit, so it reads the active layer even when locked.
void DrawingCanvas::copySelection()
{
    if (!m_panel || m_selPath.isEmpty())
        return;
    const Layer *layer = m_panel->activeLayer();
    if (!layer || layer->image.isNull())
        return;
    const QRect r = m_selPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;
    QImage img = layer->image.copy(r);
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    p.drawImage(0, 0, selectionMask(r)); // only pixels inside the mask survive
    p.end();
    m_clipImg = img;
    m_clipPos = r.topLeft(); // for Paste in Place
}

void DrawingCanvas::cutSelection()
{
    Layer *layer = editableActiveLayer(); // locked layers block the edit
    if (!layer || m_selPath.isEmpty())
        return;
    copySelection();
    const QRect r = m_selPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;
    pushUndo();
    QPainter p(&layer->image);
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawImage(r.topLeft(), selectionMask(r)); // clears exactly the copied pixels
    p.end();
    update();
    emit contentChanged();
}

// Paste creates FLOATING pixels the user can drag with the Move tool;
// click-away or Enter commits them, Esc discards. atOriginalPos = the exact
// coordinates the pixels were copied from; otherwise the view centre.
void DrawingCanvas::pasteClipboard(bool atOriginalPos)
{
    if (m_clipImg.isNull() || !m_panel)
        return;
    if (!editableActiveLayer())
        return; // locked layer: block
    commitFloating(); // any previous floating paste lands first
    m_floatActive = true;
    m_floatFromPaste = true;
    m_floatImg = m_clipImg;
    if (atOriginalPos) {
        m_floatPos = m_clipPos; // integral: captured from an aligned rect
    } else {
        const QPointF viewCentre = toCanvasF(QPointF(width() / 2.0, height() / 2.0));
        const QPointF raw = viewCentre - QPointF(m_clipImg.width() / 2.0, m_clipImg.height() / 2.0);
        m_floatPos = QPointF(qRound(raw.x()), qRound(raw.y())); // stay pixel-aligned
    }
    m_floatDelta = clampFloatDelta(QPointF()); // never spawn hanging off-canvas
    m_selPath = QPainterPath(); // the floating outline replaces the selection
    m_selDrag = false;
    m_lassoPts.clear();
    updateAntsTimer();
    update();
}

// Save a move-pipeline stage to C:\SankoTv\app\debug\. `checker` composites
// the (alpha) image over a checkerboard so transparent regions and clipped
// edges are visible; otherwise the raw image is saved.
void DrawingCanvas::dumpMoveDebug(const QString &name, const QImage &img, bool checker) const
{
    if (!m_debugMove || img.isNull())
        return;
    const QString dir = QStringLiteral("C:/SankoTv/app/debug");
    QDir().mkpath(dir);
    QImage out = img.convertToFormat(QImage::Format_ARGB32);
    if (checker) {
        QImage bg(out.size(), QImage::Format_ARGB32);
        const int cell = 8;
        for (int y = 0; y < bg.height(); ++y)
            for (int x = 0; x < bg.width(); ++x) {
                const bool dark = ((x / cell) + (y / cell)) & 1;
                bg.setPixel(x, y, dark ? qRgb(0x60, 0x60, 0x60) : qRgb(0xa0, 0xa0, 0xa0));
            }
        QPainter p(&bg);
        p.drawImage(0, 0, out);
        p.end();
        out = bg;
    }
    out.save(dir + QLatin1Char('/') + name);
}

// MOUSE DOWN of a move. Steps 1-4 of the move algorithm:
//   1) build the selection mask,
//   2) copy the masked pixels into the floating buffer (complete, sized to
//      the selection bounding rect, never cropped),
//   3) snapshot the ENTIRE layer into m_layerBackup,
//   4) do NOT modify the layer.
void DrawingCanvas::beginMoveDrag(const QPointF &grabCanvasPt)
{
    Layer *layer = editableActiveLayer();
    if (!layer || m_selPath.isEmpty())
        return;
    const QRect r = m_selPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;

    m_moveMask = selectionMask(r); // ONE mask for buffer + commit clear
    m_moveSrcRect = r;

    m_floatImg = layer->image.copy(r); // 2) mask-limited copy of the pixels
    {
        QPainter bufferPainter(&m_floatImg);
        bufferPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        bufferPainter.drawImage(0, 0, m_moveMask);
    }

    m_layerBackup = layer->image.copy(); // 3) pristine pre-move snapshot

    // Visual debug: the lifted buffer (on a checkerboard so its true extent
    // and any clipping are visible) and the full pre-move layer.
    dumpMoveDebug(QStringLiteral("debug_1_floatBuffer.png"), m_floatImg, true);
    dumpMoveDebug(QStringLiteral("debug_2_layerBackup.png"), m_layerBackup, false);

    // 4) the layer itself stays untouched until mouse up.
    m_moveActive = true;
    m_floatActive = true; // drives the paintEvent preview + marching ants
    m_floatFromPaste = false;
    m_floatDragging = true;
    m_floatPos = r.topLeft();
    m_floatDelta = QPointF();
    m_floatGrabC = grabCanvasPt;
    m_floatGrabDelta = QPointF();
    updateAntsTimer();
    update();
}

// MOUSE UP of a move — the layer is written here, exactly ONCE.
// Steps 7-11 of the move algorithm.
void DrawingCanvas::commitMoveDrag()
{
    m_floatDragging = false;
    if (!m_moveActive)
        return;
    m_moveActive = false;
    m_floatActive = false;

    Layer *layer = editableActiveLayer();
    if (layer && !m_layerBackup.isNull()) {
        // 7) start from the pristine pre-move layer.
        QImage result = m_layerBackup;

        // 8) clear the ORIGINAL selection area (masked pixels only, never
        //    the whole bounding rectangle).
        {
            QPainter clearPainter(&result);
            clearPainter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            clearPainter.drawImage(m_moveSrcRect.topLeft(), m_moveMask);
        }
        dumpMoveDebug(QStringLiteral("debug_3_afterClear.png"), result, false);

        // 9) paste the buffer at the final dragged position, SourceOver.
        //    The position is CLAMPED so the whole buffer lands inside the
        //    canvas — the layer is exactly canvas-sized, so any overhang
        //    would be clipped and DESTROYED (debug_4). Clamping preserves
        //    every pixel (the stated goal); the art parks flush at the edge.
        //    No selection-rect clip is applied. Starting from the backup with
        //    the source cleared first, moving onto the former area is safe.
        const QPointF target = m_floatPos + clampFloatDelta(m_floatDelta);
        const QPoint aligned(qRound(target.x()), qRound(target.y()));
        {
            QPainter pastePainter(&result);
            pastePainter.drawImage(aligned, m_floatImg);
        }
        dumpMoveDebug(QStringLiteral("debug_4_final.png"), result, false);

        // 11) ONE undo entry for the whole move: the layer still holds the
        //     pre-move image right now, so this snapshot IS the original.
        pushUndo();

        // 10) assign the rebuilt image back and refresh.
        layer->image = result;
        if (!m_selPath.isEmpty())
            m_selPath.translate(QPointF(aligned) - m_floatPos); // ants follow
        emit contentChanged();
    }

    m_layerBackup = QImage();
    m_moveMask = QImage();
    m_floatImg = QImage();
    m_floatDelta = QPointF();
    updateAntsTimer();
    update();
}

// Commit of a floating PASTE (click-away / Enter / tool switch).
void DrawingCanvas::commitFloating()
{
    if (!m_floatActive || !m_floatFromPaste)
        return;
    m_floatActive = false;
    m_floatDragging = false;
    Layer *layer = editableActiveLayer();
    if (layer) {
        pushUndo();
        const QPointF target = m_floatPos + m_floatDelta;
        const QPoint aligned(qRound(target.x()), qRound(target.y()));
        QPainter p(&layer->image);
        p.drawImage(aligned, m_floatImg); // limited only by the canvas bounds
        p.end();
        emit contentChanged();
    }
    m_floatDelta = QPointF();
    m_floatImg = QImage();
    updateAntsTimer();
    update();
}

void DrawingCanvas::cancelFloatingPaste()
{
    if (!m_floatActive || !m_floatFromPaste)
        return;
    m_floatActive = false; // display-only: discarding leaves no artifacts
    m_floatDragging = false;
    m_floatDelta = QPointF();
    m_floatImg = QImage();
    updateAntsTimer();
    update();
}

// --- Non-destructive transform box (Move tool) ------------------------------

namespace {
// Rotate a vector by `deg` degrees (no translation).
inline QPointF rotVec(qreal deg, const QPointF &v)
{
    return QTransform().rotate(deg).map(v);
}
constexpr qreal kMinBox = 4.0; // smallest box extent, canvas px (avoids collapse)
} // namespace

// ACTIVATING Move with a selection: lift the masked pixels into the pristine
// transform buffer, snapshot the whole layer, clear the source once so the
// preview shows the art lifted out, and initialise an axis-aligned box.
void DrawingCanvas::beginTransform()
{
    Layer *layer = editableActiveLayer();
    if (!layer || m_selPath.isEmpty())
        return;
    const QRect r = m_selPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;

    m_moveMask = selectionMask(r);
    m_moveSrcRect = r;

    m_transformBuf = layer->image.copy(r); // PRISTINE lifted pixels (never re-transformed)
    {
        QPainter p(&m_transformBuf);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, m_moveMask);
    }

    m_layerBackup = layer->image.copy(); // for commit rebuild + Esc restore

    {
        QPainter c(&layer->image); // clear the source once (preview shows it gone)
        c.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        c.drawImage(r.topLeft(), m_moveMask);
    }

    m_boxCenter = QRectF(r).center();
    m_boxW = r.width();
    m_boxH = r.height();
    m_boxAngle = 0.0;
    m_xformActive = true;
    m_xformMode = XNone;
    setMouseTracking(true); // hover updates the scale/rotate cursor
    updateAntsTimer();
    update();
}

// Maps the pristine buffer (0..srcW, 0..srcH) onto the current box in CANVAS
// coordinates: recentre, rotate, scale — always from the ORIGINAL buffer.
QTransform DrawingCanvas::boxTransform() const
{
    const qreal srcW = m_moveSrcRect.width();
    const qreal srcH = m_moveSrcRect.height();
    QTransform t;
    t.translate(m_boxCenter.x(), m_boxCenter.y());
    t.rotate(m_boxAngle);
    if (srcW > 0.0 && srcH > 0.0)
        t.scale(m_boxW / srcW, m_boxH / srcH);
    t.translate(-srcW / 2.0, -srcH / 2.0);
    return t;
}

// The 8 handle points (4 corners + 4 edge midpoints) in canvas coordinates,
// ordered TL, TR, BR, BL, T, R, B, L.
QVector<QPointF> DrawingCanvas::boxHandlesCanvas() const
{
    const qreal hw = m_boxW / 2.0, hh = m_boxH / 2.0;
    const QPointF local[8] = {
        {-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}, // corners
        {0, -hh}, {hw, 0}, {0, hh}, {-hw, 0}};      // edge mids
    QVector<QPointF> pts;
    pts.reserve(8);
    for (const QPointF &l : local)
        pts.append(m_boxCenter + rotVec(m_boxAngle, l));
    return pts;
}

// Which part of the box the widget-space point is over. Handles/rotation zones
// use constant SCREEN tolerances so they stay grabbable at any zoom.
DrawingCanvas::XformMode DrawingCanvas::hitTestBox(const QPointF &widgetPos) const
{
    if (!m_xformActive)
        return XNone;
    const QTransform toWidget = viewTransform(); // canvas -> widget (rotate/flip aware)

    const QVector<QPointF> handles = boxHandlesCanvas();
    const XformMode order[8] = {XScaleTL, XScaleTR, XScaleBR, XScaleBL,
                                XScaleT, XScaleR, XScaleB, XScaleL};
    const qreal handleTol = 8.0;  // px
    const qreal rotateTol = 22.0; // px, just outside the corners

    // Handles first (corners take priority over edges via ordering).
    for (int i = 0; i < 8; ++i) {
        const QPointF w = toWidget.map(handles.at(i));
        if (QLineF(w, widgetPos).length() <= handleTol)
            return order[i];
    }
    // Inside the box body -> move.
    QPolygonF poly;
    for (int i = 0; i < 4; ++i)
        poly << toWidget.map(handles.at(i));
    if (poly.containsPoint(widgetPos, Qt::OddEvenFill))
        return XMove;
    // Just outside a corner -> rotate.
    for (int i = 0; i < 4; ++i) {
        const QPointF w = toWidget.map(handles.at(i));
        if (QLineF(w, widgetPos).length() <= rotateTol)
            return XRotate;
    }
    return XNone;
}

// Recompute the box from the press-time snapshot (never from the running
// result) for the active handle. proportional keeps the aspect ratio.
void DrawingCanvas::applyXformDrag(const QPointF &canvasPos, bool proportional)
{
    switch (m_xformMode) {
    case XMove:
        m_boxCenter = m_boxCenter0 + (canvasPos - m_dragStartCanvas);
        break;
    case XRotate: {
        const qreal now = std::atan2(canvasPos.y() - m_boxCenter0.y(),
                                     canvasPos.x() - m_boxCenter0.x());
        const qreal deltaDeg = (now - m_rotStart0) * 180.0 / M_PI;
        m_boxAngle = m_boxAngle0 + deltaDeg;
        break;
    }
    default: {
        // Scale. Signs of the dragged handle in the box's local frame.
        qreal hx = 0, hy = 0;
        switch (m_xformMode) {
        case XScaleTL: hx = -1; hy = -1; break;
        case XScaleTR: hx = 1;  hy = -1; break;
        case XScaleBL: hx = -1; hy = 1;  break;
        case XScaleBR: hx = 1;  hy = 1;  break;
        case XScaleT:  hy = -1; break;
        case XScaleB:  hy = 1;  break;
        case XScaleL:  hx = -1; break;
        case XScaleR:  hx = 1;  break;
        default: return;
        }
        const qreal hw0 = m_boxW0 / 2.0, hh0 = m_boxH0 / 2.0;
        // The anchor is the OPPOSITE handle; it stays fixed during the scale.
        const QPointF anchor =
            m_boxCenter0 + rotVec(m_boxAngle0, QPointF(-hx * hw0, -hy * hh0));
        // Cursor in the anchor's axis-aligned frame.
        const QPointF cur = rotVec(-m_boxAngle0, canvasPos - anchor);
        qreal newW = (hx != 0.0) ? qMax(kMinBox, qAbs(cur.x())) : m_boxW0;
        qreal newH = (hy != 0.0) ? qMax(kMinBox, qAbs(cur.y())) : m_boxH0;
        if (proportional && hx != 0.0 && hy != 0.0) {
            const qreal f = qMax(newW / m_boxW0, newH / m_boxH0);
            newW = m_boxW0 * f;
            newH = m_boxH0 * f;
        }
        // New centre = anchor + half the new box toward the dragged handle;
        // along a free (edge) axis the centre keeps its original projection.
        const QPointF center0Local = rotVec(-m_boxAngle0, m_boxCenter0 - anchor);
        const qreal offx = (hx != 0.0) ? hx * newW / 2.0 : center0Local.x();
        const qreal offy = (hy != 0.0) ? hy * newH / 2.0 : center0Local.y();
        m_boxCenter = anchor + rotVec(m_boxAngle0, QPointF(offx, offy));
        m_boxW = newW;
        m_boxH = newH;
        m_boxAngle = m_boxAngle0;
        break;
    }
    }
    update();
}

void DrawingCanvas::updateXformCursor(XformMode mode)
{
    switch (mode) {
    case XMove:    setCursor(Qt::SizeAllCursor); break;
    case XRotate:  setCursor(m_rotateCursor); break;
    case XScaleTL:
    case XScaleBR: setCursor(Qt::SizeFDiagCursor); break;
    case XScaleTR:
    case XScaleBL: setCursor(Qt::SizeBDiagCursor); break;
    case XScaleT:
    case XScaleB:  setCursor(Qt::SizeVerCursor); break;
    case XScaleL:
    case XScaleR:  setCursor(Qt::SizeHorCursor); break;
    default:       setCursor(Qt::CrossCursor); break;
    }
}

// Bake the transformed buffer into the layer ONCE (start from the pristine
// backup, clear the source, paste the transformed buffer clipped only by the
// canvas). Exactly one undo entry = the pristine pre-transform layer.
void DrawingCanvas::commitTransform()
{
    if (!m_xformActive)
        return;
    m_xformActive = false;
    m_xformMode = XNone;
    setMouseTracking(false);

    Layer *layer = editableActiveLayer();
    if (layer && !m_layerBackup.isNull()) {
        layer->image = m_layerBackup; // restore pristine so the undo snapshot is the original
        pushUndo();

        QImage result = m_layerBackup;
        {
            QPainter c(&result);
            c.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            c.drawImage(m_moveSrcRect.topLeft(), m_moveMask); // clear source
        }
        {
            QPainter p(&result); // paste the transformed buffer (canvas-bounded)
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setTransform(boxTransform()); // buffer -> canvas; result IS canvas-sized
            p.drawImage(0, 0, m_transformBuf);
        }
        layer->image = result;
        emit contentChanged();
    }

    m_transformBuf = QImage();
    m_layerBackup = QImage();
    m_moveMask = QImage();
    clearSelection(); // box + selection gone after a commit
    setCursor(Qt::CrossCursor);
    update();
}

void DrawingCanvas::cancelTransform()
{
    if (!m_xformActive)
        return;
    m_xformActive = false;
    m_xformMode = XNone;
    setMouseTracking(false);

    Layer *layer = editableActiveLayer();
    if (layer && !m_layerBackup.isNull())
        layer->image = m_layerBackup; // restore EXACTLY, discard the transform

    m_transformBuf = QImage();
    m_layerBackup = QImage();
    m_moveMask = QImage();
    setCursor(Qt::CrossCursor);
    updateAntsTimer(); // the selection outline remains
    update();
    emit contentChanged();
}

void DrawingCanvas::floodFill(const QPoint &seed)
{
    Layer *layerPtr = editableActiveLayer();
    if (!layerPtr)
        return;

    QImage image = layerPtr->image.convertToFormat(QImage::Format_ARGB32);
    if (!image.rect().contains(seed))
        return;
    // With an active selection the fill is confined to the mask: a seed
    // outside it is a no-op, and the result is written back clipped below.
    if (!m_selPath.isEmpty() && !m_selPath.contains(QPointF(seed)))
        return;

    const QRgb target = image.pixel(seed);
    const QRgb replacement = m_color.rgb() | 0xff000000;
    if (target == replacement)
        return;

    const int w = image.width();
    const int h = image.height();
    QStack<QPoint> stack;
    stack.push(seed);

    while (!stack.isEmpty()) {
        const QPoint p = stack.pop();
        if (image.pixel(p) != target)
            continue;

        int left = p.x();
        while (left > 0 && image.pixel(left - 1, p.y()) == target)
            --left;
        int right = p.x();
        while (right < w - 1 && image.pixel(right + 1, p.y()) == target)
            ++right;

        for (int x = left; x <= right; ++x) {
            image.setPixel(x, p.y(), replacement);
            if (p.y() > 0 && image.pixel(x, p.y() - 1) == target)
                stack.push(QPoint(x, p.y() - 1));
            if (p.y() < h - 1 && image.pixel(x, p.y() + 1) == target)
                stack.push(QPoint(x, p.y() + 1));
        }
    }

    if (m_selPath.isEmpty()) {
        layerPtr->image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    } else {
        // Selection active: only pixels inside the mask take the fill;
        // everything outside stays untouched.
        QPainter p(&layerPtr->image);
        p.setClipPath(m_selPath);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(0, 0, image);
        p.end();
    }
    update();
}

// --- Brush engine (stamp-based, pressure-aware) -----------------------------

// One radial-gradient dab: solid colour core out to `hardness` of the radius,
// then a falloff to fully transparent at the edge. Composited SourceOver.
void DrawingCanvas::stampDab(const QPointF &center, qreal pressure)
{
    Layer *layer = editableActiveLayer();
    if (!layer)
        return;

    pressure = qBound<qreal>(0.0, pressure, 1.0);
    const qreal alphaFactor = m_pressureToOpacity ? pressure : 1.0;

    // Pressure-scaled radius with a hard 1.0px floor: however light the
    // stylus touch, a dab is never smaller than the constant stamp spacing
    // allows, so consecutive dabs keep overlapping into a continuous line
    // instead of separating into dots.
    const qreal baseRadius = m_brushToolSize / 2.0;
    const qreal radius = m_pressureToSize
        ? qMax<qreal>(1.0, baseRadius * pressure)
        : qMax<qreal>(0.5, baseRadius);
    const qreal alpha = qBound(0.0, m_brushToolOpacity * alphaFactor, 1.0);
    if (alpha <= 0.0)
        return;

    QColor core = m_color;
    core.setAlphaF(alpha);
    QColor edge = m_color;
    edge.setAlphaF(0.0);

    // Hardness is remapped so the transparent falloff always occupies at least
    // the outer ~1.5px of the radius: hardness 1.0 starts the falloff at
    // (radius - 1.5px), hardness 0.0 near the centre. Even the hardest brush
    // keeps an anti-aliased rim, so densely spaced dabs fuse into one smooth
    // edge instead of beading into visible stamp outlines.
    const qreal maxCore = qMax<qreal>(0.0, (radius - 1.5) / radius);
    const qreal coreStop = qBound<qreal>(0.0, m_brushHardness * maxCore, 0.995);

    QRadialGradient gradient(center, radius);
    gradient.setColorAt(0.0, core);
    if (coreStop > 0.0)
        gradient.setColorAt(coreStop, core); // solid core ends here
    gradient.setColorAt(1.0, edge);          // guaranteed feathered rim

    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawEllipse(center, radius, radius);
}

void DrawingCanvas::beginBrushStroke(const QPointF &canvasPt, qreal pressure)
{
    m_lastBrushPt = canvasPt;
    m_lastBrushPressure = pressure;
    m_stampResidual = 0.0;
    stampDab(canvasPt, pressure); // dab on the initial press
    update();
}

void DrawingCanvas::moveBrushStroke(const QPointF &canvasPt, qreal pressure)
{
    const QPointF delta = canvasPt - m_lastBrushPt;
    const double dist = std::hypot(delta.x(), delta.y());
    if (dist <= 0.0)
        return;

    // Stamps spaced at 5% of the BASE brush size (the slider value, never the
    // pressure-scaled size): dab density is constant at any pressure - light
    // strokes place just as many dabs as heavy ones, only smaller/fainter.
    // Pressure is interpolated per dab, and the residual-driven loop below
    // fills arbitrarily large gaps between input points, so fast strokes stay
    // just as continuous. (The 0.5px floor only stops tiny brushes from
    // stamping several dabs per pixel.)
    double step = qMax(0.5, m_brushToolSize * 0.05);

    // At light pressure the dabs shrink; never let the spacing exceed the
    // segment's smallest effective dab radius, so consecutive dabs always
    // overlap (continuous thin line instead of separated dots).
    if (m_pressureToSize) {
        const qreal minPressure =
            qBound<qreal>(0.0, qMin(m_lastBrushPressure, pressure), 1.0);
        const double effRadius = qMax<qreal>(1.0, (m_brushToolSize / 2.0) * minPressure);
        step = qMin(step, effRadius);
    }
    const QPointF dir = delta / dist;

    double since = m_stampResidual; // distance travelled since the last dab
    double consumed = 0.0;
    while (since + (dist - consumed) >= step) {
        consumed += step - since;
        since = 0.0;
        const qreal t = consumed / dist;
        const qreal p = m_lastBrushPressure + (pressure - m_lastBrushPressure) * t;
        stampDab(m_lastBrushPt + dir * consumed, p);
    }
    m_stampResidual = since + (dist - consumed);

    m_lastBrushPt = canvasPt;
    m_lastBrushPressure = pressure;
    update();
}

void DrawingCanvas::endBrushStroke()
{
    m_brushStroke = false;
    update();
}

void DrawingCanvas::undo()
{
    if (!m_panel)
        return;
    // Pop entries until one still matches a live layer (the layer of an older
    // snapshot may have been deleted since).
    while (!m_panel->undoStack.isEmpty()) {
        const LayerUndoEntry entry = m_panel->undoStack.takeLast();
        for (Layer &layer : m_panel->layers) {
            if (layer.id == entry.layerId) {
                layer.image = entry.image;
                update();
                emit contentChanged();
                return;
            }
        }
    }
}

void DrawingCanvas::clearCanvas()
{
    Layer *layer = editableActiveLayer();
    if (!layer)
        return;
    pushUndo();
    layer->image.fill(Qt::transparent); // clears the ACTIVE layer only
    update();
    emit contentChanged();
}

// Drag the View Controls toolbar by its grip, clamped inside the canvas.
bool DrawingCanvas::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_viewGrip && m_viewToolbar) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_viewDragging = true;
                m_viewDragStart = me->globalPosition().toPoint();
                m_viewToolbarStart = m_viewToolbar->pos();
                m_viewGrip->setCursor(Qt::ClosedHandCursor);
            }
            return true;
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (m_viewDragging && (me->buttons() & Qt::LeftButton)) {
                const QPoint delta = me->globalPosition().toPoint() - m_viewDragStart;
                QPoint p = m_viewToolbarStart + delta;
                p.setX(qBound(0, p.x(), qMax(0, width() - m_viewToolbar->width())));
                p.setY(qBound(0, p.y(), qMax(0, height() - m_viewToolbar->height())));
                m_viewToolbar->move(p);
            }
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                m_viewDragging = false;
                m_viewGrip->setCursor(Qt::OpenHandCursor);
            }
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(object, event);
}

void DrawingCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#0a0a0a"));

    if (!m_panel) {
        painter.setPen(QColor("#555555"));
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("Select a panel to start drawing"));
        return;
    }

    const QSize cs = canvasSize();
    const QRectF canvasR(0, 0, cs.width(), cs.height());
    const QRect canvasRi(0, 0, cs.width(), cs.height());
    const QTransform T = viewTransform(); // canvas -> widget (zoom+pan+rotate+flip)
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Everything canvas-anchored is drawn in CANVAS coordinates through T, so
    // it zooms/rotates/flips together with the artwork.
    painter.save();
    painter.setWorldTransform(T);

    // White paper, then every VISIBLE layer bottom-to-top with its opacity.
    // Light-table ghosts are drawn ONCE on the paper, just after the
    // background/paper layer and before the drawing layers.
    painter.fillRect(canvasR, Qt::white);
    bool lightTableDrawn = false;
    for (const Layer &layer : m_panel->layers) {
        if (m_lightTable && !lightTableDrawn
            && layer.type != QLatin1String("background")) {
            drawLightTable(painter, canvasRi);
            lightTableDrawn = true;
        }
        if (!layer.visible || layer.image.isNull() || layer.opacity <= 0.0)
            continue;
        painter.setOpacity(qBound(0.0, layer.opacity, 1.0));
        painter.drawImage(0, 0, layer.image);
    }
    painter.setOpacity(1.0);
    if (m_lightTable && !lightTableDrawn) // panel had only a background layer
        drawLightTable(painter, canvasRi);

    // Onion skin: faint blue ghost of the previous panel (display only).
    if (m_onionSkin && !m_ghost.isNull()) {
        painter.setOpacity(0.30);
        painter.drawPixmap(canvasRi, m_ghost);
        painter.setOpacity(1.0);
    }

    // Cosmetic pens: 1px on screen regardless of zoom/rotation.
    auto cosmetic = [](const QColor &c, qreal w = 0.0) {
        QPen p(c, w);
        p.setCosmetic(true);
        return p;
    };
    painter.setPen(cosmetic(QColor("#2a2a2a")));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(canvasR);

    // Alignment grid (View > Grid): thin white lines every 40 canvas px.
    if (m_grid && 40.0 * scale() >= 4.0) {
        painter.setPen(cosmetic(QColor(255, 255, 255, 20)));
        for (double x = 40.0; x < cs.width(); x += 40.0)
            painter.drawLine(QPointF(x, 0), QPointF(x, cs.height()));
        for (double y = 40.0; y < cs.height(); y += 40.0)
            painter.drawLine(QPointF(0, y), QPointF(cs.width(), y));
    }

    // Camera-frame outline (the 16:9 shot). The dim OUTSIDE it is drawn in
    // widget space below, since it spans the rotated frame's complement.
    if (m_cameraFrame) {
        painter.setPen(cosmetic(QColor("#cccccc")));
        painter.drawRect(canvasR);
    }

    // Action-safe: 5% inset, amber.
    if (m_safeArea) {
        const QRectF r = canvasR.adjusted(cs.width() * 0.05, cs.height() * 0.05,
                                          -cs.width() * 0.05, -cs.height() * 0.05);
        painter.setPen(cosmetic(QColor(0xf5, 0xa6, 0x23, m_actionSafeMaskPct * 255 / 100)));
        painter.drawRect(r);
        QFont f = font();
        f.setPointSize(7);
        painter.setFont(f);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("ACTION SAFE"));
    }

    // Title-safe: 10% inset, blue.
    if (m_titleSafe) {
        const QRectF r = canvasR.adjusted(cs.width() * 0.10, cs.height() * 0.10,
                                          -cs.width() * 0.10, -cs.height() * 0.10);
        painter.setPen(cosmetic(QColor(0x4d, 0x9f, 0xff, m_titleSafeMaskPct * 255 / 100)));
        painter.drawRect(r);
        QFont f = font();
        f.setPointSize(7);
        painter.setFont(f);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("TITLE SAFE"));
    }

    // In-progress shape preview (canvas coords, through T).
    if (m_shapeDrag || (m_tool == Shapes && !m_polygonPts.isEmpty()))
        paintShapeGeometry(painter, false);

    // Floating pixels (mid-move or un-committed paste).
    if (m_floatActive && !m_floatImg.isNull())
        painter.drawImage(m_floatPos + m_floatDelta, m_floatImg);

    // Non-destructive transform preview: the PRISTINE buffer through the box
    // transform, still composed with the view transform T.
    if (m_xformActive && !m_transformBuf.isNull()) {
        painter.save();
        painter.setWorldTransform(boxTransform(), true); // compose onto T
        painter.drawImage(0, 0, m_transformBuf);
        painter.restore();
    }

    painter.restore(); // leave canvas space; overlays below are widget space

    // Camera-frame dim: the whole widget minus the rotated 16:9 quad.
    if (m_cameraFrame) {
        QPolygonF quad;
        quad << T.map(QPointF(0, 0)) << T.map(QPointF(cs.width(), 0))
             << T.map(QPointF(cs.width(), cs.height())) << T.map(QPointF(0, cs.height()));
        QPainterPath outside;
        outside.addRect(rect());
        QPainterPath inside;
        inside.addPolygon(quad);
        painter.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 102));
    }

    // Transform box outline + handles: WIDGET space (constant screen size),
    // corners mapped through T so the box tracks the rotated/flipped preview.
    if (m_xformActive && !m_transformBuf.isNull()) {
        painter.save();
        const QVector<QPointF> h = boxHandlesCanvas();
        QVector<QPointF> hw;
        for (const QPointF &c : h)
            hw.append(T.map(c));
        QPolygonF outline;
        outline << hw.at(0) << hw.at(1) << hw.at(2) << hw.at(3); // corners
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 120), 3));   // dark halo
        painter.drawPolygon(outline);
        painter.setPen(QPen(QColor(0xf5, 0xa6, 0x23), 1)); // amber box
        painter.drawPolygon(outline);
        for (const QPointF &p : hw) {                    // 8 handle squares
            const QRectF sq(p.x() - 4, p.y() - 4, 8, 8);
            painter.setBrush(Qt::white);
            painter.setPen(QPen(QColor(0x33, 0x33, 0x33), 1));
            painter.drawRect(sq);
        }
        painter.restore();
    }

    // Marching ants: selection outline / in-progress drag / floating bounds,
    // drawn in canvas space through T (rotate/flip with the view). Cosmetic
    // pens keep them 1px on screen.
    if (!m_xformActive && (!m_selPath.isEmpty() || m_selDrag || m_floatActive)) {
        painter.save();
        painter.setWorldTransform(T);

        QPainterPath ants;
        if (m_selDrag) {
            const QRectF box = QRectF(m_selStartC, m_selCurrentC).normalized();
            if (m_tool == SelectRect) {
                ants.addRect(box);
            } else if (m_tool == SelectEllipse) {
                ants.addEllipse(box);
            } else if (m_lassoPts.size() >= 2) {
                ants.addPolygon(QPolygonF(m_lassoPts));
            }
        } else if (m_floatActive) {
            if (m_floatFromPaste)
                ants.addRect(floatBounds());
            else
                ants = m_selPath.translated(m_floatDelta);
        } else {
            ants = m_selPath;
        }

        painter.setBrush(Qt::NoBrush);
        QPen underlay(Qt::white, 0);
        underlay.setCosmetic(true);
        painter.setPen(underlay);
        painter.drawPath(ants);
        QPen dashes(Qt::black, 0);
        dashes.setCosmetic(true);
        dashes.setDashPattern({4.0, 4.0});
        dashes.setDashOffset(m_antsPhase);
        painter.setPen(dashes);
        painter.drawPath(ants);
        painter.restore();
    }
}

void DrawingCanvas::wheelEvent(QWheelEvent *event)
{
    // Ctrl + wheel zooms, centred on the cursor position.
    if (event->modifiers() & Qt::ControlModifier) {
        const double steps = event->angleDelta().y() / 120.0;
        setZoom(m_zoom * std::pow(1.25, steps), event->position());
        event->accept();
        return;
    }
    event->ignore();
}

void DrawingCanvas::keyPressEvent(QKeyEvent *event)
{
    // Transform box: Enter commits (bakes once), Esc cancels (restores).
    if (m_xformActive) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commitTransform();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelTransform();
            return;
        }
    }
    // Shapes: Enter closes the in-progress polygon, Esc cancels any
    // in-progress shape without leaving artifacts.
    if (m_tool == Shapes) {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && !m_polygonPts.isEmpty()) {
            commitPolygon();
            return;
        }
        if (event->key() == Qt::Key_Escape && (m_shapeDrag || !m_polygonPts.isEmpty())) {
            cancelShape();
            return;
        }
    }
    // Floating paste: Enter commits, Esc discards. A plain selection: Esc
    // clears it.
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && m_floatActive && m_floatFromPaste) {
        commitFloating();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        if (m_floatActive && m_floatFromPaste) {
            cancelFloatingPaste();
            return;
        }
        if (!m_selPath.isEmpty() || m_selDrag) {
            clearSelection();
            return;
        }
    }
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = true;
        if (!m_panning)
            setCursor(Qt::OpenHandCursor);
        return;
    }
    QWidget::keyPressEvent(event);
}

void DrawingCanvas::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        if (!m_panning)
            setCursor(Qt::CrossCursor);
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void DrawingCanvas::focusOutEvent(QFocusEvent *event)
{
    // Losing focus mid-hold would otherwise leave the pan modifier stuck on.
    m_spaceHeld = false;
    if (!m_panning)
        setCursor(Qt::CrossCursor);
    QWidget::focusOutEvent(event);
}

void DrawingCanvas::mousePressEvent(QMouseEvent *event)
{
    // Pan: middle-button drag, or spacebar held + left drag.
    if (event->button() == Qt::MiddleButton
        || (m_spaceHeld && event->button() == Qt::LeftButton)) {
        m_panning = true;
        m_panStartScreen = event->pos();
        m_panStartOffset = m_panOffset;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (!m_panel || event->button() != Qt::LeftButton)
        return;

    // Transform box intercepts every left-click while Move is active, so its
    // handles/rotation zones work even where they fall outside the canvas
    // rect. A press on empty space (XNone) is swallowed but keeps the box.
    if (m_xformActive && m_tool == Move) {
        m_xformMode = hitTestBox(event->position());
        if (m_xformMode != XNone) {
            m_dragStartCanvas = toCanvasF(event->position());
            m_boxCenter0 = m_boxCenter;
            m_boxW0 = m_boxW;
            m_boxH0 = m_boxH;
            m_boxAngle0 = m_boxAngle;
            if (m_xformMode == XRotate)
                m_rotStart0 = std::atan2(m_dragStartCanvas.y() - m_boxCenter.y(),
                                         m_dragStartCanvas.x() - m_boxCenter.x());
        }
        return;
    }

    // Floating paste intercepts every left-click: inside it (Move tool)
    // starts dragging it, anywhere else commits it and swallows the click.
    if (m_floatActive && m_floatFromPaste) {
        const QPointF cpt = toCanvasF(event->position());
        if (m_tool == Move && floatBounds().contains(cpt)) {
            m_floatDragging = true;
            m_floatGrabC = cpt;
            m_floatGrabDelta = m_floatDelta;
        } else {
            commitFloating(); // click-away commits
        }
        return;
    }

    if (!displayRect().contains(event->pos()))
        return;
    if (!editableActiveLayer())
        return; // locked / hidden / missing layer: ignore strokes, no cursor change

    switch (m_tool) {
    case Eraser: {
        pushUndo();
        m_drawing = true;
        m_lastCanvas = toCanvas(event->pos());
        drawSegment(m_lastCanvas, m_lastCanvas, m_color); // dot on click (eraser clears)
        break;
    }
    case Brush: {
        // Mouse strokes carry no pressure: fixed 1.0 (tablets use tabletEvent).
        pushUndo();
        m_brushStroke = true;
        beginBrushStroke(toCanvasF(event->position()), 1.0);
        break;
    }
    case Shapes:
        if (m_shapeKind == ShapePolygon) {
            m_shapeCurrentC = toCanvasF(event->position());
            m_polygonPts.append(m_shapeCurrentC);
            setMouseTracking(true); // rubber segment follows the hover
        } else {
            m_shapeDrag = true;
            m_shapeStartC = m_shapeCurrentC = toCanvasF(event->position());
        }
        update();
        break;
    case Fill:
        pushUndo();
        floodFill(toCanvas(event->pos()));
        emit contentChanged();
        break;
    case SelectRect:
    case SelectEllipse:
        clearSelection(); // a bare click (degenerate drag) just clears
        m_selDrag = true;
        m_selStartC = m_selCurrentC = toCanvasF(event->position());
        updateAntsTimer();
        update();
        break;
    case Lasso:
        clearSelection();
        m_selDrag = true;
        m_lassoPts.clear();
        m_lassoPts.append(toCanvasF(event->position()));
        m_selCurrentC = m_lassoPts.first();
        updateAntsTimer();
        update();
        break;
    case Move:
        // With a selection, the transform box is active (handled by the
        // intercept above). Without one, Move does nothing.
        break;
    case Camera:
        break; // non-drawing tool: overlays are toggled from the Camera panel
    }
}

void DrawingCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_panOffset = m_panStartOffset + QPointF(event->pos() - m_panStartScreen);
        update();
        return;
    }
    if (m_xformActive && m_tool == Move) {
        if ((event->buttons() & Qt::LeftButton) && m_xformMode != XNone)
            applyXformDrag(toCanvasF(event->position()),
                           event->modifiers() & Qt::ShiftModifier);
        else
            updateXformCursor(hitTestBox(event->position())); // hover feedback
        return;
    }
    if (m_floatDragging) {
        // Whole-pixel deltas, CLAMPED so the buffer stays fully on-canvas.
        // The layer is exactly canvas-sized: pixels released past an edge
        // cannot be stored and drawImage() would destroy them at commit (the
        // "eaten corner" — proven by debug_4). Clamping keeps the moved art
        // flush against the edge instead, so it survives a round trip. The
        // preview uses the SAME clamp, so preview == commit.
        const QPointF raw = m_floatGrabDelta + (toCanvasF(event->position()) - m_floatGrabC);
        m_floatDelta = clampFloatDelta(QPointF(qRound(raw.x()), qRound(raw.y())));
        update(); // display-only: the layer is untouched until commit
        return;
    }
    if (m_selDrag) {
        m_selCurrentC = toCanvasF(event->position());
        if (m_tool == Lasso
            && (m_lassoPts.isEmpty()
                || QLineF(m_lassoPts.last(), m_selCurrentC).length() >= 1.0))
            m_lassoPts.append(m_selCurrentC);
        update(); // in-progress selection outline
        return;
    }
    if (m_shapeDrag || (m_tool == Shapes && !m_polygonPts.isEmpty())) {
        m_shapeCurrentC = toCanvasF(event->position());
        update(); // live preview
        return;
    }
    if (m_brushStroke) {
        moveBrushStroke(toCanvasF(event->position()), 1.0); // mouse: fixed pressure
        return;
    }
    if (m_drawing) {
        const QPoint p = toCanvas(event->pos());
        drawSegment(m_lastCanvas, p, m_color);
        m_lastCanvas = p;
    }
}

void DrawingCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning
        && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_panning = false;
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : Qt::CrossCursor);
        return;
    }
    if (m_brushStroke && event->button() == Qt::LeftButton) {
        endBrushStroke();
        emit contentChanged();
        return;
    }
    if (m_xformActive && m_tool == Move && event->button() == Qt::LeftButton) {
        m_xformMode = XNone; // end this handle drag; the box stays for more edits
        return;
    }
    if (m_floatDragging && event->button() == Qt::LeftButton) {
        m_floatDragging = false;
        if (m_moveActive)
            commitMoveDrag(); // the ONLY write of the whole move
        return;               // a paste keeps floating until click-away/Enter
    }
    if (m_selDrag && event->button() == Qt::LeftButton) {
        m_selDrag = false;
        m_selCurrentC = toCanvasF(event->position());
        QPainterPath path;
        // Snap rect/ellipse bounds to the pixel grid: the ants outline, the
        // mask, and the lifted pixels then agree EXACTLY (a fractional
        // outline shaves up to 1px of boundary art off the lift).
        const QRectF raw = QRectF(m_selStartC, m_selCurrentC).normalized();
        const QRectF box(QPointF(qRound(raw.left()), qRound(raw.top())),
                         QPointF(qRound(raw.right()), qRound(raw.bottom())));
        if (m_tool == SelectRect && (box.width() >= 2.0 || box.height() >= 2.0)) {
            path.addRect(box);
        } else if (m_tool == SelectEllipse && (box.width() >= 2.0 || box.height() >= 2.0)) {
            path.addEllipse(box);
        } else if (m_tool == Lasso && m_lassoPts.size() >= 3) {
            path.addPolygon(QPolygonF(m_lassoPts)); // closes on release
            path.closeSubpath();
        }
        m_selPath = path; // degenerate drags leave it empty = cleared
        m_lassoPts.clear();
        updateAntsTimer();
        update();
        return;
    }
    if (m_shapeDrag && event->button() == Qt::LeftButton) {
        m_shapeCurrentC = toCanvasF(event->position());
        commitDragShape();
        return;
    }
    if (m_drawing) {
        m_drawing = false;
        emit contentChanged();
    }
}

void DrawingCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double-click closes the in-progress polygon. (Its own press already
    // appended a vertex at this spot; commitPolygon() dedupes it.)
    if (m_tool == Shapes && m_shapeKind == ShapePolygon && !m_polygonPts.isEmpty()
        && event->button() == Qt::LeftButton) {
        commitPolygon();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

// Stylus input: real pressure for the Brush tool. Other tools ignore() so Qt
// synthesizes ordinary mouse events for them (Shapes/Fill/Erase unchanged);
// for Brush we accept() so the same stroke is NOT also delivered as a mouse
// event (which would double-draw).
void DrawingCanvas::tabletEvent(QTabletEvent *event)
{
    if (m_tool != Brush) {
        event->ignore();
        return;
    }

    switch (event->type()) {
    case QEvent::TabletPress:
        if (m_panel && displayRect().contains(event->position().toPoint())
            && editableActiveLayer()) {
            pushUndo();
            m_brushStroke = true;
            beginBrushStroke(toCanvasF(event->position()), event->pressure());
        }
        break;
    case QEvent::TabletMove:
        if (m_brushStroke)
            moveBrushStroke(toCanvasF(event->position()), event->pressure());
        break;
    case QEvent::TabletRelease:
        if (m_brushStroke) {
            endBrushStroke();
            emit contentChanged();
        }
        break;
    default:
        break;
    }
    event->accept();
}

void DrawingCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_panel && firstImageUrl(event->mimeData(), nullptr))
        event->acceptProposedAction();
}

void DrawingCanvas::dropEvent(QDropEvent *event)
{
    QString path;
    if (m_panel && firstImageUrl(event->mimeData(), &path) && importImage(path))
        event->acceptProposedAction();
}
