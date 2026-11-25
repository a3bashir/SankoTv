#pragma once

// THE SHOT CAMERA'S DATA (Phase 1A, 2026-10-25; the spec is
// Documents\SankoTV-Design\camera\phase1-spec.md). Header-only on purpose:
// StoryboardModel.h holds these by value, and every target that compiles
// the model compiles them for free. The functions live in ShotCamera.h.
//
// ONE CAMERA PER SCENE, owned by the scene (never by a panel). A key is
// ANCHORED to the panel it was set on - (panel, where in it) - and its time
// in the scene is DERIVED from the panels' durations, so retiming,
// inserting, deleting and reordering panels moves every key with its
// drawing without any command touching a key (the spec's section 2, the
// user's decision 1).

#include <QMargins>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace shotcam {

// THE STAGE (Phase 2A, the stage margin, 2026-10-31; the Phase 2 spec's
// section 3). The CANVAS is what the layers are - the stage, drawn on - and
// the FRAME is the shot at 1x, what the MP4 and the stills show. Without a
// margin they are the same size (every project made before 2A), and every
// camera function reduces, term for term, to the Phase 1 expression it
// replaced. The base framing (zoom 1) is the frame, wherever it lies on the
// stage: centred with the project's symmetric margin (Phase 2A), anywhere
// with a panel's own margins, which differ side by side (batch 6c).
struct Stage
{
    QSize canvas;
    QSize frame;
    QPoint origin; // the frame's top-left on the stage
    Stage() = default;
    // Implicit on purpose: a size alone is a stage without a margin.
    Stage(const QSize &size) : canvas(size), frame(size) {}
    Stage(const QSize &canvasSize, const QSize &frameSize)
        : canvas(canvasSize), frame(frameSize.isEmpty() ? canvasSize : frameSize),
          origin((canvas.width() - frame.width()) / 2, (canvas.height() - frame.height()) / 2)
    {
    }
    Stage(const QSize &canvasSize, const QSize &frameSize, const QPoint &frameOrigin)
        : canvas(canvasSize), frame(frameSize), origin(frameOrigin)
    {
    }
    bool hasMargin() const { return frame != canvas; }
    QSize margin() const
    {
        return QSize((canvas.width() - frame.width()) / 2,
                     (canvas.height() - frame.height()) / 2);
    }
    // The base framing in canvas px.
    QRect frameRect() const { return QRect(origin, frame); }
    QPointF frameCentre() const
    {
        return QPointF(origin.x() + frame.width() / 2.0, origin.y() + frame.height() / 2.0);
    }
};

// A stage of `canvas` with `margin` px on each side (0 x 0: none). An
// invalid margin - negative, or leaving no frame - is no margin.
inline Stage stageWithMargin(const QSize &canvas, const QSize &margin)
{
    const QSize frame(canvas.width() - 2 * margin.width(),
                      canvas.height() - 2 * margin.height());
    if (margin.width() < 0 || margin.height() < 0 || frame.width() < 1
        || frame.height() < 1)
        return Stage(canvas);
    return Stage(canvas, frame);
}

// A stage of `canvas` with each side's own margin (batch 6c). Invalid margins,
// the same: no margin.
inline Stage stageWithMargins(const QSize &canvas, const QMargins &margins)
{
    const QSize frame(canvas.width() - margins.left() - margins.right(),
                      canvas.height() - margins.top() - margins.bottom());
    if (margins.left() < 0 || margins.top() < 0 || margins.right() < 0 || margins.bottom() < 0
        || frame.width() < 1 || frame.height() < 1)
        return Stage(canvas);
    return Stage(canvas, frame, QPoint(margins.left(), margins.top()));
}

// One framing. The frame has the CANVAS's aspect: width = canvasW / zoom,
// height = canvasH / zoom, centred on (cx, cy), turned by `rotation`
// degrees clockwise about its centre. NEUTRAL = {canvasW/2, canvasH/2, 1,
// 0}: the whole canvas, which is exactly the shot every project had before
// the camera existed.
struct CameraState
{
    double cx = 0.0;       // canvas px
    double cy = 0.0;       // canvas px
    double zoom = 1.0;     // 1 = the whole canvas; 2 = half its width
    double rotation = 0.0; // degrees, clockwise
    bool operator==(const CameraState &o) const
    {
        return cx == o.cx && cy == o.cy && zoom == o.zoom
            && rotation == o.rotation;
    }
    bool operator!=(const CameraState &o) const { return !(*this == o); }
};

