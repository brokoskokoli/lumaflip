// SPDX-License-Identifier: GPL-3.0-or-later
// ImageIO decoder regressions from the code review: full-decode memory and size cap,
// Generic RGB color handling, truncation check with EXIF thumbnails (F-FMT-02, F-COL-01).

#include "platform/mac/ImageIODecoder.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <atomic>
#include <thread>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach.h>

using namespace lf;

namespace {

// Builds a w x h RGB image in `space` filled with a gray value, or with noise.
CGImageRef makeImage(int w, int h, CGColorSpaceRef space, int gray, bool noise = false) {
    CGContextRef ctx = CGBitmapContextCreate(nullptr, size_t(w), size_t(h), 8, 0, space, kCGImageAlphaNoneSkipLast);
    if (!ctx)
        return nullptr;
    // Raw component values in `space` (fill colors would be converted from device RGB).
    auto* p = static_cast<uchar*>(CGBitmapContextGetData(ctx));
    const size_t bpl = CGBitmapContextGetBytesPerRow(ctx);
    QRandomGenerator rng(42);
    for (int y = 0; y < h; ++y)
        for (size_t x = 0; x < bpl; ++x)
            p[size_t(y) * bpl + x] = (x % 4 == 3) ? 0xFF : uchar(noise ? rng.bounded(256) : quint32(gray));
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return img;
}

bool writeImage(const QString& path, CGImageRef img, UTType* type, NSDictionary* props = nil) {
    NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
    CGImageDestinationRef dst =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)type.identifier, 1, nullptr);
    if (!dst)
        return false;
    CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)props);
    const bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    return ok;
}

[[maybe_unused]] qint64 physFootprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return qint64(info.phys_footprint);
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LF_ASAN 1
#endif
#endif

} // namespace

// Review finding: a cut-off JPEG with an EXIF thumbnail passed the truncation check because
// the thumbnail's own EOI marker was found, and the image showed gray areas.
TEST(ImageIODecoderReview, F_FMT_02_TruncatedSmallJpegWithExifThumbnail) {
    QTemporaryDir dir;
    const QString path = dir.filePath("thumb.jpg");
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef img = makeImage(640, 480, cs, 0, true);
        NSDictionary* props = @{
            (id)kCGImageDestinationEmbedThumbnail : @YES,
            (id)kCGImageDestinationLossyCompressionQuality : @0.9,
            (id)kCGImagePropertyExifDictionary : @{(id)kCGImagePropertyExifDateTimeOriginal : @"2024:01:01 12:00:00"},
        };
        ASSERT_TRUE(writeImage(path, img, UTTypeJPEG, props));
        CGImageRelease(img);
        CGColorSpaceRelease(cs);
    }
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray full = f.readAll();
    f.close();
    ASSERT_LT(full.size(), 512 * 1024); // small file: the whole file is the search window
    // The EXIF thumbnail (a complete JPEG with its own EOI) sits before the main scan.
    const qsizetype thumbEoi = full.indexOf(QByteArray("\xFF\xD9", 2));
    ASSERT_GT(thumbEoi, 0);
    ASSERT_LT(thumbEoi, full.size() - 2) << "no EXIF thumbnail embedded";

    ImageIODecoder dec;
    QString err;
    ASSERT_TRUE(dec.decode(path, DecodeRequest{}, &err)) << err.toStdString();

    const QString cut = dir.filePath("cut.jpg");
    QFile o(cut);
    ASSERT_TRUE(o.open(QIODevice::WriteOnly));
    o.write(full.left(full.size() * 7 / 10));
    o.close();
    err.clear();
    EXPECT_FALSE(dec.decode(cut, DecodeRequest{}, &err));
    EXPECT_FALSE(err.isEmpty());
}

// Review finding: images tagged with Generic RGB (gamma 1.8, own primaries) were relabelled
// as sRGB without conversion, so their colors shifted.
TEST(ImageIODecoderReview, F_COL_01_GenericRgbIsNotRelabelledAsSrgb) {
    QTemporaryDir dir;
    const QString path = dir.filePath("generic.png");
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
        CGImageRef img = makeImage(16, 16, cs, 128);
        ASSERT_NE(img, nullptr);
        ASSERT_TRUE(writeImage(path, img, UTTypePNG));
        CGImageRelease(img);
        CGColorSpaceRelease(cs);
    }
    ImageIODecoder dec;
    QString err;
    const auto d = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(d) << err.toStdString();
    ASSERT_TRUE(d->image.colorSpace().isValid());
    EXPECT_NE(d->image.colorSpace(), QColorSpace(QColorSpace::SRgb));
    // Generic RGB 128 (gamma 1.8) is about 147 in sRGB; relabelling would leave 128.
    const QImage s = d->image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb))
                         .convertToFormat(QImage::Format_RGBA8888);
    const QRgb px = s.pixel(8, 8);
    EXPECT_NEAR(qGreen(px), 147, 6);
}

