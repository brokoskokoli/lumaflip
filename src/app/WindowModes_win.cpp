// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-12 (always on top) and F-VIEW-13/15 (borderless window with the title strip) on
// the HWND behind a Qt top-level. Nothing here changes Qt's window flags: setWindowFlags()
// would destroy and re-create the native window, and with it the QRhi swapchain.
//
// The borderless window keeps its normal styles (WS_CAPTION, WS_THICKFRAME, the system
// menu and the min/max boxes): Aero Snap, Win+arrow keys, the taskbar menu and the window
// animations stay. A native event filter takes the caption out of the frame instead
// (WM_NCCALCSIZE: the client area reaches the top edge, the invisible resize borders on
// the left, right and bottom stay) and answers WM_NCHITTEST for the top: the resize border,
// the caption of the strip (HTCAPTION: move, Snap, double click maximizes, right click
// opens the system menu) and its maximize button (HTMAXBUTTON: Windows 11 shows the Snap
// Layouts on hover). Qt reads the frame margins from the WM_NCCALCSIZE result, so its
// geometry follows.
#include "WindowModes.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <QWindow>

#include <windows.h>
#include <windowsx.h>

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

struct StripFrame {
    QPointer<QWidget> window;
    bool captionRemoved = false;
    std::function<TitleStripHit(QPoint)> hitTest;
};

int resizeBorder(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

class StripFrameFilter : public QAbstractNativeEventFilter {
public:
    static StripFrameFilter& instance() {
        static auto* filter = [] {
            auto* f = new StripFrameFilter; // lives as long as the application
            QCoreApplication::instance()->installNativeEventFilter(f);
            return f;
        }();
        return *filter;
    }

    StripFrame& frame(HWND hwnd, QWidget* window) {
        StripFrame& f = m_frames[hwnd];
        if (!f.window) {
            f.window = window;
            QObject::connect(window, &QObject::destroyed, [hwnd] { instance().m_frames.remove(hwnd); });
        }
        return f;
    }
    const StripFrame* find(HWND hwnd) const {
        auto it = m_frames.constFind(hwnd);
        return it == m_frames.cend() || !it->window ? nullptr : &*it;
    }

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override {
        if (type != "windows_generic_MSG")
            return false;
        MSG* msg = static_cast<MSG*>(message);
        const StripFrame* f = find(msg->hwnd);
        if (!f || !f->captionRemoved)
            return false;
        switch (msg->message) {
        case WM_NCCALCSIZE:
            return calcSize(*msg, result);
        case WM_NCHITTEST:
            return hitTest(*f, *msg, result);
        case WM_NCMOUSEMOVE:
            if (msg->wParam == HTMAXBUTTON || msg->wParam == HTCAPTION) {
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, msg->hwnd, 0};
                TrackMouseEvent(&track);
            }
            return false;
        case WM_NCMOUSELEAVE:
            if (f->hitTest)
                f->hitTest(QPoint(-1, -1));
            return false;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            if (msg->wParam != HTMAXBUTTON)
                return false;
            *result = 0; // the strip draws the button; the click happens on release
            return true;
        case WM_NCLBUTTONUP:
            if (msg->wParam != HTMAXBUTTON)
                return false;
            ShowWindow(msg->hwnd, IsZoomed(msg->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            *result = 0;
            return true;
        default:
            return false;
        }
    }

private:
    static bool calcSize(const MSG& msg, qintptr* result) {
        if (!msg.wParam)
            return false;
        auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg.lParam);
        const RECT before = params->rgrc[0];
        const LRESULT r = DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
        // Left, right and bottom keep the (invisible) resize borders; the caption and the
        // top border go. Maximized, the frame hangs over the screen edge: keep that part.
        params->rgrc[0].top = before.top + (IsZoomed(msg.hwnd) ? params->rgrc[0].left - before.left : 0);
        *result = r;
        return true;
    }

    static bool hitTest(const StripFrame& f, const MSG& msg, qintptr* result) {
        const LRESULT def = DefWindowProcW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
        if (def != HTCLIENT) {
            *result = def; // the borders on the left, right and bottom
            return true;
        }
        POINT p{GET_X_LPARAM(msg.lParam), GET_Y_LPARAM(msg.lParam)};
        RECT win;
        GetWindowRect(msg.hwnd, &win);
        const LONG_PTR style = GetWindowLongPtrW(msg.hwnd, GWL_STYLE);
        const int border = resizeBorder(msg.hwnd);
        if ((style & WS_THICKFRAME) && !IsZoomed(msg.hwnd) && p.y < win.top + border) {
            if (p.x < win.left + border * 2)
                *result = HTTOPLEFT;
            else if (p.x >= win.right - border * 2)
                *result = HTTOPRIGHT;
            else
                *result = HTTOP;
            return true;
        }
        *result = HTCLIENT;
        if (!f.hitTest)
            return true;
        ScreenToClient(msg.hwnd, &p);
        const qreal dpr = f.window->devicePixelRatioF();
        switch (f.hitTest(QPoint(int(p.x / dpr), int(p.y / dpr)))) {
        case TitleStripHit::Caption:
            *result = HTCAPTION;
            break;
        case TitleStripHit::Maximize:
            *result = HTMAXBUTTON;
            break;
        case TitleStripHit::Client:
            break;
        }
        return true;
    }

    QHash<HWND, StripFrame> m_frames;
};

void frameChanged(HWND w) {
    SetWindowPos(w, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
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
    StripFrame& f = StripFrameFilter::instance().frame(w, window->window());
    if (f.captionRemoved == on)
        return;
    f.captionRemoved = on;
    frameChanged(w); // WM_NCCALCSIZE again
}

void setWindowButtonsVisible(QWidget*, bool, int) {}

bool titleStripUsesSystemButtons() {
    return false;
}

int titleStripHeight(QWidget*) {
    return 32; // Windows 11 caption height (logical px)
}

int titleStripReservedLeft(QWidget*) {
    return 0;
}

void titleStripDoubleClick(QWidget* window) {
    // Only reached for the title text area handled by Qt (the caption itself is native).
    QWidget* top = window ? window->window() : nullptr;
    if (top)
        top->isMaximized() ? top->showNormal() : top->showMaximized();
}

void detectTitleButtonSide(QObject*, const std::function<void(TitleButtonSide)>& done) {
    if (done)
        done(TitleButtonSide::Right);
}

void setTitleStripHitTest(QWidget* window, const std::function<TitleStripHit(QPoint)>& hitTest) {
    HWND w = nativeWindow(window, true);
    if (!w)
        return;
    StripFrameFilter::instance().frame(w, window->window()).hitTest = hitTest;
}

NativeWindowState nativeWindowState(QWidget* window) {
    NativeWindowState s;
    HWND w = nativeWindow(window, false);
    if (!w)
        return s;
    s.valid = true;
    s.floating = (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const StripFrame* f = StripFrameFilter::instance().find(w);
    s.captionRemoved = f && f->captionRemoved;
    // The client area reaches the top edge: like a full size content view without title.
    RECT win;
    GetWindowRect(w, &win);
    POINT origin{0, 0};
    ClientToScreen(w, &origin);
    const bool clientAtTop = origin.y <= win.top + 1 || IsZoomed(w);
    s.fullSizeContent = s.captionRemoved && clientAtTop;
    s.titlebarTransparent = s.captionRemoved;
    s.titleHidden = s.captionRemoved;
    s.fullScreen = window->window()->isFullScreen();
    return s;
}

bool watchFullScreenTransitions(QWidget*, std::function<void(bool, bool)>) {
    return false; // not animated: Qt's state is the window's state
}

} // namespace lf
