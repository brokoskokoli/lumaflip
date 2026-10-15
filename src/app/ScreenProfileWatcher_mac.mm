// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeView_mac.h"
#include "ScreenProfileWatcher.h"

#include <QPointer>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace lf {

namespace {

NSWindow* nativeWindow(QWidget* widget) {
    QWidget* top = widget ? widget->window() : nullptr;
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (!handle || !handle->handle())
        return nil;
    NSView* view = lf::mac::nativeViewOf(handle);
    return view.window;
}

} // namespace

// By value like the Windows version, which moves it; the block below copies it once.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
ScreenProfileWatcher::ScreenProfileWatcher(QWidget* window, std::function<void()> changed, QObject* parent)
    : QObject(parent) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window);
        if (!w)
            return;
        QPointer<ScreenProfileWatcher> guard(this);
        void (^block)(NSNotification*) = ^(NSNotification*) {
          if (guard && changed)
              changed();
        };
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        NSOperationQueue* main = [NSOperationQueue mainQueue];
        NSArray* tokens = @[
            [nc addObserverForName:NSWindowDidChangeScreenProfileNotification
                            object:w
                             queue:main
                        usingBlock:block],
            [nc addObserverForName:NSWindowDidChangeScreenNotification object:w queue:main usingBlock:block],
        ];
        m_token = (void*)CFBridgingRetain(tokens);
    }
}

ScreenProfileWatcher::~ScreenProfileWatcher() {
    if (!m_token)
        return;
    @autoreleasepool {
        NSArray* tokens = CFBridgingRelease(m_token);
        for (id t in tokens)
            [[NSNotificationCenter defaultCenter] removeObserver:t];
    }
}

bool ScreenProfileWatcher::postProfileChangeForTesting(QWidget* window) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window);
        if (!w)
            return false;
        [[NSNotificationCenter defaultCenter] postNotificationName:NSWindowDidChangeScreenProfileNotification
                                                            object:w];
        return true;
    }
}

} // namespace lf
