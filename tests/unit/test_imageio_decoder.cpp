// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for the ImageIO decoder backend against the generated format corpus.
// Requirements: F-FMT-01, F-FMT-02, F-FMT-03, F-FMT-04, F-FMT-05, F-VIEW-08, F-COL-01,
// F-COL-03, F-COL-06, F-INFO-01, F-INFO-02.

#if defined(__APPLE__)

#include "CorpusTestUtil.h"
#include "platform/mac/ImageIODecoder.h"

#include <QtCore/qfloat16.h>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace lf;
using namespace lf::test;

namespace {

DecodeRequest fullRes() {
    return DecodeRequest{};
}

QColorSpace p3() {
    return QColorSpace(QColorSpace::DisplayP3);
}
QColorSpace srgb() {
    return QColorSpace(QColorSpace::SRgb);
}

} // namespace

// F-FMT-01 / F-INFO-01: every corpus file is recognised, reports correct info and decodes
// to the expected display size and colors.
TEST(ImageIODecoder, F_FMT_01_CorpusInfoAndDecode) {
    LF_REQUIRE_CORPUS();
    const QJsonObject root = expectedCorpus();
    ASSERT_FALSE(root.isEmpty()) << "tests/corpus/expected.json not readable";
    const int defaultTol = root.value(QStringLiteral("tolerance")).toInt(40);
    const QJsonObject images = root.value(QStringLiteral("images")).toObject();
    ImageIODecoder dec;
    int checked = 0;
    for (auto it = images.begin(); it != images.end(); ++it) {
        const QString name = it.key();
        const QJsonObject e = it.value().toObject();
        SCOPED_TRACE(name.toStdString());
        const QString path = corpusFile(name);
        if (!QFileInfo::exists(path)) {
            EXPECT_TRUE(e.value(QStringLiteral("optional")).toBool()) << "required corpus file missing";
            continue;
        }
        ++checked;
        EXPECT_TRUE(dec.canDecode(path, fileHeader(path, 64)));

        QString err;
        const auto info = dec.info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        const QSize display = jsonSize(e.value(QStringLiteral("displaySize")));
        const int orientation = e.value(QStringLiteral("orientation")).toInt(1);
        const QSize pixel = e.contains(QStringLiteral("pixelSize"))
                                ? jsonSize(e.value(QStringLiteral("pixelSize")))
                                : (orientation >= 5 ? display.transposed() : display);
        EXPECT_EQ(info->displaySize, display);
        EXPECT_EQ(info->pixelSize, pixel);
        EXPECT_EQ(info->exifOrientation, orientation);
        EXPECT_EQ(info->frameCount, e.value(QStringLiteral("frames")).toInt(1));
        EXPECT_EQ(info->isAnimated, e.value(QStringLiteral("frames")).toInt(1) > 1);
        EXPECT_EQ(info->pageCount, e.value(QStringLiteral("pages")).toInt(1));
        EXPECT_EQ(info->hasAlpha, e.value(QStringLiteral("hasAlpha")).toBool(false));
        EXPECT_EQ(info->isHdr, e.value(QStringLiteral("isHdr")).toBool(false));
        EXPECT_EQ(info->bitsPerComponent, e.value(QStringLiteral("bits")).toInt(8));
        if (e.contains(QStringLiteral("colorModel")))
            EXPECT_EQ(info->colorModel, e.value(QStringLiteral("colorModel")).toString());
        if (e.contains(QStringLiteral("format")))
            EXPECT_EQ(info->formatName, e.value(QStringLiteral("format")).toString());
        EXPECT_FALSE(info->formatName.isEmpty());

        const auto img = dec.decode(path, fullRes(), &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.size(), display);
        EXPECT_EQ(img->fullSize, display);
        EXPECT_FALSE(img->isPreview);
        EXPECT_TRUE(img->image.colorSpace().isValid());
        const int bits = e.value(QStringLiteral("bits")).toInt(8);
        EXPECT_EQ(img->image.format(),
                  bits > 8 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
        if (e.contains(QStringLiteral("colorSpace"))) {
            const QString cs = e.value(QStringLiteral("colorSpace")).toString();
            EXPECT_EQ(img->image.colorSpace(), cs == u"DisplayP3" ? p3() : srgb());
        }
        if (e.contains(QStringLiteral("quadrants")))
            expectQuadrants(img->image, quadrantColors(e.value(QStringLiteral("quadrants"))),
                            e.value(QStringLiteral("tolerance")).toInt(defaultTol));
    }
    EXPECT_GE(checked, 25);
}

// F-VIEW-08: EXIF orientation 1..8 is applied, in the full-resolution and the
// downsampled (thumbnail) path.
TEST(ImageIODecoder, F_VIEW_08_ExifOrientationAllEight) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    const auto rgbw = quadrantColors(QJsonValue(QStringLiteral("rgbw")));
    for (int o = 1; o <= 8; ++o) {
        SCOPED_TRACE("orientation " + std::to_string(o));
        const QString path = corpusFile(QStringLiteral("orient_%1.jpg").arg(o));
        QString err;
        auto full = dec.decode(path, fullRes(), &err);
        ASSERT_TRUE(full.has_value()) << err.toStdString();
        EXPECT_EQ(full->image.size(), QSize(64, 48));
        expectQuadrants(full->image, rgbw, 40);

        DecodeRequest small;
        small.maxPixelSize = 32;
        auto thumb = dec.decode(path, small, &err);
        ASSERT_TRUE(thumb.has_value()) << err.toStdString();
        EXPECT_EQ(thumb->image.size(), QSize(32, 24));
        EXPECT_EQ(thumb->fullSize, QSize(64, 48));
        EXPECT_TRUE(thumb->isPreview);
        expectQuadrants(thumb->image, rgbw, 48);
    }
}

