#pragma once

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QString>

// THE NOTE STYLE (Phase 1B, the user's decision 2026-10-25: option B of
// Figma 1B-5). Every non-modal note the app shows - the locks' refusals,
// the camera's notes, a panel removal's note - is a QToolTip at the
// pointer, and so is every hover tooltip. Without a style they were the
// plain Windows tooltip; with this one they all take the canvas chips'
// dark Sanko style: #161616 fill, a 1 px #2a2a2a border, radius 6, #dddddd
// text. NO font-family: the app's own UI font (Segoe UI on Windows) - Inter
// is neither bundled with the app nor installed, so naming it would only
// fall back (and bundling a font is a licensing decision, CLAUDE.md 13).
//
// Applied ONCE, from the MainWindow constructor, by APPENDING to the
// application's style sheet (main.cpp sets the scroll bars' before it):
// idempotent, so a second window or a test's second MainWindow adds
// nothing.
inline QString sankoNoteStyle()
{
    return QStringLiteral(
        "QToolTip{color:#dddddd;background-color:#161616;"
        "border:1px solid #2a2a2a;border-radius:6px;padding:4px 8px;}");
}

inline void applySankoNoteStyle()
{
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app)
        return;
    const QString style = sankoNoteStyle();
    if (!app->styleSheet().contains(style))
        app->setStyleSheet(app->styleSheet() + style);
}

// A GREYED-OUT MENU ENTRY (the user's rule, 2026-11-13): its label stays short - the
// entry's own name - and why it is off is its tooltip, shown when hovered. The menu is
// told to show tooltips; it shows only those set this way (an action's own tooltip,
// never its text), so no other entry gains one. An empty `why` takes it away again.
inline void sankoGreyWithReason(QMenu *menu, QAction *action, const QString &why, bool off = true)
{
    if (!action)
        return;
    action->setEnabled(!off);
    action->setToolTip(off ? why : QString());
    if (menu)
        menu->setToolTipsVisible(true);
}
