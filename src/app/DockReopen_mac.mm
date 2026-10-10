// SPDX-License-Identifier: GPL-3.0-or-later
#include "DockReopen.h"

#include <QCoreApplication>
#include <QMetaObject>

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

namespace lf {

namespace {

std::function<void()>& handler() {
    static std::function<void()> h;
    return h;
}

void notifyReopen() {
    // Queued: AppKit is in the middle of dispatching the Apple event.
    if (QCoreApplication* app = QCoreApplication::instance())
        QMetaObject::invokeMethod(
            app,
            [] {
                if (const auto& h = handler())
                    h();
            },
            Qt::QueuedConnection);
}

// Extends the delegate's applicationShouldHandleReopen:hasVisibleWindows: (Qt's
// QCocoaApplicationDelegate implements it) so that the original still runs; adds the
// method if the delegate has none.
void hookDelegate() {
    static bool hooked = false;
    if (hooked)
        return;
    id<NSApplicationDelegate> delegate = NSApp ? NSApp.delegate : nil;
    if (!delegate)
        return;
    hooked = true;
    Class cls = [(id)delegate class];
    SEL sel = @selector(applicationShouldHandleReopen:hasVisibleWindows:);
    Method method = class_getInstanceMethod(cls, sel);
    if (method) {
        using Original = BOOL (*)(id, SEL, NSApplication*, BOOL);
        const auto original = reinterpret_cast<Original>(method_getImplementation(method));
        IMP replacement =
            imp_implementationWithBlock(^BOOL(id self, NSApplication* app, BOOL hasVisibleWindows) {
              notifyReopen();
              return original(self, sel, app, hasVisibleWindows);
            });
        method_setImplementation(method, replacement);
    } else {
        IMP added = imp_implementationWithBlock(^BOOL(id, NSApplication*, BOOL) {
          notifyReopen();
          return YES;
        });
        class_addMethod(cls, sel, added, "c@:@c");
    }
}

} // namespace

void installDockReopenHandler(std::function<void()> h) {
    handler() = std::move(h);
    if (handler())
        hookDelegate();
}

bool simulateDockReopenForTesting() {
    id<NSApplicationDelegate> delegate = NSApp ? NSApp.delegate : nil;
    if (![(id)delegate respondsToSelector:@selector(applicationShouldHandleReopen:hasVisibleWindows:)])
        return false;
    [delegate applicationShouldHandleReopen:NSApp hasVisibleWindows:NO];
    return true;
}

} // namespace lf
