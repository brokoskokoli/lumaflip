// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Image file suffixes (F-OPEN-02, F-OPEN-06): the fast "is this an image?" check used
// while enumerating folders.
//
// On macOS the set is derived from the registered decoder backends (ImageIO's readable
// UTTypes and their filename extensions, Qt image plugins, SVG), so suffixes without any
// backend (e.g. xcf, kra, ora, pcx, x3f) are not listed and do not produce error cards.
// Document formats that a backend happens to render (pdf) are excluded. Other platforms
// use the portable default list.

#include <QString>
#include <QStringList>

namespace lf {

// `lowerSuffix` without dot, already lower case (e.g. "jpg", "cr3").
bool isImageSuffix(const QString& lowerSuffix);
// Convenience: checks the suffix of a file name/path (case-insensitive).
bool hasImageSuffix(const QString& fileName);
// All supported suffixes (sorted), e.g. for file dialogs.
QStringList imageSuffixes();
// The portable default list (sorted), independent of the installed backends.
QStringList defaultImageSuffixes();
// True for camera RAW suffixes (subset of imageSuffixes()).
bool isRawSuffix(const QString& lowerSuffix);

} // namespace lf
