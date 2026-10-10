// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-08: the GPS position of an image via ImageIO's properties (no pixels decoded):
// every format ImageIO reads (JPEG, HEIC, RAW, PNG/XMP …); the portable EXIF parser for the
// rest (files ImageIO does not know).

#include "core/FolderMap.h"

#include <QFileInfo>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <cmath>

namespace lf {

namespace {

std::optional<double> number(NSDictionary* dict, CFStringRef key) {
    id v = dict[(__bridge NSString*)key];
    if ([v isKindOfClass:[NSNumber class]])
        return [static_cast<NSNumber*>(v) doubleValue];
    if ([v isKindOfClass:[NSString class]]) {
        bool ok = false;
        const double d = QString::fromNSString(static_cast<NSString*>(v)).toDouble(&ok);
        if (ok)
            return d;
    }
    return std::nullopt;
}

QString text(NSDictionary* dict, CFStringRef key) {
    id v = dict[(__bridge NSString*)key];
    return [v isKindOfClass:[NSString class]] ? QString::fromNSString(static_cast<NSString*>(v)).toUpper()
                                              : QString();
}

} // namespace

std::optional<GeoPoint> readImageLocation(const QString& path) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        NSDictionary* options = @{(__bridge NSString*)kCGImageSourceShouldCache : @NO};
        CGImageSourceRef src =
            CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)options);
        if (!src)
            return geotag::readExifGps(path);
        std::optional<GeoPoint> result;
        bool known = false;
        if (CGImageSourceGetCount(src) > 0) {
            known = true;
            const size_t index = CGImageSourceGetPrimaryImageIndex(src);
            CFDictionaryRef propsRef =
                CGImageSourceCopyPropertiesAtIndex(src, index, (__bridge CFDictionaryRef)options);
            NSDictionary* props = CFBridgingRelease(propsRef);
            id gpsValue = props[(__bridge NSString*)kCGImagePropertyGPSDictionary];
            if ([gpsValue isKindOfClass:[NSDictionary class]]) {
                NSDictionary* gps = static_cast<NSDictionary*>(gpsValue);
                const auto lat = number(gps, kCGImagePropertyGPSLatitude);
                const auto lon = number(gps, kCGImagePropertyGPSLongitude);
                if (lat && lon) {
                    GeoPoint p{text(gps, kCGImagePropertyGPSLatitudeRef) == u"S" ? -std::abs(*lat)
                                                                                 : std::abs(*lat),
                               text(gps, kCGImagePropertyGPSLongitudeRef) == u"W" ? -std::abs(*lon)
                                                                                  : std::abs(*lon)};
                    if (geo::isValid(p) && !(p.latitude == 0.0 && p.longitude == 0.0))
                        result = p;
                }
            }
        }
        CFRelease(src);
        if (!known)
            return geotag::readExifGps(path);
        return result;
    }
}

} // namespace lf
