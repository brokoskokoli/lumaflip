// SPDX-License-Identifier: GPL-3.0-or-later
// Export / convert (F-EDIT-04) on macOS, checked through ImageIO: quality, metadata modes
// (keep / without GPS / none), orientation baked in, color space conversion, HDR gain map
// kept or tone-mapped, never touching the source.

#include "ExifTestWriter.h"
#include "core/ImageExport.h"
#include "core/LosslessTransform.h"

#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

using namespace lf;

namespace {

NSURL* urlOf(const QString& path) {
    return [NSURL fileURLWithPath:path.toNSString()];
}

// A JPEG with EXIF, GPS and orientation 6 (stored 64 x 48: shown 48 x 64), written by ImageIO.
bool writeSourceJpeg(const QString& path) {
    @autoreleasepool {
        const QImage img = test::noisyPattern(QSize(64, 48)).convertToFormat(QImage::Format_RGBA8888);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(const_cast<uchar*>(img.constBits()), 64, 48, 8,
                                                 size_t(img.bytesPerLine()), cs, kCGImageAlphaNoneSkipLast);
        CGImageRef cg = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        CGImageDestinationRef d =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)urlOf(path), CFSTR("public.jpeg"), 1, nullptr);
        NSDictionary* props = @{
            (__bridge NSString*)kCGImagePropertyOrientation : @6,
            (__bridge NSString*)kCGImagePropertyGPSDictionary : @{
                (__bridge NSString*)kCGImagePropertyGPSLatitude : @48.1,
                (__bridge NSString*)kCGImagePropertyGPSLatitudeRef : @"N",
                (__bridge NSString*)kCGImagePropertyGPSLongitude : @11.5,
                (__bridge NSString*)kCGImagePropertyGPSLongitudeRef : @"E",
            },
            (__bridge NSString*)kCGImagePropertyExifDictionary :
                @{(__bridge NSString*)kCGImagePropertyExifUserComment : @"lumaflip-test"},
            (__bridge NSString*)
            kCGImagePropertyTIFFDictionary : @{(__bridge NSString*)kCGImagePropertyTIFFMake : @"Test"},
        };
        CGImageDestinationAddImage(d, cg, (__bridge CFDictionaryRef)props);
        const bool ok = CGImageDestinationFinalize(d);
        CFRelease(d);
        CGImageRelease(cg);
        return ok;
    }
}

NSDictionary* propertiesOf(const QString& path) {
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)urlOf(path), nullptr);
    if (!src)
        return nil;
    NSDictionary* p = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr));
    CFRelease(src);
    return p;
}

QString colorSpaceName(const QString& path) {
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)urlOf(path), nullptr);
    if (!src)
        return {};
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    QString name;
    if (img) {
        CFStringRef n = CGColorSpaceCopyName(CGImageGetColorSpace(img));
        if (n) {
            name = QString::fromCFString(n);
            CFRelease(n);
        }
        CGImageRelease(img);
    }
    CFRelease(src);
    return name;
}

QByteArray readBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

TEST(ExportMac, F_EDIT_04_MetadataModesAndOrientation) {
    QTemporaryDir dir;
    const QString src = dir.filePath("src.jpg");
    ASSERT_TRUE(writeSourceJpeg(src));
    const FileStamp before = fileStamp(src);
    exporting::Options o;
    o.formatId = QStringLiteral("jpeg");

    const QString all = dir.filePath("all.jpg");
    o.metadata = exporting::MetadataMode::KeepAll;
    ASSERT_EQ(exporting::exportOne(src, all, o), QString());
    NSDictionary* p = propertiesOf(all);
    EXPECT_NE(p[(__bridge NSString*)kCGImagePropertyGPSDictionary], nil);
    EXPECT_NE(p[(__bridge NSString*)kCGImagePropertyExifDictionary], nil);
    EXPECT_EQ([p[(__bridge NSString*)kCGImagePropertyOrientation] intValue] <= 1, true); // baked in
    EXPECT_EQ([p[(__bridge NSString*)kCGImagePropertyPixelWidth] intValue], 48);
    EXPECT_EQ([p[(__bridge NSString*)kCGImagePropertyPixelHeight] intValue], 64);

    const QString noGps = dir.filePath("nogps.jpg");
    o.metadata = exporting::MetadataMode::RemoveGps;
    ASSERT_EQ(exporting::exportOne(src, noGps, o), QString());
    p = propertiesOf(noGps);
    EXPECT_EQ(p[(__bridge NSString*)kCGImagePropertyGPSDictionary], nil);
    EXPECT_NE(p[(__bridge NSString*)kCGImagePropertyTIFFDictionary], nil); // the rest stays

    const QString none = dir.filePath("none.jpg");
    o.metadata = exporting::MetadataMode::RemoveAll;
    ASSERT_EQ(exporting::exportOne(src, none, o), QString());
    p = propertiesOf(none);
    EXPECT_EQ(p[(__bridge NSString*)kCGImagePropertyGPSDictionary], nil);
    EXPECT_EQ(
        [p[(__bridge NSString*)kCGImagePropertyTIFFDictionary][(__bridge NSString*)kCGImagePropertyTIFFMake]
            length],
        0u);
    EXPECT_FALSE(readBytes(none).contains("lumaflip-test"));
    EXPECT_EQ([p[(__bridge NSString*)kCGImagePropertyPixelWidth] intValue], 48);

    EXPECT_EQ(fileStamp(src), before); // the source is never changed
}

