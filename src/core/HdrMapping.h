// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// HDR display math (F-COL-03/04/05, SPEC 8.4). Pure functions without GPU or platform
// dependencies so they can be unit tested; the fragment shader (app/shaders/image.frag)
// implements the same tone curve and must stay in sync with toneMap()/toneMapRgb().
//
// Units: linear light relative to SDR reference white (1.0 = SDR white, as EDR on macOS
// and DecodedImage::headroom use it). An HDR image's `contentPeak` is its headroom; a
// display's `headroom` is the largest value it can show right now
// (NSScreen.maximumExtendedDynamicRangeColorComponentValue, 1 = SDR only).
//
// Tone mapping is the BT.2390 EETF (Hermite knee) evaluated in the PQ domain on the
// largest colour component (maxRGB: keeps hue and saturation, never clips a channel).
// Content that fits the target (peak <= headroom) is passed through unchanged - so SDR
// images at headroom 1 are bit-identical to an unmapped rendering.

#include <QtGlobal>

#include <array>

class QImage;

namespace lf::hdr {

// Reference white used to place relative values on the PQ curve (ITU-R BT.2408).
inline constexpr float kReferenceWhiteNits = 203.0f;
// Values are "HDR" only above this (rounding noise of SDR decodes stays SDR).
inline constexpr float kHdrThreshold = 1.0f + 1.0f / 256.0f;

// SMPTE ST 2084 (PQ): absolute luminance in nits (0..10000) <-> signal (0..1).
float pqEncode(float nits);
float pqDecode(float signal);

// Precomputed per (content peak, target) pair; uploaded to the shader as uniforms.
struct ToneCurve {
    bool active = false;   // content exceeds the target: compress highlights
    float target = 1.0f;   // largest output value (display headroom)
    float knee = 1.0f;     // linear value below which the curve is the identity
    float srcPq = 0.0f;    // PQ signal of the content peak (normalisation)
    float kneePq = 1.0f;   // KS of BT.2390 (normalised PQ)
    float maxLumPq = 1.0f; // target in normalised PQ
};

// `contentPeak` and `target` are relative values (>= 1 is sensible; smaller values are
// clamped to 1 for the target). Inactive if contentPeak <= target.
ToneCurve makeToneCurve(float contentPeak, float target);

// Applies the curve to one linear relative value (>= 0). Identity below the knee,
// monotonic, never above curve.target.
float toneMap(const ToneCurve& c, float x);

// Scales an RGB triplet (linear) by toneMap(max)/max. Negative components (out of
// gamut) are kept; the result's largest component never exceeds curve.target.
std::array<float, 3> toneMapRgb(const ToneCurve& c, std::array<float, 3> rgb);

// ---- F-COL-05: exposure (display only) ----

class Exposure {
public:
    static constexpr double kMinEv = -8.0;
    static constexpr double kMaxEv = 8.0;
    static constexpr double kStepEv = 1.0 / 3.0; // keyboard/menu step

    double ev() const { return m_ev; }
    // Clamped to [kMinEv, kMaxEv] and rounded to 1/100 EV. Returns true if changed.
    bool setEv(double ev);
    bool step(int steps); // +n thirds
    bool reset();         // back to 0 EV
    bool locked() const { return m_locked; }
    void setLocked(bool locked) { m_locked = locked; }
    // A different image is shown: back to 0 EV unless locked. Returns true if changed.
    bool imageChanged();
    // Linear multiplier 2^ev.
    float multiplier() const;
    bool isNeutral() const { return m_ev == 0.0; }

private:
    double m_ev = 0.0;
    bool m_locked = false;
};

// ---- F-COL-03/04: what to do with an image on a display ----

enum class Presentation {
    Sdr,        // SDR content: shown as before (no HDR handling)
    Extended,   // HDR content with extended dynamic range (EDR surface)
    ToneMapped, // HDR content tone-mapped to SDR (toggle off or display without EDR)
};

struct DisplayState {
    float currentHeadroom = 1.0f;   // available now (changes with brightness)
    float potentialHeadroom = 1.0f; // the display's maximum (> 1: EDR capable)
};

struct Decision {
    Presentation presentation = Presentation::Sdr;
    bool useExtendedSurface = false; // present through the EDR (extended linear P3) surface
    float targetHeadroom = 1.0f;     // tone-map target
    bool badge = false;              // F-COL-04: mark the image as HDR
};

// `contentPeak`: DecodedImage::headroom of the shown image (1 = SDR); `floatSource`: the
// file stores float/extended values (EXR, Radiance) even if the peak is <= 1.
// `hdrEnabled`: the HDR toggle (F-COL-04); `exposureMultiplier`: F-COL-05. The extended
// surface is only used if something exceeds SDR white after the exposure.
Decision decide(float contentPeak, bool floatSource, bool hdrEnabled, const DisplayState& display,
                float exposureMultiplier = 1.0f);

bool isHdrContent(float contentPeak, bool floatSource);

// True if the current EDR headroom moved enough to re-render (1 % or more).
bool headroomChanged(float previous, float now);

// Peak (largest colour component, linear) of an RGBA16F/RGBA32F image's pixels; 1 for
// other formats. For tests and decoders that report no headroom.
float measuredPeak(const QImage& image);

// CPU version of the widget (SDR) pass for small images (the loupe, F-VIEW-11): `image`
// (any format and color space, no space = sRGB) times `exposureMultiplier`, tone-mapped from
// `contentPeak` x exposure to SDR white like the shader, sRGB encoded. Returns
// ARGB32_Premultiplied in sRGB. SDR content at exposure 1 comes out as a plain conversion.
QImage toneMappedSdr(const QImage& image, float contentPeak, float exposureMultiplier);

} // namespace lf::hdr
