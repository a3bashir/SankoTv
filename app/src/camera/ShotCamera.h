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

#include <QImage>
#include <QJsonObject>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>

class QPainter;
class QPixmap;
struct Panel;
struct Scene;

namespace shotcam {

// The MP4's frame: the reference for "sharp" (spec section 5).
inline constexpr QSize kOutputFrame(1920, 1080);

CameraState neutral(const QSize &canvas);
bool isNeutral(const CameraState &state, const QSize &canvas);

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

int panelFrames(const Panel &panel, int fps); // duration (>= 1 s) * fps
int panelStartFrame(const Scene &scene, int panelIndex, int fps);
int panelIndexOfUid(const Scene &scene, quint64 uid); // -1: not in the scene

// A key's derived frame in its scene; -1 when it is INERT (its panel is
// not in the scene: deleted, its delete still in the undo history).
double keyFrame(const Scene &scene, const CameraKey &key, int fps);
int liveKeyCount(const Scene &scene);
int liveKeysOnPanel(const Scene &scene, const Panel *panel);
// A key set at frame `frameInPanel` of panel `panelIndex`.
CameraKey makeKey(const Scene &scene, int panelIndex, int frameInPanel,
                  int fps, const CameraState &state,
                  Ease ease = Ease::EaseInOut);

// THE EVALUATION. `sceneFrame` counts from the scene's first frame. No
// live keys: neutral for `canvas`. Before the first / after the last key:
// that key's state. Between: centre and rotation linear in w, zoom
// geometric (a push-in at a steady speed).
CameraState evaluate(const Scene &scene, double sceneFrame, int fps,
                     const QSize &canvas);
// The camera at frame `frameInPanel` of a panel, for that panel's canvas.
CameraState evaluateAtPanel(const Scene &scene, int panelIndex,
                            int frameInPanel, int fps);

// The frame as a quad in canvas px (rotated about its centre).
QPolygonF frameQuad(const CameraState &state, const QSize &canvas);
// canvas px -> target: the frame FITTED in `target` (KeepAspectRatio,
// centred, as every consumer fits the whole panel today), or COVERING it
// (crop to fill, as the SHOTS clips are).
QTransform shotTransform(const CameraState &state, const QSize &canvas,
                         const QRectF &target, bool cover = false);
// The shot as a picture of `out` size (black beyond the artwork, as the
// letterbox is). `flat` is the panel's full-resolution flatten. Reduces
// with the same area-averaging filter the export uses before the final
// bilinear placement, so fine line work is not thinned.
QImage renderShot(const QImage &flat, const CameraState &state,
                  const QSize &out);
// Draws `picture` (the whole canvas at any resolution) through the camera
// into `target` on a painter (the preview, the clips).
void drawShot(QPainter &painter, const QPixmap &picture, const QSize &canvas,
              const CameraState &state, const QRectF &target, bool cover);
// Past this zoom an output pixel has less than one source pixel (spec 5).
double sharpLimit(const QSize &canvas, const QSize &out = kOutputFrame);

// THE CANVAS TOOL'S LIMITS (Phase 1B). The zoom runs from 0.5x (outwards)
// to 4x (inwards); STAY INSIDE ARTWORK (on by default) keeps every corner
// of the frame on the canvas - the centre is clamped, and a rotation pushes
// the zoom in just as far as the turned frame needs to fit.
inline constexpr double kMinZoom = 0.5;
inline constexpr double kMaxZoom = 4.0;
CameraState clampZoom(const CameraState &state);
double insideMinZoom(double rotation, const QSize &canvas);
CameraState clampInside(const CameraState &state, const QSize &canvas);
// Degrees into (-180, 180].
double normalisedAngle(double degrees);

// Keys at the playhead (Phase 1B). The live key AT this scene frame
// (within half a frame), or -1; the key whose segment the frame is in (the
// key at it, else the last one before it), or -1; the live keys' frames in
// order.
int keyIndexAtFrame(const Scene &scene, double sceneFrame, int fps);
int segmentKeyIndex(const Scene &scene, double sceneFrame, int fps);
QVector<double> liveKeyFrames(const Scene &scene, int fps);
// HH:MM:SS:FF of a frame counted from the film's start (the header's own
// format, AnimaticPage::updateTimecodeLabel).
QString timecodeText(int filmFrame, int fps);

// THE FILE. {"keys": [{"panel", "at", "x", "y", "zoom", "rotation",
// "ease"}]}; an EMPTY object when the scene has no live keys (the caller
// then writes no "camera" at all, so a project without camera moves saves
// the JSON it saved before). Inert keys are not written.
QJsonObject toJson(const Scene &scene);
// Read after the scene's panels are in place. A key whose panel index,
// numbers or zoom are out of range is dropped and counted.
void fromJson(const QJsonObject &obj, Scene &scene, int *dropped = nullptr);

// Canvas resize (centre-anchored, nothing scaled): every key keeps its
// framing over the same artwork - the centre moves with the offset, and
// the zoom keeps the frame's pixel width.
void remapForResize(SceneCamera &camera, const QSize &oldSize,
                    const QSize &newSize, const QPoint &offset);

// THE QUICK MOVES (Phase 1D, 2026-10-29; Figma 1D-1 to 1D-5). A whole move
// from one framing: Push In / Pull Out by the Zoom (Pull Out by its
// inverse), Pan / Tilt by the Move (a fraction of the frame, along the
// frame's own axes, so a turned frame pans the way it looks), Rotate by
// the Turn. Pure: the workspace puts START and END on the span's two keys.
// (QuickMove, QuickAmounts and QuickFrames are in CameraModel.h.)
QString quickMoveLabel(QuickMove move); // "Push In" .. "Rotate Right"
// With STAY INSIDE ARTWORK on, END is clamped onto the artwork, and when
// that would leave less than the whole move the START gives way just
// enough (the user's decisions 3 and 4, 2026-10-29): a Pan or Tilt first
// slides the start back along the move, then pushes it in, until the full
// distance fits; a Pull Out with no room to pull starts pushed in by the
// Zoom and ends at the whole artwork; a Rotate pushes the END in just far
// enough for the turned frame. With it off nothing is clamped but the
// zoom's 0.5x .. 4x.
QuickFrames quickMove(QuickMove move, const CameraState &start,
                      const QuickAmounts &amounts, const QSize &canvas,
                      bool stayInside);

} // namespace shotcam
