// SPDX-License-Identifier: GPL-3.0-or-later
// F-FILE-09 on Linux desktops (core/Wallpaper.h): there is no common API, so the desktop
// environment's own tool sets the picture for all screens:
//   - GNOME, Unity, Budgie, Pantheon: gsettings org.gnome.desktop.background picture-uri
//     (and picture-uri-dark);
//   - Cinnamon: org.cinnamon.desktop.background; MATE: org.mate.background;
//   - KDE Plasma: plasma-apply-wallpaperimage;
//   - XFCE: xfconf-query is per monitor/workspace and not handled.
// Elsewhere the menu entry reports that the desktop is not supported.

#include "app/MacIntegration.h"
#include "core/Wallpaper.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QWidget>

namespace lf::macint {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Wallpaper)
};

bool run(const QString& program, const QStringList& args, QString* error) {
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        if (error)
            *error = Tr::tr("„%1“ ist nicht installiert.").arg(program);
        return false;
    }
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(10000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error)
            *error = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
        if (error && error->isEmpty())
            *error = Tr::tr("„%1“ ist fehlgeschlagen.").arg(program);
        return false;
    }
    return true;
}

class DesktopWallpaperService final : public WallpaperService {
public:
    QList<WallpaperScreen> screens() const override {
        // The desktop tools set one picture for every screen.
        WallpaperScreen s;
        s.id = QStringLiteral("all");
        s.name = Tr::tr("Alle Bildschirme");
        s.isMain = true;
        s.hasWindow = true;
        return {s};
    }

    bool setDesktopImage(const QString&, const QString& imagePath, QString* error) override {
        const QString path = QFileInfo(imagePath).absoluteFilePath();
        const QString uri = QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
        const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
        if (desktop.contains(QLatin1String("KDE")))
            return run(QStringLiteral("plasma-apply-wallpaperimage"), {path}, error);
        if (desktop.contains(QLatin1String("CINNAMON")))
            return run(QStringLiteral("gsettings"),
                       {QStringLiteral("set"), QStringLiteral("org.cinnamon.desktop.background"),
                        QStringLiteral("picture-uri"), uri},
                       error);
        if (desktop.contains(QLatin1String("MATE")))
            return run(QStringLiteral("gsettings"),
                       {QStringLiteral("set"), QStringLiteral("org.mate.background"),
                        QStringLiteral("picture-filename"), path},
                       error);
        if (desktop.contains(QLatin1String("GNOME")) || desktop.contains(QLatin1String("UNITY")) ||
            desktop.contains(QLatin1String("BUDGIE")) || desktop.contains(QLatin1String("PANTHEON"))) {
            const QString schema = QStringLiteral("org.gnome.desktop.background");
            if (!run(QStringLiteral("gsettings"),
                     {QStringLiteral("set"), schema, QStringLiteral("picture-uri"), uri}, error))
                return false;
            // Dark style (GNOME 42+); older versions do not have the key.
            QString ignored;
            run(QStringLiteral("gsettings"),
                {QStringLiteral("set"), schema, QStringLiteral("picture-uri-dark"), uri}, &ignored);
            return true;
        }
        if (error)
            *error = Tr::tr("Diese Arbeitsumgebung wird nicht unterstützt.");
        return false;
    }
};

} // namespace

std::unique_ptr<WallpaperService> createWallpaperService(QWidget*) {
    return std::make_unique<DesktopWallpaperService>();
}

} // namespace lf::macint
