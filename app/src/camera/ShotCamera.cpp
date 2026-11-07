#include "camera/ShotCamera.h"

#include "StoryboardModel.h"

#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace shotcam {

CameraState neutral(const QSize &canvas)
{
    CameraState s;
    s.cx = canvas.width() / 2.0;
    s.cy = canvas.height() / 2.0;
    s.zoom = 1.0;
    s.rotation = 0.0;
    return s;
}

bool isNeutral(const CameraState &state, const QSize &canvas)
{
    return state == neutral(canvas);
}

bool takesOldPath(const CameraState &state, const Stage &stage)
{
    return !stage.hasMargin() && isNeutral(state, stage.canvas);
}

double ease(Ease e, double u)
{
    u = qBound(0.0, u, 1.0);
    switch (e) {
    case Ease::Linear:
        return u;
    case Ease::EaseIn:
        return u * u;
    case Ease::EaseOut:
        return 1.0 - (1.0 - u) * (1.0 - u);
    case Ease::EaseInOut:
        return u * u * (3.0 - 2.0 * u);
    case Ease::Hold:
        return u >= 1.0 ? 1.0 : 0.0;
    }
    return u;
}

QString easeName(Ease e)
{
    switch (e) {
    case Ease::Linear:
        return QStringLiteral("linear");
    case Ease::EaseIn:
        return QStringLiteral("easeIn");
    case Ease::EaseOut:
        return QStringLiteral("easeOut");
    case Ease::EaseInOut:
        return QStringLiteral("easeInOut");
    case Ease::Hold:
        return QStringLiteral("hold");
    }
    return QStringLiteral("easeInOut");
}

QString easeLabel(Ease e)
{
    switch (e) {
    case Ease::Linear: return QStringLiteral("Linear");
    case Ease::EaseIn: return QStringLiteral("Ease In");
    case Ease::EaseOut: return QStringLiteral("Ease Out");
    case Ease::EaseInOut: return QStringLiteral("Ease In/Out");
    case Ease::Hold: return QStringLiteral("Hold");
    }
    return QString();
}

Ease easeFromName(const QString &name, bool *known)
{
    const Ease all[] = {Ease::Linear, Ease::EaseIn, Ease::EaseOut,
                        Ease::EaseInOut, Ease::Hold};
    for (Ease e : all)
        if (name == easeName(e)) {
            if (known)
                *known = true;
            return e;
        }
    if (known)
        *known = false;
    return Ease::EaseInOut;
}

int frameInPanelFromMs(qint64 ms, int fps)
{
    // AnimaticTimeline::playheadFrame's expression, exactly.
    return static_cast<int>(ms / (1000.0 / qMax(1, fps)));
}

int msForFrameInPanel(int frame, int fps)
{
    // The first whole millisecond inside the frame: frameInPanelFromMs of
    // it gives the frame back (a frame is at least 16 ms long).
    return int(std::ceil(frame * (1000.0 / qMax(1, fps))));
}

int framesAtRate(int frames, int fromFps, int toFps)
{
    frames = qMax(1, frames);
    fromFps = qMax(1, fromFps);
    toFps = qMax(1, toFps);
    if (fromFps == toFps)
        return frames;
    // Integers only: frames * to / from, to the nearest frame, a half up.
    const qint64 twice = qint64(frames) * toFps * 2 + fromFps;
    return int(qMax<qint64>(1, twice / (2 * qint64(fromFps))));
}

int panelFrames(const Panel &panel, int fps)
{
    return framesAtRate(panel.frames, panel.rate, fps);
}

QString lengthText(int frames, int fps)
{
    fps = qMax(1, fps);
    frames = qMax(0, frames);
    const int s = frames / fps, f = frames % fps;
    if (s == 0)
        return QStringLiteral("%1f").arg(f);
    if (f == 0)
        return QStringLiteral("%1s").arg(s);
    return QStringLiteral("%1s %2f").arg(s).arg(f);
}

QString framesText(int frames)
{
    return QStringLiteral("%1f").arg(frames);
}

int panelStartFrame(const Scene &scene, int panelIndex, int fps)
{
    int frames = 0;
    for (int i = 0; i < panelIndex && i < scene.panels.size(); ++i)
        frames += panelFrames(*scene.panels.at(i), fps);
    return frames;
}

int panelIndexOfUid(const Scene &scene, quint64 uid)
{
    for (int i = 0; i < scene.panels.size(); ++i)
        if (scene.panels.at(i) && scene.panels.at(i)->uid.value() == uid)
            return i;
    return -1;
}

int sceneFrameCount(const Scene &scene, int fps)
{
    int frames = 0;
    for (const Panel *panel : scene.panels)
        if (panel)
            frames += panelFrames(*panel, fps);
    return frames;
}

double keyFilmFrame(const CameraKey &key, int fps)
{
    const double f = key.time * fps;
    const double whole = std::round(f);
    return qAbs(f - whole) < 1e-6 ? whole : f;
}

// Stranded (Phase 2E, D17): a pinned key outside the scene's span. The
// scene's end frame itself counts as inside, as an anchored key at a
// panel's end (at = 1) does.
bool pinnedOutside(const CameraKey &key, int fps, int filmStart, int frameCount)
{
    const double f = keyFilmFrame(key, fps) - filmStart;
    return f < 0.0 || f > frameCount;
}

double keyFrame(const Scene &scene, const CameraKey &key, int fps, int filmStart)
{
    if (key.pinned) {
        if (pinnedOutside(key, fps, filmStart, sceneFrameCount(scene, fps)))
            return -1.0;
        return keyFilmFrame(key, fps) - filmStart;
    }
    const int index = panelIndexOfUid(scene, key.panelUid);
    if (index < 0)
        return -1.0;
    return panelStartFrame(scene, index, fps)
        + key.at * panelFrames(*scene.panels.at(index), fps);
}

bool isStranded(const Scene &scene, const CameraKey &key, int fps, int filmStart)
{
    return key.pinned && keyFrame(scene, key, fps, filmStart) < 0.0;
}

int strandedCount(const Scene &scene, int fps, int filmStart)
{
    int n = 0;
    for (const CameraKey &key : scene.camera.keys)
        if (isStranded(scene, key, fps, filmStart))
            ++n;
    return n;
}

int liveKeyCount(const Scene &scene, int fps, int filmStart)
{
    int n = 0;
    for (const CameraKey &key : scene.camera.keys)
        if (keyFrame(scene, key, fps, filmStart) >= 0.0)
            ++n;
    return n;
}

CameraKey makePinnedKey(double filmFrame, int fps, const CameraState &state, Ease e)
{
    CameraKey key;
    key.pinned = true;
    key.time = filmFrame / qMax(1, fps);
    key.state = state;
    key.ease = e;
    return key;
}

