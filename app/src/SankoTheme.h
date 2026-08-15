#pragma once

#include <QColor>
#include <QString>

// THE Sanko colour tokens — the single place an accent value lives, in the
// same shared-header pattern as SankoScrollBarStyle.h.
//
// kAccent is the app's chrome accent. It is currently the LEGACY AMBER and
// exists precisely so it can be retired: commit 1 routed every amber chrome
// literal through this token with no visual change; commit 2 flips the two
// values below to the Sanko purple, and every accent surface follows.
//
// kPurple is the Sanko tone (#7C6EF6) — the accent every Figma design
// specifies and all brushlib-era work already uses. After the flip the two
// tokens are equal; kPurple stays as the named design token and kAccent as
// "the chrome accent", so a hypothetical future re-accent is still one line.
//
// NOT tokens (deliberately): colours that MEAN something are content, not
// chrome, and keep their literals at the site —
//   * the panel tag palette ("Amber" is a user-facing choice) and the layer
//     colour-tag picker's default seed,
//   * the Generation page's status set (blue queued / amber generating /
//     green complete / red failed): badge, dot, and spinner,
//   * the Consistency Board's type badges (amber Character / blue Location),
//   * the canvas action-safe mask, which is amber specifically so it can
//     never be confused with the purple perspective/selection overlays.
namespace SankoTheme {

inline const QColor kAccent(0xf5, 0xa6, 0x23);
inline const QString kAccentHex = QStringLiteral("#f5a623");
inline const QString kAccentRgb = QStringLiteral("245,166,35"); // rgba() form

inline const QColor kPurple(0x7c, 0x6e, 0xf6);
inline const QString kPurpleHex = QStringLiteral("#7c6ef6");

// Stylesheet templates carry %ACCENT% / %ACCENT_RGB% / %PURPLE% and are
// resolved here, so a stylesheet never embeds an accent literal. The
// placeholders are chosen so none is a prefix of another.
inline QString themed(const char *qss)
{
    QString s = QString::fromLatin1(qss);
    s.replace(QLatin1String("%ACCENT_RGB%"), kAccentRgb);
    s.replace(QLatin1String("%ACCENT%"), kAccentHex);
    s.replace(QLatin1String("%PURPLE%"), kPurpleHex);
    return s;
}

} // namespace SankoTheme
