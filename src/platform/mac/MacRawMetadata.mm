// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-05: all metadata ImageIO reads from a file - the property dictionaries of the
// primary image (Exif, ExifAux, TIFF, GPS, IPTC, maker notes such as {MakerCanon}, …) and
// the XMP tree (CGImageMetadata) - as a MetadataTree.

#include "core/MetadataTree.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QVariant>

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

namespace lf {

namespace {

QVariant toVariant(id value, int depth = 0) {
    if (depth > 32 || !value)
        return {};
    if ([value isKindOfClass:[NSString class]])
        return QString::fromNSString(static_cast<NSString*>(value));
    if ([value isKindOfClass:[NSNumber class]]) {
        NSNumber* n = static_cast<NSNumber*>(value);
        const char* t = n.objCType;
        if (t && (t[0] == 'f' || t[0] == 'd'))
            return n.doubleValue;
        if (CFGetTypeID((__bridge CFTypeRef)n) == CFBooleanGetTypeID())
            return bool(n.boolValue);
        return qlonglong(n.longLongValue);
    }
    if ([value isKindOfClass:[NSData class]]) {
        NSData* d = static_cast<NSData*>(value);
        return QByteArray::fromNSData(d);
    }
    if ([value isKindOfClass:[NSArray class]]) {
        QVariantList l;
        for (id v in static_cast<NSArray*>(value))
            l << toVariant(v, depth + 1);
        return l;
    }
    if ([value isKindOfClass:[NSDictionary class]]) {
        QVariantMap m;
        NSDictionary* d = static_cast<NSDictionary*>(value);
        for (id k in d) {
            NSString* key = [k isKindOfClass:[NSString class]] ? k : [k description];
            m.insert(QString::fromNSString(key), toVariant(d[k], depth + 1));
        }
        return m;
    }
    return QString::fromNSString([value description]);
}

// Group name of an ImageIO property dictionary: "{Exif}" -> "Exif".
QString groupName(NSString* key) {
    QString k = QString::fromNSString(key);
    if (k.startsWith(u'{') && k.endsWith(u'}'))
        k = k.mid(1, k.size() - 2);
    return k;
}

QVariant xmpValue(CGImageMetadataTagRef tag, int depth);

QVariant xmpTagValue(CFTypeRef value, int depth) {
    if (!value || depth > 32)
        return {};
    if (CFGetTypeID(value) == CFArrayGetTypeID()) {
        QVariantList l;
        NSArray* a = (__bridge NSArray*)value;
        for (id v in a) {
            if (CFGetTypeID((__bridge CFTypeRef)v) == CGImageMetadataTagGetTypeID())
                l << xmpValue((__bridge CGImageMetadataTagRef)v, depth + 1);
            else
                l << toVariant(v, depth + 1);
        }
        return l;
    }
    if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        QVariantMap m;
        NSDictionary* d = (__bridge NSDictionary*)value;
        for (id k in d) {
            id v = d[k];
            const QString key = QString::fromNSString([k description]);
            if (CFGetTypeID((__bridge CFTypeRef)v) == CGImageMetadataTagGetTypeID())
                m.insert(key, xmpValue((__bridge CGImageMetadataTagRef)v, depth + 1));
            else
                m.insert(key, toVariant(v, depth + 1));
        }
        return m;
    }
    return toVariant((__bridge id)value, depth + 1);
}

QVariant xmpValue(CGImageMetadataTagRef tag, int depth) {
    CFTypeRef v = CGImageMetadataTagCopyValue(tag);
    QVariant out = xmpTagValue(v, depth);
    if (v)
        CFRelease(v);
    return out;
}

QString xmpKey(CGImageMetadataTagRef tag) {
    NSString* prefix = CFBridgingRelease(CGImageMetadataTagCopyPrefix(tag));
    NSString* name = CFBridgingRelease(CGImageMetadataTagCopyName(tag));
    const QString n = QString::fromNSString(name ? name : @"?");
    return prefix.length ? QString::fromNSString(prefix) + u':' + n : n;
}

} // namespace

MetadataNode readRawMetadata(const QString& path) {
    MetadataNode root;
    root.key = QFileInfo(path).fileName();
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
        NSDictionary* opts = @{(id)kCGImageSourceShouldCache : @NO};
        CGImageSourceRef src =
            CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)opts);
        if (!src)
            return root;
        const size_t primary = CGImageSourceGetPrimaryImageIndex(src);
        NSDictionary* props = CFBridgingRelease(
            CGImageSourceCopyPropertiesAtIndex(src, primary, (__bridge CFDictionaryRef)opts));
        QVariantMap general;
        QList<MetadataNode> groups;
        NSArray* keys = [props.allKeys sortedArrayUsingSelector:@selector(compare:)];
        for (NSString* key in keys) {
            id value = props[key];
            if ([value isKindOfClass:[NSDictionary class]])
                groups.append(metadatatree::fromVariant(groupName(key), toVariant(value)));
            else
                general.insert(QString::fromNSString(key), toVariant(value));
        }
        if (!general.isEmpty())
            root.children.append(metadatatree::fromVariant(
                QCoreApplication::translate("lf::MetadataTree", "Bild (allgemein)"), general));
        root.children.append(groups);

        // XMP (incl. what ImageIO maps from EXIF/IPTC into XMP namespaces).
        CGImageMetadataRef xmp = CGImageSourceCopyMetadataAtIndex(src, primary, nullptr);
        if (xmp) {
            NSArray* tags = CFBridgingRelease(CGImageMetadataCopyTags(xmp));
            QVariantMap byNamespace;
            for (id t in tags) {
                auto tag = (__bridge CGImageMetadataTagRef)t;
                NSString* prefix = CFBridgingRelease(CGImageMetadataTagCopyPrefix(tag));
                const QString ns = prefix.length ? QString::fromNSString(prefix) : QStringLiteral("xmp");
                QVariantMap group = byNamespace.value(ns).toMap();
                group.insert(xmpKey(tag), xmpValue(tag, 0));
                byNamespace.insert(ns, group);
            }
            if (!byNamespace.isEmpty())
                root.children.append(metadatatree::fromVariant(QStringLiteral("XMP"), byNamespace));
            CFRelease(xmp);
        }
        CFRelease(src);
    }
    return root;
}

} // namespace lf
