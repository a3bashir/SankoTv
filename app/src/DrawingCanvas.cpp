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
#include <QHash>
#include <QStack>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>
#include <Qt>
#include <QtMath>

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

    // (The Canvas View Controls toolbar is the custom-painted ZoomToolbar,
    // created and wired by StoryboardPage. This canvas exposes the view
    // transform engine — setViewZoom/Rotation, toggleFlipH, viewZoomChanged.)

    // Marching ants: advance the dash offset while a selection or floating
    // paste is on screen (updateAntsTimer() starts/stops it).
    m_antsTimer = new QTimer(this);
    m_antsTimer->setInterval(150);
    connect(m_antsTimer, &QTimer::timeout, this, [this] {
        m_antsPhase = (m_antsPhase + 1) % 8;
        update();
    });

    // Rotate cursor for the transform box's rotation zones: a curved
    // DOUBLE-headed arrow in the same design language as the system resize
    // cursors the other handles use — white body with a black outline, same
    // weight. Drawn as one glyph twice: a fat black pass (the outline), then
    // the white body inset inside it.
    {
        QPixmap pm(26, 26);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF arcR(6.5, 6.5, 13.0, 13.0); // arc centreline circle
        const QPointF c = arcR.center();
        const qreal R = arcR.width() / 2.0;
        // Arrowhead triangle at an arc endpoint, tangent-aligned. dir = +1
        // points clockwise on screen, -1 anticlockwise. Grown for the outline
        // pass by scaling about its own centroid.
        auto headPoly = [&](qreal angleDeg, int dir, qreal grow) {
            const qreal a = qDegreesToRadians(angleDeg);
            const QPointF tip0(c.x() + R * qCos(a), c.y() - R * qSin(a));
            const QPointF tangent(dir * qSin(a), dir * qCos(a)); // screen coords
            const QPointF normal(-tangent.y(), tangent.x());
            QPolygonF tri({tip0 + tangent * 6.0,
                           tip0 - normal * 3.2, tip0 + normal * 3.2});
            const QPointF g = (tri[0] + tri[1] + tri[2]) / 3.0;
            for (QPointF &v : tri)
                v = g + (v - g) * grow;
            return tri;
        };
        for (int pass = 0; pass < 2; ++pass) {
            const QColor col = pass ? Qt::white : QColor(0, 0, 0);
            QPen pen(col, pass ? 2.4 : 4.6);
            pen.setCapStyle(Qt::FlatCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawArc(arcR, 205 * 16, -140 * 16); // sweep from 205deg to 65deg
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            p.drawPolygon(headPoly(205.0, -1, pass ? 1.0 : 1.45));
            p.drawPolygon(headPoly(65.0, 1, pass ? 1.0 : 1.45));
        }
        p.end();
        m_rotateCursor = QCursor(pm, 13, 13);
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

// Tight bounding rect of the non-transparent pixels (empty if the image is
// fully transparent). Used to auto-frame the Move tool's transform box.
static QRect opaquePixelBounds(const QImage &image)
{
    const QImage img = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    int minX = img.width(), minY = img.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            minX = qMin(minX, x);
            maxX = qMax(maxX, x);
            minY = qMin(minY, y);
            maxY = y; // rows scan top-to-bottom
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

void DrawingCanvas::setTool(Tool tool)
{
    commitFloating(); // an un-committed paste lands before the tool changes
    if (m_xformActive && tool != Move)
        commitTransform(false); // leaving Move finalises the box — no re-lift
    if (m_tool == SelectPoly && !m_lassoPts.isEmpty()) {
        m_lassoPts.clear(); // an un-closed polygon selection never survives
        setMouseTracking(false);
    }
    m_tool = tool;
    cancelShape(); // an in-progress shape never survives a tool switch
    // Activating Move shows the transform box at once (like Photoshop): a live
    // selection lifts the selected pixels; with NO selection the box wraps the
    // layer's whole artwork (synthesized rect selection, dropped on cancel).
    // An empty layer shows no box.
    if (tool == Move && !m_xformActive) {
        if (m_selPath.isEmpty()) {
            if (Layer *layer = editableActiveLayer()) {
                const QRect art = opaquePixelBounds(layer->image);
                if (!art.isEmpty()) {
                    QPainterPath path;
                    path.addRect(QRectF(art)); // QRect(P,P) width already spans maxX+1
                    m_selPath = path;
                    m_xformAutoSel = true;
                }
            }
        }
        if (!m_selPath.isEmpty())
            beginTransform();
    }
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
    m_panel->redoStack.clear(); // a fresh edit invalidates the redo branch
}

void DrawingCanvas::drawSegment(const QPoint &from, const QPoint &to, const QColor &color)
{
    Layer *layer = editableActiveLayer();
    if (!layer)
        return;
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // An active selection is a clip mask: the stroke only affects pixels
    // inside it (consistent with Brush and Fill).
    if (!m_selPath.isEmpty())
        painter.setClipPath(m_selPath);
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
    setMouseTracking(false); // drop any in-progress polygon-selection rubber
    updateAntsTimer();
    update();
}

// Close an in-progress polygon selection (double-click / Enter). Needs >= 3
// distinct vertices; the closing click's duplicate vertex is dropped.
void DrawingCanvas::closePolygonSelection()
{
    QVector<QPointF> pts = m_lassoPts;
    if (pts.size() >= 2
        && QLineF(pts.last(), pts.at(pts.size() - 2)).length() < 1.0)
        pts.removeLast(); // the double-click's second press landed on the first
    m_lassoPts.clear();
    setMouseTracking(false);
    if (pts.size() >= 3) {
        QPainterPath path;
        path.addPolygon(QPolygonF(pts));
        path.closeSubpath();
        m_selPath = combinedSelection(path); // Replace / Add / Subtract
    } else {
        m_selPath = combinedSelection(QPainterPath()); // too few: keep base (Add/Sub)
    }
    m_selBase = QPainterPath();
    recordSelectionChange(m_selGestureBase); // one history entry per polygon
    updateAntsTimer();
    update();
}

// Combine a freshly-drawn selection shape with the pre-drag selection
// (m_selBase) per the active operation.
QPainterPath DrawingCanvas::combinedSelection(const QPainterPath &shape) const
{
    switch (m_selOp) {
    case SelAdd:
        return m_selBase.united(shape);
    case SelSubtract:
        return m_selBase.subtracted(shape);
    default: // SelReplace
        return shape;
    }
}

void DrawingCanvas::setSelectionOp(SelectionOp op)
{
    m_selOp = op;
}

// --- Selection history (separate from the drawing undo/redo) ---------------

// Record a committed selection-region change: `before` is the selection as it
// was prior to the change now held in m_selPath. No-ops (unchanged region)
// are skipped so degenerate clicks never pollute the history.
void DrawingCanvas::recordSelectionChange(const QPainterPath &before)
{
    if (before == m_selPath)
        return;
    m_selUndoStack.append(before);
    while (m_selUndoStack.size() > 20)
        m_selUndoStack.removeFirst();
    m_selRedoStack.clear(); // a fresh change invalidates the redo branch
}

// Undo the last SELECTION change (region only — layer pixels are untouched;
// drawing has its own undo). Ignored mid-gesture.
void DrawingCanvas::undoSelection()
{
    if (m_xformActive || m_selDrag || m_selOutlineDrag || !m_lassoPts.isEmpty())
        return;
    if (m_selUndoStack.isEmpty())
        return;
    m_selRedoStack.append(m_selPath);
    m_selPath = m_selUndoStack.takeLast();
    updateAntsTimer();
    update();
}

void DrawingCanvas::redoSelection()
{
    if (m_xformActive || m_selDrag || m_selOutlineDrag || !m_lassoPts.isEmpty())
        return;
    if (m_selRedoStack.isEmpty())
        return;
    m_selUndoStack.append(m_selPath);
    m_selPath = m_selRedoStack.takeLast();
    updateAntsTimer();
    update();
}

// User-facing deselect (SelMod Deselect button / Esc): clears the selection
// AND records it in the selection history, so it can be selection-undone.
void DrawingCanvas::deselect()
{
    const QPainterPath before = m_selPath;
    clearSelection();
    recordSelectionChange(before);
}

// Move Modifier toolbar: switch the transform-box interaction mode. The live
// transform session (lifted buffer, quad, pivot) carries across mode switches;
// only commit (Enter) or cancel (Esc) ends it.
void DrawingCanvas::setXformUiMode(XformUiMode mode)
{
    m_xformUiMode = mode;
    update(); // the pivot marker shows/hides with the mode
}

// Selection Modifier "Move": dragging with a selection tool translates ONLY
// the selection outline. No artwork pixels are lifted, moved, or altered.
void DrawingCanvas::setSelectionOutlineMove(bool on)
{
    m_selOutlineMove = on;
    if (!on && m_selOutlineDrag) { // mode switched off mid-drag: end it cleanly
        m_selOutlineDrag = false;
        m_selOutlineBase = QPainterPath();
    }
    update();
}

// Select the whole active layer (the canvas rect).
void DrawingCanvas::selectAll()
{
    const QPainterPath before = m_selPath;
    QPainterPath path;
    path.addRect(QRectF(QPointF(0, 0), QSizeF(canvasSize())));
    m_selPath = path;
    m_selDrag = false;
    m_lassoPts.clear();
    recordSelectionChange(before);
    updateAntsTimer();
    update();
}

// Invert: the canvas rect minus the current selection (an empty selection
// inverts to the whole canvas).
void DrawingCanvas::invertSelection()
{
    const QPainterPath before = m_selPath;
    QPainterPath full;
    full.addRect(QRectF(QPointF(0, 0), QSizeF(canvasSize())));
    m_selPath = m_selPath.isEmpty() ? full : full.subtracted(m_selPath);
    m_selDrag = false;
    m_lassoPts.clear();
    recordSelectionChange(before);
    updateAntsTimer();
    update();
}

void DrawingCanvas::updateAntsTimer()
{
    const bool needed = !m_selPath.isEmpty() || m_selDrag || m_floatActive
        || (m_tool == SelectPoly && !m_lassoPts.isEmpty()); // building a polygon
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

// The selection path rasterized once into a mask, in boundingRect-local
// coordinates. Hard-edged (alpha strictly 0/255) by default; antialiased=true
// adds a soft 1px coverage falloff at the boundary — used by the Move tool's
// transform pipeline so moved artwork commits with clean, smooth edges.
// Either way, DestinationIn with it KEEPS exactly the masked coverage and
// DestinationOut CLEARS exactly the complement — alpha + (1-alpha) partitions
// the rect with no off-by-one and no double coverage.
QImage DrawingCanvas::selectionMask(const QRect &boundingRect, bool antialiased) const
{
    QImage mask(boundingRect.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, antialiased);
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
// --- Thin-plate-spline warp ---------------------------------------------
// Fit f(src) = affine(src) + sum wi * phi(|src - srci|), phi(r) = r^2 ln r,
// interpolating every control point's destination. The TPS is the smoothest
// (minimum bending energy) interpolant, so warped line art stays smooth and
// continuous — no per-triangle faceting. One (N+3)x(N+3) solve per mesh
// change (N is small), Gaussian elimination with partial pivoting.
void DrawingCanvas::solveWarpTps()
{
    const int n = m_warp.size();
    m_tpsValid = false;
    if (n < 3)
        return;
    const int m = n + 3;
    auto phi = [](qreal r2) { return r2 > 1e-12 ? 0.5 * r2 * std::log(r2) : 0.0; };
    // Augmented matrix [A | bx by], A = [K+lambda*I, P; P^T, 0].
    QVector<qreal> M(m * (m + 2), 0.0);
    auto at = [&](int r, int c) -> qreal & { return M[r * (m + 2) + c]; };
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            const QPointF d = m_warp.at(i).src - m_warp.at(j).src;
            at(i, j) = phi(d.x() * d.x() + d.y() * d.y());
        }
        at(i, i) += 1e-4; // tiny regularisation for numerical safety
        at(i, n) = 1.0;
        at(i, n + 1) = m_warp.at(i).src.x();
        at(i, n + 2) = m_warp.at(i).src.y();
        at(n, i) = 1.0;
        at(n + 1, i) = m_warp.at(i).src.x();
        at(n + 2, i) = m_warp.at(i).src.y();
        at(i, m) = m_warp.at(i).dst.x();
        at(i, m + 1) = m_warp.at(i).dst.y();
    }
    // Gaussian elimination, partial pivoting, two RHS columns at once.
    for (int col = 0; col < m; ++col) {
        int piv = col;
        for (int r = col + 1; r < m; ++r)
            if (qAbs(at(r, col)) > qAbs(at(piv, col)))
                piv = r;
        if (qAbs(at(piv, col)) < 1e-12)
            return; // singular (duplicate sources): keep the last valid fit
        if (piv != col)
            for (int c = 0; c < m + 2; ++c)
                std::swap(at(piv, c), at(col, c));
        for (int r = 0; r < m; ++r) {
            if (r == col)
                continue;
            const qreal f = at(r, col) / at(col, col);
            if (f == 0.0)
                continue;
            for (int c = col; c < m + 2; ++c)
                at(r, c) -= f * at(col, c);
        }
    }
    m_tpsX.resize(m);
    m_tpsY.resize(m);
    for (int r = 0; r < m; ++r) {
        m_tpsX[r] = at(r, m) / at(r, r);
        m_tpsY[r] = at(r, m + 1) / at(r, r);
    }
    m_tpsValid = true;
}

// Evaluate the spline: buffer coords -> canvas coords.
QPointF DrawingCanvas::warpMap(const QPointF &src) const
{
    const int n = m_warp.size();
    if (!m_tpsValid || m_tpsX.size() != n + 3)
        return boxTransform().map(src);
    qreal x = m_tpsX.at(n) + m_tpsX.at(n + 1) * src.x() + m_tpsX.at(n + 2) * src.y();
    qreal y = m_tpsY.at(n) + m_tpsY.at(n + 1) * src.x() + m_tpsY.at(n + 2) * src.y();
    for (int i = 0; i < n; ++i) {
        const QPointF d = src - m_warp.at(i).src;
        const qreal r2 = d.x() * d.x() + d.y() * d.y();
        const qreal k = r2 > 1e-12 ? 0.5 * r2 * std::log(r2) : 0.0;
        x += m_tpsX.at(i) * k;
        y += m_tpsY.at(i) * k;
    }
    return QPointF(x, y);
}

// The warp control point under the widget-space cursor, or -1.
int DrawingCanvas::warpPointAt(const QPointF &widgetPos) const
{
    const QTransform toWidget = viewTransform();
    for (int i = 0; i < m_warp.size(); ++i)
        if (QLineF(toWidget.map(m_warp.at(i).dst), widgetPos).length() <= 9.0)
            return i;
    return -1;
}

// Ctrl+click on the mesh: add a control point where clicked. Its SOURCE
// position is found by inverting the warp at the click point (damped
// fixed-point iteration seeded with the inverse quad map), so the new point
// pins the deformed surface exactly where it was clicked.
void DrawingCanvas::addWarpPointAt(const QPointF &widgetPos)
{
    const QPointF c = toCanvasF(widgetPos);
    if (warpPointAt(widgetPos) >= 0)
        return; // over an existing point: that's a remove, not an add
    const QRectF srcRect(0, 0, m_moveSrcRect.width(), m_moveSrcRect.height());
    const QTransform quadInv = boxTransform().inverted();
    QPointF src = quadInv.map(c); // good initial guess (exact when untouched)
    if (m_warpDirty && m_tpsValid) {
        // Newton-lite: step by the residual mapped back through the quad's
        // linear part. Converges fast for the mild-to-moderate warps a mesh
        // like this produces; bail out if it diverges.
        for (int it = 0; it < 12; ++it) {
            const QPointF res = c - warpMap(src);
            if (std::hypot(res.x(), res.y()) < 0.05)
                break;
            const QPointF step = quadInv.map(res + boxTransform().map(QPointF(0, 0)));
            src += 0.7 * step;
            if (!srcRect.adjusted(-200, -200, 200, 200).contains(src))
                break; // diverged: fall back to whatever we have
        }
    }
    src.setX(qBound(0.0, src.x(), srcRect.width()));
    src.setY(qBound(0.0, src.y(), srcRect.height()));
    // Refuse near-duplicates: coincident sources make the TPS singular.
    for (const WarpPt &w : std::as_const(m_warp))
        if (QLineF(w.src, src).length() < 3.0)
            return;
    m_warp.append({src, c});
    m_warpSel = {int(m_warp.size()) - 1}; // the fresh point becomes the selection
    m_warpDirty = true; // it pins the surface at the click: honour it exactly
    solveWarpTps();
    update();
}

// Ctrl+click on a control point (or Delete on a selection): remove it. The
// four SOURCE-corner anchors are protected — they keep the deformation
// anchored over the whole buffer.
void DrawingCanvas::removeWarpPoint(int index)
{
    if (index < 0 || index >= m_warp.size() || m_warp.size() <= 4)
        return;
    const qreal srcW = m_moveSrcRect.width(), srcH = m_moveSrcRect.height();
    const QPointF corners[4] = {{0, 0}, {srcW, 0}, {srcW, srcH}, {0, srcH}};
    for (const QPointF &corner : corners)
        if (QLineF(m_warp.at(index).src, corner).length() < 0.5)
            return; // corner anchor: not removable
    m_warp.removeAt(index);
    // Selection indices shift down past the removed one.
    QSet<int> sel;
    for (int i : std::as_const(m_warpSel))
        if (i != index)
            sel.insert(i > index ? i - 1 : i);
    m_warpSel = sel;
    solveWarpTps();
    update();
}

void DrawingCanvas::beginTransform()
{
    Layer *layer = editableActiveLayer();
    if (!layer || m_selPath.isEmpty())
        return;
    const QRect r = m_selPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;

    // Antialiased mask: the lift carries a soft 1px edge falloff, so the
    // committed result has smooth edges instead of jagged mask-cut stairs.
    m_moveMask = selectionMask(r, true);
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

    const QRectF rf(r);
    m_quad = QPolygonF({rf.topLeft(), rf.topRight(), rf.bottomRight(), rf.bottomLeft()});
    m_pivot = rf.center();
    m_pivotCustom = false;
    // Warp mesh: seeded as an even lattice over the lifted rect. src is
    // buffer-local; dst is the same lattice in canvas coords (identity warp).
    m_warp.clear();
    for (int j = 0; j < kWarpGrid; ++j)
        for (int i = 0; i < kWarpGrid; ++i) {
            const qreal fx = qreal(i) / (kWarpGrid - 1);
            const qreal fy = qreal(j) / (kWarpGrid - 1);
            m_warp.append({QPointF(rf.width() * fx, rf.height() * fy),
                           QPointF(rf.left() + rf.width() * fx,
                                   rf.top() + rf.height() * fy)});
        }
    solveWarpTps();
    m_warpSel.clear();
    m_warpDirty = false;
    m_warpIdx = -1;
    m_warpHoverIdx = -1;
    m_warpMarquee = false;
    m_xformActive = true;
    m_xformMode = XNone;
    setMouseTracking(true); // hover updates the scale/rotate cursor
    updateAntsTimer();
    update();
}

// Render the thin-plate-spline warp: the source buffer is subdivided into a
// DENSE grid of cells (cellPx source pixels each — far finer than the
// control lattice), grid nodes are mapped through the smooth TPS, and each
// cell is drawn as two seam-free micro-triangles. At this density the
// piecewise-affine deviation from the true spline is sub-pixel, so warped
// line art stays visually smooth and continuous — no faceting. Composes onto
// the painter's current transform, serving both the live preview (canvas
// space through the view transform) and the commit (layer space).
void DrawingCanvas::paintWarpedBuffer(QPainter &p, qreal cellPx) const
{
    const qreal srcW = m_moveSrcRect.width();
    const qreal srcH = m_moveSrcRect.height();
    if (srcW <= 0.0 || srcH <= 0.0 || !m_tpsValid)
        return;
    const int nx = qBound(2, qCeil(srcW / cellPx), 96);
    const int ny = qBound(2, qCeil(srcH / cellPx), 96);

    // Evaluate the spline once per grid node.
    QVector<QPointF> node((nx + 1) * (ny + 1));
    QVector<QPointF> snode((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) {
            const QPointF sp(srcW * i / nx, srcH * j / ny);
            snode[j * (nx + 1) + i] = sp;
            node[j * (nx + 1) + i] = warpMap(sp);
        }

    // Affine transform taking source triangle (s0,s1,s2) to (d0,d1,d2).
    auto triXform = [](const QPointF &s0, const QPointF &s1, const QPointF &s2,
                       const QPointF &d0, const QPointF &d1, const QPointF &d2,
                       QTransform *out) {
        const QPointF u = s1 - s0, v = s2 - s0, U = d1 - d0, V = d2 - d0;
        const qreal det = u.x() * v.y() - u.y() * v.x();
        if (qAbs(det) < 1e-9)
            return false;
        const qreal m11 = (U.x() * v.y() - V.x() * u.y()) / det;
        const qreal m12 = (U.y() * v.y() - V.y() * u.y()) / det;
        const qreal m21 = (V.x() * u.x() - U.x() * v.x()) / det;
        const qreal m22 = (V.y() * u.x() - U.y() * v.x()) / det;
        *out = QTransform(m11, m12, m21, m22,
                          d0.x() - (m11 * s0.x() + m21 * s0.y()),
                          d0.y() - (m12 * s0.x() + m22 * s0.y()));
        return true;
    };
    // One micro-triangle, clipped to its warped shape inflated by pushing
    // each EDGE outward 1.0px (vertices re-intersected, travel-clamped): the
    // overlap swallows the antialiased clip ramp so no seam hairlines
    // survive, and the +2px-expanded source rect keeps drawImage's boundary
    // sampling fed with real neighbouring pixels (a tight source rect is
    // what looked like "grid lines baked into the artwork").
    auto paintTri = [&](const QPointF &s0, const QPointF &s1, const QPointF &s2,
                        const QPointF &d0, const QPointF &d1, const QPointF &d2) {
        QTransform tf;
        if (!triXform(s0, s1, s2, d0, d1, d2, &tf))
            return;
        const QPointF v[3] = {d0, d1, d2};
        const QPointF c = (v[0] + v[1] + v[2]) / 3.0;
        QPolygonF tri;
        for (int k = 0; k < 3; ++k) {
            struct Line { QPointF p, d; };
            Line lines[2];
            for (int e = 0; e < 2; ++e) {
                const QPointF a = e == 0 ? v[(k + 2) % 3] : v[k];
                const QPointF b = e == 0 ? v[k] : v[(k + 1) % 3];
                const QPointF ab = b - a;
                const qreal len = std::hypot(ab.x(), ab.y());
                QPointF nrm = len > 1e-9 ? QPointF(ab.y() / len, -ab.x() / len)
                                         : QPointF(0, 0);
                if (QPointF::dotProduct(nrm, (a + b) / 2.0 - c) < 0)
                    nrm = -nrm; // outward
                lines[e] = {a + nrm * 1.0, ab};
            }
            const qreal den = lines[0].d.x() * lines[1].d.y()
                            - lines[0].d.y() * lines[1].d.x();
            QPointF nv = v[k];
            if (qAbs(den) > 1e-9) {
                const QPointF w = lines[1].p - lines[0].p;
                const qreal t = (w.x() * lines[1].d.y() - w.y() * lines[1].d.x()) / den;
                nv = lines[0].p + lines[0].d * t;
            }
            const QPointF travel = nv - v[k];
            const qreal tlen = std::hypot(travel.x(), travel.y());
            if (tlen > 6.0)
                nv = v[k] + travel * (6.0 / tlen);
            tri << nv;
        }
        QPainterPath clip;
        clip.addPolygon(tri);
        clip.closeSubpath();
        const QRectF srcBounds =
            QRectF(QPolygonF({s0, s1, s2}).boundingRect())
                .adjusted(-2, -2, 2, 2)
                .intersected(QRectF(0, 0, srcW, srcH));
        if (srcBounds.isEmpty())
            return;
        p.save();
        p.setClipPath(clip, Qt::IntersectClip);
        p.setTransform(tf, true);
        p.drawImage(srcBounds.topLeft(), m_transformBuf, srcBounds);
        p.restore();
    };

    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int i00 = j * (nx + 1) + i;
            const int i10 = i00 + 1;
            const int i01 = i00 + (nx + 1);
            const int i11 = i01 + 1;
            paintTri(snode.at(i00), snode.at(i10), snode.at(i11),
                     node.at(i00), node.at(i10), node.at(i11));
            paintTri(snode.at(i00), snode.at(i11), snode.at(i01),
                     node.at(i00), node.at(i11), node.at(i01));
        }
}

// Average luminance (0..255) of what the user SEES behind a canvas point:
// white paper + the visible layers + the live transform preview. Drives the
// pivot marker's adaptive colour (white over dark art, black over light).
qreal DrawingCanvas::luminanceBehind(const QPointF &canvasPt) const
{
    constexpr int R = 7; // half-extent of the sampled square, canvas px
    QImage img(2 * R, 2 * R, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white); // the paper
    QPainter p(&img);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.translate(R - canvasPt.x(), R - canvasPt.y());
    if (m_panel) {
        for (const Layer &layer : m_panel->layers) {
            if (!layer.visible || layer.image.isNull() || layer.opacity <= 0.0)
                continue;
            p.setOpacity(qBound(0.0, layer.opacity, 1.0));
            p.drawImage(0, 0, layer.image);
        }
    }
    p.setOpacity(1.0);
    if (m_xformActive && !m_transformBuf.isNull()) {
        if (m_warpDirty) {
            paintWarpedBuffer(p, 14.0);
        } else {
            p.save();
            p.setTransform(boxTransform(), true);
            p.drawImage(0, 0, m_transformBuf);
            p.restore();
        }
    }
    p.end();
    qreal sum = 0.0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = img.pixel(x, y);
            sum += 0.299 * qRed(px) + 0.587 * qGreen(px) + 0.114 * qBlue(px);
        }
    return sum / (img.width() * img.height());
}

// The rotate/scale origin: the user-placed pivot (Pivot Point mode), or the
// quad centre until one is set.
QPointF DrawingCanvas::pivotPoint() const
{
    if (m_pivotCustom)
        return m_pivot;
    return (m_quad.value(0) + m_quad.value(1) + m_quad.value(2) + m_quad.value(3)) / 4.0;
}

// Maps the pristine buffer (0..srcW, 0..srcH) onto the current quad in CANVAS
// coordinates — always from the ORIGINAL buffer. quadToQuad yields an affine
// transform for move/scale/rotate/skew quads and a projective one for
// distort/perspective; QPainter renders both (TxProject) with smooth filtering.
QTransform DrawingCanvas::boxTransform() const
{
    const qreal srcW = m_moveSrcRect.width();
    const qreal srcH = m_moveSrcRect.height();
    const QPolygonF src({QPointF(0, 0), QPointF(srcW, 0),
                         QPointF(srcW, srcH), QPointF(0, srcH)});
    QTransform t;
    if (m_quad.size() == 4 && srcW > 0.0 && srcH > 0.0
        && QTransform::quadToQuad(src, m_quad, t))
        return t;
    return QTransform(); // degenerate quad: identity (drags reject these)
}

// The 8 handle points (4 corners + 4 edge midpoints) in canvas coordinates,
// ordered TL, TR, BR, BL, T, R, B, L.
QVector<QPointF> DrawingCanvas::boxHandlesCanvas() const
{
    const QPointF tl = m_quad.value(0), tr = m_quad.value(1);
    const QPointF br = m_quad.value(2), bl = m_quad.value(3);
    return {tl, tr, br, bl,
            (tl + tr) / 2.0, (tr + br) / 2.0, (br + bl) / 2.0, (bl + tl) / 2.0};
}

// Which part of the box the widget-space point is over. Handles/rotation zones
// use constant SCREEN tolerances so they stay grabbable at any zoom.
DrawingCanvas::XformMode DrawingCanvas::hitTestBox(const QPointF &widgetPos) const
{
    if (!m_xformActive)
        return XNone;
    const QTransform toWidget = viewTransform(); // canvas -> widget (rotate/flip aware)

    // Warp mode: only the mesh control points are live handles (no
    // scale/rotate; empty-area presses start a marquee — see mousePress).
    if (m_xformUiMode == XformWarp) {
        const int hit = warpPointAt(widgetPos);
        if (hit >= 0) {
            m_warpIdx = hit;
            return XWarpPt;
        }
        return XNone;
    }

    // Pivot marker first (Pivot Point mode): it sits inside the body, so it
    // must win over the move hit.
    if (m_xformUiMode == XformPivot
        && QLineF(toWidget.map(pivotPoint()), widgetPos).length() <= 10.0)
        return XPivot;

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

// Recompute the quad from the press-time snapshot (never from the running
// result) for the active handle, honouring the Move Modifier mode. Matches
// the familiar Photoshop/Krita free-transform interactions:
//   Default      corner/edge scale, body move, outside-corner rotate
//   Pivot Point  rotate/scale about the user-placed pivot marker
//   Skew         side handles shear along their edge; corners still scale
//   Distort      corners drag freely; side handles carry their whole edge
//   Perspective  corner drags converge symmetrically (projective trapezoid)
// proportional (Shift) keeps the aspect ratio while corner-scaling.
void DrawingCanvas::applyXformDrag(const QPointF &canvasPos, bool proportional)
{
    const QPointF delta = canvasPos - m_dragStartCanvas;
    // Warp-point drags never touch the quad, so they must not depend on the
    // quad snapshot guard below (a warp drag can be the FIRST interaction).
    if (m_xformMode == XWarpPt) {
        if (m_warp0.size() == m_warp.size()) {
            for (int idx : std::as_const(m_warpSel))
                if (idx >= 0 && idx < m_warp.size())
                    m_warp[idx].dst = m_warp0.at(idx).dst + delta;
            m_warpDirty = true;
            solveWarpTps();
        }
        update();
        return;
    }
    if (m_quad0.size() != 4)
        return;

    // Local frame of the press-time quad: x along its top edge, y along its
    // left edge (unit vectors). Falls back to the world axes when degenerate.
    QPointF ux = m_quad0.at(1) - m_quad0.at(0);
    QPointF uy = m_quad0.at(3) - m_quad0.at(0);
    const qreal lx = std::hypot(ux.x(), ux.y());
    const qreal ly = std::hypot(uy.x(), uy.y());
    ux = lx > 1e-6 ? ux / lx : QPointF(1, 0);
    uy = ly > 1e-6 ? uy / ly : QPointF(0, 1);
    const QTransform B(ux.x(), ux.y(), uy.x(), uy.y(), 0.0, 0.0); // local -> canvas
    const QTransform Binv = B.inverted();

    // Handle description: index of the dragged corner (-1 = edge), the two
    // corners of a dragged edge, and the axis freedoms for scaling.
    int corner = -1, e0 = -1, e1 = -1;
    qreal hx = 0, hy = 0; // scale freedoms (sign = which side of the anchor)
    switch (m_xformMode) {
    case XScaleTL: corner = 0; hx = -1; hy = -1; break;
    case XScaleTR: corner = 1; hx = 1;  hy = -1; break;
    case XScaleBR: corner = 2; hx = 1;  hy = 1;  break;
    case XScaleBL: corner = 3; hx = -1; hy = 1;  break;
    case XScaleT:  e0 = 0; e1 = 1; hy = -1; break;
    case XScaleR:  e0 = 1; e1 = 2; hx = 1;  break;
    case XScaleB:  e0 = 2; e1 = 3; hy = 1;  break;
    case XScaleL:  e0 = 3; e1 = 0; hx = -1; break;
    default: break;
    }

    QPolygonF quad = m_quad0;
    switch (m_xformMode) {
    case XMove:
        quad.translate(delta);
        if (m_pivotCustom)
            m_pivot = m_pivot0 + delta; // the pivot rides with the box
        break;
    case XPivot:
        m_pivot = m_pivot0 + delta;
        m_pivotCustom = true;
        update();
        return; // pivot only — the quad is untouched
    case XRotate: {
        const qreal now = std::atan2(canvasPos.y() - m_pivot0.y(),
                                     canvasPos.x() - m_pivot0.x());
        const qreal deltaDeg = (now - m_rotStart0) * 180.0 / M_PI;
        for (int i = 0; i < 4; ++i)
            quad[i] = m_pivot0 + rotVec(deltaDeg, m_quad0.at(i) - m_pivot0);
        break;
    }
    default: {
        if (m_xformUiMode == XformDistort) {
            // Free quad: the corner (or the whole edge) follows the cursor.
            if (corner >= 0) {
                quad[corner] = m_quad0.at(corner) + delta;
            } else {
                quad[e0] = m_quad0.at(e0) + delta;
                quad[e1] = m_quad0.at(e1) + delta;
            }
            break;
        }
        if (m_xformUiMode == XformSkew && corner < 0) {
            // Shear: the edge slides along its own direction only (the
            // perpendicular component is discarded), opposite edge fixed.
            const QPointF d = Binv.map(delta);
            const QPointF slide = (e0 == 0 || e0 == 2) // T/B edges slide on x
                ? B.map(QPointF(d.x(), 0))
                : B.map(QPointF(0, d.y()));            // L/R edges slide on y
            quad[e0] = m_quad0.at(e0) + slide;
            quad[e1] = m_quad0.at(e1) + slide;
            break;
        }
        if (m_xformUiMode == XformPerspective && corner >= 0) {
            // Projective convergence: the dragged corner follows the cursor;
            // the neighbour on the same horizontal edge mirrors the x motion,
            // the neighbour on the same vertical edge mirrors the y motion
            // (classic Photoshop trapezoid / vanishing-point behaviour).
            static const int hNbr[4] = {1, 0, 3, 2}; // TL<->TR, BR<->BL
            static const int vNbr[4] = {3, 2, 1, 0}; // TL<->BL, TR<->BR
            const QPointF d = Binv.map(delta);
            quad[corner] = m_quad0.at(corner) + delta;
            quad[hNbr[corner]] = m_quad0.at(hNbr[corner]) - B.map(QPointF(d.x(), 0));
            quad[vNbr[corner]] = m_quad0.at(vNbr[corner]) - B.map(QPointF(0, d.y()));
            break;
        }
        // Scale (Default/Pivot modes; also Skew corners and Perspective
        // edges). Anchor = the user pivot when placed, else the opposite
        // handle. Factors are measured in the local frame; no flips (matches
        // the old box), floor keeps the box from collapsing.
        const QVector<QPointF> h0s = [this] {
            const QPointF tl = m_quad0.at(0), tr = m_quad0.at(1);
            const QPointF br = m_quad0.at(2), bl = m_quad0.at(3);
            return QVector<QPointF>{tl, tr, br, bl, (tl + tr) / 2.0,
                                    (tr + br) / 2.0, (br + bl) / 2.0, (bl + tl) / 2.0};
        }();
        int hIdx;
        switch (m_xformMode) {
        case XScaleTL: hIdx = 0; break;
        case XScaleTR: hIdx = 1; break;
        case XScaleBR: hIdx = 2; break;
        case XScaleBL: hIdx = 3; break;
        case XScaleT:  hIdx = 4; break;
        case XScaleR:  hIdx = 5; break;
        case XScaleB:  hIdx = 6; break;
        case XScaleL:  hIdx = 7; break;
        default: return;
        }
        // Opposite corner for a corner handle, opposite edge-mid for an edge.
        const QPointF anchorPt = m_pivotCustom
            ? m_pivot0
            : (hIdx < 4 ? h0s.at((hIdx + 2) % 4) : h0s.at(4 + ((hIdx - 4) + 2) % 4));
        const QPointF hLocal = Binv.map(h0s.at(hIdx) - anchorPt);
        const QPointF cLocal = Binv.map(canvasPos - anchorPt);
        qreal sx = 1.0, sy = 1.0;
        if (hx != 0.0 && qAbs(hLocal.x()) > 1e-6)
            sx = qMax(kMinBox / qMax(lx, kMinBox), qAbs(cLocal.x()) / qAbs(hLocal.x()));
        if (hy != 0.0 && qAbs(hLocal.y()) > 1e-6)
            sy = qMax(kMinBox / qMax(ly, kMinBox), qAbs(cLocal.y()) / qAbs(hLocal.y()));
        if (proportional && hx != 0.0 && hy != 0.0)
            sx = sy = qMax(sx, sy);
        for (int i = 0; i < 4; ++i) {
            const QPointF l = Binv.map(m_quad0.at(i) - anchorPt);
            quad[i] = anchorPt + B.map(QPointF(l.x() * sx, l.y() * sy));
        }
        break;
    }
    }

    // Accept only sane quads: convex, consistent winding, and above the
    // minimum area — quadToQuad needs this, and it stops the box from being
    // dragged inside-out (Photoshop rejects those states the same way).
    auto cross = [](const QPointF &o, const QPointF &a, const QPointF &b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    qreal area2 = 0.0;
    bool convex = true;
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const qreal c = cross(quad.at(i), quad.at((i + 1) % 4), quad.at((i + 2) % 4));
        if (c != 0.0) {
            const int s = c > 0.0 ? 1 : -1;
            if (sign == 0)
                sign = s;
            else if (s != sign)
                convex = false;
        }
        area2 += quad.at(i).x() * quad.at((i + 1) % 4).y()
               - quad.at((i + 1) % 4).x() * quad.at(i).y();
    }
    if (convex && qAbs(area2) / 2.0 >= kMinBox * kMinBox) {
        // Carry the warp mesh along with the quad edit: every control point's
        // DESTINATION rides through the same incremental transform, so a warp
        // survives later moves/scales/rotations (and vice versa). Source
        // positions never change, so the triangulation stays valid.
        QTransform inc;
        if (m_warp0.size() == m_warp.size()
            && QTransform::quadToQuad(m_quad0, quad, inc)) {
            for (int i = 0; i < m_warp.size(); ++i)
                m_warp[i].dst = inc.map(m_warp0.at(i).dst);
        }
        m_quad = quad;
        solveWarpTps(); // the spline follows the frame
    }
    update();
}

void DrawingCanvas::updateXformCursor(XformMode mode)
{
    switch (mode) {
    case XMove:    setCursor(Qt::SizeAllCursor); break;
    case XPivot:   setCursor(Qt::SizeAllCursor); break; // draggable pivot marker
    case XWarpPt:  setCursor(Qt::SizeAllCursor); break; // draggable mesh point
    case XRotate:  setCursor(m_rotateCursor); break;
    case XScaleTL:
    case XScaleBR: setCursor(Qt::SizeFDiagCursor); break;
    case XScaleTR:
    case XScaleBL: setCursor(Qt::SizeBDiagCursor); break;
    case XScaleT:
    case XScaleB:
        // Skew: the top/bottom edges SLIDE horizontally, so the arrows point
        // along the actual drag axis (scale keeps the perpendicular arrows).
        setCursor(m_xformUiMode == XformSkew ? Qt::SizeHorCursor
                                             : Qt::SizeVerCursor);
        break;
    case XScaleL:
    case XScaleR:
        // Skew: the left/right edges slide vertically.
        setCursor(m_xformUiMode == XformSkew ? Qt::SizeVerCursor
                                             : Qt::SizeHorCursor);
        break;
    default:       setCursor(Qt::CrossCursor); break;
    }
}

// Bake the transformed buffer into the layer ONCE (start from the pristine
// backup, clear the source, paste the transformed buffer — or the warped
// mesh — clipped only by the canvas). Exactly one undo entry = the pristine
// pre-transform layer. relift: while the Move tool remains active the box
// does not vanish — it resets to a fresh default axis-aligned box around the
// committed artwork (Photoshop behaviour).
void DrawingCanvas::commitTransform(bool relift)
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
            if (m_warpDirty) {
                paintWarpedBuffer(p, 5.0); // fine TPS mesh; result IS canvas-sized
            } else {
                p.setTransform(boxTransform()); // buffer -> canvas
                p.drawImage(0, 0, m_transformBuf);
            }
        }
        layer->image = result;
        emit contentChanged();
    }

    m_transformBuf = QImage();
    m_layerBackup = QImage();
    m_moveMask = QImage();
    m_warpDirty = false;
    m_warpSel.clear();
    m_warpMarquee = false;
    m_warpHoverIdx = -1;
    m_tpsValid = false;
    m_xformAutoSel = false;
    clearSelection(); // the committed pixels are no longer "selected"
    setCursor(Qt::CrossCursor);

    // Photoshop-style persistence: the box resets to a fresh axis-aligned
    // default around the committed artwork and stays up while Move is active.
    if (relift && m_tool == Move) {
        if (Layer *l = editableActiveLayer()) {
            const QRect art = opaquePixelBounds(l->image);
            if (!art.isEmpty()) {
                QPainterPath path;
                path.addRect(QRectF(art));
                m_selPath = path;
                m_xformAutoSel = true;
                beginTransform();
            }
        }
    }
    update();
}

