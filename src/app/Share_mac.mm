// SPDX-License-Identifier: GPL-3.0-or-later
// F-FILE-06: NSSharingServicePicker and the sharing services for the File menu.

#include "MacImage.h"
#include "Share.h"

#include <QFileInfo>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace lf {

namespace {

NSArray* nsItems(const QStringList& paths) {
    NSMutableArray* items = [NSMutableArray array];
    for (const QUrl& u : shareItems(paths))
        [items addObject:[NSURL fileURLWithPath:u.toLocalFile().toNSString()]];
    return items;
}

// +sharingServicesForItems: is deprecated since macOS 13 in favour of the picker's
// standard menu item, which Qt menus cannot host; it still works and is what the File
// menu needs (a plain list of services).
NSArray<NSSharingService*>* servicesFor(NSArray* items) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return [NSSharingService sharingServicesForItems:items];
#pragma clang diagnostic pop
}

QString serviceTitle(NSSharingService* s) {
    NSString* t = s.menuItemTitle;
    if (t.length == 0)
        t = s.title;
    return t ? QString::fromNSString(t) : QString();
}

} // namespace

QString shareTypeKey(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    @autoreleasepool {
        UTType* type = suffix.isEmpty() ? nil : [UTType typeWithFilenameExtension:suffix.toNSString()];
        if (type && !type.dynamic)
            return QString::fromNSString(type.identifier);
    }
    return suffix;
}

// Any thread (ShareServiceCache runs it on a worker): the services are only listed and
// their icons drawn into bitmaps; performing one happens on the GUI thread.
QList<ShareService> shareServices(const QStringList& paths) {
    @autoreleasepool {
        QList<ShareService> out;
        NSArray* items = nsItems(paths);
        if (items.count == 0)
            return out;
        for (NSSharingService* s in servicesFor(items)) {
            ShareService e;
            e.title = serviceTitle(s);
            e.icon = mac::toQImage(s.image, 32, 2);
            if (!e.title.isEmpty())
                out << e;
        }
        return out;
    }
}

bool performShareService(const QString& title, const QStringList& paths) {
    @autoreleasepool {
        NSArray* items = nsItems(paths);
        if (items.count == 0)
            return false;
        for (NSSharingService* s in servicesFor(items)) {
            const QString t = serviceTitle(s);
            if (t == title && [s canPerformWithItems:items]) {
                [s performWithItems:items];
                return true;
            }
        }
        return false;
    }
}

bool showSharePicker(QWidget* anchor, const QRect& rectInWindow, const QStringList& paths) {
    @autoreleasepool {
        QWidget* top = anchor ? anchor->window() : nullptr;
        QWindow* handle = top ? top->windowHandle() : nullptr;
        if (!handle || !handle->handle())
            return false;
        NSView* view = (__bridge NSView*)reinterpret_cast<void*>(handle->winId());
        NSArray* items = nsItems(paths);
        if (!view || items.count == 0)
            return false;
        NSSharingServicePicker* picker = [[NSSharingServicePicker alloc] initWithItems:items];
        // Qt's content view is flipped (origin top left), like the widget coordinates.
        NSRect r = NSMakeRect(rectInWindow.x(), rectInWindow.y(), std::max(1, rectInWindow.width()),
                              std::max(1, rectInWindow.height()));
        if (!view.isFlipped)
            r.origin.y = view.bounds.size.height - r.origin.y - r.size.height;
        // Below the anchor rect.
        [picker showRelativeToRect:r
                            ofView:view
                     preferredEdge:view.isFlipped ? NSRectEdgeMaxY : NSRectEdgeMinY];
        return true;
    }
}

} // namespace lf
