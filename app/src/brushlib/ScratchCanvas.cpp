#include "ScratchCanvas.h"

#include "SankoPaintHostAdapter.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QTabletEvent>
#include <QTimer>

namespace brushlib {
namespace {

// Phase 2's smudge fixture colours (BrushPreviewRenderer): three opaque
// bands a smudge stroke visibly drags. Kept identical so the scratch pad
// and the row swatch tell the same story.
QImage smudgeBands(const QSize &size)
{
    QImage bg(size, QImage::Format_ARGB32);
    QPainter p(&bg);
    const int w = size.width();
    p.fillRect(0, 0, w / 3, size.height(), QColor(0xc0, 0x30, 0x30));
    p.fillRect(w / 3, 0, w / 3, size.height(), QColor(0x30, 0xc0, 0x50));
    p.fillRect(2 * (w / 3), 0, w - 2 * (w / 3), size.height(),
               QColor(0x30, 0x50, 0xc0));
    return bg;
}

// Black is the "follow the app colour" sentinel (Phase 3 identity-colour
// semantics) and would be invisible on the #161617 pad, so black brushes
// draw in a neutral light ink — the same legibility rule the preview
// renderer applies. Identity-coloured brushes keep their colour.
::Brush inkAdjusted(const ::Brush &brush)
{
    ::Brush b = brush;
    if (b.color() == QColor(Qt::black))
        b.setColor(QColor(0xe6, 0xe6, 0xe6));
    return b;
}

} // namespace

ScratchCanvas::ScratchCanvas(QWidget *parent)
    : QWidget(parent)
{
    setCursor(Qt::CrossCursor);
    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    connect(m_rebuildTimer, &QTimer::timeout, this,
            &ScratchCanvas::enqueueFullRender);
    m_thread = QThread::create([this] { workerLoop(); });
    m_thread->start(QThread::LowPriority);
}

ScratchCanvas::~ScratchCanvas()
{
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        m_wake.wakeAll();
    }
    m_thread->wait();
    delete m_thread;
}

void ScratchCanvas::setBrush(const ::Brush &brush, bool tipInvalidating)
{
    m_brush = brush;
    ++m_brushRevision;
    scheduleFullRender(tipInvalidating ? kTipDebounceMs : kEditDebounceMs);
}

void ScratchCanvas::clearStrokes()
{
    m_strokes.clear();
    m_active.clear();
    m_drawing = false;
    m_sampleLaid = false;
    ++m_epoch; // in-flight results for the old content must not surface
    m_baselineStrokes = -1;
    scheduleFullRender(0);
}

bool ScratchCanvas::hasVisibleInk() const
{
    if (m_display.isNull())
        return false;
    const QImage bg = backgroundImage(m_display.size());
    const QImage a = m_display.convertToFormat(QImage::Format_ARGB32);
    const QImage b = bg.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < a.height(); ++y)
        if (memcmp(a.constScanLine(y), b.constScanLine(y),
                   size_t(a.width()) * 4)
            != 0)
            return true;
    return false;
}

QImage ScratchCanvas::backgroundImage(const QSize &size) const
{
    if (m_brush.toolMode() == ::Brush::ToolMode::Smudge)
        return smudgeBands(size);
    QImage bg(size, QImage::Format_ARGB32);
    bg.fill(Qt::transparent);
    return bg;
}

quint64 ScratchCanvas::strokeSeed(int index)
{
    return 4242 + quint64(index) * 7919;
}

void ScratchCanvas::scheduleFullRender(int delayMs)
{
    if (delayMs <= 0) {
        m_rebuildTimer->stop();
        enqueueFullRender();
        return;
    }
    m_rebuildTimer->start(delayMs); // restart: trailing debounce
}

void ScratchCanvas::enqueueFullRender()
{
    if (!size().isValid() || width() < 2 || height() < 2)
        return;
    Job job;
    job.kind = JobKind::Full;
    job.brush = inkAdjusted(m_brush);
    job.size = size();
    job.strokes = m_strokes;
    job.brushRevision = m_brushRevision;
    job.strokeCountAtEnqueue = m_strokes.size();
    job.epoch = m_epoch;
    enqueue(std::move(job));
}

void ScratchCanvas::enqueue(Job job)
{
    QMutexLocker lock(&m_mutex);
    if (job.kind != JobKind::Commit) {
        // A newer live/full render supersedes its queued predecessor;
        // commits are never dropped.
        for (int i = m_queue.size() - 1; i >= 0; --i)
            if (m_queue.at(i).kind == job.kind)
                m_queue.removeAt(i);
    }
    m_queue.enqueue(std::move(job));
    m_wake.wakeOne();
}

