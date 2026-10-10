// SPDX-License-Identifier: GPL-3.0-or-later
// HDR decoding as the image provider requests it (F-COL-03, F-COL-05).
// Requirements: F-COL-03, F-COL-05.

#if defined(__APPLE__)

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/HdrMapping.h"
#include "core/ImageProvider.h"
#include "platform/mac/ImageIODecoder.h"

#include <gtest/gtest.h>

#include <memory>

using namespace lf;
using namespace lf::test;

// Screen and Full stages ask for extended range when a display can show it; previews never
// (P-04). Float sources keep their range in any case (exposure control, F-COL-05).
TEST(HdrDecode, F_COL_03_ProviderRequestsHdrForScreenAndFullStages) {
    for (bool edr : {false, true}) {
        const DecodeRequest preview = ImageProvider::requestFor(Quality::Preview, 320, edr);
        EXPECT_FALSE(preview.hdr);
        EXPECT_FALSE(preview.hdrFloat);
        EXPECT_EQ(preview.maxPixelSize, 320);
        for (Quality q : {Quality::Screen, Quality::Full}) {
            const DecodeRequest r = ImageProvider::requestFor(q, q == Quality::Full ? 0 : 2560, edr);
            EXPECT_EQ(r.hdr, edr);
            EXPECT_TRUE(r.hdrFloat);
        }
    }
}

// Gain-map images (ISO 21496-1 HEIC and Ultra HDR style JPEG) decode to values above SDR
// white with the provider's request on an EDR system; SDR files stay 8 bit.
TEST(HdrDecode, F_COL_03_GainMapDecodeExceedsSdrWhite) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    int tested = 0;
    for (const char* name : {"gainmap.heic", "gainmap.jpg", "pq.avif"}) {
        const QString path = corpusFile(QString::fromLatin1(name));
        if (!QFileInfo::exists(path))
            continue;
        SCOPED_TRACE(name);
        ++tested;
        QString err;
        const DecodeRequest req = ImageProvider::requestFor(Quality::Screen, 2560, true);
        auto img = dec.decode(path, req, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
        EXPECT_GT(img->headroom, hdr::kHdrThreshold);
        EXPECT_GT(hdr::measuredPeak(img->image), 1.05f);
        // Without EDR the gain map is not applied: the SDR rendition.
        auto sdr = dec.decode(path, ImageProvider::requestFor(Quality::Screen, 2560, false), &err);
        ASSERT_TRUE(sdr.has_value());
        EXPECT_FALSE(hdr::isHdrContent(sdr->headroom, false));
    }
    EXPECT_GE(tested, 1);
    QString err;
    auto plain = dec.decode(corpusFile(QStringLiteral("big_24mp.jpg")),
                            ImageProvider::requestFor(Quality::Screen, 2560, true), &err);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->image.format(), QImage::Format_RGBA8888_Premultiplied);
    EXPECT_FLOAT_EQ(plain->headroom, 1.0f);
}

// EXR keeps its highlights on SDR systems too, so the exposure control can bring them back.
TEST(HdrDecode, F_COL_05_FloatSourcesKeepRangeWithoutEdr) {
    LF_REQUIRE_CORPUS();
    const QString path = corpusFile(QStringLiteral("hdr.exr"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "hdr.exr not in the corpus";
    ImageIODecoder dec;
    QString err;
    auto img = dec.decode(path, ImageProvider::requestFor(Quality::Screen, 2560, false), &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    EXPECT_EQ(img->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
    const float peak = hdr::measuredPeak(img->image);
    EXPECT_GT(peak, 3.0f); // the red quadrant is 4x SDR white
    // -2 EV brings the highlight into SDR range without tone mapping.
    hdr::Exposure e;
    e.setEv(-2.0);
    EXPECT_LE(peak * e.multiplier(), 1.05f);
    EXPECT_FALSE(hdr::makeToneCurve(peak * e.multiplier() - 0.05f, 1.0f).active);
}

// A small float file is complete at the preview stage, and that entry then also answers the
// Screen and Full requests (ImageProvider): it must keep its range as well, or the HDR badge
// (F-COL-04) and the exposure control (F-COL-05) never see it - seen on 1x displays, where
// the Screen request comes after the preview has been decoded.
TEST(HdrDecode, F_COL_05_SmallFloatFileKeepsRangeWhenPreviewIsComplete) {
    LF_REQUIRE_CORPUS();
    const QString path = corpusFile(QStringLiteral("hdr.exr"));
    if (!QFileInfo::exists(path))
        GTEST_SKIP() << "hdr.exr not in the corpus";
    test::ensureQtApp();
    DecoderRegistry registry;
    registry.add(std::make_shared<ImageIODecoder>());
    ImageProvider provider(registry);
    DecodedImagePtr preview;
    QObject::connect(&provider, &ImageProvider::imageReady,
                     [&](const QString&, Quality q, const DecodedImagePtr& i) {
                         if (q == Quality::Preview)
                             preview = i;
                     });
    int client = 0;
    provider.request(path, Quality::Preview, QSize(1000, 800), &client);
    ASSERT_TRUE(test::waitUntil([&] { return preview != nullptr; }, 10000));
    EXPECT_EQ(preview->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
    const DecodedImagePtr screen = provider.request(path, Quality::Screen, QSize(1000, 800), &client);
    ASSERT_TRUE(screen);
    EXPECT_EQ(screen->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
    EXPECT_GT(hdr::measuredPeak(screen->image), 3.0f);
}

#endif
