#include "BrushLibraryModel.h"
#include "SankoSettings.h"

#include "AbrImporter.h"
#include "BrushImporter.h"
#include "BrushPresetCodec.h"
#include "BuiltinRoster.h"

#include <QDataStream>

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace brushlib {
namespace {

const QString kKeyPrefix = QStringLiteral("brushLibrary/v1/");
constexpr int kRecentCap = 30;

QSettings appSettings()
{
    return sankoSettings();
}
} // namespace (reopened below)

// Scratch-rooted models keep shelf state beside the presets they scratch
// (see the header comment). The store itself now comes from
// sankoSettings() — the app-wide choke point with its own test override —
// and this per-model override layers on top so a scratch-rooted model's
// shelf lives beside its presets rather than in the shared test INI.
QSettings BrushLibraryModel::shelfSettings() const
{
    if (!m_rootOverride.isEmpty())
        return QSettings(m_rootOverride + QStringLiteral("/shelf.ini"),
                         QSettings::IniFormat);
    return appSettings();
}

namespace {

// The single atomic file write (D3): QSaveFile writes an adjacent temp file
// and commits by rename, so the target is either the old bytes or the new
// bytes — never a truncation. Returns false without touching the target on
// any failure (unwritable directory, read-only target, disk full).
bool atomicWrite(const QString &path, const QByteArray &bytes)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

} // namespace

BrushLibraryModel::BrushLibraryModel(QObject *parent,
                                     const QString &rootOverride)
    : QObject(parent)
    , m_rootOverride(rootOverride)
{
    // Concat at the CALL SITE, never inside builtinRoster(): that
    // function's output is a pinned baseline (the combined preview SHA and
    // the per-category counts), and the eraser roster is pinned separately.
    m_presets = builtinRoster() + builtinEraserRoster();
    loadUserPresets();
    for (int i = 0; i < m_presets.size(); ++i)
        m_byId.insert(m_presets.at(i).id, i);
    loadShelfState();
    // Built-in renames apply as display-name overrides (the roster in code
    // stays pristine; Restore Default Brushes drops the overrides).
    for (auto it = m_builtinRenames.cbegin(); it != m_builtinRenames.cend();
         ++it)
        if (const int idx = m_byId.value(it.key(), -1); idx >= 0)
            m_presets[idx].name = it.value();
    // Built-in BRUSH overrides (studio "Done" on a built-in) apply the same
    // way: over the in-code roster, never into it.
    loadBuiltinOverrides();
}

QStringList BrushLibraryModel::categories()
{
    return builtinCategories();
}

QVector<const BrushPreset *>
BrushLibraryModel::presetsIn(const QString &category) const
{
    QVector<const BrushPreset *> builtins, users;
    for (const BrushPreset &p : m_presets) {
        if (p.category != category || m_hidden.contains(p.id))
            continue;
        (p.builtin ? builtins : users).append(&p);
    }
    std::sort(users.begin(), users.end(),
              [](const BrushPreset *a, const BrushPreset *b) {
                  return a->name.localeAwareCompare(b->name) < 0;
              });
    return builtins + users;
}

QVector<const BrushPreset *> BrushLibraryModel::recentPresets() const
{
    QVector<const BrushPreset *> out;
    for (const QString &id : m_recent)
        if (const BrushPreset *p = preset(id); p && !m_hidden.contains(id))
            out.append(p);
    return out;
}

const BrushPreset *BrushLibraryModel::preset(const QString &id) const
{
    const int idx = m_byId.value(id, -1);
    return idx >= 0 ? &m_presets.at(idx) : nullptr;
}

void BrushLibraryModel::recordUsage(const QString &id)
{
    const BrushPreset *p = preset(id);
    if (!p)
        return;
    // V1 DECISION, recorded in HANDOFF: "Recent" stays BRUSH-only. Eraser
    // activations would churn the brush Recent list for no benefit (the
    // eraser roster is eight rows away, not thirty). Deliberate exclusion,
    // not an oversight; revisit if it feels wrong in use.
    if (p->brush.eraseMode())
        return;
    m_recent.removeAll(id);
    m_recent.prepend(id);
    while (m_recent.size() > kRecentCap)
        m_recent.removeLast();
    saveShelfList("recent", m_recent);
    emit changed();
}

bool BrushLibraryModel::isFavourite(const QString &id) const
{
    return m_favourites.contains(id);
}

void BrushLibraryModel::setFavourite(const QString &id, bool on)
{
    if (on == m_favourites.contains(id))
        return;
    if (on)
        m_favourites.append(id);
    else
        m_favourites.removeAll(id);
    saveShelfList("favourites", m_favourites);
    emit changed();
}

bool BrushLibraryModel::isHidden(const QString &id) const
{
    return m_hidden.contains(id);
}

void BrushLibraryModel::setBuiltinHidden(const QString &id, bool hidden)
{
    const BrushPreset *p = preset(id);
    if (!p || !p->builtin || hidden == m_hidden.contains(id))
        return;
    if (hidden)
        m_hidden.append(id);
    else
        m_hidden.removeAll(id);
    saveShelfList("hidden", m_hidden);
    emit changed();
}

void BrushLibraryModel::restoreDefaultBrushes()
{
    m_hidden.clear();
    m_builtinRenames.clear();
    // DISK FIRST (D3): a brush override is dropped from memory only when
    // its file is actually gone — otherwise the file would resurrect the
    // override at the next restart while the UI claimed stock. An override
    // whose file cannot be removed stays applied, consistently, in both.
    const QStringList overridden = m_overridden;
    QStringList stillOverridden;
    for (const QString &id : overridden) {
        const QString path = overrideFilePath(id);
        if (QFile::exists(path) && !QFile::remove(path)) {
            stillOverridden.append(id);
            continue;
        }
    }
    m_overridden = stillOverridden;
    const QVector<BrushPreset> roster =
        builtinRoster() + builtinEraserRoster();
    for (BrushPreset &p : m_presets)
        if (p.builtin)
            for (const BrushPreset &stock : roster)
                if (stock.id == p.id) {
                    p.name = stock.name;
                    if (!m_overridden.contains(p.id))
                        p.brush = stock.brush; // override gone: stock returns
                    break;
                }
    saveShelfList("hidden", m_hidden);
    shelfSettings().remove(kKeyPrefix + QStringLiteral("renames"));
    emit changed();
}

QString BrushLibraryModel::addUserPreset(BrushPreset preset)
{
    preset.builtin = false;
    preset.id = QStringLiteral("user/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!writeUserPresetFile(preset))
        return QString();
    m_byId.insert(preset.id, m_presets.size());
    m_presets.append(std::move(preset));
    emit changed();
    return m_presets.last().id;
}

bool BrushLibraryModel::removeUserPreset(const QString &id)
{
    const int idx = m_byId.value(id, -1);
    if (idx < 0 || m_presets.at(idx).builtin)
        return false;
    // DISK FIRST (D3): if the file cannot be deleted, keep the preset —
    // dropping it from memory only would resurrect it at the next restart.
    const QString path = presetFilePath(id);
    if (QFile::exists(path) && !QFile::remove(path))
        return false;
    m_presets.removeAt(idx);
    m_byId.clear();
    for (int i = 0; i < m_presets.size(); ++i)
        m_byId.insert(m_presets.at(i).id, i);
    m_favourites.removeAll(id);
    m_recent.removeAll(id);
    saveShelfList("favourites", m_favourites);
    saveShelfList("recent", m_recent);
    emit changed();
    return true;
}

bool BrushLibraryModel::renamePreset(const QString &id, const QString &name)
{
    const int idx = m_byId.value(id, -1);
    if (idx < 0 || name.trimmed().isEmpty())
        return false;
    const QString trimmed = name.trimmed();
    if (m_presets.at(idx).builtin) {
        // Display-name override only; the roster in code stays pristine.
        // Registry-backed (no detectable failure path — see class comment).
        m_presets[idx].name = trimmed;
        m_builtinRenames.insert(id, trimmed);
        QSettings s = shelfSettings();
        s.beginGroup(kKeyPrefix + QStringLiteral("renames"));
        s.setValue(QString(id).replace(QLatin1Char('/'), QLatin1Char('|')),
                   trimmed);
        s.endGroup();
    } else {
        // DISK FIRST (D3): write the renamed preset before renaming it in
        // memory, so a failed write cannot leave a name a restart forgets.
        BrushPreset candidate = m_presets.at(idx);
        candidate.name = trimmed;
        if (!writeUserPresetFile(candidate))
            return false;
        m_presets[idx].name = trimmed;
    }
    emit changed();
    return true;
}

QString BrushLibraryModel::duplicatePreset(const QString &id)
{
    const BrushPreset *src = preset(id);
    if (!src)
        return QString();
    BrushPreset copy = *src;
    copy.name = src->name + QStringLiteral(" Copy");
    return addUserPreset(std::move(copy));
}

bool BrushLibraryModel::updateBrush(const QString &id, const ::Brush &brush)
{
    const int idx = m_byId.value(id, -1);
    if (idx < 0)
        return false;
    // DISK FIRST (D3): serialise a candidate and write it atomically; only
    // a successful write commits to memory. On failure the model still
    // shows — and a restart still loads — the previous value.
    BrushPreset candidate = m_presets.at(idx);
    candidate.brush = brush;
    if (candidate.builtin) {
        // Never destroy the stock recipe: the edit is an override FILE the
        // constructor applies over the in-code roster. Restore Default
        // Brushes deletes these files and the stock brush returns.
        if (!QDir().mkpath(overrideDir()))
            return false;
        if (!atomicWrite(overrideFilePath(id),
                         BrushPresetCodec::savePreset(candidate)))
            return false;
        m_presets[idx].brush = brush;
        if (!m_overridden.contains(id))
            m_overridden.append(id);
    } else {
        if (!writeUserPresetFile(candidate))
            return false;
        m_presets[idx].brush = brush;
    }
    emit changed();
    return true;
}

bool BrushLibraryModel::hasBuiltinOverride(const QString &id) const
{
    return m_overridden.contains(id);
}

QString BrushLibraryModel::libraryName() const
{
    return m_libraryName;
}

void BrushLibraryModel::setLibraryName(const QString &name)
{
    if (name.trimmed().isEmpty() || name == m_libraryName)
        return;
    m_libraryName = name.trimmed();
    shelfSettings().setValue(kKeyPrefix + QStringLiteral("name"),
                           m_libraryName);
    emit changed();
}

namespace {
constexpr quint32 kSetMagic = 0x534E4B53; // "SNKS" bundle
constexpr quint16 kSetVersion = 1;

// The native importer: single .sankobrush presets and .sankobrushset
// bundles. Registered once, lazily, before the first import.
class SankoNativeImporter : public BrushImporter
{
public:
    QString name() const override { return QStringLiteral("SankoTV"); }
    bool probe(const QByteArray &bytes) const override
    {
        QDataStream s(bytes);
        quint32 magic = 0;
        s >> magic;
        return magic == kSetMagic || magic == 0x534E4B50; // SNKP preset
    }
    QVector<BrushPreset> import(const QByteArray &bytes) const override
    {
        QVector<BrushPreset> out;
        QDataStream s(bytes);
        s.setVersion(QDataStream::Qt_6_5);
        quint32 magic = 0;
        s >> magic;
        if (magic == kSetMagic) {
            quint16 version = 0;
            qint32 count = 0;
            s >> version >> count;
            if (version != kSetVersion)
                return out;
            for (qint32 i = 0; i < count && s.status() == QDataStream::Ok;
                 ++i) {
                QByteArray blob;
                s >> blob;
                BrushPreset p;
                if (BrushPresetCodec::loadPreset(blob, p))
                    out.append(std::move(p));
            }
        } else {
            BrushPreset p;
            if (BrushPresetCodec::loadPreset(bytes, p))
                out.append(std::move(p));
        }
        return out;
    }
};

void ensureImporters()
{
    for (BrushImporter *imp : BrushImporter::importers())
        if (imp->name() == QStringLiteral("SankoTV"))
            return;
    // Native first: its magic-number probe can never claim a foreign file,
    // and no foreign probe can claim a native one.
    BrushImporter::registerImporter(new SankoNativeImporter);
    BrushImporter::registerImporter(new AbrImporter);
}

QVector<BrushImporter *> &importerRegistry()
{
    static QVector<BrushImporter *> registry;
    return registry;
}
} // namespace

void BrushImporter::registerImporter(BrushImporter *importer)
{
    importerRegistry().append(importer);
}

const QVector<BrushImporter *> &BrushImporter::importers()
{
    return importerRegistry();
}

bool BrushLibraryModel::exportLibrary(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QVector<const BrushPreset *> visible;
    for (const BrushPreset &p : m_presets)
        if (!m_hidden.contains(p.id))
            visible.append(&p);
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_5);
    s << kSetMagic << kSetVersion << qint32(visible.size());
    for (const BrushPreset *p : visible)
        s << BrushPresetCodec::savePreset(*p);
    f.write(bytes);
    return true;
}

int BrushLibraryModel::importFile(const QString &path, QString *report,
                                  ImportSummary *summary)
{
    ensureImporters();
    if (report)
        report->clear();
    if (summary)
        *summary = ImportSummary();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray bytes = f.readAll();
    for (const BrushImporter *imp : BrushImporter::importers()) {
        if (!imp->probe(bytes))
            continue;
        if (summary)
            summary->claimed = true;
        // IDENTITY-AWARE IMPORT (phase 5 defect J8). The old rule — every
        // imported preset lands as a fresh user preset — meant re-importing
        // the library's own export duplicated all 62 built-ins as user
        // copies. Presets carry their identity in the file, so honour it:
        //   built-in id known to the roster:
        //     byte-identical to the current brush -> SKIP (nothing to do);
        //     different -> apply as an OVERRIDE via updateBrush(), exactly
        //     as if the studio's Done had made the edit here. This is what
        //     carries a user's built-in edits to a new machine.
        //   built-in id NOT in this app's roster (foreign set): fall through
        //     as a user preset — the brush is real even if the id is not.
        //   user preset id already present:
        //     byte-identical brush -> SKIP (idempotent re-import; the local
        //     NAME wins, so a local rename survives);
        //     different -> SKIP too: the local version wins over a backup.
        //     Silently replacing newer local work with older backup bytes
        //     is the exact silent-data-loss class this fix pass removes.
        //   user preset id unseen: import KEEPING the id, so the next
        //     re-import of the same file recognises it. A fresh id here is
        //     what made double-import double the library.
        // Returns the number of presets actually APPLIED (new user presets
        // + built-in overrides written); skips count as zero.
        int applied = 0;
        // Per-preset APPLICATION outcome — the second stage, distinct from
        // the importer's PARSE report above it. One line per parsed preset,
        // and the summary counts below are tallied from the same switch
        // that writes the lines, so they cannot disagree.
        ImportSummary tally;
        tally.claimed = true;
        QStringList outcomeLines;
        const auto recordAdded = [&](const BrushPreset &p) {
            ++tally.added;
            outcomeLines << QStringLiteral(
                                 "\"%1\" — added (new brush in the "
                                 "\"%2\" category)")
                                .arg(p.name, p.category);
        };
        const auto recordAlreadyPresent = [&](const BrushPreset &p,
                                              const QString &detail) {
            ++tally.alreadyPresent;
            outcomeLines << QStringLiteral("\"%1\" — already in the "
                                           "library%2")
                                .arg(p.name, detail);
        };
        const auto recordUpgraded = [&](const BrushPreset &p,
                                        const QString &detail) {
            ++tally.upgraded;
            outcomeLines << QStringLiteral("\"%1\" — %2").arg(p.name, detail);
        };
        const auto recordFailed = [&](const BrushPreset &p) {
            ++tally.failed;
            outcomeLines << QStringLiteral(
                                 "\"%1\" — FAILED: could not be written to "
                                 "the library (this brush is unchanged)")
                                .arg(p.name);
        };
        QVector<BrushPreset> presets = imp->importWithReport(bytes, report);
        tally.parsed = presets.size();
        for (BrushPreset &incoming : presets)
            if (incoming.category == QLatin1String("Watercolor"))
                incoming.category = QStringLiteral("Painting"); // retired cat
        // ABR presets that existed BEFORE this import AND belong to an
        // OLDER mapping generation — their stored id no longer matches the
        // id their content would fingerprint to today (the baked era, or a
        // future field addition). Only those are upgrade candidates; a
        // CURRENT-generation preset whose id recomputes to itself is
        // simply a different brush that may happen to share a name, and
        // the upgrade path must never claim it (two packs both containing
        // an "Imported Brush 1" stay two brushes). Each candidate can be
        // claimed at most once, and never one added by this same call.
        QSet<QString> upgradableAbr;
        for (const BrushPreset &existing : m_presets)
            if (!existing.builtin
                && existing.id.startsWith(QStringLiteral("user/abr-"))
                && AbrImporter::contentId(existing) != existing.id)
                upgradableAbr.insert(existing.id);
        for (const BrushPreset &p : presets) {
            const int existingIdx = m_byId.value(p.id, -1);
            if (p.builtin && existingIdx >= 0
                && m_presets.at(existingIdx).builtin) {
                if (BrushPresetCodec::saveBrush(p.brush)
                    == BrushPresetCodec::saveBrush(
                        m_presets.at(existingIdx).brush)) {
                    recordAlreadyPresent(
                        p, QStringLiteral(" (built-in, already in this "
                                          "state)"));
                    continue; // already in this state
                }
                if (updateBrush(p.id, p.brush)) { // override, disk-first
                    ++applied;
                    recordUpgraded(p, QStringLiteral(
                        "built-in brush updated with this file's version"));
                } else {
                    recordFailed(p);
                }
                continue;
            }
            if (!p.builtin && existingIdx >= 0) {
                recordAlreadyPresent(
                    p, QStringLiteral(" (your local version is kept)"));
                continue; // known user preset: local version wins
            }
            // ABR IDENTITY ACROSS MAPPING GENERATIONS. An imported brush's
            // id fingerprints its MAPPED content, so when the mapping
            // itself improves (the unbake that moved static transforms
            // into tip parameters; any future field addition) the same
            // source ABR produces a new id. Recognise the same source
            // instead of duplicating it:
            //   byte-equal brush among existing abr presets -> SKIP
            //     (already imported under the current mapping);
            //   same NAME among abr presets that predate this call ->
            //     UPGRADE IN PLACE via updateBrush (disk-first, D3),
            //     KEEPING the stored id — favourites, Recent entries and
            //     hidden flags keep pointing at the brush they always
            //     pointed at.
            // A rename breaks the name link (same as the old id scheme,
            // where the name was hashed into the id): the re-import then
            // lands as a new preset beside the renamed one.
            if (p.id.startsWith(QStringLiteral("user/abr-"))) {
                bool handled = false;
                const QByteArray incomingBytes =
                    BrushPresetCodec::saveBrush(p.brush);
                for (const BrushPreset &existing : m_presets)
                    if (!existing.builtin
                        && existing.id.startsWith(
                            QStringLiteral("user/abr-"))
                        && existing.name == p.name // same-content brushes
                        && BrushPresetCodec::saveBrush(existing.brush)
                            == incomingBytes) { // under other names coexist
                        handled = true; // identical content already here
                        recordAlreadyPresent(
                            p, QStringLiteral(" (identical content; "
                                              "re-import is a no-op)"));
                        break;
                    }
                if (!handled) {
                    // Pick the candidate BEFORE touching the set: the
                    // upgrade both mutates upgradableAbr and (through
                    // updateBrush) m_presets, neither of which may happen
                    // while iterating them.
                    QString claim;
                    for (const QString &id :
                         std::as_const(upgradableAbr)) {
                        const BrushPreset *existing = preset(id);
                        if (existing && existing->name == p.name) {
                            claim = id;
                            break;
                        }
                    }
                    if (!claim.isEmpty()) {
                        upgradableAbr.remove(claim); // claimed once
                        // A FAILED upgrade still counts as handled: the
                        // preset stays exactly as it was on disk and in
                        // memory (D3), and must not also be added as a
                        // duplicate — but it is REPORTED as a failure, not
                        // silently folded into "already present".
                        if (updateBrush(claim, p.brush)) {
                            ++applied;
                            recordUpgraded(p, QStringLiteral(
                                "upgraded in place — kept its id, "
                                "favourites, Recent position, and hidden "
                                "flag"));
                        } else {
                            recordFailed(p);
                        }
                        handled = true;
                    }
                }
                if (handled)
                    continue;
            }
            BrushPreset copy = p;
            copy.builtin = false;
            if (p.builtin || p.id.isEmpty()
                || !p.id.startsWith(QStringLiteral("user/"))) {
                // Foreign/malformed identity: a fresh user id.
                if (!addUserPreset(std::move(copy)).isEmpty()) {
                    ++applied;
                    recordAdded(p);
                } else {
                    recordFailed(p);
                }
            } else if (insertUserPresetKeepingId(std::move(copy))) {
                ++applied;
                recordAdded(p);
            } else {
                recordFailed(p);
            }
        }
        // The application block joins a report the importer wrote (the
        // lossy .abr path); the native importer leaves the report empty
        // and the caller words its own dialog from the summary counts.
        if (report && !report->isEmpty()) {
            *report += QStringLiteral("\nApplied to the library:\n");
            if (outcomeLines.isEmpty())
                *report += QStringLiteral("  (nothing — no brush in this "
                                          "file could be parsed)\n");
            for (const QString &line : std::as_const(outcomeLines))
                *report += QStringLiteral("  - ") + line + QLatin1Char('\n');
            if (tally.parsed == 0) {
                *report += QStringLiteral(
                    "\nNothing in this file could be imported — see the "
                    "per-brush notes above for why.");
            } else if (tally.added + tally.upgraded + tally.failed == 0) {
                *report += QStringLiteral(
                    "\nEvery brush in this file is already in the library — "
                    "nothing needed to change. Re-importing the same file "
                    "is a no-op by design.");
            } else {
                *report += QStringLiteral("\nSummary: %1 added, %2 upgraded "
                                          "in place, %3 already present, "
                                          "%4 failed.")
                               .arg(tally.added)
                               .arg(tally.upgraded)
                               .arg(tally.alreadyPresent)
                               .arg(tally.failed);
            }
            if (tally.failed > 0)
                *report += QStringLiteral(
                    "\nWARNING: %1 brush(es) could not be written to the "
                    "library. Check disk space and permissions; the "
                    "library itself is unchanged for those brushes.")
                               .arg(tally.failed);
        }
        if (summary)
            *summary = tally;
        return applied;
    }
    return 0;
}

// Import path: add a user preset PRESERVING its incoming id (disk-first,
// like every other mutation). Only called when the id is absent.
bool BrushLibraryModel::insertUserPresetKeepingId(BrushPreset preset)
{
    preset.builtin = false;
    if (!writeUserPresetFile(preset))
        return false;
    m_byId.insert(preset.id, m_presets.size());
    m_presets.append(std::move(preset));
    emit changed();
    return true;
}

QString BrushLibraryModel::userPresetDir() const
{
    if (!m_rootOverride.isEmpty())
        return m_rootOverride;
    return QStandardPaths::writableLocation(
               QStandardPaths::AppDataLocation)
        + QStringLiteral("/BrushLibrary");
}

void BrushLibraryModel::loadUserPresets()
{
    QDir dir(userPresetDir());
    if (!dir.exists())
        return;
    const QStringList files = dir.entryList(
        {QStringLiteral("*.sankobrush")}, QDir::Files, QDir::Name);
    for (const QString &file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        BrushPreset p;
        if (BrushPresetCodec::loadPreset(f.readAll(), p) && !p.builtin
            && !p.id.isEmpty() && !m_byId.contains(p.id)) {
            // The Watercolor category was retired (built-ins moved to
            // Painting): user presets saved under it are remapped so they
            // stay reachable. In-memory only — the file rewrites itself on
            // the preset's next disk-first write; nothing is lost either way.
            if (p.category == QLatin1String("Watercolor"))
                p.category = QStringLiteral("Painting");
            m_presets.append(std::move(p));
        }
    }
}

void BrushLibraryModel::loadBuiltinOverrides()
{
    QDir dir(overrideDir());
    if (!dir.exists())
        return;
    const QStringList files = dir.entryList(
        {QStringLiteral("*.sankobrush")}, QDir::Files, QDir::Name);
    for (const QString &file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        BrushPreset loaded;
        if (!BrushPresetCodec::loadPreset(f.readAll(), loaded))
            continue;
        const int idx = m_byId.value(loaded.id, -1);
        if (idx < 0 || !m_presets.at(idx).builtin)
            continue; // stale override for a roster entry that no longer exists
        m_presets[idx].brush = loaded.brush;
        m_overridden.append(loaded.id);
    }
}

void BrushLibraryModel::loadShelfState()
{
    QSettings s = shelfSettings();
    m_favourites =
        s.value(kKeyPrefix + QStringLiteral("favourites")).toStringList();
    m_hidden = s.value(kKeyPrefix + QStringLiteral("hidden")).toStringList();
    m_recent = s.value(kKeyPrefix + QStringLiteral("recent")).toStringList();
    m_libraryName = s.value(kKeyPrefix + QStringLiteral("name"),
                            QStringLiteral("Brush Library"))
                        .toString();
    s.beginGroup(kKeyPrefix + QStringLiteral("renames"));
    const QStringList keys = s.childKeys();
    for (const QString &key : keys)
        m_builtinRenames.insert(
            QString(key).replace(QLatin1Char('|'), QLatin1Char('/')),
            s.value(key).toString());
    s.endGroup();
}

void BrushLibraryModel::saveShelfList(const char *key,
                                      const QStringList &list) const
{
    shelfSettings().setValue(kKeyPrefix + QLatin1String(key), list);
}

bool BrushLibraryModel::writeUserPresetFile(const BrushPreset &preset) const
{
    if (!QDir().mkpath(userPresetDir()))
        return false;
    return atomicWrite(presetFilePath(preset.id),
                       BrushPresetCodec::savePreset(preset));
}

QString BrushLibraryModel::presetFilePath(const QString &id) const
{
    QString name = id;
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    return userPresetDir() + QLatin1Char('/') + name
        + QStringLiteral(".sankobrush");
}

QString BrushLibraryModel::overrideDir() const
{
    return userPresetDir() + QStringLiteral("/Overrides");
}

QString BrushLibraryModel::overrideFilePath(const QString &id) const
{
    QString name = id;
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    return overrideDir() + QLatin1Char('/') + name
        + QStringLiteral(".sankobrush");
}

} // namespace brushlib
