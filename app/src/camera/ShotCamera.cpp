#include "camera/ShotCamera.h"

#include "StoryboardModel.h"

#include <QJsonArray>
#include <QPainter>
#include <QPixmap>
#include <QtMath>

#include <algorithm>
#include <cmath>

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

int panelFrames(const Panel &panel, int fps)
{
    return qMax(1, panel.duration) * qMax(1, fps);
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

double keyFrame(const Scene &scene, const CameraKey &key, int fps)
{
    const int index = panelIndexOfUid(scene, key.panelUid);
    if (index < 0)
        return -1.0;
    return panelStartFrame(scene, index, fps)
        + key.at * panelFrames(*scene.panels.at(index), fps);
}

int liveKeyCount(const Scene &scene)
{
    int n = 0;
    for (const CameraKey &key : scene.camera.keys)
        if (panelIndexOfUid(scene, key.panelUid) >= 0)
            ++n;
    return n;
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
                     const QSize &canvas)
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
        const double f = keyFrame(scene, scene.camera.keys.at(i), fps);
        if (f >= 0.0)
            live.append({f, i, &scene.camera.keys.at(i)});
    }
    if (live.isEmpty())
        return neutral(canvas);
    std::stable_sort(live.begin(), live.end(), [](const Live &a, const Live &b) {
        return a.frame < b.frame;
    });
    if (sceneFrame <= live.first().frame)
        return live.first().key->state;
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
                            int frameInPanel, int fps)
{
    if (panelIndex < 0 || panelIndex >= scene.panels.size()
        || !scene.panels.at(panelIndex))
        return CameraState();
    const QSize canvas = scene.panels.at(panelIndex)->canvasSize();
    if (scene.camera.isEmpty())
        return neutral(canvas);
    return evaluate(scene, panelStartFrame(scene, panelIndex, fps) + frameInPanel,
                    fps, canvas);
}

QPolygonF frameQuad(const CameraState &state, const QSize &canvas)
{
    const double hw = canvas.width() / state.zoom / 2.0;
    const double hh = canvas.height() / state.zoom / 2.0;
    QTransform t;
    t.translate(state.cx, state.cy);
    t.rotate(state.rotation);
    return t.map(QPolygonF({QPointF(-hw, -hh), QPointF(hw, -hh),
                            QPointF(hw, hh), QPointF(-hw, hh)}));
}

