#pragma once

// WHICH SETTINGS STORE CAN THIS PROCESS ACTUALLY SEE?
//
// A process started from a PACKAGED application (an MSIX app - the Claude
// desktop app is one) gets a private, copy-on-write view of the registry:
// its writes land in a package-private hive, and any value once written
// there reads back from the private copy forever, shadowing the user's real
// one. Found 2026-10-01: from such a process the SankoTV store read as 43
// keys / 82 values while the user's real store held 51 / 109, and 50 of the
// 78 shared values differed.
//
// NOTHING IN THE PROCESS SAYS SO. Measured, inside and outside the package:
//   GetCurrentPackageFullName      "not packaged" in both
//   GetPackageFamilyName           "not packaged" in both
//   NtQueryKey (the kernel's name)  the same \REGISTRY\USER\<sid>\... in both
// and walking the parent processes for an executable under WindowsApps is
// wrong the other way: Windows Terminal is packaged too, and a shell started
// from it sees the real registry.
//
// What does tell: the operating system's own registry provider (WMI
// StdRegProv) runs in a service process outside every package and reads the
// REAL store, whoever calls it. So the two views are read side by side -
// directly, as this process sees the store, and through the provider - and
// COMPARED. Identical: this process is looking at the real store. Different:
// it is looking at a private copy, and the comparison says by how much.
//
// Read-only, both ways. Windows only; elsewhere available() is false.

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstring>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>
// The WMI interface ids live in this import library; named here so that a
// target using the header needs nothing added to its link line.
#pragma comment(lib, "wbemuuid.lib")
#endif

namespace realstore {

struct View
{
    bool ok = false;     // the read itself worked
    bool exists = false; // ...and the key is there (a fresh machine has none)
    QString error;
    QStringList keys;              // every key path under the root, relative
    QMap<QString, QString> values; // "key\name" -> "TYPE:canonical data"
};

struct Verdict
{
    bool determined = false; // false: the real store could not be read
    bool privateCopy = false;
    int directValues = 0, realValues = 0;
    int differing = 0;       // present in both, different data
    int onlyReal = 0;        // the real store has it, this process cannot see it
    int onlyDirect = 0;      // this process sees it, the real store has not got it
    QString error;

    // What a check guarding "the settings store" is really guarding.
    QString guarded() const
    {
        if (!determined)
            return QStringLiteral("a store that could NOT be identified");
        return privateCopy
            ? QStringLiteral("a PRIVATE COPY of the settings store (this "
                             "process is isolated from the user's real one)")
            : QStringLiteral("the user's REAL settings store");
    }
};

#ifdef Q_OS_WIN

namespace detail {

inline QString canonical(DWORD type, const QByteArray &raw)
{
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ: {
        QString s = QString::fromWCharArray(
            reinterpret_cast<const wchar_t *>(raw.constData()),
            int(raw.size() / sizeof(wchar_t)));
        while (s.endsWith(QChar(u'\0')))
            s.chop(1);
        return (type == REG_SZ ? QStringLiteral("SZ:") : QStringLiteral("XSZ:"))
            + s;
    }
    case REG_DWORD: {
        quint32 v = 0;
        memcpy(&v, raw.constData(), qMin<qsizetype>(raw.size(), 4));
        return QStringLiteral("DWORD:%1").arg(v);
    }
    case REG_QWORD: {
        quint64 v = 0;
        memcpy(&v, raw.constData(), qMin<qsizetype>(raw.size(), 8));
        return QStringLiteral("QWORD:%1").arg(v);
    }
    case REG_MULTI_SZ: {
        QString s = QString::fromWCharArray(
            reinterpret_cast<const wchar_t *>(raw.constData()),
            int(raw.size() / sizeof(wchar_t)));
        while (s.endsWith(QChar(u'\0')))
            s.chop(1);
        return QStringLiteral("MULTI:")
            + s.split(QChar(u'\0')).join(QLatin1Char('\n'));
    }
    default:
        return QStringLiteral("BIN:") + QString::fromLatin1(raw.toHex());
    }
}

inline void walkDirect(HKEY parent, const QString &rel, View &view)
{
    view.keys.append(rel);
    DWORD subKeys = 0, maxSubLen = 0, valueCount = 0, maxNameLen = 0,
          maxDataLen = 0;
    if (RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, &subKeys,
                         &maxSubLen, nullptr, &valueCount, &maxNameLen,
                         &maxDataLen, nullptr, nullptr)
        != ERROR_SUCCESS)
        return;
    QVector<wchar_t> name(qMax(maxNameLen, maxSubLen) + 2);
    QByteArray data(int(maxDataLen) + 4, '\0');
    for (DWORD i = 0; i < valueCount; ++i) {
        DWORD nameLen = DWORD(name.size()), dataLen = DWORD(data.size()), type = 0;
        if (RegEnumValueW(parent, i, name.data(), &nameLen, nullptr, &type,
                          reinterpret_cast<LPBYTE>(data.data()), &dataLen)
            != ERROR_SUCCESS)
            continue;
        const QString valueName = QString::fromWCharArray(name.data(), int(nameLen));
        view.values.insert(rel + QLatin1Char('\\') + valueName,
                           canonical(type, data.left(int(dataLen))));
    }
    for (DWORD i = 0; i < subKeys; ++i) {
        DWORD nameLen = DWORD(name.size());
        if (RegEnumKeyExW(parent, i, name.data(), &nameLen, nullptr, nullptr,
                          nullptr, nullptr)
            != ERROR_SUCCESS)
            continue;
        const QString sub = QString::fromWCharArray(name.data(), int(nameLen));
        HKEY child = nullptr;
        if (RegOpenKeyExW(parent, reinterpret_cast<LPCWSTR>(sub.utf16()), 0,
                          KEY_READ, &child)
            == ERROR_SUCCESS) {
            walkDirect(child, rel.isEmpty() ? sub : rel + QLatin1Char('\\') + sub,
                       view);
            RegCloseKey(child);
        }
    }
}

