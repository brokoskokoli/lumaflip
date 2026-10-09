// SPDX-License-Identifier: GPL-3.0-or-later
// Sorting by capture date / pixel count / rating with values read from the files
// themselves (F-SORT-02): generated JPEGs with EXIF DateTimeOriginal and XMP rating.

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/Platform.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

using namespace lf;

namespace {

// Writes a w x h JPEG with optional EXIF DateTimeOriginal ("yyyy:MM:dd HH:mm:ss") and rating.
bool writeJpeg(const QString& path, int w, int h, const char* dateTimeOriginal, int rating = -1) {
    bool ok = false;
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, size_t(w), size_t(h), 8, 0, cs,
                                                 kCGImageAlphaNoneSkipLast);
        CGContextSetRGBFillColor(ctx, 0.3, 0.5, 0.7, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
        NSMutableDictionary* props = [NSMutableDictionary dictionary];
        if (dateTimeOriginal)
            props[(id)kCGImagePropertyExifDictionary] =
                @{(id)kCGImagePropertyExifDateTimeOriginal : @(dateTimeOriginal)};
        if (rating >= 0)
            props[(id)kCGImagePropertyIPTCDictionary] = @{(id)kCGImagePropertyIPTCStarRating : @(rating)};
        if (dst) {
            CGImageDestinationAddImage(dst, img, (__bridge CFDictionaryRef)props);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CGImageRelease(img);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
    }
    return ok;
}

class ImagePropsSortTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test::ensureApp();
        ASSERT_TRUE(dir.isValid());
        collator = createNameCollator();
        attributes = createFileAttributeProvider();
    }
    QString path(const QString& name) const { return QDir::cleanPath(dir.path()) + "/" + name; }
    QStringList sortedNames(const SortSpec& spec) {
        lf::Collection c;
        c.setProviders(collator.get(), &order, attributes.get());
        int finished = 0;
        QObject::connect(&c, &lf::Collection::loadingFinished, [&] { ++finished; });
        order.setSpec(spec);
        c.openFolder(dir.path());
        EXPECT_TRUE(test::waitUntil([&] { return finished >= 1 && !c.isLoading(); }));
        QStringList out;
        for (const FileEntry& e : c.entries())
            out << e.name;
        return out;
    }

    QTemporaryDir dir;
    std::unique_ptr<NameCollator> collator;
    std::unique_ptr<FileAttributeProvider> attributes;
    test::FakeOrderProvider order;
};

} // namespace

TEST(ImageProperties, F_SORT_02_ReadsCaptureDatePixelsAndRating) {
    QTemporaryDir dir;
    const QString p = QDir(dir.path()).filePath("x.jpg");
    ASSERT_TRUE(writeJpeg(p, 40, 30, "2021:06:15 08:30:00", 4));
    auto attributes = createFileAttributeProvider();
    FileEntry e;
    e.path = p;
    e.name = "x.jpg";
    attributes->fillImageProperties(e);
    EXPECT_TRUE(e.imagePropertiesRead);
    EXPECT_EQ(e.pixelCount, 1200);
    ASSERT_TRUE(e.captured.has_value());
    EXPECT_EQ(e.captured->date(), QDate(2021, 6, 15));
    EXPECT_EQ(e.captured->time(), QTime(8, 30, 0));
    EXPECT_EQ(e.rating, 4);
}

TEST_F(ImagePropsSortTest, F_SORT_02_SortByCaptureDate) {
    // Name order a, b, c, d; capture order c, a, b; d has no EXIF date (sorts last).
    ASSERT_TRUE(writeJpeg(path("a.jpg"), 10, 10, "2020:01:02 10:00:00"));
    ASSERT_TRUE(writeJpeg(path("b.jpg"), 10, 10, "2022:03:04 10:00:00"));
    ASSERT_TRUE(writeJpeg(path("c.jpg"), 10, 10, "2019:12:31 23:59:59"));
    ASSERT_TRUE(writeJpeg(path("d.jpg"), 10, 10, nullptr));
    EXPECT_EQ(sortedNames({SortKey::DateCaptured, true, SortSource::FinderDsStore}),
              (QStringList{"c.jpg", "a.jpg", "b.jpg", "d.jpg"}));
    EXPECT_EQ(sortedNames({SortKey::DateCaptured, false, SortSource::FinderDsStore}),
              (QStringList{"b.jpg", "a.jpg", "c.jpg", "d.jpg"}));
}

TEST_F(ImagePropsSortTest, F_SORT_02_SortByPixelCount) {
    ASSERT_TRUE(writeJpeg(path("a.jpg"), 300, 200, nullptr)); // 60000
    ASSERT_TRUE(writeJpeg(path("b.jpg"), 50, 40, nullptr));   // 2000
    ASSERT_TRUE(writeJpeg(path("c.jpg"), 100, 100, nullptr)); // 10000
    EXPECT_EQ(sortedNames({SortKey::PixelCount, true, SortSource::FinderDsStore}),
              (QStringList{"b.jpg", "c.jpg", "a.jpg"}));
    EXPECT_EQ(sortedNames({SortKey::PixelCount, false, SortSource::FinderDsStore}),
              (QStringList{"a.jpg", "c.jpg", "b.jpg"}));
}

TEST_F(ImagePropsSortTest, F_SORT_02_SortByEmbeddedRating) {
    ASSERT_TRUE(writeJpeg(path("a.jpg"), 10, 10, nullptr, 2));
    ASSERT_TRUE(writeJpeg(path("b.jpg"), 10, 10, nullptr, 5));
    ASSERT_TRUE(writeJpeg(path("c.jpg"), 10, 10, nullptr, 1));
    EXPECT_EQ(sortedNames({SortKey::Rating, false, SortSource::FinderDsStore}),
              (QStringList{"b.jpg", "a.jpg", "c.jpg"}));
}
