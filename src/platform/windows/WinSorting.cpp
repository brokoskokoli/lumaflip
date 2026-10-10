// SPDX-License-Identifier: GPL-3.0-or-later
// Name order, folder order and file attributes on Windows (Platform.h).
//
// Name order (F-SORT-01) like File Explorer: the user's locale, case-insensitive, runs of
// digits compared by value (CompareStringEx with SORT_DIGITSASNUMBERS - the documented
// counterpart of StrCmpLogicalW, which Explorer uses). Ties (names that differ only in case
// or width) are broken by code point order: only identical names compare equal.
//
// Folder order (SPEC 6.1): Explorer keeps its view settings in the "Bags" of the user's
// registry (ShellBags), keyed by folder ID lists and not documented - there is no readable
// per-folder sort for arbitrary folders. Lumaflip uses name order there; a .DS_Store a Mac
// left on a shared or external drive still applies (same as the generic layer).
//
// Kind: the type name Explorer shows ("JPG-Datei"), SHGetFileInfo by extension.

#include "core/FinderViewSettings.h"
#include "core/Platform.h"
#include "platform/windows/WinCom.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QTimeZone>

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace lf {

namespace {

constexpr DWORD kFlags = NORM_IGNORECASE | NORM_LINGUISTIC_CASING | SORT_DIGITSASNUMBERS;

int sign(int v) {
    return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

int systemCompare(const QString& a, const QString& b) {
    const int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, kFlags, win::wstr(a), int(a.size()), win::wstr(b),
                                  int(b.size()), nullptr, nullptr, 0);
    if (r == 0) // error: fall back to an ordinal case-insensitive comparison
        return sign(a.compare(b, Qt::CaseInsensitive));
    return r - CSTR_EQUAL; // CSTR_LESS_THAN = 1, CSTR_EQUAL = 2, CSTR_GREATER_THAN = 3
}

int forcedCompare(const QString& a, const QString& b) {
    int c = systemCompare(a, b);
    if (c == 0)
        c = sign(a.compare(b));
    return c;
}

// Sort keys (LCMapStringEx, same flags): one memcmp per comparison instead of a
// CompareStringEx call (P-05: sorting 10 000 names).
class WinPreparedNames final : public PreparedNames {
public:
    explicit WinPreparedNames(QVector<QString> names) : m_names(std::move(names)) {
        m_keys.reserve(m_names.size());
        for (const QString& n : std::as_const(m_names)) {
            QByteArray key;
            const int size = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_SORTKEY | kFlags, win::wstr(n),
                                           int(n.size()), nullptr, 0, nullptr, nullptr, 0);
            if (size > 0) {
                key.resize(size);
                if (LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_SORTKEY | kFlags, win::wstr(n), int(n.size()),
                                  reinterpret_cast<LPWSTR>(key.data()), size, nullptr, nullptr, 0) != size)
                    key.clear();
            }
            if (key.isEmpty())
                m_valid = false;
            m_keys.append(key);
        }
    }
    int compare(qsizetype i, qsizetype j) const override {
        if (!m_valid)
            return forcedCompare(m_names.at(i), m_names.at(j));
        const QByteArray& a = m_keys.at(i);
        const QByteArray& b = m_keys.at(j);
        const int c = std::memcmp(a.constData(), b.constData(), size_t(std::min(a.size(), b.size())));
        if (c != 0)
            return sign(c);
        if (a.size() != b.size())
            return a.size() < b.size() ? -1 : 1;
        return sign(m_names.at(i).compare(m_names.at(j)));
    }

private:
    QVector<QString> m_names;
    QVector<QByteArray> m_keys;
    bool m_valid = true;
};

class WinNameCollator final : public NameCollator {
public:
    int compare(const QString& a, const QString& b) const override { return forcedCompare(a, b); }
    std::unique_ptr<PreparedNames> prepare(QVector<QString> names) const override {
        return std::make_unique<WinPreparedNames>(std::move(names));
    }
};

class WinFolderOrderProvider final : public FolderOrderProvider {
public:
    SortSpec finderOrder(const QString& folderPath, bool) override {
        // .DS_Store files copied from a Mac still carry the Finder settings; otherwise the
        // default (name order).
        return finder::resolve(finder::readFolderRecords(folderPath), finder::GlobalContext{});
    }
    QHash<QString, QPointF> iconPositions(const QString& folderPath) override {
        return finder::readIconPositions(folderPath);
    }
};

class WinFileAttributeProvider final : public FileAttributeProvider {
public:
    void fill(FileEntry& entry) override {
        if (!entry.created.isValid())
            entry.created = QFileInfo(entry.path).birthTime(QTimeZone::UTC);
        if (entry.kind.isEmpty())
            entry.kind = kindFor(entry.name);
    }

private:
    QString kindFor(const QString& name) {
        const QString suffix = QFileInfo(name).suffix().toLower();
        {
            QMutexLocker lock(&m_mutex);
            auto it = m_kinds.constFind(suffix);
            if (it != m_kinds.cend())
                return *it;
        }
        QString kind;
        {
            win::ComScope com;
            SHFILEINFOW info{};
            const QString pseudo = QStringLiteral("x.") + suffix;
            if (SHGetFileInfoW(win::wstr(pseudo), FILE_ATTRIBUTE_NORMAL, &info, sizeof info,
                               SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
                kind = QString::fromWCharArray(info.szTypeName);
        }
        QMutexLocker lock(&m_mutex);
        m_kinds.insert(suffix, kind);
        return kind;
    }

    QMutex m_mutex;
    QHash<QString, QString> m_kinds;
};

} // namespace

std::unique_ptr<NameCollator> createNameCollator() {
    return std::make_unique<WinNameCollator>();
}

std::unique_ptr<FolderOrderProvider> createFolderOrderProvider() {
    return std::make_unique<WinFolderOrderProvider>();
}

std::unique_ptr<FileAttributeProvider> createFileAttributeProvider() {
    return std::make_unique<WinFileAttributeProvider>();
}

} // namespace lf
