// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-12 (window level) and F-VIEW-13 (minimal title bar) on the NSWindow behind a Qt
// top-level.

#include "WindowModes.h"

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
    NSView* view = (__bridge NSView*)reinterpret_cast<void*>(handle->winId());
    return view.window;
}

} // namespace

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
    }
    return s;
}

} // namespace lf
