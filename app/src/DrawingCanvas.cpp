#include "DrawingCanvas.h"

#include "StoryboardModel.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
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
#include <QStack>
#include <QTabletEvent>
#include <QTimer>
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

    // Corner zoom control: "-" | "100%" | "+" (children, always over the
    // canvas). Same #161616 as the floating toolbar so the "-" button blends
    // in when the pill sits directly above it.
    const QString zoomBtn = QStringLiteral(
        "QPushButton { background-color: #161616; color: #cccccc; border: 1px solid #2a2a2a;"
        " border-radius: 3px; font-size: 10px; padding: 0; }"
        "QPushButton:hover { background-color: #262626; color: #ffffff; }");
    m_zoomOutButton = new QPushButton(QStringLiteral("-"), this);
    m_zoomOutButton->setFixedSize(22, 20);
    m_zoomOutButton->setToolTip(QStringLiteral("Zoom out"));
    m_zoomResetButton = new QPushButton(QStringLiteral("100%"), this);
    m_zoomResetButton->setFixedSize(46, 20);
    m_zoomResetButton->setToolTip(QStringLiteral("Reset zoom to 100% and recentre"));
    m_zoomInButton = new QPushButton(QStringLiteral("+"), this);
    m_zoomInButton->setFixedSize(22, 20);
    m_zoomInButton->setToolTip(QStringLiteral("Zoom in"));
    for (QPushButton *b : {m_zoomOutButton, m_zoomResetButton, m_zoomInButton}) {
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(zoomBtn);
        b->setFocusPolicy(Qt::NoFocus); // don't steal the canvas's space-pan focus
    }
    connect(m_zoomOutButton, &QPushButton::clicked, this,
            [this] { setZoom(m_zoom / 1.25, QPointF(width() / 2.0, height() / 2.0)); });
    connect(m_zoomInButton, &QPushButton::clicked, this,
            [this] { setZoom(m_zoom * 1.25, QPointF(width() / 2.0, height() / 2.0)); });
    connect(m_zoomResetButton, &QPushButton::clicked, this, [this] { resetView(); });

    // Marching ants: advance the dash offset while a selection or floating
    // paste is on screen (updateAntsTimer() starts/stops it).
    m_antsTimer = new QTimer(this);
    m_antsTimer->setInterval(150);
    connect(m_antsTimer, &QTimer::timeout, this, [this] {
        m_antsPhase = (m_antsPhase + 1) % 8;
        update();
    });
}

QSize DrawingCanvas::canvasSize()
{
    return QSize(960, 540); // 16:9
}

void DrawingCanvas::setActivePanel(Panel *panel)
{
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
    m_tool = tool;
    cancelShape(); // an in-progress shape never survives a tool switch
    update();      // the selection itself DOES survive (Select -> Move flow)
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
        updateZoomUi();
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

    updateZoomUi();
    update();
}

void DrawingCanvas::resetView()
{
    m_zoom = 1.0;
    m_panOffset = QPointF(0, 0);
    updateZoomUi();
    update();
}

void DrawingCanvas::updateZoomUi()
{
    if (m_zoomResetButton)
        m_zoomResetButton->setText(QStringLiteral("%1%").arg(qRound(m_zoom * 100)));
}

void DrawingCanvas::positionZoomControls()
{
    if (!m_zoomOutButton)
        return;
    const int y = height() - 28;
    m_zoomOutButton->move(8, y);
    m_zoomResetButton->move(8 + 22 + 4, y);
    m_zoomInButton->move(8 + 22 + 4 + 46 + 4, y);
    m_zoomOutButton->raise();
    m_zoomResetButton->raise();
    m_zoomInButton->raise();
}

void DrawingCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionZoomControls();
}

double DrawingCanvas::scale() const
{
    const int w = displayRect().width();
    return w > 0 ? w / double(canvasSize().width()) : 1.0;
}

QPoint DrawingCanvas::toCanvas(const QPoint &widgetPoint) const
{
    const QRect d = displayRect();
    const double s = scale();
    const QSize cs = canvasSize();
    int px = int((widgetPoint.x() - d.x()) / s);
    int py = int((widgetPoint.y() - d.y()) / s);
    px = qBound(0, px, cs.width() - 1);
    py = qBound(0, py, cs.height() - 1);
    return QPoint(px, py);
}

int DrawingCanvas::penWidth() const
{
    return qMax(1, qRound(m_brushSize / scale()));
}