// One StdRegProv method call. Returns the out-parameters object, or null.
inline IWbemClassObject *call(IWbemServices *services, IWbemClassObject *cls,
                              const wchar_t *method, const QString &subKey,
                              const QString *valueName = nullptr)
{
    IWbemClassObject *inSig = nullptr, *in = nullptr, *out = nullptr;
    if (FAILED(cls->GetMethod(method, 0, &inSig, nullptr)) || !inSig)
        return nullptr;
    const HRESULT spawned = inSig->SpawnInstance(0, &in);
    inSig->Release();
    if (FAILED(spawned) || !in)
        return nullptr;
    _variant_t hive(LONG(0x80000001u)); // HKEY_CURRENT_USER
    hive.vt = VT_I4;
    in->Put(L"hDefKey", 0, &hive, 0);
    _variant_t key(reinterpret_cast<const wchar_t *>(subKey.utf16()));
    in->Put(L"sSubKeyName", 0, &key, 0);
    if (valueName) {
        _variant_t name(reinterpret_cast<const wchar_t *>(valueName->utf16()));
        in->Put(L"sValueName", 0, &name, 0);
    }
    const HRESULT hr = services->ExecMethod(_bstr_t(L"StdRegProv"),
                                            _bstr_t(method), 0, nullptr, in,
                                            &out, nullptr);
    in->Release();
    return SUCCEEDED(hr) ? out : nullptr;
}

inline QStringList stringArray(IWbemClassObject *out, const wchar_t *field)
{
    QStringList list;
    _variant_t v;
    if (!out || FAILED(out->Get(field, 0, &v, nullptr, nullptr))
        || !(v.vt & VT_ARRAY) || !v.parray)
        return list;
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(v.parray, 1, &lo);
    SafeArrayGetUBound(v.parray, 1, &hi);
    for (LONG i = lo; i <= hi; ++i) {
        BSTR s = nullptr;
        if (SUCCEEDED(SafeArrayGetElement(v.parray, &i, &s)) && s) {
            list.append(QString::fromWCharArray(s, int(SysStringLen(s))));
            SysFreeString(s);
        }
    }
    return list;
}

inline QVector<int> intArray(IWbemClassObject *out, const wchar_t *field)
{
    QVector<int> list;
    _variant_t v;
    if (!out || FAILED(out->Get(field, 0, &v, nullptr, nullptr))
        || !(v.vt & VT_ARRAY) || !v.parray)
        return list;
    LONG lo = 0, hi = -1;
    SafeArrayGetLBound(v.parray, 1, &lo);
    SafeArrayGetUBound(v.parray, 1, &hi);
    for (LONG i = lo; i <= hi; ++i) {
        LONG n = 0;
        if (SUCCEEDED(SafeArrayGetElement(v.parray, &i, &n)))
            list.append(int(n));
    }
    return list;
}

