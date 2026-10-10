// SPDX-License-Identifier: GPL-3.0-or-later
// Which XMP sidecar belongs to which image (see Sidecars.h).

#include "Sidecars.h"

#include "ImageFormats.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace lf::sidecars {

namespace {

const QString kXmpLower = QStringLiteral("xmp");

QString nfc(const QString& s) {
    return s.normalized(QString::NormalizationForm_C);
}

QString completeBase(const QString& fileName) {
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 ? fileName : fileName.left(dot);
}

QString suffixLower(const QString& fileName) {
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    return dot <= 0 ? QString() : fileName.mid(dot + 1).toLower();
}

bool isRawName(const QString& fileName) {
    return isRawSuffix(suffixLower(fileName));
}

// Listings of recently used folders, valid while the folder's modification date is the
// same (adding, removing or renaming a file changes it). A folder changed within the last
// two seconds is listed again (the date has a coarse resolution on some volumes).
struct ListingCache {
    QMutex mutex;
    struct Item {
        QDateTime modified;
        Folder folder;
    };
    QHash<QString, Item> items;
    QStringList order;
};

ListingCache& cache() {
    static ListingCache c;
    return c;
}

Folder cachedListing(const QString& folderPath) {
    const QDateTime modified = QFileInfo(folderPath).lastModified(QTimeZone::UTC);
    const bool settled = modified.isValid() && modified.msecsTo(QDateTime::currentDateTimeUtc()) > 2000;
    ListingCache& c = cache();
    if (settled) {
        QMutexLocker g(&c.mutex);
        const auto it = c.items.constFind(folderPath);
        if (it != c.items.cend() && it->modified == modified)
            return it->folder;
    }
    Folder f = Folder::list(folderPath);
    if (settled) {
        QMutexLocker g(&c.mutex);
        if (!c.items.contains(folderPath))
            c.order.append(folderPath);
        c.items.insert(folderPath, {modified, f});
        while (c.order.size() > 8)
            c.items.remove(c.order.takeFirst());
    }
    return f;
}

} // namespace

bool isCaseInsensitiveVolume(const QString& folderPath) {
#if defined(Q_OS_UNIX) && defined(_PC_CASE_SENSITIVE)
    const long v = ::pathconf(QFile::encodeName(folderPath).constData(), _PC_CASE_SENSITIVE);
    if (v == 0)
        return true;
    if (v == 1)
        return false;
#else
    Q_UNUSED(folderPath);
#endif
#if defined(Q_OS_DARWIN) || defined(Q_OS_WIN)
    return true;
#else
    return false;
#endif
}

Folder Folder::list(const QString& folderPath) {
    Folder f;
    f.m_path = folderPath;
    f.m_caseInsensitive = isCaseInsensitiveVolume(folderPath);
    QDirIterator it(folderPath, QDir::Files | QDir::Hidden | QDir::System);
    while (it.hasNext()) {
        it.next();
        const QString name = it.fileName();
        f.m_names.insert(f.fold(name), name);
        if (suffixLower(name) != kXmpLower && hasImageSuffix(name))
            f.m_byBase[f.fold(completeBase(name))].append(name);
    }
    return f;
}

QString Folder::fold(const QString& name) const {
    const QString n = nfc(name);
    return m_caseInsensitive ? n.toCaseFolded() : n;
}

QString Folder::find(const QString& name) const {
    return m_names.value(fold(name));
}

QStringList Folder::imagesWithBase(const QString& baseName) const {
    return m_byBase.value(fold(baseName));
}

bool ownsAdobeSidecar(const QString& imagePath, const Folder& folder) {
    const QString name = QFileInfo(imagePath).fileName();
    if (isRawName(name))
        return true;
    // Another image with the same base name (the RAW of a pair, IMG_1.PNG …): the Adobe name
    // is not this file's.
    const QString self = folder.fold(name);
    for (const QString& other : folder.imagesWithBase(completeBase(name)))
        if (folder.fold(other) != self)
            return false;
    return true;
}

