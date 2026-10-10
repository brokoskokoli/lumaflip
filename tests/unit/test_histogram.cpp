// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-03: RGB + luminance histogram on synthetic images.

#include "core/Histogram.h"

#include <QColorSpace>
#include <QImage>

#include <gtest/gtest.h>

#include <numeric>

using namespace lf;

namespace {

QImage solid(QColor c, QSize size = QSize(40, 30), QImage::Format fmt = QImage::Format_RGB32) {
    QImage img(size, fmt);
    img.fill(c);
    return img;
}

quint64 sum(const std::array<quint32, Histogram::kBins>& bins) {
    return std::accumulate(bins.begin(), bins.end(), quint64(0));
}

} // namespace

TEST(Histogram, F_INFO_03_PureRed) {
    const Histogram h = computeHistogram(solid(Qt::red));
    const quint32 n = 40 * 30;
    EXPECT_EQ(h.samples, n);
    EXPECT_EQ(h.red[255], n);
    EXPECT_EQ(h.green[0], n);
    EXPECT_EQ(h.blue[0], n);
    EXPECT_EQ(h.luma[lumaOf(255, 0, 0)], n);
    EXPECT_EQ(lumaOf(255, 0, 0), 54); // 0.2126 * 255
    for (const auto* ch : {&h.red, &h.green, &h.blue, &h.luma})
        EXPECT_EQ(sum(*ch), n);
}

TEST(Histogram, F_INFO_03_BlackAndWhite) {
    const Histogram black = computeHistogram(solid(Qt::black));
    EXPECT_EQ(black.red[0], black.samples);
    EXPECT_EQ(black.luma[0], black.samples);
    const Histogram white = computeHistogram(solid(Qt::white));
    EXPECT_EQ(white.green[255], white.samples);
    EXPECT_EQ(white.luma[255], white.samples);

    // Half black, half white: two equal peaks in every channel.
    QImage img = solid(Qt::black, QSize(10, 10));
    for (int y = 0; y < 10; ++y)
        for (int x = 5; x < 10; ++x)
            img.setPixel(x, y, qRgb(255, 255, 255));
    const Histogram h = computeHistogram(img);
    EXPECT_EQ(h.blue[0], 50u);
    EXPECT_EQ(h.blue[255], 50u);
    EXPECT_EQ(h.luma[0], 50u);
    EXPECT_EQ(h.luma[255], 50u);
    // Clipped ends only count for the scale when nothing else is set.
    EXPECT_EQ(h.maxCount(true), 50u);
}

TEST(Histogram, F_INFO_03_GrayLumaAndTransparency) {
    const Histogram gray = computeHistogram(solid(QColor(128, 128, 128)));
    EXPECT_EQ(gray.luma[128], gray.samples);
    // Fully transparent pixels are no part of the picture.
    QImage img(8, 8, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    img.setPixel(0, 0, qRgba(0, 0, 255, 255));
    const Histogram h = computeHistogram(img);
    EXPECT_EQ(h.samples, 1u);
    EXPECT_EQ(h.blue[255], 1u);
    EXPECT_TRUE(computeHistogram(QImage()).isEmpty());
}

TEST(Histogram, F_INFO_03_LargeImagesAreSampled) {
    const Histogram h = computeHistogram(solid(Qt::green, QSize(4000, 3000)), 100000);
    EXPECT_GT(h.samples, 50000u);
    EXPECT_LE(h.samples, 100000u);
    EXPECT_EQ(h.green[255], h.samples);
}

TEST(Histogram, F_INFO_03_FloatImageUsesDisplayCurve) {
    // Linear float (HDR pipeline): mid gray 0.2140 linear is ~128 in sRGB encoding.
    QImage img(16, 16, QImage::Format_RGBA32FPx4);
    img.fill(QColor::fromRgbF(0.2140f, 0.2140f, 0.2140f, 1.0f));
    img.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
    const Histogram h = computeHistogram(img);
    ASSERT_EQ(h.samples, 256u);
    int peak = 0;
    for (int i = 0; i < Histogram::kBins; ++i)
        if (h.luma[i] > h.luma[peak])
            peak = i;
    EXPECT_NEAR(peak, 128, 2);
}
