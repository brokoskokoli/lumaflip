// SPDX-License-Identifier: GPL-3.0-or-later
// Type descriptions without UTType (DisplayText.h): the shared-mime-info database knows
// the common image types under their MIME names; the UTType identifiers the app uses map to
// those for the names and suffixes.

#include "app/DisplayText.h"

#include <QHash>
#include <QMimeDatabase>

namespace lf {

namespace {

QString mimeFor(const QString& identifier) {
    static const QHash<QString, QString> map = {
        {QStringLiteral("public.jpeg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("public.png"), QStringLiteral("image/png")},
        {QStringLiteral("public.tiff"), QStringLiteral("image/tiff")},
        {QStringLiteral("com.compuserve.gif"), QStringLiteral("image/gif")},
        {QStringLiteral("com.microsoft.bmp"), QStringLiteral("image/bmp")},
        {QStringLiteral("org.webmproject.webp"), QStringLiteral("image/webp")},
        {QStringLiteral("public.heic"), QStringLiteral("image/heic")},
        {QStringLiteral("public.heif"), QStringLiteral("image/heif")},
        {QStringLiteral("public.avif"), QStringLiteral("image/avif")},
        {QStringLiteral("public.jpeg-xl"), QStringLiteral("image/jxl")},
        {QStringLiteral("public.svg-image"), QStringLiteral("image/svg+xml")},
        {QStringLiteral("com.ilm.openexr-image"), QStringLiteral("image/x-exr")},
        {QStringLiteral("com.adobe.photoshop-image"), QStringLiteral("image/vnd.adobe.photoshop")},
        {QStringLiteral("com.adobe.pdf"), QStringLiteral("application/pdf")},
    };
    if (identifier.contains(QLatin1Char('/')))
        return identifier; // already a MIME type
    return map.value(identifier);
}

} // namespace

QString systemTypeDescription(const QString& identifier) {
    const QString mime = mimeFor(identifier);
    if (mime.isEmpty())
        return {};
    const QMimeType t = QMimeDatabase().mimeTypeForName(mime);
    return t.isValid() ? t.comment() : QString();
}

QStringList systemTypeExtensions(const QString& identifier) {
    const QString mime = mimeFor(identifier);
    if (mime.isEmpty())
        return {};
    const QMimeType t = QMimeDatabase().mimeTypeForName(mime);
    QStringList out;
    for (const QString& s : t.suffixes())
        if (!out.contains(s.toLower()))
            out << s.toLower();
    return out;
}

} // namespace lf
