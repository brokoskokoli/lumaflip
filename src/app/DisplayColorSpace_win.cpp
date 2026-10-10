// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: color space of the monitor a window is on (F-COL-01/02).
//
// Qt's D3D swapchain is not color managed: the desktop shows its pixels as they are, in
// the monitor's color space. Like on macOS the canvas therefore has to encode its pixels in
// the monitor's ICC profile (the one Windows color management assigns to the display,
// GetICMProfileW). Exception: with "advanced color" (HDR or Auto Color Management) the
// compositor works in scRGB and expects SDR content in sRGB - it does the conversion to
// the panel itself, so the monitor profile must not be applied a second time.
#include "DisplayColorSpace.h"

#include "platform/windows/WinDisplay.h"

#include <QFile>
#include <QWidget>
#include <QWindow>

#include <windows.h>

namespace lf {

namespace {

HMONITOR monitorOf(const QWidget* widget) {
    const QWidget* top = widget ? widget->window() : nullptr;
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (handle && handle->handle())
        return MonitorFromWindow(reinterpret_cast<HWND>(handle->winId()), MONITOR_DEFAULTTONEAREST);
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

} // namespace

QColorSpace windowColorSpace(const QWidget* widget) {
    const HMONITOR monitor = monitorOf(widget);
    if (monitor && win::monitorAdvancedColor(monitor))
        return QColorSpace(QColorSpace::SRgb);
    const QString profile = win::monitorIccProfilePath(monitor);
    if (!profile.isEmpty()) {
        QFile f(profile);
        if (f.size() > 0 && f.size() < 4 * 1024 * 1024 && f.open(QIODevice::ReadOnly)) {
            const QColorSpace cs = QColorSpace::fromIccProfile(f.readAll());
            if (cs.isValid())
                return cs;
        }
    }
    return QColorSpace(QColorSpace::SRgb);
}

} // namespace lf
