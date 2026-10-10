// SPDX-License-Identifier: GPL-3.0-or-later
// Windows HDR (F-COL-03): the macOS EDR concept maps to an HDR output in "advanced color"
// mode. The headroom is the display's peak luminance (DXGI_OUTPUT_DESC1::MaxLuminance)
// relative to the SDR white level the user chose ("SDR content brightness"); the extended
// presentation is an scRGB (RGBA16F, linear, sRGB primaries, 1.0 = SDR white) swapchain
// (EdrSurface, QRhiSwapChain::HDRExtendedSrgbLinear).
#include "EdrDisplay.h"

#include "platform/windows/WinDisplay.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#include <functional>
#include <vector>

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

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
    return TRUE;
}

struct DisplayFilter : QAbstractNativeEventFilter {
    std::function<void()> changed;
    bool nativeEventFilter(const QByteArray& type, void* message, qintptr*) override {
        if (type != "windows_generic_MSG")
            return false;
        const MSG* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_DISPLAYCHANGE && changed)
            changed();
        return false;
    }
};

} // namespace

hdr::DisplayState edrDisplayState(const QWidget* widget) {
    hdr::DisplayState s;
    const win::MonitorColorInfo info = win::monitorColorInfo(monitorOf(widget));
    const float headroom = win::monitorHeadroom(info);
    s.currentHeadroom = headroom;
    s.potentialHeadroom = headroom;
    return s;
}

bool anyScreenSupportsEdr() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors));
    for (HMONITOR m : monitors)
        if (win::monitorHeadroom(win::monitorColorInfo(m)) > hdr::kHdrThreshold)
            return true;
    return false;
}

bool edrPresentationAvailable() {
    return QGuiApplication::platformName() == QLatin1String("windows") &&
           qEnvironmentVariableIntValue("LF_NO_EDR") != 1;
}

EdrDisplayWatcher::EdrDisplayWatcher(std::function<void()> changed, QObject* parent) : QObject(parent) {
    auto* filter = new DisplayFilter;
    filter->changed = std::move(changed);
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installNativeEventFilter(filter);
    m_token = filter;
}

EdrDisplayWatcher::~EdrDisplayWatcher() {
    auto* filter = static_cast<DisplayFilter*>(m_token);
    if (QCoreApplication::instance())
        QCoreApplication::instance()->removeNativeEventFilter(filter);
    delete filter;
}

} // namespace lf
