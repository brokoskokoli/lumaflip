// SPDX-License-Identifier: GPL-3.0-or-later
#include "Wallpaper.h"

#include "VirtualPath.h"

#include <QCoreApplication>

namespace lf::wallpaper {

namespace {
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Wallpaper)
};
} // namespace

QStringList selectScreens(const QList<WallpaperScreen>& screens, Target target) {
    QStringList out;
    if (screens.isEmpty())
        return out;
    if (target == Target::AllScreens) {
        for (const WallpaperScreen& s : screens)
            out << s.id;
        return out;
    }
    for (const WallpaperScreen& s : screens)
        if (s.hasWindow)
            return {s.id};
    for (const WallpaperScreen& s : screens)
        if (s.isMain)
            return {s.id};
    return {screens.first().id};
}

QString checkImage(const QString& path) {
    if (path.isEmpty())
        return Tr::tr("Es ist kein Bild geöffnet.");
    if (vpath::isMember(path))
        return Tr::tr(
            "Bilder in Archiven und PDF-Seiten können nicht als Schreibtischhintergrund festgelegt werden.");
    return {};
}

Result apply(WallpaperService& service, const QString& imagePath, Target target) {
    Result r;
    if (const QString problem = checkImage(imagePath); !problem.isEmpty()) {
        r.errors << problem;
        return r;
    }
    const QList<WallpaperScreen> screens = service.screens();
    const QStringList ids = selectScreens(screens, target);
    if (ids.isEmpty()) {
        r.errors << Tr::tr("Es wurde kein Bildschirm gefunden.");
        return r;
    }
    for (const QString& id : ids) {
        QString error;
        if (service.setDesktopImage(id, imagePath, &error)) {
            ++r.changed;
            continue;
        }
        QString name = id;
        for (const WallpaperScreen& s : screens)
            if (s.id == id && !s.name.isEmpty())
                name = s.name;
        //: wallpaper error per screen: "<screen>: <reason>"
        r.errors << Tr::tr("%1: %2").arg(name, error);
    }
    return r;
}

} // namespace lf::wallpaper
