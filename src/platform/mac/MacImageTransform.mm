// SPDX-License-Identifier: GPL-3.0-or-later
// macOS part of saving the rotation (F-EDIT-02, see core/LosslessTransform.h): ImageIO for
// the lossless metadata copy and re-encoding, copyfile/clonefile/setattrlist/rename and
// F_FULLFSYNC for the durable replacement that keeps the file's attributes.

#include "core/LosslessTransform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <copyfile.h>
#include <fcntl.h>
#include <sys/attr.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <vector>

namespace lf::platform {

namespace {


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

const std::array<int, 4>& matrixOf(int op) {
    return kOrient[size_t(op >= 1 && op <= 8 ? op : 1)];
}

// ---- auxiliary images (gain maps, depth, mattes) ----

struct AuxType {
    CFStringRef type;
    const char* name; // German, user facing
};

QStringList auxiliaryImages(CGImageSourceRef src) {
    const AuxType types[] = {
        {kCGImageAuxiliaryDataTypeHDRGainMap, QT_TRANSLATE_NOOP("lf::LosslessTransform", "HDR-Gain-Map")},
        {kCGImageAuxiliaryDataTypeISOGainMap, QT_TRANSLATE_NOOP("lf::LosslessTransform", "HDR-Gain-Map")},
        {kCGImageAuxiliaryDataTypeDepth, QT_TRANSLATE_NOOP("lf::LosslessTransform", "Tiefenkarte")},
        {kCGImageAuxiliaryDataTypeDisparity, QT_TRANSLATE_NOOP("lf::LosslessTransform", "Tiefenkarte")},
        {kCGImageAuxiliaryDataTypePortraitEffectsMatte,
         QT_TRANSLATE_NOOP("lf::LosslessTransform", "Porträt-Maske")},
        {kCGImageAuxiliaryDataTypeSemanticSegmentationSkinMatte,
         QT_TRANSLATE_NOOP("lf::LosslessTransform", "Segmentierungsmaske")},
        {kCGImageAuxiliaryDataTypeSemanticSegmentationHairMatte,
         QT_TRANSLATE_NOOP("lf::LosslessTransform", "Segmentierungsmaske")},
        {kCGImageAuxiliaryDataTypeSemanticSegmentationTeethMatte,
         QT_TRANSLATE_NOOP("lf::LosslessTransform", "Segmentierungsmaske")},
        {kCGImageAuxiliaryDataTypeSemanticSegmentationGlassesMatte,
         QT_TRANSLATE_NOOP("lf::LosslessTransform", "Segmentierungsmaske")},
        {kCGImageAuxiliaryDataTypeSemanticSegmentationSkyMatte,
         QT_TRANSLATE_NOOP("lf::LosslessTransform", "Segmentierungsmaske")},
    };
    QStringList found;
    const size_t index = CGImageSourceGetPrimaryImageIndex(src);
    for (const AuxType& t : types) {
        CFDictionaryRef info = CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, index, t.type);
        if (!info)
            continue;
        CFRelease(info);
        found << QString::fromNSString((__bridge NSString*)t.type)
              << QCoreApplication::translate("lf::LosslessTransform", t.name);
    }
    return found; // pairs: type id, German name
}

QString auxNames(const QStringList& pairs) {
    QStringList names;
    for (qsizetype i = 1; i < pairs.size(); i += 2)
        if (!names.contains(pairs.at(i)))
            names << pairs.at(i);
    return names.join(QStringLiteral(", "));
}

QString auxLostMessage(const QStringList& pairs) {
    return QCoreApplication::translate(
               "lf::LosslessTransform",
               "Die Datei enthält Zusatzbilder (%1), die beim Speichern der Ausrichtung verloren gingen. "
               "Die Drehung wurde deshalb nicht gespeichert und gilt weiter nur in der Anzeige.")
        .arg(auxNames(pairs));
}

// ---- raw pixel buffers ----

struct Pixels {
    size_t width = 0, height = 0, bpc = 0, bpp = 0, bytesPerRow = 0;
    CGBitmapInfo info = 0;
    CGColorSpaceRef space = nullptr; // not owned (belongs to the image)
    NSData* data = nil;
};

// The decoded buffer of `img` as it is (no conversion). nullopt if it cannot be moved
// pixel by pixel (sub-byte pixels, packed formats).
std::optional<Pixels> rawPixels(CGImageRef img) {
    Pixels p;
    p.width = CGImageGetWidth(img);
    p.height = CGImageGetHeight(img);
    p.bpc = CGImageGetBitsPerComponent(img);
    p.bpp = CGImageGetBitsPerPixel(img);
    p.bytesPerRow = CGImageGetBytesPerRow(img);
    p.info = CGImageGetBitmapInfo(img);
    p.space = CGImageGetColorSpace(img);
    if (!p.space || p.bpp % 8 != 0 || p.bpp == 0 ||
        CGImageGetPixelFormatInfo(img) != kCGImagePixelFormatPacked)
        return std::nullopt;
    CGDataProviderRef provider = CGImageGetDataProvider(img);
    if (!provider)
        return std::nullopt;
    p.data = CFBridgingRelease(CGDataProviderCopyData(provider));
    if (!p.data || p.data.length < p.bytesPerRow * (p.height - 1) + p.width * (p.bpp / 8))
        return std::nullopt;
    return p;
}

// Moves the pixels of `src` by `op` into a new image of the same layout and color space.
CGImageRef rotatedImage(CGImageRef img, const Pixels& p, int op) {
    const auto& m = matrixOf(op);
    const size_t W = p.width, H = p.height, B = p.bpp / 8;
    const bool swap = m[0] == 0;
    const size_t W2 = swap ? H : W, H2 = swap ? W : H;
    const long minX = std::min(0L, long(m[0]) * long(W - 1)) + std::min(0L, long(m[1]) * long(H - 1));
    const long minY = std::min(0L, long(m[2]) * long(W - 1)) + std::min(0L, long(m[3]) * long(H - 1));
    const size_t rowOut = W2 * B;
    NSMutableData* out = [NSMutableData dataWithLength:rowOut * H2];
    if (!out)
        return nullptr;
    const auto* in = static_cast<const uint8_t*>(p.data.bytes);
    auto* dst = static_cast<uint8_t*>(out.mutableBytes);
    for (size_t y = 0; y < H; ++y) {
        const uint8_t* row = in + y * p.bytesPerRow;
        for (size_t x = 0; x < W; ++x) {
            const long X = m[0] * long(x) + m[1] * long(y) - minX;
            const long Y = m[2] * long(x) + m[3] * long(y) - minY;
            std::memcpy(dst + size_t(Y) * rowOut + size_t(X) * B, row + x * B, B);
        }
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)out);
    CGImageRef result = CGImageCreate(W2, H2, p.bpc, p.bpp, rowOut, p.space, p.info, provider,
                                      CGImageGetDecode(img), false, CGImageGetRenderingIntent(img));
    CGDataProviderRelease(provider);
    return result;
}

