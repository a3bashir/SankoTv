#pragma once

#include "SankoIcons.h"
#include "SankoTimelineTheme.h"

#include <QIcon>
#include <QString>
#include <QToolButton>

// ONE lock and ONE eye across SankoTV (approved 2026-10-30, Figma "PROPOSAL
// 2026-10-30 — One lock and one eye across SankoTV"): the Timeline's track
// controls, and the Layers panel's rows take the same. A 16 px icon in a
// 20 px flat checkable button, the hover a #1e1e26 rounded-4 square behind
// the icon, pointing-hand cursor, no focus; one click toggles.
//   lock: unchecked the OPEN padlock in the column grey, checked (locked)
//         the CLOSED padlock in the accent;
//   eye:  unchecked (shown) the eye in the column grey, checked (hidden) the
//         same eye at 35 %.
// What a click MEANS stays each owner's: a layer's lock and visibility are
// Undo steps, a track's are view state.
namespace SankoToggleControls {

constexpr int kIconSize = 16;
constexpr int kButtonSize = kIconSize + 4;

inline QColor dimmed(const QColor &c)
{
    QColor d = c;
    d.setAlphaF(0.35);
    return d;
}

// Off / On renderings at 1x and 2x (a QIcon needs both on mixed screens).
inline QIcon stateIcon(const QString &offFile, const QColor &offTint,
                       const QString &onFile, const QColor &onTint)
{
    QIcon icon;
    for (qreal scale : {1.0, 2.0}) {
        icon.addPixmap(SankoIcons::renderSvg(offFile, QSize(kIconSize, kIconSize), scale, offTint),
                       QIcon::Normal, QIcon::Off);
        icon.addPixmap(SankoIcons::renderSvg(onFile, QSize(kIconSize, kIconSize), scale, onTint),
                       QIcon::Normal, QIcon::On);
    }
    return icon;
}

inline QIcon lockIcon()
{
    using namespace SankoTimelineTheme;
    return stateIcon(QStringLiteral(":/icons/timeline/track_lock.svg"), kTrackControl,
                     QStringLiteral(":/icons/timeline/track_lock_closed.svg"), kTrackControlActive);
}

inline QIcon eyeIcon()
{
    using namespace SankoTimelineTheme;
    const QString eye = QStringLiteral(":/icons/timeline/track_eye.svg");
    return stateIcon(eye, kTrackControl, eye, dimmed(kTrackControl));
}

// The button every lock, eye and track toggle sits in.
inline QToolButton *makeToggle(QWidget *parent, const QString &name, const QIcon &icon)
{
    auto *b = new QToolButton(parent);
    b->setObjectName(name);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    b->setIcon(icon);
    b->setIconSize(QSize(kIconSize, kIconSize));
    b->setFixedSize(kButtonSize, kButtonSize);
    b->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; background: transparent; padding: 0; }"
        "QToolButton:hover { background: #1e1e26; border-radius: 4px; }"));
    return b;
}

} // namespace SankoToggleControls