// F-COL-01: embedded profiles are kept (no flattening to sRGB); untagged = sRGB.
TEST(ImageIODecoder, F_COL_01_DisplayP3IsPreservedNotFlattened) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    QString err;
    auto img = dec.decode(corpusFile(QStringLiteral("p3.png")), fullRes(), &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    const QColorSpace cs = img->image.colorSpace();
    EXPECT_EQ(cs.primaries(), QColorSpace::Primaries::DciP3D65);
    EXPECT_EQ(cs, p3());
    // Pixel values are untouched P3 values (an sRGB-flattened image would differ).
    const QColor c = img->image.pixelColor(16, 12);
    EXPECT_NEAR(c.red(), 204, 2);
    EXPECT_NEAR(c.green(), 77, 2);
    EXPECT_NEAR(c.blue(), 51, 2);
    // Converted to sRGB the same color is clearly different - proves the check is meaningful.
    const QColor inSrgb = img->image.convertedToColorSpace(srgb()).pixelColor(16, 12);
    EXPECT_GT(std::abs(inSrgb.red() - 204) + std::abs(inSrgb.green() - 77) + std::abs(inSrgb.blue() - 51),
              10);

    for (const char* name : {"untagged.png", "srgb.png", "orient_1.jpg"}) {
        SCOPED_TRACE(name);
        auto u = dec.decode(corpusFile(QString::fromLatin1(name)), fullRes(), &err);
        ASSERT_TRUE(u.has_value()) << err.toStdString();
        EXPECT_EQ(u->image.colorSpace(), srgb());
    }
}

// F-COL-06: CMYK JPEG/TIFF are converted by ColorSync into Display P3.
TEST(ImageIODecoder, F_COL_06_CmykConverted) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    for (const char* name : {"cmyk.jpg", "cmyk.tiff"}) {
        SCOPED_TRACE(name);
        QString err;
        auto info = dec.info(corpusFile(QString::fromLatin1(name)), &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_EQ(info->colorModel, QStringLiteral("CMYK"));
        auto img = dec.decode(corpusFile(QString::fromLatin1(name)), fullRes(), &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.colorSpace(), p3());
        // Approximate hue checks: cyan, magenta, yellow, black.
        const QColor c = img->image.pixelColor(16, 12), m = img->image.pixelColor(48, 12);
        const QColor y = img->image.pixelColor(16, 36), k = img->image.pixelColor(48, 36);
        EXPECT_LT(c.red(), 100);
        EXPECT_GT(c.blue(), 150);
        EXPECT_GT(c.green(), 100);
        EXPECT_GT(m.red(), 150);
        EXPECT_LT(m.green(), 100);
        EXPECT_GT(m.blue(), 80);
        EXPECT_GT(y.red(), 200);
        EXPECT_GT(y.green(), 200);
        EXPECT_LT(y.blue(), 100);
        EXPECT_LT(k.red() + k.green() + k.blue(), 200);
    }
}