void ScratchCanvas::laySampleStroke()
{
    if (m_sampleLaid || m_drawing)
        return; // idempotent until cleared; never interleave a live stroke
    // An S of two semicircles, sized to the pad: about 2*pi*r of arc (past
    // the 256 px default fade for any reasonable pad) and 360 degrees of
    // turning. Fixed geometry relative to the pad — the same press always
    // draws the same path, and the render seed is per-stroke-index, so
    // before/after comparisons are like with like.
    const qreal w = width(), h = height();
    const qreal r = qMin(qMin(w, h) * 0.23, (h - 40.0) * 0.25);
    if (r < 12.0)
        return; // pad too small to say anything useful
    const qreal cx = w * 0.5;
    const qreal y0 = h * 0.5 - 2.0 * r;
    auto arcPoint = [](const QPointF &c, qreal radius, qreal deg) {
        const qreal rad = qDegreesToRadians(deg);
        return QPointF(c.x() + radius * std::cos(rad),
                       c.y() + radius * std::sin(rad));
    };
    QVector<QPointF> path;
    const QPointF c1(cx, y0 + r), c2(cx, y0 + 3.0 * r);
    for (int i = 0; i <= 60; ++i) // 270 -> 90 through the LEFT side
        path.append(arcPoint(c1, r, 270.0 - i * 3.0));
    for (int i = 1; i <= 60; ++i) // 270 -> 90 through the RIGHT side
        path.append(arcPoint(c2, r, 270.0 + i * 3.0));
    beginStroke(path.first(), 1.0, 0.0, 0.0, 0.0);
    for (int i = 1; i < path.size(); ++i)
        moveStroke(path.at(i), 1.0, 0.0, 0.0, 0.0);
    endStroke();
    m_sampleLaid = true;
}

void ScratchCanvas::beginStroke(const QPointF &pos, qreal pressure,
                                qreal tiltX, qreal tiltY, qreal rotation)
{
    m_drawing = true;
    m_active.clear();
    StrokePoint p;
    p.position = pos;
    p.pressure = pressure;
    p.tiltX = tiltX;
    p.tiltY = tiltY;
    p.rotation = rotation;
    m_active.append(p);
    moveStroke(pos, pressure, tiltX, tiltY, rotation);
}

void ScratchCanvas::moveStroke(const QPointF &pos, qreal pressure,
                               qreal tiltX, qreal tiltY, qreal rotation)
{
    if (!m_drawing)
        return;
    StrokePoint p;
    p.position = pos;
    p.pressure = pressure;
    p.tiltX = tiltX;
    p.tiltY = tiltY;
    p.rotation = rotation;
    m_active.append(p);

    Job job;
    job.kind = JobKind::Live;
    job.brush = inkAdjusted(m_brush);
    job.base = m_baseline;
    job.size = size();
    job.live = m_active;
    job.brushRevision = m_brushRevision;
    job.strokeCountAtEnqueue = m_strokes.size();
    job.epoch = m_epoch;
    enqueue(std::move(job));
}

void ScratchCanvas::endStroke()
{
    if (!m_drawing)
        return;
    m_drawing = false;
    Job job;
    job.kind = JobKind::Commit;
    job.brush = inkAdjusted(m_brush);
    job.base = m_baseline;
    job.size = size();
    job.live = m_active;
    job.brushRevision = m_brushRevision;
    m_strokes.append(m_active);
    m_active.clear();
    job.strokeCountAtEnqueue = m_strokes.size(); // baseline it will represent
    job.epoch = m_epoch;
    enqueue(std::move(job));
}

QImage ScratchCanvas::renderStrokeOnto(const QImage &base,
                                       const ::Brush &brush,
                                       const QVector<StrokePoint> &points,
                                       quint64 seed)
{
    if (points.isEmpty() || base.isNull())
        return base;
    StrokeBuilder sb(base.size(), brush, false, seed, 0);
    std::unique_ptr<StrokeBuilder> sb2;
    if (brush.dualBrushEnabled())
        sb2 = std::make_unique<StrokeBuilder>(base.size(),
                                              brush.secondaryBrush(), false,
                                              seed, 1);
    for (const StrokePoint &p : points) {
        sb.addRawPoint(p);
        if (sb2)
            sb2->addRawPoint(p);
    }

    SankoPaintHostAdapter::StrokeWork w;
    w.layerKey = QStringLiteral("brush-studio-scratch");
    w.canvasSize = base.size();
    w.brush = brush;
    w.rawPoints = sb.rawPoints();
    w.primaryStamps = sb.stamps();
    if (sb2)
        w.secondaryStamps = sb2->stamps();
    w.affectedRect =
        sb.affectedRect().intersected(QRect(QPoint(0, 0), base.size()));
    if (w.affectedRect.isEmpty())
        return base;
    w.beforeRegion = base.copy(w.affectedRect)
                         .convertToFormat(QImage::Format_ARGB32);
    w.seed = seed;
    w.preferGpu = false; // CPU: deterministic, never the GPU commit worker
    StrokeBuilder::resolveColorDynamics(w.primaryStamps, w.brush,
                                        w.beforeRegion,
                                        w.affectedRect.topLeft());
    if (sb2)
        StrokeBuilder::resolveColorDynamics(w.secondaryStamps,
                                            w.brush.secondaryBrush(),
                                            w.beforeRegion,
                                            w.affectedRect.topLeft());
    const auto result = SankoPaintHostAdapter::render(w);
    if (!result.succeeded)
        return base; // e.g. a pure smudge over emptiness: keep the base

    QImage out = base.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(w.affectedRect.topLeft(), result.afterRegion);
    return out;
}

