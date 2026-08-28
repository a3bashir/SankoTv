#pragma once

#include <QSettings>
#include <QString>

// THE one way application code opens its settings store.
//
// Why a choke point exists: the two-argument QSettings("SankoTV","SankoTV")
// form ignores BOTH QSettings::setDefaultFormat and the application-level
// organization name, so every such site silently bypassed the test
// families' scratch redirection (setDefaultFormat(IniFormat) + setPath only
// govern the DEFAULT-constructed QSettings). Thirty sites carried the
// pattern; eleven were writers. Nothing had leaked into the real store only
// because tests happened not to drive those writers — and on 2026-08-27 the
// same trap DID bite: a measurement probe redirected the Dev Recorder's
// output through app-level settings, the recorder read the two-argument
// form instead, and two sessions of synthetic strokes landed in the user's
// real recordings folder. The scratch rule must hold without anyone
// checking; this header is how.
//
// The store is EXACTLY what the two-argument form opened - org "SankoTV",
// app "SankoTV", native format (the registry on Windows) - so existing
// user data stays where it is. Deliberately NOT the app-level org name:
// main.cpp says org "Sanko", and the app is split-brained across the two
// (settings under "SankoTV", QStandardPaths data such as the brush shelf
// under "Sanko"). Renaming either side means migrating user data; that is
// recorded in HANDOFF as its own decision and is not this header's job.
//
// Tests call sankoSettingsSetOverrideForTest(<scratch>.ini) FIRST; every
// sankoSettings() thereafter reads and writes that INI instead, and the
// real store is provably untouched (asserted by the lifecycle family,
// which snapshots the real store's keys before running and compares after).
//
// QSettings is a QObject and cannot be copied; returning by value is legal
// because a returned prvalue constructs in place (C++17 guaranteed
// elision) - the same pattern recentSettings() and shelfSettings() already
// use. Consume as `QSettings s = sankoSettings();` or call through the
// temporary: `sankoSettings().setValue(...)`.

inline QString &sankoSettingsOverridePath()
{
    static QString path;
    return path;
}

inline void sankoSettingsSetOverrideForTest(const QString &iniPath)
{
    sankoSettingsOverridePath() = iniPath;
}

inline QSettings sankoSettings()
{
    const QString &override = sankoSettingsOverridePath();
    if (!override.isEmpty())
        return QSettings(override, QSettings::IniFormat);
    return QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
}
