// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the portable decoder backends (QImageReader, QtSvg).
// Requirements: F-FMT-01, F-FMT-02, F-VIEW-08.

#include "CorpusTestUtil.h"
#include "core/QtImageDecoder.h"

#include <gtest/gtest.h>

using namespace lf;
using namespace lf::test;

// F-FMT-01: SVG renders at the requested size (vector, also upscaled).
TEST(SvgDecoder, F_FMT_01_RendersAtRequestedSize) {
    LF_REQUIRE_CORPUS();
    SvgDecoder dec;
    const QString path = corpusFile(QStringLiteral("vector.svg"));
    EXPECT_TRUE(dec.canDecode(path, fileHeader(path)));
    QString err;
    auto info = dec.info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->displaySize, QSize(200, 100));
    EXPECT_EQ(info->formatName, QStringLiteral("SVG"));

    DecodeRequest req;
    req.maxPixelSize = 400;
    auto img = dec.decode(path, req, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    EXPECT_EQ(img->image.size(), QSize(400, 200));
    EXPECT_EQ(img->image.colorSpace(), QColorSpace(QColorSpace::SRgb));
    expectQuadrants(img->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);

    auto full = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(full.has_value()) << err.toStdString();
    EXPECT_EQ(full->image.size(), QSize(SvgDecoder::kDefaultLongEdge, SvgDecoder::kDefaultLongEdge / 2));
    EXPECT_FALSE(full->isPreview);
    EXPECT_TRUE(img->isPreview);
}

TEST(SvgDecoder, F_FMT_02_InvalidSvgFails) {
    LF_REQUIRE_CORPUS();
    SvgDecoder dec;
    QString err;
    const QString garbage = corpusFile(QStringLiteral("garbage.jpg"));
    EXPECT_FALSE(dec.canDecode(garbage, fileHeader(garbage)));
    EXPECT_FALSE(dec.decode(garbage, DecodeRequest{}, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    // Raster files are not claimed.
    const QString png = corpusFile(QStringLiteral("srgb.png"));
    EXPECT_FALSE(dec.canDecode(png, fileHeader(png)));
}

TEST(QtImageDecoder, F_FMT_01_DecodesPngAndJpeg) {
    LF_REQUIRE_CORPUS();
    QtImageDecoder dec;
    const auto rgbw = quadrantColors(QJsonValue(QStringLiteral("rgbw")));
    for (const char* name : {"srgb.png", "orient_1.jpg", "png_as_jpg.jpg"}) {
        SCOPED_TRACE(name);
        const QString path = corpusFile(QString::fromLatin1(name));
        EXPECT_TRUE(dec.canDecode(path, fileHeader(path)));
        QString err;
        auto img = dec.decode(path, DecodeRequest{}, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.size(), QSize(64, 48));
        EXPECT_EQ(img->image.format(), QImage::Format_RGBA8888_Premultiplied);
        EXPECT_TRUE(img->image.colorSpace().isValid());
        expectQuadrants(img->image, rgbw, 40);
    }
    // Not claimed: SVG (own backend), garbage.
    const QString svg = corpusFile(QStringLiteral("vector.svg"));
    EXPECT_FALSE(dec.canDecode(svg, fileHeader(svg)));
    const QString garbage = corpusFile(QStringLiteral("garbage.jpg"));
    EXPECT_FALSE(dec.canDecode(garbage, fileHeader(garbage)));
}

TEST(QtImageDecoder, F_VIEW_08_OrientationAndDownsampling) {
    LF_REQUIRE_CORPUS();
    QtImageDecoder dec;
    const auto rgbw = quadrantColors(QJsonValue(QStringLiteral("rgbw")));
    for (int o = 1; o <= 8; ++o) {
        SCOPED_TRACE("orientation " + std::to_string(o));
        const QString path = corpusFile(QStringLiteral("orient_%1.jpg").arg(o));
        QString err;
        auto info = dec.info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_EQ(info->exifOrientation, o);
        EXPECT_EQ(info->displaySize, QSize(64, 48));
        auto img = dec.decode(path, DecodeRequest{}, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.size(), QSize(64, 48));
        expectQuadrants(img->image, rgbw, 40);
    }
    DecodeRequest req;
    req.maxPixelSize = 32;
    QString err;
    auto small = dec.decode(corpusFile(QStringLiteral("orient_6.jpg")), req, &err);
    ASSERT_TRUE(small.has_value()) << err.toStdString();
    EXPECT_EQ(small->image.size(), QSize(32, 24));
    EXPECT_TRUE(small->isPreview);
}

TEST(QtImageDecoder, F_FMT_02_BrokenFilesFail) {
    LF_REQUIRE_CORPUS();
    QtImageDecoder dec;
    for (const char* name : {"empty.jpg", "garbage.jpg"}) {
        SCOPED_TRACE(name);
        QString err;
        EXPECT_FALSE(dec.decode(corpusFile(QString::fromLatin1(name)), DecodeRequest{}, &err).has_value());
        EXPECT_FALSE(err.isEmpty());
        EXPECT_FALSE(dec.info(corpusFile(QString::fromLatin1(name)), &err).has_value());
    }
}

TEST(QtImageDecoder, F_FMT_03_GifFrames) {
    LF_REQUIRE_CORPUS();
    QtImageDecoder dec;
    const QString path = corpusFile(QStringLiteral("anim.gif"));
    QString err;
    auto info = dec.info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_TRUE(info->isAnimated);
    EXPECT_EQ(info->frameCount, 3);
    DecodeRequest req;
    req.frame = 2;
    auto img = dec.decode(path, req, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    const Rgba blue = namedColor(QLatin1Char('b'));
    expectQuadrants(img->image, {blue, blue, blue, blue}, 10);
}
