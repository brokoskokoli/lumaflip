// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-09 (M4): set the current image as desktop picture - on the screen the window is on
// (else the main screen), or on all screens. The platform part (macOS: NSWorkspace
// setDesktopImageURL:forScreen:options:error:, app/Wallpaper_mac.mm) is behind WallpaperService; the screen
// choice and the error handling are here (testable with a fake service).

#include <QList>
#include <QString>
#include <QStringList>

namespace lf {

struct WallpaperScreen {
    QString id;   // stable for the session (macOS: NSScreenNumber)
    QString name; // localized display name
    bool isMain = false;
    bool hasWindow = false; // the viewer window is (mostly) on this screen
};

class WallpaperService {
public:
    virtual ~WallpaperService() = default;
    virtual QList<WallpaperScreen> screens() const = 0;
    virtual bool setDesktopImage(const QString& screenId, const QString& imagePath, QString* error) = 0;
};

namespace wallpaper {

enum class Target { WindowScreen, AllScreens };

// Screens to change: every screen for AllScreens; else the window's screen, the main screen
// or - if none is marked - the first one. Empty if there are no screens.
QStringList selectScreens(const QList<WallpaperScreen>& screens, Target target);

// Only real image files can become desktop pictures: no archive members or PDF pages
// (VirtualPath.h), not empty. Returns the user facing reason, empty if fine.
QString checkImage(const QString& path);

struct Result {
    int changed = 0;
    QStringList errors; // "<screen name>: <error>"
    bool ok() const { return changed > 0 && errors.isEmpty(); }
};
Result apply(WallpaperService& service, const QString& imagePath, Target target);

} // namespace wallpaper
} // namespace lf
