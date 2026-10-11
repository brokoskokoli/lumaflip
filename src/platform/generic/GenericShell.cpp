// SPDX-License-Identifier: GPL-3.0-or-later
// Shell integration for freedesktop systems (Linux; Platform.h):
//   - reveal: org.freedesktop.FileManager1.ShowItems over D-Bus (Nautilus, Dolphin, Nemo,
//     Caja, Thunar …), else the folder is opened with the default file manager;
//   - open with: the application's .desktop file (XdgDesktop.h), or an executable;
//   - default viewer (F-MAC-04 counterpart): $XDG_CONFIG_HOME/mimeapps.list, like
//     `xdg-mime default`. An AppImage first installs a desktop entry for itself into
//     $XDG_DATA_HOME/applications (pointing at $APPIMAGE), so there is something to
//     associate.
//   - default app query (F-MAC-09): the file's MIME type (QMimeDatabase) and its default
//     application from mimeapps.list compared with the app's desktop file id.

#include "XdgDesktop.h"
#include "core/DefaultAppOffer.h"
#include "core/Platform.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>

#if LF_HAVE_QTDBUS
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#endif

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Shell)
};

constexpr auto kDesktopId = "io.github.brokoskokoli.lumaflip.desktop";

// The MIME types the app registers for (packaging/linux/*.desktop has the same list).
QStringList imageMimeTypes() {
    return {QStringLiteral("image/jpeg"),
            QStringLiteral("image/png"),
            QStringLiteral("image/gif"),
            QStringLiteral("image/tiff"),
            QStringLiteral("image/bmp"),
            QStringLiteral("image/webp"),
            QStringLiteral("image/heic"),
            QStringLiteral("image/heif"),
            QStringLiteral("image/avif"),
            QStringLiteral("image/jxl"),
            QStringLiteral("image/svg+xml"),
            QStringLiteral("image/x-exr"),
            QStringLiteral("image/x-portable-anymap"),
            QStringLiteral("image/x-qoi"),
            QStringLiteral("image/x-xcf"),
            QStringLiteral("image/vnd.adobe.photoshop"),
            QStringLiteral("image/x-dcraw"),
            QStringLiteral("image/x-canon-cr2"),
            QStringLiteral("image/x-canon-cr3"),
            QStringLiteral("image/x-nikon-nef"),
            QStringLiteral("image/x-sony-arw"),
            QStringLiteral("image/x-adobe-dng"),
            QStringLiteral("image/x-fuji-raf"),
            QStringLiteral("image/x-olympus-orf"),
            QStringLiteral("image/x-panasonic-rw2")};
}

// F-OPEN-10: archives and PDF documents, offered in "Open with" only (never made default).
QStringList containerMimeTypes() {
    return {QStringLiteral("application/vnd.comicbook+zip"),
            QStringLiteral("application/vnd.comicbook-rar"),
            QStringLiteral("application/zip"),
            QStringLiteral("application/vnd.rar"),
            QStringLiteral("application/x-rar"),
            QStringLiteral("application/x-rar-compressed"),
            QStringLiteral("application/x-7z-compressed"),
            QStringLiteral("application/x-cb7"),
            QStringLiteral("application/pdf")};
}

void openContainingFolder(const QString& path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

#if LF_HAVE_QTDBUS
// Entirely asynchronous - no blocking D-Bus round trip on the GUI thread (the bus starts an
// activatable file manager by itself, which may take a moment): an error reply (no file
// manager service, or it failed) opens the folder instead.
void showItemsOverDbus(const QString& path) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        openContainingFolder(path);
        return;
    }
    const QString service = QStringLiteral("org.freedesktop.FileManager1");
    QDBusMessage call = QDBusMessage::createMethodCall(
        service, QStringLiteral("/org/freedesktop/FileManager1"), service, QStringLiteral("ShowItems"));
    call << QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)} << QString();
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 15000), QCoreApplication::instance());
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
                     [path](QDBusPendingCallWatcher* w) {
                         if (w->isError())
                             openContainingFolder(path);
                         w->deleteLater();
                     });
}
#endif

