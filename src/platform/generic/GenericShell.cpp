// SPDX-License-Identifier: GPL-3.0-or-later
// Portable shell integration via QDesktopServices / QProcess.

#include "core/Platform.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QUrl>

namespace lf {

namespace {

class GenericShell final : public Shell {
public:
    void revealInFileManager(const QString& path) override {
        // No portable "select file" call: open the containing folder.
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    }

    void openWith(const QString& path, const QString& appPath) override {
        if (appPath.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        else
            QProcess::startDetached(appPath, {path});
    }

    QStringList imageDocumentTypes() const override {
        return {"image/jpeg", "image/png",  "image/gif",  "image/tiff", "image/bmp",     "image/webp",
                "image/heic", "image/heif", "image/avif", "image/jxl",  "image/svg+xml", "image/x-exr"};
    }

    bool makeDefaultViewer(const QStringList& /*types*/, QString* error) override {
        if (error)
            *error = QCoreApplication::translate("lf::Shell",
                                                 "Auf dieser Plattform wird das Festlegen der Standard-App "
                                                 "noch nicht unterstützt.");
        return false;
    }
};

} // namespace

std::unique_ptr<Shell> createShell() {
    return std::make_unique<GenericShell>();
}

bool systemRestoresWindows() {
    return false; // like the macOS default
}

QStringList appLanguagePreference() {
    return {}; // no per-app language setting outside macOS
}

} // namespace lf
