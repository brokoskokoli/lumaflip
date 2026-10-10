// SPDX-License-Identifier: GPL-3.0-or-later
// Library decoder backends for platforms without ImageIO (core/LibraryDecoders.h, src/codecs):
// libheif (HEIF/AVIF), libjxl, LibRaw, OpenEXR. The test files are encoded here with the
// same libraries (no generated corpus needed on Linux); the generated corpus and the RAW
// samples (tests/corpus/download_raw.sh) are used too when present. A backend that is not
// built in skips (LF_REQUIRE_LIBRARY_DECODERS=1 makes that a failure, set by the Linux CI).

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/DecoderHelper.h"
#include "core/LibraryDecoders.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <cmath>

#if LF_TEST_HAVE_OPENEXR
#include <ImfArray.h>
#include <ImfRgbaFile.h>
#endif
#if LF_TEST_HAVE_LIBJXL
#include <jxl/encode.h>
#endif
#if LF_TEST_HAVE_LIBHEIF
#include <libheif/heif.h>
#endif

using namespace lf;
using namespace lf::test;

namespace {

std::shared_ptr<Decoder> requireBackend(std::shared_ptr<Decoder> d, const char* name) {
    if (!d && qEnvironmentVariableIntValue("LF_REQUIRE_LIBRARY_DECODERS") == 1)
        ADD_FAILURE() << name << " is not built in (LF_REQUIRE_LIBRARY_DECODERS=1)";
    return d;
}

#define LF_REQUIRE_BACKEND(var, factory, name)                                                               \
    auto var = requireBackend(factory(), name);                                                              \
    if (!var)                                                                                                \
        GTEST_SKIP() << name << " not built in";

// 64×48 test pattern: gradients in R/G, a constant B, alpha ramp optional.
QImage pattern(bool alpha) {
    QImage img(64, 48, QImage::Format_RGBA8888);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixelColor(x, y, QColor(x * 4, y * 5, 160, alpha ? 255 - x * 2 : 255));
    return img;
}

int maxDiff(const QImage& a, const QImage& b) {
    if (a.size() != b.size())
        return 1000;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888),
                 y = b.convertToFormat(QImage::Format_RGBA8888);
    int worst = 0;
    for (int r = 0; r < x.height(); ++r)
        for (int i = 0; i < x.width() * 4; ++i)
            worst = std::max(worst, std::abs(int(x.constScanLine(r)[i]) - int(y.constScanLine(r)[i])));
    return worst;
}

QByteArray header(const QString& path) {
    return fileHeader(path, 512);
}

// Decodes `path` with `d` directly and - if the helper binary is there - in the sandboxed
// decoder helper process; both must agree.
std::optional<DecodedImage> decodeBoth(const std::shared_ptr<Decoder>& d, const QString& path,
                                       const DecodeRequest& req = {}) {
    QString error;
    EXPECT_TRUE(d->canDecode(path, header(path))) << path.toStdString();
    auto local = d->decode(path, req, &error);
    EXPECT_TRUE(local.has_value()) << path.toStdString() << ": " << error.toStdString();
#ifdef LF_DECODER_HELPER_BINARY
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.extraEnvironment = {QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    RemoteDecoder remote(d, std::make_shared<DecoderHelperPool>(o));
    QString remoteError;
    const auto r = remote.decode(path, req, &remoteError);
    EXPECT_EQ(r.has_value(), local.has_value()) << "helper: " << remoteError.toStdString();
    if (r && local) {
        EXPECT_EQ(r->image.size(), local->image.size());
        EXPECT_EQ(r->image.format(), local->image.format());
        EXPECT_LE(maxDiff(r->image, local->image), 1);
    }
#endif
    return local;
}

} // namespace

TEST(LibraryDecoders, F_FMT_01_RegisteredSuffixesMatchBackends) {
    const QStringList suffixes = libraryDecoderSuffixes();
    EXPECT_EQ(suffixes.contains("heic"), createLibHeifDecoder() != nullptr);
    EXPECT_EQ(suffixes.contains("avif"), createLibHeifDecoder() != nullptr);
    EXPECT_EQ(suffixes.contains("jxl"), createLibJxlDecoder() != nullptr);
    EXPECT_EQ(suffixes.contains("cr3"), createLibRawDecoder() != nullptr);
    EXPECT_EQ(suffixes.contains("exr"), createOpenExrDecoder() != nullptr);
    EXPECT_EQ(libraryDecoders().size(), libraryDecoderVersions().size());
    for (const QString& v : libraryDecoderVersions())
        std::printf("library decoder: %s\n", qPrintable(v));
}

