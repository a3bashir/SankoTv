#include "DrawingCanvas.h"

#include "StoryboardModel.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImage>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QStack>
#include <QUrl>
#include <Qt>

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

// True if every pixel is white (RGB ignored alpha) — i.e. an untouched panel.
bool isBlankPixmap(const QPixmap &pixmap)
{
    if (pixmap.isNull())
        return true;
    const QImage img = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if ((line[x] | 0xff000000u) != 0xffffffffu)
                return false;
        }
    }
    return true;
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
    setCursor(Qt::CrossCursor);
    setMinimumHeight(220);
    setAcceptDrops(true); // import images by dropping files onto the canvas
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

bool DrawingCanvas::importImage(const QString &filePath)
{
    if (!m_panel)
        return false;

    QPixmap loaded(filePath);
    if (loaded.isNull()) {
        QMessageBox::warning(this, QStringLiteral("Import Image"),
                             QStringLiteral("Could not load the selected image."));
        return false;
    }

    // Warn before replacing an existing drawing (skip if the panel is blank).
    if (!isBlankPixmap(m_panel->pixmap)) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Import Image"),
            QStringLiteral("This will replace the current drawing. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return false;
    }

    pushUndo(); // so Ctrl+Z reverts to the previous drawing

    QPixmap result(canvasSize());
    result.fill(Qt::black); // pad with the canvas background
    {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = loaded.size();
        target.scale(canvasSize(), Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(result.rect().center());
        painter.drawPixmap(r, loaded);
    }
    m_panel->pixmap = result;

    update();
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

QRect DrawingCanvas::displayRect() const
{
    const QSize cs = canvasSize();
    const double s = qMin(width() / double(cs.width()), height() / double(cs.height()));
    const int w = int(cs.width() * s);
    const int h = int(cs.height() * s);
    return QRect((width() - w) / 2, (height() - h) / 2, w, h);
}

double DrawingCanvas::scale() const
{
    const int w = displayRect().width();
    return w > 0 ? w / double(canvasSize().width()) : 1.0;
}

QPoint DrawingCanvas::toPixmap(const QPoint &widgetPoint) const
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

void DrawingCanvas::pushUndo()
{
    if (!m_panel)
        return;
    m_panel->undoStack.append(m_panel->pixmap.copy());
    while (m_panel->undoStack.size() > 20)
        m_panel->undoStack.removeFirst();
}

void DrawingCanvas::drawSegment(const QPoint &from, const QPoint &to, const QColor &color)
{
    if (!m_panel)
        return;
    QPainter painter(&m_panel->pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
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
    if (!m_panel)
        return;

    QImage image = m_panel->pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
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

    m_panel->pixmap = QPixmap::fromImage(image);
    update();
}

void DrawingCanvas::undo()
{
    if (!m_panel || m_panel->undoStack.isEmpty())
        return;
    m_panel->pixmap = m_panel->undoStack.takeLast();
    update();
    emit contentChanged();
}

void DrawingCanvas::clearCanvas()
{
    if (!m_panel)
        return;
    pushUndo();
    m_panel->pixmap.fill(Qt::white);
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
    painter.drawPixmap(d, m_panel->pixmap);

    // Onion skin: faint blue ghost of the previous panel on top of the opaque
    // white canvas (display only — never written to Panel::pixmap).
    if (m_onionSkin && !m_ghost.isNull()) {
        painter.setOpacity(0.30);
        painter.drawPixmap(d, m_ghost);
        painter.setOpacity(1.0);
    }

    painter.setPen(QPen(QColor("#2a2a2a"), 1));
    painter.drawRect(d.adjusted(0, 0, -1, -1));

    if (m_previewLine) {
        QPen pen(m_color);
        pen.setWidth(m_brushSize);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(m_lineStart, m_lineCurrent);
    }
}

void DrawingCanvas::mousePressEvent(QMouseEvent *event)
{
    if (!m_panel || event->button() != Qt::LeftButton)
        return;
    if (!displayRect().contains(event->pos()))
        return;

    switch (m_tool) {
    case Brush:
    case Eraser: {
        pushUndo();
        m_drawing = true;
        m_lastPixmap = toPixmap(event->pos());
        const QColor c = (m_tool == Eraser) ? QColor(Qt::white) : m_color;
        drawSegment(m_lastPixmap, m_lastPixmap, c); // dot on click
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
        floodFill(toPixmap(event->pos()));
        emit contentChanged();
        break;
    }
}

void DrawingCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_previewLine) {
        m_lineCurrent = event->pos();
        update();
        return;
    }
    if (m_drawing) {
        const QPoint p = toPixmap(event->pos());
        const QColor c = (m_tool == Eraser) ? QColor(Qt::white) : m_color;
        drawSegment(m_lastPixmap, p, c);
        m_lastPixmap = p;
    }
}

void DrawingCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_previewLine) {
        m_previewLine = false;
        pushUndo();
        drawSegment(toPixmap(m_lineStart), toPixmap(event->pos()), m_color);
        emit contentChanged();
        return;
    }
    if (m_drawing) {
        m_drawing = false;
        emit contentChanged();
    }
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
