// SPDX-License-Identifier: GPL-3.0-or-later
#include "SystemSymbols.h"

#include <QPainter>
#include <QPixmap>

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cmath>

namespace lf {

QImage systemSymbolMask(const QString& name, int pixelSize) {
    if (pixelSize <= 0)
        return {};
    @autoreleasepool {
        NSImage* symbol = [NSImage imageWithSystemSymbolName:name.toNSString() accessibilityDescription:nil];
        if (!symbol)
            return {};
        // Point size so that the symbol (usually a bit wider than tall) fits the square.
        NSImageSymbolConfiguration* config =
            [NSImageSymbolConfiguration configurationWithPointSize:pixelSize * 0.62
                                                            weight:NSFontWeightRegular];
        if (NSImage* configured = [symbol imageWithSymbolConfiguration:config])
            symbol = configured;
        const NSSize s = symbol.size;
        if (s.width <= 0 || s.height <= 0)
            return {};
        const double scale = 0.88 * std::min(pixelSize / s.width, pixelSize / s.height); // small margin
        const double w = s.width * scale, h = s.height * scale;
        QImage img(pixelSize, pixelSize, QImage::Format_RGBA8888_Premultiplied);
        img.fill(Qt::transparent);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx =
            CGBitmapContextCreate(img.bits(), size_t(pixelSize), size_t(pixelSize), 8,
                                  size_t(img.bytesPerLine()), cs, uint32_t(kCGImageAlphaPremultipliedLast));
        CGColorSpaceRelease(cs);
        if (!ctx)
            return {};
        NSGraphicsContext* g = [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = g;
        [symbol drawInRect:NSMakeRect((pixelSize - w) / 2, (pixelSize - h) / 2, w, h)
                  fromRect:NSZeroRect
                 operation:NSCompositingOperationSourceOver
                  fraction:1.0];
        [NSGraphicsContext restoreGraphicsState];
        CGContextRelease(ctx);
        return img;
    }
}

QIcon systemSymbolIcon(const QString& name, int size, const QColor& color) {
    QIcon icon;
    for (int scale : {1, 2}) {
        QImage img = systemSymbolMask(name, size * scale);
        if (img.isNull())
            return {};
        // Template image: only the shape counts, the color comes from the palette.
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), color);
        p.end();
        img.setDevicePixelRatio(scale);
        icon.addPixmap(QPixmap::fromImage(img));
    }
    return icon;
}

} // namespace lf