#if LF_TEST_HAVE_OPENEXR
TEST(LibraryDecoders, F_FMT_01_OpenExrHalfFloatExtendedRange) {
    LF_REQUIRE_BACKEND(exr, createOpenExrDecoder, "OpenEXR");
    ensureQtApp();
    QTemporaryDir dir;
    const QString path = dir.filePath("hdr.exr");
    const int w = 32, h = 16;
    {
        Imf::Array2D<Imf::Rgba> px(h, w);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                px[y][x] = Imf::Rgba(half(x / 8.0f), half(y / 16.0f), half(0.25f), half(1.0f)); // up to 3.875
        Imf::RgbaOutputFile out(QFile::encodeName(path).constData(), w, h, Imf::WRITE_RGBA);
        out.setFrameBuffer(&px[0][0], 1, size_t(w));
        out.writePixels(h);
    }
    QString error;
    const auto info = exr->info(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->pixelSize, QSize(w, h));
    EXPECT_TRUE(info->isHdr);
    EXPECT_EQ(info->formatName, QStringLiteral("OpenEXR"));
    // HDR request: extended range float with the peak as headroom.
    DecodeRequest req;
    req.hdrFloat = true;
    const auto hdr = decodeBoth(exr, path, req);
    ASSERT_TRUE(hdr.has_value());
    EXPECT_EQ(hdr->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
    EXPECT_NEAR(hdr->headroom, 31 / 8.0f, 0.01f);
    // SDR: clipped 16-bit, sRGB-encoded mid grey stays below white.
    const auto sdr = decodeBoth(exr, path);
    ASSERT_TRUE(sdr.has_value());
    EXPECT_EQ(sdr->image.format(), QImage::Format_RGBA64_Premultiplied);
    EXPECT_FLOAT_EQ(sdr->headroom, 1.0f);
    // Garbage with the magic number fails cleanly (no exception escapes).
    const QString bad = dir.filePath("bad.exr");
    {
        QFile f(bad);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray("\x76\x2f\x31\x01", 4) + QByteArray(100, 'x'));
    }
    EXPECT_FALSE(exr->decode(bad, {}, &error).has_value());
    EXPECT_FALSE(exr->info(bad, &error).has_value());
}
#endif

