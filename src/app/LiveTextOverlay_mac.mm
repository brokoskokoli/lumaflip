// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-06: Live Text overlay (LiveTextOverlay.h) through app/LiveTextBridge.swift.

#include "LiveTextOverlay.h"

#include "ImageView.h"
#include "NativeView_mac.h"
#include "core/LiveText.h"

#include <QImage>
#include <QPointer>
#include <QTransform>
#include <QWindow>

#import <AppKit/AppKit.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>

#if LF_HAVE_LIVETEXT
// Interface of the @objc class in LiveTextBridge.swift. The class lives in
// libLumaflipLiveText.dylib, loaded on first use (bridgeClass()): never referenced by name,
// so the app links neither the Swift runtime nor VisionKit (P-01 startup time).
@interface LFLiveTextBridge : NSObject
@property(nonatomic, readonly) NSView* view;
@property(nonatomic, readonly) BOOL hasActiveTextSelection;
+ (BOOL)isSupported;
- (void)setContentsRect:(CGRect)rect;
- (void)analyze:(CGImageRef)image completion:(void (^)(BOOL hasResults, NSString* _Nullable error))completion;
- (void)cancel;
@end
#endif

namespace lf {

namespace {
// Live Text needs no more than this; larger images are scaled down for the analysis.
constexpr int kMaxAnalysisSide = LiveTextSession::kMaxAnalysisSide;

std::atomic<bool> g_availabilityKnown{false};

#if LF_HAVE_LIVETEXT
// The bridge class from libLumaflipLiveText.dylib: Contents/Frameworks in the app bundle.
// Only an unbundled binary (tests, a plain build) falls back to the build tree: a release
// app never loads code from an absolute path outside of itself. Nil if the library cannot be
// loaded. Thread-safe (static initialization).
Class bridgeClass() {
    static Class cls = [] {
        Class found = nil;
        @autoreleasepool {
            NSMutableArray<NSString*>* candidates = [NSMutableArray array];
            const bool bundled = [NSBundle.mainBundle.bundlePath.pathExtension isEqualToString:@"app"];
            NSString* fw = NSBundle.mainBundle.privateFrameworksPath;
            if (bundled && fw)
                [candidates addObject:[fw stringByAppendingPathComponent:@"libLumaflipLiveText.dylib"]];
            if (!bundled)
                [candidates addObject:@LF_LIVETEXT_BUILD_DYLIB];
            for (NSString* path in candidates) {
                if (![NSFileManager.defaultManager fileExistsAtPath:path])
                    continue;
                if (dlopen(path.fileSystemRepresentation, RTLD_LAZY | RTLD_LOCAL)) {
                    found = NSClassFromString(@"LFLiveTextBridge");
                    if (!found)
                        NSLog(@"Live Text: %@ loaded, but LFLiveTextBridge is missing", path);
                    break;
                }
                NSLog(@"Live Text: %s", dlerror());
            }
            if (!found)
                NSLog(@"Live Text: bridge library not available");
        }
        return found;
    }();
    return cls;
}
#endif
} // namespace

struct LiveTextOverlay::Native {
#if LF_HAVE_LIVETEXT
    LFLiveTextBridge* bridge = nil;
#endif
};

LiveTextOverlay::LiveTextOverlay(ImageView* view, QObject* parent)
    : QObject(parent), m_native(std::make_unique<Native>()), m_view(view) {}

LiveTextOverlay::~LiveTextOverlay() {
#if LF_HAVE_LIVETEXT
    @autoreleasepool {
        if (m_native->bridge) {
            [m_native->bridge cancel];
            [m_native->bridge.view removeFromSuperview];
        }
    }
#endif
}

bool LiveTextOverlay::isAvailable() {
#if LF_HAVE_LIVETEXT
    Class cls = bridgeClass();
    const bool ok = cls && [cls isSupported];
#else
    const bool ok = false;
#endif
    g_availabilityKnown = true;
    return ok;
}

bool LiveTextOverlay::availabilityKnown() {
#if LF_HAVE_LIVETEXT
    return g_availabilityKnown;
#else
    return true;
#endif
}

bool LiveTextOverlay::isSupported() const {
    return isAvailable();
}

void LiveTextOverlay::analyze(quint64 token, const QImage& image) {
#if LF_HAVE_LIVETEXT
    @autoreleasepool {
        if (!m_native->bridge) {
            Class cls = bridgeClass();
            if (!cls) {
                QPointer<LiveTextOverlay> self(this);
                QMetaObject::invokeMethod(
                    this,
                    [self, token] {
                        if (self && self->m_session)
                            self->m_session->analysisFinished(token, false, QStringLiteral("VisionKit"));
                    },
                    Qt::QueuedConnection);
                return;
            }
            m_native->bridge = [[cls alloc] init];
        }
        QImage src = image;
        if (std::max(src.width(), src.height()) > kMaxAnalysisSide)
            src =
                src.scaled(kMaxAnalysisSide, kMaxAnalysisSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        CGImageRef cg = src.toCGImage();
        if (!cg) {
            QPointer<LiveTextOverlay> self(this);
            QMetaObject::invokeMethod(
                this,
                [self, token] {
                    if (self && self->m_session)
                        self->m_session->analysisFinished(token, false, QStringLiteral("CGImage"));
                },
                Qt::QueuedConnection);
            return;
        }
        QPointer<LiveTextOverlay> self(this);
        [m_native->bridge analyze:cg
                       completion:^(BOOL hasResults, NSString* error) {
                         if (!self || !self->m_session)
                             return;
                         self->m_session->analysisFinished(token, hasResults,
                                                           error ? QString::fromNSString(error) : QString());
                       }];
        CGImageRelease(cg);
        updateGeometry();
    }
#else
    Q_UNUSED(token);
    Q_UNUSED(image);
#endif
}

void LiveTextOverlay::cancel() {
#if LF_HAVE_LIVETEXT
    @autoreleasepool {
        if (m_native->bridge)
            [m_native->bridge cancel];
    }
#endif
}

void LiveTextOverlay::setVisible(bool visible) {
    m_visible = visible;
#if LF_HAVE_LIVETEXT
    @autoreleasepool {
        if (!m_native->bridge)
            return;
        NSView* overlay = m_native->bridge.view;
        if (!visible) {
            [overlay removeFromSuperview];
            return;
        }
        updateGeometry();
    }
#endif
}

void LiveTextOverlay::updateGeometry() {
#if LF_HAVE_LIVETEXT
    @autoreleasepool {
        if (!m_native->bridge || !m_view || !m_visible)
            return;
        QWidget* top = m_view->window();
        QWindow* handle = top ? top->windowHandle() : nullptr;
        NSView* content = lf::mac::nativeViewOf(handle);
        if (!content)
            return;
        NSView* overlay = m_native->bridge.view;
        if (overlay.superview != content) {
            [overlay removeFromSuperview];
            [content addSubview:overlay positioned:NSWindowAbove relativeTo:nil];
        }
        // The image view's area in the window (logical px, origin top left). Below the
        // title bar of a borderless window (F-VIEW-15): the window buttons and the title
        // strip stay above the text selection.
        const QRect viewRect(m_view->mapTo(top, QPoint(0, 0)), m_view->size());
        QRect r = viewRect;
        if (NSWindow* nw = content.window; nw && (nw.styleMask & NSWindowStyleMaskFullSizeContentView) &&
                                           !(nw.styleMask & NSWindowStyleMaskFullScreen)) {
            const int titleBar = int(NSHeight(nw.frame) - NSHeight(nw.contentLayoutRect));
            if (titleBar > 0 && r.top() < titleBar)
                r.setTop(std::min(titleBar, r.bottom()));
        }
        const CGFloat h = content.bounds.size.height;
        overlay.frame = content.isFlipped ? NSMakeRect(r.x(), r.y(), r.width(), r.height())
                                          : NSMakeRect(r.x(), h - r.y() - r.height(), r.width(), r.height());
        // The image inside it, in unit coordinates (origin top left; the bridge converts).
        const auto img = m_view->image();
        if (!img || r.width() <= 0 || r.height() <= 0)
            return;
        const QSize full = img->fullSize.isEmpty() ? img->image.size() : img->fullSize;
        const QRectF shown = m_view->imageToWidget()
                                 .mapRect(QRectF(QPointF(0, 0), QSizeF(full)))
                                 .translated(0, viewRect.top() - r.top());
        [m_native->bridge setContentsRect:CGRectMake(shown.x() / r.width(), shown.y() / r.height(),
                                                     shown.width() / r.width(), shown.height() / r.height())];
    }
#endif
}

} // namespace lf
