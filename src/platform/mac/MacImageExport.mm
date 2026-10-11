// SPDX-License-Identifier: GPL-3.0-or-later
// macOS part of exporting (F-EDIT-04, core/ImageExport.h): ImageIO decodes the source (HDR
// with its gain map on request), Core Graphics draws it oriented, scaled and color converted,
// ImageIO encodes it (ISO gain map for HDR where the format supports it) with the chosen
// metadata.

#include "core/ImageExport.h"

#include "core/CropSave.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QImage>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace lf::platform {

namespace {

using exporting::ColorMode;
using exporting::Format;
using exporting::HdrMode;
using exporting::MetadataMode;
using exporting::Options;

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

NSURL* fileURL(const QString& path) {
    return [NSURL fileURLWithPath:path.toNSString()];
}

constexpr std::array<std::array<int, 4>, 9> kOrient = {{{1, 0, 0, 1},
                                                        {1, 0, 0, 1},
                                                        {-1, 0, 0, 1},
                                                        {-1, 0, 0, -1},
                                                        {1, 0, 0, -1},
                                                        {0, 1, 1, 0},
                                                        {0, -1, 1, 0},
                                                        {0, -1, -1, 0},
                                                        {0, 1, -1, 0}}};

struct Target {
    CGColorSpaceRef space = nullptr; // owned
    bool hdr = false;                // float, extended range
    bool deep = false;               // 16 bit
    bool gray = false;
    bool alpha = false;
    ~Target() {
        if (space)
            CGColorSpaceRelease(space);
    }
};

// Output color space for `img` and the option.
CGColorSpaceRef outputSpace(CGImageRef img, ColorMode mode, bool hdr, bool* gray) {
    *gray = false;
    if (hdr) {
        // Extended linear spaces keep the values above SDR white for the gain map encoder.
        return CGColorSpaceCreateWithName(mode == ColorMode::SRgb ? kCGColorSpaceExtendedLinearSRGB
                                                                  : kCGColorSpaceExtendedLinearDisplayP3);
    }
    switch (mode) {
    case ColorMode::SRgb:
        return CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    case ColorMode::DisplayP3:
        return CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
    case ColorMode::Keep:
        break;
    }
    CGColorSpaceRef cs = CGImageGetColorSpace(img);
    const CGColorSpaceModel model = cs ? CGColorSpaceGetModel(cs) : kCGColorSpaceModelUnknown;
    if (model == kCGColorSpaceModelRGB && !CGColorSpaceUsesExtendedRange(cs))
        return CGColorSpaceRetain(cs);
    if (model == kCGColorSpaceModelMonochrome) {
        *gray = true;
        return CGColorSpaceRetain(cs);
    }
    return CGColorSpaceCreateWithName(kCGColorSpaceSRGB); // CMYK, Lab, indexed …: as sRGB
}

// Draws `img` (stored pixels) with orientation `o`, scaled to `outW` x `outH` (displayed).
CGImageRef render(CGImageRef img, int o, size_t outW, size_t outH, const Target& t, bool opaqueWhite) {
    const auto& m = kOrient[size_t(o >= 1 && o <= 8 ? o : 1)];
    const double w = double(CGImageGetWidth(img)), h = double(CGImageGetHeight(img));
    const bool swap = m[0] == 0;
    const double s = double(outW) / (swap ? h : w); // displayed x scale
    const double sy = double(outH) / (swap ? w : h);
    size_t bpc = 8;
    uint32_t info = 0;
    if (t.hdr) {
        bpc = 16;
        info = uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapFloatComponents) |
               uint32_t(kCGBitmapByteOrder16Little);
    } else if (t.gray) {
        bpc = t.deep ? 16 : 8;
        info = uint32_t(kCGImageAlphaNone) |
               uint32_t(t.deep ? kCGBitmapByteOrder16Host : kCGBitmapByteOrderDefault);
    } else {
        bpc = t.deep ? 16 : 8;
        info = uint32_t(t.alpha ? kCGImageAlphaPremultipliedLast : kCGImageAlphaNoneSkipLast) |
               uint32_t(t.deep ? kCGBitmapByteOrder16Host : kCGBitmapByteOrderDefault);
    }
    CGContextRef ctx = CGBitmapContextCreate(nullptr, outW, outH, bpc, 0, t.space, info);
    if (!ctx)
        return nullptr;
    if (opaqueWhite) {
        CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
        CGContextFillRect(ctx, CGRectMake(0, 0, double(outW), double(outH)));
    }
    // Pixel map (y down) p' = S M p + t, expressed in CG coordinates (y up); see
    // drawnRotation() in MacImageTransform.mm (here with the scale factors).
    double minX = 0, minY = 0;
    for (const auto& c : {std::array<double, 2>{0, 0}, {w, 0}, {0, h}, {w, h}}) {
        minX = std::min(minX, m[0] * c[0] + m[1] * c[1]);
        minY = std::min(minY, m[2] * c[0] + m[3] * c[1]);
    }
    const double tx = -minX * s, ty = -minY * sy;
    const CGAffineTransform xf = CGAffineTransformMake(s * m[0], -sy * m[2], -s * m[1], sy * m[3],
                                                       s * m[1] * h + tx, double(outH) - sy * m[3] * h - ty);
    CGContextConcatCTM(ctx, xf);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    CGImageRef out = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    return out;
}