QTransform shotTransform(const CameraState &state, const QSize &canvas,
                         const QRectF &target, bool cover)
{
    const double fw = canvas.width() / state.zoom;
    const double fh = canvas.height() / state.zoom;
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

QImage renderShot(const QImage &flat, const CameraState &state,
                  const QSize &out)
{
    QImage frame(out, QImage::Format_RGBX8888);
    frame.fill(Qt::black);
    if (flat.isNull() || out.isEmpty())
        return frame;
    const QSize canvas = flat.size();
    const QTransform toOut = shotTransform(state, canvas, QRectF(QPointF(0, 0), out));
    // Only the artwork the frame can see: the quad's bounds, in source px.
    const QRect src =
        frameQuad(state, canvas).boundingRect().adjusted(-2, -2, 2, 2).toAlignedRect()
        & flat.rect();
    if (src.isEmpty())
        return frame;
    const double scale = qMin(out.width() / (canvas.width() / state.zoom),
                              out.height() / (canvas.height() / state.zoom));
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

void drawShot(QPainter &painter, const QPixmap &picture, const QSize &canvas,
              const CameraState &state, const QRectF &target, bool cover)
{
    if (picture.isNull() || canvas.isEmpty())
        return;
    QTransform pictureToCanvas;
    pictureToCanvas.scale(canvas.width() / double(picture.width()),
                          canvas.height() / double(picture.height()));
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setTransform(pictureToCanvas * shotTransform(state, canvas, target, cover),
                         true);
    painter.drawPixmap(0, 0, picture);
    painter.restore();
}

double sharpLimit(const QSize &canvas, const QSize &out)
{
    if (out.isEmpty())
        return 1.0;
    return qMax(canvas.width() / double(out.width()),
                canvas.height() / double(out.height()));
}

CameraState clampZoom(const CameraState &state)
{
    CameraState s = state;
    s.zoom = qBound(kMinZoom, s.zoom, kMaxZoom);
    return s;
}

double insideMinZoom(double rotation, const QSize &canvas)
{
    if (canvas.isEmpty())
        return 1.0;
    const double r = qDegreesToRadians(rotation);
    const double c = qAbs(std::cos(r)), s = qAbs(std::sin(r));
    const double W = canvas.width(), H = canvas.height();
    // The turned frame's bounding box (fw c + fh s) x (fw s + fh c), with
    // fw = W / z and fh = H / z, must fit W x H.
    return qMax((W * c + H * s) / W, (W * s + H * c) / H);
}

CameraState clampInside(const CameraState &state, const QSize &canvas)
{
    CameraState s = clampZoom(state);
    if (canvas.isEmpty())
        return s;
    s.zoom = qMin(kMaxZoom, qMax(s.zoom, insideMinZoom(s.rotation, canvas)));
    const double r = qDegreesToRadians(s.rotation);
    const double c = qAbs(std::cos(r)), sn = qAbs(std::sin(r));
    const double fw = canvas.width() / s.zoom, fh = canvas.height() / s.zoom;
    const double bw = fw * c + fh * sn, bh = fw * sn + fh * c;
    s.cx = qBound(bw / 2.0, s.cx, canvas.width() - bw / 2.0);
    s.cy = qBound(bh / 2.0, s.cy, canvas.height() - bh / 2.0);
    return s;
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

int keyIndexAtFrame(const Scene &scene, double sceneFrame, int fps)
{
    for (int i = 0; i < scene.camera.keys.size(); ++i) {
        const double f = keyFrame(scene, scene.camera.keys.at(i), fps);
        if (f >= 0.0 && qAbs(f - sceneFrame) < 0.5)
            return i;
    }
    return -1;
}

int segmentKeyIndex(const Scene &scene, double sceneFrame, int fps)
{
    const int at = keyIndexAtFrame(scene, sceneFrame, fps);
    if (at >= 0)
        return at;
    int best = -1;
    double bestFrame = -1.0;
    for (int i = 0; i < scene.camera.keys.size(); ++i) {
        const double f = keyFrame(scene, scene.camera.keys.at(i), fps);
        if (f >= 0.0 && f < sceneFrame && f > bestFrame) {
            best = i;
            bestFrame = f;
        }
    }
    return best;
}

QVector<double> liveKeyFrames(const Scene &scene, int fps)
{
    QVector<double> frames;
    for (const CameraKey &key : scene.camera.keys) {
        const double f = keyFrame(scene, key, fps);
        if (f >= 0.0)
            frames.append(f);
    }
    std::sort(frames.begin(), frames.end());
    return frames;
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

QJsonObject toJson(const Scene &scene)
{
    QJsonArray keys;
    for (const CameraKey &key : scene.camera.keys) {
        const int index = panelIndexOfUid(scene, key.panelUid);
        if (index < 0)
            continue; // inert: its panel is deleted
        QJsonObject k;
        k[QStringLiteral("panel")] = index;
        k[QStringLiteral("at")] = key.at;
        k[QStringLiteral("x")] = key.state.cx;
        k[QStringLiteral("y")] = key.state.cy;
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

void fromJson(const QJsonObject &obj, Scene &scene, int *dropped)
{
    scene.camera.keys.clear();
    int bad = 0;
    const QJsonArray keys = obj.value(QStringLiteral("keys")).toArray();
    for (const QJsonValue &v : keys) {
        const QJsonObject k = v.toObject();
        const int index = k.value(QStringLiteral("panel")).toInt(-1);
        const double at = k.value(QStringLiteral("at")).toDouble(-1.0);
        CameraState s;
        s.cx = k.value(QStringLiteral("x")).toDouble(qQNaN());
        s.cy = k.value(QStringLiteral("y")).toDouble(qQNaN());
        s.zoom = k.value(QStringLiteral("zoom")).toDouble(qQNaN());
        s.rotation = k.value(QStringLiteral("rotation")).toDouble(qQNaN());
        const bool ok = index >= 0 && index < scene.panels.size()
            && std::isfinite(at) && at >= 0.0 && at <= 1.0
            && std::isfinite(s.cx) && std::isfinite(s.cy)
            && std::isfinite(s.zoom) && s.zoom >= 0.01 && s.zoom <= 100.0
            && std::isfinite(s.rotation);
        if (!ok) {
            ++bad;
            continue;
        }
        CameraKey key;
        key.panelUid = scene.panels.at(index)->uid.value();
        key.at = at;
        key.state = s;
        key.ease = easeFromName(k.value(QStringLiteral("ease")).toString());
        scene.camera.keys.append(key);
    }
    if (dropped)
        *dropped = bad;
}

void remapForResize(SceneCamera &camera, const QSize &oldSize,
                    const QSize &newSize, const QPoint &offset)
{
    if (oldSize.isEmpty() || newSize.isEmpty())
        return;
    for (CameraKey &key : camera.keys) {
        key.state.cx += offset.x();
        key.state.cy += offset.y();
        key.state.zoom *= newSize.width() / double(oldSize.width());
    }
}

} // namespace shotcam
