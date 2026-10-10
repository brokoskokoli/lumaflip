// SPDX-License-Identifier: GPL-3.0-or-later
// Shell integration on Windows (Platform.h: Shell):
//  - reveal: Explorer window of the folder with the file selected (SHOpenFolderAndSelectItems);
//  - open with: the handlers Windows recommends for the type (SHAssocEnumHandlers), invoked
//    like Explorer's "Open with" does; a program chosen by path is started with
//    CreateProcess and a correctly quoted command line (core/CommandLine.h), never via cmd;
//  - default app: Windows does not let programs make themselves the default (since
//    Windows 8 the choice is protected by a hash only the Settings app writes). Lumaflip
//    registers as a capable program (installer: RegisteredApplications/Capabilities) and
//    opens Settings → Apps → Default apps at its own page.

#include "core/CommandLine.h"
#include "core/Platform.h"
#include "platform/windows/WinCom.h"
#include "platform/windows/WinShellUtil.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <vector>

namespace lf {

using Microsoft::WRL::ComPtr;

namespace win {

namespace {

QString takeCoString(LPWSTR s) {
    QString out = s ? QString::fromWCharArray(s) : QString();
    CoTaskMemFree(s);
    return out;
}

QImage iconImage(const QString& location, int index, int px) {
    if (location.isEmpty())
        return {};
    HICON large = nullptr;
    if (FAILED(SHDefExtractIconW(wstr(location), index, 0, &large, nullptr, MAKELONG(px, px))) || !large)
        return {};
    QImage img = QImage::fromHICON(large);
    DestroyIcon(large);
    return img;
}

QString extensionOf(const QString& filePath) {
    const QString suffix = QFileInfo(filePath).suffix();
    return suffix.isEmpty() ? QString() : u'.' + suffix.toLower();
}

QString defaultExecutable(const QString& ext) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = DWORD(std::size(buf));
    if (FAILED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, wstr(ext), nullptr, buf,
                                 &size)))
        return {};
    return QString::fromWCharArray(buf);
}

