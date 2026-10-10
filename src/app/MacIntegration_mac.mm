// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-07 Quick Look + Services, F-FILE-09 desktop picture (MacIntegration.h).

#include "MacIntegration.h"

#include "NativeView_mac.h"
#include "core/Wallpaper.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QUrl>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <Quartz/Quartz.h> // declarations only: Quartz is loaded on first use (panelClass())
#include <dlfcn.h>
#include <objc/runtime.h>

using lf::macint::ServiceContext;
using lf::macint::ServiceImage;

// Per-window responder: Services (validRequestorForSendType) and Quick Look panel control.
@interface LFWindowResponder : NSResponder <QLPreviewPanelDataSource, QLPreviewPanelDelegate>
@property(nonatomic, copy) NSURL* previewURL;
@property(nonatomic, assign) BOOL controlsPanel;
@end

@implementation LFWindowResponder {
@public
    ServiceContext context;
    ServiceImage image;
    std::function<void(int)> step;
}

- (id)validRequestorForSendType:(NSPasteboardType)sendType returnType:(NSPasteboardType)returnType {
    if (context && sendType &&
        lf::services::canProvide(QString::fromNSString(sendType),
                                 returnType ? QString::fromNSString(returnType) : QString(), context()))
        return self;
    return [super validRequestorForSendType:sendType returnType:returnType];
}

- (BOOL)writeSelectionToPasteboard:(NSPasteboard*)pboard types:(NSArray<NSPasteboardType>*)types {
    if (!context)
        return NO;
    const lf::services::Context ctx = context();
    QStringList requested;
    for (NSPasteboardType t in types)
        requested << QString::fromNSString(t);
    const QStringList write = lf::services::typesToWrite(requested, ctx);
    if (write.isEmpty())
        return NO;
    NSMutableArray<NSPasteboardType>* declared = [NSMutableArray array];
    for (const QString& t : write)
        [declared addObject:t.toNSString()];
    [pboard declareTypes:declared owner:nil];
    BOOL wrote = NO;
    QImage pixels;
    for (const QString& t : write) {
        if (t == QLatin1String(lf::services::kFileUrlType)) {
            NSURL* url = [NSURL fileURLWithPath:ctx.path.toNSString()];
            if (NSString* s = url.absoluteString)
                wrote = [pboard setString:s forType:NSPasteboardTypeFileURL] || wrote;
            continue;
        }
        if (pixels.isNull() && image)
            pixels = image();
        if (pixels.isNull())
            continue;
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        const bool png = t == QLatin1String(lf::services::kPngType);
        if (!pixels.save(&buffer, png ? "PNG" : "TIFF"))
            continue;
        NSData* data = [NSData dataWithBytes:bytes.constData() length:NSUInteger(bytes.size())];
        wrote = [pboard setData:data forType:t.toNSString()] || wrote;
    }
    return wrote;
}

// ---- Quick Look panel control ----
- (BOOL)acceptsPreviewPanelControl:(QLPreviewPanel*)panel {
    Q_UNUSED(panel);
    return self.previewURL != nil;
}

- (void)beginPreviewPanelControl:(QLPreviewPanel*)panel {
    self.controlsPanel = YES;
    panel.dataSource = self;
    panel.delegate = self;
}

- (void)endPreviewPanelControl:(QLPreviewPanel*)panel {
    self.controlsPanel = NO;
    panel.dataSource = nil;
    panel.delegate = nil;
}

- (NSInteger)numberOfPreviewItemsInPreviewPanel:(QLPreviewPanel*)panel {
    Q_UNUSED(panel);
    return self.previewURL ? 1 : 0;
}

- (id<QLPreviewItem>)previewPanel:(QLPreviewPanel*)panel previewItemAtIndex:(NSInteger)index {
    Q_UNUSED(panel);
    Q_UNUSED(index);
    return self.previewURL;
}

