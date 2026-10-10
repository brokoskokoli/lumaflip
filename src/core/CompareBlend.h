// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Difference overlay and blending of two images (F-GRID-04). The compare view renders both
// images at the size they are shown (screen sized) and combines them here on the CPU:
//   - Difference: |a - b| per channel, multiplied by `amount` (1 = plain difference; larger
//     values make small differences visible), clipped.
//   - Blend: a * (1 - t) + b * t.
//   - Wipe (split): left of `t` (0..1 of the width) shows a, right of it b.
// Both inputs must have the same size; the result is ARGB32 (opaque where both are).

#include <QImage>
#include <QRgb>
#include <QString>

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

} // namespace lf::compare