bool startProcess(const QString& program, const QStringList& args, QString* error) {
    if (cmdline::isBatchFile(program)) {
        if (error)
            *error = QCoreApplication::translate(
                "lf::Shell", "Batch-Dateien werden aus Sicherheitsgründen nicht gestartet.");
        return false;
    }
    QString line = cmdline::build(QDir::toNativeSeparators(program), args);
    std::vector<wchar_t> buffer(size_t(line.size()) + 1, 0);
    line.toWCharArray(buffer.data());
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const QString native = QDir::toNativeSeparators(program);
    if (!CreateProcessW(wstr(native), buffer.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si, &pi)) {
        if (error)
            *error = errorText(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

QString ownExecutablePath() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    return n ? QDir::fromNativeSeparators(QString::fromWCharArray(buf.data(), int(n))) : QString();
}

QList<AssocHandler> assocHandlers(const QString& filePath, int iconPx) {
    QList<AssocHandler> out;
    const QString ext = extensionOf(filePath);
    if (ext.isEmpty())
        return out;
    ComScope com;
    ComPtr<IEnumAssocHandlers> handlers;
    if (FAILED(SHAssocEnumHandlers(wstr(ext), ASSOC_FILTER_RECOMMENDED, &handlers)) || !handlers)
        return out;
    const QString def = QDir::cleanPath(QDir::fromNativeSeparators(defaultExecutable(ext)));
    ComPtr<IAssocHandler> h;
    ULONG fetched = 0;
    while (handlers->Next(1, &h, &fetched) == S_OK && fetched == 1) {
        AssocHandler e;
        LPWSTR s = nullptr;
        if (SUCCEEDED(h->GetName(&s)))
            e.id = takeCoString(s);
        s = nullptr;
        if (SUCCEEDED(h->GetUIName(&s)))
            e.name = takeCoString(s);
        s = nullptr;
        int index = 0;
        if (SUCCEEDED(h->GetIconLocation(&s, &index)))
            e.icon = iconImage(takeCoString(s), index, iconPx);
        if (e.name.isEmpty())
            e.name = QFileInfo(e.id).completeBaseName();
        e.isDefault =
            !def.isEmpty() &&
            QDir::cleanPath(QDir::fromNativeSeparators(e.id)).compare(def, Qt::CaseInsensitive) == 0;
        if (!e.id.isEmpty()) {
            if (e.isDefault)
                out.prepend(e);
            else
                out << e;
        }
        h.Reset();
    }
    return out;
}

bool openWithHandler(const QString& filePath, const QString& id, QString* error) {
    const QString native = QDir::toNativeSeparators(QFileInfo(filePath).absoluteFilePath());
    const QString ext = extensionOf(filePath);
    ComScope com(COINIT_APARTMENTTHREADED);
    ComPtr<IEnumAssocHandlers> handlers;
    if (!ext.isEmpty() && SUCCEEDED(SHAssocEnumHandlers(wstr(ext), ASSOC_FILTER_NONE, &handlers)) &&
        handlers) {
        ComPtr<IAssocHandler> h;
        ULONG fetched = 0;
        while (handlers->Next(1, &h, &fetched) == S_OK && fetched == 1) {
            LPWSTR s = nullptr;
            const QString name = SUCCEEDED(h->GetName(&s)) ? takeCoString(s) : QString();
            if (name.compare(id, Qt::CaseInsensitive) == 0 ||
                QDir::cleanPath(QDir::fromNativeSeparators(name))
                        .compare(QDir::cleanPath(QDir::fromNativeSeparators(id)), Qt::CaseInsensitive) == 0) {
                ComPtr<IShellItem> item;
                ComPtr<IDataObject> data;
                if (SUCCEEDED(SHCreateItemFromParsingName(wstr(native), nullptr, IID_PPV_ARGS(&item))) &&
                    SUCCEEDED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) &&
                    SUCCEEDED(h->Invoke(data.Get())))
                    return true;
                break;
            }
            h.Reset();
        }
    }
    if (id.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) && QFileInfo(id).isFile())
        return startProcess(id, {native}, error);
    if (error)
        *error = QCoreApplication::translate("lf::Shell", "Das Programm wurde nicht gefunden.");
    return false;
}

AssocHandler programInfo(const QString& exePath, int iconPx) {
    AssocHandler e;
    e.id = exePath;
    const QString native = QDir::toNativeSeparators(exePath);
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(wstr(native), &dummy);
    if (size) {
        std::vector<char> data(size);
        if (GetFileVersionInfoW(wstr(native), 0, size, data.data())) {
            struct Translation {
                WORD language;
                WORD codePage;
            }* tr = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr),
                               &len) &&
                len >= sizeof(Translation)) {
                const QString key = QStringLiteral("\\StringFileInfo\\%1%2\\FileDescription")
                                        .arg(tr->language, 4, 16, QLatin1Char('0'))
                                        .arg(tr->codePage, 4, 16, QLatin1Char('0'));
                wchar_t* value = nullptr;
                if (VerQueryValueW(data.data(), wstr(key), reinterpret_cast<void**>(&value), &len) && len > 1)
                    e.name = QString::fromWCharArray(value).trimmed();
            }
        }
    }
    if (e.name.isEmpty())
        e.name = QFileInfo(exePath).completeBaseName();
    e.icon = iconImage(native, 0, iconPx);
    return e;
}

} // namespace win

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::Shell)
};

