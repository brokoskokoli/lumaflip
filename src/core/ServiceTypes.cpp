// SPDX-License-Identifier: GPL-3.0-or-later
#include "ServiceTypes.h"

#include "VirtualPath.h"

#include <QDir>
#include <QSet>

namespace lf::services {

namespace {
bool isRealFile(const QString& path) {
    return !path.isEmpty() && !vpath::isMember(path);
}
} // namespace

QStringList sendTypes() {
    return {QString::fromLatin1(kFileUrlType), QString::fromLatin1(kTiffType), QString::fromLatin1(kPngType)};
}

bool canProvide(const QString& sendType, const QString& returnType, const Context& context) {
    if (!returnType.isEmpty())
        return false; // no service may replace the image
    if (sendType == QLatin1String(kFileUrlType))
        return isRealFile(context.path); // an archive member has no file of its own
    if (sendType == QLatin1String(kTiffType) || sendType == QLatin1String(kPngType))
        return !context.path.isEmpty() && context.imageAvailable;
    return false;
}

QStringList typesToWrite(const QStringList& requested, const Context& context) {
    QStringList out;
    for (const QString& t : requested)
        if (canProvide(t, QString(), context) && !out.contains(t))
            out << t;
    return out;
}

QStringList filesFromUrls(const QList<QUrl>& urls) {
    QStringList out;
    QSet<QString> seen;
    for (const QUrl& u : urls) {
        if (!u.isLocalFile())
            continue;
        const QString p = QDir::cleanPath(u.toLocalFile());
        if (p.isEmpty() || seen.contains(p))
            continue;
        seen.insert(p);
        out << p;
    }
    return out;
}

QStringList openServiceFileTypes() {
    return {QStringLiteral("public.image"),
            QStringLiteral("public.folder"),
            QStringLiteral("public.zip-archive"),
            QStringLiteral("com.rarlab.rar-archive"),
            QStringLiteral("org.7-zip.7-zip-archive"),
            QStringLiteral("com.adobe.pdf")};
}

} // namespace lf::services
