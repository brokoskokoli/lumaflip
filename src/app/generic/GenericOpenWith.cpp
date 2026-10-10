// SPDX-License-Identifier: GPL-3.0-or-later
// F-FILE-05 on freedesktop systems: the applications for a file come from the installed
// .desktop files and mimeapps.list (platform/generic/XdgDesktop.h). AppEntry::path is the
// .desktop file (Shell::openWith() launches it), bundleId the desktop file id without
// ".desktop". Runs on worker threads: icons are read as image files (no QIcon theme lookup,
// which needs the GUI thread).

#include "app/OpenWith.h"
#include "platform/generic/XdgDesktop.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>

namespace lf {

namespace {

constexpr int kIconPx = 32;

QString uiLocale() {
    return QLocale().name();
}

// hicolor/pixmaps lookup of a theme icon name (good enough for menu icons).
QImage loadIcon(const QString& icon) {
    if (icon.isEmpty())
        return {};
    QStringList candidates;
    if (QDir::isAbsolutePath(icon)) {
        candidates << icon;
    } else {
        QStringList dirs{xdg::dataHome()};
        dirs << xdg::dataDirs();
        for (const QString& d : std::as_const(dirs)) {
            for (const char* size : {"32x32", "48x48", "64x64", "128x128", "256x256", "24x24", "16x16"})
                candidates
                    << QStringLiteral("%1/icons/hicolor/%2/apps/%3.png").arg(d, QLatin1String(size), icon);
            candidates << QStringLiteral("%1/icons/hicolor/scalable/apps/%2.svg").arg(d, icon);
            candidates << QStringLiteral("%1/pixmaps/%2.png").arg(d, icon);
            candidates << QStringLiteral("%1/pixmaps/%2.svg").arg(d, icon);
        }
    }
    for (const QString& path : std::as_const(candidates)) {
        if (!QFileInfo::exists(path))
            continue;
        QImageReader r(path);
        r.setScaledSize(QSize(kIconPx, kIconPx));
        const QImage img = r.read();
        if (!img.isNull()) {
            QImage out = img;
            out.setDevicePixelRatio(2.0);
            return out;
        }
    }
    return {};
}

AppEntry entryFor(const xdg::DesktopEntry& e, bool isDefault) {
    AppEntry a;
    a.name = e.name;
    a.path = e.path;
    a.bundleId = e.id.endsWith(QLatin1String(".desktop")) ? e.id.chopped(8) : e.id;
    a.icon = loadIcon(e.icon);
    a.isDefault = isDefault;
    return a;
}

} // namespace

QList<AppEntry> queryApplicationsForFile(const QString& filePath) {
    const QMimeType type = QMimeDatabase().mimeTypeForFile(filePath);
    if (!type.isValid())
        return {};
    const QString def = xdg::defaultApplication(type.name());
    QList<AppEntry> out;
    for (const xdg::DesktopEntry& e : xdg::applicationsForMimeType(type.name(), uiLocale()))
        out << entryFor(e, e.id == def);
    return out;
}

AppEntry applicationInfo(const QString& appPath) {
    if (appPath.isEmpty())
        return {};
    if (appPath.endsWith(QLatin1String(".desktop"))) {
        if (const auto e = xdg::parseDesktopFile(appPath, QString(), uiLocale()))
            return entryFor(*e, false);
    }
    AppEntry a; // a plain executable
    a.path = appPath;
    a.name = QFileInfo(appPath).fileName();
    return a;
}

QString ownBundlePath() {
    // The installed desktop entry plays the role of the bundle (OpenWith.cpp leaves it out).
    if (const auto e = xdg::findApplication(QStringLiteral("io.github.brokoskokoli.lumaflip.desktop")))
        return e->path;
    return {};
}

QString ownBundleId() {
    return QString::fromLatin1(kLumaflipBundleId);
}

} // namespace lf
