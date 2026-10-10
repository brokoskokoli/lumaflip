// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Display texts that must not mix languages: the UI is German for now (CLAUDE.md), while
// Qt's and the system's own texts follow the system language ("680 bytes", "JPEG image").
// Sizes are formatted with German units and separators, type names come from a German
// table by file extension / UTType (fallback: the system's description).

#include <QString>
#include <QStringList>

namespace lf {

// "680 Byte", "2,8 MB" (SI units like the Finder).
QString formatByteSize(qint64 bytes);
// Info panel: "2,8 MB (2.812.345 Byte)"; below 1000 bytes only "680 Byte".
QString formatByteSizeExact(qint64 bytes);

// German name of an image type by file extension ("JPEG-Bild", "Kamera-RAW (Canon CR3)"),
// empty if unknown.
QString kindNameForSuffix(const QString& suffix);
// The kind shown for a file: the German name, else `systemKind` (the system's text).
QString displayKind(const QString& fileName, const QString& systemKind);

// An image document type (UTType) for the default-app checklist (F-MAC-04).
struct ImageTypeDescription {
    QString identifier; // "public.jpeg"
    QString name;       // "JPEG-Bild"
    QStringList extensions; // "jpg", "jpeg" … (may be empty)
};
ImageTypeDescription describeImageType(const QString& identifier);

// Platform part (DisplayText_mac.mm): the system's description and file extensions of a
// UTType; empty elsewhere.
QString systemTypeDescription(const QString& identifier);
QStringList systemTypeExtensions(const QString& identifier);

} // namespace lf
