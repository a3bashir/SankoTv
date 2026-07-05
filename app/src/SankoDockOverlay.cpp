#include "SankoDockOverlay.h"

#include "DockManager.h"
#include "DockOverlay.h"

#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QTimer>

namespace {

constexpr int kGlowBand = 18;  // total width of the edge glow band
constexpr int kTabStripH = 28; // height of the tab-bar highlight strip

// Solid ~4px core at 70% opacity fading to fully transparent across the band.
void addGlowStops(QLinearGradient &gradient)
{
    const QColor amber(0xf5, 0xa6, 0x23);
    QColor core = amber;
    core.setAlphaF(0.70);
    QColor mid = amber;
    mid.setAlphaF(0.30);
    QColor clear = amber;
    clear.setAlphaF(0.0);
    gradient.setColorAt(0.0, core);
    gradient.setColorAt(4.0 / kGlowBand, core); // ~4px solid bar
    gradient.setColorAt(0.55, mid);
    gradient.setColorAt(1.0, clear);
}

} // namespace

SankoDockOverlay::SankoDockOverlay(SankoDockManager *manager)
    : QObject(manager)
    , m_manager(manager)
{
    // ~30fps repaint of the visible overlay: the hint follows the cursor
    // smoothly even if ADS itself doesn't repaint on every mouse move.
    m_tracker = new QTimer(this);
    m_tracker->setInterval(33);
    QObject::connect(m_tracker, &QTimer::timeout, this, [this] {
        if (m_manager->containerOverlay()->isVisible())
            m_manager->containerOverlay()->update();
        if (m_manager->dockAreaOverlay()->isVisible())
            m_manager->dockAreaOverlay()->update();
    });

    // Hide the centre arrow crosses. Each CDockOverlayCross is a Qt::Tool
    // top-level window parented to the MANAGER (not the overlay), with its
    // icon pixmaps baked at construction — so stylesheet recolouring can't
    // reach them reliably. Window opacity 0 makes them fully invisible while
    // their icon geometry keeps hit-testing, so the docking behaviour
    // (centre/edge targets, tabbing) is untouched. Discovered by class name:
    // no ADS symbols beyond the exported CDockManager/CDockOverlay are used.
    const QList<QWidget *> children = m_manager->findChildren<QWidget *>();
    for (QWidget *child : children) {
        if (qstrcmp(child->metaObject()->className(), "ads::CDockOverlayCross") == 0)
            child->setWindowOpacity(0.0);
    }

    const auto overlays = {m_manager->containerOverlay(), m_manager->dockAreaOverlay()};
    for (ads::CDockOverlay *overlay : overlays)
        overlay->installEventFilter(this);
}

bool SankoDockOverlay::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::Paint:
        if (auto *overlay = qobject_cast<ads::CDockOverlay *>(watched)) {
            paintHints(overlay);
            return true; // suppress ADS's filled drop-preview rectangle
        }
        break;
    case QEvent::Show:
    case QEvent::Hide:
        updateTrackingTimer();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void SankoDockOverlay::updateTrackingTimer()
{
    const bool anyVisible = m_manager->containerOverlay()->isVisible()
                            || m_manager->dockAreaOverlay()->isVisible();
    if (anyVisible) {
        if (!m_tracker->isActive())
            m_tracker->start();
    } else {
        m_tracker->stop();
    }
}

// Renders the drag-time hint for the drop area currently under the cursor,
// using ADS's PUBLIC hit-testing so the visual always matches where the
// panel would actually land.
void SankoDockOverlay::paintHints(ads::CDockOverlay *overlay)
{
    // Respects ADS's internal drop-allowed logic (returns Invalid when the
    // overlay is hidden or the drop preview is disabled).
    const ads::DockWidgetArea area = overlay->visibleDropAreaUnderCursor();
    if (area == ads::InvalidDockWidgetArea)
        return;

    // NOTE: exactly ONE QPainter per paint pass. On this translucent overlay
    // window a second sequential painter fails to begin silently, so all
    // rendering below shares this painter.
    QPainter p(overlay);
    const QRect r = overlay->rect();

    switch (area) {
    case ads::LeftDockWidgetArea: {
        QLinearGradient g(QPointF(r.left(), 0), QPointF(r.left() + kGlowBand, 0));
        addGlowStops(g);
        p.fillRect(QRect(r.left(), r.top(), kGlowBand, r.height()), g);
        break;
    }
    case ads::RightDockWidgetArea: {
        QLinearGradient g(QPointF(r.right() + 1, 0), QPointF(r.right() + 1 - kGlowBand, 0));
        addGlowStops(g);
        p.fillRect(QRect(r.right() + 1 - kGlowBand, r.top(), kGlowBand, r.height()), g);
        break;
    }
    case ads::TopDockWidgetArea: {
        QLinearGradient g(QPointF(0, r.top()), QPointF(0, r.top() + kGlowBand));
        addGlowStops(g);
        p.fillRect(QRect(r.left(), r.top(), r.width(), kGlowBand), g);
        break;
    }
    case ads::BottomDockWidgetArea: {
        QLinearGradient g(QPointF(0, r.bottom() + 1), QPointF(0, r.bottom() + 1 - kGlowBand));
        addGlowStops(g);
        p.fillRect(QRect(r.left(), r.bottom() + 1 - kGlowBand, r.width(), kGlowBand), g);
        break;
    }
    case ads::CenterDockWidgetArea: {
        // Dock-as-tab: highlight ONLY the target's tab-bar strip, not the
        // whole panel — an amber wash plus a crisp underline.
        const QRect strip(r.left(), r.top(), r.width(), kTabStripH);
        const QColor amber(0xf5, 0xa6, 0x23);
        QColor wash = amber;
        wash.setAlphaF(0.25);
        p.fillRect(strip, wash);
        QColor line = amber;
        line.setAlphaF(0.85);
        p.fillRect(QRect(strip.left(), strip.bottom() - 1, strip.width(), 2), line);
        break;
    }
    default:
        break; // auto-hide/sidebar targets: leave unhinted
    }
}
