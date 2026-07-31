#include "BrushLibraryModel.h"

#include "BrushImporter.h"
#include "BrushPresetCodec.h"
#include "BuiltinRoster.h"

#include <QDataStream>

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace brushlib {
namespace {

const QString kKeyPrefix = QStringLiteral("brushLibrary/v1/");
constexpr int kRecentCap = 30;

QSettings appSettings()
{
    return QSettings(QStringLiteral("SankoTV"), QStringLiteral("SankoTV"));
}

} // namespace

BrushLibraryModel::BrushLibraryModel(QObject *parent)
    : QObject(parent)
{
    m_presets = builtinRoster();
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
    if (!preset(id))
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
    for (BrushPreset &p : m_presets)
        if (p.builtin)
            for (const BrushPreset &stock : builtinRoster())
                if (stock.id == p.id) {
                    p.name = stock.name;
                    p.brush = stock.brush; // drop any studio override
                    break;
                }
    const QStringList overridden = m_overridden;
    for (const QString &id : overridden)
        QFile::remove(overrideFilePath(id));
    m_overridden.clear();
    saveShelfList("hidden", m_hidden);
    appSettings().remove(kKeyPrefix + QStringLiteral("renames"));
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
    QFile::remove(presetFilePath(id));
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
    BrushPreset &p = m_presets[idx];
    p.name = name.trimmed();
    if (p.builtin) {
        // Display-name override only; the roster in code stays pristine.
        m_builtinRenames.insert(id, p.name);
        QSettings s = appSettings();
        s.beginGroup(kKeyPrefix + QStringLiteral("renames"));
        s.setValue(QString(id).replace(QLatin1Char('/'), QLatin1Char('|')),
                   p.name);
        s.endGroup();
    } else {
        writeUserPresetFile(p);
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
    BrushPreset &p = m_presets[idx];
    p.brush = brush;
    bool ok = false;
    if (p.builtin) {
        // Never destroy the stock recipe: the edit is an override FILE the
        // constructor applies over the in-code roster. Restore Default
        // Brushes deletes these files and the stock brush returns.
        QDir().mkpath(overrideDir());
        QFile f(overrideFilePath(id));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(BrushPresetCodec::savePreset(p));
            ok = true;
            if (!m_overridden.contains(id))
                m_overridden.append(id);
        }
    } else {
        ok = writeUserPresetFile(p);
    }
    emit changed();
    return ok;
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
    appSettings().setValue(kKeyPrefix + QStringLiteral("name"),
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

void ensureNativeImporter()
{
    for (BrushImporter *imp : BrushImporter::importers())
        if (imp->name() == QStringLiteral("SankoTV"))
            return;
    BrushImporter::registerImporter(new SankoNativeImporter);
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

int BrushLibraryModel::importFile(const QString &path)
{
    ensureNativeImporter();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray bytes = f.readAll();
    for (const BrushImporter *imp : BrushImporter::importers()) {
        if (!imp->probe(bytes))
            continue;
        int added = 0;
        const QVector<BrushPreset> presets = imp->import(bytes);
        for (const BrushPreset &p : presets) {
            BrushPreset copy = p; // imported presets land as USER presets
            if (!addUserPreset(std::move(copy)).isEmpty())
                ++added;
        }
        return added;
    }
    return 0;
}

QString BrushLibraryModel::userPresetDir() const
{
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
            && !p.id.isEmpty() && !m_byId.contains(p.id))
            m_presets.append(std::move(p));
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
    QSettings s = appSettings();
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
    appSettings().setValue(kKeyPrefix + QLatin1String(key), list);
}

bool BrushLibraryModel::writeUserPresetFile(const BrushPreset &preset) const
{
    QDir().mkpath(userPresetDir());
    QFile f(presetFilePath(preset.id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(BrushPresetCodec::savePreset(preset));
    return true;
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
