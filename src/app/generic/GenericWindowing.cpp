// SPDX-License-Identifier: GPL-3.0-or-later
// Window system integration without AppKit (Linux and other non-macOS builds; the macOS
// versions are the *_mac.mm files next to the headers in src/app). What has no counterpart
// here (Dock, native window tabs, EDR screens, the menu key-equivalent fix, per-window
// screen profiles) is a no-op that reports "not available"; see docs/LINUX.md.

#include "app/DisplayColorSpace.h"
#include "app/DockReopen.h"
#include "app/EdrDisplay.h"
#include "app/MenuKeyEquivalents.h"
#include "app/ScreenProfileWatcher.h"
#include "app/StartupWarmup.h"
#include "app/SystemSymbols.h"
#include "app/WindowModes.h"
#include "app/WindowTabbing.h"

#include <QColorSpace>
#include <QIcon>
#include <QImage>
#include <QWidget>
#include <QWindow>

namespace lf {

// ---- DisplayColorSpace.h ----
// No portable API for the monitor profile (colord would need D-Bus and a configured
// profile): images are converted to sRGB, which is what the compositor assumes.
QColorSpace windowColorSpace(const QWidget*) {
    return QColorSpace(QColorSpace::SRgb);
}

// ---- DockReopen.h ----
void installDockReopenHandler(std::function<void()>) {}

bool simulateDockReopenForTesting() {
    return false;
}

// ---- EdrDisplay.h ----
hdr::DisplayState edrDisplayState(const QWidget*) {
    return {}; // SDR: headroom 1 (HDR images are tone-mapped)
}

bool anyScreenSupportsEdr() {
    return false;
}

bool edrPresentationAvailable() {
    return false;
}

EdrDisplayWatcher::EdrDisplayWatcher(std::function<void()>, QObject* parent) : QObject(parent) {}

EdrDisplayWatcher::~EdrDisplayWatcher() = default;

// ---- MenuKeyEquivalents.h ----
int fixShiftedMenuKeyEquivalents() {
    return 0;
}

void installMenuKeyEquivalentFix() {}

// ---- ScreenProfileWatcher.h ----
ScreenProfileWatcher::ScreenProfileWatcher(QWidget*, std::function<void()>, QObject* parent)
    : QObject(parent) {}

ScreenProfileWatcher::~ScreenProfileWatcher() = default;

bool ScreenProfileWatcher::postProfileChangeForTesting(QWidget*) {
    return false;
}

// ---- SystemSymbols.h ----
// No SF Symbols: callers fall back to the style's standard icons.
QImage systemSymbolMask(const QString&, int) {
    return {};
}

QIcon systemSymbolIcon(const QString&, int, const QColor&) {
    return {};
}

// ---- WindowModes.h ----
void setWindowAlwaysOnTop(QWidget* window, bool on) {
    if (!window || window->windowFlags().testFlag(Qt::WindowStaysOnTopHint) == on)
        return;
    const bool visible = window->isVisible();
    window->setWindowFlag(Qt::WindowStaysOnTopHint, on);
    if (visible)
        window->show(); // changing the flags hides the window
}

void setWindowMinimalChrome(QWidget*, bool) {
    // The window manager owns the title bar on X11/Wayland.
}

NativeWindowState nativeWindowState(QWidget* window) {
    NativeWindowState s;
    if (!window || !window->windowHandle())
        return s;
    s.valid = true;
    s.floating = window->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
    return s;
}

// ---- WindowTabbing.h ----
void configureViewerWindow(QWidget*, bool) {}

NativeTabbingMode nativeTabbingMode(QWidget*) {
    return NativeTabbingMode::Unknown;
}

bool performTabCommand(QWidget*, TabCommand) {
    return false;
}

int tabGroupSize(QWidget*) {
    return 1;
}

bool supportsSystemTiling(QWidget*) {
    return false;
}

} // namespace lf

namespace lf::startup {

// StartupWarmup.h: the GPU driver is loaded by QRhi itself (OpenGL/Vulkan).
void warmUpGpu() {}

} // namespace lf::startup
