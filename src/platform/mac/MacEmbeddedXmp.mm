// SPDX-License-Identifier: GPL-3.0-or-later
// Ratings embedded as XMP (F-CULL-02 b) with ImageIO: CGImageDestinationCopyImageSource
// copies the compressed image data unchanged and merges the given metadata
// (kCGImageDestinationMergeMetadata; properties set to kCFNull are removed). PNG files get
// their iTXt XMP chunk written directly (PngXmp.h). Reading
// enumerates the XMP tags by namespace, so the Lumaflip namespace is found whatever
// prefix the file uses.

#include "core/PngXmp.h"
#include "core/RatingStore.h"
#include "core/Xmp.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

namespace lf::platform {

namespace {

// Tr::tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::RatingStore)
};

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

NSURL* fileURL(const QString& path) {
    return [NSURL fileURLWithPath:path.toNSString()];
}

CGImageSourceRef openSource(const QString& path) {
    NSDictionary* opts = @{(__bridge NSString*)kCGImageSourceShouldCache : @NO};
    return CGImageSourceCreateWithURL((__bridge CFURLRef)fileURL(path), (__bridge CFDictionaryRef)opts);
}

// Auxiliary images (gain maps, depth, mattes) of the primary image that a copy must keep.
QStringList auxiliaryTypes(CGImageSourceRef src) {
    const CFStringRef types[] = {
        kCGImageAuxiliaryDataTypeHDRGainMap,
        kCGImageAuxiliaryDataTypeISOGainMap,
        kCGImageAuxiliaryDataTypeDepth,
        kCGImageAuxiliaryDataTypeDisparity,
        kCGImageAuxiliaryDataTypePortraitEffectsMatte,
        kCGImageAuxiliaryDataTypeSemanticSegmentationSkinMatte,
        kCGImageAuxiliaryDataTypeSemanticSegmentationHairMatte,
        kCGImageAuxiliaryDataTypeSemanticSegmentationTeethMatte,
        kCGImageAuxiliaryDataTypeSemanticSegmentationGlassesMatte,
        kCGImageAuxiliaryDataTypeSemanticSegmentationSkyMatte,
    };
    QStringList found;
    const size_t index = CGImageSourceGetPrimaryImageIndex(src);
    for (CFStringRef t : types) {
        if (CFDictionaryRef info = CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, index, t)) {
            CFRelease(info);
            found << QString::fromNSString((__bridge NSString*)t);
        }
    }
    return found;
}

QString stringOf(CFTypeRef value) {
    if (!value)
        return {};
    if (CFGetTypeID(value) == CFStringGetTypeID())
        return QString::fromNSString((__bridge NSString*)value);
    if (CFGetTypeID(value) == CFNumberGetTypeID())
        return QString::fromNSString([(__bridge NSNumber*)value stringValue]);
    return {};
}

// PNG: ImageIO's metadata copy does not write PNG XMP so that it reads back; the iTXt chunk
// "XML:com.adobe.xmp" is written directly instead (PngXmp.h: every other chunk byte for
// byte, the pixels untouched).
bool writePng(const QString& srcPath, const QString& dstPath, const Rating& rating, QString* error) {
    QFile in(srcPath);
    if (!in.open(QIODevice::ReadOnly) || in.size() > (qint64(1) << 31)) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return false;
    }
    const QByteArray data = in.readAll();
    in.close();
    const std::optional<QString> packet = pngxmp::read(data);
    if (!packet) {
        setError(error, Tr::tr("In dieses Format kann die Bewertung nicht sicher geschrieben werden."));
        return false;
    }
    const std::optional<QString> written = xmp::writeRating(*packet, rating);
    if (!written) {
        setError(error, Tr::tr("Das XMP im Bild ist beschädigt und wird nicht verändert."));
        return false;
    }
    const std::optional<QByteArray> out = pngxmp::withPacket(data, xmp::wrapPacket(*written));
    QFile f(dstPath);
    if (!out || !f.open(QIODevice::WriteOnly | QIODevice::NewOnly) || f.write(*out) != out->size() ||
        !f.flush()) {
        setError(error, Tr::tr("Die Bewertung ließ sich nicht ins Bild schreiben."));
        return false;
    }
    return true;
}

