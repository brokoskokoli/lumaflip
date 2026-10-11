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
#include <QCoreApplication>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QWidget>
#include <QWindow>

#include <optional>
#include <utility>

namespace lf {

namespace {

// F-VIEW-14: the side of the window buttons in the desktop's own title bars. Runs on a
// worker thread.
TitleButtonSide readDesktopButtonSide() {
    constexpr TitleButtonSide fallback = TitleButtonSide::Right;
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    if (desktop.contains(QLatin1String("KDE"), Qt::CaseInsensitive)) {
        const QString path =
            QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("kwinrc"));
        if (path.isEmpty())
            return fallback;
        QSettings ini(path, QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("org.kde.kdecoration2"));
        return buttonSideFromKdeButtons(
            ini.value(QStringLiteral("ButtonsOnLeft"), QStringLiteral("MS")).toString(),
            ini.value(QStringLiteral("ButtonsOnRight"), QStringLiteral("HIAX")).toString(), fallback);
    }
    // GNOME and the desktops that share its setting (Cinnamon, Budgie, Pantheon …).
    QProcess p;
    p.start(QStringLiteral("gsettings"),
            {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.wm.preferences"),
             QStringLiteral("button-layout")});
    if (!p.waitForFinished(1500)) {
        p.kill();
        p.waitForFinished(500);
        return fallback;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return fallback;
    return buttonSideFromGnomeLayout(QString::fromUtf8(p.readAllStandardOutput()), fallback);
}

} // namespace

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

void setWindowMinimalChrome(QWidget* window, bool on) {
    // F-VIEW-13/14: no title bar - a frameless window; the title strip (TitleStrip) moves it
    // with QWindow::startSystemMove() and resizes it at the edges with startSystemResize()
    // (X11 and Wayland).
    QWidget* top = window ? window->window() : nullptr;
    if (!top || top->windowFlags().testFlag(Qt::FramelessWindowHint) == on)
        return;
    Qt::WindowFlags flags = top->windowFlags();
    flags.setFlag(Qt::FramelessWindowHint, on);
    if (QWindow* handle = top->windowHandle()) {
        // The native window stays (QWidget::setWindowFlags() would re-create it, and with
        // it the GPU surface): only the decoration hints change.
        top->overrideWindowFlags(flags);
        handle->setFlags(flags);
    } else {
        top->setWindowFlags(flags); // not created yet
    }
}

void setWindowButtonsVisible(QWidget*, bool, int) {}

bool titleStripUsesSystemButtons() {
    return false;
}

int titleStripHeight(QWidget*) {
    return 32;
}

int titleStripReservedLeft(QWidget*) {
    return 0;
}

void titleStripDoubleClick(QWidget* window) {
    QWidget* top = window ? window->window() : nullptr;
    if (top)
        top->isMaximized() ? top->showNormal() : top->showMaximized();
}

void detectTitleButtonSide(QObject* receiver, const std::function<void(TitleButtonSide)>& done) {
    // Read once per process on a worker thread (a file and possibly a gsettings process).
    static std::optional<TitleButtonSide> known;
    static QList<std::pair<QPointer<QObject>, std::function<void(TitleButtonSide)>>> waiting;
    static bool running = false;
    if (!done)
        return;
    if (known) {
        done(*known);
        return;
    }
    waiting.append({QPointer<QObject>(receiver), done});
    if (running)
        return;
    running = true;
    QThreadPool::globalInstance()->start([] {
        const TitleButtonSide side = readDesktopButtonSide();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [side] {
            known = side;
            running = false;
            const auto list = std::exchange(waiting, decltype(waiting)());
            for (const auto& [object, callback] : list)
                if (object)
                    callback(side);
        });
    });
}

void setTitleStripHitTest(QWidget*, std::function<TitleStripHit(QPoint)>) {}

NativeWindowState nativeWindowState(QWidget* window) {
    NativeWindowState s;
    if (!window || !window->windowHandle())
        return s;
    s.valid = true;
    s.floating = window->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
    // Frameless: the content fills the window and there is no (transparent or visible) title.
    const bool frameless = window->windowFlags().testFlag(Qt::FramelessWindowHint);
    s.fullSizeContent = s.titlebarTransparent = s.titleHidden = frameless;
    s.fullScreen = window->window()->isFullScreen();
    return s;
}

bool watchFullScreenTransitions(QWidget*, std::function<void(bool, bool)>) {
    return false; // not animated here: Qt's state is the window's state
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