namespace {

QStringList extensionVariants(const QString& imagePath, const Folder& folder) {
    QStringList out;
    const QString name = QFileInfo(imagePath).fileName();
    for (const QString& candidate : {name + QStringLiteral(".xmp"), name + QStringLiteral(".XMP")}) {
        const QString actual = folder.find(candidate);
        const QString p = actual.isEmpty() ? QString() : QDir(folder.path()).filePath(actual);
        if (!p.isEmpty() && !out.contains(p))
            out << p;
    }
    return out;
}

QStringList adobeVariants(const QString& imagePath, const Folder& folder) {
    QStringList out;
    const QString base = completeBase(QFileInfo(imagePath).fileName());
    if (base == QFileInfo(imagePath).fileName())
        return out; // no suffix: "<name>.xmp" is the extension variant
    for (const QString& candidate : {base + QStringLiteral(".xmp"), base + QStringLiteral(".XMP")}) {
        const QString actual = folder.find(candidate);
        const QString p = actual.isEmpty() ? QString() : QDir(folder.path()).filePath(actual);
        if (!p.isEmpty() && !out.contains(p))
            out << p;
    }
    return out;
}

Folder listingFor(const QString& imagePath, const Folder* folder) {
    if (folder)
        return *folder;
    return cachedListing(QFileInfo(imagePath).absolutePath());
}

QStringList readableIn(const QString& imagePath, const Folder& folder) {
    QStringList out = extensionVariants(imagePath, folder);
    if (ownsAdobeSidecar(imagePath, folder))
        for (const QString& p : adobeVariants(imagePath, folder))
            if (!out.contains(p))
                out << p;
    return out;
}

} // namespace

QStringList readable(const QString& imagePath, const Folder* folder) {
    if (!folder) {
        // Most images have no sidecar: four stats instead of a listing.
        const QFileInfo fi(imagePath);
        const QString base = fi.dir().filePath(fi.completeBaseName());
        bool any = false;
        for (const QString& c : {imagePath + QStringLiteral(".xmp"), imagePath + QStringLiteral(".XMP"),
                                 base + QStringLiteral(".xmp"), base + QStringLiteral(".XMP")})
            any = any || QFileInfo(c).isFile();
        if (!any)
            return {};
    }
    return readableIn(imagePath, listingFor(imagePath, folder));
}

QString forWriting(const QString& imagePath, const Folder* folder) {
    const Folder f = listingFor(imagePath, folder);
    const QStringList existing = readableIn(imagePath, f);
    if (!existing.isEmpty())
        return existing.first();
    const QFileInfo fi(imagePath);
    if (!ownsAdobeSidecar(imagePath, f) || fi.completeBaseName().isEmpty() || fi.suffix().isEmpty())
        return imagePath + QStringLiteral(".xmp");
    return fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".xmp"));
}

QStringList travelling(const QStringList& members, const Folder* folder) {
    QStringList out;
    if (members.isEmpty())
        return out;
    const Folder f = folder ? *folder : Folder::list(QFileInfo(members.first()).absolutePath());
    QSet<QString> memberNames;
    for (const QString& m : members)
        memberNames.insert(f.fold(QFileInfo(m).fileName()));
    for (const QString& m : members)
        for (const QString& p : extensionVariants(m, f))
            if (!out.contains(p))
                out << p;
    QSet<QString> bases;
    for (const QString& m : members) {
        const QString name = QFileInfo(m).fileName();
        const QString base = completeBase(name);
        if (base == name || bases.contains(f.fold(base)) || !ownsAdobeSidecar(m, f))
            continue;
        bases.insert(f.fold(base));
        // Another owner that stays behind keeps it (two RAWs with one base name).
        bool ownedOutside = false;
        for (const QString& other : f.imagesWithBase(base)) {
            if (memberNames.contains(f.fold(other)))
                continue;
            if (ownsAdobeSidecar(QDir(f.path()).filePath(other), f)) {
                ownedOutside = true;
                break;
            }
        }
        if (ownedOutside)
            continue;
        for (const QString& p : adobeVariants(m, f))
            if (!out.contains(p))
                out << p;
    }
    return out;
}

} // namespace lf::sidecars
