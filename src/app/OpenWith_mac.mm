// SPDX-License-Identifier: GPL-3.0-or-later
// F-FILE-05: LaunchServices queries (worker threads) - apps for a file, app names/icons.

#include "MacImage.h"
#include "OpenWith.h"

#include <QDir>

#import <AppKit/AppKit.h>

namespace lf {

namespace {

constexpr int kIconPt = 16;
constexpr int kIconScale = 2; // Retina

AppEntry entryFor(NSURL* appUrl) {
    AppEntry e;
    e.path = QString::fromNSString(appUrl.path);
    NSString* name = [[NSFileManager defaultManager] displayNameAtPath:appUrl.path];
    if ([name.pathExtension.lowercaseString isEqualToString:@"app"])
        name = name.stringByDeletingPathExtension;
    e.name = QString::fromNSString(name);
    e.bundleId = QString::fromNSString([NSBundle bundleWithURL:appUrl].bundleIdentifier);
    e.icon = mac::toQImage([[NSWorkspace sharedWorkspace] iconForFile:appUrl.path], kIconPt * kIconScale,
                           kIconScale);
    return e;
}

} // namespace

namespace mac {
QImage toQImage(NSImage* image, int px, qreal devicePixelRatio) {
    if (!image || px <= 0)
        return {};
    QImage out(px, px, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx =
        CGBitmapContextCreate(out.bits(), size_t(px), size_t(px), 8, size_t(out.bytesPerLine()), cs,
                              uint32_t(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Host);
    CGColorSpaceRelease(cs);
    if (!ctx)
        return {};
    NSGraphicsContext* g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:g];
    [image drawInRect:NSMakeRect(0, 0, px, px)
             fromRect:NSZeroRect
            operation:NSCompositingOperationSourceOver
             fraction:1.0];
    [NSGraphicsContext restoreGraphicsState];
    CGContextRelease(ctx);
    out.setDevicePixelRatio(devicePixelRatio);
    return out;
}
} // namespace mac

QList<AppEntry> queryApplicationsForFile(const QString& filePath) {
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:filePath.toNSString()];
        NSWorkspace* ws = [NSWorkspace sharedWorkspace];
        NSURL* def = [ws URLForApplicationToOpenURL:url];
        NSArray<NSURL*>* all = [ws URLsForApplicationsToOpenURL:url];
        QList<AppEntry> out;
        QStringList seen;
        auto add = [&](NSURL* u, bool isDefault) {
            if (!u)
                return;
            const QString p = QDir::cleanPath(QString::fromNSString(u.path));
            if (seen.contains(p))
                return;
            seen << p;
            AppEntry e = entryFor(u);
            e.isDefault = isDefault;
            out << e;
        };
        add(def, true);
        QList<AppEntry> rest;
        for (NSURL* u in all) {
            const qsizetype before = out.size();
            add(u, false);
            if (out.size() > before)
                rest << out.takeLast();
        }
        std::sort(rest.begin(), rest.end(), [](const AppEntry& a, const AppEntry& b) {
            return [a.name.toNSString() localizedStandardCompare:b.name.toNSString()] == NSOrderedAscending;
        });
        out += rest;
        return out;
    }
}

AppEntry applicationInfo(const QString& appPath) {
    @autoreleasepool {
        if (appPath.isEmpty())
            return {};
        return entryFor([NSURL fileURLWithPath:appPath.toNSString()]);
    }
}

QString ownBundlePath() {
    @autoreleasepool {
        NSURL* bundle = [[NSBundle mainBundle] bundleURL];
        if (![bundle.pathExtension isEqualToString:@"app"])
            return {};
        return QString::fromNSString(bundle.path);
    }
}

QString ownBundleId() {
    @autoreleasepool {
        return QString::fromNSString([NSBundle mainBundle].bundleIdentifier);
    }
}

} // namespace lf
