#pragma once

#include <QDialog>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QRect>
#include <QRegion>
#include <QScreen>

// Drag-by-header for the frameless studio dialogs (New Project, Project
// Settings). Behaviour only — no title bar, no resize grip, no chrome the
// design does not show. A press inside the dialog's HEADER BAND (the strip
// above the first field, where the section headers and their underline
// rules are painted) starts a move; presses on the body or on any control
// never do — a drag-from-anywhere dialog turns every mis-click into a
// window move. The dialog is kept entirely inside the available area of
// the screen it is on, so a drag toward an edge stops at the edge instead
// of leaving the artist with a half-visible window.
//
// Usage: hold one HeaderDrag as a member; forward mousePress/Move/Release
// from the dialog (child widgets consume their own presses first, so only
// background presses ever reach these).
class HeaderDrag
{
public:
    // Returns true when the press landed in `headerBand` (dialog-local) and
    // a drag began.
    bool press(QDialog *dialog, const QMouseEvent *event, const QRect &headerBand)
    {
        if (event->button() != Qt::LeftButton
            || !headerBand.contains(event->position().toPoint())) {
            m_active = false;
            return false;
        }
        m_active = true;
        m_grab = event->globalPosition().toPoint() - dialog->frameGeometry().topLeft();
        return true;
    }

    // "Fully visible" across SEVERAL screens means pixel-wise coverage:
    // every pixel of the frame lies on some screen's available area. That is
    // the only definition right in every arrangement — it lets the frame
    // straddle a full-height shared edge freely (no jump), refuses the dead
    // space of mismatched heights or an L-shape, and never trusts a single
    // rectangle (the first version clamped to the CURRENT screen's available
    // geometry, which made every shared edge a wall: a drag toward the other
    // monitor stopped at the boundary and could never cross).
    static bool fullyCovered(const QRect &frame)
    {
        QRegion covered;
        for (const QScreen *s : QGuiApplication::screens())
            covered += s->availableGeometry();
        return (QRegion(frame) - covered).isEmpty();
    }

    bool move(QDialog *dialog, const QMouseEvent *event)
    {
        if (!m_active)
            return false;
        const QPoint globalPos = event->globalPosition().toPoint();
        QPoint target = globalPos - m_grab;
        const QSize size = dialog->frameGeometry().size();
        if (!fullyCovered(QRect(target, size))) {
            // Not fully visible there. Clamp into the screen the POINTER is
            // on (so a drag that has crossed to another monitor lands there
            // — on mismatched heights this is the one bounded snap, where
            // the geometry forces it); if the pointer is over no screen at
            // all (dead space, off the desktop), fall back to the dialog's
            // current screen, and to the primary if even that is gone (a
            // monitor disconnected under the dialog). Never a null screen.
            const QScreen *screen = QGuiApplication::screenAt(globalPos);
            if (!screen)
                screen = dialog->screen();
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            if (screen) {
                const QRect avail = screen->availableGeometry();
                target.setX(qBound(avail.left(), target.x(),
                                   qMax(avail.left(), avail.right() - size.width() + 1)));
                target.setY(qBound(avail.top(), target.y(),
                                   qMax(avail.top(), avail.bottom() - size.height() + 1)));
            }
        }
        dialog->move(target);
        return true;
    }

    bool release()
    {
        const bool was = m_active;
        m_active = false;
        return was;
    }

    bool active() const { return m_active; }

private:
    bool m_active = false;
    QPoint m_grab;
};
