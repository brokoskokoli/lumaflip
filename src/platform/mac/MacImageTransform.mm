// SPDX-License-Identifier: GPL-3.0-or-later
// macOS part of saving the rotation (F-EDIT-02, see core/LosslessTransform.h): ImageIO for
// the lossless metadata copy and re-encoding, copyfile/setattrlist/rename for the atomic
// replacement that keeps the file's attributes.

#include "core/LosslessTransform.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <copyfile.h>
#include <sys/attr.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstring>

namespace lf::platform {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::LosslessTransform", text);
}

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

NSURL* fileURL(const QString& path) {
    return [NSURL fileURLWithPath:path.toNSString()];
}

// Owned CGImageSource (nullptr if the file is unreadable). Caller is in an autorelease pool.
CGImageSourceRef openSource(const QString& path) {
    NSDictionary* opts = @{(__bridge NSString*)kCGImageSourceShouldCache : @NO};
    return CGImageSourceCreateWithURL((__bridge CFURLRef)fileURL(path), (__bridge CFDictionaryRef)opts);
}

QString errorText(CFErrorRef err) {
    if (!err)
        return {};
    NSError* e = CFBridgingRelease(err);
    return QString::fromNSString(e.localizedDescription);
}

// Same matrices as core (x' = m0 x + m1 y, y' = m2 x + m3 y; pixel coordinates, y down).
constexpr std::array<std::array<int, 4>, 9> kOrient = {{{1, 0, 0, 1},
                                                        {1, 0, 0, 1},
                                                        {-1, 0, 0, 1},
                                                        {-1, 0, 0, -1},
                                                        {1, 0, 0, -1},
                                                        {0, 1, 1, 0},
                                                        {0, -1, 1, 0},
                                                        {0, -1, -1, 0},
                                                        {0, 1, -1, 0}}};

} // namespace

QString imageTypeIdentifier(const QString& path) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(path);
        if (!src)
            return {};
        CFStringRef t = CGImageSourceGetType(src);
        const QString type = t ? QString::fromNSString((__bridge NSString*)t) : QString();
        CFRelease(src);
        return type;
    }
}

bool isCameraRawType(const QString& typeId) {
    @autoreleasepool {
        UTType* type = [UTType typeWithIdentifier:typeId.toNSString()];
        UTType* raw = [UTType typeWithIdentifier:@"public.camera-raw-image"];
        return type && raw && [type conformsToType:raw];
    }
}

int imageCount(const QString& path) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(path);
        if (!src)
            return 0;
        const int n = int(CGImageSourceGetCount(src));
        CFRelease(src);
        return n;
    }
}

bool supportsOrientationMetadata(const QString& typeId) {
    // Formats for which CGImageDestinationCopyImageSource rewrites only the metadata.
    return typeId == QLatin1String("public.jpeg") || typeId == QLatin1String("public.heic") ||
           typeId == QLatin1String("public.heif") || typeId == QLatin1String("public.tiff") ||
           typeId == QLatin1String("public.png");
}

bool isLosslessCodec(const QString& typeId) {
    return typeId == QLatin1String("public.png") || typeId == QLatin1String("com.microsoft.bmp") ||
           typeId == QLatin1String("public.tiff") || typeId == QLatin1String("com.truevision.tga-image");
}