// Pixel values independent of the memory layout: per pixel the color components and the
// alpha (max if none), as 16-bit numbers. nullopt for layouts this does not understand.
struct Canonical {
    std::vector<uint32_t> values;
    bool alpha = false;
    bool premultiplied = false;
    bool floating = false;
};

std::optional<Canonical> canonical(CGImageRef img) {
    auto p = rawPixels(img);
    if (!p)
        return std::nullopt;
    Canonical c;
    const CGImageAlphaInfo ai = CGImageAlphaInfo(p->info & kCGBitmapAlphaInfoMask);
    const CGBitmapInfo order = p->info & kCGBitmapByteOrderMask;
    c.floating = (p->info & kCGBitmapFloatComponents) != 0;
    const size_t colors = CGColorSpaceGetNumberOfComponents(p->space);
    const bool hasExtra = ai != kCGImageAlphaNone;
    const bool extraFirst =
        ai == kCGImageAlphaPremultipliedFirst || ai == kCGImageAlphaFirst || ai == kCGImageAlphaNoneSkipFirst;
    c.alpha = ai == kCGImageAlphaPremultipliedFirst || ai == kCGImageAlphaPremultipliedLast ||
              ai == kCGImageAlphaFirst || ai == kCGImageAlphaLast;
    c.premultiplied = ai == kCGImageAlphaPremultipliedFirst || ai == kCGImageAlphaPremultipliedLast;
    if (ai == kCGImageAlphaOnly || (p->bpc != 8 && p->bpc != 16 && p->bpc != 32))
        return std::nullopt;
    const size_t samples = colors + (hasExtra ? 1 : 0);
    if (samples * p->bpc != p->bpp)
        return std::nullopt;
    const size_t B = p->bpp / 8, S = p->bpc / 8;
    // Byte order: 16/32-bit words in little endian reverse the bytes of each word.
    size_t word = 0; // 0 = big endian / bytewise
    if (order == kCGBitmapByteOrder16Little)
        word = 2;
    else if (order == kCGBitmapByteOrder32Little)
        word = 4;
    if (word && (B % word != 0))
        return std::nullopt;
    const uint32_t maxValue = p->bpc == 32 ? 0xFFFFFFFFu : ((1u << p->bpc) - 1u);
    const auto* bytes = static_cast<const uint8_t*>(p->data.bytes);
    c.values.reserve(p->width * p->height * (colors + 1));
    std::vector<uint8_t> px(B);
    for (size_t y = 0; y < p->height; ++y) {
        for (size_t x = 0; x < p->width; ++x) {
            const uint8_t* q = bytes + y * p->bytesPerRow + x * B;
            // Big-endian byte sequence of the pixel.
            for (size_t i = 0; i < B; ++i)
                px[i] = word ? q[(i / word) * word + (word - 1 - i % word)] : q[i];
            std::vector<uint32_t> s(samples);
            for (size_t k = 0; k < samples; ++k) {
                uint32_t v = 0;
                for (size_t b = 0; b < S; ++b)
                    v = (v << 8) | px[k * S + b];
                s[k] = v;
            }
            const size_t first = hasExtra && extraFirst ? 1 : 0;
            for (size_t k = 0; k < colors; ++k)
                c.values.push_back(s[first + k]);
            const uint32_t a = c.alpha ? s[extraFirst ? 0 : samples - 1] : maxValue;
            c.values.push_back(a);
        }
    }
    return c;
}

