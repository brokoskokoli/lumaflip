// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-10 (M4): PDF pages browsed like a folder - page count, render size and orientation
// with CoreGraphics offscreen (no windows), in process and in the sandboxed decoder helper.
// The PDFs are generated here with CGPDFContext.

#include "QtTestApp.h"
#include "core/Archive.h"
#include "core/Collection.h"
#include "core/DecoderHelper.h"
#include "core/DecoderIpc.h"
#include "core/PdfDocument.h"
#include "core/VirtualPath.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QTemporaryDir>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

using namespace lf;

namespace {

// Pages of `sizes` (points); each page white with a black square of 1/4 of its width at the
// top left corner (orientation check).
bool writePdf(const QString& path, const QList<QSizeF>& sizes) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        CGRect first = CGRectMake(0, 0, sizes.first().width(), sizes.first().height());
        CGContextRef ctx = CGPDFContextCreateWithURL((__bridge CFURLRef)url, &first, nullptr);
        if (!ctx)
            return false;
        for (const QSizeF& s : sizes) {
            CGRect box = CGRectMake(0, 0, s.width(), s.height());
            NSDictionary* info =
                @{(__bridge NSString*)kCGPDFContextMediaBox : [NSData dataWithBytes:&box length:sizeof box]};
            CGPDFContextBeginPage(ctx, (__bridge CFDictionaryRef)info);
            CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
            CGContextFillRect(ctx, box);
            CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
            const CGFloat q = s.width() / 4;
            CGContextFillRect(ctx, CGRectMake(0, s.height() - q, q, q)); // PDF origin is bottom left
            CGPDFContextEndPage(ctx);
        }
        CGPDFContextClose(ctx);
        CGContextRelease(ctx);
        return true;
    }
}

bool isDark(const QImage& img, int x, int y) {
    return qGray(img.pixel(x, y)) < 64;
}

class PdfTest : public ::testing::Test {
protected:
    void SetUp() override {
        test::ensureQtApp();
        ASSERT_TRUE(dir.isValid());
        pdf = QDir::cleanPath(dir.path()) + QStringLiteral("/Plan.pdf");
        ASSERT_TRUE(writePdf(pdf, {QSizeF(612, 792), QSizeF(842, 595), QSizeF(200, 200)}));
    }
    QTemporaryDir dir;
    QString pdf;
};

} // namespace

TEST_F(PdfTest, F_OPEN_10_PdfPageCountAndSizes) {
    ASSERT_TRUE(pdf::available());
    QString error;
    EXPECT_EQ(pdf::pageCount(pdf, &error), 3);
    EXPECT_EQ(pdf::pageSize(pdf, 1, &error), QSizeF(842, 595));
    EXPECT_FALSE(pdf::pageSize(pdf, 7, &error).isValid());
    EXPECT_FALSE(error.isEmpty());
    // Full resolution = 300 dpi.
    EXPECT_EQ(pdf::fullPixelSize(QSizeF(612, 792)), QSize(2550, 3300));
    // The listing: one member per page, in page order.
    const archive::Listing l = archive::listLocal(pdf);
    ASSERT_TRUE(l.ok) << l.error.toStdString();
    ASSERT_EQ(l.members.size(), 3);
    EXPECT_EQ(l.members[2].name, QStringLiteral("0003"));
    // Damaged and missing documents fail cleanly.
    QFile bad(QDir::cleanPath(dir.path()) + QStringLiteral("/bad.pdf"));
    ASSERT_TRUE(bad.open(QIODevice::WriteOnly));
    bad.write("%PDF-1.4 garbage");
    bad.close();
    EXPECT_EQ(pdf::pageCount(bad.fileName(), &error), -1);
    EXPECT_FALSE(archive::listLocal(bad.fileName()).ok);
}

TEST_F(PdfTest, F_OPEN_10_PdfRendersAtRequestedSize) {
    QString error;
    DecodeRequest req;
    req.maxPixelSize = 400;
    const auto page = pdf::renderPage(pdf, 0, req, &error);
    ASSERT_TRUE(page.has_value()) << error.toStdString();
    // Longer edge = the request, aspect ratio of the page, the full size known.
    EXPECT_EQ(page->image.height(), 400);
    EXPECT_NEAR(page->image.width(), 400.0 * 612 / 792, 1.0);
    EXPECT_EQ(page->fullSize, QSize(2550, 3300));
    EXPECT_TRUE(page->isPreview);
    // Upright: the black square is at the top left, the rest white.
    EXPECT_TRUE(isDark(page->image, 5, 5));
    EXPECT_FALSE(isDark(page->image, page->image.width() - 5, page->image.height() - 5));
    EXPECT_FALSE(isDark(page->image, 5, page->image.height() - 5));
    // The memory limit wins over the pixel size.
    DecodeRequest small;
    small.maxBytes = 100 * 100 * 4;
    const auto limited = pdf::renderPage(pdf, 2, small, &error);
    ASSERT_TRUE(limited.has_value());
    EXPECT_LE(qint64(limited->image.width()) * limited->image.height() * 4, small.maxBytes);
}

TEST_F(PdfTest, F_OPEN_10_PdfPagesThroughTheRegistryAndCollection) {
    QString error;
    DecodeRequest req;
    req.maxPixelSize = 300;
    const QString page2 = vpath::join(pdf, vpath::pdfPageMember(1));
    const auto img = DecoderRegistry::instance().decode(page2, req, &error);
    ASSERT_TRUE(img.has_value()) << error.toStdString();
    EXPECT_EQ(img->image.width(), 300); // landscape page
    const auto info = DecoderRegistry::instance().info(page2, &error);
    ASSERT_TRUE(info.has_value()) << error.toStdString();
    EXPECT_EQ(info->formatName, QStringLiteral("PDF"));
    EXPECT_FALSE(DecoderRegistry::instance().decode(vpath::join(pdf, QStringLiteral("0009")), req, &error));

    lf::Collection c;
    bool done = false;
    QObject::connect(&c, &lf::Collection::loadingFinished, [&] { done = true; });
    c.openContainer(pdf);
    ASSERT_TRUE(test::waitUntil([&] { return done; }, 10000));
    ASSERT_EQ(c.count(), 3);
    EXPECT_EQ(c.at(0).name, QStringLiteral("Seite 1"));
    EXPECT_EQ(c.at(2).path, vpath::join(pdf, QStringLiteral("0003")));
}

#ifdef LF_DECODER_HELPER_BINARY
TEST_F(PdfTest, F_OPEN_10_PdfPagesRenderInTheSandboxedHelper) {
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.maxProcesses = 1;
    o.extraEnvironment = {QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    auto pool = std::make_shared<DecoderHelperPool>(o);
    RemoteDecoder remote(std::make_shared<pdf::PdfPageDecoder>(), pool);
    QString error;
    DecodeRequest req;
    req.maxPixelSize = 256;
    const auto img = remote.decode(vpath::join(pdf, QStringLiteral("0001")), req, &error);
    ASSERT_TRUE(img.has_value()) << error.toStdString();
    EXPECT_EQ(img->image.height(), 256);
    EXPECT_TRUE(isDark(img->image, 3, 3));
    EXPECT_FALSE(isDark(img->image, img->image.width() - 3, img->image.height() - 3));
    ipc::Request list;
    list.op = ipc::Op::ArchiveList;
    list.path = pdf;
    const auto listed = pool->call(list, 20000, nullptr, &error);
    ASSERT_TRUE(listed.has_value()) << error.toStdString();
    EXPECT_TRUE(listed->ok) << listed->error.toStdString();
    EXPECT_EQ(listed->members.size(), 3);
}
#endif
