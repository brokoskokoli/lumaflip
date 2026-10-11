// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-10 (M4): PDF pages via CoreGraphics (core/PdfDocument.h). CGPDFDocument draws into
// a CPU bitmap context: no GPU, no window server, no PDFKit - usable in the decoder helper's
// sandbox (reads only).

#include "core/PdfDocument.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QImage>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>

namespace lf::pdf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::PdfDocument)
};

struct Document {
    CGPDFDocumentRef doc = nullptr;
    ~Document() {
        if (doc)
            CGPDFDocumentRelease(doc);
    }
};

bool open(const QString& path, Document& d, QString* error) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        d.doc = CGPDFDocumentCreateWithURL((__bridge CFURLRef)url);
    }
    if (!d.doc) {
        if (error)
            *error = QFile::exists(path) ? Tr::tr("Das PDF-Dokument ist beschädigt.")
                                         : Tr::tr("Die Datei existiert nicht.");
        return false;
    }
    if (CGPDFDocumentIsEncrypted(d.doc) && !CGPDFDocumentIsUnlocked(d.doc)) {
        if (error)
            *error = Tr::tr("Das PDF-Dokument ist passwortgeschützt.");
        return false;
    }
    return true;
}

// The visible area of the page in points, after its /Rotate.
QSizeF rotatedSize(CGPDFPageRef page) {
    const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    const int angle = ((CGPDFPageGetRotationAngle(page) % 360) + 360) % 360;
    if (angle == 90 || angle == 270)
        return QSizeF(box.size.height, box.size.width);
    return QSizeF(box.size.width, box.size.height);
}

CGPDFPageRef pageAt(const Document& d, int index, QString* error) {
    const size_t count = CGPDFDocumentGetNumberOfPages(d.doc);
    CGPDFPageRef page =
        index >= 0 && size_t(index) < count ? CGPDFDocumentGetPage(d.doc, size_t(index) + 1) : nullptr;
    if (!page && error)
        *error = Tr::tr("Die Seite gibt es im PDF-Dokument nicht.");
    return page;
}

} // namespace

bool available() {
    return true;
}

int pageCount(const QString& path, QString* error) {
    Document d;
    if (!open(path, d, error))
        return -1;
    return int(std::min<size_t>(CGPDFDocumentGetNumberOfPages(d.doc), 100000));
}

QSizeF pageSize(const QString& path, int index, QString* error) {
    Document d;
    if (!open(path, d, error))
        return {};
    CGPDFPageRef page = pageAt(d, index, error);
    return page ? rotatedSize(page) : QSizeF();
}

std::optional<DecodedImage> renderPage(const QString& path, int index, const DecodeRequest& req,
                                       QString* error) {
    Document d;
    if (!open(path, d, error))
        return std::nullopt;
    CGPDFPageRef page = pageAt(d, index, error);
    if (!page)
        return std::nullopt;
    const QSizeF points = rotatedSize(page);
    const QSize full = fullPixelSize(points);
    const QSize target = targetPixelSize(points, req);
    if (target.isEmpty()) {
        if (error)
            *error = Tr::tr("Die Seite ist leer.");
        return std::nullopt;
    }
    QImage image(target, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        if (error)
            *error = Tr::tr("Nicht genug Arbeitsspeicher für die Seite.");
        return std::nullopt;
    }
    image.fill(Qt::white);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // ARGB32 premultiplied in memory order B, G, R, A on little endian.
    CGContextRef ctx = CGBitmapContextCreate(
        image.bits(), size_t(image.width()), size_t(image.height()), 8, size_t(image.bytesPerLine()), cs,
        uint32_t(kCGImageAlphaPremultipliedFirst) | uint32_t(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(cs);
    if (!ctx) {
        if (error)
            *error = Tr::tr("Nicht genug Arbeitsspeicher für die Seite.");
        return std::nullopt;
    }
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextSetRenderingIntent(ctx, kCGRenderingIntentDefault);
    // The page's crop box, rotated, scaled to fill the bitmap (aspect kept by
    // targetPixelSize()).
    const CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    const int angle = ((CGPDFPageGetRotationAngle(page) % 360) + 360) % 360;
    const double sx = double(target.width()) / points.width();
    const double sy = double(target.height()) / points.height();
    CGContextScaleCTM(ctx, CGFloat(sx), CGFloat(sy));
    switch (angle) {
    case 90:
        CGContextTranslateCTM(ctx, 0, CGFloat(points.height()));
        CGContextRotateCTM(ctx, CGFloat(-M_PI_2));
        break;
    case 180:
        CGContextTranslateCTM(ctx, CGFloat(points.width()), CGFloat(points.height()));
        CGContextRotateCTM(ctx, CGFloat(M_PI));
        break;
    case 270:
        CGContextTranslateCTM(ctx, CGFloat(points.width()), 0);
        CGContextRotateCTM(ctx, CGFloat(M_PI_2));
        break;
    default:
        break;
    }
    CGContextTranslateCTM(ctx, -box.origin.x, -box.origin.y);
    CGContextClipToRect(ctx, box);
    CGContextDrawPDFPage(ctx, page);
    CGContextRelease(ctx);
    image.setColorSpace(QColorSpace::SRgb);

    DecodedImage out;
    out.image = std::move(image);
    out.fullSize = full;
    out.isPreview = target.width() < full.width() || target.height() < full.height();
    return out;
}

} // namespace lf::pdf