inline QString valueThroughWmi(IWbemServices *services, IWbemClassObject *cls,
                               const QString &subKey, const QString &name,
                               int type)
{
    const wchar_t *method = L"GetBinaryValue";
    switch (type) {
    // GetStringValue for both: GetExpandedStringValue would EXPAND the
    // variables, and the direct read does not - a difference of the
    // reader's making, reported as isolation.
    case REG_SZ:
    case REG_EXPAND_SZ: method = L"GetStringValue"; break;
    case REG_DWORD: method = L"GetDWORDValue"; break;
    case REG_QWORD: method = L"GetQWORDValue"; break;
    case REG_MULTI_SZ: method = L"GetMultiStringValue"; break;
    default: break;
    }
    IWbemClassObject *out = call(services, cls, method, subKey, &name);
    if (!out)
        return QStringLiteral("?:unreadable");
    QString text;
    _variant_t v;
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ:
        if (SUCCEEDED(out->Get(L"sValue", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR)
            text = (type == REG_SZ ? QStringLiteral("SZ:") : QStringLiteral("XSZ:"))
                + QString::fromWCharArray(v.bstrVal, int(SysStringLen(v.bstrVal)));
        break;
    case REG_DWORD:
        if (SUCCEEDED(out->Get(L"uValue", 0, &v, nullptr, nullptr)))
            text = QStringLiteral("DWORD:%1").arg(quint32(v.lVal));
        break;
    case REG_QWORD:
        if (SUCCEEDED(out->Get(L"uValue", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR)
            text = QStringLiteral("QWORD:")
                + QString::fromWCharArray(v.bstrVal, int(SysStringLen(v.bstrVal)));
        break;
    case REG_MULTI_SZ:
        text = QStringLiteral("MULTI:")
            + stringArray(out, L"sValue").join(QLatin1Char('\n'));
        break;
    default: {
        QByteArray bytes;
        if (SUCCEEDED(out->Get(L"uValue", 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY)
            && v.parray) {
            LONG lo = 0, hi = -1;
            SafeArrayGetLBound(v.parray, 1, &lo);
            SafeArrayGetUBound(v.parray, 1, &hi);
            for (LONG i = lo; i <= hi; ++i) {
                BYTE b = 0;
                if (SUCCEEDED(SafeArrayGetElement(v.parray, &i, &b)))
                    bytes.append(char(b));
            }
        }
        text = QStringLiteral("BIN:") + QString::fromLatin1(bytes.toHex());
        break;
    }
    }
    out->Release();
    return text.isEmpty() ? QStringLiteral("?:unreadable") : text;
}

inline void walkWmi(IWbemServices *services, IWbemClassObject *cls,
                    const QString &root, const QString &rel, View &view)
{
    view.keys.append(rel);
    const QString full = rel.isEmpty() ? root : root + QLatin1Char('\\') + rel;
    if (IWbemClassObject *out = call(services, cls, L"EnumValues", full)) {
        const QStringList names = stringArray(out, L"sNames");
        const QVector<int> types = intArray(out, L"Types");
        out->Release();
        for (int i = 0; i < names.size() && i < types.size(); ++i)
            view.values.insert(rel + QLatin1Char('\\') + names.at(i),
                               valueThroughWmi(services, cls, full,
                                               names.at(i), types.at(i)));
    }
    if (IWbemClassObject *out = call(services, cls, L"EnumKey", full)) {
        const QStringList subs = stringArray(out, L"sNames");
        out->Release();
        for (const QString &sub : subs)
            walkWmi(services, cls, root,
                    rel.isEmpty() ? sub : rel + QLatin1Char('\\') + sub, view);
    }
}

} // namespace detail

inline bool available() { return true; }

// The store AS THIS PROCESS SEES IT (advapi32, exactly what QSettings uses).
inline View readDirect(const QString &subKey)
{
    View view;
    HKEY root = nullptr;
    const LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER,
                                  reinterpret_cast<LPCWSTR>(subKey.utf16()), 0,
                                  KEY_READ, &root);
    if (rc == ERROR_FILE_NOT_FOUND) {
        view.ok = true; // readable, and the answer is "no such key"
        return view;
    }
    if (rc != ERROR_SUCCESS) {
        view.error = QStringLiteral("RegOpenKeyEx failed (%1)").arg(rc);
        return view;
    }
    detail::walkDirect(root, QString(), view);
    RegCloseKey(root);
    view.ok = view.exists = true;
    return view;
}

// The user's REAL store, as the operating system's registry provider - a
// service process outside every package - reads it for this user.
inline View readReal(const QString &subKey)
{
    View view;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = init == S_OK || init == S_FALSE;
    IWbemLocator *locator = nullptr;
    IWbemServices *services = nullptr;
    IWbemClassObject *cls = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                  reinterpret_cast<void **>(&locator));
    if (SUCCEEDED(hr))
        hr = locator->ConnectServer(_bstr_t(L"ROOT\\DEFAULT"), nullptr, nullptr,
                                    nullptr, 0, nullptr, nullptr, &services);
    if (SUCCEEDED(hr))
        hr = CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                               nullptr, RPC_C_AUTHN_LEVEL_CALL,
                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (SUCCEEDED(hr))
        hr = services->GetObject(_bstr_t(L"StdRegProv"), 0, nullptr, &cls,
                                 nullptr);
    if (SUCCEEDED(hr) && cls) {
        // Does the root exist at all, in the real store?
        if (IWbemClassObject *out = detail::call(services, cls, L"EnumKey", subKey)) {
            _variant_t rv;
            const bool answered =
                SUCCEEDED(out->Get(L"ReturnValue", 0, &rv, nullptr, nullptr));
            const long code = answered ? rv.lVal : -1;
            out->Release();
            if (code == 0) {
                detail::walkWmi(services, cls, subKey, QString(), view);
                view.ok = view.exists = true;
            } else if (code == ERROR_FILE_NOT_FOUND) {
                view.ok = true; // readable, and the answer is "no such key"
            } else {
                view.error = QStringLiteral("StdRegProv.EnumKey returned %1")
                                 .arg(code);
            }
        } else {
            view.error = QStringLiteral("StdRegProv did not answer");
        }
    } else {
        view.error = QStringLiteral("WMI unavailable (0x%1)")
                         .arg(quint32(hr), 8, 16, QLatin1Char('0'));
    }
    if (cls)
        cls->Release();
    if (services)
        services->Release();
    if (locator)
        locator->Release();
    if (uninit)
        CoUninitialize();
    return view;
}

#else // not Windows: there is no registry to be isolated from

inline bool available() { return false; }
inline View readDirect(const QString &) { return View(); }
inline View readReal(const QString &) { return View(); }

#endif

inline Verdict compare(const View &direct, const View &real)
{
    Verdict v;
    v.directValues = int(direct.values.size());
    v.realValues = int(real.values.size());
    if (!direct.ok || !real.ok) {
        v.error = !real.ok ? real.error : direct.error;
        return v;
    }
    v.determined = true;
    if (direct.exists != real.exists) {
        // One side has a store and the other has none at all: that is
        // isolation whichever way round it is.
        v.privateCopy = true;
        v.onlyReal = int(real.values.size());
        v.onlyDirect = int(direct.values.size());
        return v;
    }
    for (auto it = direct.values.cbegin(); it != direct.values.cend(); ++it) {
        const auto other = real.values.constFind(it.key());
        if (other == real.values.cend())
            ++v.onlyDirect;
        else if (*other != it.value())
            ++v.differing;
    }
    for (auto it = real.values.cbegin(); it != real.values.cend(); ++it)
        if (!direct.values.contains(it.key()))
            ++v.onlyReal;
    v.privateCopy = v.differing > 0 || v.onlyReal > 0 || v.onlyDirect > 0;
    return v;
}

// Which store is a process looking at when it opens HKCU\<subKey>? Read
// twice when the two views disagree: the real store can change under a
// running comparison (the user's own app saving a setting between the two
// reads), and one changed value must not be reported as isolation.
inline Verdict inspect(const QString &subKey)
{
    Verdict v = compare(readDirect(subKey), readReal(subKey));
    if (v.determined && v.privateCopy)
        v = compare(readDirect(subKey), readReal(subKey));
    return v;
}

} // namespace realstore