bool sameColorSpace(CGColorSpaceRef a, CGColorSpaceRef b) {
    if (CGColorSpaceGetModel(a) != CGColorSpaceGetModel(b))
        return false;
    if (CGColorSpaceGetModel(a) != kCGColorSpaceModelIndexed)
        return true; // profiles may be re-serialized; the model and the values must match
    const size_t n = CGColorSpaceGetColorTableCount(a);
    if (n != CGColorSpaceGetColorTableCount(b))
        return false;
    CGColorSpaceRef baseA = CGColorSpaceGetBaseColorSpace(a), baseB = CGColorSpaceGetBaseColorSpace(b);
    if (!baseA || !baseB ||
        CGColorSpaceGetNumberOfComponents(baseA) != CGColorSpaceGetNumberOfComponents(baseB))
        return false;
    const size_t len = n * CGColorSpaceGetNumberOfComponents(baseA);
    std::vector<uint8_t> ta(len), tb(len);
    CGColorSpaceGetColorTable(a, ta.data());
    CGColorSpaceGetColorTable(b, tb.data());
    return ta == tb;
}

// True if `written` holds exactly the pixels of `expected` (color model, depth, alpha,
// palette and every value).
bool samePixels(CGImageRef expected, CGImageRef written) {
    if (CGImageGetWidth(expected) != CGImageGetWidth(written) ||
        CGImageGetHeight(expected) != CGImageGetHeight(written) ||
        CGImageGetBitsPerComponent(expected) != CGImageGetBitsPerComponent(written))
        return false;
    CGColorSpaceRef a = CGImageGetColorSpace(expected), b = CGImageGetColorSpace(written);
    if (!a || !b || !sameColorSpace(a, b))
        return false;
    const auto ca = canonical(expected), cb = canonical(written);
    if (!ca || !cb || ca->floating != cb->floating || ca->values != cb->values)
        return false;
    // Alpha values must mean the same: premultiplied vs. straight only matters with alpha.
    if (ca->alpha != cb->alpha)
        return false;
    return !ca->alpha || ca->premultiplied == cb->premultiplied;
}