class WinShell final : public Shell {
public:
    void revealInFileManager(const QString& path) override {
        const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        win::ComScope com(COINIT_APARTMENTTHREADED);
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(win::wstr(native), nullptr, &pidl, 0, nullptr)) && pidl) {
            const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            CoTaskMemFree(pidl);
            if (SUCCEEDED(hr))
                return;
        }
        // Fallback: the folder itself.
        const QString folder = QDir::toNativeSeparators(QFileInfo(path).absolutePath());
        ShellExecuteW(nullptr, L"open", win::wstr(folder), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void openWith(const QString& path, const QString& appPath) override {
        const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
        if (appPath.isEmpty()) {
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof info;
            info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            info.lpVerb = nullptr; // the default verb
            info.lpFile = win::wstr(native);
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info)) // no program for the type: Windows' "Open with" dialog
                ShellExecuteW(nullptr, L"openas", win::wstr(native), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        QString error;
        if (!win::openWithHandler(path, appPath, &error))
            qWarning("lf::Shell: cannot open %s with %s: %s", qPrintable(path), qPrintable(appPath),
                     qPrintable(error));
    }

    QStringList imageDocumentTypes() const override {
        // File extensions the installer registers Lumaflip for ("Open with", Capabilities).
        return {QStringLiteral(".jpg"),  QStringLiteral(".jpeg"), QStringLiteral(".png"),
                QStringLiteral(".gif"),  QStringLiteral(".bmp"),  QStringLiteral(".tif"),
                QStringLiteral(".tiff"), QStringLiteral(".webp"), QStringLiteral(".heic"),
                QStringLiteral(".heif"), QStringLiteral(".avif"), QStringLiteral(".jxl"),
                QStringLiteral(".svg"),  QStringLiteral(".ico"),  QStringLiteral(".jxr"),
                QStringLiteral(".dds"),  QStringLiteral(".psd"),  QStringLiteral(".exr"),
                QStringLiteral(".dng"),  QStringLiteral(".cr2"),  QStringLiteral(".cr3"),
                QStringLiteral(".nef"),  QStringLiteral(".arw"),  QStringLiteral(".raf"),
                QStringLiteral(".orf"),  QStringLiteral(".rw2")};
    }

    bool makeDefaultViewer(const QStringList&, QString* error) override {
        openDefaultAppsSettings();
        if (error)
            *error = Tr::tr("Windows erlaubt Programmen nicht, sich selbst als Standard-App festzulegen. "
                            "Die Einstellungen „Standard-Apps“ wurden geöffnet: Wähle dort Lumaflip für die "
                            "gewünschten Dateitypen.");
        return false;
    }

    void makeDefaultViewerAsync(const QStringList& types, QObject* context, DefaultViewerDone done,
                                DefaultViewerPending) override {
        QString error;
        const bool ok = makeDefaultViewer(types, &error);
        QTimer::singleShot(0, context, [done = std::move(done), ok, error] {
            if (done)
                done(ok, error);
        });
    }

private:
    static void openDefaultAppsSettings() {
        // The page of the app itself when the installer registered it (per user or for all).
        QString uri = QStringLiteral("ms-settings:defaultapps");
        if (!QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\RegisteredApplications"),
                       QSettings::NativeFormat)
                 .value(QStringLiteral("Lumaflip"))
                 .toString()
                 .isEmpty())
            uri += QStringLiteral("?registeredAppUser=Lumaflip");
        else if (!QSettings(QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\RegisteredApplications"),
                            QSettings::NativeFormat)
                      .value(QStringLiteral("Lumaflip"))
                      .toString()
                      .isEmpty())
            uri += QStringLiteral("?registeredAppMachine=Lumaflip");
        ShellExecuteW(nullptr, L"open", win::wstr(uri), nullptr, nullptr, SW_SHOWNORMAL);
    }
};

} // namespace

std::unique_ptr<Shell> createShell() {
    return std::make_unique<WinShell>();
}

bool systemRestoresWindows() {
    return false; // Windows has no per-app "reopen windows" setting
}

QStringList appLanguagePreference(const QString&) {
    return {}; // the per-app language of Windows applies to packaged apps only
}

bool setAppKitLanguage(const QString&, const QString&) {
    return true; // no AppKit
}

} // namespace lf
