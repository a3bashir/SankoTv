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

// Header stylesheet fragments (the gradients a QColor cannot hold).
inline const QString kTransportSheet = QStringLiteral(
    "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #303042, stop:1 #272735); border: 1px solid #3e3d53;"
    " border-radius: 6px; }"
    "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #3a3a4f, stop:1 #303042); }"
    "QPushButton:pressed { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #272735, stop:1 #303042); }");
inline const QString kPlaySheet = QStringLiteral(
    "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #8060ef, stop:1 #6846db); border: 1px solid #8a6bf0;"
    " border-radius: 6px; }"
    "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #8d70f5, stop:1 #7555e5); }"
    "QPushButton:pressed { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #6846db, stop:1 #8060ef); }");
inline const QString kCollapseSheet = QStringLiteral(
    "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 #2e2e41, stop:1 #282839); border: 1px solid #3c3c52;"
    " border-radius: 4px; }"
    "QPushButton:hover { border-color: #7c6ef6; }");
// The icon-only loop buttons, and the speed buttons built in their style.
inline const QString kLoopButtonSheet = QStringLiteral(
    "QPushButton { background: #292b35; color: #cccccc; border: 1px solid #2a2a2a;"
    " border-radius: 6px; font-size: 12px; }"
    "QPushButton:hover { border-color: #4dff91; }");
inline const QString kSpeedButtonSheet = QStringLiteral(
    "QPushButton { background: #292b35; color: #cccccc; border: 1px solid #2a2a2a;"
    " border-radius: 6px; font-size: 12px; }"
    "QPushButton:hover { color: #7c6ef6; border-color: #7c6ef6; }");
inline const QString kSpeedButtonActiveSheet = QStringLiteral(
    "QPushButton { background: #7c6ef6; color: #0a0a0a; border: 1px solid #7c6ef6;"
    " border-radius: 6px; font-size: 12px; font-weight: 600; }");
// Fit / - / + keep the old fill and border; only the radius and height moved.
inline const QString kZoomButtonSheet = QStringLiteral(
    "QPushButton { background: #1a1a1a; color: #cccccc; border: 1px solid #2a2a2a;"
    " border-radius: 5px; font-size: 11px; }"
    "QPushButton:hover { border-color: #7c6ef6; color: #7c6ef6; }");
// Frames / Seconds / Snap: icon-only; checked = the accent fill.
inline const QString kViewButtonSheet = QStringLiteral(
    "QPushButton { background: transparent; border: 1px solid transparent;"
    " border-radius: 5px; }"
    "QPushButton:hover { border-color: #7c6ef6; }");
inline const QString kViewButtonCheckedSheet = QStringLiteral(
    "QPushButton { background: #7c6ef6; border: 1px solid #7c6ef6;"
    " border-radius: 5px; }");
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
