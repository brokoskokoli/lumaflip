// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Copying a small image into part of a larger one without touching the rest (the EDR
// overlay, app/ImageCanvas: only the changed part of the view travels per change, P-05).

#include <QImage>
#include <QPoint>
#include <QRect>

namespace lf {

// Copies `patch` into `dst` with its top left corner at `at` (clipped to `dst`), converted
// to dst's format if needed; the device pixel ratio is ignored (both in device pixels).
// Returns the written rectangle of `dst` (empty if nothing overlaps). Writes rows in place:
// `dst` is only detached if it was shared.
QRect copyImagePatch(QImage& dst, const QImage& patch, QPoint at);

} // namespace lf