QString strandingText(int strandedBefore, int strandedAfter, int sceneNumber)
{
    if (strandedAfter == strandedBefore)
        return QString();
    const int n = qAbs(strandedAfter - strandedBefore);
    const QString keys = n == 1 ? QStringLiteral("1 pinned camera key")
                                : QStringLiteral("%1 pinned camera keys").arg(n);
    return strandedAfter > strandedBefore
        ? QStringLiteral(" (%1 now outside scene %2)").arg(keys).arg(sceneNumber)
        : QStringLiteral(" (%1 back inside scene %2)").arg(keys).arg(sceneNumber);
}

int liveKeysOnPanel(const Scene &scene, const Panel *panel)
{
    if (!panel || panelIndexOfUid(scene, panel->uid.value()) < 0)
        return 0;
    int n = 0;
    for (const CameraKey &key : scene.camera.keys)
        if (key.panelUid == panel->uid.value())
            ++n;
    return n;
}

CameraKey makeKey(const Scene &scene, int panelIndex, int frameInPanel,
                  int fps, const CameraState &state, Ease e)
{
    CameraKey key;
    key.state = state;
    key.ease = e;
    if (panelIndex < 0 || panelIndex >= scene.panels.size())
        return key;
    const Panel *panel = scene.panels.at(panelIndex);
    const int frames = panelFrames(*panel, fps);
    key.panelUid = panel->uid.value();
    key.at = qBound(0.0, double(qBound(0, frameInPanel, frames)) / frames, 1.0);
    return key;
}

CameraState evaluate(const Scene &scene, double sceneFrame, int fps,
                     const QSize &canvas, int filmStart)
{
    struct Live
    {
        double frame;
        int order;
        const CameraKey *key;
    };
    QVector<Live> live;
    live.reserve(scene.camera.keys.size());
    for (int i = 0; i < scene.camera.keys.size(); ++i) {
        const double f = keyFrame(scene, scene.camera.keys.at(i), fps, filmStart);
        if (f >= 0.0)
            live.append({f, i, &scene.camera.keys.at(i)});
    }
    if (live.isEmpty())
        return neutral(canvas);
    // By frame; at one frame the anchored key first, the pinned after (Phase
    // 2E: they can only meet through a retime, and the pinned one's framing
    // shows from that frame).
    std::stable_sort(live.begin(), live.end(), [](const Live &a, const Live &b) {
        if (a.frame != b.frame)
            return a.frame < b.frame;
        return !a.key->pinned && b.key->pinned;
    });
    if (sceneFrame <= live.first().frame) {
        // Before the first key, its framing - and AT the first frame, the
        // last key on that frame: a pinned key that met the anchored one
        // there sorts after it (above) and shows from that frame. One key
        // on the frame, the path every project took before 2E.
        int j = 0;
        while (j + 1 < live.size() && live.at(j + 1).frame == live.first().frame)
            ++j;
        return live.at(j).key->state;
    }
    if (sceneFrame >= live.last().frame)
        return live.last().key->state;
    int i = live.size() - 2;
    while (i > 0 && live.at(i).frame > sceneFrame)
        --i;
    const Live &a = live.at(i);
    const Live &b = live.at(i + 1);
    const double span = b.frame - a.frame;
    if (span <= 0.0)
        return b.key->state;
    const double w = ease(a.key->ease, (sceneFrame - a.frame) / span);
    const CameraState &sa = a.key->state;
    const CameraState &sb = b.key->state;
    CameraState s;
    s.cx = sa.cx + (sb.cx - sa.cx) * w;
    s.cy = sa.cy + (sb.cy - sa.cy) * w;
    s.rotation = sa.rotation + (sb.rotation - sa.rotation) * w;
    s.zoom = sa.zoom == sb.zoom
        ? sa.zoom
        : std::exp(std::log(sa.zoom) + (std::log(sb.zoom) - std::log(sa.zoom)) * w);
    return s;
}

CameraState evaluateAtPanel(const Scene &scene, int panelIndex,
                            int frameInPanel, int fps, int filmStart)
{
    if (panelIndex < 0 || panelIndex >= scene.panels.size()
        || !scene.panels.at(panelIndex))
        return CameraState();
    const QSize canvas = scene.panels.at(panelIndex)->canvasSize();
    if (scene.camera.isEmpty())
        return neutral(canvas);
    return evaluate(scene, panelStartFrame(scene, panelIndex, fps) + frameInPanel,
                    fps, canvas, filmStart);
}

QPolygonF frameQuad(const CameraState &state, const Stage &stage)
{
    const double hw = stage.frame.width() / state.zoom / 2.0;
    const double hh = stage.frame.height() / state.zoom / 2.0;
    QTransform t;
    t.translate(state.cx, state.cy);
    t.rotate(state.rotation);
    return t.map(QPolygonF({QPointF(-hw, -hh), QPointF(hw, -hh),
                            QPointF(hw, hh), QPointF(-hw, hh)}));
}

QTransform shotTransform(const CameraState &state, const Stage &stage,
                         const QRectF &target, bool cover)
{
    const double fw = stage.frame.width() / state.zoom;
    const double fh = stage.frame.height() / state.zoom;
    const double sx = target.width() / fw;
    const double sy = target.height() / fh;
    const double s = cover ? qMax(sx, sy) : qMin(sx, sy);
    QTransform t; // applied last-call-first: p -> centre + s * R(-r) * (p - c)
    t.translate(target.center().x(), target.center().y());
    t.scale(s, s);
    t.rotate(-state.rotation);
    t.translate(-state.cx, -state.cy);
    return t;
}

QRect shotFrameRect(const Stage &stage, const QRect &target)
{
    if (stage.frame.isEmpty() || target.isEmpty())
        return QRect();
    const double s = qMin(target.width() / double(stage.frame.width()),
                          target.height() / double(stage.frame.height()));
    const double w = stage.frame.width() * s, h = stage.frame.height() * s;
    const QPointF c = QRectF(target).center();
    // The pixels whose centres lie inside the frame: never one past its edge.
    return QRect(QPoint(qCeil(c.x() - w / 2.0 - 0.5), qCeil(c.y() - h / 2.0 - 0.5)),
                 QPoint(qFloor(c.x() + w / 2.0 - 0.5), qFloor(c.y() + h / 2.0 - 0.5)))
        & target;
}

QImage renderShot(const QImage &flat, const CameraState &state,
                  const QSize &out)
{
    return renderShot(flat, state, Stage(flat.size()), out);
}

