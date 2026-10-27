#pragma once

#include <QColor>
#include <QString>

// THE TIMELINE'S LOOK, from the user's Figma redesign (Dasktop_UI_test, page
// "SankoTV App", node 380:644, read through the connector on 2026-10-18).
// Every value here is the design's own; the names are its layer roles, so
// a colour changed in Figma maps to exactly one line here. The accent is
// still SankoTheme::kAccent (the Seconds / Snap checked fill, the active
// speed button) - this header holds only what the redesign added.
namespace SankoTimelineTheme {

// Surfaces.
inline const QColor kPageBg(0x10, 0x11, 0x15);     // the section and the shots track
inline const QColor kHeaderBg(0x1c, 0x1e, 0x25);   // the header row and the scenes track
inline const QColor kRulerBg(0x18, 0x1a, 0x21);    // the ruler and the empty audio row
inline const QColor kHairline(0x29, 0x2b, 0x35);   // header border-bottom, ruler ticks,
                                                   // thumbnail / pill borders
inline const QColor kLabelColumnBg(0x0d, 0x0d, 0x0d);
inline const QColor kLabelColumnSeparator(0x2a, 0x2a, 0x2a);

// Text and icons.
inline const QColor kTrackLabel(0xb1, 0xb2, 0xbf);   // TIMECODE / SCENES / SHOTS / AUDIO
inline const QColor kTrackControl(0xa4, 0xa7, 0xae); // lock / speaker / eye icons
inline const QColor kTrackControlActive(0x7c, 0x6e, 0xf6); // ...when locked / hidden / muted (the accent fill's colour)
inline const QColor kRulerLabel(0xb0, 0xb2, 0xc2);
inline const QColor kSceneName(0xb7, 0xb3, 0xc7);
inline const QColor kClipNumber(0xf4, 0xf1, 0xff);
inline const QColor kClipName(0xd3, 0xd0, 0xdf);
inline const QColor kPillText(0xe5, 0xe5, 0xed);
inline const QColor kAudioHint(0x99, 0x9b, 0xa9);
inline const QColor kTimecode(0xa2, 0x94, 0xff);
inline const QColor kTotal(0xb4, 0xb6, 0xc9);
inline const QColor kTimecodeSeparator(0x37, 0x3a, 0x4b);
inline const QColor kTransportGlyph(0xf2, 0xf1, 0xfa);
inline const QColor kViewIcon(0xcc, 0xcc, 0xd2);     // Frames / Seconds / Snap, unchecked
inline const QColor kViewIconChecked(0xff, 0xff, 0xff);
inline const QColor kLoopIcon(0xe5, 0xe5, 0xed);
inline const QColor kCollapseGlyph(0xd9, 0xd9, 0xd9);

// Tracks.
inline const QColor kSceneBlock(0x20, 0x20, 0x29);
inline const QColor kSceneBaseline(0x2a, 0x27, 0x66);
inline const QColor kSceneSeparator(0x44, 0x44, 0x44);
inline const QColor kClipBg(0x25, 0x23, 0x30);
inline const QColor kClipBorder(0x3b, 0x37, 0x4b);
inline const QColor kClipSelectedBg(0x30, 0x2a, 0x48);
inline const QColor kClipSelectedBorder(0x97, 0x87, 0xff);
inline const QColor kThumbPlaceholder(0x4a, 0x4a, 0x4a); // a panel with no artwork
inline const QColor kPillBg(0x17, 0x17, 0x1f);
inline const QColor kPlayhead(0xd5, 0xcb, 0xff);

// Header stylesheet fragments. FLAT CONTROLS (the user's rule, Part D,
// 2026-10-23; Figma proposal frame 424:390): every icon sits directly on
// the header - no squares, no borders. Hover: a rounded highlight of the
// accent #7c6ef6 at 14 % behind it; pressed 26 %; checked (Frames /
// Seconds / Snap) the accent at 100 % with a white icon. Play keeps its
// purple fill as the main action. No green anywhere: every hover is the
// accent (the loop buttons and the speed dropdown used #4dff91 before).
inline const QString kFlatHoverBg = QStringLiteral("rgba(124, 110, 246, 0.14)");
inline const QString kFlatPressedBg = QStringLiteral("rgba(124, 110, 246, 0.26)");
inline const QString kTransportSheet = QStringLiteral(
    "QPushButton { background: transparent; border: none; border-radius: 6px; }"
    "QPushButton:hover { background: rgba(124, 110, 246, 0.14); }"
    "QPushButton:pressed { background: rgba(124, 110, 246, 0.26); }");
inline const QString kPlaySheet = QStringLiteral(
    "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #8060ef, stop:1 #6846db); border: none; border-radius: 6px; }"
    "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #8f72ff, stop:1 #7a58e6); }"
    "QPushButton:pressed { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #6f52d6, stop:1 #5a3bc4); }");
inline const QString kCollapseSheet = QStringLiteral(
    "QPushButton { background: transparent; border: none; border-radius: 5px; }"
    "QPushButton:hover { background: rgba(124, 110, 246, 0.14); }"
    "QPushButton:pressed { background: rgba(124, 110, 246, 0.26); }");
// The icon-only loop buttons (the clear-loop button keeps its own red hover).
inline const QString kLoopButtonSheet = QStringLiteral(
    "QPushButton { background: transparent; color: #cccccc; border: none;"
    " border-radius: 5px; font-size: 12px; }"
    "QPushButton:hover { background: rgba(124, 110, 246, 0.14); }"
    "QPushButton:pressed { background: rgba(124, 110, 246, 0.26); }");
// The speed dropdown (Figma proposal frame 417:390, flat since 424:390):
// text left, the chevron painted by the button itself (SpeedButton in
// AnimaticPage.cpp - the stylesheet's menu-indicator drew it wrong), no
// border at rest; hover = accent text, chevron and a 1 px accent border.
// font-size in POINTS, 8.25pt = 11 px at 96 dpi (pixel-identical, probe
// build/_probe/polish/fontprobe): Qt 6.11's Windows 11 style sizes a tool
// button's menu indicator from the font's point size, and a font set in
// pixels has none (-1) - "QFont::setPointSize: Point size <= 0 (-1)" when
// the pointer left this button (2026-10-27).
inline const QString kSpeedDropdownSheet = QStringLiteral(
    "QToolButton { background: transparent; color: #e5e5ed; border: 1px solid transparent;"
    " border-radius: 6px; font-size: 8.25pt; padding: 0 30px 0 6px; }"
    "QToolButton:hover { color: #7c6ef6; border-color: #7c6ef6; }"
    "QToolButton:pressed { background: rgba(124, 110, 246, 0.26); }"
    "QToolButton::menu-indicator { image: none; width: 0px; height: 0px; }");
inline const QString kSpeedMenuSheet = QStringLiteral(
    "QMenu { background: #161616; color: #cccccc; border: 1px solid #2a2a2a;"
    " border-radius: 4px; padding: 4px 0; }"
    "QMenu::item { padding: 4px 18px 4px 24px; font-size: 11px; }"
    "QMenu::item:selected { background: #262626; color: #ffffff; }"
    "QMenu::item:checked { color: #7c6ef6; }"
    "QMenu::indicator { width: 12px; height: 12px; left: 6px; }"
    "QMenu::indicator:checked { image: url(:/icons/timeline/speed_tick.svg); }");
// Fit / - / +: text only; hover = accent text on the highlight.
inline const QString kZoomButtonSheet = QStringLiteral(
    "QPushButton { background: transparent; color: #cccccc; border: none;"
    " border-radius: 5px; font-size: 11px; }"
    "QPushButton:hover { background: rgba(124, 110, 246, 0.14); color: #7c6ef6; }"
    "QPushButton:pressed { background: rgba(124, 110, 246, 0.26); color: #7c6ef6; }");
// The zoom field (Part D; Figma proposal frame 423:476): the design's
// 46 x 28 #333333 radius 5, the percentage centred in 11 px #cccccc; while
// editing a 1 px accent border and white text.
inline const QString kZoomFieldSheet = QStringLiteral(
    "QLineEdit { background: #333333; color: #cccccc; border: 1px solid #333333;"
    " border-radius: 5px; font-size: 11px; padding: 0; }");
inline const QString kZoomFieldEditSheet = QStringLiteral(
    "QLineEdit { background: #333333; color: #ffffff; border: 1px solid #7c6ef6;"
    " border-radius: 5px; font-size: 11px; padding: 0;"
    " selection-background-color: rgba(124, 110, 246, 0.35); selection-color: #ffffff; }");
// Frames / Seconds / Snap: icon-only; checked = the accent fill.
inline const QString kViewButtonSheet = QStringLiteral(
    "QPushButton { background: transparent; border: none; border-radius: 5px; }"
    "QPushButton:hover { background: rgba(124, 110, 246, 0.14); }"
    "QPushButton:pressed { background: rgba(124, 110, 246, 0.26); }");
inline const QString kViewButtonCheckedSheet = QStringLiteral(
    "QPushButton { background: #7c6ef6; border: none; border-radius: 5px; }");
// The audio warning (Part D; Figma proposal frame 425:390): a flat 28 x 28
// hit area for the 16 px warning glyph, the message as its tooltip.
inline const QString kWarningButtonSheet = QStringLiteral(
    "QToolButton { background: transparent; border: none; border-radius: 5px; }"
    "QToolButton:hover { background: rgba(124, 110, 246, 0.14); }");
// The Audio track's volume popup (Part D; the user's drawing in node
// 380:644 and proposal frame 423:492): the zoom slider's look upright - a
// 10 px groove, the accent gradient filling from the bottom, a 10 x 16
// dragger - rising from the speaker inside the label column.
inline const QString kVolumeSliderSheet = QStringLiteral(
    "QSlider { background: transparent; }"
    "QSlider::groove:vertical { width: 10px; background: #333333;"
    " border-radius: 2px; margin: 0; }"
    "QSlider::add-page:vertical { background: qlineargradient(x1:0, y1:1,"
    " x2:0, y2:0, stop:0 #4b4397, stop:1 #7c6ef6); border-radius: 1px; }"
    "QSlider::sub-page:vertical { background: transparent; }"
    "QSlider::handle:vertical { width: 10px; height: 16px; margin: 0;"
    " background: #b3b3b3; border-radius: 1px; }");
// The zoom slider: a 10 px groove, the filled part in the accent gradient,
// a 20 x 10 dragger. (The design's extra white 10 % -> 0 % sheen on the
// filled part is not expressible in a stylesheet; noted in HANDOFF.)
inline const QString kZoomSliderSheet = QStringLiteral(
    "QSlider { background: transparent; }"
    "QSlider::groove:horizontal { height: 10px; background: #333333;"
    " border-radius: 2px; margin: 0; }"
    "QSlider::sub-page:horizontal { background: qlineargradient(x1:0, y1:0,"
    " x2:1, y2:0, stop:0 #4b4397, stop:1 #7c6ef6); border-radius: 1px; }"
    "QSlider::add-page:horizontal { background: transparent; }"
    "QSlider::handle:horizontal { width: 20px; height: 10px; margin: 0;"
    " background: #b3b3b3; border-radius: 1px; }");

} // namespace SankoTimelineTheme
