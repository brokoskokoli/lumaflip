// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AF points from the camera's metadata (F-INFO-07), read from the raw EXIF/TIFF structures
// (ImageIO does not expose the AF parts of the maker notes). Supported:
//   - Canon: MakerNote AFInfo2 (0x0026): all AF areas with size and position, in-focus
//     and selected flags (EOS models since about 2008; positive Y is up).
//   - Nikon: MakerNote AFInfo2 (0x00B7) version 0100: the contrast-detect (live view) AF
//     area. Phase-detect points need a per-model grid and are not shown.
//   - Sony: MakerNote FocusLocation (0x2027): the focus point.
//   - Fujifilm: MakerNote FocusPixel (0x1023): the focus point (also inside RAF files,
//     through the embedded JPEG).
//   - any camera/phone: EXIF SubjectArea (0x9214) / SubjectLocation (0xA214), e.g. iPhone.
// Containers: JPEG (APP1), TIFF based RAW, Fujifilm RAF (embedded JPEG, one level only),
// HEIF/HEIC (the "Exif" item, e.g. iPhone) and Canon CR3 (CMT1-3 boxes: AFInfo2 as above).
// Everything else (Olympus/OM, Panasonic, Pentax, Nikon phase detect, Canon AFInfo of old
// bodies) is reported as "not available" (docs/MANUAL_CHECKS.md lists it).
//
// Coordinates are normalized (0..1) in the stored image (before EXIF orientation);
// toDisplay() applies the orientation.

#include <QByteArray>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

namespace lf::afpoints {

struct Point {
    QRectF rect; // normalized
    bool inFocus = false;
    bool selected = false;
    bool operator==(const Point&) const = default;
};

struct Result {
    QVector<Point> points;
    QString source; // "Canon", "Nikon", "Sony", "Fujifilm", "EXIF" (not translated)
    bool valid() const { return !points.isEmpty(); }
};

// Parses a JPEG, a TIFF-based file (CR2, NEF, ARW, DNG, TIFF), a Fujifilm RAF, a HEIF/HEIC
// or a Canon CR3 in memory. Malformed input gives an empty result (bounds-checked).
Result parse(const QByteArray& data);
// Worker thread: reads the file (memory-mapped) and parses it.
Result read(const QString& path);
// Normalized stored coordinates -> normalized displayed coordinates (EXIF orientation).
QVector<Point> toDisplay(const QVector<Point>& points, int exifOrientation);

} // namespace lf::afpoints
