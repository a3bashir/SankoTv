#include "SankoTheme.h"
#include "DrawingCanvas.h"

#include "StoryboardModel.h"

#include <QDir>
#include <QActionGroup>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QEventLoop>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFutureWatcher>
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
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QHash>
#include <QStack>
#include <QTabletEvent>
#include <QTimer>
#include <QThreadPool>
#include <QThread>
#include <QToolButton>
#include <QUrl>
#include <QWheelEvent>
#include <Qt>
#include <QUndoCommand>
#include <QUndoStack>
#include <QtMath>
#include <QtConcurrentRun>

#include <algorithm>
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

    // One serial QRhi worker owns the D3D11 device. CPU tile work uses Qt's
    // shared pool, so it cooperates with SankoTV instead of reserving sixteen
    // app-exclusive threads. Pipeline creation is paid during the first idle
    // turn rather than on the artist's first stroke.
    m_paintGpuPool = std::make_unique<QThreadPool>();
    m_paintGpuPool->setMaxThreadCount(1);
    m_paintGpuPool->setExpiryTimeout(-1);
    QTimer::singleShot(0, this, [this] {
        if (!m_paintGpuPool)
            return;
        m_paintGpuPool->start([] {
            SankoPaintHostAdapter::warmUpGpu();
        });
    });

    // Persisted safe-area guide opacities (Preferences > Camera).
    const QSettings settings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    m_actionSafeMaskPct =
        qBound(0, settings.value(QStringLiteral("camera/actionSafeOpacity"), 50).toInt(), 100);
    m_titleSafeMaskPct =
        qBound(0, settings.value(QStringLiteral("camera/titleSafeOpacity"), 50).toInt(), 100);

    // App-level stroke stabilization (Brush Settings studio > Smoothing).
    // 0 by default: the input path is byte-identical to the pre-studio app.
    m_strokeStabilization = qBound(
        0.0,
        settings.value(QStringLiteral("paint/v1/stabilization"), 0.0)
            .toDouble(),
        1.0);

    // Initial working brush: the exact state the old stroke-start rebuild
    // used to produce from the default slider values. With the working-brush
    // model this is assembled ONCE here; the sliders edit fields afterward.
    // (Guarded byte-for-byte by SankoCanvasBrushLock.)
    {
        ::Brush &b = m_paintEngine.brush();
        b.setColor(m_color);
        b.setSize(m_brushToolSize);
        b.setSpacing(0.05);
        b.setOpacity(m_brushToolOpacity);
        b.setHardness(m_brushHardness);
        b.sizePressureCurve().setControlPoints(
            m_pressureToSize ? QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}}
                             : QVector<QPointF>{{0.0, 1.0}, {1.0, 1.0}});
        b.opacityPressureCurve().setControlPoints(
            m_pressureToOpacity ? QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}}
                                : QVector<QPointF>{{0.0, 1.0}, {1.0, 1.0}});
        b.hardnessPressureCurve().setControlPoints({{0.0, 1.0}, {1.0, 1.0}});
    }

    // Workspace grid: persisted view furniture + its show/hide shortcut.
    // Ctrl+' (unused elsewhere; the Photoshop grid-toggle convention).
    // WindowShortcut so it works without canvas focus; the handler ignores
    // it while another page is frontmost.
    loadGridSettings();
    auto *gridToggleAct = new QAction(tr("Show Grid"), this);
    gridToggleAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Apostrophe));
    gridToggleAct->setShortcutContext(Qt::WindowShortcut);
    connect(gridToggleAct, &QAction::triggered, this, [this] {
        if (isVisible())
            setGridVisible(!m_gridVisible);
    });
    addAction(gridToggleAct);

    // (The Canvas View Controls toolbar is the custom-painted ZoomToolbar,
    // created and wired by StoryboardPage. This canvas exposes the view
    // transform engine — setViewZoom/Rotation, toggleFlipH, viewZoomChanged.)

    // Marching ants: advance the dash offset while a selection or floating
    // paste is on screen (updateAntsTimer() starts/stops it).
    m_antsTimer = new QTimer(this);
    m_perspective.reset();

    // --- QuickShape wiring ------------------------------------------------
    // overlayPathChanged drives the display-only vector overlay; on
    // recognition the rough freehand pixels are rolled back to the
    // beginLayerEdit() snapshot (nothing reaches the undo stack yet); a
    // commit replays the corrected path through the normal brush engine.
    // freehandStrokeFinished needs no handler: the ordinary endBrushStroke
    // release path already commits unrecognized strokes unchanged.
    m_qsPreviewTimer = new QTimer(this);
    m_qsPreviewTimer->setSingleShot(true);
    m_qsPreviewTimer->setInterval(16); // coalesce to ~one render per frame
    connect(m_qsPreviewTimer, &QTimer::timeout,
            this, &DrawingCanvas::renderQuickShapePreview);
    connect(&m_quickShape, &quickshape::QuickShapeSession::overlayPathChanged,
            this, [this](const QPainterPath &path) {
        m_quickShapeOverlay = path;
        scheduleQuickShapePreview();
        update();
    });
    connect(&m_quickShape, &quickshape::QuickShapeSession::shapeRecognized,
            this, [this](const quickshape::QuickShapeResult &) {
        discardRoughStroke();
    });
    connect(&m_quickShape, &quickshape::QuickShapeSession::commitRequested,
            this, [this](const quickshape::QuickShapeCommit &commit) {
        replayQuickShape(commit);
    });
    // Hold feedback (repair stage 9): the previously unconsumed
    // statusChanged signal now drives immediate repaints on state
    // transitions, and a light tick repaints the hold-progress ring while
    // the pen is down. Self-stopping: the tick dies with m_qsHeld.
    connect(&m_quickShape, &quickshape::QuickShapeSession::statusChanged,
            this, [this](const QString &, const QString &) { update(); });
    m_qsHoldTick = new QTimer(this);
    m_qsHoldTick->setInterval(33);
    connect(m_qsHoldTick, &QTimer::timeout, this, [this] {
        if (!m_qsHeld) {
            // FINAL frame before stopping: the tick used to stop silently
            // here, and nothing else invalidated the overlay region — the
            // session only emits statusChanged on the freehand branch — so
            // the last painted frame, hint included, survived on screen
            // after release (Dev Recorder session 20260816-210133). Paint
            // once with the flag down so the overlay's disappearance never
            // depends on the session choosing to emit anything.
            update();
            m_qsHoldTick->stop();
            return;
        }
        update();
    });
    connect(&m_quickShape, &quickshape::QuickShapeSession::activeShapeChanged,
            this, [this](bool available) {
        if (!available) {
            m_qsEditing = false;
            m_qsNode = -1;
            m_qsHover = -1;
            m_qsGeometry = {}; // canonical geometry dies with its shape
            m_qsPreview = QImage(); // never leave a stale preview behind
            clearPenUiLatch();      // the latched button may be going away
            ++m_qsPreviewGen; // in-flight renders land in the void
            m_qsPreviewShownGen = m_qsPreviewGen; // and can never resurrect
            m_qsPreviewDirty = false;
            if (m_qsPreviewTimer)
                m_qsPreviewTimer->stop();
        }
        updateQuickShapeUi();
        update();
    });

    // Edit Shape entry button + shape-type selector: canvas CHILDREN, so they
    // receive their own mouse events and interacting with them can never draw.
    // NoFocus keeps them from stealing canvas focus (focus loss bakes shapes).
    m_qsTabletClock.start();
    m_qsEditButton = new QPushButton(QStringLiteral("Edit Shape"), this);
    m_qsEditButton->setFocusPolicy(Qt::NoFocus);
    m_qsEditButton->setCursor(Qt::PointingHandCursor);
    m_qsEditButton->setStyleSheet(SankoTheme::themed("QPushButton { background:#212121; color:#cccccc; border:1px solid #2a2a2a;"
        " border-radius:6px; padding:6px 16px; font-size:12px; font-weight:600; }"
        "QPushButton:hover { color:#ffffff; border-color:%PURPLE%; }"));
    m_qsEditButton->hide();
    connect(m_qsEditButton, &QPushButton::clicked,
            this, [this] { enterQuickShapeEdit(); });

    m_qsDoneButton = new QPushButton(QStringLiteral("Done"), this);
    m_qsDoneButton->setFocusPolicy(Qt::NoFocus);
    m_qsDoneButton->setCursor(Qt::PointingHandCursor);
    m_qsDoneButton->setStyleSheet(SankoTheme::themed("QPushButton { background:%PURPLE%; color:#ffffff; border:none;"
        " border-radius:6px; padding:6px 16px; font-size:12px; font-weight:600; }"
        "QPushButton:hover { background:#8d80f8; }"
        "QPushButton:disabled { background:#3d3766; color:#8a86a8; }"));
    m_qsDoneButton->hide();
    connect(m_qsDoneButton, &QPushButton::clicked, this, [this] {
        // One press = exactly one commit; the guard blocks any duplicate
        // activation while the (synchronous) commit runs.
        if (m_qsCommitting || !m_quickShape.hasActiveShape())
            return;
        // Writability pre-flight (repair stage 8): requestCommit() clears
        // the session whether or not the bake can land, so the check must
        // run FIRST — the shape stays fully pending for a retry after the
        // user unlocks/shows the layer (or Esc discards it).
        if (!editableActiveLayer()) {
            QMessageBox::warning(this, QStringLiteral("QuickShape"),
                QStringLiteral("The target layer is locked or hidden, so the "
                               "shape cannot be baked yet.\n\nUnlock or show "
                               "the layer and press Done again — the shape is "
                               "still editable. Press Esc to discard it."));
            return;
        }
        m_qsCommitting = true;
        m_qsDoneButton->setEnabled(false);
        m_quickShape.requestCommit();
        m_qsDoneButton->setEnabled(true);
        m_qsCommitting = false;
    });

    m_qsTypeBar = new QWidget(this);
    m_qsTypeBar->setObjectName(QStringLiteral("qsTypeBar"));
    m_qsTypeBar->setAttribute(Qt::WA_StyledBackground, true);
    m_qsTypeBar->setStyleSheet(QStringLiteral(
        "#qsTypeBar { background:#212121; border:1px solid #2a2a2a;"
        " border-radius:6px; }"));
    auto *qsTypeLayout = new QHBoxLayout(m_qsTypeBar);
    qsTypeLayout->setContentsMargins(6, 4, 6, 4);
    qsTypeLayout->setSpacing(4);
    m_qsTypeBar->hide();

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
        // One cursor per corner: the base glyph reads as the TOP-LEFT corner
        // arrow (it bends around an upper-left corner); the other corners get
        // the same glyph rotated to bend around THEIR corner.
        for (int corner = 0; corner < 4; ++corner) { // TL, TR, BR, BL
            QTransform rot;
            rot.rotate(90.0 * corner);
            m_rotateCursors[corner] =
                QCursor(pm.transformed(rot, Qt::SmoothTransformation), 13, 13);
        }
    }

}

DrawingCanvas::~DrawingCanvas()
{
    if (!m_paintGpuPool)
        return;
    m_paintGpuPool->clear();
    if (!m_paintGpuPool->waitForDone(1500)) {
        (void)m_paintGpuPool.release();
        return;
    }
    // Destroy D3D/QRhi objects on their owning worker before Qt and the
    // worker thread itself are torn down.
    m_paintGpuPool->start([] {
        SankoPaintHostAdapter::shutdownGpuForCurrentThread();
    });
    m_paintGpuPool->waitForDone();
}

namespace {

// One drawing edit in the app-wide chronological undo history: before/after
// pixels of just the affected region on one layer (memory-efficient — never
// two whole-layer copies). The first redo() is skipped because the edit has
// already been applied interactively by the time the command is pushed.
class DrawingCommand : public QUndoCommand
{
public:
    DrawingCommand(DrawingCanvas *canvas, Panel *panel, const QString &layerId,
                   const QRect &region, const QImage &before, const QImage &after,
                   const QString &text)
        : QUndoCommand(text), m_canvas(canvas), m_panel(panel),
          m_layerId(layerId), m_region(region), m_before(before), m_after(after)
    {
    }
    void undo() override
    {
        m_canvas->applyLayerRegionForUndo(m_panel, m_layerId, m_region, m_before);
    }
    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false; // already applied interactively
            return;
        }
        m_canvas->applyLayerRegionForUndo(m_panel, m_layerId, m_region, m_after);
    }

private:
    DrawingCanvas *m_canvas;
    Panel *m_panel;
    QString m_layerId;
    QRect m_region;
    QImage m_before;
    QImage m_after;
    bool m_firstRedo = true;
};

// Brush-only command on SankoTV's shared chronological stack.  Its retention
// policy may discard only its own after pixels; neighbouring panel/layer/
// transform commands are never removed or rebuilt.
class SankoPaintStrokeCommand final : public QUndoCommand
{
public:
    SankoPaintStrokeCommand(DrawingCanvas *canvas, Panel *panel,
                            const QString &layerId,
                            const SankoPaintHostAdapter::StrokeResult &result)
        : QUndoCommand(QStringLiteral("Brush Stroke")), m_canvas(canvas),
          m_panel(panel), m_layerId(layerId), m_commit(result.commit),
          m_replay(result.replay)
    {
        for (const BrushTilePatch &patch : m_commit.tilePatches) {
            m_afterBytes += patch.after.sizeInBytes();
        }
        m_afterHashes = result.afterHashes;
    }

    void undo() override
    {
        if (m_canvas)
            m_canvas->applyBrushCommitForUndo(m_panel, m_layerId, m_commit, false);
    }

    void redo() override
    {
        if (!m_canvas)
            return;
        if (m_firstRedo) {
            m_firstRedo = false;
            return; // pixels were published before QUndoStack::push()
        }
        if (m_hasAfterPixels) {
            m_canvas->applyBrushCommitForUndo(m_panel, m_layerId, m_commit, true);
            return;
        }
        // Old entries re-render from their immutable captured settings and
        // resolved samples. Publish only after every tile passes SHA-256.
        const auto regenerated = SankoPaintHostAdapter::render(m_replay);
        if (!regenerated.succeeded
            || regenerated.commit.tilePatches.size() != m_afterHashes.size())
            return;
        for (const BrushTilePatch &patch : regenerated.commit.tilePatches) {
            const QByteArray digest = QCryptographicHash::hash(
                QByteArrayView(reinterpret_cast<const char *>(patch.after.constBits()),
                               patch.after.sizeInBytes()),
                QCryptographicHash::Sha256);
            if (m_afterHashes.value(patch.coordinate) != digest)
                return;
        }
        m_canvas->applyBrushCommitForUndo(
            m_panel, m_layerId, regenerated.commit, true);
    }

    qsizetype afterBytes() const { return m_afterBytes; }
    bool hasAfterPixels() const { return m_hasAfterPixels; }
    void dropAfterPixels()
    {
        if (!m_hasAfterPixels)
            return;
        for (BrushTilePatch &patch : m_commit.tilePatches)
            patch.after = QImage();
        m_afterBytes = 0;
        m_hasAfterPixels = false;
    }

private:
    DrawingCanvas *m_canvas = nullptr;
    Panel *m_panel = nullptr;
    QString m_layerId;
    BrushStrokeCommit m_commit;
    SankoPaintHostAdapter::StrokeWork m_replay;
    QHash<QPoint, QByteArray> m_afterHashes;
    qsizetype m_afterBytes = 0;
    bool m_hasAfterPixels = true;
    bool m_firstRedo = true;
};

// One selection-region change in the app-wide history (create/add/remove,
// select all, inverse, deselect, outline moves). Restores the previous
// vector path; never touches layer pixels.
class SelectionCommand : public QUndoCommand
{
public:
    SelectionCommand(DrawingCanvas *canvas, const QPainterPath &before,
                     const QPainterPath &after)
        : QUndoCommand(QStringLiteral("Selection")), m_canvas(canvas),
          m_before(before), m_after(after)
    {
    }
    void undo() override { m_canvas->applySelectionPathForUndo(m_before); }
    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_canvas->applySelectionPathForUndo(m_after);
    }

private:
    DrawingCanvas *m_canvas;
    QPainterPath m_before;
    QPainterPath m_after;
    bool m_firstRedo = true;
};

// Perspective model change (VP create/move/delete, guide-settings edit) as
// ONE chronological entry in the shared app stack: full before/after JSON
// snapshots of the PerspectiveTool, applied via fromJson on undo/redo. The
// live edit already happened, so the first redo() is skipped.
class PerspectiveCommand : public QUndoCommand
{
public:
    PerspectiveCommand(DrawingCanvas *canvas, const QJsonObject &before,
                       const QJsonObject &after, const QString &text)
        : QUndoCommand(text), m_canvas(canvas), m_before(before), m_after(after)
    {
    }
    void undo() override { m_canvas->applyPerspectiveForUndo(m_before); }
    void redo() override
    {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        m_canvas->applyPerspectiveForUndo(m_after);
    }

private:
    DrawingCanvas *m_canvas;
    QJsonObject m_before;
    QJsonObject m_after;
    bool m_firstRedo = true;
};

// Single tuning point for the QuickShape hold behaviour. Values are
// SCREEN-space; applyQuickShapeTiming() converts them into document units at
// each stroke start so the feel is identical at every zoom level.
struct QuickShapeTuning
{
    // 900 ms, raised from 550 after measuring the Dev Recorder sessions:
    // natural mid-stroke pauses reach 170 ms with a pen (24 canvas strokes,
    // 20260802-144716) and 335 ms with a mouse, while the one deliberate
    // pre-snap hold in the data measured 1039 ms. 900 clears the worst
    // natural pause by 2.7x and still sits under the observed intentional
    // hold, so a thinking pause no longer snaps a stroke the user meant to
    // keep freehand.
    int holdDurationMs = 900;
    // The dwell runs SILENTLY until this fraction of it has elapsed; the
    // ring then appears and sweeps the remaining quarter. Both numbers
    // live here so the reveal point and the hold length are read and
    // tuned together — a reveal fraction means nothing without the
    // duration it is a fraction OF. At 900 ms x 0.75 the ring is hidden
    // for 675 ms and visible for the final 225 ms.
    qreal ringRevealFraction = 0.75;
    int morphDurationMs = 220;
    qreal dwellRadiusScreenPx = 8.0;
    qreal maxDwellVelocityScreenPxPerSec = 20.0;
};
constexpr QuickShapeTuning kQuickShapeTuning{};

// Whole-stroke dwell indicator geometry (screen px). The ring sits BELOW
// the slot where Edit Shape | Done appear after recognition (top-centre,
// y = kQsChromeY in updateQuickShapeUi), so the user is already looking
// there when the buttons arrive and the buttons never move into space the
// ring occupied. Ring and buttons also never coexist: the buttons need
// hasActiveShape && !m_qsHeld, the ring needs m_qsHeld && !hasActiveShape.
constexpr int kQsChromeY = 10;       // the buttons' y in updateQuickShapeUi
constexpr int kQsRingGap = 8;        // below the buttons' slot
constexpr qreal kQsRingR = 8.0;      // the progress arc's radius
constexpr qreal kQsRingBackR = 14.0; // the opaque backing disc's radius

// Closed-shape pressure seam policy — THE single home for every seam tuning
// constant (no magic values in replay code). A closed QuickShape replays the
// ORIGINAL stroke's pressures resampled onto the ideal path, so the
// touchdown and lift-off — the two lightest moments of a real pen stroke —
// both land on the SAME visible edge, and a hexagon can read as a broken
// pentagon (dev recording 20260802-144716). For CLOSED shapes only,
// quickShapePointStream() blends the pressures across the closing seam and
// applies a conservative local floor:
//   * blendFraction: the fraction of the path, on EACH side of the seam,
//     cross-faded (smoothstep) toward the join pressure — the average of
//     the pressures just outside the two blend windows;
//   * minSeamPressure: floor applied INSIDE the blend windows only, and
//     never above the stroke's median pressure — a deliberately light
//     stroke stays uniformly light instead of growing a heavy seam bump.
// Pressure variation outside the windows is untouched, and because the
// closing point repeats the (corrected) start pressure, the overlapping
// stamps at the join match their neighbours — no double-stamp bump. Open
// shapes (lines, arcs, polylines, open ellipses) are NEVER corrected: their
// start/end taper is the artist's.
struct QuickShapeSeamPolicy
{
    qreal blendFraction = 0.12;
    qreal minSeamPressure = 0.35;
};
constexpr QuickShapeSeamPolicy kQuickShapeSeamPolicy{};

// Circumcircle through three points; false when they are collinear.
bool circumcircle(const QPointF &a, const QPointF &b, const QPointF &p,
                  QPointF *center, qreal *radius)
{
    const qreal d = 2.0 * (a.x() * (b.y() - p.y()) + b.x() * (p.y() - a.y())
                           + p.x() * (a.y() - b.y()));
    if (qAbs(d) < 1e-6)
        return false;
    const qreal a2 = a.x() * a.x() + a.y() * a.y();
    const qreal b2 = b.x() * b.x() + b.y() * b.y();
    const qreal p2 = p.x() * p.x() + p.y() * p.y();
    center->setX((a2 * (b.y() - p.y()) + b2 * (p.y() - a.y())
                  + p2 * (a.y() - b.y())) / d);
    center->setY((a2 * (p.x() - b.x()) + b2 * (a.x() - p.x())
                  + p2 * (b.x() - a.x())) / d);
    *radius = QLineF(*center, a).length();
    return true;
}

// Tight bounding rect of the pixels that differ between two same-sized
// images (empty when identical).
QRect diffRegion(const QImage &a, const QImage &b)
{
    if (a.size() != b.size() || a.isNull())
        return b.rect(); // shape changed: treat everything as affected
    const int w = a.width(), hgt = a.height();
    int top = -1, bottom = -1;
    for (int y = 0; y < hgt; ++y) {
        if (memcmp(a.constScanLine(y), b.constScanLine(y), size_t(w) * 4) != 0) {
            if (top < 0)
                top = y;
            bottom = y;
        }
    }
    if (top < 0)
        return QRect();
    int left = w, right = -1;
    for (int y = top; y <= bottom; ++y) {
        const QRgb *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < left; ++x)
            if (ra[x] != rb[x]) {
                left = x;
                break;
            }
        for (int x = w - 1; x > right; --x)
            if (ra[x] != rb[x]) {
                right = x;
                break;
            }
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

} // namespace

// THE canvas-size authority guard — one shape, used at every document-space
// entry point that could otherwise run with no active panel. Reaching such
// a path panel-less is a CODING DEFECT (every UI route is gated), never a
// state the user can create. Debug builds stop at the fault. Release
// builds refuse the operation, log the site once, and leave the document
// untouched — the user keeps a correct canvas (the no-document workspace),
// never a silently wrong one. No computation ever proceeds on QSize(-1,-1)
// and no 960x540 stand-in exists anywhere.
#define SANKO_REQUIRE_PANEL(returnValue) \
    do { \
        if (Q_UNLIKELY(!m_panel)) { \
            Q_ASSERT_X(m_panel, __func__, \
                       "no active panel: canvas size unavailable"); \
            static bool sankoWarnedOnce = false; \
            if (!sankoWarnedOnce) { \
                sankoWarnedOnce = true; \
                qCritical("SankoTV: %s reached with no active panel; " \
                          "operation refused", __func__); \
            } \
            return returnValue; \
        } \
    } while (false)

