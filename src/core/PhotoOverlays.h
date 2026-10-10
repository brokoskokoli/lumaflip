// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Photo overlays (F-INFO-07): clipping warning (highlights red, shadows blue) and focus
// peaking. The GPU shader of ImageCanvas (src/app/shaders/image.frag) does the per-pixel
// work; the functions here are its CPU reference (unit tests) and turn the settings into
// shader parameters.
//
// Clipping is decided in linear light before the HDR tone mapping:
//   - highlight: the largest channel reaches `clipHigh` (as an sRGB-encoded fraction, e.g.
//     98 %) of the reference white. The reference is SDR white (1.0) for SDR content; for
//     HDR content either the content's headroom (what the file can hold, default) or SDR
//     white (shows everything brighter than SDR).
//   - shadow: the largest channel is at or below `clipLow` (sRGB-encoded fraction of SDR
//     white).
// Focus peaking marks pixels whose luma gradient (3x3 Sobel on the displayed image, so it
// follows the zoom) exceeds a threshold derived from the sensitivity.
// With both overlays off the shader takes exactly its previous path (the SDR output stays
// bit-identical).

#include <QColor>

#include <array>

namespace lf::overlays {

struct Params {
    bool clipping = false;
    bool peaking = false;
    float clipHigh = 0.98f; // sRGB-encoded fraction of the reference white
    float clipLow = 0.02f;  // sRGB-encoded fraction of SDR white
    bool hdrRelativeToHeadroom = true;
    float peakingSensitivity = 0.5f; // 0..1
    QColor peakingColor = QColor(255, 32, 32);
    QColor highlightColor = QColor(255, 0, 0);
    QColor shadowColor = QColor(0, 64, 255);
    bool active() const { return clipping || peaking; }
    bool operator==(const Params&) const = default;
};

// From the settings (key::OverlayClipping … key::OverlayPeakingColor).
Params fromSettings();

enum class Clip { None, Highlight, Shadow };

float srgbToLinear(float encoded);
float linearToSrgb(float linear);
// Linear thresholds for content with `contentPeak` (headroom x exposure, 1 = SDR).
float highThreshold(const Params& p, float contentPeak);
float lowThreshold(const Params& p);
// `rgb`: linear light in the source's color space (before the gamut conversion into the
// output space, which would make saturated colors look clipped), 1.0 = SDR white (after
// exposure, before tone mapping).
Clip classify(const std::array<float, 3>& rgb, const Params& p, float contentPeak);

// Rec. 709 luma of encoded RGB.
float luma(const std::array<float, 3>& rgb);
// The luma focus peaking works on, from a texel as the shader samples it: premultiplied
// texels are un-premultiplied; linear sources (float, HDR) are scaled by the exposure,
// normalised to `headroom` (the largest value the display shows, >= 1) and sRGB-encoded, so
// the threshold means the same as for encoded images.
float peakingLuma(const std::array<float, 4>& texel, bool premultiplied, bool linearSource, float exposure,
                  float headroom);
// Sobel gradient magnitude of a 3x3 neighbourhood (row-major, l[4] = center).
float sobel(const std::array<float, 9>& l);
// Gradient magnitude from which a pixel counts as an edge (sensitivity 0 = only strong
// edges, 1 = fine texture too).
float peakingThreshold(float sensitivity);
bool isPeak(const std::array<float, 9>& l, float sensitivity);

// The color shown for one pixel: `shown` (output-encoded, what the view draws without
// overlays), `linear` (see classify()), `neighbourhood` (luma 3x3 or null). Returns `shown`
// unchanged when no overlay applies.
std::array<float, 3> apply(const std::array<float, 3>& shown, const std::array<float, 3>& linear,
                           const std::array<float, 9>* neighbourhood, const Params& p, float contentPeak);

} // namespace lf::overlays
