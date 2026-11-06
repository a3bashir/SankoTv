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

#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>

namespace shotcam {

// THE STAGE (Phase 2A, the stage margin, 2026-10-31; the Phase 2 spec's
// section 3). The CANVAS is what the layers are - the stage, drawn on - and
// the FRAME is the shot at 1x, what the MP4 and the stills show. Without a
// margin they are the same size (every project made before 2A), and every
// camera function reduces, term for term, to the Phase 1 expression it
// replaced. The margins are symmetric whole pixels, so the base framing
// (zoom 1, centred) is the frame centred on the stage.
struct Stage
{
    QSize canvas;
    QSize frame;
    Stage() = default;
    // Implicit on purpose: a size alone is a stage without a margin.
    Stage(const QSize &size) : canvas(size), frame(size) {}
    Stage(const QSize &canvasSize, const QSize &frameSize)
        : canvas(canvasSize), frame(frameSize.isEmpty() ? canvasSize : frameSize)
    {
    }
    bool hasMargin() const { return frame != canvas; }
    QSize margin() const
    {
        return QSize((canvas.width() - frame.width()) / 2,
                     (canvas.height() - frame.height()) / 2);
    }
    // The base framing in canvas px.
    QRect frameRect() const { return QRect(QPoint(margin().width(), margin().height()), frame); }
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
