// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-12 (always on top) and F-VIEW-13 (minimal title bar) on the HWND behind a Qt
// top-level. Changing the style bits directly keeps the native window (Qt's
// setWindowFlags() would destroy and re-create it, and with it the QRhi swapchain).
#include "WindowModes.h"

#include <QWidget>
#include <QWindow>

#include <windows.h>

namespace lf {

namespace {

HWND nativeWindow(QWidget* widget, bool create) {
    QWidget* top = widget ? widget->window() : nullptr;
    if (!top)
        return nullptr;
    if (create)
        top->winId(); // creates the platform window (not shown)
    QWindow* handle = top->windowHandle();
    if (!handle || !handle->handle())
        return nullptr;
    return reinterpret_cast<HWND>(handle->winId());
}

} // namespace

void setWindowAlwaysOnTop(QWidget* window, bool on) {
    HWND w = nativeWindow(window, true);
    if (!w)
        return;
    SetWindowPos(w, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void setWindowMinimalChrome(QWidget* window, bool on) {
    HWND w = nativeWindow(window, true);
    if (!w)
        return;
    // Without a caption the window keeps its resize frame; it moves by the handle strip
    // at the top (WindowExtras, QWindow::startSystemMove).
    LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    if (on)
        style &= ~LONG_PTR(WS_CAPTION);
    else
        style |= WS_CAPTION;
    SetWindowLongPtrW(w, GWL_STYLE, style);
    SetWindowPos(w, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
}

NativeWindowState nativeWindowState(QWidget* window) {
    NativeWindowState s;
    HWND w = nativeWindow(window, false);
    if (!w)
        return s;
    s.valid = true;
    s.floating = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool captionless = (GetWindowLongPtrW(w, GWL_STYLE) & WS_CAPTION) != WS_CAPTION;
    s.fullSizeContent = captionless;
    s.titlebarTransparent = captionless;
    s.titleHidden = captionless;
    return s;
}

} // namespace lf