// P-01 decode side: downsampling at decode time respects maxPixelSize and aspect ratio.
TEST(ImageIODecoder, P_01_DownsampledDecodeRespectsMaxPixelSize) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    DecodeRequest req;
    req.maxPixelSize = 2560;
    QString err;
    auto img = dec.decode(corpusFile(QStringLiteral("big_24mp.jpg")), req, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    EXPECT_EQ(img->image.width(), 2560);
    EXPECT_NEAR(img->image.height(), 1707, 1);
    EXPECT_EQ(img->fullSize, QSize(6000, 4000));
    EXPECT_TRUE(img->isPreview);

    // Requests larger than the image never upscale.
    req.maxPixelSize = 4096;
    auto small = dec.decode(corpusFile(QStringLiteral("orient_6.jpg")), req, &err);
    ASSERT_TRUE(small.has_value()) << err.toStdString();
    EXPECT_EQ(small->image.size(), QSize(64, 48));
    EXPECT_FALSE(small->isPreview);
}

// F-FMT-05 / P-04: the embedded preview path returns a (small) preview quickly.
TEST(ImageIODecoder, F_FMT_05_EmbeddedPreviewIsPreview) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    DecodeRequest req;
    req.maxPixelSize = 512;
    req.preferEmbeddedPreview = true;
    QString err;
    auto img = dec.decode(corpusFile(QStringLiteral("exif.jpg")), req, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    EXPECT_TRUE(img->isPreview);
    EXPECT_LE(std::max(img->image.width(), img->image.height()), 512);
    EXPECT_EQ(img->fullSize, QSize(640, 480));
    EXPECT_NEAR(double(img->image.width()) / img->image.height(), 640.0 / 480.0, 0.05);
    expectQuadrants(img->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 48);
}

// F-FMT-03 (decode side): animated GIF/APNG/HEICS/WebP frames and durations.
TEST(ImageIODecoder, F_FMT_03_AnimationFramesAndDurations) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    int tested = 0;
    for (const char* name : {"anim.gif", "anim.png", "anim.heics", "anim.webp"}) {
        const QString path = corpusFile(QString::fromLatin1(name));
        if (!QFileInfo::exists(path))
            continue;
        SCOPED_TRACE(name);
        ++tested;
        QString err;
        auto info = dec.info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_TRUE(info->isAnimated);
        EXPECT_EQ(info->frameCount, 3);
        EXPECT_EQ(info->pageCount, 1);
        const QJsonArray durations = expectedCorpus()
                                         .value(QStringLiteral("images"))
                                         .toObject()
                                         .value(QString::fromLatin1(name))
                                         .toObject()
                                         .value(QStringLiteral("durations"))
                                         .toArray();
        ASSERT_EQ(durations.size(), 3);
        const char colors[] = {'r', 'g', 'b'};
        for (int f = 0; f < 3; ++f) {
            DecodeRequest req;
            req.frame = f;
            auto img = dec.decode(path, req, &err);
            ASSERT_TRUE(img.has_value()) << err.toStdString();
            EXPECT_EQ(img->frameDurationMs, durations[f].toInt()) << "frame " << f;
            const Rgba c = namedColor(QChar::fromLatin1(colors[f]));
            expectQuadrants(img->image, {c, c, c, c}, 40);
        }
        DecodeRequest bad;
        bad.frame = 3;
        EXPECT_FALSE(dec.decode(path, bad, &err).has_value());
        EXPECT_FALSE(err.isEmpty());
    }
    EXPECT_GE(tested, 1);
}

