// SPDX-License-Identifier: GPL-3.0-or-later
// F-WIN-05 (tabs) and F-WIN-04 (system tiling) on the NSWindow behind a Qt top-level.

#include "WindowTabbing.h"

#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace lf {

namespace {

NSString* const kTabbingIdentifier = @"io.github.brokoskokoli.lumaflip.viewer";

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

void configureViewerWindow(QWidget* window, bool allowTabs) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, true);
        if (!w)
            return;
        w.tabbingIdentifier = kTabbingIdentifier;
        w.tabbingMode = allowTabs ? NSWindowTabbingModePreferred : NSWindowTabbingModeDisallowed;
        // Tiling and full screen need a resizable primary window (Qt's default; keep it).
        NSWindowCollectionBehavior b = w.collectionBehavior;
        b &= ~(NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorFullScreenNone);
        b |= NSWindowCollectionBehaviorFullScreenPrimary;
        w.collectionBehavior = b;
    }
}

NativeTabbingMode nativeTabbingMode(QWidget* window) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return NativeTabbingMode::Unknown;
        switch (w.tabbingMode) {
        case NSWindowTabbingModeAutomatic:
            return NativeTabbingMode::Automatic;
        case NSWindowTabbingModePreferred:
            return NativeTabbingMode::Preferred;
        case NSWindowTabbingModeDisallowed:
            return NativeTabbingMode::Disallowed;
        }
        return NativeTabbingMode::Unknown;
    }
}

bool performTabCommand(QWidget* window, TabCommand command) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w || w.tabbingMode == NSWindowTabbingModeDisallowed)
            return false;
        switch (command) {
        case TabCommand::MergeAllWindows:
            [w mergeAllWindows:nil];
            return true;
        case TabCommand::MoveTabToNewWindow:
            if (w.tabbedWindows.count < 2)
                return false;
            [w moveTabToNewWindow:nil];
            return true;
        case TabCommand::SelectNextTab:
            if (w.tabbedWindows.count < 2)
                return false;
            [w selectNextTab:nil];
            return true;
        case TabCommand::SelectPreviousTab:
            if (w.tabbedWindows.count < 2)
                return false;
            [w selectPreviousTab:nil];
            return true;
        case TabCommand::ToggleTabBar:
            [w toggleTabBar:nil];
            return true;
        }
        return false;
    }
}

int tabGroupSize(QWidget* window) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return 0;
        const NSUInteger n = w.tabbedWindows.count;
        return n == 0 ? 1 : int(n);
    }
}

bool supportsSystemTiling(QWidget* window) {
    @autoreleasepool {
        NSWindow* w = nativeWindow(window, false);
        if (!w)
            return false;
        return (w.styleMask & NSWindowStyleMaskResizable) &&
               (w.collectionBehavior & NSWindowCollectionBehaviorFullScreenPrimary);
    }
}

} // namespace lf