bool hasAlpha(CGImageRef img) {
    const CGImageAlphaInfo ai = CGImageGetAlphaInfo(img);
    return ai != kCGImageAlphaNone && ai != kCGImageAlphaNoneSkipLast && ai != kCGImageAlphaNoneSkipFirst;
}

// Metadata for the output; nullptr = none. Orientation is baked into the pixels.
CGMutableImageMetadataRef outputMetadata(CGImageMetadataRef meta, MetadataMode mode, size_t w, size_t h) {
    if (mode == MetadataMode::RemoveAll || !meta)
        return nullptr;
    CGMutableImageMetadataRef m = CGImageMetadataCreateMutableCopy(meta);
    if (!m)
        return nullptr;
    CGImageMetadataSetValueMatchingImageProperty(m, kCGImagePropertyTIFFDictionary,
                                                 kCGImagePropertyTIFFOrientation, (__bridge CFTypeRef) @1);
    CGImageMetadataSetValueMatchingImageProperty(
        m, kCGImagePropertyExifDictionary, kCGImagePropertyExifPixelXDimension, (__bridge CFTypeRef) @(w));
    CGImageMetadataSetValueMatchingImageProperty(
        m, kCGImagePropertyExifDictionary, kCGImagePropertyExifPixelYDimension, (__bridge CFTypeRef) @(h));
    if (mode == MetadataMode::RemoveGps) {
        NSMutableArray<NSString*>* gps = [NSMutableArray array];
        CGImageMetadataEnumerateTagsUsingBlock(
            m, nullptr, nullptr, ^bool(CFStringRef path, CGImageMetadataTagRef tag) {
              NSString* name = CFBridgingRelease(CGImageMetadataTagCopyName(tag));
              if ([name hasPrefix:@"GPS"])
                  [gps addObject:(__bridge NSString*)path];
              return true;
            });
        for (NSString* path in gps)
            CGImageMetadataRemoveTagWithPath(m, nullptr, (__bridge CFStringRef)path);
    }
    return m;
}

bool write(CGImageRef image, CGImageMetadataRef meta, const QString& dst, const Format& f, const Options& o,
           bool gainMap, QString* error) {
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)fileURL(dst), (__bridge CFStringRef)f.typeId.toNSString(), 1, nullptr);
    if (!d) {
        setError(error,
                 QCoreApplication::translate("lf::ImageExport",
                                             "Dieses Format kann auf diesem Mac nicht geschrieben werden."));
        return false;
    }
    NSMutableDictionary* opts = [NSMutableDictionary dictionary];
    opts[(__bridge NSString*)kCGImagePropertyOrientation] = @1;
    if (f.lossy)
        opts[(__bridge NSString*)kCGImageDestinationLossyCompressionQuality] =
            @(std::clamp(o.quality, 1, 100) / 100.0);
    if (gainMap)
        opts[(__bridge NSString*)kCGImageDestinationEncodeRequest] =
            (__bridge NSString*)kCGImageDestinationEncodeToISOGainmap;
    if (o.metadata == MetadataMode::RemoveGps)
        opts[(__bridge NSString*)kCGImageMetadataShouldExcludeGPS] = @YES;
    CGImageDestinationAddImageAndMetadata(d, image, meta, (__bridge CFDictionaryRef)opts);
    const bool ok = CGImageDestinationFinalize(d);
    CFRelease(d);
    if (!ok) {
        QFile::remove(dst);
        setError(error,
                 QCoreApplication::translate("lf::ImageExport", "Das Bild konnte nicht geschrieben werden."));
    }
    return ok;
}

