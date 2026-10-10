// SPDX-License-Identifier: GPL-3.0-or-later
#include "EdrDisplay.h"
#include "NativeView_mac.h"

#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace lf {

namespace {

NSScreen* screenOf(const QWidget* widget) {
    const QWidget* top = widget ? widget->window() : nullptr;
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (handle && handle->handle() && QGuiApplication::platformName() == QLatin1String("cocoa")) {
        NSView* view = lf::mac::nativeViewOf(handle);
        if (view.window.screen)
            return view.window.screen;
    }
    return NSScreen.mainScreen;
}

} // namespace

hdr::DisplayState edrDisplayState(const QWidget* widget) {
    @autoreleasepool {
        hdr::DisplayState s;
        NSScreen* screen = screenOf(widget);
        if (!screen)
            return s;
        s.currentHeadroom = std::max(1.0f, float(screen.maximumExtendedDynamicRangeColorComponentValue));
        s.potentialHeadroom =
            std::max(1.0f, float(screen.maximumPotentialExtendedDynamicRangeColorComponentValue));
        return s;
    }
}

bool anyScreenSupportsEdr() {
    @autoreleasepool {
        for (NSScreen* screen in NSScreen.screens)
            if (screen.maximumPotentialExtendedDynamicRangeColorComponentValue > hdr::kHdrThreshold)
                return true;
        return false;
    }
}

bool edrPresentationAvailable() {
    return QGuiApplication::platformName() == QLatin1String("cocoa") &&
           qEnvironmentVariableIntValue("LF_NO_EDR") != 1;
}

EdrDisplayWatcher::EdrDisplayWatcher(std::function<void()> changed, QObject* parent) : QObject(parent) {
    @autoreleasepool {
        id token = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationDidChangeScreenParametersNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification*) {
                      if (changed)
                          changed();
                    }];
        m_token = (__bridge_retained void*)token;
    }
}

EdrDisplayWatcher::~EdrDisplayWatcher() {
    if (m_token) {
        id token = (__bridge_transfer id)m_token;
        [[NSNotificationCenter defaultCenter] removeObserver:token];
    }
}

} // namespace lf