- (BOOL)previewPanel:(QLPreviewPanel*)panel handleEvent:(NSEvent*)event {
    Q_UNUSED(panel);
    if (event.type != NSEventTypeKeyDown || !step)
        return NO;
    const unichar c =
        event.charactersIgnoringModifiers.length ? [event.charactersIgnoringModifiers characterAtIndex:0] : 0;
    int direction = 0;
    if (c == NSRightArrowFunctionKey || c == NSDownArrowFunctionKey)
        direction = 1;
    else if (c == NSLeftArrowFunctionKey || c == NSUpArrowFunctionKey)
        direction = -1;
    if (direction == 0)
        return NO;
    // Queued: the window flips (and calls quickLookFollow) outside the panel's event handling.
    auto callback = step;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [callback, direction] { callback(direction); }, Qt::QueuedConnection);
    return YES;
}
@end

// "In Lumaflip öffnen" (NSServices, NSMessage = openInLumaflip).
@interface LFServicesProvider : NSObject
@end

@implementation LFServicesProvider {
@public
    std::function<void(const QStringList&)> open;
}
- (void)openInLumaflip:(NSPasteboard*)pboard
              userData:(NSString*)userData
                 error:(NSString* __autoreleasing*)error {
    Q_UNUSED(userData);
    NSArray<NSURL*>* urls = [pboard readObjectsForClasses:@[ [NSURL class] ]
                                                  options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    QList<QUrl> list;
    for (NSURL* u in urls)
        list << QUrl::fromNSURL(u);
    const QStringList files = lf::services::filesFromUrls(list);
    if (files.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("lf::MacIntegration", "Es wurden keine Dateien übergeben.")
                         .toNSString();
        return;
    }
    auto callback = open;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [callback, files] {
            if (callback)
                callback(files);
        },
        Qt::QueuedConnection);
}
@end

namespace lf::macint {

namespace {

char kResponderKeyStorage = 0; // its address is the associated-object key
const void* const kResponderKey = &kResponderKeyStorage;

NSWindow* nativeWindow(QWidget* widget) {
    QWidget* top = widget ? widget->window() : nullptr;
    if (!top)
        return nil;
    QWindow* handle = top->windowHandle();
    if (!handle || !handle->handle())
        return nil;
    NSView* view = lf::mac::nativeViewOf(handle);
    return view.window;
}

LFWindowResponder* responderFor(QWidget* widget, bool create) {
    NSWindow* w = nativeWindow(widget);
    if (!w)
        return nil;
    LFWindowResponder* r = objc_getAssociatedObject(w, kResponderKey);
    if (!r && create) {
        r = [[LFWindowResponder alloc] init];
        objc_setAssociatedObject(w, kResponderKey, r, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        // After the window in the responder chain: AppKit asks it for Services and the Quick
        // Look panel when the window's views do not answer.
        r.nextResponder = w.nextResponder;
        w.nextResponder = r;
    }
    return r;
}

} // namespace

namespace {

// QLPreviewPanel without linking Quartz (P-01: the umbrella framework costs startup time).
// Loaded on first use; nil if it cannot be loaded.
Class panelClass() {
    static Class cls = [] {
        if (!dlopen("/System/Library/Frameworks/Quartz.framework/Quartz", RTLD_LAZY | RTLD_LOCAL))
            return Class(nil);
        return NSClassFromString(@"QLPreviewPanel");
    }();
    return cls;
}

// The shared panel if it exists (without creating it or loading Quartz).
QLPreviewPanel* existingPanel() {
    static bool loaded = false;
    if (!loaded) {
        // Not loaded yet: no panel can exist.
        if (!dlopen("/System/Library/Frameworks/Quartz.framework/Quartz", RTLD_LAZY | RTLD_NOLOAD))
            return nil;
        loaded = true;
    }
    Class cls = panelClass();
    return cls && [cls sharedPreviewPanelExists] ? [cls sharedPreviewPanel] : nil;
}

} // namespace

bool quickLookToggle(QWidget* window, const QString& path, std::function<void(int)> step) {
    @autoreleasepool {
        LFWindowResponder* r = responderFor(window, true);
        if (!r)
            return false;
        if (QLPreviewPanel* open = existingPanel(); open && open.isVisible) {
            [open orderOut:nil];
            return true;
        }
        Class cls = panelClass();
        if (!cls)
            return false;
        r.previewURL = [NSURL fileURLWithPath:path.toNSString()];
        r->step = std::move(step);
        QLPreviewPanel* panel = [cls sharedPreviewPanel];
        [panel updateController];
        [panel makeKeyAndOrderFront:nil];
        return true;
    }
}

void quickLookFollow(QWidget* window, const QString& path) {
    @autoreleasepool {
        LFWindowResponder* r = responderFor(window, false);
        if (!r || !r.controlsPanel)
            return;
        r.previewURL = path.isEmpty() ? nil : [NSURL fileURLWithPath:path.toNSString()];
        if (QLPreviewPanel* panel = existingPanel())
            [panel reloadData];
    }
}

bool quickLookVisible() {
    QLPreviewPanel* panel = existingPanel();
    return panel && panel.isVisible;
}

void quickLookClose() {
    if (QLPreviewPanel* panel = existingPanel())
        [panel orderOut:nil];
}

void installServiceResponder(QWidget* window, ServiceContext context, ServiceImage image) {
    @autoreleasepool {
        LFWindowResponder* r = responderFor(window, true);
        if (!r)
            return;
        r->context = std::move(context);
        r->image = std::move(image);
    }
}

void registerServicesProvider(std::function<void(const QStringList& files)> openFiles) {
    if (!NSApp)
        return;
    static LFServicesProvider* provider = nil;
    @autoreleasepool {
        if (!provider)
            provider = [[LFServicesProvider alloc] init];
        provider->open = std::move(openFiles);
        NSApp.servicesProvider = provider;
        NSMutableArray<NSPasteboardType>* send = [NSMutableArray array];
        for (const QString& t : services::sendTypes())
            [send addObject:t.toNSString()];
        [NSApp registerServicesMenuSendTypes:send returnTypes:@[]];
    }
}

namespace {

class MacWallpaperService final : public WallpaperService {
public:
    explicit MacWallpaperService(QWidget* window) : m_window(window) {}