// F-FMT-04 (decode side): multi-page TIFF and ICO expose pages.
TEST(ImageIODecoder, F_FMT_04_MultipageTiffPages) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    const QString path = corpusFile(QStringLiteral("multipage.tiff"));
    QString err;
    auto info = dec.info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->pageCount, 3);
    EXPECT_FALSE(info->isAnimated);
    const QSize sizes[] = {{64, 48}, {32, 24}, {16, 12}};
    for (int p = 0; p < 3; ++p) {
        DecodeRequest req;
        req.page = p;
        auto img = dec.decode(path, req, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.size(), sizes[p]);
        EXPECT_EQ(img->frameDurationMs, 0);
    }
    DecodeRequest bad;
    bad.page = 3;
    EXPECT_FALSE(dec.decode(path, bad, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
}

// F-FMT-02: broken files fail with a readable error instead of crashing or showing garbage.
TEST(ImageIODecoder, F_FMT_02_BrokenFilesFailCleanly) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    const QJsonArray broken = expectedCorpus().value(QStringLiteral("broken")).toArray();
    ASSERT_EQ(broken.size(), 3);
    for (const QJsonValue& v : broken) {
        const QString path = corpusFile(v.toString());
        SCOPED_TRACE(v.toString().toStdString());
        ASSERT_TRUE(QFileInfo::exists(path));
        QString err;
        EXPECT_FALSE(dec.decode(path, fullRes(), &err).has_value());
        EXPECT_FALSE(err.isEmpty());
        DecodeRequest small;
        small.maxPixelSize = 256;
        err.clear();
        EXPECT_FALSE(dec.decode(path, small, &err).has_value());
        EXPECT_FALSE(err.isEmpty());
        (void)dec.metadata(path); // must not crash
    }
    QString err;
    EXPECT_FALSE(dec.canDecode(corpusFile(QStringLiteral("garbage.jpg")),
                               fileHeader(corpusFile(QStringLiteral("garbage.jpg")))));
    EXPECT_FALSE(dec.canDecode(corpusFile(QStringLiteral("empty.jpg")), QByteArray()));
    EXPECT_FALSE(dec.info(corpusFile(QStringLiteral("empty.jpg")), &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(dec.info(corpusFile(QStringLiteral("garbage.jpg")), &err).has_value());
    EXPECT_FALSE(dec.decode(corpusFile(QStringLiteral("does-not-exist.jpg")), fullRes(), &err).has_value());
}

// F-FMT-01: detection by signature - a PNG named .jpg decodes as PNG.
TEST(ImageIODecoder, F_FMT_01_SignatureWinsOverSuffix) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    const QString path = corpusFile(QStringLiteral("png_as_jpg.jpg"));
    EXPECT_TRUE(dec.canDecode(path, fileHeader(path, 64)));
    QString err;
    auto info = dec.info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->formatName, QStringLiteral("PNG"));
    auto img = dec.decode(path, fullRes(), &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    EXPECT_EQ(img->image.size(), QSize(64, 48));
    // A JPEG header without any suffix is still recognised.
    EXPECT_TRUE(dec.canDecode(QStringLiteral("/tmp/noext"),
                              fileHeader(corpusFile(QStringLiteral("orient_1.jpg")), 64)));
    // SVG is not ImageIO's business.
    EXPECT_FALSE(dec.canDecode(corpusFile(QStringLiteral("vector.svg")),
                               fileHeader(corpusFile(QStringLiteral("vector.svg")))));
}

TEST(ImageIODecoder, F_INFO_01_FormatNamesAndExtensions) {
    EXPECT_EQ(ImageIODecoder::formatNameForType(QStringLiteral("com.canon.cr3-raw-image")),
              QStringLiteral("Canon CR3"));
    EXPECT_EQ(ImageIODecoder::formatNameForType(QStringLiteral("public.jpeg")), QStringLiteral("JPEG"));
    EXPECT_EQ(ImageIODecoder::formatNameForType(QStringLiteral("public.heic")), QStringLiteral("HEIC"));
    const QStringList exts = ImageIODecoder::supportedExtensions();
    for (const char* e : {"jpg", "jpeg", "png", "gif", "heic", "tif", "webp", "cr3", "nef", "arw", "dng"})
        EXPECT_TRUE(exts.contains(QString::fromLatin1(e))) << e;
    EXPECT_FALSE(exts.contains(QStringLiteral("svg")));
}

// F-INFO-02: grouped metadata (camera, exposure, GPS, date, rating) from EXIF/XMP.
TEST(ImageIODecoder, F_INFO_02_MetadataFromExif) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    const Metadata md = dec.metadata(corpusFile(QStringLiteral("exif.jpg")));
    const QVariantMap file = md.groups.value(QStringLiteral("Datei")).toMap();
    const QVariantMap cam = md.groups.value(QStringLiteral("Kamera")).toMap();
    const QVariantMap shot = md.groups.value(QStringLiteral("Aufnahme")).toMap();
    const QVariantMap gps = md.groups.value(QStringLiteral("GPS")).toMap();
    EXPECT_EQ(file.value(QStringLiteral("Format")).toString(), QStringLiteral("JPEG"));
    EXPECT_EQ(file.value(QStringLiteral("Pixelmaße")).toString(), QStringLiteral("640 × 480"));
    EXPECT_EQ(file.value(QStringLiteral("HDR")).toString(), QStringLiteral("Nein"));
    EXPECT_EQ(cam.value(QStringLiteral("Hersteller")).toString(), QStringLiteral("Lumaflip"));
    EXPECT_EQ(cam.value(QStringLiteral("Modell")).toString(), QStringLiteral("Synthetic Camera X1"));
    EXPECT_EQ(cam.value(QStringLiteral("Objektiv")).toString(), QStringLiteral("Synthetic 35mm F1.4"));
    EXPECT_EQ(shot.value(QStringLiteral("Blende")).toString(), QStringLiteral("f/2,8"));
    EXPECT_EQ(shot.value(QStringLiteral("Belichtungszeit")).toString(), QStringLiteral("1/250 s"));
    EXPECT_EQ(shot.value(QStringLiteral("ISO")).toString(), QStringLiteral("400"));
    EXPECT_EQ(shot.value(QStringLiteral("Brennweite")).toString(), QStringLiteral("35 mm"));
    EXPECT_EQ(shot.value(QStringLiteral("Belichtungskorrektur")).toString(), QStringLiteral("-0,7 EV"));
    EXPECT_EQ(shot.value(QStringLiteral("Blitz")).toString(), QStringLiteral("Nicht ausgelöst"));
    EXPECT_TRUE(shot.value(QStringLiteral("Aufnahmedatum"))
                    .toString()
                    .startsWith(QStringLiteral("17.05.2024 14:30:05")));
    ASSERT_TRUE(md.captured.has_value());
    EXPECT_EQ(md.captured->toUTC(), QDateTime(QDate(2024, 5, 17), QTime(12, 30, 5), QTimeZone::UTC));
    ASSERT_TRUE(md.latitude && md.longitude);
    EXPECT_NEAR(*md.latitude, 48.1371, 1e-4);
    EXPECT_NEAR(*md.longitude, -122.4194, 1e-4);
    EXPECT_FALSE(gps.value(QStringLiteral("Höhe")).toString().isEmpty());
    EXPECT_EQ(md.rating, 4);

    // Files without EXIF yield no camera/GPS groups and no rating.
    const Metadata plain = dec.metadata(corpusFile(QStringLiteral("srgb.png")));
    EXPECT_FALSE(plain.groups.contains(QStringLiteral("Kamera")));
    EXPECT_FALSE(plain.latitude.has_value());
    EXPECT_EQ(plain.rating, -1);
}

