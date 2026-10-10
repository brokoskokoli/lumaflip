// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-08 without ImageIO: the EXIF GPS position of JPEG and TIFF-based files.

#include "core/FolderMap.h"

namespace lf {

std::optional<GeoPoint> readImageLocation(const QString& path) {
    return geotag::readExifGps(path);
}

} // namespace lf