TEST(ExportMac, F_EDIT_04_QualityAndColorSpace) {
    QTemporaryDir dir;
    const QString src = dir.filePath("src.jpg");
    ASSERT_TRUE(writeSourceJpeg(src));
    exporting::Options o;
    o.formatId = QStringLiteral("jpeg");
    o.quality = 20;
    const QString low = dir.filePath("q20.jpg");
    ASSERT_EQ(exporting::exportOne(src, low, o), QString());
    o.quality = 100;
    const QString high = dir.filePath("q100.jpg");
    ASSERT_EQ(exporting::exportOne(src, high, o), QString());
    EXPECT_LT(QFile(low).size(), QFile(high).size());

    o.color = exporting::ColorMode::DisplayP3;
    const QString p3 = dir.filePath("p3.png");
    o.formatId = QStringLiteral("png");
    ASSERT_EQ(exporting::exportOne(src, p3, o), QString());
    EXPECT_TRUE(colorSpaceName(p3).contains(QStringLiteral("P3"))) << colorSpaceName(p3).toStdString();
    o.color = exporting::ColorMode::SRgb;
    const QString srgb = dir.filePath("srgb.tif");
    o.formatId = QStringLiteral("tiff");
    ASSERT_EQ(exporting::exportOne(src, srgb, o), QString());
    EXPECT_TRUE(colorSpaceName(srgb).contains(QStringLiteral("SRGB"), Qt::CaseInsensitive))
        << colorSpaceName(srgb).toStdString();
}

TEST(ExportMac, F_EDIT_04_HdrGainMapKeptOrToneMapped) {
    QTemporaryDir dir;
    // An ISO gain-map JPEG written by ImageIO from an extended-range image.
    const QString src = dir.filePath("hdr.jpg");
    @autoreleasepool {
        const size_t w = 32, h = 24;
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, w, h, 16, 0, cs,
                                                 uint32_t(kCGImageAlphaPremultipliedLast) |
                                                     uint32_t(kCGBitmapFloatComponents) |
                                                     uint32_t(kCGBitmapByteOrder16Little));
        ASSERT_TRUE(ctx);
        CGContextSetRGBFillColor(ctx, 0.2, 0.3, 0.4, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
        const CGFloat bright[] = {3.0, 3.0, 3.0, 1.0};
        CGColorRef c = CGColorCreate(cs, bright);
        CGContextSetFillColorWithColor(ctx, c);
        CGContextFillRect(ctx, CGRectMake(0, 0, w / 2, h / 2));
        CGColorRelease(c);
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CGImageRef withHeadroom = CGImageCreateCopyWithContentHeadroom(3.0f, img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        CGImageDestinationRef d =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)urlOf(src), CFSTR("public.jpeg"), 1, nullptr);
        NSDictionary* opts = @{
            (__bridge NSString*)
            kCGImageDestinationEncodeRequest : (__bridge NSString*)kCGImageDestinationEncodeToISOGainmap
        };
        CGImageDestinationAddImage(d, withHeadroom ? withHeadroom : img, (__bridge CFDictionaryRef)opts);
        const bool ok = CGImageDestinationFinalize(d);
        CFRelease(d);
        if (withHeadroom)
            CGImageRelease(withHeadroom);
        CGImageRelease(img);
        ASSERT_TRUE(ok);
    }
    auto hasGainMap = [](const QString& path) {
        CGImageSourceRef s = CGImageSourceCreateWithURL((__bridge CFURLRef)urlOf(path), nullptr);
        if (!s)
            return false;
        bool found = false;
        for (CFStringRef t : {kCGImageAuxiliaryDataTypeISOGainMap, kCGImageAuxiliaryDataTypeHDRGainMap})
            if (CFDictionaryRef info = CGImageSourceCopyAuxiliaryDataInfoAtIndex(s, 0, t)) {
                CFRelease(info);
                found = true;
            }
        CFRelease(s);
        return found;
    };
    if (!hasGainMap(src))
        GTEST_SKIP() << "this macOS cannot write ISO gain maps";
    exporting::Options o;
    o.formatId = QStringLiteral("jpeg");
    o.hdr = exporting::HdrMode::Keep;
    const QString kept = dir.filePath("kept.jpg");
    ASSERT_EQ(exporting::exportOne(src, kept, o), QString());
    EXPECT_TRUE(hasGainMap(kept));
    o.hdr = exporting::HdrMode::ToneMapSdr;
    const QString sdr = dir.filePath("sdr.jpg");
    ASSERT_EQ(exporting::exportOne(src, sdr, o), QString());
    EXPECT_FALSE(hasGainMap(sdr));
    // PNG cannot hold a gain map: exported as SDR with a notice.
    o.hdr = exporting::HdrMode::Keep;
    o.formatId = QStringLiteral("png");
    QString notice;
    ASSERT_EQ(exporting::exportOne(src, dir.filePath("hdr.png"), o, &notice), QString());
    EXPECT_FALSE(notice.isEmpty());
}

TEST(ExportMac, F_EDIT_04_DecodedPixelsOfContainerMembers) {
    QTemporaryDir dir;
    exporting::Options o;
    o.formatId = QStringLiteral("png");
    o.maxLongEdge = 20;
    const QImage img = test::noisyPattern(QSize(40, 10));
    const QString out = dir.filePath("member.png");
    QString error;
    ASSERT_TRUE(platform::exportDecoded(img, out, o, &error, nullptr)) << error.toStdString();
    EXPECT_EQ(QImage(out).size(), QSize(20, 5));
}
