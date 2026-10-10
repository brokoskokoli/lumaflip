// SPDX-License-Identifier: GPL-3.0-or-later
// HDR display math (core/HdrMapping.h): tone curve, display decisions, exposure.
// Requirements: F-COL-03, F-COL-04, F-COL-05.

#include "core/HdrMapping.h"

#include <QFloat16>
#include <QImage>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace lf;
using namespace lf::hdr;

namespace {

std::vector<float> samples(float maxValue, int n = 4000) {
    std::vector<float> v;
    v.reserve(size_t(n) + 1);
    for (int i = 0; i <= n; ++i)
        v.push_back(maxValue * float(i) / float(n));
    return v;
}

} // namespace

// SDR content (peak 1) on an SDR target (headroom 1) is passed through bit-identically:
// the renderer's output for SDR images must not change (F-COL-03 "SDR identical").
TEST(HdrMapping, F_COL_03_SdrContentIsIdentityAtHeadroom1) {
    const ToneCurve c = makeToneCurve(1.0f, 1.0f);
    EXPECT_FALSE(c.active);
    for (float x : samples(1.0f, 1024)) {
        const auto out = toneMapRgb(c, {x, x * 0.5f, 1.0f - x});
        EXPECT_EQ(out[0], x);
        EXPECT_EQ(out[1], x * 0.5f);
        EXPECT_EQ(out[2], 1.0f - x);
    }
    // Every 8-bit code value, through the curve, stays the same float.
    for (int i = 0; i < 256; ++i) {
        const float v = float(i) / 255.0f;
        EXPECT_EQ(toneMap(c, v), v);
    }
    // Also with a larger target (EDR display showing SDR content).
    EXPECT_FALSE(makeToneCurve(1.0f, 4.0f).active);
    EXPECT_FALSE(makeToneCurve(3.9f, 4.0f).active);
}

TEST(HdrMapping, F_COL_03_ToneCurveMonotonicBoundedAndContinuous) {
    for (float peak : {1.5f, 2.0f, 4.0f, 8.0f, 16.0f, 100.0f, 1000.0f}) {
        for (float target : {1.0f, 1.25f, 2.0f, 4.0f, 8.0f}) {
            SCOPED_TRACE(testing::Message() << "peak " << peak << " target " << target);
            const ToneCurve c = makeToneCurve(peak, target);
            if (peak <= target) {
                EXPECT_FALSE(c.active);
                continue;
            }
            ASSERT_TRUE(c.active);
            EXPECT_GT(c.knee, 0.0f);
            EXPECT_LT(c.knee, target);
            float prev = -1.0f;
            for (float x : samples(peak * 1.5f)) {
                const float y = toneMap(c, x);
                EXPECT_GE(y, prev - 1e-5f) << "x " << x;
                EXPECT_LE(y, target * (1.0f + 1e-5f)) << "x " << x;
                EXPECT_LE(y, x * (1.0f + 1e-4f) + 1e-6f) << "never brightens, x " << x;
                if (x <= c.knee)
                    EXPECT_EQ(y, x);
                prev = y;
            }
            // The content peak lands on the target (BT.2390: P(1) = maxLum).
            EXPECT_NEAR(toneMap(c, peak), target, target * 0.01f);
            // Continuous at the knee.
            const float justAbove = c.knee * 1.001f;
            EXPECT_NEAR(toneMap(c, justAbove), justAbove, c.knee * 0.002f);
        }
    }
}

TEST(HdrMapping, F_COL_03_ToneMapRgbKeepsHueAndClipsNoChannel) {
    const ToneCurve c = makeToneCurve(16.0f, 2.0f);
    const auto out = toneMapRgb(c, {12.0f, 6.0f, 3.0f});
    EXPECT_LE(std::max({out[0], out[1], out[2]}), 2.0f + 1e-5f);
    EXPECT_NEAR(out[1] / out[0], 0.5f, 1e-5f);
    EXPECT_NEAR(out[2] / out[0], 0.25f, 1e-5f);
    // Below the knee nothing changes, also for out-of-gamut (negative) components.
    const auto low = toneMapRgb(c, {0.5f, -0.1f, 0.2f});
    EXPECT_EQ(low[0], 0.5f);
    EXPECT_EQ(low[1], -0.1f);
    EXPECT_EQ(low[2], 0.2f);
}

