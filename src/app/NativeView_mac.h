// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The NSView behind a Qt window - only on the cocoa platform. On other platforms
// (offscreen in tests, minimal) winId() is not an NSView and must never be bridged.

#include <QGuiApplication>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace lf::mac {

inline NSView* nativeViewOf(QWindow* window) {
    if (!window || !window->handle() || QGuiApplication::platformName() != QLatin1String("cocoa"))
        return nil;
    // Qt's cocoa plugin: the WId is the NSView pointer.
    // NOLINTNEXTLINE(bugprone-casting-through-void,performance-no-int-to-ptr)
    return (__bridge NSView*)reinterpret_cast<void*>(window->winId());
}

} // namespace lf::mac
