#include "DrawingCanvas.h"

#include "StoryboardModel.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QStack>
#include <QTabletEvent>
#include <QUrl>
#include <QWheelEvent>
#include <Qt>

#include <cmath>

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

    // Corner zoom control: "-" | "100%" | "+" (children, always over the canvas).
    const QString zoomBtn = QStringLiteral(
        "QPushButton { background-color: #1c1c1c; color: #cccccc; border: 1px solid #2a2a2a;"
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
}

QSize DrawingCanvas::canvasSize()
{
    return QSize(960, 540); // 16:9
}

void DrawingCanvas::setActivePanel(Panel *panel)
{
    m_panel = panel;
    m_drawing = false;
    m_previewLine = false;
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
    m_tool = tool;
    m_previewLine = false;
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

void DrawingCanvas::floodFill(const QPoint &seed)
{
    Layer *layerPtr = editableActiveLayer();
    if (!layerPtr)
        return;

    QImage image = layerPtr->image.convertToFormat(QImage::Format_ARGB32);
    if (!image.rect().contains(seed))
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

    layerPtr->image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
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
    const qreal sizeFactor = m_pressureToSize ? qMax<qreal>(0.05, pressure) : 1.0;
    const qreal alphaFactor = m_pressureToOpacity ? pressure : 1.0;

    const qreal radius = qMax(0.5, (m_brushToolSize * sizeFactor) / 2.0);
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

    // Stamps spaced at 5% of the brush size - dense enough that individual
    // dabs disappear into a continuous edge. Pressure is interpolated per dab,
    // and the residual-driven loop below fills arbitrarily large gaps between
    // input points, so fast strokes stay just as continuous. (The 0.5px floor
    // only stops tiny brushes from stamping several dabs per pixel.)
    const double step = qMax(0.5, m_brushToolSize * 0.05);
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

    // Action-safe: 5% inset, amber @ 50%.
    if (m_safeArea) {
        const int ix = qRound(d.width() * 0.05);
        const int iy = qRound(d.height() * 0.05);
        const QRect r = d.adjusted(ix, iy, -ix, -iy);
        const QColor amber(0xf5, 0xa6, 0x23, 128);
        painter.setPen(QPen(amber, 1));
        painter.drawRect(r);
        QFont f = font();
        f.setPointSize(7);
        painter.setFont(f);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("ACTION SAFE"));
    }

    // Title-safe: 10% inset, blue @ 50%.
    if (m_titleSafe) {
        const int ix = qRound(d.width() * 0.10);
        const int iy = qRound(d.height() * 0.10);
        const QRect r = d.adjusted(ix, iy, -ix, -iy);
        const QColor blue(0x4d, 0x9f, 0xff, 128);
        painter.setPen(QPen(blue, 1));
        painter.drawRect(r);
        QFont f = font();
        f.setPointSize(7);
        painter.setFont(f);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft,
                         QStringLiteral("TITLE SAFE"));
    }

    // In-progress line preview, always on top of the composite.
    if (m_previewLine) {
        QPen pen(m_color);
        pen.setWidth(m_brushSize);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(m_lineStart, m_lineCurrent);
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
    if (!displayRect().contains(event->pos()))
        return;
    if (!editableActiveLayer())
        return; // locked / hidden / missing layer: ignore strokes, no cursor change

    switch (m_tool) {
    case Pen:
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
    case Line:
        m_previewLine = true;
        m_lineStart = event->pos();
        m_lineCurrent = event->pos();
        update();
        break;
    case Fill:
        pushUndo();
        floodFill(toCanvas(event->pos()));
        emit contentChanged();
        break;
    }
}

void DrawingCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_panOffset = m_panStartOffset + QPointF(event->pos() - m_panStartScreen);
        update();
        return;
    }
    if (m_previewLine) {
        m_lineCurrent = event->pos();
        update();
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
    if (m_previewLine) {
        m_previewLine = false;
        pushUndo();
        drawSegment(toCanvas(m_lineStart), toCanvas(event->pos()), m_color);
        emit contentChanged();
        return;
    }
    if (m_drawing) {
        m_drawing = false;
        emit contentChanged();
    }
}

// Stylus input: real pressure for the Brush tool. Other tools ignore() so Qt
// synthesizes ordinary mouse events for them (Pen/Line/Fill/Erase unchanged);
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