bool writeImpl(const QString& srcPath, const QString& dstPath, const Rating& rating, QString* error) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(srcPath);
        if (!src) {
            setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
            return false;
        }
        const QStringList auxBefore = auxiliaryTypes(src);
        CFStringRef type = CGImageSourceGetType(src);
        if (type && CFStringCompare(type, CFSTR("public.png"), 0) == kCFCompareEqualTo) {
            CFRelease(src);
            return writePng(srcPath, dstPath, rating, error);
        }
        CGImageDestinationRef dst =
            type ? CGImageDestinationCreateWithURL((__bridge CFURLRef)fileURL(dstPath), type,
                                                   CGImageSourceGetCount(src), nullptr)
                 : nullptr;
        if (!dst) {
            CFRelease(src);
            setError(error, Tr::tr("In dieses Format kann die Bewertung nicht sicher geschrieben werden."));
            return false;
        }
        CGMutableImageMetadataRef meta = CGImageMetadataCreateMutable();
        CGImageMetadataRegisterNamespaceForPrefix(meta, (__bridge CFStringRef) @(xmp::kNsLumaflip),
                                                  (__bridge CFStringRef) @(xmp::kPrefixLumaflip), nullptr);
        const xmp::RatingValues v = xmp::ratingValues(rating);
        // Removing a property through the merge (kCFNull) is not honored for every format
        // (PNG keeps it): absent properties are written as "0" instead, which reads back
        // as "unrated" / "no flag" (xmp::ratingFromValues).
        auto set = [meta](NSString* path, const std::optional<QString>& value) {
            NSString* text = value ? value->toNSString() : @"0";
            CGImageMetadataSetValueWithPath(meta, nullptr, (__bridge CFStringRef)path,
                                            (__bridge CFStringRef)text);
        };
        set(@"xmp:Rating", v.rating);
        const QString lf = QString::fromLatin1(xmp::kPrefixLumaflip);
        set((lf + QStringLiteral(":Pick")).toNSString(), v.pick);
        set((lf + QStringLiteral(":Stars")).toNSString(), v.stars);
        NSDictionary* opts = @{
            (__bridge NSString*)kCGImageDestinationMetadata : (__bridge id)meta,
            (__bridge NSString*)kCGImageDestinationMergeMetadata : @YES,
        };
        CFErrorRef err = nullptr;
        const bool ok = CGImageDestinationCopyImageSource(dst, src, (__bridge CFDictionaryRef)opts, &err);
        CFRelease(meta);
        CFRelease(dst);
        CFRelease(src);
        if (!ok) {
            NSError* e = err ? CFBridgingRelease(err) : nil;
            setError(error,
                     Tr::tr("Die Bewertung ließ sich nicht verlustfrei ins Bild schreiben.") +
                         (e ? QStringLiteral(" (%1)").arg(QString::fromNSString(e.localizedDescription))
                            : QString()));
            return false;
        }
        if (!auxBefore.isEmpty()) {
            CGImageSourceRef written = openSource(dstPath);
            const QStringList auxAfter = written ? auxiliaryTypes(written) : QStringList();
            if (written)
                CFRelease(written);
            for (const QString& t : auxBefore) {
                if (!auxAfter.contains(t)) {
                    setError(error, Tr::tr("Zusatzbilder der Datei (z. B. eine HDR-Gain-Map) gingen beim "
                                           "Schreiben verloren."));
                    return false;
                }
            }
        }
        return QFileInfo::exists(dstPath);
    }
}

} // namespace

bool canEmbedRatingType(const QString& typeId) {
    static const QStringList allowed = {QStringLiteral("public.jpeg"), QStringLiteral("public.heic"),
                                        QStringLiteral("public.heif"), QStringLiteral("public.tiff"),
                                        QStringLiteral("public.png"),  QStringLiteral("com.adobe.raw-image")};
    if (!allowed.contains(typeId))
        return false;
    @autoreleasepool {
        NSArray* writable = CFBridgingRelease(CGImageDestinationCopyTypeIdentifiers());
        return [writable containsObject:typeId.toNSString()];
    }
}

std::optional<Rating> readEmbeddedRating(const QString& path) {
    @autoreleasepool {
        CGImageSourceRef src = openSource(path);
        if (!src)
            return std::nullopt;
        if (CGImageSourceGetCount(src) == 0) {
            CFRelease(src);
            return std::nullopt;
        }
        CGImageMetadataRef meta =
            CGImageSourceCopyMetadataAtIndex(src, CGImageSourceGetPrimaryImageIndex(src), nullptr);
        CFRelease(src);
        if (!meta)
            return std::nullopt;
        __block xmp::RatingValues v;
        NSString* nsXmp = @(xmp::kNsXmp);
        NSString* nsLf = @(xmp::kNsLumaflip);
        CGImageMetadataEnumerateTagsUsingBlock(
            meta, nullptr, nullptr, ^bool(CFStringRef, CGImageMetadataTagRef tag) {
              NSString* ns = CFBridgingRelease(CGImageMetadataTagCopyNamespace(tag));
              NSString* name = CFBridgingRelease(CGImageMetadataTagCopyName(tag));
              CFTypeRef value = CGImageMetadataTagCopyValue(tag);
              const QString text = stringOf(value);
              if (value)
                  CFRelease(value);
              if ([ns isEqualToString:nsXmp] && [name isEqualToString:@"Rating"])
                  v.rating = text;
              else if ([ns isEqualToString:nsLf] && [name isEqualToString:@"Pick"])
                  v.pick = text;
              else if ([ns isEqualToString:nsLf] && [name isEqualToString:@"Stars"])
                  v.stars = text;
              return true;
            });
        CFRelease(meta);
        return xmp::ratingFromValues(v);
    }
}

bool writeEmbeddedRating(const QString& src, const QString& dst, const Rating& rating, QString* error) {
    if (writeImpl(src, dst, rating, error))
        return true;
    // What a failed ImageIO write leaves: the destination and its own temporary file next
    // to it (".<name>-XXXXXX"); the destination is a unique temporary name of ours.
    QFile::remove(dst);
    const QFileInfo fi(dst);
    QDir dir = fi.dir();
    for (const QString& name :
         dir.entryList({QLatin1Char('.') + fi.fileName() + QStringLiteral("-*")}, QDir::Files | QDir::Hidden))
        QFile::remove(dir.filePath(name));
    return false;
}

} // namespace lf::platform