QSize DrawingCanvas::canvasSize() const
{
    // Pixels are the truth: the active panel's layers carry the size the
    // project's manifest shaped at create/load. No panel -> INVALID size,
    // never a default.
    return m_panel ? m_panel->canvasSize() : QSize();
}

void DrawingCanvas::setActivePanel(Panel *panel)
{
    // A GPU brush commit captures the current Panel pointer.  Finish that
    // short publication before the host changes panel ownership so the
    // completion callback can never observe a detached/deleted panel.
    flushPaintCommit();

    // Leaving a panel (or re-clicking it) with live canvas-side state: bake
    // everything into the CURRENT panel's own layers FIRST. A floating paste
    // commits; a live transform session commits WITHOUT relifting — the
    // default relift used to hollow the layer again right before the switch,
    // stranding the artwork in m_transformBuf (the panel's data model went
    // empty and undo appeared to skip back past committed transforms).
    commitQuickShape(); // bake into the panel we are LEAVING
    commitFloating();
    if (m_xformActive)
        commitTransform(false);

    // DEFENCE IN DEPTH: nothing below may DEREFERENCE the panel we are
    // leaving. freeScenes() now detaches before it destroys, so this should
    // never see a dead panel — but this function used to invalidate the
    // caches through canvasSize(), which read the outgoing panel, and a
    // project load had already deleted it. Dropping the caches needs no
    // panel at all: clear the flags directly, and let the invalidated rect
    // describe the panel being switched TO, which is what the next repaint
    // covers anyway.
    m_compValid = false;
    m_compPanel = nullptr; // never compare against a pointer that may be dead
    m_panel = panel;
    invalidateComposite();
    m_drawing = false;
    m_brushStroke = false;
    m_strokeMask = StrokeMaskNone;
    m_strokeBuf = QImage();
    m_editPanel = nullptr; // any in-flight edit snapshot dies with the switch
    cancelShape(); // preview state never carries across panels
    m_floatActive = false;
    m_floatDragging = false;
    m_moveActive = false;
    m_layerBackup = QImage();
    m_moveMask = QImage();
    m_floatImg = QImage();
    m_floatDelta = QPointF();
    clearSelection(); // the selection mask never carries across panels
    // The Move tool's transform box follows onto the newly-active panel.
    if (m_tool == Move && m_panel)
        liftDefaultTransformBox();
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
    // Group folders are organisation rows, never paint targets; a member of
    // a hidden folder is as un-drawable as a hidden layer.
    if (!layer || layer->locked || isGroupLayer(*layer)
        || !m_panel->layerEffectivelyVisible(*layer))
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
    // Both the padding buffer AND the fit target come from the panel's own
    // size (they used to come from two different oracles).
    QImage content = makeLayerImage(canvasSize());
    {
        QPainter painter(&content);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QSize target = loaded.size();
        target.scale(canvasSize(), Qt::KeepAspectRatio);
        QRect r(QPoint(0, 0), target);
        r.moveCenter(content.rect().center());
        painter.drawImage(r, loaded);
    }

    Layer layer =
        makeRasterLayer(QFileInfo(filePath).completeBaseName(), canvasSize());
    layer.type = QStringLiteral("image");
    layer.image = content;

    const int insertAt = qBound(0, m_panel->activeLayerIndex + 1, m_panel->layers.size());
    m_panel->layers.insert(insertAt, layer);
    m_panel->activeLayerIndex = insertAt;

    invalidateComposite();
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
    clearPenUiLatch(); // a latched pen tap does not survive a tool change

    const Tool previous = m_tool;
    commitQuickShape(); // a pending QuickShape bakes before the tool changes
    commitFloating(); // an un-committed paste lands before the tool changes
    if (m_xformActive && tool != Move)
        commitTransform(false); // leaving Move finalises the box — no re-lift
    if (m_tool == SelectPoly && !m_lassoPts.isEmpty()) {
        m_lassoPts.clear(); // an un-closed polygon selection never survives
        setMouseTracking(false);
    }
    m_tool = tool;
    // Perspective hover feedback (handles grow under the cursor) needs
    // buttonless move events; SelectPoly manages its own tracking below.
    if (tool == Perspective)
        setMouseTracking(true);
    else if (tool != SelectPoly)
        setMouseTracking(false);
    m_perspHover = -1;
    cancelShape(); // an in-progress shape never survives a tool switch
    // Activating Move shows the transform box at once (like Photoshop).
    if (tool == Move)
        liftDefaultTransformBox();
    if (!m_panning)
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : defaultCursorShape());
    update(); // the selection itself DOES survive (Select -> Move flow)
    if (m_tool != previous)
        emit toolChanged(m_tool); // per-tool Size CTL sliders re-sync
}

void DrawingCanvas::refreshTransformBox()
{
    if (m_tool != Move)
        return;
    commitTransform(false); // running session lands in ITS layers (by ID)
    liftDefaultTransformBox(); // fresh box around the new target
    update();
}

// After an outside model change (merge, etc.) the session's pristine buffers
// no longer match the layers — committing them would paste PRE-change pixels
// back. Drop the session instead, then lift fresh around the current target.
void DrawingCanvas::resetTransformBox()
{
    if (m_tool != Move)
        return;
    cancelTransform(false);
    liftDefaultTransformBox();
    update();
}

// Move tool activation / panel-switch persistence: show the transform box —
// a live selection lifts the selected pixels; with NO selection the box wraps
// the layer's whole artwork (synthesized rect selection, dropped on cancel).
// An empty layer shows no box.
void DrawingCanvas::liftDefaultTransformBox()
{
    if (m_xformActive)
        return;
    // A selected GROUP folder gets one box around all of its members.
    if (m_panel) {
        Layer *active = m_panel->activeLayer();
        if (active && isGroupLayer(*active)) {
            beginGroupTransform(active);
            return;
        }
        // MULTIPLE selected rows (no marquee): lift them ALL behind one box
        // — the same transform lands on every one, in place, stacking order
        // untouched. A marquee selection keeps the classic single-layer lift.
        if (m_selectionPath.isEmpty() && m_selectedLayerIds.size() > 1) {
            beginLayersTransform(m_selectedLayerIds);
            if (m_xformActive)
                return; // multi-lift succeeded
        }
    }
    if (m_selectionPath.isEmpty()) {
        if (Layer *layer = editableActiveLayer()) {
            const QRect art = opaquePixelBounds(layer->image);
            if (!art.isEmpty()) {
                QPainterPath path;
                path.addRect(QRectF(art)); // QRect(P,P) width already spans maxX+1
                m_selectionPath = path;
                m_xformAutoSel = true;
            }
        }
    }
    if (!m_selectionPath.isEmpty())
        beginTransform();
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
    m_paintEngine.brush().setColor(color);
}

void DrawingCanvas::setBrushSize(int size)
{
    m_brushSize = qBound(1, size, 20);
}

void DrawingCanvas::setBrushToolSize(int px)
{
    m_brushToolSize = qBound(1, px, 200);
    m_paintEngine.brush().setSize(m_brushToolSize);
    emit paintBrushEdited();
}

void DrawingCanvas::setBrushOpacity(int percent)
{
    m_brushToolOpacity = qBound(0, percent, 100) / 100.0;
    m_paintEngine.brush().setOpacity(m_brushToolOpacity);
    emit paintBrushEdited();
}

void DrawingCanvas::setBrushHardness(int percent)
{
    m_brushHardness = qBound(0, percent, 100) / 100.0;
    m_paintEngine.brush().setHardness(m_brushHardness);
    emit paintBrushEdited();
}

void DrawingCanvas::setEraserSize(int px)
{
    m_eraserSize = qBound(1, px, 200);
}

void DrawingCanvas::setEraserOpacity(int percent)
{
    m_eraserOpacity = qBound(0, percent, 100) / 100.0;
}

void DrawingCanvas::setPressureToSize(bool on)
{
    // The toggle writes its curve straight into the working brush. This
    // deliberately OVERRIDES a preset's richer multi-point curve — the user
    // flipped an explicit switch; re-selecting the preset restores it.
    m_pressureToSize = on;
    m_paintEngine.brush().sizePressureCurve().setControlPoints(
        on ? QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}}
           : QVector<QPointF>{{0.0, 1.0}, {1.0, 1.0}});
    emit paintBrushEdited();
}

void DrawingCanvas::setPressureToOpacity(bool on)
{
    m_pressureToOpacity = on;
    m_paintEngine.brush().opacityPressureCurve().setControlPoints(
        on ? QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}}
           : QVector<QPointF>{{0.0, 1.0}, {1.0, 1.0}});
    emit paintBrushEdited();
}

QString DrawingCanvas::paintLayerKey(Panel *panel, const QString &layerId) const
{
    return QString::number(quintptr(panel), 16) + QLatin1Char(':') + layerId;
}

void DrawingCanvas::syncPaintBrushSettings()
{
    // WORKING-BRUSH MODEL (Brush Library phase 3): the engine brush IS the
    // canvas's brush state. Selecting a library preset copies the preset in
    // wholesale (setPaintBrush); the sliders and pressure toggles edit
    // FIELDS of that working copy as they move. Nothing is rebuilt at
    // stroke start any more — historically this function reassembled
    // colour, size, spacing and all three curves from four slider values,
    // which would have clobbered every other parameter a preset carries.
    // Colour is the one live-bound property: it belongs to the app's colour
    // panel, not to brush identity, so it is re-asserted here.
    m_paintEngine.brush().setColor(m_color);
}

void DrawingCanvas::setStrokeStabilization(double amount)
{
    m_strokeStabilization = qBound(0.0, amount, 1.0);
}

QPointF DrawingCanvas::stabilizeStrokePoint(const QPointF &raw,
                                            bool strokeBegin)
{
    // OFF is a full bypass, not a zero-strength filter: with the studio's
    // Smoothing slider at 0 the live input path is byte-identical to the
    // pre-studio application.
    if (m_strokeStabilization <= 0.0)
        return raw;
    if (strokeBegin) {
        m_stabPoint = raw; // anchor: the stroke starts where the pen touched
        return raw;
    }
    // Position EMA. Runs BEFORE QuickShape and the engine's own fixed
    // smoothing see the point, so recognition, the rough stroke, and the
    // stamps all agree on the same filtered path.
    const qreal alpha = 1.0 - 0.92 * m_strokeStabilization;
    m_stabPoint += (raw - m_stabPoint) * alpha;
    return m_stabPoint;
}

void DrawingCanvas::setPaintBrush(const ::Brush &brush)
{
    // Library selection: the preset's FULL parameter set becomes the working
    // brush (a copy — edits never write back into the preset). The slider
    // mirrors resync so the Brush Options panel reflects the selection, and
    // the pressure toggles mirror whether the preset's curves respond to
    // pressure at all.
    m_paintEngine.setBrush(brush);
    m_brushToolSize = brush.size();
    m_brushToolOpacity = brush.opacity();
    m_brushHardness = brush.hardness();
    // A preset with an identity colour (Blue Pencil, Sanguine...) adopts it
    // as the app colour; black-ink presets keep the user's current colour.
    if (brush.color() != QColor(Qt::black))
        m_color = brush.color();
    m_paintEngine.brush().setColor(m_color);
    const auto respondsToPressure = [](const PressureCurve &curve) {
        return curve.valueAt(0.0) < 0.99; // flat-at-1 = no pressure response
    };
    m_pressureToSize = respondsToPressure(brush.sizePressureCurve());
    m_pressureToOpacity = respondsToPressure(brush.opacityPressureCurve());
    emit paintBrushChanged();
}

void DrawingCanvas::pushPaintStroke(
    const SankoPaintHostAdapter::StrokeResult &result, Panel *panel,
    const QString &layerId, const QString &undoText)
{
    if (!m_undoStack || !panel || !result.succeeded)
        return;
    auto *command = new SankoPaintStrokeCommand(this, panel, layerId, result);
    command->setText(undoText);
    m_undoStack->push(command);
    enforcePaintUndoPolicy();
}

void DrawingCanvas::enforcePaintUndoPolicy()
{
    if (!m_undoStack)
        return;
    QVector<SankoPaintStrokeCommand *> strokes;
    qsizetype retainedAfter = 0;
    for (int index = 0; index < m_undoStack->count(); ++index) {
        auto *command = const_cast<SankoPaintStrokeCommand *>(
            dynamic_cast<const SankoPaintStrokeCommand *>(m_undoStack->command(index)));
        if (!command)
            continue; // host commands are deliberately invisible to this policy
        strokes.append(command);
        retainedAfter += command->afterBytes();
    }
    constexpr int keepAfterCount = 20;
    constexpr qsizetype budget = qsizetype(256) * 1024 * 1024;
    const int firstRetained = qMax(0, strokes.size() - keepAfterCount);
    for (int i = 0; i < firstRetained; ++i) {
        retainedAfter -= strokes.at(i)->afterBytes();
        strokes.at(i)->dropAfterPixels();
    }
    for (SankoPaintStrokeCommand *command : strokes) {
        if (retainedAfter <= budget)
            break;
        if (!command->hasAfterPixels())
            continue;
        retainedAfter -= command->afterBytes();
        command->dropAfterPixels();
    }
}

// The letterbox fit is the zoom=1.0 baseline; m_zoom scales it and m_panOffset
// shifts it. Because toCanvas()/scale() derive from this rect, EVERY mouse
// press/move/release converts screen -> image coordinates through the same
// zoom+pan mapping, so strokes land under the cursor at any zoom level.
QRect DrawingCanvas::displayRect() const
{
    if (!m_panel)
        return QRect(); // no document: nothing to letterbox
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
    if (!m_panel) { // no document: remember the zoom, skip the anchor math
        m_zoom = zoom;
        syncViewToolbar();
        return;
    }
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

// Fit Screen: ALWAYS centre the canvas and fit it fully in the viewport,
// regardless of the current zoom or pan. Zoom 1.0 is the letterbox fit
// (displayRect() centres when the pan offset is zero); a small margin keeps
// the canvas edges breathing. setViewZoom(1.0) could not do this — it
// early-returns at zoom 1.0 and never clears the pan.
void DrawingCanvas::fitToScreen()
{
    constexpr double kFitMargin = 0.96; // 4% breathing room around the canvas
    m_zoom = kFitMargin;
    m_panOffset = QPointF(0, 0);
    syncViewToolbar();
    emit viewZoomChanged(m_zoom);
    update();
}

void DrawingCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionViewToolbar();
    updateQuickShapeUi();
}

// --- Canvas View Controls (display-only view transforms) --------------------

