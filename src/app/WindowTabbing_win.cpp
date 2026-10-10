// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: there are no native window tabs (F-WIN-04 is a macOS feature); every viewer is
// a window of its own. Snap layouts (system tiling) work with every resizable window.
#include "WindowTabbing.h"

#include <QWidget>
#include <QWindow>

#include <windows.h>

namespace lf {

void configureViewerWindow(QWidget*, bool) {}

NativeTabbingMode nativeTabbingMode(QWidget*) {
    return NativeTabbingMode::Disallowed;
}

bool performTabCommand(QWidget*, TabCommand) {
    return false;
}

int tabGroupSize(QWidget* window) {
    return window && window->windowHandle() ? 1 : 0;
}

bool supportsSystemTiling(QWidget* window) {
    QWidget* top = window ? window->window() : nullptr;
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (!handle || !handle->handle())
        return false;
    const LONG_PTR style = GetWindowLongPtrW(reinterpret_cast<HWND>(handle->winId()), GWL_STYLE);
    return (style & WS_THICKFRAME) && (style & WS_MAXIMIZEBOX);
}

} // namespace lf
