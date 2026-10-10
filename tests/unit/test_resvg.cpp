// SPDX-License-Identifier: GPL-3.0-or-later
// SVG via resvg (SPEC 4.4 "Vektor", M3): intrinsic size, rendering at the requested size,
// CSS selectors, filters, gradients and text (system fonts), QtSvg as the fallback behind it,
// and the difference to QtSvg for CSS that QtSvg ignores. Requirement ID: F-FMT-01.

#include "CorpusTestUtil.h"
#include "core/Decoder.h"
#include "core/QtImageDecoder.h"
#include "core/ResvgDecoder.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <zlib.h>

using namespace lf;
using namespace lf::test;

namespace {

#define LF_REQUIRE_RESVG()                                                                                   \
    do {                                                                                                     \
        if (!::lf::resvgAvailable())                                                                         \
            GTEST_SKIP() << "built without resvg (LF_ENABLE_RESVG)";                                         \
    } while (0)

const QByteArray kQuad = R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
  <rect x="0" y="0" width="100" height="50" fill="#ff0000"/>
  <rect x="100" y="0" width="100" height="50" fill="#00ff00"/>
  <rect x="0" y="50" width="100" height="50" fill="#0000ff"/>
  <rect x="100" y="50" width="100" height="50" fill="#ffffff"/>
</svg>
)svg";

// CSS that QtSvg ignores (attribute selectors, child combinator with :first-child): QtSvg
// shows the gray presentation attributes there, resvg the CSS colours.
const QByteArray kCss = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <style>
    rect[data-q="tl"] { fill: #ff0000; }
    g.top > rect:first-child + rect, rect[data-q="tr"] { fill: #00ff00; }
    #bl { fill: #0000ff; }
    .br { fill: #ffffff; }
  </style>
  <g class="top">
    <rect data-q="tl" x="0" y="0" width="100" height="50" fill="#808080"/>
    <rect data-q="tr" x="100" y="0" width="100" height="50" fill="#808080"/>
  </g>
  <rect id="bl" x="0" y="50" width="100" height="50" fill="#808080"/>
  <rect class="br" x="100" y="50" width="100" height="50" fill="#808080"/>
</svg>
)svg";

const QByteArray kBlur = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <defs><filter id="b" x="-50%" y="-50%" width="200%" height="200%">
    <feGaussianBlur stdDeviation="8"/></filter></defs>
  <rect width="200" height="100" fill="#ffffff"/>
  <rect x="60" y="20" width="80" height="60" fill="#0000ff" filter="url(#b)"/>
</svg>
)svg";

const QByteArray kGradient = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
  <defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>
  </linearGradient></defs>
  <rect width="200" height="100" fill="url(#g)"/>
</svg>
)svg";

const QByteArray kText = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="300" height="100">
  <rect width="300" height="100" fill="#ffffff"/>
  <text x="10" y="75" font-family="Helvetica, Arial, sans-serif" font-size="72" fill="#000000">Lumaflip</text>
</svg>
)svg";

Rgba at(const QImage& img, int x, int y) {
    const QColor c = img.pixelColor(x, y);
    return {c.red(), c.green(), c.blue(), c.alpha()};
}

QString writeTemp(const QTemporaryDir& dir, const QString& name, const QByteArray& data) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
    return path;
}