void DrawingCanvas::setViewZoom(double zoom)
{
    setZoom(zoom, QPointF(width() / 2.0, height() / 2.0)); // centred on the view
    // Zoom changed while the pen is down (repair stage 9): re-derive the
    // document-space dwell tolerances so the hold keeps its ~8 screen px
    // feel at the new zoom instead of the stroke-start conversion.
    if (m_quickShapeEnabled && m_qsHeld)
        applyQuickShapeTiming();
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

// PERMANENT test surface (SankoCanvasEdgeLock). Places canvasPt at widgetPt at
// an exact on-screen scale. The pan is solved through a MEASURED 2x2 Jacobian
// of viewTransform() in m_panOffset rather than a hand-derived matrix: the
// mapping is affine in the pan but composed with rotation/flip about the view
// centre, and a hand-derived version once landed visibly wrong at DPR 2 while
// still passing at DPR 1 — exactly the near-miss a boundary test cannot afford.
void DrawingCanvas::placeViewForTest(double onScreenScale, const QPointF &canvasPt,
                                     const QPointF &widgetPt)
{
    SANKO_REQUIRE_PANEL();
    const QSize cs = canvasSize();
    const double fit = qMin(width() / double(cs.width()),
                            height() / double(cs.height()));
    if (fit <= 0.0)
        return;
    m_zoom = onScreenScale / fit;
    const double w = cs.width() * onScreenScale, h = cs.height() * onScreenScale;
    m_panOffset = QPointF(widgetPt.x() - canvasPt.x() * onScreenScale - (width() - w) / 2.0,
                          widgetPt.y() - canvasPt.y() * onScreenScale - (height() - h) / 2.0);
    const QPointF base = m_panOffset;
    const QPointF p0 = viewTransform().map(canvasPt);
    m_panOffset = base + QPointF(1, 0);
    const QPointF dx = viewTransform().map(canvasPt) - p0;
    m_panOffset = base + QPointF(0, 1);
    const QPointF dy = viewTransform().map(canvasPt) - p0;
    m_panOffset = base;
    const qreal det = dx.x() * dy.y() - dy.x() * dx.y();
    if (!qFuzzyIsNull(det)) {
        const QPointF err = widgetPt - p0;
        m_panOffset += QPointF((err.x() * dy.y() - dy.x() * err.y()) / det,
                               (dx.x() * err.y() - err.x() * dx.y()) / det);
    }
    update();
}

// PERMANENT test surface (SankoCanvasEdgeLock). Same members the grid menu
// sets, WITHOUT saveGridSettings(): a test must never write the user's real
// workspace preferences.
void DrawingCanvas::setWorkspaceForTest(const QColor &gutter, bool gridVisible,
                                        GridStyle style, const QColor &gridColor)
{
    m_gutterColor = gutter;
    m_gridVisible = gridVisible;
    m_gridStyle = style;
    m_gridColor = gridColor;
    m_gridTileDirty = true;
    update();
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
    SANKO_REQUIRE_PANEL(QPoint());
    const QSize cs = canvasSize();
    const QPointF p = viewTransform().inverted().map(QPointF(widgetPoint));
    return QPoint(qBound(0, int(p.x()), cs.width() - 1),
                  qBound(0, int(p.y()), cs.height() - 1));
}

int DrawingCanvas::penWidth() const
{
    // The Eraser carries its own full-range width in CANVAS pixels (same
    // semantics as the brush-size slider, zoom-independent); other classic
    // strokes keep the screen-mapped width.
    if (m_tool == Eraser)
        return qMax(1, m_eraserSize);
    return qMax(1, qRound(m_brushSize / scale()));
}

// Float variant for the brush engine: sub-pixel dab placement, no clamping
// (dabs partially outside the image are clipped by QPainter automatically).
QPointF DrawingCanvas::toCanvasF(const QPointF &widgetPoint) const
{
    return viewTransform().inverted().map(widgetPoint); // inverse zoom+pan+rotate+flip
}

// --- App-wide undo plumbing --------------------------------------------------
// beginLayerEdit() snapshots the ACTIVE layer before a mutating operation;
// finalizeLayerEdit() diffs that snapshot against the layer and pushes ONE
// region-limited DrawingCommand (before/after sub-images of just the changed
// rect, keyed by layer id) onto the shared stack. No-op edits push nothing.
void DrawingCanvas::beginLayerEdit()
{
    m_editPanel = nullptr;
    if (!m_panel)
        return;
    Layer *layer = m_panel->activeLayer();
    if (!layer)
        return;
    m_editPanel = m_panel;
    m_editLayerId = layer->id;
    m_editBefore = layer->image.copy();
}

void DrawingCanvas::finalizeLayerEdit(const QString &text, const QImage &beforeOverride)
{
    Panel *panel = m_editPanel;
    m_editPanel = nullptr;
    if (!panel || !m_undoStack)
        return;
    Layer *target = nullptr;
    for (Layer &layer : panel->layers)
        if (layer.id == m_editLayerId)
            target = &layer;
    const QImage before = beforeOverride.isNull() ? m_editBefore : beforeOverride;
    m_editBefore = QImage();
    if (!target)
        return;
    const QRect region = diffRegion(before, target->image);
    if (region.isEmpty())
        return;
    m_undoStack->push(new DrawingCommand(this, panel, m_editLayerId, region,
                                         before.copy(region),
                                         target->image.copy(region), text));
}

// Command callback: write back one region of one layer (Source mode replaces
// pixels exactly, alpha included), then refresh.
void DrawingCanvas::applyLayerRegionForUndo(Panel *panel, const QString &layerId,
                                            const QRect &region, const QImage &pixels)
{
    if (!panel)
        return;
    for (Layer &layer : panel->layers) {
        if (layer.id != layerId)
            continue;
        QPainter p(&layer.image);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(region.topLeft(), pixels);
        p.end();
        break;
    }
    invalidateComposite(); // undo/redo can rewrite a non-active layer
    update();
    emit contentChanged();
    emit panelEdited(panel); // the command's OWN panel refreshes its thumbnail
}

void DrawingCanvas::applyBrushCommitForUndo(Panel *panel, const QString &layerId,
                                            const BrushStrokeCommit &commit,
                                            bool after)
{
    if (!panel)
        return;
    for (Layer &layer : panel->layers) {
        if (layer.id != layerId)
            continue;
        const QString key = QString::number(quintptr(panel), 16)
            + QLatin1Char(':') + layerId;
        if (!m_paintEngine.applyCommit(key, commit, after, layer.image,
                                      BrushCoherenceTrigger::UndoRedo))
            return;
        break;
    }
    const Layer *active = panel == m_panel ? panel->activeLayer() : nullptr;
    if (active && active->id == layerId)
        invalidateCompositeRegion(commit.affectedRect);
    else
        invalidateComposite();
    update();
    emit contentChanged();
    emit panelEdited(panel);
}

bool DrawingCanvas::flushPaintCommit(int timeoutMs)
{
    if (!m_paintCommitPending)
        return true;
    QElapsedTimer timer;
    timer.start();
    while (m_paintCommitPending && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        QThread::msleep(1);
    }
    return !m_paintCommitPending;
}

void DrawingCanvas::ensurePanelCpuCoherent(Panel *panel,
                                           BrushCoherenceTrigger trigger)
{
    if (!panel)
        return;
    if (trigger == BrushCoherenceTrigger::Save
        || trigger == BrushCoherenceTrigger::Thumbnail
        || trigger == BrushCoherenceTrigger::ExplicitDemand)
        flushPaintCommit();
    for (Layer &layer : panel->layers) {
        if (isGroupLayer(layer) || layer.image.isNull())
            continue;
        // Authority-checked: a host image the classic tools edited since the
        // last engine sync re-seeds the mirror instead of being overwritten,
        // so Erase/Move/Fill/Shapes/paste/undo edits always survive.
        m_paintEngine.refreshCoherentImage(paintLayerKey(panel, layer.id),
                                           layer.image, trigger);
    }
}

#ifdef SANKOTV_DEV_RECORDER
// TEMP dev-recorder camera probe: read-only snapshot of the live view
// transform, so a recording can distinguish "paper moved off-screen" from
// "paper not drawn". Removed with the recorder (option SANKOTV_DEV_RECORDER).
QVariantMap DrawingCanvas::devCameraState() const
{
    const QTransform t = viewTransform();
    const QRect vis =
        t.inverted().mapRect(QRectF(rect())).toAlignedRect();
    // No document: honest zeros in the recorder stream (hasPanel already
    // says so), never an invalid -1x-1 rect's numbers.
    const QRect canvasR(QPoint(), m_panel ? canvasSize() : QSize(0, 0));
    const QRect disp = displayRect();
    QVariantMap m;
    m.insert(QStringLiteral("zoom"), m_zoom);
    m.insert(QStringLiteral("rotation"), m_viewRotation);
    m.insert(QStringLiteral("flipH"), m_viewFlipH);
    m.insert(QStringLiteral("panX"), m_panOffset.x());
    m.insert(QStringLiteral("panY"), m_panOffset.y());
    m.insert(QStringLiteral("scalePx"), scale());
    m.insert(QStringLiteral("dispX"), disp.x());
    m.insert(QStringLiteral("dispY"), disp.y());
    m.insert(QStringLiteral("dispW"), disp.width());
    m.insert(QStringLiteral("dispH"), disp.height());
    m.insert(QStringLiteral("visX"), vis.x());
    m.insert(QStringLiteral("visY"), vis.y());
    m.insert(QStringLiteral("visW"), vis.width());
    m.insert(QStringLiteral("visH"), vis.height());
    m.insert(QStringLiteral("canvasW"), canvasR.width());
    m.insert(QStringLiteral("canvasH"), canvasR.height());
    m.insert(QStringLiteral("hasPanel"), m_panel != nullptr);
    m.insert(QStringLiteral("paperOnScreen"),
             disp.intersects(rect()) && m_panel != nullptr);
    return m;
}
#endif

// Command callback: restore a selection path (display state only).
void DrawingCanvas::applyPerspectiveForUndo(const QJsonObject &state)
{
    m_perspective.fromJson(state);
    emit perspectiveEdited(); // the Modifier bar re-syncs its sliders
    update();
}

void DrawingCanvas::pushPerspectiveCommand(const QJsonObject &before,
                                           const QString &text)
{
    if (!m_undoStack)
        return;
    const QJsonObject after = m_perspective.toJson();
    if (after == before)
        return; // no-op gesture (e.g. a rejected 4th tap): nothing to undo
    m_undoStack->push(new PerspectiveCommand(this, before, after, text));
}

void DrawingCanvas::beginPerspectiveEdit()
{
    m_perspBefore = m_perspective.toJson();
    m_perspGesture = true;
    m_perspGestureText = QStringLiteral("Perspective Settings");
}

void DrawingCanvas::endPerspectiveEdit(const QString &text)
{
    if (!m_perspGesture)
        return;
    m_perspGesture = false;
    pushPerspectiveCommand(m_perspBefore, text);
}

void DrawingCanvas::applySelectionPathForUndo(const QPainterPath &path)
{
    m_selectionPath = path;
    m_selDrag = false;
    m_lassoPts.clear();
    updateAntsTimer();
    update();
}

void DrawingCanvas::drawSegment(const QPoint &from, const QPoint &to, const QColor &color)
{
    Layer *layer = editableActiveLayer();
    if (!layer)
        return;
    QPen pen(color);
    pen.setWidth(penWidth());
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    // Qt skips zero-length lines entirely, so a click without motion would
    // draw nothing: stamp an explicit round dot instead (tap-to-erase).
    const bool dot = from == to;
    const qreal dotR = penWidth() / 2.0;
    // Only the touched segment repaints (large erasers made full-widget
    // repaints per event lag, same as the brush).
    const qreal pad = dotR + 2.0;
    const QRectF segBounds = QRectF(QPointF(from), QPointF(to)).normalized()
                                 .adjusted(-pad, -pad, pad, pad);
    if (m_strokeMask == StrokeMaskErase) {
        // Selection active: the erase stroke accumulates UNMASKED coverage in
        // the scratch (white alpha); the cached mask caps it once — in the
        // paintEvent preview live, and on release when it is baked. Soft
        // selection edges erase softly and can never saturate hard.
        QPainter sp(&m_strokeBuf);
        sp.setRenderHint(QPainter::Antialiasing, true);
        if (dot) {
            sp.setPen(Qt::NoPen);
            sp.setBrush(Qt::white);
            sp.drawEllipse(QPointF(from), dotR, dotR);
        } else {
            QPen strokePen(Qt::white);
            strokePen.setWidth(penWidth());
            strokePen.setCapStyle(Qt::RoundCap);
            strokePen.setJoinStyle(Qt::RoundJoin);
            sp.setPen(strokePen);
            sp.drawLine(from, to);
        }
        sp.end();
        updateBrushRegion(segBounds);
        return;
    }
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Eraser carves the layer back to TRANSPARENT (revealing layers below /
    // the white paper) — painting white would smear over lower layers.
    if (m_tool == Eraser)
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
    if (dot) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(from), dotR, dotR);
    } else {
        painter.setPen(pen);
        painter.drawLine(from, to);
    }
    painter.end();
    updateBrushRegion(segBounds);
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
    beginLayerEdit();
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintShapeGeometry(painter, true);
    painter.end();
    finalizeLayerEdit(QStringLiteral("Shape"));
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
    beginLayerEdit();
    QPainter painter(&layer->image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintShapeGeometry(painter, true);
    painter.end();
    finalizeLayerEdit(QStringLiteral("Shape"));
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
    m_selectionPath = QPainterPath();
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
        m_selectionPath = combinedSelection(path); // Replace / Add / Subtract
    } else {
        m_selectionPath = combinedSelection(QPainterPath()); // too few: keep base (Add/Sub)
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
// was prior to the change now held in m_selectionPath. No-ops (unchanged region)
// are skipped so degenerate clicks never pollute the history.
void DrawingCanvas::recordSelectionChange(const QPainterPath &before)
{
    if (before == m_selectionPath || !m_undoStack)
        return;
    m_undoStack->push(new SelectionCommand(this, before, m_selectionPath));
}

// User-facing deselect (SelMod Deselect button / Esc): clears the selection
// AND records it in the selection history, so it can be selection-undone.
void DrawingCanvas::deselect()
{
    const QPainterPath before = m_selectionPath;
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
    // PRE-EXISTING defect fixed with the resolution epic: no panel gate —
    // selecting all of a document that does not exist created a phantom
    // fixed-size selection.
    if (!m_panel)
        return;
    const QPainterPath before = m_selectionPath;
    QPainterPath path;
    path.addRect(QRectF(QPointF(0, 0), QSizeF(canvasSize())));
    m_selectionPath = path;
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
    if (!m_panel) // PRE-EXISTING gate gap, same as selectAll()
        return;
    const QPainterPath before = m_selectionPath;
    QPainterPath full;
    full.addRect(QRectF(QPointF(0, 0), QSizeF(canvasSize())));
    m_selectionPath = m_selectionPath.isEmpty() ? full : full.subtracted(m_selectionPath);
    m_selDrag = false;
    m_lassoPts.clear();
    recordSelectionChange(before);
    updateAntsTimer();
    update();
}

void DrawingCanvas::updateAntsTimer()
{
    const bool needed = !m_selectionPath.isEmpty() || m_selDrag || m_floatActive
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

// The vector selection's raster side: a full-canvas ANTIALIASED grayscale
// coverage mask (white x coverage, premultiplied), rebuilt lazily only when
// m_selectionPath has changed — the cache is self-invalidating by comparing
// against the path it was built from, so every place that edits the path
// (tools, booleans, outline moves, undo/redo) is covered automatically.
// DestinationIn with the mask KEEPS exactly the masked coverage and
// DestinationOut CLEARS exactly the complement — alpha + (1-alpha)
// partitions the pixels with no off-by-one and no double coverage.
const QImage &DrawingCanvas::cachedSelectionMask() const
{
    // Size check added with the resolution epic: a path-only test left a
    // wrong-sized mask alive across any event that changed the canvas size
    // without changing the selection path (the one genuinely stale-able
    // cache in the survey).
    if (m_selMaskCache.isNull() || m_selMaskPath != m_selectionPath
        || m_selMaskCache.size() != canvasSize()) {
        m_selMaskCache = QImage(canvasSize(), QImage::Format_ARGB32_Premultiplied);
        m_selMaskCache.fill(Qt::transparent);
        QPainter painter(&m_selMaskCache);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillPath(m_selectionPath, Qt::white);
        painter.end();
        m_selMaskPath = m_selectionPath;
    }
    return m_selMaskCache;
}

// boundingRect-local crop of the cached mask (rect must lie on-canvas, which
// every caller guarantees by intersecting with the layer/canvas rect).
QImage DrawingCanvas::selectionMask(const QRect &boundingRect) const
{
    return cachedSelectionMask().copy(boundingRect);
}

// Copy is not an edit, so it reads the active layer even when locked.
void DrawingCanvas::copySelection()
{
    if (!m_panel || m_selectionPath.isEmpty())
        return;
    const Layer *layer = m_panel->activeLayer();
    if (!layer || layer->image.isNull())
        return;
    const QRect r = m_selectionPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
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
    if (!layer || m_selectionPath.isEmpty())
        return;
    copySelection();
    const QRect r = m_selectionPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;
    beginLayerEdit();
    QPainter p(&layer->image);
    p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    p.drawImage(r.topLeft(), selectionMask(r)); // clears exactly the copied pixels
    p.end();
    finalizeLayerEdit(QStringLiteral("Cut"));
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
    m_selectionPath = QPainterPath(); // the floating outline replaces the selection
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
    if (!layer || m_selectionPath.isEmpty())
        return;
    const QRect r = m_selectionPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
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
        beginLayerEdit();

        // 10) assign the rebuilt image back and refresh.
        layer->image = result;
        finalizeLayerEdit(QStringLiteral("Move"));
        if (!m_selectionPath.isEmpty())
            m_selectionPath.translate(QPointF(aligned) - m_floatPos); // ants follow
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
        beginLayerEdit();
        const QPointF target = m_floatPos + m_floatDelta;
        const QPoint aligned(qRound(target.x()), qRound(target.y()));
        QPainter p(&layer->image);
        p.drawImage(aligned, m_floatImg); // limited only by the canvas bounds
        p.end();
        finalizeLayerEdit(QStringLiteral("Paste"));
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
    if (!layer || m_selectionPath.isEmpty())
        return;
    const QRect r = m_selectionPath.boundingRect().toAlignedRect().intersected(layer->image.rect());
    if (r.isEmpty())
        return;

    // Antialiased mask (from the cache): the lift carries a soft coverage
    // falloff, so the committed result has smooth edges, never jagged stairs.
    m_moveMask = selectionMask(r);
    m_moveSrcRect = r;

    m_transformBuf = layer->image.copy(r); // PRISTINE lifted pixels (never re-transformed)
    {
        QPainter p(&m_transformBuf);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.drawImage(0, 0, m_moveMask);
    }
    m_xformLayerIds = QStringList{layer->id}; // commit resolves by ID
    m_xformBufs = QVector<QImage>{m_transformBuf};
    // Source-subtracted view of the layer, built ONCE for the whole session
    // (layer pixels never change mid-session) — not per repaint.
    QImage hole = layer->image;
    {
        QPainter hp(&hole);
        hp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        hp.drawImage(r.topLeft(), m_moveMask);
    }
    m_xformHoles = QVector<QImage>{hole};
    // SINGLE SOURCE OF TRUTH: the panel's layer image is NOT touched during
    // the session — thumbnails, saves, and panel switches always read the
    // committed state. The VIEW subtracts the lifted source region and shows
    // the transformed buffer instead (see paintEvent); commit rewrites the
    // layer once; cancel simply drops the session buffers.

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

void DrawingCanvas::beginGroupTransform(Layer *group)
{
    if (m_xformActive || !m_panel || !group || !isGroupLayer(*group))
        return;
    QStringList memberIds;
    for (const Layer &member : m_panel->layers)
        if (member.groupId == group->id)
            memberIds.append(member.id);
    beginLayersTransform(memberIds);
}

// Shared lift core: group members or multi-selected rows. Each candidate
// that is visible, unlocked and has artwork lifts into its own pristine
// buffer; ONE box drives them all, and both preview and commit keep every
// layer at its own z-position.
void DrawingCanvas::beginLayersTransform(const QStringList &candidateIds)
{
    if (m_xformActive || !m_panel || candidateIds.isEmpty())
        return;

    // Collect the transformable candidates and the union of their artwork
    // bounds — the ONE box everyone shares.
    QRect r;
    QStringList ids;
    for (const Layer &member : m_panel->layers) {
        if (!candidateIds.contains(member.id) || member.locked
            || isGroupLayer(member)
            || member.type == QLatin1String("background")
            || !m_panel->layerEffectivelyVisible(member))
            continue;
        const QRect art = opaquePixelBounds(member.image);
        if (art.isEmpty())
            continue;
        r = r.isNull() ? art : r.united(art);
        ids.append(member.id);
    }
    if (ids.isEmpty() || r.isEmpty())
        return;

    // One pristine buffer per member (preview and commit both transform each
    // in place, at its layer's z-position). The composite is kept only as
    // the session's non-null sentinel.
    m_xformLayerIds = ids;
    m_xformBufs.clear();
    QImage composite(r.size(), QImage::Format_ARGB32_Premultiplied);
    composite.fill(Qt::transparent);
    {
        QPainter p(&composite);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const QString &id : ids)
            for (const Layer &member : m_panel->layers)
                if (member.id == id) {
                    const QImage buf = member.image.copy(r);
                    m_xformBufs.append(buf);
                    p.setOpacity(qBound(0.0, member.opacity, 1.0));
                    p.drawImage(0, 0, buf);
                }
    }
    m_moveSrcRect = r;
    m_moveMask = QImage(r.size(), QImage::Format_ARGB32_Premultiplied);
    m_moveMask.fill(QColor(255, 255, 255, 255)); // full-rect lift per member
    // Source-subtracted view of every member, built ONCE per session (see
    // beginTransform) — parallel to m_xformBufs.
    m_xformHoles.clear();
    for (const QString &id : ids)
        for (const Layer &member : m_panel->layers)
            if (member.id == id) {
                QImage hole = member.image;
                QPainter hp(&hole);
                hp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                hp.drawImage(r.topLeft(), m_moveMask);
                hp.end();
                m_xformHoles.append(hole);
            }
    m_transformBuf = composite;
    m_xformAutoSel = false; // no synthesized selection: the box IS the lift

    // Seed the box/pivot/warp exactly like beginTransform().
    const QRectF rf(r);
    m_quad = QPolygonF({rf.topLeft(), rf.topRight(), rf.bottomRight(),
                        rf.bottomLeft()});
    m_pivot = rf.center();
    m_pivotCustom = false;
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
    setMouseTracking(true);
    update();
}

// Render the thin-plate-spline warp at professional quality.
//
// Pipeline (identical for the live preview AND the commit, always from the
// PRISTINE buffer):
//   1. The smooth TPS is evaluated on a fine source grid (cellPx source px);
//      each micro-cell is split into two triangles whose mapping is inverted
//      EXACTLY (barycentric), so the deformation is continuous across every
//      shared edge -- no seams, and at this density no visible faceting.
//   2. Every destination pixel is BACKWARD-mapped to source coordinates and
//      the buffer is sampled with premultiplied-alpha BILINEAR interpolation
//      (never point sampling): edges fade cleanly, colours never fringe.
//   3. The whole result is rendered SUPERSAMPLED (kSS x) and downscaled with
//      Qt::SmoothTransformation -- deformed edges come out antialiased with
//      no stair-stepping even under strong deformation.
// The composed image is then drawn ONCE through the painter's transform
// (view transform for the preview, identity for the commit).
void DrawingCanvas::paintWarpedBuffer(QPainter &p, qreal cellPx,
                                      const QImage &overrideBuf) const
{
    const QImage &pristine =
        overrideBuf.isNull() ? m_transformBuf : overrideBuf;
    const qreal srcW = m_moveSrcRect.width();
    const qreal srcH = m_moveSrcRect.height();
    if (srcW <= 0.0 || srcH <= 0.0 || !m_tpsValid || pristine.isNull())
        return;
    // Supersample factor (both preview and commit). 3x, not 2x: where the
    // warp locally COMPRESSES the artwork, a hard source edge's bilinear ramp
    // shrinks below one output pixel, and 2x point sampling can step right
    // over it (aliased binary edges); 3x subsample spacing stays inside the
    // ramp for the compressions a mesh warp produces.
    constexpr int kSS = 3;

    const QImage src =
        pristine.format() == QImage::Format_ARGB32_Premultiplied
            ? pristine
            : pristine.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int sw = src.width(), sh = src.height();

    // 1. Forward-map the fine source grid through the spline. The domain
    // extends ONE pixel beyond the buffer on every side: the bilinear fade at
    // the artwork boundary needs the outer half of its ramp (source coords
    // just outside the buffer, sampling toward transparent) rasterised too,
    // otherwise edges lose their soft half and read harder than they should.
    const qreal gx0 = -1.0, gy0 = -1.0;
    const qreal gw = srcW + 2.0, gh = srcH + 2.0;
    const int nx = qBound(2, qCeil(gw / cellPx), 256);
    const int ny = qBound(2, qCeil(gh / cellPx), 256);
    QVector<QPointF> node((nx + 1) * (ny + 1));
    QVector<QPointF> snode((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) {
            const QPointF sp(gx0 + gw * i / nx, gy0 + gh * j / ny);
            snode[j * (nx + 1) + i] = sp;
            node[j * (nx + 1) + i] = warpMap(sp);
        }

    // Occupancy: skip cells whose source region (plus a 1px bilinear margin)
    // is fully transparent. Exact -- built from every buffer pixel.
    QVector<quint8> occupied(nx * ny, 0);
    const qreal cw = gw / nx, ch = gh / ny;
    for (int y = 0; y < sh; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < sw; ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            const int ci0 = qBound(0, int((x - 1 - gx0) / cw), nx - 1);
            const int ci1 = qBound(0, int((x + 1 - gx0) / cw), nx - 1);
            const int cj0 = qBound(0, int((y - 1 - gy0) / ch), ny - 1);
            const int cj1 = qBound(0, int((y + 1 - gy0) / ch), ny - 1);
            for (int cj = cj0; cj <= cj1; ++cj)
                for (int ci = ci0; ci <= ci1; ++ci)
                    occupied[cj * nx + ci] = 1;
        }
    }

    // Destination bounds (canvas coords), padded for the AA rim and capped so
    // a wildly-dragged point cannot allocate an absurd buffer.
    // (Manual min/max: QRectF's union operator IGNORES empty rects, and a
    // zero-size QRectF around a point is empty — uniting those collapses the
    // bounds to a single node.)
    qreal minX = node.first().x(), maxX = minX;
    qreal minY = node.first().y(), maxY = minY;
    for (const QPointF &d : node) {
        minX = qMin(minX, d.x());
        maxX = qMax(maxX, d.x());
        minY = qMin(minY, d.y());
        maxY = qMax(maxY, d.y());
    }
    QRectF bboxF(QPointF(minX, minY), QPointF(maxX, maxY));
    bboxF.adjust(-2, -2, 2, 2);
    bboxF = bboxF.intersected(QRectF(-256, -256, canvasSize().width() + 512,
                                     canvasSize().height() + 512));
    const QRect bbox = bboxF.toAlignedRect();
    if (bbox.isEmpty())
        return;
    const int W = bbox.width() * kSS, H = bbox.height() * kSS;
    if (qint64(W) * H > qint64(24) * 1024 * 1024)
        return; // degenerate spline blow-up: refuse rather than stall

    QImage out(W, H, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QRgb *outBits = reinterpret_cast<QRgb *>(out.bits());
    const int outStride = out.bytesPerLine() / 4;

    // Premultiplied bilinear tap; transparent outside the buffer, so the
    // artwork boundary fades over one source pixel (clean AA, no fringing).
    auto sample = [&](qreal sx, qreal sy) -> QRgb {
        const int x0 = qFloor(sx), y0 = qFloor(sy);
        const qreal fx = sx - x0, fy = sy - y0;
        auto tap = [&](int tx, int ty) -> QRgb {
            if (tx < 0 || ty < 0 || tx >= sw || ty >= sh)
                return 0;
            return reinterpret_cast<const QRgb *>(src.constScanLine(ty))[tx];
        };
        const QRgb c00 = tap(x0, y0), c10 = tap(x0 + 1, y0);
        const QRgb c01 = tap(x0, y0 + 1), c11 = tap(x0 + 1, y0 + 1);
        auto lerpC = [&](int shift) {
            const qreal t0 = ((c00 >> shift) & 0xff) * (1 - fx)
                           + ((c10 >> shift) & 0xff) * fx;
            const qreal t1 = ((c01 >> shift) & 0xff) * (1 - fx)
                           + ((c11 >> shift) & 0xff) * fx;
            return uint(qBound(0.0, t0 * (1 - fy) + t1 * fy + 0.5, 255.0));
        };
        return (lerpC(24) << 24) | (lerpC(16) << 16) | (lerpC(8) << 8) | lerpC(0);
    };

    // 2. Rasterise each micro-triangle: destination pixels are backward-
    // mapped with the exact barycentric inverse and bilinearly sampled.
    auto rasterTri = [&](const QPointF &sa, const QPointF &sb, const QPointF &sc,
                         QPointF da, QPointF db, QPointF dc) {
        // Into supersampled offscreen pixel coords.
        auto toSS = [&](QPointF d) {
            return QPointF((d.x() - bbox.left()) * kSS, (d.y() - bbox.top()) * kSS);
        };
        da = toSS(da); db = toSS(db); dc = toSS(dc);
        const qreal det = (db.x() - da.x()) * (dc.y() - da.y())
                        - (db.y() - da.y()) * (dc.x() - da.x());
        if (qAbs(det) < 1e-12)
            return;
        const qreal inv = 1.0 / det;
        const int minX = qBound(0, qFloor(qMin(da.x(), qMin(db.x(), dc.x()))), W - 1);
        const int maxX = qBound(0, qCeil(qMax(da.x(), qMax(db.x(), dc.x()))), W - 1);
        const int minY = qBound(0, qFloor(qMin(da.y(), qMin(db.y(), dc.y()))), H - 1);
        const int maxY = qBound(0, qCeil(qMax(da.y(), qMax(db.y(), dc.y()))), H - 1);
        for (int y = minY; y <= maxY; ++y) {
            QRgb *row = outBits + y * outStride;
            const qreal py = y + 0.5;
            for (int x = minX; x <= maxX; ++x) {
                const qreal px = x + 0.5;
                const qreal w1 = ((px - da.x()) * (dc.y() - da.y())
                                - (py - da.y()) * (dc.x() - da.x())) * inv;
                if (w1 < -1e-6 || w1 > 1.0 + 1e-6)
                    continue;
                const qreal w2 = ((db.x() - da.x()) * (py - da.y())
                                - (db.y() - da.y()) * (px - da.x())) * inv;
                if (w2 < -1e-6 || w1 + w2 > 1.0 + 1e-6)
                    continue;
                const qreal sx = sa.x() + w1 * (sb.x() - sa.x()) + w2 * (sc.x() - sa.x());
                const qreal sy = sa.y() + w1 * (sb.y() - sa.y()) + w2 * (sc.y() - sa.y());
                // Sample at the SUPERSAMPLED position: src coords are in
                // buffer space already (grid is in source pixels).
                const QRgb v = sample(sx - 0.5, sy - 0.5);
                if (v != 0)
                    row[x] = v; // shared-edge overlap rewrites identical values
            }
        }
    };

    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            if (!occupied.at(j * nx + i))
                continue;
            const int i00 = j * (nx + 1) + i, i10 = i00 + 1;
            const int i01 = i00 + (nx + 1), i11 = i01 + 1;
            rasterTri(snode.at(i00), snode.at(i10), snode.at(i11),
                      node.at(i00), node.at(i10), node.at(i11));
            rasterTri(snode.at(i00), snode.at(i11), snode.at(i01),
                      node.at(i00), node.at(i11), node.at(i01));
        }

    // 3. Smooth downscale (premultiplied-correct), then ONE draw through
    // the painter's transform with high-quality hints.
    const QImage finalImg = out.scaled(bbox.width(), bbox.height(),
                                       Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawImage(bbox.topLeft(), finalImg);
    p.restore();
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
            if (m_xformActive && !m_transformBuf.isNull()
                && &layer == m_panel->activeLayer()) {
                QImage temp = layer.image; // hide the lifted region, as the view does
                QPainter tp(&temp);
                tp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                tp.drawImage(m_moveSrcRect.topLeft(), m_moveMask);
                tp.end();
                p.drawImage(0, 0, temp);
                continue;
            }
            p.drawImage(0, 0, layer.image);
        }
    }
    p.setOpacity(1.0);
    if (m_xformActive && !m_transformBuf.isNull()) {
        if (m_warpDirty) {
            paintWarpedBuffer(p, 4.0);
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
    // Just outside a corner -> rotate. Remember WHICH corner so the cursor
    // can bend around it (handles 0..3 are TL, TR, BR, BL).
    for (int i = 0; i < 4; ++i) {
        const QPointF w = toWidget.map(handles.at(i));
        if (QLineF(w, widgetPos).length() <= rotateTol) {
            m_rotateCorner = i;
            return XRotate;
        }
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
    case XRotate:  setCursor(m_rotateCursors[m_rotateCorner & 3]); break;
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
    default:       setCursor(defaultCursorShape()); break;
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

    // Commit resolves each lifted layer BY ID (captured at lift time): a
    // selection/stack change mid-session can never paste the result into a
    // different layer — the classic "scale created a duplicate" bug. Group
    // sessions transform every member buffer with the same box and write
    // each back into its own layer; the whole commit is ONE undo entry
    // (a macro when several members took part).
    if (!m_xformLayerIds.isEmpty() && !m_transformBuf.isNull() && m_panel
        && m_undoStack) {
        // Bake every member FIRST, keeping only the layers whose pixels
        // actually changed. The macro opens ONLY when something real gets
        // pushed: an untouched session (e.g. the identity box a relift
        // leaves up) must contribute ZERO history entries — an empty macro
        // (or an AA-rounding micro-diff) on the stack made Ctrl+Z after an
        // Enter commit "undo" nothing instead of the transform itself.
        struct Baked
        {
            Layer *layer;
            QImage before;
            QImage result;
            QRect region;
        };
        QVector<Baked> baked;
        for (int k = 0; k < m_xformLayerIds.size(); ++k) {
            Layer *layer = nullptr;
            for (Layer &candidate : m_panel->layers)
                if (candidate.id == m_xformLayerIds.at(k)) {
                    layer = &candidate;
                    break;
                }
            if (!layer || layer->locked || k >= m_xformBufs.size())
                continue; // deleted/locked mid-session: skip safely

            const QImage before = layer->image;
            QImage result = layer->image;
            {
                QPainter c(&result);
                c.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                c.drawImage(m_moveSrcRect.topLeft(), m_moveMask); // clear source
            }
            {
                QPainter p(&result); // paste the transformed buffer
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.setRenderHint(QPainter::Antialiasing, true);
                if (m_warpDirty) {
                    // This member's own pristine buffer through the mesh —
                    // same path+params as the preview.
                    paintWarpedBuffer(p, 4.0, m_xformBufs.at(k));
                } else {
                    p.setTransform(boxTransform()); // buffer -> canvas
                    p.drawImage(0, 0, m_xformBufs.at(k));
                }
            }
            const QRect region = diffRegion(before, result);
            if (region.isEmpty())
                continue; // unchanged: keep the EXACT pixels, no command
            baked.append({layer, before, result, region});
        }
        // One Enter commit = exactly ONE undo entry covering every affected
        // layer: a single command alone, several under one macro. Only
        // pixels are rewritten — visibility, opacity, lock, and stacking
        // order are untouched by a transform, so undo restores each layer
        // exactly as it was.
        const bool macro = baked.size() > 1;
        if (macro)
            m_undoStack->beginMacro(QStringLiteral("Transform Group"));
        for (const Baked &b : baked) {
            b.layer->image = b.result; // IN PLACE: no new layer, no merging
            m_undoStack->push(new DrawingCommand(
                this, m_panel, b.layer->id, b.region, b.before.copy(b.region),
                b.result.copy(b.region), QStringLiteral("Transform")));
        }
        if (macro)
            m_undoStack->endMacro();
        if (!baked.isEmpty()) {
            invalidateComposite(); // non-active members may sit in the caches
            emit contentChanged();
        }
    }

    m_transformBuf = QImage();
    m_xformLayerIds.clear();
    m_xformBufs.clear();
    m_xformHoles.clear();
    m_layerBackup = QImage();
    m_moveMask = QImage();
    m_warpDirty = false;
    m_warpSel.clear();
    m_warpMarquee = false;
    m_warpHoverIdx = -1;
    m_tpsValid = false;
    m_xformAutoSel = false;
    clearSelection(); // the committed pixels are no longer "selected"
    setCursor(defaultCursorShape());

    // Committing a Warp returns to the DEFAULT move/scale/rotate box (the
    // other modes already read as the default box after commit since they
    // share its handles); the Move Modifier toolbar unchecks via the signal.
    if (m_xformUiMode == XformWarp) {
        m_xformUiMode = XformDefault;
        emit xformUiModeReset();
    }

    // Photoshop-style persistence: the box resets to a fresh axis-aligned
    // default around the committed artwork (or the whole group) and stays up
    // while Move is active.
    if (relift && m_tool == Move)
        liftDefaultTransformBox();
}

void DrawingCanvas::cancelTransform(bool relift)
{
    if (!m_xformActive)
        return;
    m_xformActive = false;
    m_xformMode = XNone;
    setMouseTracking(false);

    // The model was never altered during the session: cancelling only drops
    // the session buffers, and the intact committed layer shows again.
    m_transformBuf = QImage();
    m_xformLayerIds.clear();
    m_xformBufs.clear();
    m_xformHoles.clear();
    m_moveMask = QImage();
    m_warpDirty = false;
    m_warpSel.clear();
    m_warpMarquee = false;
    m_warpHoverIdx = -1;
    m_tpsValid = false;
    if (m_xformAutoSel) { // synthesized whole-artwork selection: drop it too
        m_xformAutoSel = false;
        m_selectionPath = QPainterPath();
    }
    setCursor(defaultCursorShape());

    // Esc while the Move tool stays active: the box resets to the default
    // around the restored artwork instead of disappearing.
    if (relift && m_tool == Move) {
        if (Layer *l = editableActiveLayer()) {
            if (m_selectionPath.isEmpty()) {
                const QRect art = opaquePixelBounds(l->image);
                if (!art.isEmpty()) {
                    QPainterPath path;
                    path.addRect(QRectF(art));
                    m_selectionPath = path;
                    m_xformAutoSel = true;
                }
            }
            if (!m_selectionPath.isEmpty())
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
    if (!m_selectionPath.isEmpty() && !m_selectionPath.contains(QPointF(seed)))
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

    if (m_selectionPath.isEmpty()) {
        layerPtr->image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    } else {
        // Selection active: composite the fill through an ANTIALIASED selection
        // mask so the filled region has smooth edges. (A hard clip path — or a
        // binary mask — leaves jagged 1px stair-stepping at the boundary.)
        const QRect r = m_selectionPath.boundingRect().toAlignedRect().intersected(image.rect());
        if (!r.isEmpty()) {
            // Soft mask from the CACHE: antialiased coverage with a 1px
            // falloff at the selection boundary, rasterised only when the
            // path last changed.
            const QImage mask = selectionMask(r);
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

// Repaint only the widget region the stroke actually touched (padded for AA
// and sub-pixel filtering) instead of the whole widget — with a large brush
// the full-widget repaint per input event was a major part of the lag.
void DrawingCanvas::updateBrushRegion(const QRectF &canvasBounds)
{
    if (canvasBounds.isNull())
        return;
    const QRectF padded = canvasBounds.adjusted(-2, -2, 2, 2);
    update(viewTransform().mapRect(padded).toAlignedRect()
               .adjusted(-3, -3, 3, 3));
}

void DrawingCanvas::beginBrushStroke(const QPointF &canvasPt, qreal pressure,
                                     qreal tiltX, qreal tiltY, qreal rotation,
                                     quint64 timestamp, quint64 seed,
                                     bool rasterizePreview)
{
    m_perspective.beginStroke(canvasPt); // snap assist anchors at stroke start
    Layer *layer = editableActiveLayer();
    if (!layer || !m_panel)
        return;
    syncPaintBrushSettings();
    StrokePoint point;
    point.position = canvasPt;
    point.pressure = qBound<qreal>(0.0, pressure, 1.0);
    point.tiltX = tiltX;
    point.tiltY = tiltY;
    point.rotation = rotation;
    point.viewportRotation = m_viewRotation;
    point.timestamp = timestamp ? timestamp
                                : quint64(QDateTime::currentMSecsSinceEpoch());
    m_paintEngine.beginStroke(paintLayerKey(m_panel, layer->id), layer->image,
                              point, seed, rasterizePreview);
    m_lastBrushPt = canvasPt;
    m_lastBrushPressure = pressure;
    if (const TiledImage *preview = m_paintEngine.previewTiles();
        preview && preview->allocatedTileCount() > 0) {
        updateBrushRegion(QRectF(
            canvasPt - QPointF(m_brushToolSize / 2.0, m_brushToolSize / 2.0),
            QSizeF(m_brushToolSize, m_brushToolSize)));
    }
}

void DrawingCanvas::moveBrushStroke(const QPointF &canvasPt, qreal pressure,
                                    qreal tiltX, qreal tiltY, qreal rotation,
                                    quint64 timestamp)
{
    // The engine is the only stroke rasterizer. (The legacy 5%-spacing dab
    // stamper that used to live here served only the pre-engine QuickShape
    // preview; the preview now runs through the engine too.)
    if (m_paintEngine.strokeActive()) {
        StrokePoint point;
        point.position = canvasPt;
        point.pressure = qBound<qreal>(0.0, pressure, 1.0);
        point.tiltX = tiltX;
        point.tiltY = tiltY;
        point.rotation = rotation;
        point.viewportRotation = m_viewRotation;
        point.timestamp = timestamp ? timestamp
                                    : quint64(QDateTime::currentMSecsSinceEpoch());
        const QRect dirty = m_paintEngine.appendPoint(point);
        updateBrushRegion(dirty);
    }
    // No engine stroke (e.g. the press landed on a locked/hidden layer):
    // nothing rasterizes, only the pointer bookkeeping advances.
    m_lastBrushPt = canvasPt;
    m_lastBrushPressure = pressure;
}

void DrawingCanvas::endBrushStroke(const QString &undoText)
{
    m_brushStroke = false;
    if (m_paintEngine.strokeActive()) {
        Panel *panel = m_panel;
        Layer *layer = editableActiveLayer();
        const QString layerId = layer ? layer->id : QString();
        // Freeze the live preview into ONE canvas-space image before
        // finishStrokeWork() drops the tiles: paintEvent keeps showing it
        // until the async publish lands, so the stroke never vanishes for
        // the render's flight time. The selection mask is applied now —
        // the same mask the commit itself will apply.
        m_pendingPreview = QImage();
        m_pendingPreviewRect = QRect();
        if (const TiledImage *preview = m_paintEngine.previewTiles();
            preview && preview->allocatedTileCount() > 0) {
            QRect bounds;
            for (auto it = preview->allocatedTiles().cbegin();
                 it != preview->allocatedTiles().cend(); ++it)
                bounds = bounds.united(TiledImage::tileLayerRect(it.key()));
            bounds = bounds.intersected(QRect(QPoint(), canvasSize()));
            if (!bounds.isEmpty()) {
                QImage frozen(bounds.size(),
                              QImage::Format_ARGB32_Premultiplied);
                frozen.fill(Qt::transparent);
                QPainter fp(&frozen);
                for (auto it = preview->allocatedTiles().cbegin();
                     it != preview->allocatedTiles().cend(); ++it) {
                    const QRect tileRect = TiledImage::tileLayerRect(it.key());
                    const QRect isect = tileRect.intersected(bounds);
                    if (isect.isEmpty())
                        continue;
                    fp.drawImage(isect.topLeft() - bounds.topLeft(),
                                 it.value(),
                                 isect.translated(-tileRect.topLeft()));
                }
                if (!m_selectionPath.isEmpty()) {
                    fp.setCompositionMode(
                        QPainter::CompositionMode_DestinationIn);
                    fp.drawImage(QPoint(), cachedSelectionMask(), bounds);
                }
                fp.end();
                m_pendingPreview = frozen;
                m_pendingPreviewRect = bounds;
            }
        }
        auto work = m_paintEngine.finishStrokeWork(true);
        const QImage selectionMask = m_selectionPath.isEmpty()
            ? QImage() : cachedSelectionMask();
        work.selectionMask = selectionMask;
        m_paintCommitPending = true;
        auto *watcher = new QFutureWatcher<SankoPaintHostAdapter::StrokeResult>(this);
        connect(watcher,
                &QFutureWatcher<SankoPaintHostAdapter::StrokeResult>::finished,
                this, [this, watcher, panel, layerId, undoText] {
            auto result = watcher->result();
            watcher->deleteLater();
            completePaintStroke(std::move(result), panel, layerId, undoText);
        });
        watcher->setFuture(QtConcurrent::run(
            m_paintGpuPool.get(), [work = std::move(work)] {
                return SankoPaintHostAdapter::render(work);
            }));
        m_editPanel = nullptr; // the engine command owns this stroke's history
        m_editBefore = QImage();
        update();
        return;
    }
    if (m_strokeMask == StrokeMaskPaint) {
        // ONE mask application for the whole stroke: multiply the stroke's
        // alpha by the cached antialiased coverage and composite.
        if (Layer *layer = editableActiveLayer()) {
            QImage masked = m_strokeBuf;
            QPainter mp(&masked);
            mp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            mp.drawImage(0, 0, cachedSelectionMask());
            mp.end();
            QPainter p(&layer->image);
            p.drawImage(0, 0, masked);
        }
        m_strokeMask = StrokeMaskNone;
        m_strokeBuf = QImage();
    }
    finalizeLayerEdit(undoText);
    update();
}

void DrawingCanvas::completePaintStroke(
    SankoPaintHostAdapter::StrokeResult result, Panel *panel,
    const QString &layerId, const QString &undoText)
{
    m_paintCommitPending = false;
    // The published pixels (or, on failure, the truth that nothing landed)
    // take over from the frozen preview; repaint its area either way.
    const QRect pendingRect = m_pendingPreviewRect;
    m_pendingPreview = QImage();
    m_pendingPreviewRect = QRect();
    if (!pendingRect.isEmpty())
        updateBrushRegion(pendingRect);
    m_lastPaintRenderer = result.renderer;
    bool published = false;
    Layer *layer = nullptr;
    if (panel) {
        for (Layer &candidate : panel->layers)
            if (candidate.id == layerId) {
                layer = &candidate;
                break;
            }
    }
    if (result.succeeded && layer && panel) {
            if (m_paintEngine.publish(result, layer->image)) {
                published = true;
                pushPaintStroke(result, panel, layerId, undoText);
                invalidateCompositeRegion(result.affectedRect);
                emit contentChanged();
                emit panelEdited(panel);
            }
    }
    if (!published)
        update();
}

// --- Phase 1 CPU brush engine integration -----------------------------------
// The engine paints in CANVAS coordinates onto its own stroke buffer; the
// commit bakes that buffer onto the active layer ONCE and pushes a single
// region-diffed DrawingCommand through the shared undo path — identical
// undo/redo semantics to every other layer edit.
// Convert the screen-space QuickShape tuning into document units at the
// CURRENT view scale and push it into the session — called once per stroke,
// so hold feel, dwell tolerance, and the velocity gate are zoom-independent.
// --- Dwell chrome: ONE state, ONE position ----------------------------------

DrawingCanvas::QsOverlay DrawingCanvas::quickShapeOverlay() const
{
    if (!m_qsHeld || !m_quickShapeEnabled)
        return QsOverlay::None;
    if (m_quickShape.hasActiveShape())
        return QsOverlay::Hint; // recognised: the ring is gone by definition
    // Still collecting: the ring is SILENT until the reveal point, so an
    // ordinary pause shows nothing at all and there is no flicker to
    // reset. holdProgress() is 0 whenever the hold timer is not running
    // (between strokes, mid-morph), which keeps this None on those paths.
    return m_quickShape.holdProgress() >= kQuickShapeTuning.ringRevealFraction
        ? QsOverlay::Dwell
        : QsOverlay::None;
}

QPointF DrawingCanvas::quickShapeOverlayCentre() const
{
    // Below the slot where Edit Shape | Done appear (fixed top-centre at
    // kQsChromeY), so neither element can collide with them. Widget space.
    const int chromeBottom = kQsChromeY
        + (m_qsDoneButton ? m_qsDoneButton->sizeHint().height() : 30);
    return QPointF(width() / 2.0,
                   chromeBottom + kQsRingGap + kQsRingBackR);
}

double DrawingCanvas::quickShapeDwellSweep() const
{
    // The visible window REMAPPED to a full 0..1 sweep, not the absolute
    // 75-100% slice. A quarter-arc creeping through a quarter-turn in
    // ~225 ms reads as "stuck"; a ring that starts empty and closes
    // completely reads as "filling up, about to fire" — and it means the
    // same thing every time it is seen, because the visible window always
    // spans exactly empty-to-full.
    const double reveal =
        qBound(0.0, double(kQuickShapeTuning.ringRevealFraction), 0.99);
    const double p = m_quickShape.holdProgress();
    return qBound(0.0, (p - reveal) / (1.0 - reveal), 1.0);
}

void DrawingCanvas::applyQuickShapeTiming()
{
    const QTransform T = viewTransform();
    const qreal scale = qMax(0.05, std::hypot(T.m11(), T.m12()));
    quickshape::QuickShapeTiming timing = m_quickShape.timing();
    timing.holdDurationMs = kQuickShapeTuning.holdDurationMs;
    timing.morphDurationMs = kQuickShapeTuning.morphDurationMs;
    timing.dwellRadius = kQuickShapeTuning.dwellRadiusScreenPx / scale;
    timing.maxDwellVelocity =
        kQuickShapeTuning.maxDwellVelocityScreenPxPerSec / scale;
    m_quickShape.setTiming(timing);
}

// A shape was recognized mid-stroke: the rough freehand pixels become a
// corrected vector instead. Roll the active layer back to the pre-stroke
// beginLayerEdit() snapshot (or drop the selection scratch, which never
// touched the layer), and abandon the pending edit — nothing is pushed to
// the undo stack until the corrected path is committed.
void DrawingCanvas::discardRoughStroke()
{
    if (!m_brushStroke)
        return;
    m_brushStroke = false;
    m_paintEngine.cancelStroke(); // rough engine preview was never published
    if (m_strokeMask == StrokeMaskPaint) {
        m_strokeMask = StrokeMaskNone;
        m_strokeBuf = QImage();
    } else if (m_editPanel && !m_editBefore.isNull()) {
        for (Layer &layer : m_editPanel->layers) {
            if (layer.id != m_editLayerId)
                continue;
            QPainter p(&layer.image);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.drawImage(0, 0, m_editBefore);
            break;
        }
    }
    m_editPanel = nullptr;
    m_editBefore = QImage();
    update();
}

void DrawingCanvas::captureQuickShapeBrush()
{
    m_qsBrush.color = m_color;
    m_qsBrush.size = m_brushToolSize;
    m_qsBrush.opacity = m_brushToolOpacity;
    m_qsBrush.hardness = m_brushHardness;
    m_qsBrush.pressureToSize = m_pressureToSize;
    m_qsBrush.pressureToOpacity = m_pressureToOpacity;
    // The FULL engine brush: the commit renders from this copy, so a
    // preset/slider change while the shape is being edited can no longer
    // change what Done produces (only colour ever reached the bake before —
    // the engine brush is live canvas state).
    m_qsFullBrush = m_paintEngine.brush();
    m_qsFullBrush.setColor(m_color); // colour is app state, asserted per stroke
    // One nonzero seed pins scatter/jitter/colour dynamics: every render of
    // this shape replays the SAME randomness.
    do
        m_qsSeed = QRandomGenerator::global()->generate64();
    while (!m_qsSeed);
    m_qsViewRotation = m_viewRotation;
}

// PERMANENT test surface: the geometry-lock test intercepts the exact
// replay stream instead of trusting raster equality (the stage-6 defect
// proved pixels can agree while the geometry is wrong).
QVector<StrokePoint> DrawingCanvas::quickShapeReplayStreamForTest() const
{
    if (!m_quickShape.hasActiveShape())
        return {};
    return quickShapePointStream(m_quickShape.currentCommit());
}

// One point stream, used verbatim by EVERY render of the commit. Timestamps
// are synthetic (1..n) — the engine only uses them for dedup and
// interpolation, and the corrected path has no meaningful wall-clock — and
// the viewport rotation is the captured one, so renders see byte-identical
// inputs regardless of when they run. Stylus tilt/rotation ride along when
// the session recorded them (tablet); mouse strokes carry zeros.
QVector<StrokePoint> DrawingCanvas::quickShapePointStream(
    const quickshape::QuickShapeCommit &commit) const
{
    const int n = int(commit.points.size());
    QVector<qreal> pressures;
    pressures.reserve(n);
    for (int i = 0; i < n; ++i)
        pressures.append(
            qBound<qreal>(0.0, commit.pressures.value(i, 1.0), 1.0));

    // Closed-shape seam correction (policy + constants: kQuickShapeSeamPolicy
    // above). Open shapes keep their natural start/end taper untouched.
    if (commit.isClosed() && n >= 8) {
        const auto &seam = kQuickShapeSeamPolicy;
        const int w = qBound(2, int(std::lround(n * seam.blendFraction)),
                             n / 3);
        // Join target: the average of the pressures just OUTSIDE the two
        // blend windows — the seam fades into what surrounds it.
        const qreal joinTarget = (pressures.at(w) + pressures.at(n - 1 - w))
                                 / 2.0;
        // Conservative floor: never above the stroke's median, so a
        // deliberately light stroke stays uniformly light.
        QVector<qreal> sorted = pressures;
        std::sort(sorted.begin(), sorted.end());
        const qreal floorP =
            qMin(seam.minSeamPressure, sorted.at(sorted.size() / 2));
        for (int i = 0; i < w; ++i) {
            const qreal t = qreal(i) / w;          // 0 at the seam
            const qreal s = t * t * (3.0 - 2.0 * t); // smoothstep, C1 joins
            auto correct = [&](qreal p) {
                return qMax(joinTarget * (1.0 - s) + p * s, floorP);
            };
            pressures[i] = correct(pressures.at(i));
            pressures[n - 1 - i] = correct(pressures.at(n - 1 - i));
        }
    }

    QVector<StrokePoint> stream;
    stream.reserve(n + 1);
    quint64 t = 0;
    auto append = [&](int i) {
        StrokePoint point;
        point.position = commit.points.at(i);
        point.pressure = pressures.at(i);
        point.tiltX = commit.tiltXs.value(i, 0.0);
        point.tiltY = commit.tiltYs.value(i, 0.0);
        point.rotation = commit.rotations.value(i, 0.0);
        point.viewportRotation = m_qsViewRotation;
        point.timestamp = ++t;
        stream.append(point);
    };
    for (int i = 0; i < n; ++i)
        append(i);
    if (commit.isClosed()) // close the loop so e.g. circles have no gap
        append(0);         // (repeats the CORRECTED start pressure — no bump)
    return stream;
}

// Bake the corrected vector by replaying it through the NORMAL brush engine
// — under the COMPLETE state captured at stroke start (full engine brush,
// colour, stroke seed, viewport rotation, and the shared pressure/tilt point
// stream), so nothing that changes mid-edit — preset, sliders, palette,
// camera — can alter the committed pixels. Selection masking follows the
// standard stroke rules, and the whole replay is bracketed by ONE
// beginLayerEdit/endBrushStroke — exactly one undo entry.
void DrawingCanvas::replayQuickShape(const quickshape::QuickShapeCommit &commit)
{
    m_quickShapeOverlay = QPainterPath(); // the vector is being baked
    // PERFORMANCE (bake follow-up): the already-rendered QS preview becomes
    // the flight placeholder, letting the replay below skip the engine's
    // per-move preview tiles — measured as 97% of the bake's synchronous
    // cost at 4K (372 of 388 ms at brush 149 on a ~2960 px shape) for
    // pixels that were shown only until the async publish landed. The
    // placeholder is display-only and is replaced by the published layer
    // pixels; the work object, render, publish, undo patches, and hashes
    // are the identical pipeline either way. When NO preview exists (Done
    // inside the 16 ms coalesce window or the render's flight, a failed
    // render, lifecycle commits that raced the first render), the replay
    // keeps the OLD rasterizing behaviour so the shape never vanishes —
    // a brief visual gap would be worse than the speed is good.
    const QImage bakePlaceholder = m_qsPreview;
    m_qsPreview = QImage();
    ++m_qsPreviewGen; // any in-flight preview render is now stale
    m_qsPreviewShownGen = m_qsPreviewGen; // and can never resurrect
    if (m_qsPreviewTimer)
        m_qsPreviewTimer->stop();
    if (commit.points.size() < 2 || !m_panel || !editableActiveLayer()) {
        update();
        return;
    }
    // The bake renders from the CAPTURED state — full engine brush, stroke
    // seed, viewport rotation, and the shared point stream — never from live
    // canvas state (captured-state-wins; see captureQuickShapeBrush).
    const QsBrushState live{m_color, m_brushToolSize, m_brushToolOpacity,
                            m_brushHardness, m_pressureToSize,
                            m_pressureToOpacity};
    const ::Brush liveBrush = m_paintEngine.brush();
    const qreal liveRotation = m_viewRotation;
    m_color = m_qsBrush.color;
    m_brushToolSize = m_qsBrush.size;
    m_brushToolOpacity = m_qsBrush.opacity;
    m_brushHardness = m_qsBrush.hardness;
    m_pressureToSize = m_qsBrush.pressureToSize;
    m_pressureToOpacity = m_qsBrush.pressureToOpacity;
    m_paintEngine.setBrush(m_qsFullBrush);
    m_viewRotation = m_qsViewRotation; // pins StrokePoint.viewportRotation;
                                       // restored before any repaint runs

    const QVector<StrokePoint> stream = quickShapePointStream(commit);
    beginLayerEdit();
    m_brushStroke = true; // normal stroke path (selection scratch included)
    beginBrushStroke(stream.first().position, stream.first().pressure,
                     stream.first().tiltX, stream.first().tiltY,
                     stream.first().rotation, stream.first().timestamp,
                     m_qsSeed, /*rasterizePreview=*/bakePlaceholder.isNull());
    for (qsizetype i = 1; i < stream.size(); ++i)
        moveBrushStroke(stream.at(i).position, stream.at(i).pressure,
                        stream.at(i).tiltX, stream.at(i).tiltY,
                        stream.at(i).rotation, stream.at(i).timestamp);
    endBrushStroke(QStringLiteral("QuickShape"));
    // Fast path only: with no engine preview tiles, endBrushStroke had no
    // frozen preview to install, so hand it the QS preview instead. The
    // pending-preview mechanism (paint + clear in completePaintStroke) is
    // unchanged; installing after the watcher is armed keeps this a pure
    // display decision. A stale placeholder (the shape edited within the
    // last render's flight) shows the previous geometry for the ~70 ms
    // flight and is then replaced by the CORRECT published pixels.
    if (!bakePlaceholder.isNull() && m_paintCommitPending
        && m_pendingPreview.isNull()) {
        m_pendingPreview = bakePlaceholder;
        m_pendingPreviewRect = QRect(QPoint(), canvasSize());
        update();
    }

    m_viewRotation = liveRotation;
    m_paintEngine.setBrush(liveBrush);
    m_color = live.color;
    m_brushToolSize = live.size;
    m_brushToolOpacity = live.opacity;
    m_brushHardness = live.hardness;
    m_pressureToSize = live.pressureToSize;
    m_pressureToOpacity = live.pressureToOpacity;
    emit contentChanged();
}

void DrawingCanvas::scheduleQuickShapePreview()
{
    if (!m_quickShape.hasActiveShape()) {
        m_qsPreview = QImage();
        return;
    }
    if (m_qsPreviewTimer && !m_qsPreviewTimer->isActive())
        m_qsPreviewTimer->start(); // coalesces bursts of overlay changes
}

// Render the corrected path through the REAL brush engine — the SAME
// beginStroke/appendPoint/finishStrokeWork/render pipeline the Done bake
// uses, consuming the SAME Stage-2 capture (full brush, seed, viewport
// rotation, pressure/tilt point stream) — against a dedicated preview layer
// key and a transparent canvas-sized host. The layer, its engine mirror,
// the composite caches and the undo stack are never touched; the GPU render
// runs async on the stroke pool and a generation counter drops results that
// arrive after the shape changed again. Every render REPLACES the previous
// preview whole. What the artist sees IS what Done bakes (for brushes that
// read the canvas beneath — dual-brush non-normal blends, wet mixing — the
// preview composites over transparency instead of the artwork, the same
// bounded approximation the live freehand stroke preview has always shown).
void DrawingCanvas::renderQuickShapePreview()
{
    if (!m_quickShape.hasActiveShape()) {
        m_qsPreview = QImage();
        m_qsPreviewShownGen = m_qsPreviewGen; // a clear is never resurrected
        update();
        return;
    }
    if (m_qsPreviewInFlight) {
        // The shape changed under an in-flight render: mark it stale (the
        // callback drops it instead of showing an outdated shape) and
        // re-render as soon as it lands.
        ++m_qsPreviewGen;
        m_qsPreviewDirty = true;
        return;
    }
    if (m_paintEngine.strokeActive())
        return; // never interleave with a live stroke's builder state
    const quickshape::QuickShapeCommit commit = m_quickShape.currentCommit();
    if (commit.points.size() < 2) {
        m_qsPreview = QImage();
        m_qsPreviewShownGen = m_qsPreviewGen; // a clear is never resurrected
        update();
        return;
    }

    // PERFORMANCE (stroke-path pass): the engine replay used to run HERE,
    // synchronously — 33 MB host alloc + the appendPoint loop measured up
    // to 37 ms at 4K and 12-24 ms at 960x540, landing a GUI stall on EVERY
    // shape manipulation (recognition, rotate/scale, conversion, vertex
    // drag). The whole build now runs inside the pooled job on a PRIVATE
    // adapter, so the GUI-side cost is only capturing the inputs.
    // Captured-state-wins survives the flight the same way the bake keeps
    // it synchronously: everything the replay needs travels BY VALUE in
    // the job — stream, brush, seed, canvas size, selection mask — and the
    // job never touches live members. The shared m_paintEngine is no
    // longer involved at all (no brush swap, no preview layer key, no
    // forgetLayer bookkeeping), so a live stroke can no longer share any
    // builder state with a preview render; the strokeActive() guard above
    // is kept anyway for behavioural parity — previews still never start
    // mid-stroke. The generation/in-flight/dirty protocol is byte-for-byte
    // the one the stale-race lock already pins, so staleness behaviour is
    // unchanged. Off-thread engine building has precedent: the brush
    // library renders whole preview categories off the UI thread.
    const QVector<StrokePoint> stream = quickShapePointStream(commit);
    const QImage selectionMask = m_selectionPath.isEmpty()
        ? QImage() : cachedSelectionMask(); // bake parity, captured on GUI side
    const ::Brush brush = m_qsFullBrush;
    const quint64 seed = m_qsSeed;
    const QSize size = canvasSize();

    m_qsPreviewInFlight = true;
    const quint64 gen = ++m_qsPreviewGen;
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, gen] {
        const QImage rendered = watcher->result();
        watcher->deleteLater();
        m_qsPreviewInFlight = false;
        // MONOTONIC PROGRESSIVE DISPLAY (drag-blanking fix). The old rule
        // displayed a result only if NO further change had happened while
        // it rendered — under a continuous rotate/scale drag at 4K every
        // ~105 ms flight overlapped another change, so every frame was
        // dropped and the shape vanished for the whole drag (the
        // synchronous pre-fix build had merely masked this latent race by
        // starving the event queue). New rule: display any landed frame
        // NEWER than the one on screen while the shape is alive. Landings
        // are serialized by the in-flight guard, so what the artist sees
        // only ever moves forward — an older result can never replace a
        // newer one — and the dirty chain below still guarantees the final
        // settled frame is the LATEST geometry (the stale-race lock pins
        // exactly that).
        if (m_quickShape.hasActiveShape() && !rendered.isNull()
            && gen > m_qsPreviewShownGen) {
            m_qsPreviewShownGen = gen;
            m_qsPreview = rendered;
            update();
        }
        if (m_qsPreviewDirty) {
            m_qsPreviewDirty = false;
            scheduleQuickShapePreview();
        }
    });
    watcher->setFuture(QtConcurrent::run(
        m_paintGpuPool.get(),
        [stream, brush, seed, size, selectionMask]() -> QImage {
            QImage host(size, QImage::Format_ARGB32_Premultiplied);
            host.fill(Qt::transparent);
            SankoPaintHostAdapter adapter; // private: shares nothing live
            adapter.setBrush(brush);
            adapter.beginStroke(QStringLiteral("quickshape:preview"), host,
                                stream.first(), seed,
                                /*rasterizePreview=*/false);
            for (qsizetype i = 1; i < stream.size(); ++i)
                adapter.appendPoint(stream.at(i));
            auto work = adapter.finishStrokeWork(true);
            work.selectionMask = selectionMask;
            auto result = SankoPaintHostAdapter::render(work);
            if (!result.succeeded || !adapter.publish(result, host))
                return QImage();
            return host;
        }));
}

// --- QuickShape Edit Shape mode ---------------------------------------------

void DrawingCanvas::updateQuickShapeUi()
{
    // Repaint the overlay whenever the dwell-chrome state may have moved:
    // every site that clears m_qsHeld calls this immediately after, so this
    // is the single choke point that guarantees a Dwell/Hint overlay never
    // outlives its state as stale pixels (the recorded defect: the hint
    // survived release because nothing invalidated its region). The hold
    // tick's final frame covers the same ground — deliberate redundancy,
    // since the stale hint shipped precisely because the disappearance
    // depended on one incidental signal.
    update();
    const bool ready = m_quickShape.hasActiveShape() && !m_qsHeld;
    const int gap = 8;
    QWidget *left = nullptr; // Edit Shape (ready) or the type bar (editing)
    if (m_qsEditButton) {
        const bool showButton = ready && !m_qsEditing;
        m_qsEditButton->setVisible(showButton);
        if (showButton) {
            m_qsEditButton->adjustSize();
            left = m_qsEditButton;
        }
    }
    if (m_qsTypeBar) {
        const bool showBar = ready && m_qsEditing;
        m_qsTypeBar->setVisible(showBar);
        if (showBar) {
            m_qsTypeBar->adjustSize();
            left = m_qsTypeBar;
        }
    }
    if (m_qsDoneButton) {
        m_qsDoneButton->setVisible(ready);
        if (ready)
            m_qsDoneButton->adjustSize();
    }
    // Centre the [controls][Done] pair at the top of the canvas.
    if (ready && left && m_qsDoneButton) {
        const int total = left->width() + gap + m_qsDoneButton->width();
        const int x = (width() - total) / 2;
        left->move(x, 10);
        left->raise();
        m_qsDoneButton->move(x + left->width() + gap, 10);
        m_qsDoneButton->raise();
    } else if (ready && m_qsDoneButton) {
        m_qsDoneButton->move((width() - m_qsDoneButton->width()) / 2, 10);
        m_qsDoneButton->raise();
    }
}

void DrawingCanvas::enterQuickShapeEdit()
{
    if (!m_quickShape.hasActiveShape())
        return;
    // ONE canonical geometry (stage-6 geometry-lock fix): after an explicit
    // conversion the canvas geometry is authoritative — re-deriving corner
    // nodes from the dense session samples (structuralVerticesOf) miscounts
    // regular polygons (pentagon -> 6, hexagon -> 8). Derive from the
    // session only when the canvas holds nothing for this shape (fresh
    // recognition); shape dismissal invalidates the held geometry below.
    if (!m_qsGeometry.isValid()
        || m_qsGeometry.name != m_quickShape.currentCommit().name)
        m_qsGeometry = m_quickShape.currentGeometry();
    if (!m_qsGeometry.isValid())
        return;
    m_qsEditing = true;
    m_qsNode = -1;
    m_qsHover = -1;
    rebuildQuickShapeTypeBar();
    updateQuickShapeUi();
    update();
}

void DrawingCanvas::applyQuickShapeGeometry()
{
    m_quickShape.setEditedGeometry(m_qsGeometry); // overlay refresh via signal
}

// The compact type selector offers only alternatives that keep the shape's
// open/closed character; the active entry is highlighted.
void DrawingCanvas::rebuildQuickShapeTypeBar()
{
    if (!m_qsTypeBar)
        return;
    QLayout *layout = m_qsTypeBar->layout();
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    const bool closed = m_qsGeometry.isClosed();
    const QStringList names = closed
        ? QStringList{QStringLiteral("Circle"), QStringLiteral("Ellipse"),
                      QStringLiteral("Triangle"), QStringLiteral("Rectangle"),
                      QStringLiteral("Polygon")}
        : QStringList{QStringLiteral("Line"), QStringLiteral("Polyline"),
                      QStringLiteral("Arc")};
    const QString &current = m_qsGeometry.name;
    for (const QString &name : names) {
        const bool active = name == current
            || (name == QLatin1String("Polygon")
                && (current == QLatin1String("Quadrilateral")
                    || current == QLatin1String("Pentagon")
                    || current == QLatin1String("Hexagon")))
            || (name == QLatin1String("Arc")
                && current == QLatin1String("Elliptical Arc"))
            || (name == QLatin1String("Polyline")
                && (current == QLatin1String("Angled line")
                    || current == QLatin1String("Zigzag")));
        auto *button = new QPushButton(name, m_qsTypeBar);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(active
            ? SankoTheme::themed("QPushButton { background:%PURPLE%; color:#ffffff; border:none;"
                  " border-radius:4px; padding:4px 10px; font-size:11px; }")
            : QStringLiteral(
                  "QPushButton { background:#161616; color:#aaaaaa; border:none;"
                  " border-radius:4px; padding:4px 10px; font-size:11px; }"
                  "QPushButton:hover { color:#ffffff; }"));
        connect(button, &QPushButton::clicked,
                this, [this, name] { convertQuickShapeTo(name); });
        layout->addWidget(button);
        button->show();
    }

    // Polygon vertex-count selector (repair stage 6): visible whenever the
    // Polygon family is active, the CURRENT count highlighted — the explicit
    // replacement for the old ambiguous Polygon tap (which kept vertices
    // from one family and built a hexagon from the other with no indication
    // of either). Counts rebuild from the shared frame, so centre, scale and
    // rotation are preserved.
    const bool polygonFamily = current == QLatin1String("Polygon")
        || current == QLatin1String("Quadrilateral")
        || current == QLatin1String("Pentagon")
        || current == QLatin1String("Hexagon");
    if (closed && polygonFamily) {
        const int currentCount = int(m_qsGeometry.nodes.size());
        for (int sides : {3, 4, 5, 6, 8}) {
            auto *count = new QPushButton(QString::number(sides), m_qsTypeBar);
            count->setFocusPolicy(Qt::NoFocus);
            count->setCursor(Qt::PointingHandCursor);
            const bool active = sides == currentCount;
            count->setStyleSheet(active
                ? SankoTheme::themed("QPushButton { background:%PURPLE%; color:#ffffff;"
                      " border:none; border-radius:4px; padding:4px 7px;"
                      " font-size:11px; }")
                : QStringLiteral(
                      "QPushButton { background:#161616; color:#aaaaaa;"
                      " border:none; border-radius:4px; padding:4px 7px;"
                      " font-size:11px; }"
                      "QPushButton:hover { color:#ffffff; }"));
            connect(count, &QPushButton::clicked, this,
                    [this, sides] { convertQuickShapeToPolygon(sides); });
            layout->addWidget(count);
            count->show();
        }
    }
    m_qsTypeBar->adjustSize();
    updateQuickShapeUi();
}

// Shared frame of the CURRENT closed geometry (repair stage 6): centre,
// rotation, and half-extents measured IN the rotated frame. Every closed-
// family conversion rebuilds its target from these few parameters instead
// of transforming the previous type's points — orientation survives (a
// rotated ellipse becomes a ROTATED rectangle) and repeated conversions
// cannot accumulate error, because nothing is ever re-transformed. Vertex
// shapes carry their orientation in QuickShapeGeometry::rotationRad (the
// field is unused by the vertex sampler), written by every conversion below;
// fresh recognizer output is axis-aligned, so its 0 is correct.
namespace {
struct QsFrame
{
    QPointF centre;
    qreal rot = 0.0;
    qreal rx = 8.0;
    qreal ry = 8.0;
};

QsFrame qsFrameOf(const quickshape::QuickShapeGeometry &g)
{
    using G = quickshape::QuickShapeGeometry;
    QsFrame f;
    if (g.kind == G::Ellipse) {
        f.centre = g.center;
        f.rot = g.rotationRad;
        f.rx = qMax(4.0, g.radiusX);
        f.ry = qMax(4.0, g.radiusY);
        return f;
    }
    // Vertex shapes CARRY their frame (centre/radiusX/radiusY written by
    // every conversion below): an inscribed N-gon does not touch the frame
    // corners, so re-deriving extents from its nodes would shrink the frame
    // on every count change. A hand-edited shape clears the carried frame
    // (see quickShapeEditMove) and falls back to node-derived extents.
    if (g.radiusX > 0.0 && g.radiusY > 0.0) {
        f.centre = g.center;
        f.rot = g.rotationRad;
        f.rx = qMax(4.0, g.radiusX);
        f.ry = qMax(4.0, g.radiusY);
        return f;
    }
    if (g.nodes.isEmpty())
        return f;
    QPointF c(0, 0);
    for (const QPointF &pt : g.nodes)
        c += pt;
    c /= qreal(g.nodes.size());
    f.centre = c;
    f.rot = g.rotationRad;
    const qreal cs = std::cos(f.rot), sn = std::sin(f.rot);
    qreal rx = 4.0, ry = 4.0;
    for (const QPointF &pt : g.nodes) {
        const QPointF d = pt - c; // rotate by -rot into the local frame
        rx = qMax(rx, qAbs(d.x() * cs + d.y() * sn));
        ry = qMax(ry, qAbs(-d.x() * sn + d.y() * cs));
    }
    f.rx = rx;
    f.ry = ry;
    return f;
}

QPointF qsFramePoint(const QsFrame &f, qreal lx, qreal ly)
{
    const qreal cs = std::cos(f.rot), sn = std::sin(f.rot);
    return f.centre + QPointF(lx * cs - ly * sn, lx * sn + ly * cs);
}

// Regular N-gon inscribed in the frame (elliptical when rx != ry), apex up
// in the LOCAL frame — the world rotation comes from the frame itself.
QVector<QPointF> qsFramePolygon(const QsFrame &f, int sides)
{
    QVector<QPointF> nodes;
    nodes.reserve(sides);
    for (int i = 0; i < sides; ++i) {
        const qreal a = 2.0 * M_PI * i / sides - M_PI / 2.0;
        nodes.append(qsFramePoint(f, std::cos(a) * f.rx, std::sin(a) * f.ry));
    }
    return nodes;
}
} // namespace

// Explicit polygon vertex count (repair stage 6): the Polygon type's count
// selector calls this directly; the geometry rebuilds from the shared frame,
// so centre, scale, and rotation are preserved and repeated count changes
// do not drift.
void DrawingCanvas::convertQuickShapeToPolygon(int sides)
{
    using G = quickshape::QuickShapeGeometry;
    const QsFrame f = qsFrameOf(m_qsGeometry);
    G out;
    out.name = QStringLiteral("Polygon");
    out.kind = G::Polygon;
    out.nodes = qsFramePolygon(f, qBound(3, sides, 12));
    out.rotationRad = f.rot;
    out.center = f.centre; // carried frame (see qsFrameOf)
    out.radiusX = f.rx;
    out.radiusY = f.ry;
    if (!out.isValid())
        return;
    m_qsGeometry = out;
    applyQuickShapeGeometry();
    rebuildQuickShapeTypeBar();
    update();
}

// Convert the temporary vector to another type — overlay only, never
// rasterized here. Conversions derive from the current geometry (endpoints,
// vertices, or the overlay's bounding box), preserving open vs closed.
void DrawingCanvas::convertQuickShapeTo(const QString &typeName)
{
    using G = quickshape::QuickShapeGeometry;
    const G g = m_qsGeometry;
    const QRectF box = m_quickShape.overlayPath().boundingRect();
    auto endpoints = [&]() -> QPair<QPointF, QPointF> {
        if (g.kind == G::Ellipse)
            return {g.ellipsePointAt(g.startAngleRad),
                    g.ellipsePointAt(g.startAngleRad + g.spanAngleRad)};
        return {g.nodes.first(), g.nodes.last()};
    };

    G out;
    out.name = typeName;
    if (typeName == QLatin1String("Line")) {
        const auto ends = endpoints();
        out.kind = G::Polyline;
        out.nodes = {ends.first, ends.second};
    } else if (typeName == QLatin1String("Polyline")) {
        out.kind = G::Polyline;
        if (g.kind == G::Ellipse) {
            for (int i = 0; i <= 6; ++i)
                out.nodes.append(g.ellipsePointAt(
                    g.startAngleRad + g.spanAngleRad * i / 6.0));
        } else {
            out.nodes = g.nodes;
        }
    } else if (typeName == QLatin1String("Arc")) {
        out.kind = G::Ellipse;
        if (g.kind == G::Ellipse) {
            out = g;
            out.name = typeName;
        } else {
            const auto ends = endpoints();
            QPointF mid = g.nodes.size() >= 3 ? g.nodes.at(g.nodes.size() / 2)
                                              : QPointF();
            if (g.nodes.size() < 3) { // a straight line: bow it gently
                const QPointF d = ends.second - ends.first;
                mid = (ends.first + ends.second) / 2.0
                      + QPointF(-d.y(), d.x()) * 0.25;
            }
            QPointF center;
            qreal radius = 0.0;
            if (!circumcircle(ends.first, mid, ends.second, &center, &radius)) {
                center = box.center();
                radius = qMax(8.0, (box.width() + box.height()) / 4.0);
            }
            out.center = center;
            out.radiusX = out.radiusY = qMax(4.0, radius);
            out.rotationRad = 0.0;
            const qreal a0 = out.ellipseAngleOf(ends.first);
            const qreal am = out.ellipseAngleOf(mid);
            const qreal a1 = out.ellipseAngleOf(ends.second);
            auto positive = [](qreal a) {
                while (a < 0.0)
                    a += 2.0 * M_PI;
                return std::fmod(a, 2.0 * M_PI);
            };
            const qreal ccwSpan = positive(a1 - a0);
            const bool midOnCcw = positive(am - a0) <= ccwSpan + 1e-6;
            out.startAngleRad = a0;
            out.spanAngleRad = midOnCcw ? ccwSpan : ccwSpan - 2.0 * M_PI;
        }
    } else if (typeName == QLatin1String("Circle")) {
        const QsFrame f = qsFrameOf(g);
        out.kind = G::Ellipse;
        out.center = f.centre;
        out.radiusX = out.radiusY = qMax(4.0, (f.rx + f.ry) / 2.0);
        out.startAngleRad = -M_PI / 2.0;
    } else if (typeName == QLatin1String("Ellipse")) {
        const QsFrame f = qsFrameOf(g);
        out.kind = G::Ellipse;
        out.center = f.centre;
        out.radiusX = f.rx;
        out.radiusY = f.ry;
        out.rotationRad = f.rot; // a rotated frame stays rotated
        out.startAngleRad = -M_PI / 2.0;
    } else if (typeName == QLatin1String("Triangle")) {
        const QsFrame f = qsFrameOf(g);
        out.kind = G::Polygon;
        out.nodes = {qsFramePoint(f, 0.0, -f.ry), qsFramePoint(f, f.rx, f.ry),
                     qsFramePoint(f, -f.rx, f.ry)};
        out.rotationRad = f.rot;
        out.center = f.centre; // carried frame (see qsFrameOf)
        out.radiusX = f.rx;
        out.radiusY = f.ry;
    } else if (typeName == QLatin1String("Rectangle")) {
        const QsFrame f = qsFrameOf(g);
        out.kind = G::Polygon;
        out.nodes = {qsFramePoint(f, -f.rx, -f.ry),
                     qsFramePoint(f, f.rx, -f.ry),
                     qsFramePoint(f, f.rx, f.ry),
                     qsFramePoint(f, -f.rx, f.ry)};
        out.rotationRad = f.rot; // rotated ellipse -> ROTATED rectangle
        out.center = f.centre;   // carried frame (see qsFrameOf)
        out.radiusX = f.rx;
        out.radiusY = f.ry;
    } else if (typeName == QLatin1String("Polygon")) {
        out.kind = G::Polygon;
        if (g.kind != G::Ellipse && g.nodes.size() >= 3) {
            // Keep the fitted vertices (intentional irregularity survives);
            // the count selector this activation reveals is the explicit way
            // to a regular N-gon. Nothing is transformed, so nothing drifts.
            out.nodes = g.nodes;
            out.rotationRad = g.rotationRad;
            out.center = g.center; // keep whatever frame g carried
            out.radiusX = g.radiusX;
            out.radiusY = g.radiusY;
        } else {
            const QsFrame f = qsFrameOf(g);
            out.nodes = qsFramePolygon(f, 6);
            out.rotationRad = f.rot;
            out.center = f.centre; // carried frame (see qsFrameOf)
            out.radiusX = f.rx;
            out.radiusY = f.ry;
        }
    } else {
        return;
    }
    if (!out.isValid())
        return;
    m_qsGeometry = out;
    applyQuickShapeGeometry();
    rebuildQuickShapeTypeBar();
    update();
}

// Editable nodes in CANVAS coordinates. Ellipse family: centre + the two
// axis handles (+ arc endpoints when the shape is open, so the deliberate
// gap stays directly editable). Vertex shapes: one node per vertex.
QVector<QPointF> DrawingCanvas::quickShapeNodes() const
{
    using G = quickshape::QuickShapeGeometry;
    if (m_qsGeometry.kind != G::Ellipse)
        return m_qsGeometry.nodes;
    QVector<QPointF> nodes;
    const qreal rot = m_qsGeometry.rotationRad;
    nodes << m_qsGeometry.center;
    nodes << m_qsGeometry.center
                 + QPointF(qCos(rot) * m_qsGeometry.radiusX,
                           qSin(rot) * m_qsGeometry.radiusX);
    nodes << m_qsGeometry.center
                 + QPointF(-qSin(rot) * m_qsGeometry.radiusY,
                           qCos(rot) * m_qsGeometry.radiusY);
    if (qAbs(m_qsGeometry.spanAngleRad) < 2.0 * M_PI - 0.02) {
        nodes << m_qsGeometry.ellipsePointAt(m_qsGeometry.startAngleRad);
        nodes << m_qsGeometry.ellipsePointAt(m_qsGeometry.startAngleRad
                                             + m_qsGeometry.spanAngleRad);
    }
    return nodes;
}

int DrawingCanvas::quickShapeNodeAt(const QPointF &widgetPos) const
{
    const QTransform T = viewTransform();
    const QVector<QPointF> nodes = quickShapeNodes();
    for (int i = 0; i < nodes.size(); ++i)
        if (QLineF(T.map(nodes.at(i)), widgetPos).length() <= 12.0)
            return i;
    return -1;
}

bool DrawingCanvas::quickShapeEditPress(const QPointF &widgetPos)
{
    const int hit = quickShapeNodeAt(widgetPos);
    if (hit >= 0) {
        m_qsNode = hit;
        update();
        return true;
    }
    // Tapping clearly outside the shape commits it and leaves edit mode
    // (activeShapeChanged(false) resets the editing state).
    const QRectF bounds = m_quickShape.overlayPath().boundingRect().adjusted(
        -m_brushToolSize - 24.0, -m_brushToolSize - 24.0,
        m_brushToolSize + 24.0, m_brushToolSize + 24.0);
    if (!bounds.contains(toCanvasF(widgetPos))) {
        m_quickShape.requestCommit();
        return true;
    }
    return true; // inside the shape but off-node: swallow, never draw
}

void DrawingCanvas::quickShapeEditMove(const QPointF &widgetPos)
{
    using G = quickshape::QuickShapeGeometry;
    if (m_qsNode < 0) { // idle hover feedback
        const int hover = quickShapeNodeAt(widgetPos);
        if (hover != m_qsHover) {
            m_qsHover = hover;
            update();
        }
        return;
    }
    const QPointF p = toCanvasF(widgetPos);
    auto wrapSpan = [](qreal span, qreal sign) {
        const qreal twoPi = 2.0 * M_PI;
        span = std::fmod(span, twoPi);
        if (sign >= 0.0 && span <= 0.0)
            span += twoPi;
        if (sign < 0.0 && span >= 0.0)
            span -= twoPi;
        return span;
    };
    if (m_qsGeometry.kind == G::Ellipse) {
        switch (m_qsNode) {
        case 0:
            m_qsGeometry.center = p;
            break;
        case 1: { // major-axis handle: radius + orientation
            const QPointF d = p - m_qsGeometry.center;
            m_qsGeometry.radiusX = qMax(2.0, QLineF(QPointF(), d).length());
            m_qsGeometry.rotationRad = qAtan2(d.y(), d.x());
            break;
        }
        case 2: { // minor-axis handle
            const QPointF d = p - m_qsGeometry.center;
            m_qsGeometry.radiusY = qMax(2.0, QLineF(QPointF(), d).length());
            m_qsGeometry.rotationRad = qAtan2(d.y(), d.x()) - M_PI / 2.0;
            break;
        }
        case 3: { // arc start: keep the far end fixed, gap follows
            const qreal end =
                m_qsGeometry.startAngleRad + m_qsGeometry.spanAngleRad;
            const qreal ns = m_qsGeometry.ellipseAngleOf(p);
            const qreal span = wrapSpan(end - ns, m_qsGeometry.spanAngleRad);
            if (qAbs(span) > 0.1) {
                m_qsGeometry.startAngleRad = ns;
                m_qsGeometry.spanAngleRad = span;
            }
            break;
        }
        case 4: { // arc end
            const qreal span =
                wrapSpan(m_qsGeometry.ellipseAngleOf(p)
                             - m_qsGeometry.startAngleRad,
                         m_qsGeometry.spanAngleRad);
            if (qAbs(span) > 0.1)
                m_qsGeometry.spanAngleRad = span;
            break;
        }
        default:
            break;
        }
    } else if (m_qsNode < m_qsGeometry.nodes.size()) {
        m_qsGeometry.nodes[m_qsNode] = p;
        // Hand-edited: the carried frame no longer describes the nodes —
        // clear it so conversions re-derive extents from the real shape.
        m_qsGeometry.radiusX = 0.0;
        m_qsGeometry.radiusY = 0.0;
    }
    applyQuickShapeGeometry();
}

void DrawingCanvas::quickShapeEditRelease()
{
    m_qsNode = -1;
    update();
}

// Double-click: on a node deletes it (respecting the minimum count); on a
// segment inserts a node at the projection. Ellipse-family shapes have
// parametric handles only. Always swallowed — never a brush stroke.
bool DrawingCanvas::quickShapeEditDoubleClick(const QPointF &widgetPos)
{
    using G = quickshape::QuickShapeGeometry;
    if (m_qsGeometry.kind == G::Ellipse)
        return true;
    const int minNodes = m_qsGeometry.kind == G::Polygon ? 3 : 2;
    const int hit = quickShapeNodeAt(widgetPos);
    if (hit >= 0) {
        if (m_qsGeometry.nodes.size() > minNodes) {
            m_qsGeometry.nodes.remove(hit);
            m_qsGeometry.name = m_qsGeometry.kind == G::Polygon
                ? QStringLiteral("Polygon")
                : (m_qsGeometry.nodes.size() == 2 ? QStringLiteral("Line")
                                                  : QStringLiteral("Polyline"));
            m_qsNode = -1;
            m_qsHover = -1;
            applyQuickShapeGeometry();
            rebuildQuickShapeTypeBar();
            update();
        }
        return true;
    }
    // Insert on the nearest segment within reach.
    const QPointF p = toCanvasF(widgetPos);
    const int nodeCount = m_qsGeometry.nodes.size();
    const int segments = m_qsGeometry.kind == G::Polygon ? nodeCount
                                                          : nodeCount - 1;
    int bestSegment = -1;
    qreal bestDist = m_brushToolSize / 2.0 + 10.0;
    QPointF bestPoint;
    for (int i = 0; i < segments; ++i) {
        const QPointF a = m_qsGeometry.nodes.at(i);
        const QPointF b = m_qsGeometry.nodes.at((i + 1) % nodeCount);
        const QPointF ab = b - a;
        const qreal len2 = QPointF::dotProduct(ab, ab);
        if (len2 < 1e-9)
            continue;
        const qreal t =
            qBound(0.0, QPointF::dotProduct(p - a, ab) / len2, 1.0);
        const QPointF proj = a + ab * t;
        const qreal dist = QLineF(proj, p).length();
        if (dist < bestDist) {
            bestDist = dist;
            bestSegment = i;
            bestPoint = proj;
        }
    }
    if (bestSegment >= 0) {
        m_qsGeometry.nodes.insert(bestSegment + 1, bestPoint);
        m_qsGeometry.name = m_qsGeometry.kind == G::Polygon
            ? QStringLiteral("Polygon") : QStringLiteral("Polyline");
        applyQuickShapeGeometry();
        rebuildQuickShapeTypeBar();
        update();
    }
    return true;
}

void DrawingCanvas::setQuickShapeEnabled(bool enabled)
{
    if (m_quickShapeEnabled == enabled)
        return;
    commitQuickShape(); // never strand a pending vector behind a toggle
    m_quickShapeEnabled = enabled;
}

// Lifecycle resolution (tool/layer/panel change, save, load, focus loss):
// bake a pending temporary shape through the brush engine; a stroke still
// being collected just detaches from recognition and finishes as ordinary
// freehand through the normal release path.
void DrawingCanvas::commitQuickShape()
{
    if (m_quickShape.hasActiveShape()) {
        // Writability pre-flight (repair stage 8). Mid-lifecycle (panel,
        // tool, or layer change already in motion) the shape cannot be kept
        // pending — but it is never discarded SILENTLY: the user is told
        // what was lost and why, per the no-silent-work-loss rule.
        if (!editableActiveLayer()) {
            m_quickShape.cancelActiveShape();
            update();
            QMessageBox::warning(this, QStringLiteral("QuickShape"),
                QStringLiteral("A pending QuickShape was discarded: its "
                               "target layer is locked or hidden, so it "
                               "could not be baked before this change."));
            return;
        }
        m_quickShape.requestCommit(); // synchronously replays + clears
    } else {
        m_quickShape.reset(); // never leave a stuck pointer/collect state
    }
}

void DrawingCanvas::cancelQuickShape()
{
    m_quickShape.cancelActiveShape();
}

void DrawingCanvas::undo()
{
    if (!m_undoStack)
        return;
    // A pending QuickShape is NOT yet document history (repair stage 7):
    // the first Undo cancels the temporary vector — it neither commits it
    // nor rewinds an older document command in the same action, and since
    // the vector never reached the stack, Redo cannot resurrect it. The
    // next Undo operates on real history normally. Both the toolbar button
    // and Ctrl+Z route through here, so the policy is single-sourced.
    if (m_quickShape.hasActiveShape()) {
        cancelQuickShape();
        return;
    }
    // Resolve any live interactive session first so history stays coherent:
    // a floating paste lands (as its own command) and a live transform
    // gesture COMMITS, becoming the top history entry — this undo then
    // reverses exactly that gesture. (Cancelling here used to discard the
    // gesture AND still rewind the PREVIOUS entry, silently un-toggling
    // visibility or collapsing layer-stack state the user never asked to
    // undo. An untouched box commits nothing, so plain undo is unaffected.)
    commitFloating();
    if (m_xformActive)
        commitTransform(false);
    m_undoStack->undo();
    // Move stays live: a fresh box around the RESTORED artwork, targeting
    // the full current row selection (multi-layer lifts included) — never a
    // stale or partial target.
    resetTransformBox();
}

// Re-apply the exact command that the last undo reversed.
void DrawingCanvas::redo()
{
    if (!m_undoStack)
        return;
    commitFloating();
    if (m_xformActive)
        commitTransform(false); // a live gesture clears the redo branch anyway
    m_undoStack->redo();
    resetTransformBox(); // fresh box around the re-applied artwork
}

void DrawingCanvas::clearCanvas()
{
    Layer *layer = editableActiveLayer();
    if (!layer)
        return;
    beginLayerEdit();
    if (!m_selectionPath.isEmpty()) {
        // An active selection scopes the clear: erase through the cached
        // antialiased mask (soft edge), leaving everything else untouched.
        const QRect r = m_selectionPath.boundingRect().toAlignedRect()
                            .intersected(layer->image.rect());
        if (!r.isEmpty()) {
            QPainter p(&layer->image);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(r.topLeft(), selectionMask(r));
            p.end();
        }
    } else {
        layer->image.fill(Qt::transparent); // clears the ACTIVE layer only
    }
    finalizeLayerEdit(QStringLiteral("Clear"));
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

// Rebuild the below/above composite caches when the panel, active index, or
// stack size changed, or after invalidateComposite(). Accumulation matches
// the direct loop step for step (paper first, per-layer group-aware
// effective opacity, SourceOver) so the cached and direct paths render the
// same stack identically.
void DrawingCanvas::ensureComposite()
{
    SANKO_REQUIRE_PANEL(); // only paintEvent calls this, after its own gate
    const int active = m_panel ? m_panel->activeLayerIndex : -1;
    const int count = m_panel ? m_panel->layers.size() : 0;
    if (m_compValid && m_compPanel == m_panel && m_compActive == active
        && m_compCount == count)
        return;
    ensurePanelCpuCoherent(m_panel, BrushCoherenceTrigger::CompositeCache);
    const QSize cs = canvasSize();
    if (m_compBelow.size() != cs) { // allocate once, reuse across rebuilds
        m_compBelow = QImage(cs, QImage::Format_ARGB32_Premultiplied);
        m_compAbove = QImage(cs, QImage::Format_ARGB32_Premultiplied);
    }
    m_compBelow.fill(Qt::white); // the paper
    m_compAbove.fill(Qt::transparent);
    if (m_panel) {
        QPainter below(&m_compBelow);
        QPainter above(&m_compAbove);
        below.setRenderHint(QPainter::SmoothPixmapTransform, true);
        above.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int i = 0; i < m_panel->layers.size(); ++i) {
            if (i == active)
                continue; // the live layer is drawn between the caches
            const Layer &layer = m_panel->layers.at(i);
            if (isGroupLayer(layer))
                continue;
            const double effOpacity = m_panel->layerEffectiveOpacity(layer);
            if (!m_panel->layerEffectivelyVisible(layer)
                || layer.image.isNull() || effOpacity <= 0.0)
                continue;
            QPainter &p = i < active ? below : above;
            p.setOpacity(effOpacity);
            p.drawImage(0, 0, layer.image);
        }
    }
    m_compValid = true;
    m_compPanel = m_panel;
    m_compActive = active;
    m_compCount = count;
}

// --- Workspace grid (gutter-only, screen-space) ----------------------------
// Pure view state: the setters repaint but touch no layer pixels, no undo,
// no save/load, no thumbnails, and do NOT invalidate the composite caches
// (m_compValid is deliberately left alone — the caches hold canvas-space
// pixels the grid never enters).

void DrawingCanvas::loadGridSettings()
{
    const QSettings s(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QString k = QStringLiteral("canvas/grid/v1/");
    m_gridVisible = s.value(k + QStringLiteral("visible"), false).toBool();
    const int style = s.value(k + QStringLiteral("style"), 0).toInt();
    m_gridStyle = (style >= 0 && style <= 3) ? GridStyle(style)
                                             : GridStyle::Square;
    QColor gc(s.value(k + QStringLiteral("gridColor"),
                      QStringLiteral("#2a2a2a")).toString());
    if (gc.isValid())
        m_gridColor = gc;
    QColor bg(s.value(k + QStringLiteral("gutterColor"),
                      QStringLiteral("#0a0a0a")).toString());
    if (bg.isValid())
        m_gutterColor = bg;
    m_gridTileDirty = true;
}

void DrawingCanvas::saveGridSettings() const
{
    QSettings s(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
    const QString k = QStringLiteral("canvas/grid/v1/");
    s.setValue(k + QStringLiteral("visible"), m_gridVisible);
    s.setValue(k + QStringLiteral("style"), int(m_gridStyle));
    s.setValue(k + QStringLiteral("gridColor"), m_gridColor.name());
    s.setValue(k + QStringLiteral("gutterColor"), m_gutterColor.name());
}

void DrawingCanvas::setGridVisible(bool on)
{
    if (m_gridVisible == on)
        return;
    m_gridVisible = on;
    saveGridSettings();
    update();
}

void DrawingCanvas::setGridStyle(GridStyle style)
{
    if (m_gridStyle == style)
        return;
    m_gridStyle = style;
    m_gridTileDirty = true;
    saveGridSettings();
    update();
}

void DrawingCanvas::setGridColor(const QColor &color)
{
    if (!color.isValid() || m_gridColor == color)
        return;
    m_gridColor = color;
    m_gridTileDirty = true;
    saveGridSettings();
    update();
}

void DrawingCanvas::setGutterColor(const QColor &color)
{
    if (!color.isValid() || m_gutterColor == color)
        return;
    m_gutterColor = color;
    saveGridSettings();
    update();
}

void DrawingCanvas::ensureGridTile()
{
    // One kGridSpacing^2 tile, rebuilt only when style or colour changes;
    // paintEvent blits it with drawTiledPixmap over just the update region,
    // so the per-repaint cost is a clipped pattern fill.
    if (!m_gridTileDirty && !m_gridTile.isNull())
        return;
    m_gridTileDirty = false;
    const int s = kGridSpacing;
    m_gridTile = QPixmap(s, s);
    m_gridTile.fill(Qt::transparent);
    QPainter p(&m_gridTile);
    const QVector<QPoint> corners{QPoint(0, 0), QPoint(s, 0), QPoint(0, s),
                                  QPoint(s, s)};
    switch (m_gridStyle) {
    case GridStyle::Square:
        // Full 1px lines along the tile's top and left edges; tiling
        // completes them into continuous rules every 25px.
        p.setPen(QPen(m_gridColor, 1));
        p.drawLine(0, 0, s - 1, 0);
        p.drawLine(0, 0, 0, s - 1);
        break;
    case GridStyle::Dashed: {
        // Same rules, dashed. 3-on/2-off = period 5, which divides the 25px
        // tile exactly, so the pattern tiles without a seam or phase jump.
        QPen pen(m_gridColor, 1);
        pen.setDashPattern({3.0, 2.0});
        p.setPen(pen);
        p.drawLine(0, 0, s - 1, 0);
        p.drawLine(0, 0, 0, s - 1);
        break;
    }
    case GridStyle::Cross:
        // A 7px + at every intersection, nothing between them. Drawn at all
        // four tile corners (clipped); tiling reassembles whole crosses.
        p.setPen(QPen(m_gridColor, 1));
        for (const QPoint &c : corners) {
            p.drawLine(c.x() - 3, c.y(), c.x() + 3, c.y());
            p.drawLine(c.x(), c.y() - 3, c.x(), c.y() + 3);
        }
        break;
    case GridStyle::Dot:
        // A 3px antialiased dot at every intersection.
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(m_gridColor);
        for (const QPoint &c : corners)
            p.drawEllipse(QPointF(c), 1.5, 1.5);
        break;
    }
}

bool DrawingCanvas::viewInteractionActive() const
{
    // Every pointer state machine that must not be interrupted by a popup:
    // live strokes (engine brush and classic tools), panning, selection
    // drags (marquee/lasso/polygon), any transform-box drag, and Edit Shape
    // mode.
    return m_brushStroke || m_drawing || m_strokeMask != StrokeMaskNone
        || m_panning || m_selDrag || m_selOutlineDrag
        || !m_polygonPts.isEmpty() || m_xformMode != XNone || m_qsEditing;
}

QMenu *DrawingCanvas::buildGridMenu()
{
    auto *menu = new QMenu(this);

    QAction *show = menu->addAction(tr("Show Grid"));
    show->setCheckable(true);
    show->setChecked(m_gridVisible);
    show->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Apostrophe));
    connect(show, &QAction::toggled, this,
            [this](bool on) { setGridVisible(on); });

    menu->addSeparator();
    auto *styleGroup = new QActionGroup(menu);
    const struct { const char *name; GridStyle style; } styles[] = {
        {"Square", GridStyle::Square},
        {"Cross", GridStyle::Cross},
        {"Dot", GridStyle::Dot},
        {"Dashed", GridStyle::Dashed},
    };
    for (const auto &def : styles) {
        QAction *a = menu->addAction(tr(def.name));
        a->setCheckable(true);
        a->setChecked(m_gridStyle == def.style);
        a->setActionGroup(styleGroup);
        const GridStyle st = def.style;
        connect(a, &QAction::triggered, this, [this, st] {
            setGridStyle(st);
            setGridVisible(true); // picking a style implies wanting to see it
        });
    }

    menu->addSeparator();
    QMenu *colorMenu = menu->addMenu(tr("Grid Colour"));
    const struct { const char *name; const char *hex; } gridColors[] = {
        {"Dim Grey (default)", "#2a2a2a"}, {"Grey", "#4d4d4d"},
        {"Light Grey", "#9a9a9a"},         {"White", "#ffffff"},
        {"Sanko Accent", "#7c6ef6"},
    };
    for (const auto &def : gridColors) {
        QAction *a = colorMenu->addAction(tr(def.name));
        a->setCheckable(true);
        a->setChecked(m_gridColor == QColor(def.hex));
        const QColor c(def.hex);
        connect(a, &QAction::triggered, this, [this, c] { setGridColor(c); });
    }
    QAction *custom = colorMenu->addAction(tr("Custom..."));
    connect(custom, &QAction::triggered, this, [this] {
        const QColor c =
            QColorDialog::getColor(m_gridColor, this, tr("Grid Colour"));
        if (c.isValid())
            setGridColor(c);
    });

    QMenu *bgMenu = menu->addMenu(tr("Gutter Background"));
    const struct { const char *name; const char *hex; } gutterColors[] = {
        {"Dark (default)", "#0a0a0a"}, {"Charcoal", "#1f1f1f"},
        {"Dark Grey", "#2b2b2b"},      {"Mid Grey", "#4d4d4d"},
        {"Silver", "#b3b3b3"},         {"Light", "#e8e8e8"},
    };
    for (const auto &def : gutterColors) {
        QAction *a = bgMenu->addAction(tr(def.name));
        a->setCheckable(true);
        a->setChecked(m_gutterColor == QColor(def.hex));
        const QColor c(def.hex);
        connect(a, &QAction::triggered, this, [this, c] { setGutterColor(c); });
    }
    return menu;
}

void DrawingCanvas::contextMenuEvent(QContextMenuEvent *event)
{
    // The grid menu opens anywhere over the canvas widget — gutter OR paper:
    // at high zoom the paper can fill the viewport, and right-click has no
    // other job here. Never mid-interaction: a popup would orphan the
    // pointer state machine's release event.
    if (viewInteractionActive()) {
        event->ignore();
        return;
    }
    QMenu *menu = buildGridMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(event->globalPos());
    event->accept();
}

void DrawingCanvas::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    // Gutter background + optional workspace grid, both SCREEN-SPACE: painted
    // before the world transform so they are fixed to the viewport — they
    // never pan, zoom, or rotate with the artwork, and the constant 25px
    // spacing means there is no low-zoom moire to subdivide away. The paper
    // is composited OPAQUE on top (both the composite caches and the direct
    // path start from a solid white fill covering the full canvas rect), so
    // the grid can never show through the document, whatever the layer
    // stack's visibility or alpha — no clipping geometry needed.
    painter.fillRect(rect(), m_gutterColor);
    if (m_gridVisible) {
        ensureGridTile();
        const QRect er = event->rect(); // draw only the update region; the
        // pattern phase keeps the tile anchored to the widget origin.
        painter.drawTiledPixmap(er, m_gridTile,
                                QPointF(er.x() % kGridSpacing,
                                        er.y() % kGridSpacing));
    }

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
    // The update region in CANVAS space (from the paint event — the painter's
    // clipBoundingRect() reports only user clips, so it is empty in normal
    // backing-store paints as well as grab()/render() passes). The live
    // stroke previews compose only this region; the dirty-region updates
    // keep it a small rect per input event during large-brush strokes.
    const QRect strokeClipC = T.inverted()
                                  .mapRect(QRectF(event->rect()))
                                  .toAlignedRect()
                                  .adjusted(-2, -2, 2, 2)
                                  .intersected(canvasRi);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Everything canvas-anchored is drawn in CANVAS coordinates through T, so
    // it zooms/rotates/flips together with the artwork.
    painter.save();
    painter.setWorldTransform(T);
    // The document is ONE surface, but its stack is drawn as several
    // full-canvas quads (paper/compBelow, the active layer, compAbove) sharing
    // identical geometry. Antialiased, each quad carries its own partial edge
    // coverage a, so the stack composites in SCREEN space and the paper leaks
    // through the layers above it at weight a(1-a) — up to 25% white along the
    // outermost row/column. That is the border gap: with a (20,20,20) stroke on
    // a boundary landing at device y=349.69 the edge pixel measured 61,61,61,
    // matching 255*a(1-a) exactly. It only appears when the boundary falls on a
    // FRACTIONAL device coordinate, which is why integer 100%/400% tests missed
    // it while the 0.85 startup zoom and any rotation show it.
    //
    // Aliasing the content quads makes every layer cover exactly the same
    // device pixels, so the stack composites as the single surface it is and no
    // leak is possible at any zoom, pan or angle. Interior quality is untouched
    // — SmoothPixmapTransform still does the bilinear filtering; this hint only
    // governs the quad's own outline. The tradeoff is that a ROTATED document
    // edge is now hard rather than feathered; the antialiased frame outline
    // drawn just outside it carries the smooth boundary instead.
    painter.setRenderHint(QPainter::Antialiasing, false);

    // White paper, then every VISIBLE layer bottom-to-top with its opacity.
    // Light-table ghosts are drawn ONCE on the paper, just after the
    // background/paper layer and before the drawing layers.
    // ONE layer's pixels (transform session in-place preview, live stroke
    // previews, or the plain image) — shared by the cached and direct paths
    // below so both render identically.
    auto paintLayerContent = [&](const Layer &layer, double effOpacity) {
        painter.setOpacity(effOpacity);
        if (m_xformActive && !m_xformBufs.isEmpty()
            && m_xformLayerIds.contains(layer.id)) {
            // Live transform session: the MODEL keeps the committed pixels
            // (single source of truth); the view shows the precomputed
            // source-subtracted layer (built once at lift, not per frame)
            // and draws THIS layer's transformed buffer right here, at the
            // layer's real z-position — the Move tool never brings the
            // edited layer to the front, not even temporarily. A layer
            // hidden or deleted mid-session simply stops painting.
            const int bi = m_xformLayerIds.indexOf(layer.id);
            if (bi >= 0 && bi < m_xformHoles.size())
                painter.drawImage(0, 0, m_xformHoles.at(bi));
            if (bi >= 0 && bi < m_xformBufs.size()) {
                painter.save();
                if (m_warpDirty) {
                    paintWarpedBuffer(painter, 4.0, m_xformBufs.at(bi));
                } else {
                    painter.setWorldTransform(boxTransform(), true);
                    painter.drawImage(0, 0, m_xformBufs.at(bi));
                }
                painter.restore();
            }
            return;
        }
        const bool liveStroke = m_strokeMask != StrokeMaskNone
            && &layer == m_panel->activeLayer();
        if (liveStroke && m_strokeMask == StrokeMaskErase) {
            // Live erase preview: the active layer minus the capped stroke —
            // exactly what the release will bake. The selection mask applies
            // only when a selection exists; eraser opacity scales the whole
            // stroke's coverage uniformly. Composed only over the UPDATE
            // region (canvas space): the dirty-region updates repaint a
            // small rect per input event, so the full-canvas copies that made
            // large strokes lag shrink to the touched area.
            const QRect region = strokeClipC;
            if (!region.isEmpty()) {
                QImage temp = layer.image.copy(region);
                QImage cut = m_strokeBuf.copy(region);
                QPainter cp(&cut);
                cp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                if (!m_selectionPath.isEmpty())
                    cp.drawImage(QPoint(0, 0), cachedSelectionMask(), region);
                if (m_eraserOpacity < 1.0)
                    cp.fillRect(cut.rect(),
                                QColor(0, 0, 0,
                                       qRound(m_eraserOpacity * 255.0)));
                cp.end();
                QPainter tp(&temp);
                tp.setCompositionMode(QPainter::CompositionMode_DestinationOut);
                tp.drawImage(0, 0, cut);
                tp.end();
                painter.drawImage(region.topLeft(), temp);
            }
        } else {
            painter.drawImage(0, 0, layer.image);
            if (&layer == m_panel->activeLayer()) {
                // Live stroke preview: the tiles are composed into ONE
                // region image and drawn with a single drawImage. Drawing
                // each 256px tile as its own quad under the scaled/rotated
                // view transform made every tile edge a filtering seam, and
                // a tile skipped by one partial repaint but blended by the
                // next made those seams clip-dependent — a flickering grid.
                // One quad has no interior boundaries, and strokeClipC's 2px
                // margin past the update rect keeps the composite's own edge
                // fringe outside the repainted area.
                const TiledImage *preview = m_paintEngine.previewTiles();
                const bool livePreview =
                    preview && preview->allocatedTileCount() > 0;
                if (livePreview && !strokeClipC.isEmpty()) {
                    QImage comp;
                    for (auto it = preview->allocatedTiles().cbegin();
                         it != preview->allocatedTiles().cend(); ++it) {
                        const QRect tileRect =
                            TiledImage::tileLayerRect(it.key());
                        const QRect isect = tileRect.intersected(strokeClipC);
                        if (isect.isEmpty())
                            continue;
                        if (comp.isNull()) {
                            comp = QImage(strokeClipC.size(),
                                          QImage::Format_ARGB32_Premultiplied);
                            comp.fill(Qt::transparent);
                        }
                        QPainter cp(&comp);
                        cp.drawImage(isect.topLeft() - strokeClipC.topLeft(),
                                     it.value(),
                                     isect.translated(-tileRect.topLeft()));
                    }
                    if (!comp.isNull()) {
                        if (!m_selectionPath.isEmpty()) {
                            QPainter maskPainter(&comp);
                            maskPainter.setCompositionMode(
                                QPainter::CompositionMode_DestinationIn);
                            maskPainter.drawImage(QPoint(),
                                                  cachedSelectionMask(),
                                                  strokeClipC);
                        }
                        painter.drawImage(strokeClipC.topLeft(), comp);
                    }
                } else if (!livePreview && !m_pendingPreview.isNull()
                           && m_pendingPreviewRect.intersects(strokeClipC)) {
                    // Commit in flight: the frozen preview bridges the gap
                    // between finishStrokeWork() dropping the live tiles
                    // and the async publish landing in layer.image, so the
                    // stroke never vanishes after release.
                    painter.drawImage(m_pendingPreviewRect.topLeft(),
                                      m_pendingPreview);
                }
            }
            if (liveStroke && m_strokeMask == StrokeMaskPaint) {
                // Live paint preview: the mask-capped stroke over the layer,
                // update-region-bounded like the erase preview above.
                const QRect region = strokeClipC;
                if (!region.isEmpty()) {
                    QImage s = m_strokeBuf.copy(region);
                    QPainter sp(&s);
                    sp.setCompositionMode(
                        QPainter::CompositionMode_DestinationIn);
                    sp.drawImage(QPoint(0, 0), cachedSelectionMask(), region);
                    sp.end();
                    painter.drawImage(region.topLeft(), s);
                }
            }
        }
    };

    // FAST PATH (the common case): paper + everything below the active layer
    // and everything above it come from the two composite caches, so a
    // repaint touches at most three images however deep the stack is. Any
    // state the caches cannot represent (transform session lifting arbitrary
    // layers, light-table interleaving) falls back to the direct loop —
    // identical output either way, since both call paintLayerContent().
    const int activeIdx = m_panel->activeLayerIndex;
    if (!m_xformActive && !m_lightTable && activeIdx >= 0
        && activeIdx < m_panel->layers.size()) {
        ensureComposite();
        painter.drawImage(0, 0, m_compBelow); // white paper + layers below
        const Layer &active = m_panel->layers.at(activeIdx);
        const double effOpacity = m_panel->layerEffectiveOpacity(active);
        if (!isGroupLayer(active) && m_panel->layerEffectivelyVisible(active)
            && !active.image.isNull() && effOpacity > 0.0)
            paintLayerContent(active, effOpacity);
        painter.setOpacity(1.0);
        painter.drawImage(0, 0, m_compAbove); // layers above
    } else {
        painter.fillRect(canvasR, Qt::white);
        bool lightTableDrawn = false;
        for (const Layer &layer : m_panel->layers) {
            if (m_lightTable && !lightTableDrawn
                && layer.type != QLatin1String("background")) {
                drawLightTable(painter, canvasRi);
                lightTableDrawn = true;
            }
            // Group folders paint nothing; members inherit folder visibility
            // and multiply by folder opacity (matches flattenedPixmap).
            if (isGroupLayer(layer))
                continue;
            const double effOpacity = m_panel->layerEffectiveOpacity(layer);
            if (!m_panel->layerEffectivelyVisible(layer) || layer.image.isNull()
                || effOpacity <= 0.0)
                continue;
            paintLayerContent(layer, effOpacity);
        }
        if (m_lightTable && !lightTableDrawn) { // only a background layer
            painter.setOpacity(1.0);
            drawLightTable(painter, canvasRi);
        }
    }
    painter.setOpacity(1.0);

    // Onion skin: faint blue ghost of the previous panel (display only).
    if (m_onionSkin && !m_ghost.isNull()) {
        painter.setOpacity(0.30);
        painter.drawPixmap(canvasRi, m_ghost);
        painter.setOpacity(1.0);
    }

    // Document content is done; overlays below are outlines and guides, which
    // do want antialiasing.
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Cosmetic pens: 1px on screen regardless of zoom/rotation.
    auto cosmetic = [](const QColor &c, qreal w = 0.0) {
        QPen p(c, w);
        p.setCosmetic(true);
        return p;
    };
    // The canvas draws NO permanent outline. A cosmetic frame here — even
    // pushed geometrically outside canvasR — always composites a constant
    // pale screen pixel between the artwork and the gutter (blatant at 400%),
    // because a 1px antialiased line straddles device pixels at almost every
    // zoom. The white paper edge itself is the document/workspace separator.
    // Any boundary line must come from an optional overlay (camera frame).

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
        // Outset half a screen pixel (canvas units) so the cosmetic outline
        // sits wholly in the gutter and never blends into the outermost
        // artwork row/column — the same defect the removed permanent frame
        // had. This is an OPTIONAL overlay; the normal canvas has no border.
        const qreal frameOutset = 0.5 / scale();
        const QRectF frameR = canvasR.adjusted(-frameOutset, -frameOutset,
                                               frameOutset, frameOutset);
        painter.setPen(cosmetic(QColor("#cccccc")));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(frameR);
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

    // Perspective guides: DISPLAY-ONLY overlay — never written into a layer,
    // never in flattenedPixmap() or exports. Canvas space through T, clipped
    // to the canvas so the "infinite" rays stop at its edges.
    if (m_perspective.isVisible()) {
        painter.save();
        painter.setClipRect(canvasR);
        m_perspective.paintGuides(painter, canvasR);
        painter.restore();
    }

    // Off-canvas VP beacons: ALWAYS visible (whenever guides are shown) for
    // EVERY VP outside the canvas, whichever tool is active — clipped
    // strictly OUTSIDE the canvas rect, so the artwork is never painted
    // over. On-canvas VPs draw nothing.
    if (m_perspective.isVisible()) {
        painter.save();
        QPainterPath outside;
        outside.addPolygon(painter.worldTransform().inverted().map(
            QPolygonF(QRectF(rect()))));
        outside.closeSubpath();
        QPainterPath canvasPath;
        canvasPath.addRect(canvasR);
        painter.setClipPath(outside.subtracted(canvasPath));
        for (int v = 0; v < m_perspective.count(); ++v)
            m_perspective.paintEdgeIndicator(painter, canvasR, v);
        painter.restore();
    }

    // QuickShape: the corrected shape rendered by the SAME engine pipeline
    // the Done bake runs (captured full brush, seed, and point stream) into
    // a scratch image — the preview IS what Done will bake. In edit mode a
    // thin cosmetic outline is drawn on top as a UI-only editing guide; it
    // never represents brush thickness and is never committed.
    if (!m_qsPreview.isNull() && m_quickShape.hasActiveShape()) {
        painter.save();
        painter.setClipRect(canvasR);
        painter.drawImage(0, 0, m_qsPreview);
        painter.restore();
    }
    // Hold feedback (repair stage 9), cosmetic overlay only — never part
    // of any layer, flattenedPixmap, or export.
    //
    // ONE state, drawn by ONE switch: the dwell ring and the
    // post-recognition hint are values of QsOverlay, so they cannot both
    // appear. They also share quickShapeOverlayCentre(), so recognition
    // is a straight swap in place — ring out, hint in, same pixel, no gap
    // and no crossfade (a fade would leave a dissolving ring on screen
    // after the thing it measured has already happened).
    //
    // The ring stays hidden for the first ringRevealFraction of the hold
    // and sweeps the remainder. Resets are therefore invisible before the
    // reveal — nothing was drawn — and INSTANT after it: the state simply
    // reads None again on the next paint, which is what "reset" should
    // look like. Any interruption (focus loss, tool/layer change, hide)
    // clears m_qsHeld or the session's hold timer, so both values fall
    // back to None without a separate teardown path.
    const QsOverlay overlay = quickShapeOverlay();
    if (overlay != QsOverlay::None) {
        painter.save();
        // WIDGET space, explicitly: when the canvas is zoomed, the painter
        // reaches this block still carrying the view transform, which threw
        // the old pen-anchored ring (and the post-recognition hint) clean
        // off-widget at any zoom != 1. Chrome never lives in document space.
        painter.resetTransform();
        painter.setRenderHint(QPainter::Antialiasing, true);
        // OPAQUE backings on BOTH, deliberately: this is chrome over the
        // user's ARTWORK, which can be white paper or solid ink, so a
        // contrast ratio against any assumed background is fiction (the
        // panel-number lesson — that defect was a semi-transparent chip,
        // and the hint below carried the same alpha-200 bug until now).
        // Every legibility-bearing pixel sits on its backing.
        const QPointF c = quickShapeOverlayCentre();
        switch (overlay) {
        case QsOverlay::Dwell: {
            painter.setPen(QPen(QColor(0x2a, 0x2a, 0x2a), 1.0));
            painter.setBrush(QColor(0x16, 0x16, 0x16)); // OPAQUE
            painter.drawEllipse(c, kQsRingBackR, kQsRingBackR);
            const QRectF ring(c.x() - kQsRingR, c.y() - kQsRingR,
                              kQsRingR * 2.0, kQsRingR * 2.0);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(0x6e, 0x6e, 0x6e), 3.0)); // track
            painter.drawEllipse(ring);
            QPen arc(SankoTheme::kAccentLight, 3.0);
            arc.setCapStyle(Qt::RoundCap);
            painter.setPen(arc);
            painter.drawArc(ring, 90 * 16,
                            int(-quickShapeDwellSweep() * 360.0 * 16));
            break;
        }
        case QsOverlay::Hint: {
            const QString hint = QStringLiteral("Drag: Rotate | Scale");
            QFont f = painter.font();
            f.setPixelSize(10);
            painter.setFont(f);
            // Centred on the same point the ring used, so the swap happens
            // where the eye already is.
            QRectF text(QRectF(painter.fontMetrics().boundingRect(hint))
                            .adjusted(-6, -3, 6, 3));
            text.moveCenter(c);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x16, 0x16, 0x16)); // OPAQUE
            painter.drawRoundedRect(text, 4, 4);
            painter.setPen(QColor(0xcc, 0xcc, 0xcc));
            painter.drawText(text, Qt::AlignCenter, hint);
            break;
        }
        case QsOverlay::None:
            break; // unreachable: guarded above
        }
        painter.restore();
    }
    if (m_qsEditing && !m_quickShapeOverlay.isEmpty()) {
        painter.save();
        painter.setClipRect(canvasR);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen guidePen(QColor(0x7c, 0x6e, 0xf6, 190), 1.0);
        guidePen.setCosmetic(true);
        guidePen.setStyle(Qt::DashLine);
        painter.setPen(guidePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(m_quickShapeOverlay);
        painter.restore();
    }

    // In-progress shape preview (canvas coords, through T).
    if (m_shapeDrag || (m_tool == Shapes && !m_polygonPts.isEmpty()))
        paintShapeGeometry(painter, false);

    // Floating pixels (mid-move or un-committed paste).
    if (m_floatActive && !m_floatImg.isNull())
        painter.drawImage(m_floatPos + m_floatDelta, m_floatImg);

    // (The live transform preview is drawn INSIDE the layer loop above, at
    // each lifted layer's own z-position and effective opacity — stacking
    // order never changes during a Move session.)

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

    // Perspective tool with no VPs yet: centred prompt chip over the canvas
    // (display-only, Sanko dark chip in the Modifier-bar language).
    if (m_tool == Perspective && m_panel && m_perspective.count() == 0) {
        const QString msg = QStringLiteral(
            "Tap anywhere to create the first Vanishing Point.");
        QFont chipFont(QStringLiteral("Inter"));
        chipFont.setPixelSize(12);
        chipFont.setWeight(QFont::DemiBold);
        painter.setFont(chipFont);
        const QFontMetricsF fm(chipFont);
        QRectF chip(0, 0, fm.horizontalAdvance(msg) + 28, fm.height() + 16);
        chip.moveCenter(QRectF(displayRect()).center());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x21, 0x21, 0x21, 166));
        painter.drawRoundedRect(chip, 8, 8);
        painter.setPen(QColor(0xcc, 0xcc, 0xcc));
        painter.drawText(chip, Qt::AlignCenter, msg);
    }

    // Perspective editing handles (Perspective tool): WIDGET space and NOT
    // clipped to the canvas — vanishing points may sit outside its bounds.
    if (m_tool == Perspective && m_panel)
        m_perspective.paintHandles(painter, T, m_perspHover);

    // QuickShape edit nodes: WIDGET space (constant screen size) so they stay
    // grabbable at any zoom; the hovered/dragged node grows for feedback.
    if (m_qsEditing && m_quickShape.hasActiveShape()) {
        const QVector<QPointF> nodes = quickShapeNodes();
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < nodes.size(); ++i) {
            const QPointF w = T.map(nodes.at(i));
            const qreal r =
                (i == m_qsHover || i == m_qsNode) ? 7.0 : 5.0;
            painter.setPen(QPen(QColor(0, 0, 0, 150), 3.0));
            painter.setBrush(SankoTheme::kPurple);
            painter.drawEllipse(w, r, r);
            painter.setPen(QPen(Qt::white, 1.6));
            painter.drawEllipse(w, r, r);
        }
        painter.restore();
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
        && (!m_selectionPath.isEmpty() || m_selDrag || m_floatActive || polyInProgress)) {
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
                ants = m_selectionPath.translated(m_floatDelta);
        } else {
            ants = m_selectionPath;
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
    // PRE-EXISTING defect fixed with the resolution epic: this handler had
    // no panel gate, so Ctrl+wheel over the empty workspace ran the
    // anchor-preserving zoom math against a document that did not exist.
    if (!m_panel) {
        event->ignore();
        return;
    }
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
    // A pending QuickShape: Escape discards the temporary vector, Enter
    // bakes it through the brush engine.
    if (m_quickShape.hasActiveShape()) {
        if (event->key() == Qt::Key_Escape) {
            m_quickShape.cancelActiveShape();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            m_quickShape.requestCommit();
            return;
        }
    }

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
        if (!m_selectionPath.isEmpty() || m_selDrag) {
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
            setCursor(defaultCursorShape());
        return;
    }
    QWidget::keyReleaseEvent(event);
}

// A broken mouse grab (another widget grabbing, popup, system steal) ends
// the stroke exactly like focus loss: commit, never orphan.
bool DrawingCanvas::event(QEvent *event)
{
    if (event->type() == QEvent::UngrabMouse)
        finishInterruptedStroke();
    return QWidget::event(event);
}

void DrawingCanvas::focusOutEvent(QFocusEvent *event)
{
    clearPenUiLatch(); // never leave a control stuck pressed on focus loss
    finishInterruptedStroke(); // a live stroke commits, never orphans
    if (m_qsHeld) {    // pen focus torn away mid-hold: drop the held flag so
        m_qsHeld = false; // the UI never sticks in the held state
        updateQuickShapeUi();
    }

    // Focus moving elsewhere bakes a READY temporary shape (a stroke still
    // being drawn is left to the normal release path) — through the stage-8
    // pre-flight, so a locked/hidden layer makes this an ANNOUNCED discard,
    // never a silent one.
    if (m_quickShape.hasActiveShape())
        commitQuickShape();
    // Losing focus mid-hold would otherwise leave the pan modifier stuck on.
    m_spaceHeld = false;
    if (!m_panning)
        setCursor(defaultCursorShape());
    QWidget::focusOutEvent(event);
}

void DrawingCanvas::mousePressEvent(QMouseEvent *event)
{
    // Edit Shape mode swallows every canvas press: nodes drag, taps outside
    // commit — nothing here may start a brush stroke.
    if (m_qsEditing && event->button() == Qt::LeftButton) {
        quickShapeEditPress(event->position());
        return;
    }

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

    // Ctrl + Left click: Photoshop-style auto-select — pick the topmost
    // VISIBLE layer with opaque pixels under the cursor, whatever layer it
    // lives on; the Layers panel highlights that row. Warp mode keeps Ctrl
    // for its mesh-topology editing.
    if ((event->modifiers() & Qt::ControlModifier)
        && !(m_xformActive && m_xformUiMode == XformWarp)) {
        const QPoint cp = toCanvas(event->pos());
        if (QRect(QPoint(0, 0), canvasSize()).contains(cp)) {
            for (int i = m_panel->layers.size() - 1; i >= 0; --i) {
                const Layer &layer = m_panel->layers.at(i);
                if (isGroupLayer(layer)
                    || layer.type == QLatin1String("background")
                    || !m_panel->layerEffectivelyVisible(layer)
                    || layer.image.isNull())
                    continue;
                if (qAlpha(layer.image.pixel(cp)) > 25) {
                    emit layerPickRequested(layer.id);
                    break;
                }
            }
        }
        return; // the pick click never draws and never starts a box drag
    }

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

    // Perspective tool: press on a VP handle grabs it (select + drag);
    // pressing empty space TAP-CREATES the next VP anywhere in the workspace,
    // inside OR outside the canvas (VP1 starts the horizon, VP2 tilts it, a
    // tap clearly off the horizon adds VP3). Runs BEFORE the on-canvas and
    // editable-layer gates — guide editing never touches pixels.
    if (m_tool == Perspective) {
        // Snapshot BEFORE the gesture mutates anything; the matching command
        // is pushed on release (create+place or move = one undo entry).
        m_perspBefore = m_perspective.toJson();
        m_perspHandle = m_perspective.hitTest(event->position(), viewTransform());
        if (m_perspHandle >= 0) {
            m_perspGesture = true;
            m_perspGestureText = QStringLiteral("Move Vanishing Point");
            m_perspective.setSelected(m_perspHandle);
            emit perspectiveEdited();
            update();
        } else {
            m_perspHandle =
                m_perspective.addVanishingPoint(toCanvasF(event->position()));
            if (m_perspHandle >= 0) { // keep dragging to fine-place the new VP
                m_perspGesture = true;
                m_perspGestureText = QStringLiteral("Create Vanishing Point");
                emit perspectiveEdited();
                update();
            }
        }
        return;
    }

    // A click anywhere (canvas or letterbox) bakes a pending QuickShape
    // before anything else happens — the classic tap-away commit.
    if (m_quickShape.hasActiveShape() && event->button() == Qt::LeftButton)
        m_quickShape.requestCommit();

    // Widget-wide press acceptance for the PAINTING tools (canvas-edge fix):
    // a Brush or Eraser stroke may begin in the gutter — out-of-paper points
    // flow to the engine, which clips PIXELS to the document while keeping
    // the path for smoothing/spacing/scatter. Document-anchored tools
    // (shapes, selections, fill, move) keep the paper gate.
    if (!displayRect().contains(event->pos())
        && m_tool != Brush && m_tool != Eraser)
        return;
    if (!editableActiveLayer())
        return; // locked / hidden / missing layer: ignore strokes, no cursor change

    // Selection Modifier "Move" mode: with a live selection, dragging any
    // selection tool translates the OUTLINE only (never pixels).
    if (m_selOutlineMove && !m_selectionPath.isEmpty()
        && (m_tool == SelectRect || m_tool == SelectEllipse || m_tool == Lasso
            || m_tool == SelectPoly)) {
        m_selOutlineDrag = true;
        m_selOutlineStartC = toCanvasF(event->position());
        m_selOutlineBase = m_selectionPath;
        return;
    }

    switch (m_tool) {
    case Eraser: {
        beginLayerEdit();
        m_drawing = true;
        if (!m_selectionPath.isEmpty() || m_eraserOpacity < 1.0) {
            // Same stroke-level masking as the brush: erase coverage builds
            // in the scratch and the mask/strength caps it ONCE (preview +
            // bake) — partial opacity erases uniformly across the stroke,
            // with no double-erase at segment joints.
            m_strokeMask = StrokeMaskErase;
            m_strokeBuf = QImage(canvasSize(), QImage::Format_ARGB32_Premultiplied);
            m_strokeBuf.fill(Qt::transparent);
        }
        // UNCLAMPED anchor (canvas-edge fix): toCanvas() pinned the press to
        // the document border, so an eraser stroke leaving the paper crawled
        // along the edge. drawSegment endpoints may lie off-document —
        // QPainter clips the pixels to the image.
        m_lastCanvas = toCanvasF(event->position()).toPoint();
        m_perspective.beginStroke(toCanvasF(event->position())); // snap anchor
        drawSegment(m_lastCanvas, m_lastCanvas, m_color); // dot on click (eraser clears)
        break;
    }
    case Brush: {
        if (m_paintCommitPending)
            break; // UI remains responsive while the previous stroke publishes
        // Mouse strokes carry no pressure: fixed 1.0 (tablets use tabletEvent).
        beginLayerEdit();
        m_brushStroke = true;
        emit liveBrushStrokeStarted(); // live input only (see the signal)
        const QPointF pt =
            stabilizeStrokePoint(toCanvasF(event->position()), true);
        if (m_quickShapeEnabled) {
            applyQuickShapeTiming();
            captureQuickShapeBrush();
            m_quickShape.pointerPress(pt, 1.0);
            m_qsHeld = true;
            m_qsHoldTick->start(); // hold-progress feedback (stage 9)
            updateQuickShapeUi();
        }
        beginBrushStroke(pt, 1.0);
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
        beginLayerEdit();
        floodFill(toCanvas(event->pos()));
        finalizeLayerEdit(QStringLiteral("Fill"));
        emit contentChanged();
        break;
    case SelectRect:
    case SelectEllipse:
        // Add/Subtract combine the new shape with the pre-drag selection;
        // Replace starts fresh. (A bare click = degenerate drag: Replace clears,
        // Add/Subtract leave the existing selection untouched.)
        m_selGestureBase = m_selectionPath; // selection-history "before" snapshot
        m_selBase = m_selOp == SelReplace ? QPainterPath() : m_selectionPath;
        clearSelection();
        m_selDrag = true;
        m_selStartC = m_selCurrentC = toCanvasF(event->position());
        updateAntsTimer();
        update();
        break;
    case Lasso:
        m_selGestureBase = m_selectionPath;
        m_selBase = m_selOp == SelReplace ? QPainterPath() : m_selectionPath;
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
            m_selGestureBase = m_selectionPath;
            m_selBase = m_selOp == SelReplace ? QPainterPath() : m_selectionPath;
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
    case Perspective: // handled by the intercept above; nothing draws here
        break; // non-drawing tools: their panels drive the overlays
    }
}

void DrawingCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_qsEditing) {
        quickShapeEditMove(event->position());
        return;
    }
    if (m_panning) {
        m_panOffset = m_panStartOffset + QPointF(event->pos() - m_panStartScreen);
        update();
        return;
    }
    if (m_tool == Perspective) {
        if ((event->buttons() & Qt::LeftButton) && m_perspHandle >= 0) {
            m_perspective.moveVanishingPoint(m_perspHandle,
                                             toCanvasF(event->position()));
            update();
        } else if (!(event->buttons() & Qt::LeftButton)) {
            // Idle hover: the handle under the cursor grows slightly.
            const int hover =
                m_perspective.hitTest(event->position(), viewTransform());
            if (hover != m_perspHover) {
                m_perspHover = hover;
                update();
            }
        }
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
        m_selectionPath = m_selOutlineBase.translated(qRound(raw.x()), qRound(raw.y()));
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
        if (m_shapeDrag && m_shapeKind == ShapeLine) // snap the line to a VP ray
            m_shapeCurrentC = m_perspective.snapToRay(m_shapeStartC, m_shapeCurrentC);
        update(); // live preview
        return;
    }
    if (m_quickShape.hasActiveShape() && (event->buttons() & Qt::LeftButton)) {
        // Recognized: the held pointer rotates/scales the corrected vector.
        m_quickShape.pointerMove(toCanvasF(event->position()), 1.0);
        return;
    }
    if (m_brushStroke) {
        // Stabilize BEFORE the perspective snap so a snapped stroke stays
        // exactly on its ray; QuickShape and the engine receive the same
        // filtered point (mouse: fixed pressure).
        const QPointF pt = m_perspective.snapPoint(
            stabilizeStrokePoint(toCanvasF(event->position()), false));
        if (m_quickShapeEnabled)
            m_quickShape.pointerMove(pt, 1.0);
        moveBrushStroke(pt, 1.0);
        return;
    }
    if (m_drawing) {
        const QPoint p =
            m_perspective.snapPoint(toCanvasF(event->position())).toPoint();
        drawSegment(m_lastCanvas, p, m_color);
        m_lastCanvas = p;
    }
}

void DrawingCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_qsEditing && event->button() == Qt::LeftButton) {
        quickShapeEditRelease();
        return;
    }
    if (m_panning
        && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_panning = false;
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : defaultCursorShape());
        return;
    }
    if (m_tool == Perspective && event->button() == Qt::LeftButton) {
        if (m_perspGesture) {
            m_perspGesture = false;
            pushPerspectiveCommand(m_perspBefore, m_perspGestureText);
        }
        m_perspHandle = -1;
        return;
    }
    if (m_quickShape.hasActiveShape() && event->button() == Qt::LeftButton
        && !m_brushStroke) {
        // The corrected vector stays as a temporary overlay after release.
        m_quickShape.pointerRelease(toCanvasF(event->position()), 1.0);
        m_qsHeld = false;
        updateQuickShapeUi();
        return;
    }
    if (m_brushStroke && event->button() == Qt::LeftButton) {
        if (m_quickShapeEnabled)
            m_quickShape.pointerRelease(toCanvasF(event->position()), 1.0);
        m_qsHeld = false;
        endBrushStroke();
        updateQuickShapeUi();
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
        m_selectionPath = combinedSelection(path);
        m_selBase = QPainterPath();
        m_lassoPts.clear();
        recordSelectionChange(m_selGestureBase); // one history entry per gesture
        updateAntsTimer();
        update();
        return;
    }
    if (m_shapeDrag && event->button() == Qt::LeftButton) {
        m_shapeCurrentC = toCanvasF(event->position());
        if (m_shapeKind == ShapeLine) // the commit matches the snapped preview
            m_shapeCurrentC = m_perspective.snapToRay(m_shapeStartC, m_shapeCurrentC);
        commitDragShape();
        return;
    }
    if (m_drawing)
        finishEraseStroke();
}

