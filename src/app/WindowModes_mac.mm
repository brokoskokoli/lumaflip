// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-12 (window level) and F-VIEW-13 (minimal title bar) on the NSWindow behind a Qt
// top-level.

#include "NativeView_mac.h"
#include "WindowModes.h"

#include <QObject>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <cmath>
#include <optional>

namespace lf {

namespace {

NSWindow* nativeWindow(QWidget* widget, bool create) {
    QWidget* top = widget ? widget->window() : nullptr;
    if (!top)
        return nil;
    if (create)
        top->winId(); // creates the platform window (not shown)
    QWindow* handle = top->windowHandle();
    if (!handle || !handle->handle())
        return nil;
    NSView* view = lf::mac::nativeViewOf(handle);
    return view.window;
}

// Observes the full screen notifications of one NSWindow; removed with the widget.
class FullScreenObserver : public QObject {
public:
    FullScreenObserver(QWidget* owner, NSWindow* window, std::function<void(bool, bool)> callback)
        : QObject(owner), m_window(window), m_callback(std::move(callback)) {
        setObjectName(QStringLiteral("lfFullScreenObserver"));
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        auto add = [&](NSNotificationName name, bool entering, bool finished) {
            FullScreenObserver* self = this; // the observers are removed in the destructor
            // queue nil: called synchronously on the posting (main) thread.
            id token = [nc addObserverForName:name
                                       object:window
                                        queue:nil
                                   usingBlock:^(NSNotification*) {
                                     if (self->m_callback)
                                         self->m_callback(entering, finished);
                                   }];
            [m_tokens addObject:token];
        };
        m_tokens = [NSMutableArray array];
        add(NSWindowWillEnterFullScreenNotification, true, false);
        add(NSWindowDidEnterFullScreenNotification, true, true);
        add(NSWindowWillExitFullScreenNotification, false, false);
        add(NSWindowDidExitFullScreenNotification, false, true);
        // A failed transition has no notification (only a delegate call, which is Qt's):
        // the window's guard timer ends the wait then.
    }
    ~FullScreenObserver() override {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        for (id token in m_tokens)
            [nc removeObserver:token];
    }
    NSWindow* window() const { return m_window; }

private:
    __weak NSWindow* m_window = nil;
    NSMutableArray* m_tokens = nil;
    std::function<void(bool, bool)> m_callback;
};

} // namespace

bool watchFullScreenTransitions(QWidget* widget,
                                std::function<void(bool entering, bool finished)> onTransition) {
    @autoreleasepool {
        QWidget* top = widget ? widget->window() : nullptr;
        NSWindow* w = nativeWindow(widget, false);
        if (!top || !w)
            return false;
        // Only this class uses the name (no Q_OBJECT here, so no qobject_cast).
        auto* existing = static_cast<FullScreenObserver*>(
            top->findChild<QObject*>(QStringLiteral("lfFullScreenObserver"), Qt::FindDirectChildrenOnly));
        if (existing && existing->window() == w)
            return true;
        delete existing; // the native window was re-created
        new FullScreenObserver(top, w, std::move(onTransition));
        return true;
    }
}

void setWindowAlwaysOnTop(QWidget* window, bool on) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, true);
        if (!w)
            return;
        w.level = on ? NSFloatingWindowLevel : NSNormalWindowLevel;
        // Stays on top of other apps' windows too, also when Lumaflip is not active.
        w.hidesOnDeactivate = NO;
    }
}

void setWindowMinimalChrome(QWidget* window, bool on) {
    @autoreleasepool {
        QWidget* top = window ? window->window() : nullptr;
        if (!top)
            return;
        if (!top->windowHandle() || !top->windowHandle()->handle()) {
            // Not created yet: Qt creates the NSWindow with a full size content view and a
            // transparent title bar, so its frame margins are right from the start (placing,
            // restoring the geometry). Creating it here instead would cost startup time (P-01:
            // the window would be created before its GPU surface type is known).
            Qt::WindowFlags flags = top->windowFlags();
            flags.setFlag(Qt::ExpandedClientAreaHint, on);
            flags.setFlag(Qt::NoTitleBarBackgroundHint, on);
            if (flags != top->windowFlags())
                top->setWindowFlags(flags);
            return;
        }
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return;
        const bool fullSize = (w.styleMask & NSWindowStyleMaskFullSizeContentView) != 0;
        if (fullSize != on) {
            if (on)
                w.styleMask |= NSWindowStyleMaskFullSizeContentView;
            else
                w.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
        }
        if (w.titlebarAppearsTransparent != on)
            w.titlebarAppearsTransparent = on;
        const NSWindowTitleVisibility title = on ? NSWindowTitleHidden : NSWindowTitleVisible;
        if (w.titleVisibility != title)
            w.titleVisibility = title;
        // Dragging the image pans it; the window moves by the title strip at the top
        // (TitleStrip, QWindow::startSystemMove), not by its background.
        w.movableByWindowBackground = NO;
    }
}

namespace {

constexpr NSWindowButton kWindowButtons[] = {NSWindowCloseButton, NSWindowMiniaturizeButton,
                                             NSWindowZoomButton};

void showButtonsAtOnce(NSWindow* w) {
    for (NSWindowButton b : kWindowButtons) {
        NSButton* button = [w standardWindowButton:b];
        button.alphaValue = 1.0;
        button.hidden = NO;
    }
}

char kButtonsObserverKey = 0; // its address is the associated object key

} // namespace

} // namespace lf