void DrawingCanvas::cancelTransform(bool relift)
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
    m_warpDirty = false;
    m_warpSel.clear();
    m_warpMarquee = false;
    m_warpHoverIdx = -1;
    m_tpsValid = false;
    if (m_xformAutoSel) { // synthesized whole-artwork selection: drop it too
        m_xformAutoSel = false;
        m_selPath = QPainterPath();
    }
    setCursor(Qt::CrossCursor);

    // Esc while the Move tool stays active: the box resets to the default
    // around the restored artwork instead of disappearing.
    if (relift && m_tool == Move) {
        if (Layer *l = editableActiveLayer()) {
            if (m_selPath.isEmpty()) {
                const QRect art = opaquePixelBounds(l->image);
                if (!art.isEmpty()) {
                    QPainterPath path;
                    path.addRect(QRectF(art));
                    m_selPath = path;
                    m_xformAutoSel = true;
                }
            }
            if (!m_selPath.isEmpty())
                beginTransform();
        }
    }

    updateAntsTimer(); // a USER selection outline remains
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
        // Selection active: composite the fill through an ANTIALIASED selection
        // mask so the filled region has smooth edges. (A hard clip path — or a
        // binary mask — leaves jagged 1px stair-stepping at the boundary.)
        const QRect r = m_selPath.boundingRect().toAlignedRect().intersected(image.rect());
        if (!r.isEmpty()) {
            // Soft mask: antialiased path fill gives a 1px coverage falloff at
            // the selection boundary.
            QImage mask(r.size(), QImage::Format_ARGB32_Premultiplied);
            mask.fill(Qt::transparent);
            QPainter mp(&mask);
            mp.setRenderHint(QPainter::Antialiasing, true);
            mp.translate(-r.topLeft());
            mp.fillPath(m_selPath, Qt::white);
            mp.end();
            // The flood-filled pixels within the selection bbox, clipped to the
            // soft mask (alpha = mask coverage).
            QImage fill = image.copy(r).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QPainter fp(&fill);
            fp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            fp.drawImage(0, 0, mask);
            fp.end();
            // Blend it over the layer with smooth (antialiased) edges.
            QPainter p(&layerPtr->image);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(r.topLeft(), fill);
            p.end();
        }
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
    // An active selection is a clip mask: dabs only affect pixels inside it.
    // Outside the selection the brush has no effect (matches Erase and Fill).
    if (!m_selPath.isEmpty())
        painter.setClipPath(m_selPath);
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
                // The state being undone becomes redo-able.
                m_panel->redoStack.append({layer.id, layer.image.copy()});
                while (m_panel->redoStack.size() > 20)
                    m_panel->redoStack.removeFirst();
                layer.image = entry.image;
                update();
                emit contentChanged();
                return;
            }
        }
    }
}