// Untagged images are still interpreted as sRGB (no conversion).
TEST(ImageIODecoderReview, F_COL_01_UntaggedStaysSrgb) {
    QTemporaryDir dir;
    const QString path = dir.filePath("device.png");
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = makeImage(16, 16, cs, 128);
        ASSERT_TRUE(writeImage(path, img, UTTypePNG));
        CGImageRelease(img);
        CGColorSpaceRelease(cs);
    }
    ImageIODecoder dec;
    QString err;
    const auto d = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(d) << err.toStdString();
    EXPECT_EQ(d->image.colorSpace(), QColorSpace(QColorSpace::SRgb));
    EXPECT_NEAR(qGreen(d->image.convertToFormat(QImage::Format_RGBA8888).pixel(8, 8)), 128, 2);
}

// Review finding: full decodes beyond any sane size must be capped (downsampled) instead of
// allocating gigabytes; fullSize stays the real size.
TEST(ImageIODecoderReview, ReviewFullDecodeIsCapped) {
    QTemporaryDir dir;
    const QString path = dir.filePath("wide.png");
    const int w = ImageIODecoder::kMaxFullEdge + 7232; // 40000
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef img = makeImage(w, 8, cs, 200);
        ASSERT_TRUE(writeImage(path, img, UTTypePNG));
        CGImageRelease(img);
        CGColorSpaceRelease(cs);
    }
    ImageIODecoder dec;
    QString err;
    const auto d = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(d) << err.toStdString();
    EXPECT_EQ(d->fullSize, QSize(w, 8));
    EXPECT_LE(std::max(d->image.width(), d->image.height()), ImageIODecoder::kMaxFullEdge);
    EXPECT_GE(d->image.width(), ImageIODecoder::kMaxFullEdge - 1);
    EXPECT_FALSE(d->isPreview);
}

// Review finding: full decodes kept ImageIO's cached bitmap (kCGImageSourceShouldCacheImmediately)
// while drawing into a second full-size QImage. Measured: ImageIO materializes one decode
// buffer during the draw in any case (peak ~2x, also with CGDataProviderCopyData/vImage), but
// nothing beyond the output may stay alive afterwards, and the peak must not grow further.
TEST(ImageIODecoderReview, ReviewFullDecodeMemoryBound) {
#ifdef LF_ASAN
    GTEST_SKIP() << "footprint measurements are meaningless under ASan";
#else
    QTemporaryDir dir;
    const QString path = dir.filePath("big.jpg");
    const int w = 8000, h = 6000; // 48 MP -> 183 MB RGBA8
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef img = makeImage(w, h, cs, 90);
        ASSERT_TRUE(writeImage(path, img, UTTypeJPEG));
        CGImageRelease(img);
        CGColorSpaceRelease(cs);
    }
    const qint64 outBytes = qint64(w) * h * 4;
    ImageIODecoder dec;
    std::atomic<bool> running{true};
    std::atomic<qint64> peak{0};
    const qint64 base = physFootprint();
    std::thread sampler([&] {
        while (running.load()) {
            peak.store(std::max(peak.load(), physFootprint()));
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    QString err;
    qint64 held = 0;
    {
        const auto d = dec.decode(path, DecodeRequest{}, &err);
        held = physFootprint() - base;
        running = false;
        sampler.join();
        ASSERT_TRUE(d) << err.toStdString();
        EXPECT_EQ(d->image.size(), QSize(w, h));
    }
    const qint64 delta = std::max(peak.load(), base + held) - base;
    EXPECT_LT(held, outBytes * 5 / 4) << "held " << held / (1 << 20) << " MB";
    EXPECT_LT(delta, outBytes * 5 / 2) << "peak " << delta / (1 << 20) << " MB for a " << outBytes / (1 << 20)
                                       << " MB image";
#endif
}
