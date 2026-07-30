#include "BrushLibraryModel.h"

#include "BrushPresetCodec.h"
#include "BuiltinRoster.h"

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
                    break;
                }
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
    if (idx < 0 || m_presets.at(idx).builtin)
        return false;
    m_presets[idx].brush = brush;
    const bool ok = writeUserPresetFile(m_presets.at(idx));
    emit changed();
    return ok;
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

} // namespace brushlib