// Fallback for lossy codecs (user agreed): draws into an RGB context.
CGImageRef drawnRotation(CGImageRef img, int op) {
    const auto& m = matrixOf(op);
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
    if (!ctx)
        return nullptr;
    // Pixel map p' = M p + t (y down), expressed in CG coordinates (y up).
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
    return rotated;
}

// ---- durability ----

// Flushes `fd` to the permanent storage (not only to the drive's cache).
bool fullSync(int fd) {
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return true;
    return fsync(fd) == 0; // file systems without F_FULLFSYNC (some network volumes)
}

bool fullSyncPath(const QByteArray& path, int flags = O_RDONLY) {
    const int fd = open(path.constData(), flags | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = fullSync(fd);
    close(fd);
    return ok;
}

QString folderOf(const QString& path) {
    return QFileInfo(path).absolutePath();
}

// Copies the contents of `from` into the existing file `to` (same inode), truncates it and
// flushes it. The modification date becomes `mtime`.
bool copyIntoInode(const QByteArray& from, const QByteArray& to, const struct timespec& atime,
                   const struct timespec& mtime, QString* error) {
    const int in = open(from.constData(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                    "Die neue Datei kann nicht gelesen werden."));
        return false;
    }
    const int out = open(to.constData(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (out < 0) {
        close(in);
        setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                    "Die Datei kann nicht geschrieben werden: %1")
                            .arg(QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    std::vector<char> buf(1 << 20);
    off_t offset = 0;
    bool ok = true;
    for (;;) {
        const ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0)
            break;
        ssize_t done = 0;
        while (done < n) {
            const ssize_t w = pwrite(out, buf.data() + done, size_t(n - done), offset + done);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            done += w;
        }
        if (!ok)
            break;
        offset += n;
    }
    ok = ok && ftruncate(out, offset) == 0;
    const struct timespec times[2] = {atime, mtime};
    if (ok)
        futimens(out, times);
    ok = ok && fullSync(out);
    close(out);
    close(in);
    if (!ok)
        setError(error,
                 QCoreApplication::translate("lf::LosslessTransform",
                                             "Die Datei konnte nicht vollständig geschrieben werden: %1")
                     .arg(QString::fromLocal8Bit(strerror(errno))));
    return ok;
}

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

namespace {

bool writeOrientationImpl(const QString& srcPath, const QString& dstPath, int orientation, QString* error) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(srcPath);
        if (!src) {
            setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                        "Die Datei kann nicht gelesen werden."));
            return false;
        }
        const QStringList auxBefore = auxiliaryImages(src);
        CFStringRef type = CGImageSourceGetType(src);
        CGImageDestinationRef dst =
            type ? CGImageDestinationCreateWithURL((__bridge CFURLRef)fileURL(dstPath), type,
                                                   CGImageSourceGetCount(src), nullptr)
                 : nullptr;
        if (!dst) {
            CFRelease(src);
            setError(error, QCoreApplication::translate(
                                "lf::LosslessTransform",
                                "Für dieses Format kann die Ausrichtung nicht gespeichert werden."));
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
            if (!auxBefore.isEmpty()) {
                setError(error, auxLostMessage(auxBefore));
                return false;
            }
            setError(error, detail.isEmpty()
                                ? QCoreApplication::translate(
                                      "lf::LosslessTransform",
                                      "Die Ausrichtung konnte nicht verlustfrei geschrieben werden.")
                                //: %1 is the system's error message
                                : QCoreApplication::translate(
                                      "lf::LosslessTransform",
                                      "Die Ausrichtung konnte nicht verlustfrei geschrieben werden (%1).")
                                      .arg(detail));
            return false;
        }
        if (!QFileInfo::exists(dstPath)) {
            setError(error, QCoreApplication::translate(
                                "lf::LosslessTransform",
                                "Die Ausrichtung konnte nicht verlustfrei geschrieben werden."));
            return false;
        }
        // Every auxiliary image must have survived the copy.
        if (!auxBefore.isEmpty()) {
            CGImageSourceRef written = openSource(dstPath);
            const QStringList auxAfter = written ? auxiliaryImages(written) : QStringList();
            if (written)
                CFRelease(written);
            for (qsizetype i = 0; i < auxBefore.size(); i += 2) {
                if (!auxAfter.contains(auxBefore.at(i))) {
                    QFile::remove(dstPath);
                    setError(error, auxLostMessage(auxBefore));
                    return false;
                }
            }
        }
        return true;
    }
}

