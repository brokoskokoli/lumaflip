// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Display texts that must not mix languages: they follow the UI language (SPEC 4.16), while
// the system's own texts follow the system language ("JPEG image"). Sizes are formatted
// for the UI language and region (core/LocaleFormat.h, F-I18N-04); type names come from a
// translatable table by file extension / UTType (fallback: the system's description).

#include "core/LocaleFormat.h"

#include <QString>
#include <QStringList>

namespace lf {

// Sizes: formatByteSize() ("680 Byte", "2,8 MB" / "2.8 MB") and formatByteSizeExact()
// ("2,8 MB (2.812.345 Byte)") from core/LocaleFormat.h.

// Name of an image type by file extension in the UI language ("JPEG-Bild", "Kamera-RAW
// (Canon CR3)"), empty if unknown.
QString kindNameForSuffix(const QString& suffix);
// The kind shown for a file: the translated name, else `systemKind` (the system's text).
QString displayKind(const QString& fileName, const QString& systemKind);

// An image document type (UTType) for the default-app checklist (F-MAC-04).
struct ImageTypeDescription {
    QString identifier;     // "public.jpeg"
    QString name;           // "JPEG-Bild" (UI language)
    QStringList extensions; // "jpg", "jpeg" … (may be empty)
};
ImageTypeDescription describeImageType(const QString& identifier);

// Platform part (DisplayText_mac.mm): the system's description and file extensions of a
// UTType; empty elsewhere.
QString systemTypeDescription(const QString& identifier);
QStringList systemTypeExtensions(const QString& identifier);

} // namespace lf