// Float variant for the brush engine: sub-pixel dab placement, no clamping
// (dabs partially outside the image are clipped by QPainter automatically).
QPointF DrawingCanvas::toCanvasF(const QPointF &widgetPoint) const
{
    const QRect d = displayRect();
    const double s = scale();
    return QPointF((widgetPoint.x() - d.x()) / s, (widgetPoint.y() - d.y()) / s);
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

    const QRect d = displayRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // White paper, then every VISIBLE layer bottom-to-top with its opacity.
    painter.fillRect(d, Qt::white);
    for (const Layer &layer : m_panel->layers) {
        if (!layer.visible || layer.image.isNull() || layer.opacity <= 0.0)
            continue;
        painter.setOpacity(qBound(0.0, layer.opacity, 1.0));
        painter.drawImage(d, layer.image);
    }
    painter.setOpacity(1.0);

    // Onion skin: faint blue ghost of the previous panel on top of the layers
    // (display only — never written to any layer).
    if (m_onionSkin && !m_ghost.isNull()) {
        painter.setOpacity(0.30);
        painter.drawPixmap(d, m_ghost);
        painter.setOpacity(1.0);
    }

    painter.setPen(QPen(QColor("#2a2a2a"), 1));
    painter.drawRect(d.adjusted(0, 0, -1, -1));

    // Alignment grid (View > Grid): thin #ffffff lines at 8% opacity every
    // 40 canvas px, anchored to the image rect so it zooms/pans with the
    // artwork. Display-only — never written to a layer or flattenedPixmap().
    if (m_grid) {
        painter.setPen(QPen(QColor(255, 255, 255, 20), 1));
        const double step = 40.0 * scale();
        if (step >= 4.0) { // skip when zoomed out so far the grid would be noise
            for (double x = d.left() + step; x < d.right(); x += step)
                painter.drawLine(QPointF(x, d.top()), QPointF(x, d.bottom() + 1));
            for (double y = d.top() + step; y < d.bottom(); y += step)
                painter.drawLine(QPointF(d.left(), y), QPointF(d.right() + 1, y));
        }
    }

    // --- Viewport overlays: DISPLAY-ONLY, drawn after the layers and never
    // --- written to any layer or included in flattenedPixmap(). All are
    // --- anchored to the image rect `d`, so they zoom and pan with it.

    // Camera frame: the image is the 16:9 shot; dim everything outside it.
    if (m_cameraFrame) {
        const QColor dim(0, 0, 0, 102); // black @ 40%
        painter.fillRect(QRect(0, 0, width(), d.top()), dim);
        painter.fillRect(QRect(0, d.bottom() + 1, width(), height() - d.bottom() - 1), dim);
        painter.fillRect(QRect(0, d.top(), d.left(), d.height()), dim);
        painter.fillRect(QRect(d.right() + 1, d.top(), width() - d.right() - 1, d.height()), dim);
        painter.setPen(QPen(QColor("#cccccc"), 1));
        painter.drawRect(d.adjusted(0, 0, -1, -1));
    }

    // Action-safe: 5% inset, amber at the Preferences > Camera opacity.
    if (m_safeArea) {
        const int ix = qRound(d.width() * 0.05);
        const int iy = qRound(d.height() * 0.05);
        const QRect r = d.adjusted(ix, iy, -ix, -iy);
        const QColor amber(0xf5, 0xa6, 0x23, m_actionSafeMaskPct * 255 / 100);
        painter.setPen(QPen(amber, 1));
        painter.drawRect(r);
        QFont f = font();
        f.setPointSize(7);
        painter.setFont(f);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("ACTION SAFE"));
    }

    // Title-safe: 10% inset, blue at the Preferences > Camera opacity.
    if (m_titleSafe) {
        const int ix = qRound(d.width() * 0.10);
        const int iy = qRound(d.height() * 0.10);
        const QRect r = d.adjusted(ix, iy, -ix, -iy);
        const QColor blue(0x4d, 0x9f, 0xff, m_titleSafeMaskPct * 255 / 100);
        painter.setPen(QPen(blue, 1));
        painter.drawRect(r);
        QFont f = font();
        f.setPointSize(7);
        painter.setFont(f);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("TITLE SAFE"));
    }

    // In-progress shape preview, always on top of the composite. Painted
    // through the display transform so it matches the committed result
    // exactly; display-only until commit — cancelling leaves no artifacts.
    if (m_shapeDrag || (m_tool == Shapes && !m_polygonPts.isEmpty())) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipRect(d);
        painter.translate(d.topLeft());
        const double s = scale();
        painter.scale(s, s);
        paintShapeGeometry(painter, false);
        painter.restore();
    }

    // Floating pixels (mid-move or un-committed paste): display-only until
    // committed into the layer.
    if (m_floatActive && !m_floatImg.isNull()) {
        painter.save();
        painter.setClipRect(d);
        painter.translate(d.topLeft());
        const double s = scale();
        painter.scale(s, s);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(m_floatPos + m_floatDelta, m_floatImg);
        painter.restore();
    }

    // Marching ants: the selection outline (translated while its pixels are
    // mid-move), the in-progress selection drag, or the floating paste
    // bounds. White underlay + animated black dashes; cosmetic pens stay
    // 1px at any zoom.
    if (!m_selPath.isEmpty() || m_selDrag || m_floatActive) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setClipRect(d);
        painter.translate(d.topLeft());
        const double s = scale();
        painter.scale(s, s);

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
    case Move: {
        const QPointF cpt = toCanvasF(event->position());
        if (!m_selPath.isEmpty() && m_selPath.contains(cpt))
            beginMoveDrag(cpt); // deferred commit: layer untouched until release
        else
            clearSelection();   // Move click outside the selection clears it
        break;
    }
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