// The ease of the segment that STARTS at a key.
enum class Ease { Linear, EaseIn, EaseOut, EaseInOut, Hold };

// w(u) for u in [0, 1], the one curve of every key's ease - the camera's (shotcam::ease
// reads it) and a layer's (batch I-1): Linear u; Ease In u^2; Ease Out 1-(1-u)^2; Ease
// In/Out 3u^2-2u^3; Hold 0 until the next key. Here, header-only, so the model's own
// flatten (StoryboardModel.h) poses a layer with the camera's curve, in every target.
inline double easeCurve(Ease e, double u)
{
    u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
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

// WHAT A MOVE DOES (2026-11-06, Figma CAM-2): a segment's end key against
// its start key, for the canvas's movement glyphs - editing guides only,
// never drawn by anything that renders the shot. shotcam::motionBetween /
// segmentMotion (ShotCamera.h) work it out.
struct Motion
{
    int zoom = 0;   // +1 pushes in, -1 pulls out, 0 neither
    QPointF pan;    // the centre's travel in canvas px; null: no pan
    int rotate = 0; // +1 clockwise, -1 anticlockwise, 0 neither
    bool any() const { return zoom != 0 || !pan.isNull() || rotate != 0; }
};

struct CameraKey
{
    quint64 panelUid = 0; // the panel it was set on (PanelUid::value)
    double at = 0.0;      // where in that panel: 0 = its first frame, 1 = its end
    CameraState state;
    Ease ease = Ease::EaseInOut;
    // PINNED TO THE SOUND (Phase 2E, 2026-11-01; the Phase 2 spec's 5.2,
    // decisions D16-D21): the key holds its TIME on the film instead of its
    // panel, so a retime moves the drawings under it and it stays on its
    // beat. panelUid / at are unused while pinned. A pinned key whose time
    // falls outside its scene's span is STRANDED (derived, never stored):
    // kept, saved, drawn, not evaluated, live again when the scene covers
    // its time again.
    bool pinned = false;
    double time = 0.0; // seconds from the film's start; used only when pinned
};

struct SceneCamera
{
    QVector<CameraKey> keys; // empty = the neutral camera
    bool isEmpty() const { return keys.isEmpty(); }
};

// LAYER MOTION (batch I-1, 2026-11-24; Figma I-1 693:755, approved with the user's answers
// that evening). A layer's own move over its panel, before its depth and the camera place
// it: where it is moved from where it is drawn, turned and sized about its PIVOT (the
// centre of its drawn art when it first animated), and its opacity. The identity pose is
// the layer as drawn.
struct LayerPose
{
    double dx = 0.0, dy = 0.0; // stage px, from where it is drawn
    double rotation = 0.0;     // degrees, clockwise, about the pivot
    double scale = 1.0;        // one size for both directions, about the pivot (1 = as drawn)
    double opacity = 1.0;      // 0 .. 1, the Layers panel's slider
    bool operator==(const LayerPose &o) const
    {
        return dx == o.dx && dy == o.dy && rotation == o.rotation && scale == o.scale && opacity == o.opacity;
    }
    bool operator!=(const LayerPose &o) const { return !(*this == o); }
};

// One key of a layer's motion. ANCHORED TO ITS PANEL as the camera's keys are (the user's
// answer 1): `at` is where in the panel - 0 its first frame, 1 its end - so a panel made
// longer stretches the move with it. A layer belongs to one panel, so no uid is needed.
struct LayerKey
{
    double at = 0.0;
    LayerPose pose;
    Ease ease = Ease::EaseInOut; // the move that STARTS at this key
    bool operator==(const LayerKey &o) const { return at == o.at && pose == o.pose && ease == o.ease; }
    bool operator!=(const LayerKey &o) const { return !(*this == o); }
};

// A layer's pose at `at` from its keys, sorted or not: before the first key that key's,
// after the last the last's; between two, position, turn and opacity in a straight line
// on the ease's w, the size on a ratio scale (as the camera's zoom: 50 % - 100 % - 200 %
// are even steps). No keys: the identity pose with `restOpacity`. Header-only (above).
inline LayerPose poseFromKeys(const QVector<LayerKey> &keys, double at, double restOpacity)
{
    LayerPose rest;
    rest.opacity = restOpacity;
    if (keys.isEmpty())
        return rest;
    QVector<const LayerKey *> sorted;
    sorted.reserve(keys.size());
    for (const LayerKey &k : keys)
        sorted.append(&k);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const LayerKey *a, const LayerKey *b) { return a->at < b->at; });
    if (at <= sorted.first()->at)
        return sorted.first()->pose;
    if (at >= sorted.last()->at)
        return sorted.last()->pose;
    int i = int(sorted.size()) - 2;
    while (i > 0 && sorted.at(i)->at > at)
        --i;
    const LayerKey &a = *sorted.at(i);
    const LayerKey &b = *sorted.at(i + 1);
    const double span = b.at - a.at;
    if (span <= 0.0)
        return b.pose;
    const double w = easeCurve(a.ease, (at - a.at) / span);
    LayerPose p;
    p.dx = a.pose.dx + (b.pose.dx - a.pose.dx) * w;
    p.dy = a.pose.dy + (b.pose.dy - a.pose.dy) * w;
    p.rotation = a.pose.rotation + (b.pose.rotation - a.pose.rotation) * w;
    p.scale = a.pose.scale == b.pose.scale || a.pose.scale <= 0.0 || b.pose.scale <= 0.0
        ? (w < 1.0 ? a.pose.scale : b.pose.scale)
        : std::exp(std::log(a.pose.scale) + (std::log(b.pose.scale) - std::log(a.pose.scale)) * w);
    p.opacity = a.pose.opacity + (b.pose.opacity - a.pose.opacity) * w;
    return p;
}