// The whole-stroke erase finalize — shared by the normal release and by
// finishInterruptedStroke() (focus loss / broken grab), so an interrupted
// erase commits exactly like a released one.
void DrawingCanvas::finishEraseStroke()
{
    m_drawing = false;
    if (m_strokeMask == StrokeMaskErase) {
        // ONE mask/strength application for the whole erase stroke.
        if (Layer *layer = editableActiveLayer()) {
            QImage cut = m_strokeBuf;
            QPainter cp(&cut);
            cp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            if (!m_selectionPath.isEmpty())
                cp.drawImage(0, 0, cachedSelectionMask());
            if (m_eraserOpacity < 1.0)
                cp.fillRect(cut.rect(),
                            QColor(0, 0, 0,
                                   qRound(m_eraserOpacity * 255.0)));
            cp.end();
            QPainter p(&layer->image);
            p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            p.drawImage(0, 0, cut);
        }
        m_strokeMask = StrokeMaskNone;
        m_strokeBuf = QImage();
    }
    finalizeLayerEdit(QStringLiteral("Erase"));
    emit contentChanged();
}

// Delivery ended without a release — focus torn away or the mouse grab
// broken mid-stroke. The partial stroke COMMITS (work is preserved, never
// silently dropped — the QuickShape lifecycle policy, approved for strokes
// too). A stroke that deposited nothing pushes no undo entry, so a
// gutter-only interruption leaves history untouched. Nothing stays stuck:
// no open engine stroke, no held flags, no erase scratch.
void DrawingCanvas::finishInterruptedStroke()
{
    if (m_brushStroke) {
        if (m_quickShapeEnabled)
            m_quickShape.pointerRelease(m_lastBrushPt, 0.0);
        m_qsHeld = false;
        endBrushStroke();
        updateQuickShapeUi();
    }
    if (m_drawing)
        finishEraseStroke();
}

void DrawingCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_qsEditing && event->button() == Qt::LeftButton) {
        quickShapeEditDoubleClick(event->position());
        return; // an editing double-click never begins a brush stroke
    }

    // Perspective: double-clicking a VP handle removes that vanishing point
    // (the horizon and guide fans re-derive from the survivors).
    if (m_tool == Perspective && event->button() == Qt::LeftButton) {
        const int hit = m_perspective.hitTest(event->position(), viewTransform());
        if (hit >= 0) {
            const QJsonObject before = m_perspective.toJson();
            m_perspective.removeVanishingPoint(hit);
            m_perspHandle = -1;
            m_perspHover = -1;
            emit perspectiveEdited();
            update();
            pushPerspectiveCommand(before, QStringLiteral("Delete Vanishing Point"));
        }
        return;
    }

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
// Recursive pen hit test: childAt() returns the DEEPEST descendant (which
// can be a label or icon inside a button); walk up until the owning
// QAbstractButton — or the canvas — is found.
QAbstractButton *DrawingCanvas::penUiButtonAt(const QPointF &widgetPos) const
{
    QWidget *w = childAt(widgetPos.toPoint());
    while (w && w != this) {
        if (auto *button = qobject_cast<QAbstractButton *>(w))
            return button;
        w = w->parentWidget();
    }
    return nullptr;
}

