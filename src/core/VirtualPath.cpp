// SPDX-License-Identifier: GPL-3.0-or-later
#include "VirtualPath.h"

#include "ImageFormats.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QMutex>

namespace lf::vpath {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::VirtualPath)
};

QString suffixOf(const QString& path) {
    const qsizetype slash = path.lastIndexOf(u'/');
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot < 0 || dot < slash || dot == path.size() - 1)
        return {};
    return path.mid(dot + 1).toLower();
}

// A real folder whose name ends in a container suffix and "#" ("Band.zip#"): its contents
// are ordinary files, never members. Remembered per path (bounded), so the file system is asked once.
bool isRealFolder(const QString& container) {
    static QMutex mutex;
    static QHash<QString, bool> known;
    {
        QMutexLocker lock(&mutex);
        if (const auto it = known.constFind(container); it != known.cend())
            return *it;
    }
    const bool folder = QFileInfo(container).isDir();
    QMutexLocker lock(&mutex);
    if (known.size() >= 512)
        known.clear();
    known.insert(container, folder);
    return folder;
}

} // namespace

QStringList containerSuffixes() {
    return archiveSuffixes() << QStringLiteral("pdf");
}

QStringList archiveSuffixes() {
    return {QStringLiteral("zip"), QStringLiteral("cbz"), QStringLiteral("rar"),
            QStringLiteral("cbr"), QStringLiteral("7z"),  QStringLiteral("cb7")};
}

ContainerKind kindForSuffix(const QString& s) {
    if (s == QLatin1String("zip") || s == QLatin1String("cbz"))
        return ContainerKind::Zip;
    if (s == QLatin1String("rar") || s == QLatin1String("cbr") || s == QLatin1String("7z") ||
        s == QLatin1String("cb7"))
        return ContainerKind::LibArchive;
    if (s == QLatin1String("pdf"))
        return ContainerKind::Pdf;
    return ContainerKind::None;
}

ContainerKind containerKind(const QString& path) {
    if (path.isEmpty() || isMember(path))
        return ContainerKind::None;
    return kindForSuffix(suffixOf(path));
}

bool isContainerPath(const QString& path) {
    return containerKind(path) != ContainerKind::None;
}

bool isComicArchive(const QString& path) {
    const QString s = suffixOf(containerOf(path));
    return s == QLatin1String("cbz") || s == QLatin1String("cbr") || s == QLatin1String("cb7");
}

std::optional<Parts> split(const QString& path) {
    const QLatin1StringView marker(kMarker);
    qsizetype from = 0;
    while (true) {
        const qsizetype at = path.indexOf(marker, from);
        if (at <= 0)
            return std::nullopt;
        const QString container = path.left(at);
        if (kindForSuffix(suffixOf(container)) != ContainerKind::None && at + marker.size() < path.size() &&
            !isRealFolder(path.left(at + 1)))
            return Parts{container, path.mid(at + marker.size())};
        from = at + 1;
    }
}

bool isMember(const QString& path) {
    return split(path).has_value();
}

QString join(const QString& container, const QString& member) {
    return container + QLatin1StringView(kMarker) + member;
}

QString containerOf(const QString& path) {
    const auto p = split(path);
    return p ? p->container : path;
}

QString pdfPageMember(int index) {
    return QStringLiteral("%1").arg(index + 1, 4, 10, QLatin1Char('0'));
}

int pdfPageIndex(const QString& member) {
    if (member.isEmpty() || member.size() > 9)
        return -1;
    bool ok = false;
    const int n = member.toInt(&ok);
    return ok && n >= 1 && member.at(0).isDigit() ? n - 1 : -1;
}

QString memberFileName(const QString& path) {
    const auto p = split(path);
    if (!p) {
        const qsizetype slash = path.lastIndexOf(u'/');
        return slash < 0 ? path : path.mid(slash + 1);
    }
    if (kindForSuffix(suffixOf(p->container)) == ContainerKind::Pdf) {
        const int page = pdfPageIndex(p->member);
        if (page >= 0)
            //: one page of a PDF document browsed like a folder (F-OPEN-10)
            return Tr::tr("Seite %1").arg(page + 1);
    }
    const qsizetype slash = p->member.lastIndexOf(u'/');
    return slash < 0 ? p->member : p->member.mid(slash + 1);
}

QString displayName(const QString& path) {
    const auto p = split(path);
    if (!p)
        return memberFileName(path);
    const qsizetype slash = p->container.lastIndexOf(u'/');
    const QString archive = slash < 0 ? p->container : p->container.mid(slash + 1);
    QString inner = p->member;
    if (kindForSuffix(suffixOf(p->container)) == ContainerKind::Pdf)
        inner = memberFileName(path);
    //: archive or PDF and the image inside it, e.g. "Band 1.cbz › 012.jpg" (F-OPEN-10)
    return Tr::tr("%1 › %2").arg(archive, inner);
}

bool isImageMemberName(const QString& member) {
    if (member.isEmpty() || member.endsWith(u'/'))
        return false;
    const QStringList parts = member.split(u'/', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    for (const QString& part : parts) {
        if (part.startsWith(u'.') || part == QLatin1String("__MACOSX"))
            return false;
        if (part == QLatin1String(".."))
            return false;
    }
    return hasImageSuffix(parts.last());
}

} // namespace lf::vpath