// Re-apply the last undone drawing change. The redo branch survives only
// until the next fresh edit (pushUndo clears it).
void DrawingCanvas::redo()
{
    if (!m_panel)
        return;
    while (!m_panel->redoStack.isEmpty()) {
        const LayerUndoEntry entry = m_panel->redoStack.takeLast();
        for (Layer &layer : m_panel->layers) {
            if (layer.id == entry.layerId) {
                // The state being replaced goes back onto the undo stack
                // DIRECTLY (pushUndo would wipe the remaining redo branch).
                m_panel->undoStack.append({layer.id, layer.image.copy()});
                while (m_panel->undoStack.size() > 20)
                    m_panel->undoStack.removeFirst();
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
    // transform (or the piecewise warp mesh), still composed with the view
    // transform T.
    if (m_xformActive && !m_transformBuf.isNull()) {
        painter.save();
        if (m_warpDirty) {
            paintWarpedBuffer(painter, 14.0); // dense TPS mesh, composed onto T
        } else {
            painter.setWorldTransform(boxTransform(), true); // compose onto T
            painter.drawImage(0, 0, m_transformBuf);
        }
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
    // Photoshop-style: OUTLINE ONLY in the Sanko accent #7C6EF6, no fill.
    if (m_xformActive && !m_transformBuf.isNull()) {
        painter.save();
        const QColor accent(0x7c, 0x6e, 0xf6);
        painter.setBrush(Qt::NoBrush);

        if (m_xformUiMode == XformWarp) {
            // Warp: the mesh replaces the box. DISPLAY-ONLY overlay — never
            // composited into the layer. A CLEAN deformed grid (no
            // triangulation diagonals): the source-space lattice lines are
            // sampled densely and mapped through the smooth spline, so the
            // guides curve with the artwork without cluttering it.
            painter.setPen(QPen(QColor(accent.red(), accent.green(),
                                       accent.blue(), 170), 1));
            const qreal srcW = m_moveSrcRect.width();
            const qreal srcH = m_moveSrcRect.height();
            constexpr int kGuideSamples = 28;
            for (int axis = 0; axis < 2; ++axis) {
                for (int g = 0; g < kWarpGrid; ++g) {
                    const qreal f = qreal(g) / (kWarpGrid - 1);
                    QPolygonF line;
                    for (int st = 0; st <= kGuideSamples; ++st) {
                        const qreal t = qreal(st) / kGuideSamples;
                        const QPointF sp = axis == 0
                            ? QPointF(srcW * t, srcH * f)   // row
                            : QPointF(srcW * f, srcH * t);  // column
                        line << T.map(m_warpDirty && m_tpsValid
                                          ? warpMap(sp)
                                          : boxTransform().map(sp));
                    }
                    painter.drawPolyline(line);
                }
            }
            for (int i = 0; i < m_warp.size(); ++i) { // control points
                const QPointF p = T.map(m_warp.at(i).dst);
                // Hovered points grow slightly for grab feedback.
                const qreal r = i == m_warpHoverIdx ? 5.5 : 4.0;
                painter.setBrush(m_warpSel.contains(i) ? QBrush(accent)
                                                       : QBrush(Qt::white));
                painter.setPen(QPen(m_warpSel.contains(i) ? QColor(Qt::white)
                                                          : accent, 1.5));
                painter.drawEllipse(p, r, r);
            }
            if (m_warpMarquee) { // rubber-band selection box, widget space
                QPen dash(accent, 1);
                dash.setStyle(Qt::DashLine);
                painter.setPen(dash);
                painter.setBrush(QColor(accent.red(), accent.green(),
                                        accent.blue(), 30));
                painter.drawRect(QRectF(m_marqueeStartW, m_marqueeEndW).normalized());
            }
        } else {
            const QVector<QPointF> h = boxHandlesCanvas();
            QVector<QPointF> hw;
            for (const QPointF &c : h)
                hw.append(T.map(c));
            QPolygonF outline;
            outline << hw.at(0) << hw.at(1) << hw.at(2) << hw.at(3); // corners
            painter.setPen(QPen(accent, 1.5)); // accent outline, no fill
            painter.drawPolygon(outline);
            for (const QPointF &p : hw) { // 8 handles: white with accent ring
                const QRectF sq(p.x() - 4, p.y() - 4, 8, 8);
                painter.setBrush(Qt::white);
                painter.setPen(QPen(accent, 1.5));
                painter.drawRect(sq);
            }
        }
        // Pivot marker (Photoshop-style reference point): a ringed crosshair
        // at the rotate/scale origin. Shown while Pivot Point mode is active,
        // and kept visible in other modes once the user has placed it. The
        // marker colour ADAPTS to what is behind it — white over dark
        // artwork, black over light — re-sampled every repaint, so it stays
        // visible as it is dragged across different areas.
        if (m_xformUiMode == XformPivot || m_pivotCustom) {
            const QPointF pv = pivotPoint();
            const QPointF pw = T.map(pv);
            const QColor mark = luminanceBehind(pv) < 128.0 ? Qt::white : Qt::black;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(mark, 1.4));
            painter.drawEllipse(pw, 5.5, 5.5);
            painter.drawLine(QPointF(pw.x() - 9, pw.y()), QPointF(pw.x() - 3, pw.y()));
            painter.drawLine(QPointF(pw.x() + 3, pw.y()), QPointF(pw.x() + 9, pw.y()));
            painter.drawLine(QPointF(pw.x(), pw.y() - 9), QPointF(pw.x(), pw.y() - 3));
            painter.drawLine(QPointF(pw.x(), pw.y() + 3), QPointF(pw.x(), pw.y() + 9));
        }
        painter.restore();
    }

    // Marching ants: selection outline / in-progress drag / floating bounds,
    // drawn in canvas space through T (rotate/flip with the view). Cosmetic
    // pens keep them 1px on screen.
    const bool polyInProgress = m_tool == SelectPoly && !m_lassoPts.isEmpty();
    if (!m_xformActive
        && (!m_selPath.isEmpty() || m_selDrag || m_floatActive || polyInProgress)) {
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
        } else if (polyInProgress) {
            // Open polyline through the dropped vertices + a rubber segment to
            // the cursor (not closed until double-click/Enter).
            QPolygonF open(m_lassoPts);
            open.append(m_selCurrentC);
            ants.addPolygon(open);
        } else if (m_floatActive) {
            if (m_floatFromPaste)
                ants.addRect(floatBounds());
            else
                ants = m_selPath.translated(m_floatDelta);
        } else {
            ants = m_selPath;
        }
        // Add/Remove mode: the committed selection (m_selBase) stays visible
        // the whole time a new shape is being drawn on top of it, so the user
        // sees exactly what they are adding to / subtracting from.
        if ((m_selDrag || polyInProgress) && !m_selBase.isEmpty())
            ants.addPath(m_selBase);

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
    // Transform box: Enter commits (bakes once), Esc cancels (restores). In
    // both cases the box RESETS to a fresh default around the artwork while
    // the Move tool stays active (Photoshop behaviour). In Warp mode, Delete
    // removes the selected control points (corner anchors refuse).
    if (m_xformActive) {
        if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
            && m_xformUiMode == XformWarp && !m_warpSel.isEmpty()) {
            QList<int> sel = m_warpSel.values();
            std::sort(sel.begin(), sel.end(), std::greater<int>());
            for (int i : sel) // descending: removals don't shift what's left
                removeWarpPoint(i);
            update();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commitTransform();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelTransform(true);
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
    // Polygon selection: Enter closes the vertices into a selection, Esc
    // cancels the in-progress polygon (before it becomes a committed mask).
    if (m_tool == SelectPoly && !m_lassoPts.isEmpty()) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            closePolygonSelection();
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            m_lassoPts.clear();
            setMouseTracking(false);
            updateAntsTimer();
            update();
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
            deselect(); // recorded in the selection history
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
        // Warp mode has its own modifier-aware interactions.
        if (m_xformUiMode == XformWarp) {
            const int hit = warpPointAt(event->position());
            if (event->modifiers() & Qt::ControlModifier) {
                // Ctrl+click: edit the mesh topology — remove the point under
                // the cursor, or add one where the mesh was clicked.
                if (hit >= 0)
                    removeWarpPoint(hit);
                else
                    addWarpPointAt(event->position());
                return;
            }
            if (event->modifiers() & Qt::ShiftModifier) {
                // Shift+click: toggle the point in/out of the selection.
                if (hit >= 0) {
                    if (m_warpSel.contains(hit))
                        m_warpSel.remove(hit);
                    else
                        m_warpSel.insert(hit);
                    update();
                }
                return;
            }
            if (hit >= 0) {
                // Plain press on a point: drag it — together with the rest of
                // the selection if it belongs to one, alone otherwise.
                if (!m_warpSel.contains(hit))
                    m_warpSel = {hit};
                m_warpIdx = hit;
                m_xformMode = XWarpPt;
                m_dragStartCanvas = toCanvasF(event->position());
                m_warp0 = m_warp;
                m_quad0 = m_quad; // applyXformDrag's sanity guard reads it
                update();
                return;
            }
            // Empty area: rubber-band marquee to select multiple points.
            m_warpMarquee = true;
            m_marqueeStartW = m_marqueeEndW = event->position();
            m_xformMode = XWarpBox;
            update();
            return;
        }

        m_xformMode = hitTestBox(event->position());
        if (m_xformMode != XNone) {
            m_dragStartCanvas = toCanvasF(event->position());
            m_quad0 = m_quad;
            m_warp0 = m_warp;
            m_pivot0 = m_xformMode == XPivot ? m_pivot : pivotPoint();
            if (m_xformMode == XPivot && !m_pivotCustom)
                m_pivot0 = pivotPoint(); // start from the tracked centre
            if (m_xformMode == XRotate)
                m_rotStart0 = std::atan2(m_dragStartCanvas.y() - m_pivot0.y(),
                                         m_dragStartCanvas.x() - m_pivot0.x());
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

    // Selection Modifier "Move" mode: with a live selection, dragging any
    // selection tool translates the OUTLINE only (never pixels).
    if (m_selOutlineMove && !m_selPath.isEmpty()
        && (m_tool == SelectRect || m_tool == SelectEllipse || m_tool == Lasso
            || m_tool == SelectPoly)) {
        m_selOutlineDrag = true;
        m_selOutlineStartC = toCanvasF(event->position());
        m_selOutlineBase = m_selPath;
        return;
    }

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
        // Add/Subtract combine the new shape with the pre-drag selection;
        // Replace starts fresh. (A bare click = degenerate drag: Replace clears,
        // Add/Subtract leave the existing selection untouched.)
        m_selGestureBase = m_selPath; // selection-history "before" snapshot
        m_selBase = m_selOp == SelReplace ? QPainterPath() : m_selPath;
        clearSelection();
        m_selDrag = true;
        m_selStartC = m_selCurrentC = toCanvasF(event->position());
        updateAntsTimer();
        update();
        break;
    case Lasso:
        m_selGestureBase = m_selPath;
        m_selBase = m_selOp == SelReplace ? QPainterPath() : m_selPath;
        clearSelection();
        m_selDrag = true;
        m_lassoPts.clear();
        m_lassoPts.append(toCanvasF(event->position()));
        m_selCurrentC = m_lassoPts.first();
        updateAntsTimer();
        update();
        break;
    case SelectPoly:
        // Polygon selection: each click drops a vertex (no drag). The first
        // vertex captures the pre-drag selection (for Add/Subtract) and clears
        // the live one; a rubber segment follows the cursor until close.
        if (m_lassoPts.isEmpty()) {
            m_selGestureBase = m_selPath;
            m_selBase = m_selOp == SelReplace ? QPainterPath() : m_selPath;
            clearSelection();
        }
        m_lassoPts.append(toCanvasF(event->position()));
        m_selCurrentC = m_lassoPts.last();
        setMouseTracking(true); // rubber segment tracks the hover
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
        if ((event->buttons() & Qt::LeftButton) && m_xformMode == XWarpBox) {
            m_marqueeEndW = event->position(); // rubber band follows
            update();
            return;
        }
        if ((event->buttons() & Qt::LeftButton) && m_xformMode != XNone)
            applyXformDrag(toCanvasF(event->position()),
                           event->modifiers() & Qt::ShiftModifier);
        else {
            updateXformCursor(hitTestBox(event->position())); // hover feedback
            if (m_xformUiMode == XformWarp) { // enlarge the hovered point
                const int h = warpPointAt(event->position());
                if (h != m_warpHoverIdx) {
                    m_warpHoverIdx = h;
                    update();
                }
            }
        }
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
    if (m_selOutlineDrag) {
        // Outline-only move: whole-pixel deltas keep the region snapped to the
        // pixel grid, so a later lift/cut covers exactly what the ants show.
        const QPointF raw = toCanvasF(event->position()) - m_selOutlineStartC;
        m_selPath = m_selOutlineBase.translated(qRound(raw.x()), qRound(raw.y()));
        update(); // display only — no layer writes
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
    if (m_tool == SelectPoly && !m_lassoPts.isEmpty()) {
        m_selCurrentC = toCanvasF(event->position()); // rubber segment endpoint
        update();
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
        if (m_warpMarquee) { // marquee done: points inside become the selection
            m_warpMarquee = false;
            const QRectF r = QRectF(m_marqueeStartW, m_marqueeEndW).normalized();
            const QTransform toWidget = viewTransform();
            QSet<int> sel;
            for (int i = 0; i < m_warp.size(); ++i)
                if (r.contains(toWidget.map(m_warp.at(i).dst)))
                    sel.insert(i);
            m_warpSel = sel;
            update();
        }
        m_xformMode = XNone; // end this handle drag; the box stays for more edits
        return;
    }
    if (m_floatDragging && event->button() == Qt::LeftButton) {
        m_floatDragging = false;
        if (m_moveActive)
            commitMoveDrag(); // the ONLY write of the whole move
        return;               // a paste keeps floating until click-away/Enter
    }
    if (m_selOutlineDrag && event->button() == Qt::LeftButton) {
        m_selOutlineDrag = false; // the translated outline IS the selection now
        recordSelectionChange(m_selOutlineBase); // outline moves are undoable too
        m_selOutlineBase = QPainterPath();
        return;
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
        // Replace: the new shape (degenerate = cleared). Add/Subtract combine
        // it with the pre-drag selection (degenerate = base unchanged).
        m_selPath = combinedSelection(path);
        m_selBase = QPainterPath();
        m_lassoPts.clear();
        recordSelectionChange(m_selGestureBase); // one history entry per gesture
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
    // Warp: double-clicking a control point resets ONLY that point to its
    // original (un-warped, quad-mapped) position; every other point keeps its
    // deformation. The spline re-solves around the restored point.
    if (m_xformActive && m_tool == Move && m_xformUiMode == XformWarp
        && event->button() == Qt::LeftButton) {
        const int hit = warpPointAt(event->position());
        if (hit >= 0) {
            m_warp[hit].dst = boxTransform().map(m_warp.at(hit).src);
            m_warpDirty = false; // still dirty only if ANY point stays moved
            for (const WarpPt &w : std::as_const(m_warp))
                if (QLineF(w.dst, boxTransform().map(w.src)).length() > 0.1) {
                    m_warpDirty = true;
                    break;
                }
            solveWarpTps();
            m_xformMode = XNone; // cancel the drag the first press started
            update();
        }
        return;
    }

    // Double-click closes the in-progress polygon. (Its own press already
    // appended a vertex at this spot; commitPolygon() dedupes it.)
    if (m_tool == Shapes && m_shapeKind == ShapePolygon && !m_polygonPts.isEmpty()
        && event->button() == Qt::LeftButton) {
        commitPolygon();
        return;
    }
    // Double-click closes an in-progress polygon SELECTION.
    if (m_tool == SelectPoly && event->button() == Qt::LeftButton) {
        closePolygonSelection();
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