bool writeOrientationLossless(const QString& srcPath, const QString& dstPath, int orientation,
                              QString* error) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(srcPath);
        if (!src) {
            setError(error, tr("Die Datei kann nicht gelesen werden."));
            return false;
        }
        CFStringRef type = CGImageSourceGetType(src);
        CGImageDestinationRef dst =
            type ? CGImageDestinationCreateWithURL((__bridge CFURLRef)fileURL(dstPath), type,
                                                   CGImageSourceGetCount(src), nullptr)
                 : nullptr;
        if (!dst) {
            CFRelease(src);
            setError(error, tr("Für dieses Format kann die Ausrichtung nicht gespeichert werden."));
            return false;
        }
        NSDictionary* opts = @{(__bridge NSString*)kCGImageDestinationOrientation : @(orientation)};
        CFErrorRef err = nullptr;
        // Copies the compressed image data unchanged and finalizes the destination.
        const bool ok = CGImageDestinationCopyImageSource(dst, src, (__bridge CFDictionaryRef)opts, &err);
        CFRelease(dst);
        CFRelease(src);
        if (!ok) {
            const QString detail = errorText(err);
            QFile::remove(dstPath);
            setError(error, tr("Die Ausrichtung konnte nicht verlustfrei geschrieben werden.") +
                                (detail.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(detail)));
            return false;
        }
        return QFileInfo::exists(dstPath);
    }
}

bool reencodeTransformed(const QString& srcPath, const QString& dstPath, int pixelOp, QString* error) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(srcPath);
        if (!src) {
            setError(error, tr("Die Datei kann nicht gelesen werden."));
            return false;
        }
        const size_t index = CGImageSourceGetPrimaryImageIndex(src);
        NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, index, nullptr));
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, index, nullptr);
        CFStringRef type = CGImageSourceGetType(src);
        if (!img || !type) {
            if (img)
                CGImageRelease(img);
            CFRelease(src);
            setError(error, tr("Das Bild konnte nicht dekodiert werden."));
            return false;
        }
        const auto& m = kOrient[size_t(pixelOp >= 1 && pixelOp <= 8 ? pixelOp : 1)];
        const size_t W = CGImageGetWidth(img), H = CGImageGetHeight(img);
        const bool swap = m[0] == 0;
        const size_t W2 = swap ? H : W, H2 = swap ? W : H;
        const bool deep = CGImageGetBitsPerComponent(img) > 8;
        CGColorSpaceRef cs = CGImageGetColorSpace(img);
        CGColorSpaceRef own = nullptr;
        if (!cs || CGColorSpaceGetModel(cs) != kCGColorSpaceModelRGB)
            cs = own = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        const CGImageAlphaInfo ai = CGImageGetAlphaInfo(img);
        const bool alpha =
            ai != kCGImageAlphaNone && ai != kCGImageAlphaNoneSkipLast && ai != kCGImageAlphaNoneSkipFirst;
        const uint32_t info = (alpha ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast) |
                              (deep ? kCGBitmapByteOrder16Host : kCGBitmapByteOrderDefault);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, W2, H2, deep ? 16 : 8, 0, cs, info);
        if (own)
            CGColorSpaceRelease(own);
        if (!ctx) {
            CGImageRelease(img);
            CFRelease(src);
            setError(error, tr("Nicht genug Speicher zum Drehen."));
            return false;
        }
        // Pixel map p' = M p + t (y down), expressed in CG coordinates (y up), see header.
        const double w = double(W), h = double(H), h2 = double(H2);
        double minX = 0, minY = 0;
        for (const auto& c : {std::array<double, 2>{0, 0}, {w, 0}, {0, h}, {w, h}}) {
            minX = std::min(minX, m[0] * c[0] + m[1] * c[1]);
            minY = std::min(minY, m[2] * c[0] + m[3] * c[1]);
        }
        const double tx = -minX, ty = -minY;
        const CGAffineTransform t = CGAffineTransformMake(double(m[0]), double(-m[2]), double(-m[1]),
                                                          double(m[3]), m[1] * h + tx, h2 - m[3] * h - ty);
        CGContextConcatCTM(ctx, t);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
        CGImageRef rotated = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        CGImageRelease(img);

        NSMutableDictionary* out = props ? [props mutableCopy] : [NSMutableDictionary dictionary];
        out[(__bridge NSString*)kCGImagePropertyOrientation] = @1;
        NSMutableDictionary* tiff = [out[(__bridge NSString*)kCGImagePropertyTIFFDictionary] mutableCopy];
        if (!tiff)
            tiff = [NSMutableDictionary dictionary];
        tiff[(__bridge NSString*)kCGImagePropertyTIFFOrientation] = @1;
        out[(__bridge NSString*)kCGImagePropertyTIFFDictionary] = tiff;
        [out removeObjectForKey:(__bridge NSString*)kCGImagePropertyPixelWidth];
        [out removeObjectForKey:(__bridge NSString*)kCGImagePropertyPixelHeight];
        out[(__bridge NSString*)kCGImageDestinationLossyCompressionQuality] = @0.95;
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)fileURL(dstPath), type, 1, nullptr);
        bool ok = false;
        if (dst && rotated) {
            CGImageDestinationAddImage(dst, rotated, (__bridge CFDictionaryRef)out);
            ok = CGImageDestinationFinalize(dst);
        }
        if (dst)
            CFRelease(dst);
        if (rotated)
            CGImageRelease(rotated);
        CFRelease(src);
        if (!ok) {
            QFile::remove(dstPath);
            setError(error, tr("Dieses Format kann nicht geschrieben werden."));
        }
        return ok;
    }
}