// Shows the window buttons again when the window enters full screen (its title bar appears
// with the menu bar there) and remembers the last requested state; lives as an associated
// object of the NSWindow.
@interface LFWindowButtonsObserver : NSObject
@property(nonatomic, strong) id token;
@property(nonatomic) BOOL buttonsVisible;
@end

@implementation LFWindowButtonsObserver
- (void)dealloc {
    if (_token)
        [[NSNotificationCenter defaultCenter] removeObserver:_token];
}
@end

namespace lf {

void setWindowButtonsVisible(QWidget* window, bool visible, int fadeMs) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return;
        LFWindowButtonsObserver* observer = objc_getAssociatedObject(w, &kButtonsObserverKey);
        if (!observer) {
            observer = [[LFWindowButtonsObserver alloc] init];
            __weak NSWindow* weakWindow = w;
            __weak LFWindowButtonsObserver* weakObserver = observer;
            observer.token = [[NSNotificationCenter defaultCenter]
                addObserverForName:NSWindowWillEnterFullScreenNotification
                            object:w
                             queue:nil
                        usingBlock:^(NSNotification*) {
                          weakObserver.buttonsVisible = YES;
                          if (NSWindow* strong = weakWindow)
                              showButtonsAtOnce(strong);
                        }];
            objc_setAssociatedObject(w, &kButtonsObserverKey, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        if ((w.styleMask & NSWindowStyleMaskFullScreen) != 0)
            visible = true;
        observer.buttonsVisible = visible;
        for (NSWindowButton b : kWindowButtons) {
            NSButton* button = [w standardWindowButton:b];
            if (!button)
                continue;
            const CGFloat target = visible ? 1.0 : 0.0;
            if (visible)
                button.hidden = NO;
            if (fadeMs <= 0) {
                button.alphaValue = target;
                button.hidden = !visible;
                continue;
            }
            [NSAnimationContext
                runAnimationGroup:^(NSAnimationContext* context) {
                  context.duration = fadeMs / 1000.0;
                  button.animator.alphaValue = target;
                }
                completionHandler:^{
                  // Hidden buttons take no clicks - unless a fade-in followed meanwhile.
                  if (!observer.buttonsVisible)
                      button.hidden = YES;
                }];
        }
    }
}

bool titleStripUsesSystemButtons() {
    return true;
}

int titleStripHeight(QWidget* window) {
    constexpr int kDefault = 28; // title bar of a standard NSWindow
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w || (w.styleMask & NSWindowStyleMaskFullScreen) != 0)
            return kDefault;
        // With a full size content view, the content layout rect excludes the title bar.
        const CGFloat h = NSHeight(w.frame) - NSHeight(w.contentLayoutRect);
        if ((w.styleMask & NSWindowStyleMaskFullSizeContentView) == 0 || h < 20 || h > 60)
            return kDefault;
        return int(std::lround(h));
    }
}

int titleStripReservedLeft(QWidget* window) {
    constexpr int kDefault = 78; // three buttons of a standard title bar and a gap
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        NSButton* zoom = w ? [w standardWindowButton:NSWindowZoomButton] : nil;
        if (!zoom || !zoom.superview)
            return kDefault;
        const NSRect r = [zoom convertRect:zoom.bounds toView:nil];
        const int right = int(std::lround(NSMaxX(r))) + 12;
        return right > 20 && right < 200 ? right : kDefault;
    }
}

void titleStripDoubleClick(QWidget* window) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return;
        NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
        NSString* action = [defaults stringForKey:@"AppleActionOnDoubleClick"];
        std::optional<bool> miniaturize;
        if ([defaults objectForKey:@"AppleMiniaturizeOnDoubleClick"])
            miniaturize = [defaults boolForKey:@"AppleMiniaturizeOnDoubleClick"];
        switch (titleDoubleClickAction(action ? QString::fromNSString(action) : QString(), miniaturize)) {
        case TitleDoubleClick::Zoom:
            [w zoom:nil];
            break;
        case TitleDoubleClick::Minimize:
            [w miniaturize:nil];
            break;
        case TitleDoubleClick::Nothing:
            break;
        }
    }
}

void detectTitleButtonSide(QObject*, const std::function<void(TitleButtonSide)>& done) {
    if (done)
        done(TitleButtonSide::Left);
}

void setTitleStripHitTest(QWidget*, const std::function<TitleStripHit(QPoint)>&) {}

NativeWindowState nativeWindowState(QWidget* window) {
    NativeWindowState s;
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return s;
        s.valid = true;
        s.floating = w.level > NSNormalWindowLevel;
        s.fullSizeContent = (w.styleMask & NSWindowStyleMaskFullSizeContentView) != 0;
        s.titlebarTransparent = w.titlebarAppearsTransparent;
        s.titleHidden = w.titleVisibility == NSWindowTitleHidden;
        s.fullScreen = (w.styleMask & NSWindowStyleMaskFullScreen) != 0;
        NSButton* close = [w standardWindowButton:NSWindowCloseButton];
        s.buttonsHidden = close && (close.hidden || close.alphaValue < 0.5);
    }
    return s;
}

} // namespace lf
