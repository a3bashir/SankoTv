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

#include <QVector>
#include <QtGlobal>

#include <atomic>

namespace shotcam {

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

struct CameraKey
{
    quint64 panelUid = 0; // the panel it was set on (PanelUid::value)
    double at = 0.0;      // where in that panel: 0 = its first frame, 1 = its end
    CameraState state;
    Ease ease = Ease::EaseInOut;
};

struct SceneCamera
{
    QVector<CameraKey> keys; // empty = the neutral camera
    bool isEmpty() const { return keys.isEmpty(); }
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
