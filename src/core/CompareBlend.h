// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Difference overlay and blending of two images (F-GRID-04). The compare view renders both
// images at the size they are shown (screen sized) and combines them here on the CPU:
//   - Difference: |a - b| per channel, multiplied by `amount` (1 = plain difference; larger
//     values make small differences visible), clipped.
//   - Blend: a * (1 - t) + b * t.
//   - Wipe (split): left of `t` (0..1 of the width) shows a, right of it b.
// Both inputs must have the same size; the result is ARGB32 (opaque where both are).
// The per-pixel functions work on premultiplied pixels (channels never exceed alpha).
// Placement (partnerPlacement/alignPartner) and rendering (renderLayer) are pure, so the
// compare overlay runs them on a worker thread at screen size.

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRgb>
#include <QSizeF>
#include <QString>
#include <QTransform>

namespace lf::compare {

enum class BlendMode { Off, Difference, Blend, Wipe };
QString blendModeId(BlendMode m); // "off", "difference", "blend", "wipe"
BlendMode blendModeFromId(const QString& id);

QRgb differencePixel(QRgb a, QRgb b, double amount);
QRgb blendPixel(QRgb a, QRgb b, double t);
// Combines `a` and `b` (same size, any format; converted to ARGB32 premultiplied).
// `value`: amount (Difference, >= 1) or t (Blend, Wipe: 0..1).
QImage composite(const QImage& a, const QImage& b, BlendMode mode, double value);
// Statistics of a difference (tests, status line): largest channel difference and the share
// of pixels that differ at all.
struct DiffStats {
    int maxDelta = 0;
    double changedFraction = 0.0;
};
DiffStats differenceStats(const QImage& a, const QImage& b);

// Where the second image lies in the first one's full-size pixel space: fitted into the first
// image with its own aspect ratio and centred (no stretching). B full px -> A full px.
QTransform partnerPlacement(QSizeF fullA, QSizeF fullB);
// The second image's mapping to the output: `bToA` (partnerPlacement) followed by the first
// image's view `aToOut`, then shifted so that the output centre `outCenter` shows the
// image point moved by `centerDelta` (relative to the second image, 0..1 units) and scaled by
// `zoomRatio` around the centre - the pane offsets and own zoom of an ⌥-drag (F-GRID-03).
QTransform alignPartner(const QTransform& bToA, const QTransform& aToOut, QSizeF fullB, QPointF outCenter,
                        QPointF centerDelta, double zoomRatio);
// Draws `src` (any format and color space) with `toOut` (src px -> output px) into an image
// of `outSize`, converts it to sRGB and puts it over `background`. The work is proportional
// to the output size, not to the source (worker thread). ARGB32 premultiplied.
QImage renderLayer(const QImage& src, const QTransform& toOut, QSize outSize, const QColor& background);

} // namespace lf::compare
