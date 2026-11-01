#pragma once

#include <QString>

// THE one scrollbar style (the Brush Library panel's design, generalized):
// a 10 px transparent-track bar with a 6 px #2f2f2f rounded handle that
// brightens on hover, no stepper buttons, no page background. Installed
// APP-WIDE from main.cpp via qApp->setStyleSheet — the selectors are scoped
// strictly to QScrollBar and its subcontrols, so unlike a bare QWidget rule
// this cannot cascade onto anything that is not a scrollbar (the
// StoryboardPage background-color trap). Hosts with special geometry pass a
// bottom margin — the Brush Library keeps 12 px so the bar clears its
// rounded bottom-right arc — and apply the result to their own scrollbar,
// where widget-sheet precedence overrides the app default.
//
// The :disabled handles are transparent so a Qt::ScrollBarAlwaysOn bar
// (the studio's properties column reserves its gutter permanently to keep
// content width constant) reads as plain margin when a section is short
// enough not to scroll.
inline QString sankoScrollBarStyle(int bottomMargin = 2)
{
    return QStringLiteral(
               "QScrollBar:vertical { background: transparent; width: 10px;"
               " margin: 2px 2px %1px 2px; }"
               "QScrollBar::handle:vertical { background: #2f2f2f;"
               " border-radius: 3px; min-height: 24px; }"
               "QScrollBar::handle:vertical:hover { background: #3d3d3d; }"
               "QScrollBar::handle:vertical:disabled {"
               " background: transparent; }"
               "QScrollBar:horizontal { background: transparent;"
               " height: 10px; margin: 2px 2px 2px 2px; }"
               "QScrollBar::handle:horizontal { background: #2f2f2f;"
               " border-radius: 3px; min-width: 24px; }"
               "QScrollBar::handle:horizontal:hover { background: #3d3d3d; }"
               "QScrollBar::handle:horizontal:disabled {"
               " background: transparent; }"
               "QScrollBar::add-line, QScrollBar::sub-line {"
               " width: 0; height: 0; }"
               "QScrollBar::add-page, QScrollBar::sub-page {"
               " background: transparent; }")
        .arg(bottomMargin);
}

// THE one text-selection colour (2026-11-01, the user's rule after the 2B
// and 2C hand tests: the Depth popover's field and the Layers rename field
// selected in Windows' system red): every text field in the app selects in
// the accent, white text on #7c6ef6, set ONCE here. Scoped to the editable
// text widgets and the editor inside a spin box or an editable combo, so
// nothing else is restyled. A widget's own sheet still wins where it sets
// these properties - none does any more; the two that set the accent
// themselves (the script editor, the Shot Info fields) agree with it.
inline QString sankoSelectionStyle()
{
    return QStringLiteral(
        "QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox, QComboBox {"
        " selection-background-color: #7c6ef6; selection-color: #ffffff; }");
}

// Everything main.cpp installs app-wide, in one string: the scrollbars and
// the selection colour. (The checks apply this same function to the test
// application, so what they measure is what the app installs.)
inline QString sankoAppStyle()
{
    return sankoScrollBarStyle() + sankoSelectionStyle();
}