// Size after orientation and the long-edge limit (never enlarged).
void outputSize(size_t w, size_t h, int o, int maxLongEdge, size_t* outW, size_t* outH) {
    const bool swap = o >= 5 && o <= 8;
    double W = double(swap ? h : w), H = double(swap ? w : h);
    const double longEdge = std::max(W, H);
    if (maxLongEdge > 0 && longEdge > maxLongEdge) {
        const double s = maxLongEdge / longEdge;
        W = std::max(1.0, std::round(W * s));
        H = std::max(1.0, std::round(H * s));
    }
    *outW = size_t(W);
    *outH = size_t(H);
}

// `img` limited to the crop of `o` (normalized to the displayed image, F-EDIT-03), in stored
// pixels; a new reference (the same image without a crop).
CGImageRef croppedSource(CGImageRef img, int orientation, const Options& o) {
    const QSize stored(int(CGImageGetWidth(img)), int(CGImageGetHeight(img)));
    const QRect all(QPoint(0, 0), stored);
    const QSize displayed = crop::storedToDisplayRect(all, stored, orientation).size();
    const QRect shown = exporting::cropPixels(o.crop, displayed);
    if (shown == QRect(QPoint(0, 0), displayed))
        return CGImageRetain(img);
    const QRect r = crop::displayToStoredRect(shown, stored, orientation);
    return CGImageCreateWithImageInRect(img, CGRectMake(r.x(), r.y(), r.width(), r.height()));
}

bool exportCGImage(CGImageRef source, int orientation, CGImageMetadataRef meta, bool hdr, const QString& dst,
                   const Options& o, QString* error) {
    const Format f = exporting::formatById(o.formatId);
    if (f.id.isEmpty()) {
        setError(error, QCoreApplication::translate("lf::ImageExport", "Unbekanntes Zielformat."));
        return false;
    }
    const float sourceHeadroom = hdr ? CGImageGetContentHeadroom(source) : 1.0f;
    CGImageRef img = croppedSource(source, orientation, o);
    if (!img) {
        setError(error,
                 QCoreApplication::translate("lf::ImageExport", "Nicht genug Speicher zum Exportieren."));
        return false;
    }
    struct Release {
        CGImageRef ref;
        ~Release() { CGImageRelease(ref); }
    } releaseCropped{img};
    size_t w = 0, h = 0;
    outputSize(CGImageGetWidth(img), CGImageGetHeight(img), orientation, o.maxLongEdge, &w, &h);
    Target t;
    t.hdr = hdr;
    t.space = outputSpace(img, o.color, hdr, &t.gray);
    t.deep = !hdr && CGImageGetBitsPerComponent(img) > 8 && f.id != QLatin1String("jpeg");
    t.alpha = hasAlpha(img) && f.id != QLatin1String("jpeg");
    if (t.gray && hasAlpha(img) && t.alpha) {
        // Gray with alpha has no bitmap context: as sRGB.
        CGColorSpaceRelease(t.space);
        t.space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        t.gray = false;
    }
    const bool white = hasAlpha(img) && !t.alpha; // JPEG: transparency on white
    CGImageRef out = render(img, orientation, w, h, t, white);
    if (!out) {
        setError(error,
                 QCoreApplication::translate("lf::ImageExport", "Nicht genug Speicher zum Exportieren."));
        return false;
    }
    if (hdr) {
        const float headroom = std::max(sourceHeadroom, CGImageGetContentHeadroom(img));
        if (headroom > 1.0f) {
            if (CGImageRef withHeadroom = CGImageCreateCopyWithContentHeadroom(headroom, out)) {
                CGImageRelease(out);
                out = withHeadroom;
            }
        }
    }
    CGMutableImageMetadataRef m = outputMetadata(meta, o.metadata, w, h);
    const bool ok = write(out, m, dst, f, o, hdr, error);
    if (m)
        CFRelease(m);
    CGImageRelease(out);
    return ok;
}

bool sourceIsHdr(CGImageSourceRef src, size_t index, CGImageRef sdr) {
    for (CFStringRef type : {kCGImageAuxiliaryDataTypeISOGainMap, kCGImageAuxiliaryDataTypeHDRGainMap}) {
        if (CFDictionaryRef info = CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, index, type)) {
            CFRelease(info);
            return true;
        }
    }
    CGColorSpaceRef cs = sdr ? CGImageGetColorSpace(sdr) : nullptr;
    return cs && (CGColorSpaceUsesITUR_2100TF(cs) || CGColorSpaceUsesExtendedRange(cs));
}

} // namespace

QStringList exportTypeIdentifiers() {
    @autoreleasepool {
        NSArray* types = CFBridgingRelease(CGImageDestinationCopyTypeIdentifiers());
        QStringList out;
        for (id t in types)
            if ([t isKindOfClass:[NSString class]])
                out << QString::fromNSString(t);
        return out;
    }
}