// Every cancellation path funnels here: tablet release, focus loss,
// QuickShape teardown, tool change. (Widget destruction needs nothing —
// QPointer nulls itself.) After a clear, the next canvas stroke begins
// normally.
void DrawingCanvas::clearPenUiLatch()
{
    if (m_penUiTarget)
        m_penUiTarget->setDown(false);
    m_penUiTarget.clear();
    m_penUiDeadZone = false;
}

void DrawingCanvas::tabletEvent(QTabletEvent *event)
{
    if (m_tool != Brush) {
        event->ignore();
        return;
    }

    // Tablet -> UI router (repair stage 5; ownership rule 1). A latched
    // interaction consumes every tablet event until release: Move updates
    // only the latched control's pressed look — activation NEVER transfers
    // to a different button — and Release activates only if the pen is
    // still over the same valid target. Accepting the events means Qt never
    // synthesizes the fallback mouse sequence, so one tap is exactly one
    // action and rapid taps can no longer coalesce into mouseDblClick.
    if (m_penUiTarget || m_penUiDeadZone) {
        if (event->type() == QEvent::TabletRelease) {
            QAbstractButton *target = m_penUiTarget; // QPointer -> may be null
            const bool activate = target
                && penUiButtonAt(event->position()) == target
                && target->isVisible() && target->isEnabled()
                && isAncestorOf(target);
            clearPenUiLatch();
            if (activate)
                target->click(); // full Qt press/release/clicked sequence, once
        } else if (event->type() == QEvent::TabletMove && m_penUiTarget) {
            m_penUiTarget->setDown(
                penUiButtonAt(event->position()) == m_penUiTarget);
        }
        event->accept();
        return;
    }
    // An in-progress stroke / hold / node drag keeps canvas ownership, so
    // dragging across a button mid-stroke does not tear the pointer away.
    const bool midCanvasInteraction =
        m_brushStroke || m_qsHeld || (m_qsEditing && m_qsNode >= 0);
    if (!midCanvasInteraction && event->type() == QEvent::TabletPress) {
        if (QAbstractButton *button = penUiButtonAt(event->position())) {
            if (button->isEnabled()) {
                m_penUiTarget = button;
                button->setDown(true);
            } else {
                m_penUiDeadZone = true; // disabled control: accepted dead zone
            }
            event->accept();
            return;
        }
    }
    // Non-button child widgets (none today) keep the pre-router fallback:
    // ignore, so Qt synthesizes mouse events for them and nothing draws.
    if (!midCanvasInteraction) {
        QWidget *child = childAt(event->position().toPoint());
        if (child && child != this) {
            event->ignore();
            return;
        }
    }

    // Edit Shape mode: the pen edits nodes — accept() consumes the event so
    // Qt never synthesizes a duplicate mouse action, and nothing draws. A
    // quick second pen-tap on the same node counts as the delete gesture.
    if (m_qsEditing) {
        switch (event->type()) {
        case QEvent::TabletPress: {
            const int node = quickShapeNodeAt(event->position());
            const qint64 now = m_qsTabletClock.elapsed();
            if (node >= 0 && node == m_qsLastTabletNode
                && now - m_qsLastTabletPressMs < 350) {
                quickShapeEditDoubleClick(event->position());
            } else {
                quickShapeEditPress(event->position());
            }
            m_qsLastTabletNode = node;
            m_qsLastTabletPressMs = now;
            break;
        }
        case QEvent::TabletMove:
            quickShapeEditMove(event->position());
            break;
        case QEvent::TabletRelease:
            quickShapeEditRelease();
            break;
        default:
            break;
        }
        event->accept();
        return;
    }

    switch (event->type()) {
    case QEvent::TabletPress:
        // A pen tap anywhere resolves a pending QuickShape (tap-away bake)
        // BEFORE the new stroke snapshots the layer.
        if (m_quickShape.hasActiveShape())
            m_quickShape.requestCommit();
        // Widget-wide press acceptance (canvas-edge fix): a pen stroke may
        // begin in the gutter; the engine clips pixels, not the path. The
        // stage-5 router and edit-mode handlers already ran above, so a
        // press over a canvas-child control can never reach this branch.
        if (m_panel && editableActiveLayer() && !m_paintCommitPending) {
            beginLayerEdit();
            m_brushStroke = true;
            emit liveBrushStrokeStarted(); // live input only (see the signal)
            const QPointF pt =
                stabilizeStrokePoint(toCanvasF(event->position()), true);
            if (m_quickShapeEnabled) {
                applyQuickShapeTiming();
                captureQuickShapeBrush();
                m_quickShape.pointerPress(pt, event->pressure(),
                                          event->xTilt(), event->yTilt(),
                                          event->rotation());
                m_qsHeld = true;
                m_qsHoldTick->start(); // hold-progress feedback (stage 9)
                updateQuickShapeUi();
            }
            beginBrushStroke(pt, event->pressure(), event->xTilt(), event->yTilt(),
                             event->rotation(), event->timestamp());
        }
        break;
    case QEvent::TabletMove:
        if (m_quickShape.hasActiveShape()) {
            // Recognized: the held pen rotates/scales the corrected vector.
            m_quickShape.pointerMove(toCanvasF(event->position()),
                                     event->pressure());
        } else if (m_brushStroke) {
            // Stabilize before the snap (same order as the mouse path).
            const QPointF pt = m_perspective.snapPoint(stabilizeStrokePoint(
                toCanvasF(event->position()), false));
            if (m_quickShapeEnabled)
                m_quickShape.pointerMove(pt, event->pressure(),
                                         event->xTilt(), event->yTilt(),
                                         event->rotation());
            moveBrushStroke(pt, event->pressure(), event->xTilt(), event->yTilt(),
                            event->rotation(), event->timestamp());
        }
        break;
    case QEvent::TabletRelease:
        if (m_quickShape.hasActiveShape()) {
            // Keep the corrected vector as a temporary overlay until a
            // tap-away / lifecycle event bakes it.
            m_quickShape.pointerRelease(toCanvasF(event->position()),
                                        event->pressure());
        } else if (m_brushStroke) {
            if (m_quickShapeEnabled)
                m_quickShape.pointerRelease(toCanvasF(event->position()),
                                            event->pressure(),
                                            event->xTilt(), event->yTilt(),
                                            event->rotation());
            endBrushStroke();
        }
        m_qsHeld = false;
        updateQuickShapeUi();
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
