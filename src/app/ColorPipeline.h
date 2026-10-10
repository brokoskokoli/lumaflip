// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Display color management for the image canvas (F-COL-01, SPEC 8.4).
//
// The canvas converts every image from its own color space (QImage::colorSpace(),
// untagged = sRGB) into the *output* color space in the fragment shader:
//
//     encoded src --decode TRC--> linear src --3x3 matrix--> linear out --encode TRC--> out
//
// Transfer functions are evaluated analytically for the common cases (linear,
// sRGB, pure gamma) and through small 1D lookup textures for everything else
// (parametric ICC curves, tables, PQ/HLG, ProPhoto ...). The 3x3 matrix and the
// lookup tables are derived with Qt's own color math (QColorTransform on float
// values), so the result matches QImage::convertedToColorSpace().
//
// Color spaces that are not matrix/TRC based (ICC A2B/B2A lookup profiles, Gray,
// CMYK) cannot be expressed this way; those images are converted on a worker
// thread with QImage::convertedToColorSpace() instead (needsCpuConversion()).

#include <QColorSpace>
#include <QImage>
#include <QVector>

namespace lf {

enum class TransferKind : int { Linear = 0, SRgb = 1, Gamma = 2, Lut = 3 };

struct ColorPipeline {
    bool identity = true; // no conversion needed (fast path)
    TransferKind srcKind = TransferKind::SRgb;
    float srcGamma = 2.2f;
    TransferKind dstKind = TransferKind::SRgb;
    float dstGamma = 2.2f;
    float matrix[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // row-major, linear src -> linear dst
    // Lookup tables, kLutSize RGBA texels each (A unused), only for kind == Lut.
    // src: index = encoded value in [0,1] -> linear.
    // dst: index = sqrt(linear) in [0,1] -> encoded (sqrt spacing keeps shadows precise).
    QVector<float> srcLut;
    QVector<float> dstLut;

    static constexpr int kLutSize = 1024;
};

// Color space of an image as the canvas interprets it (untagged = sRGB).
QColorSpace effectiveSourceColorSpace(const QImage& image);

// True if `src` cannot be handled by the shader (non-RGB or LUT based ICC).
bool needsCpuConversion(const QColorSpace& src);

// Makes `dst` usable as shader target: falls back to a parametric approximation
// with the same primaries (never to plain sRGB if primaries are known).
QColorSpace sanitizeOutputColorSpace(const QColorSpace& dst);

// Shader parameters converting `src` into `dst` (both must be RGB matrix spaces).
ColorPipeline makeColorPipeline(const QColorSpace& src, const QColorSpace& dst);

// Destination half only (shared by all images): encoding TRC of `dst`.
void fillDestinationTransfer(ColorPipeline& p, const QColorSpace& dst);

} // namespace lf
