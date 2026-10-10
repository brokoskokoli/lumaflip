// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RGB + luminance histogram of a decoded image (F-INFO-03). Pure computation, call it on
// a worker thread: large images are sampled on a regular grid (at most `maxSamples`).

#include <QImage>
#include <QtGlobal>

#include <array>

namespace lf {

struct Histogram {
    static constexpr int kBins = 256;
    std::array<quint32, kBins> red{};
    std::array<quint32, kBins> green{};
    std::array<quint32, kBins> blue{};
    std::array<quint32, kBins> luma{}; // Rec. 709 weights on the encoded values
    quint64 samples = 0;               // pixels counted (fully transparent ones are skipped)

    bool isEmpty() const { return samples == 0; }
    // Highest bin of all channels; with `ignoreEnds` bins 0 and 255 (clipped shadows and
    // highlights, which would flatten everything else) are left out unless nothing else is set.
    quint32 maxCount(bool ignoreEnds = true) const;
};

// Luminance bin of an 8-bit RGB value (Rec. 709: 0.2126 R + 0.7152 G + 0.0722 B).
int lumaOf(int r, int g, int b);

// Histogram of `image` in its own encoding (8 bit per channel). Float/HDR images are
// converted to their primaries with the sRGB transfer curve first.
Histogram computeHistogram(const QImage& image, qint64 maxSamples = qint64(1) << 20);

} // namespace lf
