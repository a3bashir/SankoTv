#pragma once

#include "BrushPreset.h"

#include <QHash>
#include <QObject>
#include <QSettings>
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
// DISK-FIRST WRITE ORDERING (phase 5 defect D3): every operation that
// persists to a preset file writes the file FIRST — atomically, via
// QSaveFile (write-to-temp-then-rename, so a crash mid-write can never
// leave a truncated preset) — and commits to memory only on success. On
// failure, memory is untouched and the operation reports false, so what the
// UI shows is always what a restart will reload. Shelf state (favourites,
// hidden, MRU, renames of built-ins, library name) is registry-backed
// QSettings, which has no detectable failure path; those keep their
// original write ordering.
class BrushLibraryModel : public QObject
{
    Q_OBJECT
public:
    // rootOverride: tests point the preset store at a scratch directory
    // (the same pattern as BrushPreviewRenderer's cacheRootOverride); empty
    // uses the app-data BrushLibrary folder. The phase 5 seam destroyed two
    // user presets because it had to share the REAL library directory —
    // with an injectable root, a test never touches user files at all.
    explicit BrushLibraryModel(QObject *parent = nullptr,
                               const QString &rootOverride = QString());

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

    // --- Override state (discoverability + the stale hazard) --------------
    // None: no override, OR an override byte-equal to current stock
    //       (harmless residue - no mark).
    // Modified: override diverges from stock, and stock has NOT moved since
    //       the override was saved (the stored stock fingerprint matches).
    // StockChanged: THE HAZARD - the override shadows a recipe that has
    //       moved underneath it; the user cannot see the improvement.
    // Unknown: an override with NO stored fingerprint (saved before this
    //       machinery existed). Deliberately NOT defaulted to a reassuring
    //       state: the app cannot actually support that answer.
    enum class OverrideState { None, Modified, StockChanged, Unknown };
    OverrideState overrideState(const QString &id) const;
    // Reset ONE built-in to stock: deletes exactly that override file,
    // restores the stock recipe in the model, drops the fingerprint.
    bool resetBuiltinToStock(const QString &id);

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
    // Gate seam: marks a stored preset as pre-cap (its FILE carries an
    // oversized image) so the Done-notice flow can be driven without
    // crafting a spliced legacy file inside the lifecycle harness. The
    // codec-level flag propagation is pinned separately (BrushLibrary b10).
    void setImagesCappedOnLoadForTest(const QString &id);
    bool hasBuiltinOverride(const QString &id) const;

    QString libraryName() const;
    void setLibraryName(const QString &name);

    // Whole-library export: every visible preset (built-in and user) into
    // one .sankobrushset bundle. Import walks the BrushImporter registry;
    // returns the number of presets APPLIED (added + upgraded/overridden).
    // A lossy importer (Photoshop .abr) fills *report with its per-brush
    // mapped/approximated/dropped account; the native importer leaves it
    // empty.
    //
    // Parsing and application are two different stages with two different
    // counts, and describing them as one produced a self-contradicting
    // report ("1 of 1 imported" + "no new brushes were added"). The
    // summary keeps them apart: per parsed preset the APPLICATION outcome
    // is exactly one of added / alreadyPresent / upgraded / failed, and
    // when the importer wrote a report, importFile appends the outcome
    // lines and a matching summary to it — the two can never disagree
    // because both are generated from the same records.
    struct ImportSummary {
        bool claimed = false;    // some importer recognised the file
        int parsed = 0;          // valid presets the importer produced
        int added = 0;           // new presets in the library
        int alreadyPresent = 0;  // identical content already there (J8)
        int upgraded = 0;        // upgraded in place / built-in override
        int failed = 0;          // could not be written; library unchanged
    };
    bool exportLibrary(const QString &path) const;
    int importFile(const QString &path, QString *report = nullptr,
                   ImportSummary *summary = nullptr);

    QString userPresetDir() const;

signals:
    void changed(); // any roster/shelf mutation (coarse; the UI re-lists)

private:
    void loadUserPresets();
    void loadShelfState();
    void loadBuiltinOverrides();
    // Shelf state (favourites, hidden, MRU, renames, library name). The
    // production model uses the registry-backed app settings; a model
    // built on a rootOverride keeps them in <root>/shelf.ini instead, so
    // a test with a scratch library really touches NOTHING of the user's —
    // the rootOverride comment always promised that, but shelf state
    // leaked to the real registry until the import-report seam refused to
    // run against it.
    QSettings shelfSettings() const;
    void saveShelfList(const char *key, const QStringList &list) const;
    bool writeUserPresetFile(const BrushPreset &preset) const;
    QString presetFilePath(const QString &id) const;
    QString overrideDir() const;
    QString overrideFilePath(const QString &id) const;

    bool insertUserPresetKeepingId(BrushPreset preset); // import path

    QString m_rootOverride; // empty = the app-data BrushLibrary folder
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
