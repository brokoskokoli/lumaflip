// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Default list of image file suffixes (F-OPEN-02, F-OPEN-06). The decoder registry
// uses this as its default "is this an image?" check; folder enumeration uses it
// to filter a folder's contents.

#include <QString>
#include <QStringList>

namespace lf {

// `lowerSuffix` without dot, already lower case (e.g. "jpg", "cr3").
bool isImageSuffix(const QString& lowerSuffix);
// Convenience: checks the suffix of a file name/path (case-insensitive).
bool hasImageSuffix(const QString& fileName);
// All known suffixes (sorted), e.g. for file dialogs.
QStringList imageSuffixes();
// True for camera RAW suffixes (subset of imageSuffixes()).
bool isRawSuffix(const QString& lowerSuffix);

} // namespace lf
