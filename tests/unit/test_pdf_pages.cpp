// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-10: PDF pages through the platform renderer (core/PdfDocument.h: CoreGraphics on
// macOS, QtPdf elsewhere). The document is written with QPdfWriter, so the test needs no
// corpus file; skipped where the build has no renderer.

#include "QtTestApp.h"
#include "core/PdfDocument.h"

#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace lf;

TEST(PdfPages, F_OPEN_10_PageCountSizeAndRendering) {
    if (!pdf::available())
        GTEST_SKIP() << "no PDF renderer in this build";
    test::ensureQtApp();
    QTemporaryDir dir;
    const QString path = dir.filePath("doc.pdf");
    {
        QPdfWriter writer(path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(72);
        QPainter p(&writer);
        p.fillRect(QRect(0, 0, 200, 200), Qt::red); // top left of page 1
        writer.newPage();
        p.fillRect(QRect(300, 600, 100, 100), Qt::blue);
    }
    QString error;
    EXPECT_EQ(pdf::pageCount(path, &error), 2) << error.toStdString();
    const QSizeF size = pdf::pageSize(path, 0, &error);
    EXPECT_NEAR(size.width(), 595.0, 2.0);
    EXPECT_NEAR(size.height(), 842.0, 2.0);
    EXPECT_FALSE(pdf::pageSize(path, 5, &error).isValid());
    EXPECT_FALSE(error.isEmpty());

    DecodeRequest req;
    req.maxPixelSize = 842;
    const auto page = pdf::renderPage(path, 0, req, &error);
    ASSERT_TRUE(page.has_value()) << error.toStdString();
    EXPECT_EQ(std::max(page->image.width(), page->image.height()), 842);
    EXPECT_TRUE(page->isPreview); // below the 300 dpi full size
    EXPECT_EQ(page->fullSize, pdf::fullPixelSize(size));
    const QImage img = page->image.convertToFormat(QImage::Format_RGB32);
    const QColor red = img.pixelColor(50, 50), paper = img.pixelColor(500, 700);
    EXPECT_GT(red.red(), 200);
    EXPECT_LT(red.green(), 60);
    EXPECT_GT(paper.red(), 240); // white paper, not transparent black
    EXPECT_GT(paper.blue(), 240);

    const QString broken = dir.filePath("broken.pdf");
    {
        QFile f(broken);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("%PDF-1.7\nnot really");
    }
    EXPECT_EQ(pdf::pageCount(broken, &error), -1);
    EXPECT_FALSE(error.isEmpty());
}