TEST(HdrMapping, F_COL_03_PqRoundTrip) {
    for (float nits : {0.0f, 0.1f, 1.0f, 100.0f, 203.0f, 1000.0f, 4000.0f, 10000.0f}) {
        const float s = pqEncode(nits);
        EXPECT_GE(s, 0.0f);
        EXPECT_LE(s, 1.0f);
        EXPECT_NEAR(pqDecode(s), nits, std::max(0.01f, nits * 1e-3f));
    }
    EXPECT_NEAR(pqEncode(10000.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(pqEncode(203.0f), 0.58f, 0.01f); // BT.2408 reference white
}

TEST(HdrMapping, F_COL_03_HeadroomChangeThreshold) {
    EXPECT_FALSE(headroomChanged(2.0f, 2.01f));
    EXPECT_TRUE(headroomChanged(2.0f, 2.05f));
    EXPECT_TRUE(headroomChanged(1.0f, 1.6f));
    EXPECT_FALSE(headroomChanged(1.0f, 1.0f));
}

TEST(HdrMapping, F_COL_03_ExtendedSurfaceOnlyForHdrOnEdrDisplays) {
    const DisplayState edr{2.5f, 16.0f};
    const DisplayState sdr{1.0f, 1.0f};
    // HDR image, EDR display, toggle on: extended range up to the current headroom.
    Decision d = decide(8.0f, false, true, edr);
    EXPECT_EQ(d.presentation, Presentation::Extended);
    EXPECT_TRUE(d.useExtendedSurface);
    EXPECT_FLOAT_EQ(d.targetHeadroom, 2.5f);
    // Headroom currently 1 (e.g. just switched on): still the EDR surface (macOS raises the
    // headroom once EDR content is shown), tone-mapped to what is available now.
    d = decide(8.0f, false, true, DisplayState{1.0f, 16.0f});
    EXPECT_TRUE(d.useExtendedSurface);
    EXPECT_FLOAT_EQ(d.targetHeadroom, 1.0f);
    // SDR display: tone-mapped.
    d = decide(8.0f, false, true, sdr);
    EXPECT_EQ(d.presentation, Presentation::ToneMapped);
    EXPECT_FALSE(d.useExtendedSurface);
    EXPECT_FLOAT_EQ(d.targetHeadroom, 1.0f);
    // SDR image: unchanged path everywhere.
    for (const DisplayState& s : {edr, sdr}) {
        d = decide(1.0f, false, true, s);
        EXPECT_EQ(d.presentation, Presentation::Sdr);
        EXPECT_FALSE(d.useExtendedSurface);
        EXPECT_FALSE(d.badge);
        EXPECT_FLOAT_EQ(d.targetHeadroom, 1.0f);
    }
    // Rounding noise of an SDR decode is not HDR.
    EXPECT_FALSE(isHdrContent(1.002f, false));
    EXPECT_TRUE(isHdrContent(1.0f, true)); // float source (EXR) without highlights
    // A float file without highlights is HDR content (badge, exposure) but needs no
    // extended surface - until the exposure pushes it above SDR white.
    d = decide(1.0f, true, true, edr);
    EXPECT_TRUE(d.badge);
    EXPECT_EQ(d.presentation, Presentation::Extended);
    EXPECT_FALSE(d.useExtendedSurface);
    EXPECT_TRUE(decide(1.0f, true, true, edr, 2.0f).useExtendedSurface);
    // Darkened below SDR white: no extended surface either.
    EXPECT_FALSE(decide(3.0f, false, true, edr, 0.25f).useExtendedSurface);
}

TEST(HdrMapping, F_COL_03_MeasuredPeakOfFloatImages) {
    QImage img(4, 2, QImage::Format_RGBA16FPx4_Premultiplied);
    img.fill(Qt::black);
    auto* p = reinterpret_cast<qfloat16*>(img.scanLine(1));
    p[4] = qfloat16(1.5f); // premultiplied with alpha 0.5 -> 3.0
    p[5] = qfloat16(0.2f);
    p[6] = qfloat16(0.1f);
    p[7] = qfloat16(0.5f);
    EXPECT_NEAR(measuredPeak(img), 3.0f, 0.01f);
    EXPECT_FLOAT_EQ(measuredPeak(QImage(2, 2, QImage::Format_RGBA8888)), 1.0f);
}

// F-COL-04: the HDR toggle switches between extended range and the SDR preview; HDR
// images keep their badge in both modes, SDR images never get one.
TEST(HdrMapping, F_COL_04_ToggleAndBadge) {
    const DisplayState edr{3.0f, 8.0f};
    const Decision on = decide(6.0f, false, true, edr);
    const Decision off = decide(6.0f, false, false, edr);
    EXPECT_EQ(on.presentation, Presentation::Extended);
    EXPECT_EQ(off.presentation, Presentation::ToneMapped);
    EXPECT_TRUE(on.badge);
    EXPECT_TRUE(off.badge);
    EXPECT_FALSE(off.useExtendedSurface);
    EXPECT_FLOAT_EQ(off.targetHeadroom, 1.0f);
    // The SDR preview compresses the highlights into SDR white.
    const ToneCurve preview = makeToneCurve(6.0f, off.targetHeadroom);
    EXPECT_TRUE(preview.active);
    EXPECT_LE(toneMap(preview, 6.0f), 1.0f + 1e-6f);
    // Toggling for an SDR image changes nothing.
    EXPECT_EQ(decide(1.0f, false, false, edr).presentation, Presentation::Sdr);
    EXPECT_FALSE(decide(1.0f, false, false, edr).badge);
}

TEST(HdrMapping, F_COL_05_ExposureMath) {
    Exposure e;
    EXPECT_TRUE(e.isNeutral());
    EXPECT_FLOAT_EQ(e.multiplier(), 1.0f);
    EXPECT_TRUE(e.setEv(1.0));
    EXPECT_FLOAT_EQ(e.multiplier(), 2.0f);
    EXPECT_TRUE(e.setEv(-2.0));
    EXPECT_FLOAT_EQ(e.multiplier(), 0.25f);
    EXPECT_FALSE(e.setEv(-2.0)); // unchanged
    // Clamped to the range.
    e.setEv(100.0);
    EXPECT_DOUBLE_EQ(e.ev(), Exposure::kMaxEv);
    e.setEv(-100.0);
    EXPECT_DOUBLE_EQ(e.ev(), Exposure::kMinEv);
    EXPECT_FALSE(e.setEv(std::nan("")));
    // Steps of a third stop, snapping to the grid.
    e.reset();
    e.step(1);
    EXPECT_NEAR(e.ev(), 0.33, 1e-9);
    e.step(2);
    EXPECT_NEAR(e.ev(), 1.0, 1e-9);
    e.setEv(0.5); // slider value between grid points
    e.step(1);
    EXPECT_NEAR(e.ev(), 1.0, 1e-9); // round(0.5 / (1/3)) = 2 -> 3 thirds
    e.step(-3);
    EXPECT_NEAR(e.ev(), 0.0, 1e-9);
    EXPECT_FALSE(std::signbit(e.ev()));
    // Exposure and tone mapping: +1 EV doubles the values, the curve keeps them in range.
    e.setEv(1.0);
    const float peak = 4.0f * e.multiplier();
    const ToneCurve c = makeToneCurve(peak, 2.0f);
    EXPECT_LE(toneMap(c, 4.0f * e.multiplier()), 2.0f + 1e-5f);
    EXPECT_EQ(toneMap(c, 0.1f * e.multiplier()), 0.2f);
}

TEST(HdrMapping, F_COL_05_ExposureResetsOnImageChangeUnlessLocked) {
    Exposure e;
    e.setEv(1.5);
    EXPECT_TRUE(e.imageChanged());
    EXPECT_TRUE(e.isNeutral());
    EXPECT_FALSE(e.imageChanged()); // already neutral
    e.setEv(-1.0);
    e.setLocked(true);
    EXPECT_FALSE(e.imageChanged());
    EXPECT_DOUBLE_EQ(e.ev(), -1.0);
    e.setLocked(false);
    EXPECT_TRUE(e.imageChanged());
    EXPECT_DOUBLE_EQ(e.ev(), 0.0);
}
