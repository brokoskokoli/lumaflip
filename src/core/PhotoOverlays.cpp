// SPDX-License-Identifier: GPL-3.0-or-later
// Photo overlays (F-INFO-07), see PhotoOverlays.h. Keep in sync with
// src/app/shaders/image.frag (applyOverlays()).

#include "PhotoOverlays.h"

#include "Settings.h"

#include <algorithm>
#include <cmath>

namespace lf::overlays {

Params fromSettings() {
    Settings& st = Settings::instance();
    Params p;
    p.clipping = st.value(key::OverlayClipping).toBool();
    p.peaking = st.value(key::OverlayPeaking).toBool();
    p.clipHigh = float(st.value(key::OverlayClipHighPercent).toInt()) / 100.0f;
    p.clipLow = float(st.value(key::OverlayClipLowPercent).toInt()) / 100.0f;
    p.hdrRelativeToHeadroom = st.value(key::OverlayClipHdrReference).toString() != QLatin1String("sdr");
    p.peakingSensitivity = float(st.value(key::OverlayPeakingSensitivity).toInt()) / 100.0f;
    const QColor c = QColor::fromString(st.value(key::OverlayPeakingColor).toString());
    if (c.isValid())
        p.peakingColor = c;
    return p;
}

float srgbToLinear(float c) {
    const float a = std::abs(c);
    const float v = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    return std::copysign(v, c);
}

float linearToSrgb(float c) {
    const float a = std::abs(c);
    const float v = a <= 0.0031308f ? a * 12.92f : 1.055f * std::pow(a, 1.0f / 2.4f) - 0.055f;
    return std::copysign(v, c);
}

float highThreshold(const Params& p, float contentPeak) {
    const float reference = p.hdrRelativeToHeadroom ? std::max(1.0f, contentPeak) : 1.0f;
    return srgbToLinear(std::clamp(p.clipHigh, 0.0f, 1.0f)) * reference;
}

float lowThreshold(const Params& p) {
    return srgbToLinear(std::clamp(p.clipLow, 0.0f, 1.0f));
}

Clip classify(const std::array<float, 3>& rgb, const Params& p, float contentPeak) {
    if (!p.clipping)
        return Clip::None;
    const float m = std::max({rgb[0], rgb[1], rgb[2]});
    if (m >= highThreshold(p, contentPeak))
        return Clip::Highlight;
    if (m <= lowThreshold(p))
        return Clip::Shadow;
    return Clip::None;
}

float luma(const std::array<float, 3>& c) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

float peakingLuma(const std::array<float, 4>& t, bool premultiplied, bool linearSource, float exposure,
                  float headroom) {
    std::array<float, 3> rgb{t[0], t[1], t[2]};
    if (premultiplied) {
        const float a = std::clamp(t[3], 0.0f, 1.0f);
        for (float& c : rgb)
            c = a > 0.0f ? c / a : 0.0f;
    }
    if (linearSource)
        for (float& c : rgb)
            c = linearToSrgb(std::clamp(c * exposure / std::max(headroom, 1.0f), 0.0f, 1.0f));
    return luma(rgb);
}

float sobel(const std::array<float, 9>& l) {
    const float gx = (l[2] + 2.0f * l[5] + l[8]) - (l[0] + 2.0f * l[3] + l[6]);
    const float gy = (l[6] + 2.0f * l[7] + l[8]) - (l[0] + 2.0f * l[1] + l[2]);
    return std::sqrt(gx * gx + gy * gy);
}

float peakingThreshold(float sensitivity) {
    // 0 -> 1.2 (only hard edges), 1 -> 0.12 (fine texture); the maximum of a Sobel
    // magnitude on [0, 1] luma is about 5.7.
    const float s = std::clamp(sensitivity, 0.0f, 1.0f);
    return 1.2f + (0.12f - 1.2f) * s;
}

bool isPeak(const std::array<float, 9>& l, float sensitivity) {
    return sobel(l) > peakingThreshold(sensitivity);
}

std::array<float, 3> apply(const std::array<float, 3>& shown, const std::array<float, 3>& linear,
                           const std::array<float, 9>* neighbourhood, const Params& p, float contentPeak) {
    if (!p.active())
        return shown;
    std::array<float, 3> out = shown;
    if (p.peaking && neighbourhood && isPeak(*neighbourhood, p.peakingSensitivity))
        out = {float(p.peakingColor.redF()), float(p.peakingColor.greenF()), float(p.peakingColor.blueF())};
    switch (classify(linear, p, contentPeak)) {
    case Clip::Highlight:
        out = {float(p.highlightColor.redF()), float(p.highlightColor.greenF()),
               float(p.highlightColor.blueF())};
        break;
    case Clip::Shadow:
        out = {float(p.shadowColor.redF()), float(p.shadowColor.greenF()), float(p.shadowColor.blueF())};
        break;
    case Clip::None:
        break;
    }
    return out;
}

} // namespace lf::overlays