bool reencodeImpl(const QString& srcPath, const QString& dstPath, int pixelOp, bool requireExact,
                  QString* error, bool* inexact) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(srcPath);
        if (!src) {
            setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                        "Die Datei kann nicht gelesen werden."));
            return false;
        }
        const size_t index = CGImageSourceGetPrimaryImageIndex(src);
        NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, index, nullptr));
        // The stored pixels as they are: no conversion, no premultiplication.
        NSDictionary* decodeOpts = @{(__bridge NSString*)kCGImageSourceShouldCache : @NO};
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, index, (__bridge CFDictionaryRef)decodeOpts);
        CFStringRef type = CGImageSourceGetType(src);
        if (!img || !type) {
            if (img)
                CGImageRelease(img);
            CFRelease(src);
            setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                        "Das Bild konnte nicht dekodiert werden."));
            return false;
        }
        CGImageRef rotated = nullptr;
        if (auto raw = rawPixels(img))
            rotated = rotatedImage(img, *raw, pixelOp);
        if (!rotated && !requireExact)
            rotated = drawnRotation(img, pixelOp);
        CGImageRelease(img);
        if (!rotated) {
            CFRelease(src);
            if (requireExact && inexact)
                *inexact = true;
            setError(error, requireExact ? QCoreApplication::translate(
                                               "lf::LosslessTransform",
                                               "Das Bild lässt sich nicht exakt neu speichern.")
                                         : QCoreApplication::translate("lf::LosslessTransform",
                                                                       "Nicht genug Speicher zum Drehen."));
            return false;
        }

        NSMutableDictionary* out = props ? [props mutableCopy] : [NSMutableDictionary dictionary];
        out[(__bridge NSString*)kCGImagePropertyOrientation] = @1;
        NSMutableDictionary* tiff = [out[(__bridge NSString*)kCGImagePropertyTIFFDictionary] mutableCopy];
        if (!tiff)
            tiff = [NSMutableDictionary dictionary];
        tiff[(__bridge NSString*)kCGImagePropertyTIFFOrientation] = @1;
        out[(__bridge NSString*)kCGImagePropertyTIFFDictionary] = tiff;
        [out removeObjectForKey:(__bridge NSString*)kCGImagePropertyPixelWidth];
        [out removeObjectForKey:(__bridge NSString*)kCGImagePropertyPixelHeight];
        if (!requireExact)
            out[(__bridge NSString*)kCGImageDestinationLossyCompressionQuality] = @0.95;
        CGImageDestinationRef dst =
            CGImageDestinationCreateWithURL((__bridge CFURLRef)fileURL(dstPath), type, 1, nullptr);
        bool ok = false;
        if (dst) {
            CGImageDestinationAddImage(dst, rotated, (__bridge CFDictionaryRef)out);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CFRelease(src);
        if (!ok) {
            CGImageRelease(rotated);
            QFile::remove(dstPath);
            setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                        "Dieses Format kann nicht geschrieben werden."));
            return false;
        }
        if (requireExact) {
            // Read back: the codec must have stored exactly the moved pixels.
            bool exact = false;
            if (CGImageSourceRef check = openSource(dstPath)) {
                if (CGImageRef back =
                        CGImageSourceCreateImageAtIndex(check, 0, (__bridge CFDictionaryRef)decodeOpts)) {
                    exact = samePixels(rotated, back);
                    CGImageRelease(back);
                }
                CFRelease(check);
            }
            if (!exact) {
                CGImageRelease(rotated);
                QFile::remove(dstPath);
                if (inexact)
                    *inexact = true;
                setError(error, QCoreApplication::translate(
                                    "lf::LosslessTransform",
                                    "Das Bild lässt sich in diesem Format nicht exakt neu speichern."));
                return false;
            }
        }
        CGImageRelease(rotated);
        return true;
    }
}

} // namespace