// F-COL-03 (decode side): HDR sources decode to extended linear P3 half floats with
// headroom > 1; without req.hdr the SDR path is used.
TEST(ImageIODecoder, F_COL_03_HdrDecodeAndSdrFallback) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    int tested = 0;
    for (const char* name : {"hdr.exr", "gainmap.heic", "pq.avif"}) {
        const QString path = corpusFile(QString::fromLatin1(name));
        if (!QFileInfo::exists(path))
            continue;
        SCOPED_TRACE(name);
        ++tested;
        QString err;
        DecodeRequest req;
        req.hdr = true;
        auto hdr = dec.decode(path, req, &err);
        ASSERT_TRUE(hdr.has_value()) << err.toStdString();
        EXPECT_EQ(hdr->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
        EXPECT_EQ(hdr->image.colorSpace().transferFunction(), QColorSpace::TransferFunction::Linear);
        EXPECT_EQ(hdr->image.colorSpace().primaries(), QColorSpace::Primaries::DciP3D65);
        EXPECT_GT(hdr->headroom, 1.0f);
        // Some pixel exceeds SDR white.
        float peak = 0;
        for (int y = 0; y < hdr->image.height(); ++y) {
            const auto* p = reinterpret_cast<const qfloat16*>(hdr->image.constScanLine(y));
            for (int x = 0; x < hdr->image.width() * 4; ++x)
                if (x % 4 != 3)
                    peak = std::max(peak, float(p[x]));
        }
        EXPECT_GT(peak, 1.2f);

        auto sdr = dec.decode(path, fullRes(), &err);
        ASSERT_TRUE(sdr.has_value()) << err.toStdString();
        EXPECT_NE(sdr->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
        EXPECT_FLOAT_EQ(sdr->headroom, 1.0f);
        EXPECT_EQ(sdr->image.size(), QSize(64, 48));
    }
    EXPECT_GE(tested, 1);
    // SDR images stay SDR even if HDR was requested.
    DecodeRequest req;
    req.hdr = true;
    QString err;
    auto img = dec.decode(corpusFile(QStringLiteral("orient_1.jpg")), req, &err);
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->image.format(), QImage::Format_RGBA8888_Premultiplied);
    EXPECT_FLOAT_EQ(img->headroom, 1.0f);
}

