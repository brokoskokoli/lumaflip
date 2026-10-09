// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayColorSpace.h"

#include <QByteArray>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>

namespace lf {

QColorSpace windowColorSpace(const QWidget* widget) {
    @autoreleasepool {
        NSColorSpace* ns = nil;
        const QWidget* top = widget ? widget->window() : nullptr;
        QWindow* handle = top ? top->windowHandle() : nullptr;
        if (handle && handle->handle()) {
            NSView* view = (__bridge NSView*)reinterpret_cast<void*>(handle->winId());
            if (view.window)
                ns = view.window.colorSpace;
        }
        if (!ns)
            ns = NSScreen.mainScreen.colorSpace;
        NSData* icc = ns.ICCProfileData;
        if (icc) {
            const QColorSpace cs = QColorSpace::fromIccProfile(QByteArray::fromNSData(icc));
            if (cs.isValid())
                return cs;
        }
        return QColorSpace(QColorSpace::SRgb);
    }
}

} // namespace lf