// Removes what a failed ImageIO write left behind: the destination and ImageIO's own
// temporary file next to it (".<name>-XXXXXX", seen with HEIC files it cannot modify). The
// destination names are unique temporary names of ours, so the pattern matches nothing else.
static void removeFailedDestination(const QString& dstPath) {
    QFile::remove(dstPath);
    const QFileInfo fi(dstPath);
    QDir dir = fi.dir();
    const QStringList leftovers =
        dir.entryList({QLatin1Char('.') + fi.fileName() + QStringLiteral("-*")}, QDir::Files | QDir::Hidden);
    for (const QString& name : leftovers)
        QFile::remove(dir.filePath(name));
}

// Cleanup runs after the autorelease pool of the ImageIO work has drained (destinations are
// finished when their last reference goes away).
bool writeOrientationLossless(const QString& srcPath, const QString& dstPath, int orientation,
                              QString* error) {
    if (writeOrientationImpl(srcPath, dstPath, orientation, error))
        return true;
    removeFailedDestination(dstPath);
    return false;
}

bool reencodeTransformed(const QString& srcPath, const QString& dstPath, int pixelOp, bool requireExact,
                         QString* error, bool* inexact) {
    if (inexact)
        *inexact = false;
    if (reencodeImpl(srcPath, dstPath, pixelOp, requireExact, error, inexact))
        return true;
    removeFailedDestination(dstPath);
    return false;
}

