#pragma once

// THE SHOT CAMERA'S ONE EVALUATION PATH (Phase 1A, 2026-10-25; spec
// sections 1-3). Every consumer - the playback preview, the SHOTS clip
// thumbnails, the MP4, the PNG and PDF stills, and (Part 1B) the canvas
// frame - asks evaluate() for the camera at a frame and draws through
// shotTransform() / renderShot(). The same frame number gives the same
// CameraState everywhere.
//
// NEUTRAL TAKES THE OLD CODE. A scene without keys evaluates to the exact
// neutral state, and every consumer, seeing isNeutral(), runs the code it
// ran before the camera existed: a project without camera moves renders
// the bytes the neutral pins (SankoCameraTest c1p) recorded on f46ccbcc6.

#include "camera/CameraModel.h"

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>

class QPainter;
class QPainterPath;
class QPixmap;
struct Panel;
struct Scene;

namespace shotcam {

// The MP4's frame: the reference for "sharp" (spec section 5).
inline constexpr QSize kOutputFrame(1920, 1080);
// The veil over a stage margin with a drawing tool and in the strip
// (Phase 2A, the user's decision D5): black at 20 %. The Camera tool keeps
// its own 40 % dim.
inline const QColor kStageVeil(0, 0, 0, 51);

// The neutral camera: centred, 1x. On a stage with a margin that is the
// BASE FRAMING (the frame centred), not the whole canvas - the same numbers.
CameraState neutral(const QSize &canvas);
bool isNeutral(const CameraState &state, const QSize &canvas);
// The same on a stage that may have its own margins (batch 6c): the frame's
// centre, wherever the frame lies.
CameraState neutral(const Stage &stage);
bool isNeutral(const CameraState &state, const Stage &stage);
// The pre-camera code may draw it: no margin, and the neutral camera.
bool takesOldPath(const CameraState &state, const Stage &stage);
// A panel's stage (batch 6c): its own margins when it has them, else the
// project's `projectMargin` around its pixels.
Stage panelStage(const Panel &panel, const QSize &projectMargin);
// THE PART OF A MOVE THAT LEAVES A PANEL'S ART (batch 6d, Q9, Figma MARGIN-2):
// the panel's frames whose shot goes past its stage, as the camera plays them -
// never clamped, so it cannot jump at a cut - and the margin each side would need
// to hold the whole move. Past the stage's edge by half a pixel or more, or held in
// by a depth layer that reaches it (Phase 2C).
struct ArtExit
{
    int frames = 0;   // how many of the panel's frames leave its art
    int first = -1;   // the first and last of them, from the panel's start
    int last = -1;
    QMargins grow;    // whole px each side, to fit every frame of the move
    bool leaves() const { return frames > 0; }
};
ArtExit artExit(const Scene &scene, int panelIndex, int fps, int filmStart, const Stage &stage);

// The keys' space and a panel's stage (Panel::keyShift): a state read on the
// panel, written to a key; a key, read on the panel. Nothing without an own margin.
CameraState toPanelSpace(const Panel &panel, const CameraState &state);
CameraState toCameraSpace(const Panel &panel, const CameraState &state);

// w(u) for u in [0, 1]: Linear u; Ease In u^2; Ease Out 1-(1-u)^2;
// Ease In/Out 3u^2-2u^3; Hold 0 until the next key.
double ease(Ease e, double u);
QString easeName(Ease e);   // the file's word ("easeInOut")
QString easeLabel(Ease e);  // the artist's ("Ease In/Out"): the Camera bar, the CAMERA row
Ease easeFromName(const QString &name, bool *known = nullptr);

// THE PLAYHEAD'S ROUNDING: the frame a time inside a panel falls in - the
// timeline's own expression (AnimaticTimeline::playheadFrame), so the
// camera and the playhead never disagree about which frame it is.
int frameInPanelFromMs(qint64 ms, int fps);
// The first millisecond of a frame inside a panel (its inverse).
int msForFrameInPanel(int frame, int fps);

// A panel's length at `fps` (2026-11-06, batch 5): its frames counted at
// its own rate, converted - exact when the rates agree or the length is
// whole seconds, else rounded to the nearest frame (a half up), and never
// less than 1. Every reader of a panel's length goes through this.
int panelFrames(const Panel &panel, int fps);
int framesAtRate(int frames, int fromFps, int toFps);
// A length for the artist: "2s", "1s 12f", "12f" (the Seconds view, the
// drag chip, the typing field's "= ..."); framesText: "36f" (the Frames view).
QString lengthText(int frames, int fps);
QString framesText(int frames);
int panelStartFrame(const Scene &scene, int panelIndex, int fps);
int panelIndexOfUid(const Scene &scene, quint64 uid); // -1: not in the scene

// A key's derived frame in its scene; -1 when it is INERT (its panel is
// not in the scene: deleted, its delete still in the undo history) or, for
// a PINNED key (Phase 2E), STRANDED: its film time, less the scene's film
// start `filmStart` (the frames before the scene; every caller knows its
// scene's place), falls outside the scene's span. A pinned key's frame is
// time x fps, snapped to the nearest whole frame when within 1e-6 of one,
// so a key pinned at frame f evaluates at f exactly at the same fps.
double keyFrame(const Scene &scene, const CameraKey &key, int fps, int filmStart = 0);
int sceneFrameCount(const Scene &scene, int fps); // every panel's frames
bool isStranded(const Scene &scene, const CameraKey &key, int fps, int filmStart);
// The rule itself, for a scene that starts at filmStart and is frameCount
// long: what a structural command asks of the film its redo will make.
bool pinnedOutside(const CameraKey &key, int fps, int filmStart, int frameCount);
int strandedCount(const Scene &scene, int fps, int filmStart);
// A pinned key's frame on the FILM (for drawing it, stranded or not).
double keyFilmFrame(const CameraKey &key, int fps);
int liveKeyCount(const Scene &scene, int fps = 24, int filmStart = 0);
int liveKeysOnPanel(const Scene &scene, const Panel *panel);
// A key set at frame `frameInPanel` of panel `panelIndex`.
CameraKey makeKey(const Scene &scene, int panelIndex, int frameInPanel,
                  int fps, const CameraState &state,
                  Ease ease = Ease::EaseInOut);
// A key PINNED at film frame `filmFrame` (Phase 2E).
CameraKey makePinnedKey(double filmFrame, int fps, const CameraState &state,
                        Ease ease = Ease::EaseInOut);
// The words an edit that strands or un-strands pinned keys adds to its
// undo text: " (1 pinned camera key now outside scene 2)", " (2 pinned
// camera keys back inside scene 2)"; empty when the count is unchanged.
QString strandingText(int strandedBefore, int strandedAfter, int sceneNumber);

// THE EVALUATION. `sceneFrame` counts from the scene's first frame. No
// live keys: neutral for `canvas`. Before the first / after the last key:
// that key's state. Between: centre and rotation linear in w, zoom
// geometric (a push-in at a steady speed). Live keys are sorted by frame,
// stably; a pinned key at an anchored key's frame sorts after it (they can
// only meet through a retime), so its framing shows from that frame.
CameraState evaluate(const Scene &scene, double sceneFrame, int fps,
                     const QSize &canvas, int filmStart = 0);
// The camera at frame `frameInPanel` of a panel, in that panel's stage px.
CameraState evaluateAtPanel(const Scene &scene, int panelIndex,
                            int frameInPanel, int fps, int filmStart = 0);

// The frame as a quad in canvas px (rotated about its centre). Its size is
// the stage's FRAME over the zoom (Phase 2A); without a margin, the canvas.
QPolygonF frameQuad(const CameraState &state, const Stage &stage);
// canvas px -> target: the frame FITTED in `target` (KeepAspectRatio,
// centred, as every consumer fits the whole panel today), or COVERING it
// (crop to fill, as the SHOTS clips are).
QTransform shotTransform(const CameraState &state, const Stage &stage,
                         const QRectF &target, bool cover = false);
// THE FRAME IN A TARGET (2026-11-07): where shotTransform(.., cover = false)
// puts the frame - fitted whole and centred, whatever the camera does - as
// the whole pixels inside it. A target of another shape has bars beside or
// above it, and nothing of the stage belongs in them: the preview blacks
// everything outside this, so only what the camera sees shows. Upright
// always: the frame is what the output is; a turn moves the picture inside it.
QRect shotFrameRect(const Stage &stage, const QRect &target);
// Paints everything of `target` outside that rect black, after the shot: what
// is inside it is the paint's own, untouched. The preview's picture and the
// film's frames both end with it.
void fillOutsideFrame(QPainter &painter, const Stage &stage, const QRect &target);
// The film's frames without it, the old bleed: only so a check can show what
// the black bars take away, and that they take nothing else.
void setFilmBarsOpenForTest(bool open);
// The shot as a picture of `out` size (black beyond the artwork, as the
// letterbox is). `flat` is the panel's full-resolution flatten - the whole
// stage. Reduces with the same area-averaging filter the export uses before
// the final bilinear placement, so fine line work is not thinned.
QImage renderShot(const QImage &flat, const CameraState &state,
                  const QSize &out);
QImage renderShot(const QImage &flat, const CameraState &state, const Stage &stage,
                  const QSize &out);
// Draws `picture` (the whole stage at any resolution) through the camera
// into `target` on a painter (the preview, the clips).
void drawShot(QPainter &painter, const QPixmap &picture, const Stage &stage,
              const CameraState &state, const QRectF &target, bool cover);
// Past this zoom an output pixel has less than one source pixel (spec 5):
// the FRAME's pixels against the output's.
double sharpLimit(const Stage &stage, const QSize &out = kOutputFrame);

// DEPTH (Phase 2B, 2026-11-01; the Phase 2 spec's 4.3 / 4.4, Figma 2B-1 to
// 2B-5). A layer of parallax `a` (Layer::parallax through the folder rule,
// Panel::effectiveParallax) under the camera (c, z), with the base
// framing's centre c0 (the stage's centre), shows a layer pixel p on the
// picture plane at
//     d = z (1 - a) + a,   q = c + (a (c0 - c) + (p - c0)) / d
// - a uniform scale by 1/d and a shift: layerToStage. Rotation stays in
// shotTransform, so the roll turns every layer alike. a = 1 is the identity
// for every camera, and the base framing (c = c0, z = 1) is the identity for
// every a: every layer lines up as drawn (the user's decision D8).
//
// THE ONE PLACEMENT. Every consumer that shows depth - the Camera tool's
// canvas, the preview, the SHOTS clips, the strip, the MP4 and the stills -
// draws each layer through layerToStage and nothing else decides where a
// layer goes. A panel without depth, or a camera at the base framing, takes
// the Phase 1 / 2A code unchanged (layeredShot is false): the neutral,
// moving and margin pins hold by construction.
double depthDivisor(double a, const CameraState &state); // d
QTransform layerToStage(double a, const CameraState &state, const Stage &stage);
// A layer the camera has reached is not drawn: d below kNearCut (a = 1.5
// stops just past 2.8x). Naming it on the canvas is 2C's (decision D10).
inline constexpr double kNearCut = 0.1;
bool layerShown(double a, const CameraState &state);
// The camera is away from the base framing (it moves or zooms; a turn alone
// places every layer alike).
bool offBaseFraming(const CameraState &state, const Stage &stage);
// The layered path draws this shot: the panel has depth and the camera is
// off its base framing.
bool layeredShot(const Panel &panel, const CameraState &state, const Stage &stage);
// THE LAYERED PATH. The paper is white on the picture plane's stage, then
// every drawn layer bottom to top with its opacity through layerToStage and
// the consumer's own transform, each resampled ONCE into its target (never
// composited at stage size and resampled again, so near layers are not
// softened twice); a layer that is reduced is first area-averaged to the
// nearest quarter-octave at or above its scale (Phase 1's rule against
// thinned lines), and that mip is cached by the layer's QImage::cacheKey().
// renderShot / drawShot are the Phase 1 functions' layered twins (the MP4
// and stills; the preview); stageImage is the stage as the camera sees it,
// in stage px scaled to `target` (the strip).
QImage renderShot(const Panel &panel, const CameraState &state, const Stage &stage,
                  const QSize &out);
void drawShot(QPainter &painter, const Panel &panel, const Stage &stage,
              const CameraState &state, const QRectF &target, bool cover);
QImage stageImage(const Panel &panel, const CameraState &state, const Stage &stage,
                  const QSize &target);
// stageImage's painter twin, in STAGE px through the painter's own
// transform (the Camera tool's canvas: the view transform); only what falls
// inside `visible` (stage px) is drawn.
void drawStage(QPainter &painter, const Panel &panel, const CameraState &state,
               const Stage &stage, const QRectF &visible);
// The layered pictures the small, often-repainted consumers show, cached by
// everything that shapes them (the layers' pixels, visibility, opacity and
// depth, the camera, the stage and the size): the SHOTS clip (drawShot into
// a `size` image) and the strip (stageImage). Worker-thread safe.
QImage shotThumb(const Panel &panel, const CameraState &state, const Stage &stage,
                 const QSize &size, bool cover);
QImage stageThumb(const Panel &panel, const CameraState &state, const Stage &stage,
                  const QSize &size);
// Test hooks: the mip cache's entries, and emptying both caches.
int layerMipCountForTest();
void clearDepthCachesForTest();

// THE CANVAS TOOL'S LIMITS (Phase 1B). The zoom runs from 0.5x (outwards)
// to 4x (inwards); STAY INSIDE ARTWORK (on by default) keeps every corner
// of the frame on the canvas - the centre is clamped, and a rotation pushes
// the zoom in just as far as the turned frame needs to fit.
inline constexpr double kMinZoom = 0.5;
inline constexpr double kMaxZoom = 4.0;
// With a margin (Phase 2A) "the artwork" is the stage: the turned frame
// must fit the canvas, so insideMinZoom can be below 1 (a pull-out shows
// the margin, down to the largest framing the stage holds).
CameraState clampZoom(const CameraState &state);
double insideMinZoom(double rotation, const Stage &stage);
CameraState clampInside(const CameraState &state, const Stage &stage);
// Degrees into (-180, 180].
double normalisedAngle(double degrees);

// Keys at the playhead (Phase 1B). The live key AT this scene frame
// (within half a frame), or -1; the key whose segment the frame is in (the
// key at it, else the last one before it), or -1; the live keys' frames in
// order.
int keyIndexAtFrame(const Scene &scene, double sceneFrame, int fps, int filmStart = 0);
int segmentKeyIndex(const Scene &scene, double sceneFrame, int fps, int filmStart = 0);
// THE MOVEMENT GLYPHS (2026-11-06, Figma CAM-2). What `to` does against
// `from`: a zoom by more than 1 % (in / out), the centre by more than 2 %
// of the frame's width (a pan, its travel in canvas px), a turn by more
// than 0.5 degrees. And the move of the segment a scene frame is in: none
// before the first live key, after the last, or on a Hold segment.
Motion motionBetween(const CameraState &from, const CameraState &to, const Stage &stage);
Motion segmentMotion(const Scene &scene, double sceneFrame, int fps, const Stage &stage,
                     int filmStart = 0);
QVector<double> liveKeyFrames(const Scene &scene, int fps, int filmStart = 0);
// THE GLYPHS' STROKES (CAM-2 as adjusted in Figma, 2026-11-06): every stroke
// of a move's glyph on a frame drawn as `quad` (widget px, its corners in
// frameQuad's order), the pan's way on screen given as a unit vector.
// Measured in the frame's own axes; `scale` 1 for the Camera tool, 10/14 for
// a drawing tool; the painter strokes them 1 px wide.
//   Push In / Pull Out: at each corner a 14 px arrow at 45 degrees, 6 px in
//     from both edges, its head inwards / at the corner.
//   A pan within 22.5 degrees of the frame's horizontal: a chevron (5 deep,
//     8 wide) on the top and on the bottom edge, 3 to 8 px past each edge's
//     middle the way the frame travels. A tilt: the same on the two sides.
//   A diagonal pan: at each corner a 15 px arrow along the travel, its tip
//     7 px short of the corner.
//   A turn: a half circle (radius 12) above the top edge, its centre 3.6 px
//     out from the edge's middle, the head at the end the frame turns to.
// Nothing in the middle of the frame. Arrowheads: two 6.3 px barbs at 35
// degrees. A pure function: the gate checks the numbers.
QPainterPath motionGlyphPath(const QPolygonF &quad, const Motion &motion,
                             const QPointF &screenPanDir, double scale);
// HH:MM:SS:FF of a frame counted from the film's start (the header's own
// format, AnimaticPage::updateTimecodeLabel).
QString timecodeText(int filmFrame, int fps);

// THE FILE. {"keys": [{"panel", "at", "x", "y", "zoom", "rotation",
// "ease"}]}; an EMPTY object when the scene has no live keys (the caller
// then writes no "camera" at all, so a project without camera moves saves
// the JSON it saved before). Inert keys are not written. A PINNED key
// (Phase 2E) writes {"pinned": true, "time", "x", ...} and no panel / at;
// stranded or not, it is written (the sound is still there). A project
// without pinned keys writes exactly the JSON it wrote before.
QJsonObject toJson(const Scene &scene);
// THE FILE IN FRAME SPACE (2026-11-06, batch 6a; file version 2). The keys'
// x and y measured from the shot frame's top-left - `frameOrigin` (where
// the frame starts on the stage, the margin's left and top) taken off - and
// added back by keysToStage on reading. Every panel shares the project's
// stage until batch 6c, so one origin serves every key, anchored or pinned.
// Without a margin the numbers are the same in both spaces. toJson above
// writes stage px, as version 1 did.
QJsonObject toFrameSpaceJson(const Scene &scene, const QPointF &frameOrigin);
void keysToStage(Scene &scene, const QPointF &frameOrigin);
// Read after the scene's panels are in place. A key whose panel index,
// numbers or zoom are out of range is dropped and counted; so is a pinned
// key whose time is not a finite number at or after 0.
void fromJson(const QJsonObject &obj, Scene &scene, int *dropped = nullptr);

// Canvas resize (centre-anchored, nothing scaled): every key keeps its
// framing over the same artwork - the centre moves with the offset, and
// the zoom keeps the frame's pixel width. A margin change leaves the frame
// as it was, so only the centres move (Phase 2A).
void remapForResize(SceneCamera &camera, const Stage &oldStage,
                    const Stage &newStage, const QPoint &offset);

// THE QUICK MOVES (Phase 1D, 2026-10-29; Figma 1D-1 to 1D-5). A whole move
// from one framing: Push In / Pull Out by the Zoom (Pull Out by its
// inverse), Pan / Tilt by the Move (a fraction of the frame, along the
// frame's own axes, so a turned frame pans the way it looks), Rotate by
// the Turn. Pure: the workspace puts START and END on the span's two keys.
// (QuickMove, QuickAmounts and QuickFrames are in CameraModel.h.)
QString quickMoveLabel(QuickMove move); // "Push In" .. "Rotate Right"
// What a Rotate names (the user's choice, 2026-11-07, Figma CAM-3: the drag,
// the names and the curved arrow all describe the FRAME). Empty for the rest.
QString quickMoveTooltip(QuickMove move);
// With STAY INSIDE ARTWORK on, END is clamped onto the artwork, and when
// that would leave less than the whole move the START gives way just
// enough (the user's decisions 3 and 4, 2026-10-29): a Pan or Tilt first
// slides the start back along the move, then pushes it in, until the full
// distance fits; a Pull Out with no room to pull starts pushed in by the
// Zoom and ends at the whole artwork; a Rotate pushes the END in just far
// enough for the turned frame. With it off nothing is clamped but the
// zoom's 0.5x .. 4x.
QuickFrames quickMove(QuickMove move, const CameraState &start,
                      const QuickAmounts &amounts, const Stage &stage,
                      bool stayInside);

// STAY INSIDE WITH DEPTH (Phase 2C, 2026-11-01; the spec's 4.6, decision
// D12, Figma 2C-3 / 2C-4). Stay inside keeps the frame's FOOTPRINT on every
// layer whose art reaches an edge of the stage inside that edge. On a layer
// of depth a the footprint is the turned frame box scaled by d, centred at
// c0 + a (c - c0), so each edge is one linear bound on the centre. The
// paper reaches every edge and is 2A's clamp; a layer at 0 % (its
// footprint is the base framing) or at 100 % (the paper's bound) adds
// nothing, nor does a layer the camera has passed. (DepthEdge, the data, is
// in CameraModel.h.)
// The panel's drawn layers off the picture plane (and not fixed to the
// camera) whose art reaches an edge: a pixel at 10 % opacity or more within
// 2 px of it (the user's Q6), worked out once per change of a layer (by its
// QImage::cacheKey). Empty for a panel without depth.
QVector<DepthEdge> depthEdges(const Panel &panel);
// clampInside with those layers. With none it IS clampInside(state, stage).
// When no centre fits at the zoom, the zoom is pushed in by Phase 1D's
// search (x1.05, then 40 halvings). *stoppedBy: the index in `edges` of the
// layer that held the frame in, -1 for the stage or nothing.
CameraState clampInside(const CameraState &state, const Stage &stage,
                        const QVector<DepthEdge> &edges, int *stoppedBy = nullptr);
// quickMove through that clamp (1D's rules unchanged: the START gives way,
// slid back, then pushed in). With no edges it is quickMove() exactly.
QuickFrames quickMove(QuickMove move, const CameraState &start,
                      const QuickAmounts &amounts, const Stage &stage,
                      bool stayInside, const QVector<DepthEdge> &edges);
// THE CAMERA PASSES A NEAR LAYER (Phase 2C, decision D10, Figma 2C-2): the
// drawn layers whose divisor is below kNearCut at `state` - not drawn -
// bottom to top, by name.
QStringList passedLayers(const Panel &panel, const CameraState &state);
// The mode chip's words for them (the user's Q1): "Passed 'Bush'",
// "Passed 'Bush', 'Leaves'", "Passed 3 layers"; empty for none.
QString passedText(const QStringList &names);
// Test hook: the edge cache's entries.
int depthEdgeCacheCountForTest();

} // namespace shotcam