// F-FMT-05: RAW files (optional CC0 samples from tests/corpus/download_raw.sh): embedded
// preview first, full development via ImageIO's RAW engine. Skipped if not downloaded.
TEST(ImageIODecoder, F_FMT_05_RawSamplesIfPresent) {
    const struct {
        const char* file;
        const char* format;
    } samples[] = {
        {"raw/canon_eos_r6.cr3", "Canon CR3"},     {"raw/nikon_d2h.nef", "Nikon NEF"},
        {"raw/sony_ilce7s.arw", "Sony ARW"},       {"raw/sigma_fp.dng", "Adobe DNG"},
        {"raw/fujifilm_xs10.raf", "Fujifilm RAF"},
    };
    ImageIODecoder dec;
    int tested = 0;
    for (const auto& s : samples) {
        const QString path = corpusFile(QString::fromLatin1(s.file));
        if (!QFileInfo::exists(path))
            continue;
        SCOPED_TRACE(s.file);
        ++tested;
        EXPECT_TRUE(dec.canDecode(path, fileHeader(path, 64)));
        QString err;
        auto info = dec.info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_EQ(info->formatName, QString::fromLatin1(s.format));
        EXPECT_GT(info->displaySize.width(), 0);

        DecodeRequest preview;
        preview.maxPixelSize = 1024;
        preview.preferEmbeddedPreview = true;
        auto p = dec.decode(path, preview, &err);
        ASSERT_TRUE(p.has_value()) << err.toStdString();
        EXPECT_TRUE(p->isPreview);
        EXPECT_LE(std::max(p->image.width(), p->image.height()), 1024);

        DecodeRequest screen;
        screen.maxPixelSize = 2048;
        auto full = dec.decode(path, screen, &err);
        ASSERT_TRUE(full.has_value()) << err.toStdString();
        // Orientation-aware aspect ratio matches the reported display size.
        const double want = double(info->displaySize.width()) / info->displaySize.height();
        EXPECT_NEAR(double(full->image.width()) / full->image.height(), want, 0.05);
    }
    if (tested == 0)
        GTEST_SKIP() << "no RAW samples - run tests/corpus/download_raw.sh (optional)";
}

// Decoders are called from worker threads concurrently (SPEC 8.3).
TEST(ImageIODecoder, ConcurrentDecodeIsThreadSafe) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    const QStringList files = {QStringLiteral("orient_6.jpg"), QStringLiteral("p3.png"),
                               QStringLiteral("anim.gif"),     QStringLiteral("still.heic"),
                               QStringLiteral("cmyk.jpg"),     QStringLiteral("garbage.jpg")};
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 20; ++i) {
                const QString name = files[(t + i) % files.size()];
                QString err;
                const bool ok = dec.decode(corpusFile(name), DecodeRequest{}, &err).has_value();
                if (ok == (name == u"garbage.jpg"))
                    ++failures;
                (void)dec.metadata(corpusFile(name));
            }
        });
    }
    for (auto& th : threads)
        th.join();
    EXPECT_EQ(failures.load(), 0);
}

#endif // __APPLE__