bool exportImage(const QString& source, const QString& dst, const Options& o, QString* error,
                 QString* notice) {
    @autoreleasepool {
        NSDictionary* openOpts = @{(__bridge NSString*)kCGImageSourceShouldCache : @NO};
        CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)fileURL(source),
                                                          (__bridge CFDictionaryRef)openOpts);
        if (!src) {
            setError(error,
                     QCoreApplication::translate("lf::ImageExport", "Die Datei kann nicht gelesen werden."));
            return false;
        }
        const size_t index = CGImageSourceGetPrimaryImageIndex(src);
        NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, index, nullptr));
        CGImageMetadataRef meta = CGImageSourceCopyMetadataAtIndex(src, index, nullptr);
        const int orientation = [props[(__bridge NSString*)kCGImagePropertyOrientation] intValue];
        const Format f = exporting::formatById(o.formatId);
        CGImageRef img = CGImageSourceCreateImageAtIndex(src, index, (__bridge CFDictionaryRef)openOpts);
        bool hdr = false;
        if (img && sourceIsHdr(src, index, img)) {
            if (o.hdr == HdrMode::Keep && f.gainMap) {
                NSDictionary* hdrOpts = @{
                    (__bridge NSString*)kCGImageSourceShouldCache : @NO,
                    (__bridge NSString*)
                    kCGImageSourceDecodeRequest : (__bridge NSString*)kCGImageSourceDecodeToHDR
                };
                if (CGImageRef h =
                        CGImageSourceCreateImageAtIndex(src, index, (__bridge CFDictionaryRef)hdrOpts)) {
                    CGImageRelease(img);
                    img = h;
                    hdr = true;
                }
            } else if (o.hdr == HdrMode::Keep && notice) {
                *notice = QCoreApplication::translate(
                              "lf::ImageExport",
                              "%1 kann keine HDR-Gain-Map speichern – das Bild wurde als SDR exportiert.")
                              .arg(f.label);
            }
        }
        if (!img) {
            if (meta)
                CFRelease(meta);
            CFRelease(src);
            setError(error, QCoreApplication::translate("lf::ImageExport",
                                                        "Das Bild konnte nicht dekodiert werden."));
            return false;
        }
        const bool ok = exportCGImage(img, orientation, meta, hdr, dst, o, error);
        CGImageRelease(img);
        if (meta)
            CFRelease(meta);
        CFRelease(src);
        return ok;
    }
}

bool exportDecoded(const QImage& image, const QString& dst, const Options& o, QString* error,
                   QString* notice) {
    @autoreleasepool {
        // Pixels of archive members and PDF pages: as 8-bit sRGB (they carry no metadata).
        // Never silently: say when HDR values or a wider gamut are lost on the way.
        const bool deep = image.depth() > 32;
        const QColorSpace sourceSpace = image.colorSpace();
        const bool wide = sourceSpace.isValid() && sourceSpace.primaries() != QColorSpace::Primaries::SRgb;
        if (notice && (deep || wide) && o.color != ColorMode::SRgb)
            *notice = QCoreApplication::translate(
                "lf::ImageExport", "Bilder aus Archiven und PDF-Dokumenten werden als 8-Bit-sRGB "
                                   "exportiert – HDR und erweiterter Farbraum gehen verloren.");
        QImage rgba = image.copy(exporting::cropPixels(o.crop, image.size()));
        if (rgba.colorSpace().isValid() && rgba.colorSpace() != QColorSpace(QColorSpace::SRgb))
            rgba = rgba.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), QImage::Format_RGBA8888);
        rgba = rgba.convertToFormat(QImage::Format_RGBA8888);
        if (rgba.isNull()) {
            setError(error, QCoreApplication::translate("lf::ImageExport",
                                                        "Das Bild konnte nicht dekodiert werden."));
            return false;
        }
        NSData* data = [NSData dataWithBytes:rgba.constBits() length:size_t(rgba.sizeInBytes())];
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef img =
            CGImageCreate(size_t(rgba.width()), size_t(rgba.height()), 8, 32, size_t(rgba.bytesPerLine()), cs,
                          CGBitmapInfo(uint32_t(kCGImageAlphaLast) | uint32_t(kCGBitmapByteOrderDefault)),
                          provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        if (!img) {
            setError(error,
                     QCoreApplication::translate("lf::ImageExport", "Nicht genug Speicher zum Exportieren."));
            return false;
        }
        const bool ok = exportCGImage(img, 1, nullptr, false, dst, o, error);
        CGImageRelease(img);
        return ok;
    }
}

} // namespace lf::platform
