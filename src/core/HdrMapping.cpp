// SPDX-License-Identifier: GPL-3.0-or-later
#include "HdrMapping.h"

#include <QFloat16>
#include <QImage>

#include <algorithm>
#include <cmath>

namespace lf::hdr {

namespace {

// SMPTE ST 2084 constants.
constexpr float kM1 = 2610.0f / 16384.0f;
constexpr float kM2 = 2523.0f / 4096.0f * 128.0f;
constexpr float kC1 = 3424.0f / 4096.0f;
constexpr float kC2 = 2413.0f / 4096.0f * 32.0f;
constexpr float kC3 = 2392.0f / 4096.0f * 32.0f;
constexpr float kPqMaxNits = 10000.0f;

} // namespace

float pqEncode(float nits) {
    const float y = std::clamp(nits / kPqMaxNits, 0.0f, 1.0f);
    const float p = std::pow(y, kM1);
    return std::pow((kC1 + kC2 * p) / (1.0f + kC3 * p), kM2);
}

float pqDecode(float signal) {
    const float e = std::pow(std::clamp(signal, 0.0f, 1.0f), 1.0f / kM2);
    const float num = std::max(e - kC1, 0.0f);
    const float den = kC2 - kC3 * e;
    return kPqMaxNits * std::pow(num / den, 1.0f / kM1);
}

ToneCurve makeToneCurve(float contentPeak, float target) {
    ToneCurve c;
    c.target = std::max(1.0f, std::isfinite(target) ? target : 1.0f);
    if (!std::isfinite(contentPeak) || contentPeak <= c.target)
        return c; // fits: identity
    c.srcPq = pqEncode(contentPeak * kReferenceWhiteNits);
    const float dstPq = pqEncode(c.target * kReferenceWhiteNits);
    if (c.srcPq <= dstPq) // both beyond the PQ range: nothing to compress
        return c;
    c.active = true;
    c.maxLumPq = dstPq / c.srcPq;
    // Since target >= 1 (203 nits, PQ 0.58) and srcPq <= 1, maxLumPq >= 0.58 and KS >= 0.37:
    // the Hermite spline below is monotonic.
    c.kneePq = std::max(0.0f, 1.5f * c.maxLumPq - 0.5f);
    c.knee = pqDecode(c.kneePq * c.srcPq) / kReferenceWhiteNits;
    return c;
}

float toneMap(const ToneCurve& c, float x) {
    if (!c.active || x <= c.knee)
        return x;
    const float e1 = pqEncode(x * kReferenceWhiteNits) / c.srcPq;
    if (e1 >= 1.0f)
        return c.target;
    const float ks = c.kneePq;
    const float t = (e1 - ks) / (1.0f - ks);
    const float t2 = t * t, t3 = t2 * t;
    const float p = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) + (-2 * t3 + 3 * t2) * c.maxLumPq;
    const float out = pqDecode(p * c.srcPq) / kReferenceWhiteNits;
    return std::clamp(out, c.knee, c.target);
}

std::array<float, 3> toneMapRgb(const ToneCurve& c, std::array<float, 3> rgb) {
    if (!c.active)
        return rgb;
    const float m = std::max({rgb[0], rgb[1], rgb[2]});
    if (m <= c.knee)
        return rgb;
    const float gain = toneMap(c, m) / m;
    for (float& v : rgb)
        v *= gain;
    return rgb;
}

// ---- Exposure ----

bool Exposure::setEv(double ev) {
    if (!std::isfinite(ev))
        return false;
    const double v = std::round(std::clamp(ev, kMinEv, kMaxEv) * 100.0) / 100.0;
    if (v == m_ev)
        return false;
    m_ev = v == 0.0 ? 0.0 : v; // no negative zero
    return true;
}

bool Exposure::step(int steps) {
    // Snap to the third-stop grid first so mixed slider/keyboard use stays on it.
    const double thirds = std::round(m_ev / kStepEv);
    return setEv((thirds + steps) * kStepEv);
}

bool Exposure::reset() {
    return setEv(0.0);
}

bool Exposure::imageChanged() {
    return m_locked ? false : reset();
}

float Exposure::multiplier() const {
    return float(std::exp2(m_ev));
}

// ---- decisions ----

bool isHdrContent(float contentPeak, bool floatSource) {
    return floatSource || (std::isfinite(contentPeak) && contentPeak > kHdrThreshold);
}

Decision decide(float contentPeak, bool floatSource, bool hdrEnabled, const DisplayState& display) {
    Decision d;
    if (!isHdrContent(contentPeak, floatSource))
        return d; // Sdr, target 1
    d.badge = true;
    const bool edrCapable = display.potentialHeadroom > kHdrThreshold;
    if (hdrEnabled && edrCapable) {
        d.presentation = Presentation::Extended;
        d.useExtendedSurface = true;
        d.targetHeadroom = std::max(1.0f, display.currentHeadroom);
    } else {
        d.presentation = Presentation::ToneMapped;
        d.targetHeadroom = 1.0f;
    }
    return d;
}

bool headroomChanged(float previous, float now) {
    if (!std::isfinite(previous) || !std::isfinite(now))
        return previous != now;
    return std::abs(now - previous) > 0.01f * std::max(previous, 1.0f);
}

float measuredPeak(const QImage& image) {
    float peak = 0.0f;
    const bool premul = image.format() == QImage::Format_RGBA16FPx4_Premultiplied ||
                        image.format() == QImage::Format_RGBA32FPx4_Premultiplied;
    auto take = [&](float r, float g, float b, float a) {
        float m = std::max({r, g, b});
        if (premul)
            m = a > 0.0f ? m / a : 0.0f;
        if (std::isfinite(m))
            peak = std::max(peak, m);
    };
    switch (image.format()) {
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBX16FPx4:
        for (int y = 0; y < image.height(); ++y) {
            const auto* p = reinterpret_cast<const qfloat16*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x, p += 4)
                take(float(p[0]), float(p[1]), float(p[2]), float(p[3]));
        }
        return std::max(peak, 0.0f);
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
    case QImage::Format_RGBX32FPx4:
        for (int y = 0; y < image.height(); ++y) {
            const auto* p = reinterpret_cast<const float*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x, p += 4)
                take(p[0], p[1], p[2], p[3]);
        }
        return std::max(peak, 0.0f);
    default:
        return 1.0f;
    }
}

} // namespace lf::hdr
