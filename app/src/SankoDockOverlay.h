#pragma once

#include "DockManager.h"

#include <QObject>

namespace ads {
class CDockOverlay;
}
class QTimer;

// CDockManager keeps its two stock overlay instances behind PROTECTED
// accessors. This thin subclass promotes them to public (a using-declaration
// is the intended subclass surface — no ADS source is edited) so the custom
// paint filter below can reach them.
class SankoDockManager : public ads::CDockManager
{
public:
    using ads::CDockManager::CDockManager;
    using ads::CDockManager::containerOverlay; // promote protected -> public
    using ads::CDockManager::dockAreaOverlay;
};

// Photoshop-style docking drop hints for ADS, implemented WITHOUT editing the
// ADS library: an event filter on the two stock CDockOverlay instances
// swallows their paint events and renders a slim amber edge glow (or a
// tab-bar-only highlight for the dock-as-tab target) instead of ADS's filled
// rectangle. The centre arrow cross is hidden by styling its icon colours
// fully transparent through ADS's documented qproperty API — the invisible
// icons still hit-test, so docking BEHAVIOUR is completely unchanged; only
// the drag-time visuals differ.
class SankoDockOverlay : public QObject
{
public:
    explicit SankoDockOverlay(SankoDockManager *manager); // parent-owned by it

    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void paintHints(ads::CDockOverlay *overlay);
    void updateTrackingTimer(); // run only while an overlay is visible

    SankoDockManager *m_manager = nullptr;
    QTimer *m_tracker = nullptr; // repaints visible overlays so the glow
                                 // follows the cursor without flicker
};
