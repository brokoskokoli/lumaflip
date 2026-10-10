// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: the window moved to another monitor (QWindow::screenChanged) or the display
// configuration changed (WM_DISPLAYCHANGE: resolution, HDR switched, profile assignment
// via the color management control panel is followed by a display change broadcast).
#include "ScreenProfileWatcher.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#include <windows.h>

namespace lf {

namespace {

struct Watch : QAbstractNativeEventFilter {
    QPointer<ScreenProfileWatcher> owner;
    std::function<void()> changed;
    QMetaObject::Connection screenConnection;
    QWindow* window = nullptr;

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (type != "windows_generic_MSG")
            return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_DISPLAYCHANGE ||
            (msg->message == WM_SETTINGCHANGE && msg->lParam &&
             lstrcmpiW(reinterpret_cast<LPCWSTR>(msg->lParam), L"ImmersiveColorSet") == 0))
            notify();
        return false;
    }
    void notify() {
        if (owner && changed)
            changed();
    }
};

} // namespace

ScreenProfileWatcher::ScreenProfileWatcher(QWidget* window, std::function<void()> changed, QObject* parent)
    : QObject(parent) {
    QWidget* top = window ? window->window() : nullptr;
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (!handle || !handle->handle())
        return;
    auto* w = new Watch;
    w->owner = this;
    w->changed = std::move(changed);
    w->window = handle;
    w->screenConnection = connect(handle, &QWindow::screenChanged, this, [w] { w->notify(); });
    QCoreApplication::instance()->installNativeEventFilter(w);
    m_token = w;
}

ScreenProfileWatcher::~ScreenProfileWatcher() {
    auto* w = static_cast<Watch*>(m_token);
    if (!w)
        return;
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(w);
    disconnect(w->screenConnection);
    delete w;
}

bool ScreenProfileWatcher::postProfileChangeForTesting(QWidget* window) {
    QWidget* top = window ? window->window() : nullptr;
    QWindow* handle = top ? top->windowHandle() : nullptr;
    if (!handle || !handle->handle())
        return false;
    // Delivered to the native event filters like a real broadcast.
    SendMessageW(reinterpret_cast<HWND>(handle->winId()), WM_DISPLAYCHANGE, 32, 0);
    return true;
}

} // namespace lf
