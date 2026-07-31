#pragma once

#include "BrushPreset.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVector>

namespace brushlib {

// The brush library: built-in roster + user presets + shelf state
// (favourites, hidden built-ins, most-recently-used, library name).
//
// APPLICATION state, never DOCUMENT state: nothing here touches the
// app-wide QUndoStack, composite caches, Layer::image, or document dirty
// flags. User presets live as one .sankobrush file each under the app-data
// BrushLibrary folder; shelf state lives in versioned QSettings keys
// (brushLibrary/v1/*). The built-ins themselves ship in code
// (BuiltinRoster.cpp) — deleting a built-in HIDES it, restore brings the
// stock roster back.
class BrushLibraryModel : public QObject
{
    Q_OBJECT
public:
    explicit BrushLibraryModel(QObject *parent = nullptr);

    static QStringList categories(); // fixed order; excludes "Recent"

    // Hidden built-ins filtered out; built-ins first (roster order), then
    // user presets by name.
    QVector<const BrushPreset *> presetsIn(const QString &category) const;
    QVector<const BrushPreset *> recentPresets() const; // MRU, newest first
    const BrushPreset *preset(const QString &id) const;

    void recordUsage(const QString &id); // MRU front, capped, persisted

    bool isFavourite(const QString &id) const;
    void setFavourite(const QString &id, bool on);

    bool isHidden(const QString &id) const;
    void setBuiltinHidden(const QString &id, bool hidden); // "delete"
    // Unhide all built-ins, drop renames AND brush overrides (stock roster).
    void restoreDefaultBrushes();

    // User preset CRUD. addUserPreset assigns a fresh user id, writes the
    // .sankobrush file, and returns the id (empty on failure).
    QString addUserPreset(BrushPreset preset);
    bool removeUserPreset(const QString &id); // deletes the file
    bool renamePreset(const QString &id, const QString &name);
    QString duplicatePreset(const QString &id); // -> user copy "<name> Copy"
    // Studio "Done": replace a preset's brush. User presets rewrite their
    // file; BUILT-INS are never destroyed — the edit lands as an OVERRIDE
    // file (Overrides/ next to the user presets) applied over the in-code
    // roster at load, so Restore Default Brushes always recovers stock.
    bool updateBrush(const QString &id, const ::Brush &brush);
    bool hasBuiltinOverride(const QString &id) const;

    QString libraryName() const;
    void setLibraryName(const QString &name);

    // Whole-library export: every visible preset (built-in and user) into
    // one .sankobrushset bundle. Import walks the BrushImporter registry;
    // returns the number of presets added (as user presets).
    bool exportLibrary(const QString &path) const;
    int importFile(const QString &path);

    QString userPresetDir() const;

signals:
    void changed(); // any roster/shelf mutation (coarse; the UI re-lists)

private:
    void loadUserPresets();
    void loadShelfState();
    void loadBuiltinOverrides();
    void saveShelfList(const char *key, const QStringList &list) const;
    bool writeUserPresetFile(const BrushPreset &preset) const;
    QString presetFilePath(const QString &id) const;
    QString overrideDir() const;
    QString overrideFilePath(const QString &id) const;

    QVector<BrushPreset> m_presets; // built-ins then user presets
    QHash<QString, int> m_byId;
    QStringList m_favourites;
    QStringList m_hidden;
    QStringList m_recent;
    QHash<QString, QString> m_builtinRenames;
    QStringList m_overridden; // built-in ids with a brush override on disk
    QString m_libraryName;
};

} // namespace brushlib
