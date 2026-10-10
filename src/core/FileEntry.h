// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QPointF>
#include <QString>
#include <optional>

namespace lf {

// One image file in a Collection. Attributes are filled lazily/cheaply where possible;
// fields that are unknown stay at their "empty" value.
struct FileEntry {
    QString path;     // absolute, canonical path
    QString name;     // file name incl. extension
    qint64 size = -1; // bytes
    QDateTime modified;
    QDateTime created;
    QDateTime added;                           // "Date Added" (macOS: NSURLAddedToDirectoryDateKey)
    std::optional<QDateTime> captured;         // EXIF DateTimeOriginal, filled on demand
    QString kind;                              // localized type description ("JPEG image")
    std::optional<QPointF> finderIconPosition; // from .DS_Store Iloc, icon view "none"
    int rating = -1;                           // -1 unknown, 0..5; filled on demand (M3)
    qint64 pixelCount = -1;                    // filled on demand
    // True once FileAttributeProvider::fillImageProperties() ran for this entry (captured,
    // pixelCount and rating then hold whatever the file provides).
    bool imagePropertiesRead = false;
};

} // namespace lf
