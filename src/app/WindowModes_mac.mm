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
        NSWindow* w = nativeWindow(window, true);
        if (!w)
            return;
        if (on)
            w.styleMask |= NSWindowStyleMaskFullSizeContentView;
        else
            w.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
        w.titlebarAppearsTransparent = on;
        w.titleVisibility = on ? NSWindowTitleHidden : NSWindowTitleVisible;
        // Dragging the image pans it; the window moves by the handle strip at the top
        // (WindowExtras, QWindow::startSystemMove), not by its background.
        w.movableByWindowBackground = NO;
    }
}

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
    }
    return s;
}

} // namespace lf