QByteArray gzip(const QByteArray& data) {
    z_stream zs{};
    deflateInit2(&zs, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(qsizetype(deflateBound(&zs, uLong(data.size())) + 32), '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    zs.avail_in = uInt(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(qsizetype(zs.total_out));
    deflateEnd(&zs);
    return out;
}

} // namespace

TEST(ResvgDecoder, F_FMT_01_ResvgIntrinsicSizeAndQuadrants) {
    LF_REQUIRE_RESVG();
    QString err;
    const auto size = ResvgDecoder::intrinsicSize(kQuad, &err);
    ASSERT_TRUE(size.has_value()) << err.toStdString();
    EXPECT_EQ(*size, QSize(200, 100));
    const QImage img = ResvgDecoder::render(kQuad, QString(), QSize(400, 200), &err);
    ASSERT_FALSE(img.isNull()) << err.toStdString();
    EXPECT_EQ(img.format(), QImage::Format_RGBA8888_Premultiplied);
    EXPECT_EQ(img.colorSpace(), QColorSpace(QColorSpace::SRgb));
    expectQuadrants(img, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
}

TEST(ResvgDecoder, F_FMT_01_ResvgCssSelectorsWhereQtSvgDiffers) {
    LF_REQUIRE_RESVG();
    QString err;
    const QImage img = ResvgDecoder::render(kCss, QString(), QSize(200, 100), &err);
    ASSERT_FALSE(img.isNull()) << err.toStdString();
    expectQuadrants(img, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
    // QtSvg (SVG Tiny 1.2) ignores the attribute selectors: the top quadrants stay gray.
    QTemporaryDir dir;
    const QString path = writeTemp(dir, QStringLiteral("css.svg"), kCss);
    SvgDecoder qt;
    DecodeRequest qreq;
    qreq.maxPixelSize = 200;
    const auto qtImg = qt.decode(path, qreq, &err);
    ASSERT_TRUE(qtImg.has_value()) << err.toStdString();
    const Rgba qtTl = at(qtImg->image, 50, 25);
    EXPECT_NEAR(qtTl[0], 128, 3);
    EXPECT_NEAR(qtTl[1], 128, 3);
    const Rgba ours = at(img, 50, 25);
    EXPECT_GT(ours[0], 250);
    EXPECT_LT(ours[1], 5);
}

TEST(ResvgDecoder, F_FMT_01_ResvgGaussianBlurFilter) {
    LF_REQUIRE_RESVG();
    QString err;
    const QImage img = ResvgDecoder::render(kBlur, QString(), QSize(200, 100), &err);
    ASSERT_FALSE(img.isNull()) << err.toStdString();
    const Rgba center = at(img, 100, 50);
    EXPECT_LT(center[0], 40); // still blue inside
    EXPECT_GT(center[2], 230);
    // At the former hard edge the blur gives a mix of blue and white.
    const Rgba edge = at(img, 60, 50);
    EXPECT_GT(edge[0], 60);
    EXPECT_LT(edge[0], 200);
    const Rgba outside = at(img, 10, 10);
    EXPECT_GT(outside[0], 245);
}

TEST(ResvgDecoder, F_FMT_01_ResvgLinearGradient) {
    LF_REQUIRE_RESVG();
    QString err;
    const QImage img = ResvgDecoder::render(kGradient, QString(), QSize(200, 100), &err);
    ASSERT_FALSE(img.isNull());
    const Rgba left = at(img, 1, 50), mid = at(img, 100, 50), right = at(img, 198, 50);
    EXPECT_GT(left[0], 245);
    EXPECT_LT(left[2], 10);
    EXPECT_GT(right[2], 245);
    EXPECT_LT(right[0], 10);
    EXPECT_NEAR(mid[0], 128, 6);
    EXPECT_NEAR(mid[2], 128, 6);
}

TEST(ResvgDecoder, F_FMT_01_ResvgTextWithSystemFonts) {
    LF_REQUIRE_RESVG();
    QString err;
    const QImage img = ResvgDecoder::render(kText, QString(), QSize(300, 100), &err);
    ASSERT_FALSE(img.isNull()) << err.toStdString();
    EXPECT_TRUE(ResvgDecoder::systemFontsLoaded());
    int dark = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qGray(img.pixel(x, y)) < 64)
                ++dark;
    EXPECT_GT(dark, 1000) << "the text was not rendered (no fonts?)";
}

TEST(ResvgDecoder, F_FMT_01_ResvgBackendSizesAndFallback) {
    LF_REQUIRE_RESVG();
    QTemporaryDir dir;
    const QString path = writeTemp(dir, QStringLiteral("quad.svg"), kQuad);
    // resvg comes before QtSvg; QtSvg stays registered as the fallback.
    const auto candidates = DecoderRegistry::instance().candidates(path);
    ASSERT_GE(candidates.size(), 2);
    EXPECT_EQ(candidates[0]->name(), QStringLiteral("resvg"));
    EXPECT_EQ(candidates[1]->name(), QStringLiteral("QtSvg"));

    ResvgDecoder dec;
    QString err;
    const auto info = dec.info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->displaySize, QSize(200, 100)); // intrinsic size (status bar, info panel)
    EXPECT_EQ(info->formatName, QStringLiteral("SVG"));
    auto img = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->image.size(), QSize(2048, 1024)); // default longer edge
    EXPECT_FALSE(img->isPreview);
    DecodeRequest small;
    small.maxPixelSize = 400;
    img = dec.decode(path, small, &err);
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->image.size(), QSize(400, 200));
    EXPECT_TRUE(img->isPreview);
    expectQuadrants(img->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
    DecodeRequest budget; // P-08: 4 bytes per pixel, at most 20000 pixels
    budget.maxBytes = 80000;
    img = dec.decode(path, budget, &err);
    ASSERT_TRUE(img.has_value());
    EXPECT_LE(img->image.sizeInBytes(), 80000);
    // SVGZ.
    const QString svgz = writeTemp(dir, QStringLiteral("quad.svgz"), gzip(kQuad));
    EXPECT_TRUE(dec.canDecode(svgz, fileHeader(svgz)));
    img = dec.decode(svgz, small, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    expectQuadrants(img->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
    // Through the registry (resvg wins).
    img = DecoderRegistry::instance().decode(path, small, &err);
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->image.size(), QSize(400, 200));
}

TEST(ResvgDecoder, F_FMT_01_ResvgMalformed) {
    LF_REQUIRE_RESVG();
    QString err;
    EXPECT_FALSE(ResvgDecoder::intrinsicSize(QByteArray("<svg"), &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(ResvgDecoder::intrinsicSize(QByteArray("not xml at all"), &err).has_value());
    err.clear();
    EXPECT_FALSE(
        ResvgDecoder::intrinsicSize(
            QByteArray(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0"/>)svg"), &err)
            .has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(ResvgDecoder::render(kQuad, QString(), QSize(0, 10), &err).isNull());
    EXPECT_TRUE(ResvgDecoder::render(kQuad, QString(), QSize(ResvgDecoder::kMaxEdge + 1, 10), &err).isNull());
    // Truncations of a valid document never crash.
    for (qsizetype n = 0; n < kCss.size(); n += 7) {
        QString e;
        const QImage img = ResvgDecoder::render(kCss.left(n), QString(), QSize(50, 25), &e);
        if (img.isNull())
            EXPECT_FALSE(e.isEmpty());
    }
}

TEST(ResvgDecoder, F_FMT_01_ResvgCorpus) {
    LF_REQUIRE_RESVG();
    LF_REQUIRE_CORPUS();
    const QJsonObject group = expectedCorpus().value(QStringLiteral("resvg")).toObject();
    ASSERT_FALSE(group.isEmpty());
    ResvgDecoder dec;
    for (auto it = group.begin(); it != group.end(); ++it) {
        SCOPED_TRACE(it.key().toStdString());
        const QJsonObject e = it.value().toObject();
        const QString path = corpusFile(it.key());
        ASSERT_TRUE(QFileInfo::exists(path));
        QString err;
        const auto info = dec.info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_EQ(info->displaySize, jsonSize(e.value(QStringLiteral("displaySize"))));
        DecodeRequest req;
        req.maxPixelSize = std::max(info->displaySize.width(), info->displaySize.height());
        const auto img = dec.decode(path, req, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.size(), info->displaySize);
        if (e.contains(QStringLiteral("quadrants")))
            expectQuadrants(img->image, quadrantColors(e.value(QStringLiteral("quadrants"))),
                            e.value(QStringLiteral("tolerance")).toInt(3));
        if (e.contains(QStringLiteral("minDarkPixels"))) {
            int dark = 0;
            for (int y = 0; y < img->image.height(); ++y)
                for (int x = 0; x < img->image.width(); ++x)
                    dark += qGray(img->image.pixel(x, y)) < 64 ? 1 : 0;
            EXPECT_GE(dark, e.value(QStringLiteral("minDarkPixels")).toInt());
        }
    }
}