    QList<WallpaperScreen> screens() const override {
        QList<WallpaperScreen> out;
        @autoreleasepool {
            NSWindow* w = m_window ? nativeWindow(m_window) : nil;
            NSArray<NSScreen*>* all = NSScreen.screens;
            for (NSUInteger i = 0; i < all.count; ++i) {
                NSScreen* s = all[i];
                WallpaperScreen ws;
                ws.id = QString::number([s.deviceDescription[@"NSScreenNumber"] unsignedIntValue]);
                ws.name = QString::fromNSString(s.localizedName);
                ws.isMain = i == 0; // the screen with the menu bar
                ws.hasWindow = w && w.screen == s;
                out << ws;
            }
        }
        return out;
    }

    bool setDesktopImage(const QString& screenId, const QString& imagePath, QString* error) override {
        @autoreleasepool {
            for (NSScreen* s in NSScreen.screens) {
                if (QString::number([s.deviceDescription[@"NSScreenNumber"] unsignedIntValue]) != screenId)
                    continue;
                NSWorkspace* ws = NSWorkspace.sharedWorkspace;
                NSDictionary* options = [ws desktopImageOptionsForScreen:s];
                if (!options)
                    options = @{};
                NSError* err = nil;
                if ([ws setDesktopImageURL:[NSURL fileURLWithPath:imagePath.toNSString()]
                                 forScreen:s
                                   options:options
                                     error:&err])
                    return true;
                if (error)
                    *error = err ? QString::fromNSString(err.localizedDescription) : QString();
                return false;
            }
            if (error)
                *error =
                    QCoreApplication::translate("lf::MacIntegration", "Der Bildschirm ist nicht mehr da.");
            return false;
        }
    }

private:
    QPointer<QWidget> m_window;
};

} // namespace

std::unique_ptr<WallpaperService> createWallpaperService(QWidget* window) {
    return std::make_unique<MacWallpaperService>(window);
}

} // namespace lf::macint