#if LF_TEST_HAVE_LIBJXL
TEST(LibraryDecoders, F_FMT_01_JpegXlLosslessRoundTrip) {
    LF_REQUIRE_BACKEND(jxl, createLibJxlDecoder, "libjxl");
    QTemporaryDir dir;
    const QString path = dir.filePath("lossless.jxl");
    const QImage src = pattern(true);
    {
        JxlEncoder* enc = JxlEncoderCreate(nullptr);
        ASSERT_NE(enc, nullptr);
        JxlBasicInfo bi;
        JxlEncoderInitBasicInfo(&bi);
        bi.xsize = uint32_t(src.width());
        bi.ysize = uint32_t(src.height());
        bi.bits_per_sample = 8;
        bi.num_color_channels = 3;
        bi.alpha_bits = 8;
        bi.num_extra_channels = 1;
        bi.uses_original_profile = JXL_TRUE;
        ASSERT_EQ(JxlEncoderSetBasicInfo(enc, &bi), JXL_ENC_SUCCESS);
        JxlColorEncoding ce;
        JxlColorEncodingSetToSRGB(&ce, JXL_FALSE);
        ASSERT_EQ(JxlEncoderSetColorEncoding(enc, &ce), JXL_ENC_SUCCESS);
        JxlEncoderFrameSettings* fs = JxlEncoderFrameSettingsCreate(enc, nullptr);
        ASSERT_EQ(JxlEncoderSetFrameLossless(fs, JXL_TRUE), JXL_ENC_SUCCESS);
        const JxlPixelFormat fmt{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        ASSERT_EQ(JxlEncoderAddImageFrame(fs, &fmt, src.constBits(), size_t(src.sizeInBytes())),
                  JXL_ENC_SUCCESS);
        JxlEncoderCloseInput(enc);
        QByteArray out(1 << 16, Qt::Uninitialized);
        uint8_t* next = reinterpret_cast<uint8_t*>(out.data());
        size_t avail = size_t(out.size());
        JxlEncoderStatus st;
        while ((st = JxlEncoderProcessOutput(enc, &next, &avail)) == JXL_ENC_NEED_MORE_OUTPUT) {
            const qsizetype used = qsizetype(next - reinterpret_cast<uint8_t*>(out.data()));
            out.resize(out.size() * 2);
            next = reinterpret_cast<uint8_t*>(out.data()) + used;
            avail = size_t(out.size() - used);
        }
        ASSERT_EQ(st, JXL_ENC_SUCCESS);
        out.truncate(qsizetype(next - reinterpret_cast<uint8_t*>(out.data())));
        JxlEncoderDestroy(enc);
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(out);
    }
    QString error;
    const auto info = jxl->info(path, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->displaySize, src.size());
    EXPECT_TRUE(info->hasAlpha);
    EXPECT_EQ(info->formatName, QStringLiteral("JPEG XL"));
    const auto d = decodeBoth(jxl, path);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->image.format(), QImage::Format_RGBA8888_Premultiplied);
    EXPECT_LE(maxDiff(d->image.convertToFormat(QImage::Format_RGBA8888), src),
              1); // premultiplication rounding
    // Downsampled stage.
    DecodeRequest small;
    small.maxPixelSize = 16;
    const auto s = jxl->decode(path, small, &error);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(std::max(s->image.width(), s->image.height()), 16);
    EXPECT_EQ(s->fullSize, src.size());
    EXPECT_TRUE(s->isPreview);
    // Truncated file.
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadWrite));
    f.resize(f.size() / 2);
    f.close();
    EXPECT_FALSE(jxl->decode(path, {}, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}
#endif

#if LF_TEST_HAVE_LIBHEIF
namespace {
// Encodes `img` with libheif (HEVC or AV1); false if this libheif has no such encoder.
bool encodeHeif(const QImage& src, heif_compression_format format, const QString& path) {
    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    if (heif_context_get_encoder_for_format(ctx, format, &enc).code != heif_error_Ok) {
        heif_context_free(ctx);
        return false;
    }
    heif_encoder_set_lossy_quality(enc, 95);
    heif_image* img = nullptr;
    heif_image_create(src.width(), src.height(), heif_colorspace_RGB, heif_chroma_interleaved_RGBA, &img);
    heif_image_add_plane(img, heif_channel_interleaved, src.width(), src.height(), 8);
    int stride = 0;
    uint8_t* p = heif_image_get_plane(img, heif_channel_interleaved, &stride);
    for (int y = 0; y < src.height(); ++y)
        std::memcpy(p + y * stride, src.constScanLine(y), size_t(src.width()) * 4);
    const bool ok =
        heif_context_encode_image(ctx, img, enc, nullptr, nullptr).code == heif_error_Ok &&
        heif_context_write_to_file(ctx, QFile::encodeName(path).constData()).code == heif_error_Ok;
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(ctx);
    return ok;
}
} // namespace

TEST(LibraryDecoders, F_FMT_01_HeifAndAvifRoundTrip) {
    LF_REQUIRE_BACKEND(heif, createLibHeifDecoder, "libheif");
    QTemporaryDir dir;
    const QImage src = pattern(true);
    int tested = 0;
    for (const auto& [format, name] :
         {std::pair{heif_compression_HEVC, "still.heic"}, std::pair{heif_compression_AV1, "still.avif"}}) {
        SCOPED_TRACE(name);
        const QString path = dir.filePath(QString::fromLatin1(name));
        if (!encodeHeif(src, format, path)) {
            std::printf("no libheif encoder for %s - skipped\n", name);
            continue;
        }
        QString error;
        const auto info = heif->info(path, &error);
        if (!info && error == DecoderRegistry::messageUnsupported()) {
            std::printf("no libheif decoder plugin for %s - skipped\n", name);
            continue;
        }
        ASSERT_TRUE(info.has_value()) << error.toStdString();
        EXPECT_EQ(info->displaySize, src.size());
        EXPECT_TRUE(info->hasAlpha);
        EXPECT_EQ(info->formatName, QString::fromLatin1(QByteArray(name).endsWith("avif") ? "AVIF" : "HEIF"));
        const auto d = decodeBoth(heif, path);
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(d->image.size(), src.size());
        EXPECT_LE(maxDiff(d->image.convertToFormat(QImage::Format_RGBA8888), src), 24); // lossy, 4:2:0
        ++tested;
    }
    if (tested == 0)
        GTEST_SKIP() << "libheif has neither HEVC nor AV1 encoders/decoders here";
}
#endif

// The generated corpus (macOS: tests/corpus/make_corpus.sh), when present.
TEST(LibraryDecoders, F_FMT_01_GeneratedCorpusFiles) {
    LF_REQUIRE_CORPUS();
    struct Case {
        std::shared_ptr<Decoder> decoder;
        const char* file;
    };
    const Case cases[] = {{createLibHeifDecoder(), "still.heic"},  {createLibHeifDecoder(), "alpha.heic"},
                          {createLibHeifDecoder(), "still.avif"},  {createLibHeifDecoder(), "pq.avif"},
                          {createLibJxlDecoder(), "lossless.jxl"}, {createOpenExrDecoder(), "hdr.exr"}};
    int n = 0;
    for (const Case& c : cases) {
        const QString path = corpusFile(QString::fromLatin1(c.file));
        if (!c.decoder || !QFileInfo::exists(path))
            continue;
        SCOPED_TRACE(c.file);
        QString error;
        const auto i = c.decoder->info(path, &error);
        ASSERT_TRUE(i.has_value()) << error.toStdString();
        const auto d = c.decoder->decode(path, {}, &error);
        if (!d && error == DecoderRegistry::messageUnsupported())
            continue; // e.g. no HEVC plugin
        ASSERT_TRUE(d.has_value()) << error.toStdString();
        EXPECT_EQ(d->image.size(), i->displaySize);
        ++n;
    }
    if (n == 0)
        GTEST_SKIP() << "no library decoder built in";
}

// RAW samples of tests/corpus/download_raw.sh (CC0), when downloaded.
TEST(LibraryDecoders, F_FMT_05_LibRawPreviewAndFullDecode) {
    LF_REQUIRE_BACKEND(raw, createLibRawDecoder, "LibRaw");
    const char* files[] = {"raw/nikon_d2h.nef", "raw/sony_ilce7s.arw", "raw/sigma_fp.dng",
                           "raw/canon_eos_r6.cr3", "raw/fujifilm_xs10.raf"};
    int previews = 0, full = 0, present = 0;
    for (const char* file : files) {
        const QString path = corpusFile(QString::fromLatin1(file));
        if (!QFileInfo::exists(path))
            continue;
        ++present;
        SCOPED_TRACE(file);
        QString error;
        ASSERT_TRUE(raw->canDecode(path, header(path)));
        const auto info = raw->info(path, &error);
        if (!info) {
            std::printf("%s: LibRaw cannot open it (%s)\n", file, qPrintable(error));
            continue;
        }
        EXPECT_GT(info->displaySize.width(), 1000);
        DecodeRequest preview;
        preview.preferEmbeddedPreview = true;
        preview.maxPixelSize = 512;
        const auto p = raw->decode(path, preview, &error);
        if (p) {
            EXPECT_TRUE(p->isPreview);
            EXPECT_LE(std::max(p->image.width(), p->image.height()), 512);
            EXPECT_EQ(p->fullSize, info->displaySize);
            // Same orientation as the full image.
            EXPECT_EQ(p->image.width() > p->image.height(),
                      info->displaySize.width() > info->displaySize.height());
            ++previews;
        }
        DecodeRequest screen;
        screen.maxPixelSize = 800; // half-size develop
        const auto f = raw->decode(path, screen, &error);
        if (f) {
            EXPECT_LE(std::max(f->image.width(), f->image.height()), 800);
            EXPECT_EQ(f->image.width() > f->image.height(),
                      info->displaySize.width() > info->displaySize.height());
            ++full;
        } else {
            std::printf("%s: develop failed (%s)\n", file, qPrintable(error));
        }
        const Metadata md = raw->metadata(path);
        EXPECT_FALSE(md.entries.isEmpty());
    }
    if (present == 0) {
        if (qEnvironmentVariableIntValue("LF_REQUIRE_RAW") == 1)
            FAIL() << "RAW samples missing (LF_REQUIRE_RAW=1) - run tests/corpus/download_raw.sh";
        GTEST_SKIP() << "RAW samples missing - run tests/corpus/download_raw.sh";
    }
    EXPECT_GE(previews, present - 1);
    EXPECT_GE(full, present - 2);
    // A non-RAW file with a RAW suffix fails cleanly.
    QTemporaryDir dir;
    const QString fake = dir.filePath("fake.nef");
    QFile f(fake);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(4096, 'z'));
    f.close();
    QString error;
    EXPECT_FALSE(raw->decode(fake, {}, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
}