bool replaceFile(const QString& replacement, const QString& target, bool keepModificationDate,
                 QString* error) {
    const QByteArray from = QFile::encodeName(replacement);
    const QByteArray to = QFile::encodeName(target);
    struct stat st{};
    if (lstat(to.constData(), &st) != 0) {
        setError(error, QCoreApplication::translate("lf::LosslessTransform", "„%1“ existiert nicht mehr.")
                            .arg(QFileInfo(target).fileName()));
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        // Callers resolve symbolic links; never replace a link by a regular file.
        setError(error, QCoreApplication::translate("lf::LosslessTransform", "„%1“ ist keine normale Datei.")
                            .arg(QFileInfo(target).fileName()));
        return false;
    }
    struct stat rs{};
    if (stat(from.constData(), &rs) != 0) {
        setError(error, QCoreApplication::translate("lf::LosslessTransform", "Die neue Datei fehlt."));
        return false;
    }

    if (st.st_nlink > 1) {
        // Hard links: a rename would split them (the other names keep the old contents) and
        // change the file ID. The contents go into the existing inode instead; the complete
        // new file stays on disk until that is done.
        if (!fullSyncPath(from)) {
            setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                        "Die neue Datei konnte nicht gesichert werden."));
            return false;
        }
        reportDurabilityStep(DurabilityStep::FullSync, replacement);
        // A clone of the old contents (APFS: no extra space) to roll back a failed write
        // (e.g. disk full halfway).
        const QByteArray old = QFile::encodeName(QFileInfo(target).dir().filePath(
            QStringLiteral(".%1.lumaflip-old-%2").arg(QFileInfo(target).fileName()).arg(getpid())));
        unlink(old.constData());
        const bool haveOld =
            clonefile(to.constData(), old.constData(), 0) == 0 ||
            copyfile(to.constData(), old.constData(), nullptr, COPYFILE_DATA | COPYFILE_EXCL) == 0;
        if (!haveOld) {
            unlink(old.constData());
            setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                        "Nicht genug Platz, um „%1“ sicher zu überschreiben.")
                                .arg(QFileInfo(target).fileName()));
            return false;
        }
        // Not kept: the file changed now (the replacement may carry the original's date -
        // ImageIO's metadata copy and clones do).
        const struct timespec mtime = keepModificationDate ? st.st_mtimespec : timespec{0, UTIME_NOW};
        if (!copyIntoInode(from, to, st.st_atimespec, mtime, error)) {
            QString ignored;
            if (copyIntoInode(old, to, st.st_atimespec, st.st_mtimespec, &ignored))
                unlink(old.constData());
            else if (error)
                *error += QLatin1Char(' ') + QCoreApplication::translate("lf::LosslessTransform",
                                                                         "Der alte Inhalt liegt in „%1“.")
                                                 .arg(QFile::decodeName(old));
            return false;
        }
        unlink(old.constData());
        reportDurabilityStep(DurabilityStep::InPlaceCopy, target);
        unlink(from.constData());
        return true;
    }

    // Permissions, ACL, owner (as far as allowed) and extended attributes (Finder tags,
    // comments, quarantine) of the original.
    if (copyfile(to.constData(), from.constData(), nullptr, COPYFILE_SECURITY | COPYFILE_XATTR) != 0) {
        setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                    "Die Dateiattribute konnten nicht übernommen werden."));
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
    } else {
        // F-EDIT-02: the replacement may carry the original's modification date (ImageIO's
        // metadata copy keeps it), but the file changed now.
        const struct timespec times[2] = {{0, UTIME_OMIT}, {0, UTIME_NOW}};
        utimensat(AT_FDCWD, from.constData(), times, 0);
    }
    // The data must be on the disk before the name points to it: otherwise a power loss
    // right after the rename can leave an empty or partial file under the original name.
    if (!fullSyncPath(from)) {
        setError(error, QCoreApplication::translate("lf::LosslessTransform",
                                                    "Die neue Datei konnte nicht gesichert werden."));
        return false;
    }
    reportDurabilityStep(DurabilityStep::FullSync, replacement);
    if (rename(from.constData(), to.constData()) != 0) {
        setError(error,
                 QCoreApplication::translate("lf::LosslessTransform", "„%1“ konnte nicht ersetzt werden: %2")
                     .arg(QFileInfo(target).fileName(), QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    reportDurabilityStep(DurabilityStep::Rename, target);
    if (haveAdded) {
        // The date added belongs to the directory entry: set it after the rename.
        struct timespec added = addedBuf.added;
        setattrlist(to.constData(), &addedList, &added, sizeof(added), 0);
    }
    // The new directory entry itself.
    if (fullSyncPath(QFile::encodeName(folderOf(target))))
        reportDurabilityStep(DurabilityStep::FolderSync, folderOf(target));
    return true;
}

bool copyWithMetadata(const QString& path, const QString& copy, QString* error, bool* cloned) {
    const QByteArray from = QFile::encodeName(path);
    const QByteArray to = QFile::encodeName(copy);
    if (cloned)
        *cloned = false;
    // APFS clone (same volume): data blocks shared, metadata (xattrs, ACL, dates) copied.
    if (clonefile(from.constData(), to.constData(), 0) == 0) {
        if (cloned)
            *cloned = true;
        return true;
    }
    if (copyfile(from.constData(), to.constData(), nullptr, COPYFILE_ALL | COPYFILE_EXCL) != 0) {
        if (errno != EEXIST)
            unlink(to.constData()); // partial copy (never a file that was there before)
        setError(error, QCoreApplication::translate(
                            "lf::LosslessTransform",
                            "Sicherungskopie für „Widerrufen“ konnte nicht angelegt werden."));
        return false;
    }
    return true;
}

} // namespace lf::platform