// One panel of a scene's layout as an edit will leave it (batch F1): its camera uid
// and its frames at the project's rate (shotcam::keysKeepingFrames).
struct PanelSpan
{
    quint64 uid = 0;
    int frames = 1;
};

// THE QUICK MOVES (Phase 1D): the data; shotcam::quickMove (ShotCamera.h)
// works them out.
enum class QuickMove { PushIn, PullOut, PanLeft, PanRight, TiltUp, TiltDown,
                       RotateLeft, RotateRight };
struct QuickAmounts
{
    double zoom = 1.25; // Push In multiplies, Pull Out divides
    double move = 0.25; // of the frame's width (Pan) or height (Tilt)
    double turn = 10.0; // degrees
};
// A layer whose art reaches an edge of the stage, off the picture plane:
// what Stay inside with depth holds the frame in by (Phase 2C;
// shotcam::depthEdges works them out).
struct DepthEdge
{
    double parallax = 1.0; // the layer's depth (Panel::effectiveParallax)
    bool left = false, top = false, right = false, bottom = false;
    // How far the layer's art goes past each of those edges, its art kept outside
    // the stage included (batch B2: it holds the frame at its real art's edge), in
    // stage px; 0: to the stage's edge, as before kept art.
    double leftPast = 0.0, topPast = 0.0, rightPast = 0.0, bottomPast = 0.0;
    QString name;
};

struct QuickFrames
{
    CameraState start; // may differ from the given start (see quickMove)
    CameraState end;
    bool moves = false; // false: no move fits (the zoom is at its limit)
    // Phase 2C (the give-way note, Q7): the START as given, placed on the
    // artwork before it gave way (compare with `start`), and the depth
    // layer that made it give way - its name and depth; empty: the stage.
    CameraState placedStart;
    QString stoppedBy;
    double stoppedByDepth = 1.0;
};

// A panel's identity for the camera. Minted per OBJECT: a copied Panel
// (duplicate, paste, clipboard) gets a new one, an assignment keeps the
// target's own, and it is never saved - in the file a key names its panel
// by index. A key holds this number, never a pointer, so a panel that is
// deleted (or destroyed with a truncated undo history) can only make its
// keys INERT, never dangling.
class PanelUid
{
public:
    PanelUid() : m_value(next()) {}
    PanelUid(const PanelUid &) : m_value(next()) {}
    PanelUid &operator=(const PanelUid &) { return *this; }
    quint64 value() const { return m_value; }

private:
    static quint64 next()
    {
        static std::atomic<quint64> counter{0};
        return ++counter;
    }
    quint64 m_value;
};

} // namespace shotcam
