// SPDX-License-Identifier: GPL-3.0-or-later
// Windows counterparts of the macOS integration (MacIntegration.h):
//  - Quick Look and the Services menu do not exist (the menu items are hidden: the toggle
//    reports "not available").
//  - Desktop picture (F-FILE-09): IDesktopWallpaper per monitor; SPI_SETDESKWALLPAPER as
//    fallback (all monitors).
#include "MacIntegration.h"

#include "core/Wallpaper.h"

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QWidget>
#include <QWindow>

#include <windows.h>

#include <shobjidl.h>
#include <wrl/client.h>

namespace lf::macint {

bool quickLookToggle(QWidget*, const QString&, std::function<void(int)>) {
    return false;
}

void quickLookFollow(QWidget*, const QString&) {}

bool quickLookVisible() {
    return false;
}

void quickLookClose() {}

void installServiceResponder(QWidget*, ServiceContext, ServiceImage) {}

void setServicesProvider(std::function<void(const QStringList& files)>) {}

void registerServicesSendTypes() {}

namespace {

using Microsoft::WRL::ComPtr;

QString fromCoString(LPWSTR s) {
    QString out = s ? QString::fromWCharArray(s) : QString();
    CoTaskMemFree(s);
    return out;
}

class WinWallpaperService final : public WallpaperService {
public:
    explicit WinWallpaperService(QWidget* window) : m_window(window) {}

    QList<WallpaperScreen> screens() const override {
        QList<WallpaperScreen> out;
        ComPtr<IDesktopWallpaper> wp;
        if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wp)))) {
            out << WallpaperScreen{QStringLiteral("*"),
                                   QCoreApplication::translate("lf::MacIntegration", "Alle Bildschirme"),
                                   true, true};
            return out;
        }
        HMONITOR windowMonitor = nullptr;
        if (m_window && m_window->window()->windowHandle())
            windowMonitor = MonitorFromWindow(reinterpret_cast<HWND>(m_window->window()->winId()),
                                              MONITOR_DEFAULTTONEAREST);
        UINT count = 0;
        wp->GetMonitorDevicePathCount(&count);
        for (UINT i = 0; i < count; ++i) {
            LPWSTR id = nullptr;
            if (FAILED(wp->GetMonitorDevicePathAt(i, &id)) || !id)
                continue;
            const QString monitorId = fromCoString(id);
            RECT r{};
            if (FAILED(wp->GetMonitorRECT(reinterpret_cast<LPCWSTR>(monitorId.utf16()), &r)))
                continue; // not attached
            WallpaperScreen s;
            s.id = monitorId;
            const HMONITOR m = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            s.isMain = r.left == 0 && r.top == 0;
            s.hasWindow = m && m == windowMonitor;
            s.name = QCoreApplication::translate("lf::MacIntegration", "Bildschirm %1").arg(out.size() + 1);
            for (QScreen* screen : QGuiApplication::screens()) {
                const QRect g = screen->geometry();
                const qreal dpr = screen->devicePixelRatio();
                if (qRound(g.x() * dpr) == r.left && qRound(g.y() * dpr) == r.top)
                    s.name = screen->name();
            }
            out << s;
        }
        return out;
    }

    bool setDesktopImage(const QString& screenId, const QString& imagePath, QString* error) override {
        const QString native = QDir::toNativeSeparators(imagePath);
        if (screenId == QLatin1String("*")) {
            if (SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
                                      const_cast<wchar_t*>(reinterpret_cast<const wchar_t*>(native.utf16())),
                                      SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
                return true;
        } else {
            ComPtr<IDesktopWallpaper> wp;
            if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wp))) &&
                SUCCEEDED(wp->SetWallpaper(reinterpret_cast<LPCWSTR>(screenId.utf16()),
                                           reinterpret_cast<LPCWSTR>(native.utf16()))))
                return true;
        }
        if (error)
            *error = QCoreApplication::translate("lf::MacIntegration",
                                                 "Windows hat das Bild nicht als Hintergrund übernommen.");
        return false;
    }

private:
    QPointer<QWidget> m_window;
};

} // namespace

std::unique_ptr<WallpaperService> createWallpaperService(QWidget* window) {
    return std::make_unique<WinWallpaperService>(window);
}

} // namespace lf::macint
