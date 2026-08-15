#pragma once

#include <QColor>
#include <QString>

// THE Sanko colour tokens — the single place an accent value lives, in the
// same shared-header pattern as SankoScrollBarStyle.h.
//
// kPurple is the Sanko tone (#7C6EF6) — the accent every Figma design
// specifies. kAccent is "the app's chrome accent", and it now RESOLVES TO
// kPurple: the legacy amber #F5A623 that predated the Figma designs is
// retired. The two names stay distinct because they answer different
// questions — kPurple is "what is the Sanko colour", kAccent is "what does
// chrome highlight with" — so a future re-accent is still one line here.
//
// NOT tokens (deliberately): colours that MEAN something are content, not
// chrome, and keep their literals at the site —
//   * the panel tag palette ("Amber" and "Violet" are user-facing choices)
//     and the layer colour-tag picker's default seed,
//   * the Generation page's status set (blue queued / amber generating /
//     green complete / red failed): badge, dot, and spinner,
//   * the Consistency Board's type badges (amber Character / blue Location),
//   * the canvas action-safe mask, which stays amber specifically so it can
//     never be confused with the purple perspective/selection overlays —
//     the one place the two colours must remain distinguishable,
//   * the grid-colour menu's "Sanko Accent" entry, a named choice.
namespace SankoTheme {

inline const QColor kPurple(0x7c, 0x6e, 0xf6);
inline const QString kPurpleHex = QStringLiteral("#7c6ef6");

inline const QColor kAccent = kPurple;
inline const QString kAccentHex = kPurpleHex;
inline const QString kAccentRgb = QStringLiteral("124,110,246");

// The LEGIBILITY TINT: the Sanko purple lifted 26% toward white. Amber was
// a light colour (relative luminance 0.46) and purple is a dark one (0.22),
// so accent marks that used to read easily on a dark ground lose roughly
// half their contrast in the swap. This tint restores it wherever the base
// accent falls under the thresholds — 4.5:1 for text, 3:1 for a border or
// indicator that must read as a STATE. Measured against the grounds it is
// actually used on:
//     #111111 dock body      7.23:1      #161616 dock tab      6.93:1
//     #262626 close hover    5.80:1      #2a2766 clip fill     5.09:1
//     #3d3894 selected clip  3.67:1  (state border, 3:1 floor)
// Chrome that sits on near-black and already clears its threshold keeps
// kAccent — the tint is a legibility tool, not a second brand colour.
inline const QColor kAccentLight(0x9e, 0x94, 0xf8);
inline const QString kAccentLightHex = QStringLiteral("#9e94f8");

// Stylesheet templates carry %ACCENT% / %ACCENT_RGB% / %ACCENT_LIGHT% /
// %PURPLE% and are resolved here, so a stylesheet never embeds an accent
// literal. Longest placeholders are replaced first so none can shadow
// another by prefix.
inline QString themed(const char *qss)
{
    QString s = QString::fromLatin1(qss);
    s.replace(QLatin1String("%ACCENT_LIGHT%"), kAccentLightHex);
    s.replace(QLatin1String("%ACCENT_RGB%"), kAccentRgb);
    s.replace(QLatin1String("%ACCENT%"), kAccentHex);
    s.replace(QLatin1String("%PURPLE%"), kPurpleHex);
    return s;
}

} // namespace SankoTheme