void ScratchCanvas::workerLoop()
{
    for (;;) {
        Job job;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_queue.isEmpty())
                m_wake.wait(&m_mutex);
            if (m_quit)
                return;
            job = m_queue.dequeue();
        }
        Result r;
        r.kind = job.kind;
        r.brushRevision = job.brushRevision;
        r.strokeCountAtEnqueue = job.strokeCountAtEnqueue;
        r.epoch = job.epoch;
        if (job.kind == JobKind::Full) {
            QImage img = backgroundOf(job);
            for (int i = 0; i < job.strokes.size(); ++i)
                img = renderStrokeOnto(img, job.brush, job.strokes.at(i),
                                       strokeSeed(i));
            r.image = img;
        } else {
            QImage base = job.base;
            if (base.isNull() || base.size() != job.size)
                base = backgroundOf(job);
            const int seedIndex = job.kind == JobKind::Commit
                ? job.strokeCountAtEnqueue - 1
                : job.strokeCountAtEnqueue;
            r.image = renderStrokeOnto(base, job.brush, job.live,
                                       strokeSeed(seedIndex));
        }
        QMetaObject::invokeMethod(
            this, [this, r] { handleResult(r); }, Qt::QueuedConnection);
    }
}

QImage ScratchCanvas::backgroundOf(const Job &job)
{
    if (job.brush.toolMode() == ::Brush::ToolMode::Smudge)
        return smudgeBands(job.size);
    QImage bg(job.size, QImage::Format_ARGB32);
    bg.fill(Qt::transparent);
    return bg;
}

void ScratchCanvas::handleResult(const Result &r)
{
    if (r.kind == JobKind::Full)
        ++m_fullRenders; // executed renders — the seam's tip-regen bound
    if (r.epoch != m_epoch)
        return; // cleared/resized since: stale content
    switch (r.kind) {
    case JobKind::Full:
        if (r.brushRevision != m_brushRevision)
            return; // a newer full render is already scheduled or queued
        if (r.strokeCountAtEnqueue != m_strokes.size()) {
            scheduleFullRender(0); // a stroke landed mid-render: repair
            return;
        }
        m_baseline = r.image;
        m_baselineStrokes = r.strokeCountAtEnqueue;
        m_display = r.image;
        break;
    case JobKind::Live:
        if (r.brushRevision != m_brushRevision)
            return;
        m_display = r.image;
        break;
    case JobKind::Commit:
        if (r.brushRevision != m_brushRevision
            || r.strokeCountAtEnqueue != m_strokes.size()) {
            scheduleFullRender(0); // brush/strokes moved on: re-render all
            return;
        }
        m_baseline = r.image;
        m_baselineStrokes = r.strokeCountAtEnqueue;
        m_display = r.image;
        break;
    }
    update();
    emit renderCompleted();
}

void ScratchCanvas::paintEvent(QPaintEvent *)
{
    if (m_display.isNull())
        return;
    QPainter p(this);
    p.drawImage(0, 0, m_display);
}

void ScratchCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    beginStroke(event->position(), 1.0, 0.0, 0.0, 0.0);
}

void ScratchCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        moveStroke(event->position(), 1.0, 0.0, 0.0, 0.0);
}

void ScratchCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        endStroke();
}

void ScratchCanvas::tabletEvent(QTabletEvent *event)
{
    // Real stylus pressure/tilt; accepting stops Qt from synthesising the
    // mouse events, so strokes are never double-fed.
    switch (event->type()) {
    case QEvent::TabletPress:
        beginStroke(event->position(), event->pressure(), event->xTilt(),
                    event->yTilt(), event->rotation());
        break;
    case QEvent::TabletMove:
        moveStroke(event->position(), event->pressure(), event->xTilt(),
                   event->yTilt(), event->rotation());
        break;
    case QEvent::TabletRelease:
        endStroke();
        break;
    default:
        return;
    }
    event->accept();
}

void ScratchCanvas::resizeEvent(QResizeEvent *)
{
    ++m_epoch; // in-flight results are for the old size
    scheduleFullRender(0);
}

} // namespace brushlib