QImage renderShot(const QImage &flat, const CameraState &state, const Stage &stage,
                  const QSize &out)
{
    QImage frame(out, QImage::Format_RGBX8888);
    frame.fill(Qt::black);
    if (flat.isNull() || out.isEmpty())
        return frame;
    const QTransform toOut = shotTransform(state, stage, QRectF(QPointF(0, 0), out));
    // Only the artwork the frame can see: the quad's bounds, in source px.
    const QRect src =
        frameQuad(state, stage).boundingRect().adjusted(-2, -2, 2, 2).toAlignedRect()
        & flat.rect();
    if (src.isEmpty())
        return frame;
    const double scale = qMin(out.width() / (stage.frame.width() / state.zoom),
                              out.height() / (stage.frame.height() / state.zoom));
    QImage piece = flat.copy(src);
    if (scale < 1.0) {
        // Area-average down to the output scale first (the export's own
        // filter, renderMovieFrame): the bilinear placement below then
        // works near 1:1 and does not thin fine lines.
        piece = piece.scaled(qMax(1, qRound(src.width() * scale)),
                             qMax(1, qRound(src.height() * scale)),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QTransform pieceToCanvas;
    pieceToCanvas.translate(src.x(), src.y());
    pieceToCanvas.scale(src.width() / double(piece.width()),
                        src.height() / double(piece.height()));
    QPainter p(&frame);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setTransform(pieceToCanvas * toOut);
    p.drawImage(0, 0, piece);
    p.end();
    return frame;
}

void drawShot(QPainter &painter, const QPixmap &picture, const Stage &stage,
              const CameraState &state, const QRectF &target, bool cover)
{
    if (picture.isNull() || stage.canvas.isEmpty())
        return;
    QTransform pictureToCanvas;
    pictureToCanvas.scale(stage.canvas.width() / double(picture.width()),
                          stage.canvas.height() / double(picture.height()));
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setTransform(pictureToCanvas * shotTransform(state, stage, target, cover),
                         true);
    painter.drawPixmap(0, 0, picture);
    painter.restore();
}

double sharpLimit(const Stage &stage, const QSize &out)
{
    if (out.isEmpty())
        return 1.0;
    return qMax(stage.frame.width() / double(out.width()),
                stage.frame.height() / double(out.height()));
}

// --- Depth (Phase 2B) ----------------------------------------------------------

double depthDivisor(double a, const CameraState &state)
{
    return state.zoom * (1.0 - a) + a;
}

QTransform layerToStage(double a, const CameraState &state, const Stage &stage)
{
    // q = k p + (c + a (c0 - c) k - c0 k), k = 1 / d.
    const double d = depthDivisor(a, state);
    const double k = d > 0.0 ? 1.0 / d : 0.0;
    const double c0x = stage.canvas.width() / 2.0, c0y = stage.canvas.height() / 2.0;
    return QTransform(k, 0.0, 0.0, k, state.cx + a * (c0x - state.cx) * k - c0x * k,
                      state.cy + a * (c0y - state.cy) * k - c0y * k);
}

bool layerShown(double a, const CameraState &state)
{
    return depthDivisor(a, state) >= kNearCut;
}

bool offBaseFraming(const CameraState &state, const Stage &stage)
{
    return state.zoom != 1.0 || state.cx != stage.canvas.width() / 2.0
        || state.cy != stage.canvas.height() / 2.0;
}

bool layeredShot(const Panel &panel, const CameraState &state, const Stage &stage)
{
    return offBaseFraming(state, stage) && panel.hasDepth();
}

namespace {
// THE MIP CACHE: a layer reduced to a quarter-octave level, by the image's
// cacheKey (which changes on every edit) and the level. Shared by every
// consumer and thread; the oldest entries go past 256 MB.
struct MipEntry
{
    QImage image;
    quint64 used = 0;
};
QMutex g_depthMutex;
QHash<QPair<qint64, int>, MipEntry> g_mips;
qint64 g_mipBytes = 0;
quint64 g_mipClock = 0;
constexpr qint64 kMipBudget = 256ll * 1024 * 1024;

// `image` reduced for drawing at `scale` (target px per layer px): itself at
// 1 and above, else area-averaged to 2^(-n/4) >= scale.
QImage layerMip(const QImage &image, double scale)
{
    if (scale >= 1.0 || image.isNull())
        return image;
    const int n = qMax(0, int(std::floor(-std::log2(qMax(scale, 1e-6)) * 4.0)));
    if (n == 0)
        return image;
    const QPair<qint64, int> key(image.cacheKey(), n);
    {
        QMutexLocker lock(&g_depthMutex);
        auto it = g_mips.find(key);
        if (it != g_mips.end()) {
            it->used = ++g_mipClock;
            return it->image;
        }
    }
    const double level = std::pow(2.0, -n / 4.0);
    const QImage mip = image.scaled(qMax(1, qRound(image.width() * level)),
                                    qMax(1, qRound(image.height() * level)),
                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QMutexLocker lock(&g_depthMutex);
    if (!g_mips.contains(key)) {
        g_mips.insert(key, MipEntry{mip, ++g_mipClock});
        g_mipBytes += mip.sizeInBytes();
        while (g_mipBytes > kMipBudget && g_mips.size() > 1) {
            auto oldest = g_mips.begin();
            for (auto it = g_mips.begin(); it != g_mips.end(); ++it)
                if (it->used < oldest->used)
                    oldest = it;
            g_mipBytes -= oldest->image.sizeInBytes();
            g_mips.erase(oldest);
        }
    }
    return mip;
}

// THE LAYERED PAINT, every depth consumer's: the paper on the picture
// plane's stage, then each drawn layer through layerToStage and
// `canvasToTarget` (stage px -> the painter's own coordinates), only what
// falls inside `visible` (the painter's coordinates).
void paintLayers(QPainter &painter, const Panel &panel, const CameraState &state,
                 const Stage &stage, const QTransform &canvasToTarget, const QRectF &visible)
{
    painter.save();
    const QTransform base = painter.transform();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setTransform(canvasToTarget * base);
    painter.fillRect(QRectF(QPointF(0, 0), QSizeF(stage.canvas)), Qt::white);
    for (const Layer &layer : panel.layers) {
        if (!panel.layerDrawn(layer))
            continue;
        const double a = panel.effectiveParallax(layer);
        if (!layerShown(a, state))
            continue;
        const QTransform toTarget = layerToStage(a, state, stage) * canvasToTarget;
        const double scale = std::sqrt(qAbs(toTarget.determinant()));
        const QImage mip = layerMip(layer.image, scale);
        if (mip.isNull())
            continue;
        const QTransform mipToTarget =
            QTransform::fromScale(layer.image.width() / double(mip.width()),
                                  layer.image.height() / double(mip.height()))
            * toTarget;
        const QRect src = mipToTarget.inverted()
                              .mapRect(visible)
                              .adjusted(-2, -2, 2, 2)
                              .toAlignedRect()
            & mip.rect();
        if (src.isEmpty())
            continue;
        painter.setTransform(mipToTarget * base);
        painter.setOpacity(panel.layerEffectiveOpacity(layer));
        painter.drawImage(QPointF(src.topLeft()), mip, src);
    }
    painter.restore();
}

// Everything a cached layered picture depends on.
QVector<quint64> depthKey(const Panel &panel, const CameraState &state, const Stage &stage,
                          const QSize &size, int kind)
{
    QVector<quint64> key = panel.flattenFingerprint();
    auto bits = [](double v) {
        quint64 b = 0;
        memcpy(&b, &v, sizeof(b));
        return b;
    };
    for (const Layer &layer : panel.layers)
        key.append(bits(panel.effectiveParallax(layer)));
    key << bits(state.cx) << bits(state.cy) << bits(state.zoom) << bits(state.rotation)
        << quint64(stage.canvas.width()) << quint64(stage.canvas.height())
        << quint64(stage.frame.width()) << quint64(stage.frame.height())
        << quint64(size.width()) << quint64(size.height()) << quint64(kind)
        << panel.uid.value();
    return key;
}

struct ThumbEntry
{
    QImage image;
    quint64 used = 0;
};
QHash<QVector<quint64>, ThumbEntry> g_thumbs;
constexpr int kThumbBudget = 160; // entries: a scene's clips and strip

QImage cachedThumb(const QVector<quint64> &key, const std::function<QImage()> &make)
{
    {
        QMutexLocker lock(&g_depthMutex);
        auto it = g_thumbs.find(key);
        if (it != g_thumbs.end()) {
            it->used = ++g_mipClock;
            return it->image;
        }
    }
    const QImage image = make();
    QMutexLocker lock(&g_depthMutex);
    g_thumbs.insert(key, ThumbEntry{image, ++g_mipClock});
    while (g_thumbs.size() > kThumbBudget) {
        auto oldest = g_thumbs.begin();
        for (auto it = g_thumbs.begin(); it != g_thumbs.end(); ++it)
            if (it->used < oldest->used)
                oldest = it;
        g_thumbs.erase(oldest);
    }
    return image;
}
} // namespace

QImage renderShot(const Panel &panel, const CameraState &state, const Stage &stage,
                  const QSize &out)
{
    QImage frame(out, QImage::Format_RGBX8888);
    frame.fill(Qt::black);
    if (out.isEmpty() || stage.canvas.isEmpty())
        return frame;
    const QRectF target(QPointF(0, 0), out);
    QPainter p(&frame);
    paintLayers(p, panel, state, stage, shotTransform(state, stage, target), target);
    p.end();
    return frame;
}

void drawShot(QPainter &painter, const Panel &panel, const Stage &stage,
              const CameraState &state, const QRectF &target, bool cover)
{
    if (stage.canvas.isEmpty())
        return;
    QRectF visible = target;
    if (painter.hasClipping())
        visible &= painter.clipBoundingRect();
    paintLayers(painter, panel, state, stage, shotTransform(state, stage, target, cover),
                visible);
}

QImage stageImage(const Panel &panel, const CameraState &state, const Stage &stage,
                  const QSize &target)
{
    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    if (target.isEmpty() || stage.canvas.isEmpty())
        return image;
    QPainter p(&image);
    paintLayers(p, panel, state, stage,
                QTransform::fromScale(target.width() / double(stage.canvas.width()),
                                      target.height() / double(stage.canvas.height())),
                QRectF(QPointF(0, 0), target));
    p.end();
    return image;
}

void drawStage(QPainter &painter, const Panel &panel, const CameraState &state,
               const Stage &stage, const QRectF &visible)
{
    if (stage.canvas.isEmpty())
        return;
    paintLayers(painter, panel, state, stage, QTransform(), visible);
}

QImage shotThumb(const Panel &panel, const CameraState &state, const Stage &stage,
                 const QSize &size, bool cover)
{
    return cachedThumb(depthKey(panel, state, stage, size, cover ? 1 : 0), [&] {
        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        QPainter p(&image);
        drawShot(p, panel, stage, state, QRectF(QPointF(0, 0), size), cover);
        p.end();
        return image;
    });
}

QImage stageThumb(const Panel &panel, const CameraState &state, const Stage &stage,
                  const QSize &size)
{
    return cachedThumb(depthKey(panel, state, stage, size, 2),
                       [&] { return stageImage(panel, state, stage, size); });
}

int layerMipCountForTest()
{
    QMutexLocker lock(&g_depthMutex);
    return g_mips.size();
}

void clearDepthCachesForTest()
{
    QMutexLocker lock(&g_depthMutex);
    g_mips.clear();
    g_mipBytes = 0;
    g_thumbs.clear();
}

CameraState clampZoom(const CameraState &state)
{
    CameraState s = state;
    s.zoom = qBound(kMinZoom, s.zoom, kMaxZoom);
    return s;
}

double insideMinZoom(double rotation, const Stage &stage)
{
    if (stage.canvas.isEmpty())
        return 1.0;
    const double r = qDegreesToRadians(rotation);
    const double c = qAbs(std::cos(r)), s = qAbs(std::sin(r));
    const double W = stage.canvas.width(), H = stage.canvas.height();
    const double FW = stage.frame.width(), FH = stage.frame.height();
    // The turned frame's bounding box (fw c + fh s) x (fw s + fh c), with
    // fw = FW / z and fh = FH / z, must fit W x H.
    return qMax((FW * c + FH * s) / W, (FW * s + FH * c) / H);
}

CameraState clampInside(const CameraState &state, const Stage &stage)
{
    CameraState s = clampZoom(state);
    const QSize &canvas = stage.canvas;
    if (canvas.isEmpty())
        return s;
    s.zoom = qMin(kMaxZoom, qMax(s.zoom, insideMinZoom(s.rotation, stage)));
    const double r = qDegreesToRadians(s.rotation);
    const double c = qAbs(std::cos(r)), sn = qAbs(std::sin(r));
    const double fw = stage.frame.width() / s.zoom, fh = stage.frame.height() / s.zoom;
    const double bw = fw * c + fh * sn, bh = fw * sn + fh * c;
    // qBound's own order, written out: at exactly insideMinZoom the box is
    // the canvas give or take a rounding, so the two bounds can cross by
    // ~1e-13 - Debug's qBound asserts on that, Release takes the lower one.
    s.cx = qMax(bw / 2.0, qMin(canvas.width() - bw / 2.0, s.cx));
    s.cy = qMax(bh / 2.0, qMin(canvas.height() - bh / 2.0, s.cy));
    return s;
}

// --- Stay inside with depth (Phase 2C) ------------------------------------------

namespace {
// The centres the frame may have at this zoom and turn: the stage's (the
// paper's) and each edge-reaching depth layer's bounds, with the index of
// the layer that sets each side (-1: the stage). False: none fits.
struct CentreRange
{
    double lo = 0.0, hi = 0.0;
    int loBy = -1, hiBy = -1;
};
bool depthRanges(double zoom, double rotation, const Stage &stage,
                 const QVector<DepthEdge> &edges, CentreRange *x, CentreRange *y)
{
    const double r = qDegreesToRadians(rotation);
    const double c = qAbs(std::cos(r)), sn = qAbs(std::sin(r));
    const double fw = stage.frame.width() / zoom, fh = stage.frame.height() / zoom;
    const double bw = fw * c + fh * sn, bh = fw * sn + fh * c;
    const double W = stage.canvas.width(), H = stage.canvas.height();
    const double c0x = W / 2.0, c0y = H / 2.0;
    *x = CentreRange{bw / 2.0, W - bw / 2.0, -1, -1};
    *y = CentreRange{bh / 2.0, H - bh / 2.0, -1, -1};
    for (int i = 0; i < edges.size(); ++i) {
        const DepthEdge &e = edges.at(i);
        const double a = e.parallax;
        const double d = zoom * (1.0 - a) + a;
        if (a <= 0.0 || d < kNearCut)
            continue; // fixed to the camera, or passed: no bound
        // The footprint: centre c0 + a (c - c0), half extents d * box / 2.
        const double hx = d * bw / 2.0, hy = d * bh / 2.0;
        if (e.left) {
            const double lo = c0x - (c0x - hx) / a;
            if (lo > x->lo) { x->lo = lo; x->loBy = i; }
        }
        if (e.right) {
            const double hi = c0x + (W - c0x - hx) / a;
            if (hi < x->hi) { x->hi = hi; x->hiBy = i; }
        }
        if (e.top) {
            const double lo = c0y - (c0y - hy) / a;
            if (lo > y->lo) { y->lo = lo; y->loBy = i; }
        }
        if (e.bottom) {
            const double hi = c0y + (H - c0y - hy) / a;
            if (hi < y->hi) { y->hi = hi; y->hiBy = i; }
        }
    }
    return x->lo <= x->hi + 1e-9 && y->lo <= y->hi + 1e-9;
}

// THE EDGE CACHE: which edges a layer's art reaches, by its cacheKey (which
// changes on every edit). Bits: 1 left, 2 top, 4 right, 8 bottom.
QHash<qint64, quint8> g_edgeBits;

quint8 edgeBitsOf(const QImage &image)
{
    {
        QMutexLocker lock(&g_depthMutex);
        auto it = g_edgeBits.constFind(image.cacheKey());
        if (it != g_edgeBits.constEnd())
            return it.value();
    }
    constexpr int kBand = 2;        // px from the edge
    constexpr int kVisible = 26;    // alpha: 10 % (the user's Q6)
    const QImage img = (image.format() == QImage::Format_ARGB32_Premultiplied
                        || image.format() == QImage::Format_ARGB32)
        ? image
        : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = img.width(), h = img.height();
    quint8 bits = 0;
    auto visible = [&](int x, int y) {
        return qAlpha(reinterpret_cast<const QRgb *>(img.constScanLine(y))[x]) >= kVisible;
    };
    for (int y = 0; y < h && !(bits & 2 && bits & 8); ++y) {
        if (y >= kBand && y < h - kBand)
            y = h - kBand; // only the top and bottom bands
        for (int x = 0; x < w; ++x)
            if (visible(x, y)) {
                bits |= y < kBand ? 2 : 8;
                break;
            }
    }
    for (int y = 0; y < h && !(bits & 1 && bits & 4); ++y) {
        for (int x = 0; x < qMin(kBand, w); ++x)
            if (visible(x, y))
                bits |= 1;
        for (int x = qMax(0, w - kBand); x < w; ++x)
            if (visible(x, y))
                bits |= 4;
    }
    QMutexLocker lock(&g_depthMutex);
    if (g_edgeBits.size() > 1024)
        g_edgeBits.clear(); // keys of images long since edited
    g_edgeBits.insert(image.cacheKey(), bits);
    return bits;
}
} // namespace

QVector<DepthEdge> depthEdges(const Panel &panel)
{
    QVector<DepthEdge> out;
    for (const Layer &layer : panel.layers) {
        if (!panel.layerDrawn(layer))
            continue;
        const double a = panel.effectiveParallax(layer);
        if (a <= 0.0 || a == 1.0)
            continue;
        const quint8 bits = edgeBitsOf(layer.image);
        if (!bits)
            continue;
        DepthEdge e;
        e.parallax = a;
        e.left = bits & 1;
        e.top = bits & 2;
        e.right = bits & 4;
        e.bottom = bits & 8;
        e.name = layer.name;
        out.append(e);
    }
    return out;
}

CameraState clampInside(const CameraState &state, const Stage &stage,
                        const QVector<DepthEdge> &edges, int *stoppedBy)
{
    if (stoppedBy)
        *stoppedBy = -1;
    CameraState s = clampInside(state, stage);
    if (edges.isEmpty() || stage.canvas.isEmpty())
        return s;
    CentreRange x, y;
    if (!depthRanges(s.zoom, s.rotation, stage, edges, &x, &y)) {
        // No centre fits at this zoom: pushed in by Phase 1D's search.
        const int by = x.lo > x.hi ? (x.loBy >= 0 ? x.loBy : x.hiBy)
                                   : (y.loBy >= 0 ? y.loBy : y.hiBy);
        double lo = s.zoom, hi = s.zoom;
        bool found = false;
        CentreRange tx, ty;
        while (hi < kMaxZoom) {
            hi = qMin(kMaxZoom, hi * 1.05);
            if (depthRanges(hi, s.rotation, stage, edges, &tx, &ty)) {
                found = true;
                break;
            }
            lo = hi;
        }
        if (!found)
            return s; // at the zoom's limit: the stage's clamp stands
        for (int i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2.0;
            if (depthRanges(mid, s.rotation, stage, edges, &tx, &ty))
                hi = mid;
            else
                lo = mid;
        }
        s.zoom = hi;
        depthRanges(s.zoom, s.rotation, stage, edges, &x, &y);
        if (stoppedBy)
            *stoppedBy = by;
    }
    // qBound's order written out, as clampInside: at a pushed-in zoom the
    // two bounds can cross by a rounding.
    const double cx = qMax(x.lo, qMin(x.hi, s.cx));
    const double cy = qMax(y.lo, qMin(y.hi, s.cy));
    if (stoppedBy && *stoppedBy < 0) {
        if (cx != s.cx)
            *stoppedBy = cx == x.lo ? x.loBy : x.hiBy;
        if (*stoppedBy < 0 && cy != s.cy)
            *stoppedBy = cy == y.lo ? y.loBy : y.hiBy;
    }
    s.cx = cx;
    s.cy = cy;
    return s;
}

QStringList passedLayers(const Panel &panel, const CameraState &state)
{
    QStringList out;
    for (const Layer &layer : panel.layers)
        if (panel.layerDrawn(layer) && !layerShown(panel.effectiveParallax(layer), state))
            out << layer.name;
    return out;
}

QString passedText(const QStringList &names)
{
    const QChar l(0x2018), r(0x2019);
    if (names.isEmpty())
        return QString();
    if (names.size() == 1)
        return QStringLiteral("Passed ") + l + names.at(0) + r;
    if (names.size() == 2)
        return QStringLiteral("Passed ") + l + names.at(0) + r + QStringLiteral(", ") + l
            + names.at(1) + r;
    return QStringLiteral("Passed %1 layers").arg(names.size());
}

int depthEdgeCacheCountForTest()
{
    QMutexLocker lock(&g_depthMutex);
    return g_edgeBits.size();
}

double normalisedAngle(double degrees)
{
    double a = std::fmod(degrees, 360.0);
    if (a <= -180.0)
        a += 360.0;
    else if (a > 180.0)
        a -= 360.0;
    return a;
}

int keyIndexAtFrame(const Scene &scene, double sceneFrame, int fps, int filmStart)
{
    // At one frame the anchored key is the one (the pinned sorts after it).
    int found = -1;
    for (int i = 0; i < scene.camera.keys.size(); ++i) {
        const double f = keyFrame(scene, scene.camera.keys.at(i), fps, filmStart);
        if (f >= 0.0 && qAbs(f - sceneFrame) < 0.5) {
            if (!scene.camera.keys.at(i).pinned)
                return i;
            if (found < 0)
                found = i;
        }
    }
    return found;
}

int segmentKeyIndex(const Scene &scene, double sceneFrame, int fps, int filmStart)
{
    // The key whose segment the frame is in: the last live key at or before
    // it (within half a frame counts as at it). Two keys on one frame (Phase
    // 2E, a pinned key met by a retime): the pinned one, which sorts after
    // the anchored and starts the move from there.
    int best = -1;
    double bestFrame = -1.0;
    for (int i = 0; i < scene.camera.keys.size(); ++i) {
        const double f = keyFrame(scene, scene.camera.keys.at(i), fps, filmStart);
        if (f < 0.0 || f >= sceneFrame + 0.5)
            continue;
        const bool later = best < 0 || f > bestFrame + 0.5;
        const bool same = best >= 0 && qAbs(f - bestFrame) < 0.5;
        if (later || (same && scene.camera.keys.at(i).pinned && !scene.camera.keys.at(best).pinned)) {
            best = i;
            bestFrame = f;
        }
    }
    return best;
}

Motion motionBetween(const CameraState &from, const CameraState &to, const Stage &stage)
{
    Motion m;
    if (from.zoom > 0.0 && to.zoom > 0.0) {
        const double ratio = to.zoom / from.zoom;
        if (ratio > 1.01)
            m.zoom = 1;
        else if (ratio < 1.0 / 1.01)
            m.zoom = -1;
    }
    const QPointF travel(to.cx - from.cx, to.cy - from.cy);
    if (std::hypot(travel.x(), travel.y()) > 0.02 * stage.frame.width())
        m.pan = travel;
    const double turn = normalisedAngle(to.rotation - from.rotation);
    if (turn > 0.5)
        m.rotate = 1;
    else if (turn < -0.5)
        m.rotate = -1;
    return m;
}

Motion segmentMotion(const Scene &scene, double sceneFrame, int fps, const Stage &stage,
                     int filmStart)
{
    // The live keys in evaluate()'s order: by frame, and at one frame the
    // anchored key before the pinned one.
    struct Live
    {
        double frame;
        const CameraKey *key;
    };
    QVector<Live> live;
    for (const CameraKey &key : scene.camera.keys) {
        const double f = keyFrame(scene, key, fps, filmStart);
        if (f >= 0.0)
            live.append({f, &key});
    }
    std::stable_sort(live.begin(), live.end(), [](const Live &a, const Live &b) {
        if (a.frame != b.frame)
            return a.frame < b.frame;
        return !a.key->pinned && b.key->pinned;
    });
    // The segment: the last live key at or before the frame (within half a
    // frame counts as at it, as segmentKeyIndex has it), and the next one.
    int at = -1;
    for (int i = 0; i < live.size(); ++i)
        if (live.at(i).frame < sceneFrame + 0.5)
            at = i;
    if (at < 0 || at + 1 >= live.size() || live.at(at).key->ease == Ease::Hold)
        return Motion();
    return motionBetween(live.at(at).key->state, live.at(at + 1).key->state, stage);
}

QVector<double> liveKeyFrames(const Scene &scene, int fps, int filmStart)
{
    QVector<double> frames;
    for (const CameraKey &key : scene.camera.keys) {
        const double f = keyFrame(scene, key, fps, filmStart);
        if (f >= 0.0)
            frames.append(f);
    }
    std::sort(frames.begin(), frames.end());
    return frames;
}

QPainterPath motionGlyphPath(const QPolygonF &quad, const Motion &motion,
                             const QPointF &screenPanDir, double scale)
{
    QPainterPath path;
    if (quad.size() < 4 || !motion.any() || !(scale > 0.0))
        return path;
    // Built in the frame's own axes - x along its top edge, y down its left
    // edge, the origin at its first corner - and mapped onto the quad, so a
    // turned or mirrored frame carries its glyphs with it.
    const double w = QLineF(quad.at(0), quad.at(1)).length();
    const double h = QLineF(quad.at(0), quad.at(3)).length();
    if (w < 1e-6 || h < 1e-6)
        return path;
    const QPointF ex = (quad.at(1) - quad.at(0)) / w, ey = (quad.at(3) - quad.at(0)) / h;
    const QTransform toQuad(ex.x(), ex.y(), ey.x(), ey.y(), quad.at(0).x(), quad.at(0).y());
    const double s = scale;
    QPainterPath local;
    auto head = [&](const QPointF &tip, const QPointF &u) { // u: the unit way it points
        const QPointF n(-u.y(), u.x());
        const QPointF back = tip - u * (5.16 * s); // 6.3 px barbs at 35 degrees
        local.moveTo(tip);
        local.lineTo(back + n * (3.61 * s));
        local.moveTo(tip);
        local.lineTo(back - n * (3.61 * s));
    };
    auto arrow = [&](const QPointF &from, const QPointF &to) {
        const QPointF d = to - from;
        const double l = std::hypot(d.x(), d.y());
        if (l < 1e-9)
            return;
        local.moveTo(from);
        local.lineTo(to);
        head(to, d / l);
    };
    const QPointF corner[4] = {QPointF(0, 0), QPointF(w, 0), QPointF(w, h), QPointF(0, h)};
    const QPointF inward[4] = {QPointF(1, 1), QPointF(-1, 1), QPointF(-1, -1), QPointF(1, -1)};
    if (motion.zoom != 0) {
        for (int i = 0; i < 4; ++i) {
            const QPointF a = corner[i] + inward[i] * (6.0 * s);         // 6 px in from both edges
            const QPointF b = corner[i] + inward[i] * ((6.0 + 9.9) * s); // 14 px on the 45 degree line
            if (motion.zoom > 0)
                arrow(a, b); // Push In: pointing in
            else
                arrow(b, a); // Pull Out: pointing at the corner
        }
    }
    const double travel = std::hypot(screenPanDir.x(), screenPanDir.y());
    if (!motion.pan.isNull() && travel > 1e-9) {
        // The way the frame travels, in its own axes.
        const QPointF u(QPointF::dotProduct(screenPanDir, ex) / travel,
                        QPointF::dotProduct(screenPanDir, ey) / travel);
        const double slope = std::tan(qDegreesToRadians(22.5));
        auto chevron = [&](const QPointF &mid, const QPointF &t) {
            const QPointF n(-t.y(), t.x());
            local.moveTo(mid + t * (3.0 * s) + n * (4.0 * s));
            local.lineTo(mid + t * (8.0 * s));
            local.lineTo(mid + t * (3.0 * s) - n * (4.0 * s));
        };
        if (qAbs(u.y()) <= qAbs(u.x()) * slope) { // a pan: on the top and the bottom edge
            const QPointF t(u.x() > 0.0 ? 1.0 : -1.0, 0.0);
            chevron(QPointF(w / 2.0, 0.0), t);
            chevron(QPointF(w / 2.0, h), t);
        } else if (qAbs(u.x()) <= qAbs(u.y()) * slope) { // a tilt: on the two sides
            const QPointF t(0.0, u.y() > 0.0 ? 1.0 : -1.0);
            chevron(QPointF(0.0, h / 2.0), t);
            chevron(QPointF(w, h / 2.0), t);
        } else { // diagonal: an arrow behind each corner, along the travel
            for (const QPointF &c : corner) {
                const QPointF tip = c - u * (7.0 * s);
                arrow(tip - u * (15.0 * s), tip);
            }
        }
    }
    if (motion.rotate != 0) {
        // Over the top edge, outside the frame: clockwise runs from the
        // left end over to the right one and turns down at it.
        const double r = 12.0 * s;
        const QPointF centre(w / 2.0, -3.6 * s);
        const QRectF box(centre.x() - r, centre.y() - r, 2.0 * r, 2.0 * r);
        if (motion.rotate > 0) {
            local.moveTo(centre.x() - r, centre.y());
            local.arcTo(box, 180.0, -180.0);
            head(QPointF(centre.x() + r, centre.y()), QPointF(0.0, 1.0));
        } else {
            local.moveTo(centre.x() + r, centre.y());
            local.arcTo(box, 0.0, 180.0);
            head(QPointF(centre.x() - r, centre.y()), QPointF(0.0, 1.0));
        }
    }
    return toQuad.map(local);
}

QString timecodeText(int filmFrame, int fps)
{
    fps = qMax(1, fps);
    filmFrame = qMax(0, filmFrame);
    const int ff = filmFrame % fps;
    const int totalSec = filmFrame / fps;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(totalSec / 3600, 2, 10, QChar('0'))
        .arg((totalSec / 60) % 60, 2, 10, QChar('0'))
        .arg(totalSec % 60, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

namespace {

QJsonObject keysJson(const Scene &scene, const QPointF &frameOrigin)
{
    QJsonArray keys;
    for (const CameraKey &key : scene.camera.keys) {
        QJsonObject k;
        if (key.pinned) { // Phase 2E: its time, stranded or not
            k[QStringLiteral("pinned")] = true;
            k[QStringLiteral("time")] = key.time;
        } else {
            const int index = panelIndexOfUid(scene, key.panelUid);
            if (index < 0)
                continue; // inert: its panel is deleted
            k[QStringLiteral("panel")] = index;
            k[QStringLiteral("at")] = key.at;
        }
        k[QStringLiteral("x")] = key.state.cx - frameOrigin.x();
        k[QStringLiteral("y")] = key.state.cy - frameOrigin.y();
        k[QStringLiteral("zoom")] = key.state.zoom;
        k[QStringLiteral("rotation")] = key.state.rotation;
        k[QStringLiteral("ease")] = easeName(key.ease);
        keys.append(k);
    }
    if (keys.isEmpty())
        return QJsonObject();
    QJsonObject root;
    root[QStringLiteral("keys")] = keys;
    return root;
}

} // namespace

QJsonObject toJson(const Scene &scene)
{
    return keysJson(scene, QPointF());
}

QJsonObject toFrameSpaceJson(const Scene &scene, const QPointF &frameOrigin)
{
    return keysJson(scene, frameOrigin);
}

void keysToStage(Scene &scene, const QPointF &frameOrigin)
{
    for (CameraKey &key : scene.camera.keys) {
        key.state.cx += frameOrigin.x();
        key.state.cy += frameOrigin.y();
    }
}

void fromJson(const QJsonObject &obj, Scene &scene, int *dropped)
{
    scene.camera.keys.clear();
    int bad = 0;
    const QJsonArray keys = obj.value(QStringLiteral("keys")).toArray();
    for (const QJsonValue &v : keys) {
        const QJsonObject k = v.toObject();
        const bool pinned = k.value(QStringLiteral("pinned")).toBool(false);
        const double time = k.value(QStringLiteral("time")).toDouble(-1.0);
        const int index = k.value(QStringLiteral("panel")).toInt(-1);
        const double at = k.value(QStringLiteral("at")).toDouble(-1.0);
        CameraState s;
        s.cx = k.value(QStringLiteral("x")).toDouble(qQNaN());
        s.cy = k.value(QStringLiteral("y")).toDouble(qQNaN());
        s.zoom = k.value(QStringLiteral("zoom")).toDouble(qQNaN());
        s.rotation = k.value(QStringLiteral("rotation")).toDouble(qQNaN());
        const bool where = pinned
            ? std::isfinite(time) && time >= 0.0
            : index >= 0 && index < scene.panels.size() && std::isfinite(at) && at >= 0.0
                && at <= 1.0;
        const bool ok = where
            && std::isfinite(s.cx) && std::isfinite(s.cy)
            && std::isfinite(s.zoom) && s.zoom >= 0.01 && s.zoom <= 100.0
            && std::isfinite(s.rotation);
        if (!ok) {
            ++bad;
            continue;
        }
        CameraKey key;
        if (pinned) {
            key.pinned = true;
            key.time = time;
        } else {
            key.panelUid = scene.panels.at(index)->uid.value();
            key.at = at;
        }
        key.state = s;
        key.ease = easeFromName(k.value(QStringLiteral("ease")).toString());
        scene.camera.keys.append(key);
    }
    if (dropped)
        *dropped = bad;
}

void remapForResize(SceneCamera &camera, const Stage &oldStage,
                    const Stage &newStage, const QPoint &offset)
{
    if (oldStage.frame.isEmpty() || newStage.frame.isEmpty())
        return;
    for (CameraKey &key : camera.keys) {
        key.state.cx += offset.x();
        key.state.cy += offset.y();
        key.state.zoom *= newStage.frame.width() / double(oldStage.frame.width());
    }
}

// --- The quick moves (Phase 1D) ----------------------------------------------

QString quickMoveLabel(QuickMove move)
{
    switch (move) {
    case QuickMove::PushIn: return QStringLiteral("Push In");
    case QuickMove::PullOut: return QStringLiteral("Pull Out");
    case QuickMove::PanLeft: return QStringLiteral("Pan Left");
    case QuickMove::PanRight: return QStringLiteral("Pan Right");
    case QuickMove::TiltUp: return QStringLiteral("Tilt Up");
    case QuickMove::TiltDown: return QStringLiteral("Tilt Down");
    case QuickMove::RotateLeft: return QStringLiteral("Rotate Left");
    case QuickMove::RotateRight: return QStringLiteral("Rotate Right");
    }
    return QString();
}

namespace {

bool sameFraming(const CameraState &a, const CameraState &b)
{
    return qAbs(a.cx - b.cx) < 0.5 && qAbs(a.cy - b.cy) < 0.5
        && qAbs(a.zoom / b.zoom - 1.0) < 1e-6 && qAbs(a.rotation - b.rotation) < 1e-6;
}

// A pan or tilt with Stay inside on, at zoom `z`: the START as near the
// given one as the move allows (where it is; else slid back along the move
// so the whole distance stays on the artwork). False: it does not fit.
// With depth (Phase 2C) every clamp is clampInside's with the panel's
// edge-reaching layers; *by: the layer that held the END in (-1: the stage).
bool travelFits(const CameraState &start, double z, double dx, double dy,
                const Stage &stage, const QVector<DepthEdge> &edges,
                CameraState *s, CameraState *e, int *by = nullptr)
{
    CameraState a = start;
    a.zoom = z;
    a = clampInside(a, stage, edges);
    const double tx = dx / z, ty = dy / z; // the move scales with the frame
    CameraState b = a;
    b.cx += tx;
    b.cy += ty;
    int endBy = -1;
    CameraState bc = clampInside(b, stage, edges, &endBy);
    if (by)
        *by = endBy;
    if (qAbs(bc.cx - b.cx) < 0.5 && qAbs(bc.cy - b.cy) < 0.5) {
        *s = a;
        *e = bc;
        return true;
    }
    // Slide the start back: the end where the artwork lets it be, the start
    // the whole distance behind it.
    CameraState a2 = bc;
    a2.cx -= tx;
    a2.cy -= ty;
    const CameraState a2c = clampInside(a2, stage, edges);
    if (qAbs(a2c.cx - a2.cx) < 0.5 && qAbs(a2c.cy - a2.cy) < 0.5) {
        *s = a2c;
        *e = bc;
        return true;
    }
    return false;
}

} // namespace

QuickFrames quickMove(QuickMove move, const CameraState &start,
                      const QuickAmounts &amounts, const Stage &stage,
                      bool stayInside)
{
    return quickMove(move, start, amounts, stage, stayInside, QVector<DepthEdge>());
}

QuickFrames quickMove(QuickMove move, const CameraState &start,
                      const QuickAmounts &amounts, const Stage &stage,
                      bool stayInside, const QVector<DepthEdge> &edges)
{
    QuickFrames r;
    r.start = stayInside ? clampInside(start, stage, edges) : clampZoom(start);
    r.placedStart = r.start;
    int lastBy = -1; // the depth layer behind the latest clamp that held
    auto place = [&](const CameraState &s) {
        if (!stayInside)
            return clampZoom(s);
        int by = -1;
        const CameraState c = clampInside(s, stage, edges, &by);
        if (by >= 0)
            lastBy = by;
        return c;
    };
    switch (move) {
    case QuickMove::PushIn: {
        CameraState e = r.start;
        e.zoom *= amounts.zoom;
        r.end = place(e);
        break;
    }
    case QuickMove::PullOut: {
        CameraState e = r.start;
        e.zoom /= amounts.zoom;
        r.end = place(e);
        if (stayInside && r.end.zoom >= r.start.zoom * (1.0 - 1e-9)) {
            // No room to pull out (decision 4): start pushed in by the Zoom,
            // end at the whole artwork - with a margin, the largest framing
            // the stage holds (Phase 2A).
            const double whole = insideMinZoom(r.start.rotation, stage);
            CameraState s = r.start;
            s.zoom = whole * amounts.zoom;
            r.start = place(s);
            CameraState w = r.start;
            w.zoom = whole;
            r.end = place(w);
        }
        break;
    }
    case QuickMove::PanLeft:
    case QuickMove::PanRight:
    case QuickMove::TiltUp:
    case QuickMove::TiltDown: {
        // Along the frame's own axes: x = (cos r, sin r), y = (-sin r, cos r)
        // in canvas px (y down, the rotation clockwise). The distance at zoom
        // 1 is the Move's share of the FRAME (the canvas without a margin);
        // it scales as 1 / zoom.
        const double rad = qDegreesToRadians(r.start.rotation);
        const bool pan = move == QuickMove::PanLeft || move == QuickMove::PanRight;
        const double sign = (move == QuickMove::PanRight || move == QuickMove::TiltDown) ? 1.0 : -1.0;
        const double d = sign * amounts.move * (pan ? stage.frame.width() : stage.frame.height());
        const double dx = pan ? d * std::cos(rad) : -d * std::sin(rad);
        const double dy = pan ? d * std::sin(rad) : d * std::cos(rad);
        if (!stayInside) {
            CameraState e = r.start;
            e.cx += dx / e.zoom;
            e.cy += dy / e.zoom;
            r.end = e;
            break;
        }
        // Decision 3: the start gives way just enough - slid back, then
        // pushed in (the smallest zoom that fits, found by halving).
        CameraState s, e;
        int by = -1;
        const bool fitsHere = travelFits(r.start, r.start.zoom, dx, dy, stage, edges, &s, &e, &by);
        if (by >= 0)
            lastBy = by; // what held the END in at the start's own zoom
        if (fitsHere) {
            r.start = s;
            r.end = e;
            break;
        }
        double lo = r.start.zoom, hi = lo;
        bool found = false;
        while (hi < kMaxZoom) {
            hi = qMin(kMaxZoom, hi * 1.05);
            if (travelFits(r.start, hi, dx, dy, stage, edges, &s, &e)) {
                found = true;
                break;
            }
            lo = hi;
        }
        if (!found) {
            r.end = r.start;
            break;
        }
        for (int i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2.0;
            CameraState ms, me;
            if (travelFits(r.start, mid, dx, dy, stage, edges, &ms, &me))
                hi = mid;
            else
                lo = mid;
        }
        travelFits(r.start, hi, dx, dy, stage, edges, &s, &e);
        r.start = s;
        r.end = e;
        break;
    }
    case QuickMove::RotateLeft:
    case QuickMove::RotateRight: {
        CameraState e = r.start;
        e.rotation = normalisedAngle(e.rotation
                                     + (move == QuickMove::RotateRight ? amounts.turn
                                                                       : -amounts.turn));
        if (stayInside) {
            // Decision 3: the START gives way - pushed in just enough that
            // the turned frame fits at the same zoom, so the move stays a
            // pure turn (no zoom hidden in it).
            CameraState s = r.start;
            s.zoom = qMax(s.zoom, insideMinZoom(e.rotation, stage));
            e.zoom = s.zoom;
            e = place(e);
            s.cx = e.cx;
            s.cy = e.cy;
            r.start = place(s);
            e.cx = r.start.cx;
            e.cy = r.start.cy;
        }
        r.end = place(e);
        break;
    }
    }
    r.moves = !sameFraming(r.start, r.end);
    // The START gave way (Phase 2C's note): name the depth layer behind it.
    if (r.moves && !sameFraming(r.placedStart, r.start) && lastBy >= 0
        && lastBy < edges.size()) {
        r.stoppedBy = edges.at(lastBy).name;
        r.stoppedByDepth = edges.at(lastBy).parallax;
    }
    return r;
}

} // namespace shotcam