bool replaceFile(const QString& replacement, const QString& target, bool keepModificationDate,
                 QString* error) {
    const QByteArray from = QFile::encodeName(replacement);
    const QByteArray to = QFile::encodeName(target);
    struct stat st{};
    if (stat(to.constData(), &st) != 0) {
        setError(error, tr("„%1“ existiert nicht mehr.").arg(QFileInfo(target).fileName()));
        return false;
    }
    // Permissions, ACL, owner (as far as allowed) and extended attributes (Finder tags,
    // comments, quarantine) of the original.
    if (copyfile(to.constData(), from.constData(), nullptr, COPYFILE_SECURITY | COPYFILE_XATTR) != 0) {
        setError(error, tr("Die Dateiattribute konnten nicht übernommen werden."));
        return false;
    }
    // Creation date and "date added" (Finder sorting) as before.
    struct {
        u_int32_t length;
        struct timespec added;
    } __attribute__((aligned(4), packed)) addedBuf{};
    struct attrlist addedList{};
    addedList.bitmapcount = ATTR_BIT_MAP_COUNT;
    addedList.commonattr = ATTR_CMN_ADDEDTIME;
    const bool haveAdded = getattrlist(to.constData(), &addedList, &addedBuf, sizeof(addedBuf), 0) == 0 &&
                           addedBuf.length >= sizeof(u_int32_t) + sizeof(struct timespec);
    struct attrlist crList{};
    crList.bitmapcount = ATTR_BIT_MAP_COUNT;
    crList.commonattr = ATTR_CMN_CRTIME;
    struct timespec created = st.st_birthtimespec;
    setattrlist(from.constData(), &crList, &created, sizeof(created), 0);
    if (keepModificationDate) {
        const struct timespec times[2] = {st.st_atimespec, st.st_mtimespec};
        utimensat(AT_FDCWD, from.constData(), times, 0);
    }
    if (rename(from.constData(), to.constData()) != 0) {
        setError(error, tr("„%1“ konnte nicht ersetzt werden: %2")
                            .arg(QFileInfo(target).fileName(), QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    if (haveAdded) {
        // The date added belongs to the directory entry: set it after the rename.
        struct timespec added = addedBuf.added;
        setattrlist(to.constData(), &addedList, &added, sizeof(added), 0);
    }
    return true;
}

bool copyWithMetadata(const QString& path, const QString& copy, QString* error) {
    const QByteArray from = QFile::encodeName(path);
    const QByteArray to = QFile::encodeName(copy);
    if (copyfile(from.constData(), to.constData(), nullptr, COPYFILE_ALL | COPYFILE_CLONE) != 0) {
        setError(error, tr("Sicherungskopie für „Widerrufen“ konnte nicht angelegt werden."));
        return false;
    }
    return true;
}

} // namespace lf::platform