// AppImage: a desktop entry for this very image, so it can become the default viewer.
bool ensureAppImageDesktopEntry(QString* error) {
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (appImage.isEmpty() || !QFileInfo(appImage).isFile())
        return true;
    const QString dir = xdg::dataHome() + QStringLiteral("/applications");
    QDir().mkpath(dir);
    QSaveFile f(dir + QLatin1Char('/') + QLatin1String(kDesktopId));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht geschrieben werden.").arg(f.fileName());
        return false;
    }
    QString quoted = appImage;
    // Exec quoting inside a string value: \ " $ ` escaped for both levels, % doubled.
    quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\\\\\"))
        .replace(QLatin1Char('"'), QStringLiteral("\\\\\""))
        .replace(QLatin1Char('$'), QStringLiteral("\\\\$"))
        .replace(QLatin1Char('`'), QStringLiteral("\\\\`"))
        .replace(QLatin1Char('%'), QStringLiteral("%%"));
    const QString content =
        QStringLiteral("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=Lumaflip\n"
                       "Exec=\"%1\" %F\n"
                       "Icon=io.github.brokoskokoli.lumaflip\n"
                       "Terminal=false\n"
                       "Categories=Graphics;Viewer;\n"
                       "MimeType=%2;\n"
                       "X-AppImage-Path=%3\n")
            .arg(quoted, (imageMimeTypes() + containerMimeTypes()).join(QLatin1Char(';')), appImage);
    f.write(content.toUtf8());
    if (!f.commit()) {
        if (error)
            *error = Tr::tr("„%1“ kann nicht geschrieben werden.").arg(f.fileName());
        return false;
    }
    return true;
}

class XdgShell final : public Shell {
public:
    void revealInFileManager(const QString& path) override {
#if LF_HAVE_QTDBUS
        showItemsOverDbus(QFileInfo(path).absoluteFilePath());
#else
        openContainingFolder(path); // no D-Bus: the containing folder
#endif
    }

    void openWith(const QString& path, const QString& appPath) override {
        if (appPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
        if (appPath.endsWith(QLatin1String(".desktop"))) {
            if (const auto e = xdg::parseDesktopFile(appPath)) {
                xdg::launch(*e, {path});
                return;
            }
        }
        QProcess::startDetached(appPath, {path});
    }

    QStringList imageDocumentTypes() const override { return imageMimeTypes(); }

    // F-MAC-09 (worker thread): only the types the desktop entry lists (or their subtypes,
    // e.g. image/x-canon-cr2 below image/x-dcraw).
    DefaultViewerStatus defaultViewerStatus(const QString& filePath) override {
        DefaultViewerStatus status;
        const QMimeType mime = QMimeDatabase().mimeTypeForFile(filePath);
        if (!mime.isValid() || mime.isDefault())
            return status;
        bool ours = false;
        for (const QString& t : imageMimeTypes()) {
            if (mime.inherits(t)) {
                ours = true;
                break;
            }
        }
        if (!ours)
            return status;
        status.type = mime.name();
        status.typeName = mime.comment().isEmpty() ? mime.name() : mime.comment();
        status.state = xdg::defaultApplication(mime.name()) == QLatin1String(kDesktopId)
                           ? DefaultViewerStatus::State::IsDefault
                           : DefaultViewerStatus::State::NotDefault;
        return status;
    }

    // F-MAC-09: not from a build tree; an AppImage or an installed desktop entry.
    bool isInstalledApp() override {
        if (defaultapp::isInBuildTree(QCoreApplication::applicationDirPath()))
            return false;
        const QString appImage = qEnvironmentVariable("APPIMAGE");
        if (!appImage.isEmpty() && QFileInfo(appImage).isFile())
            return true;
        return xdg::findApplication(QLatin1String(kDesktopId)).has_value();
    }

    bool makeDefaultViewer(const QStringList& types, QString* error) override {
        if (!ensureAppImageDesktopEntry(error))
            return false;
        if (!xdg::findApplication(QLatin1String(kDesktopId))) {
            if (error)
                *error = Tr::tr("Lumaflip ist nicht als Anwendung installiert (%1 fehlt).")
                             .arg(QLatin1String(kDesktopId));
            return false;
        }
        return xdg::setDefaultApplication(QLatin1String(kDesktopId),
                                          types.isEmpty() ? imageMimeTypes() : types, error);
    }
};

} // namespace

std::unique_ptr<Shell> createShell() {
    return std::make_unique<XdgShell>();
}

bool systemRestoresWindows() {
    return false; // like the macOS default
}

QStringList appLanguagePreference(const QString&) {
    return {}; // no per-app language setting outside macOS
}

bool setAppKitLanguage(const QString&, const QString&) {
    return true; // no AppKit outside macOS
}

} // namespace lf
