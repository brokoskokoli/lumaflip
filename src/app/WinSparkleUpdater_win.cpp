// SPDX-License-Identifier: GPL-3.0-or-later
#include "WinSparkleUpdater_win.h"

#include "Application.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QMutex>
#include <QSettings>

#include <windows.h>

#include <cstring>
#include <cwchar>

Q_DECLARE_LOGGING_CATEGORY(lcUpdater)

namespace lf {

namespace {

// The subset of winsparkle.h (WinSparkle >= 0.9) Lumaflip uses. Declared here instead of
// including the header: the DLL is loaded at run time, nothing is linked.
struct ConfigMethods {
    int(__cdecl* configRead)(const char* name, wchar_t* buf, size_t len, void* userData);
    void(__cdecl* configWrite)(const char* name, const wchar_t* value, void* userData);
    void(__cdecl* configDelete)(const char* name, void* userData);
    void* userData;
};
using VoidFn = void(__cdecl*)();
using IntFn = int(__cdecl*)();

struct Api {
    void(__cdecl* init)() = nullptr;
    void(__cdecl* cleanup)() = nullptr;
    void(__cdecl* setLang)(const char*) = nullptr;
    void(__cdecl* setAppcastUrl)(const char*) = nullptr;
    int(__cdecl* setEddsaPublicKey)(const char*) = nullptr;
    void(__cdecl* setAppDetails)(const wchar_t*, const wchar_t*, const wchar_t*) = nullptr;
    void(__cdecl* setConfigMethods)(ConfigMethods*) = nullptr;
    void(__cdecl* setAutomaticCheck)(int) = nullptr;
    int(__cdecl* getAutomaticCheck)() = nullptr;
    void(__cdecl* setErrorCallback)(VoidFn) = nullptr;
    void(__cdecl* setCanShutdownCallback)(IntFn) = nullptr;
    void(__cdecl* setShutdownRequestCallback)(VoidFn) = nullptr;
    void(__cdecl* checkWithUi)() = nullptr;
};

template <typename F> bool resolve(HMODULE module, const char* name, F& fn) {
    // FARPROC -> the real signature (through a generic function pointer, no cast warning).
    fn = reinterpret_cast<F>(reinterpret_cast<void (*)()>(::GetProcAddress(module, name)));
    return fn != nullptr;
}

// Loaded once per process and never unloaded (WinSparkle keeps threads running).
const Api* loadApi(const QString& path, QString* error) {
    static Api api;
    static bool loaded = false;
    static QString loadError;
    static QMutex mutex;
    const QMutexLocker lock(&mutex);
    if (loaded)
        return &api;
    // Only this file, its dependencies from System32: no search through PATH or the current
    // directory (DLL planting).
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    HMODULE module = ::LoadLibraryExW(native.c_str(), nullptr,
                                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        *error = QStringLiteral("WinSparkle.dll not loadable (%1, error %2)").arg(path).arg(::GetLastError());
        return nullptr;
    }
    const bool ok =
        resolve(module, "win_sparkle_init", api.init) &&
        resolve(module, "win_sparkle_cleanup", api.cleanup) &&
        resolve(module, "win_sparkle_set_lang", api.setLang) &&
        resolve(module, "win_sparkle_set_appcast_url", api.setAppcastUrl) &&
        resolve(module, "win_sparkle_set_eddsa_public_key", api.setEddsaPublicKey) &&
        resolve(module, "win_sparkle_set_app_details", api.setAppDetails) &&
        resolve(module, "win_sparkle_set_config_methods", api.setConfigMethods) &&
        resolve(module, "win_sparkle_set_automatic_check_for_updates", api.setAutomaticCheck) &&
        resolve(module, "win_sparkle_get_automatic_check_for_updates", api.getAutomaticCheck) &&
        resolve(module, "win_sparkle_set_error_callback", api.setErrorCallback) &&
        resolve(module, "win_sparkle_set_can_shutdown_callback", api.setCanShutdownCallback) &&
        resolve(module, "win_sparkle_set_shutdown_request_callback", api.setShutdownRequestCallback) &&
        resolve(module, "win_sparkle_check_update_with_ui", api.checkWithUi);
    if (!ok) {
        *error = QStringLiteral("WinSparkle.dll is too old (EdDSA needs WinSparkle 0.9 or later)");
        return nullptr; // the module stays loaded; harmless
    }
    loaded = true;
    return &api;
}

// ---- settings store (WinSparkle calls these from any of its threads) ----
QMutex g_storeMutex;
QString g_storeFile; // empty: registry

QSettings* openStore() {
    if (g_storeFile.isEmpty())
        return new QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Lumaflip\\WinSparkle"),
                             QSettings::NativeFormat);
    auto* s = new QSettings(g_storeFile, QSettings::IniFormat);
    s->beginGroup(QStringLiteral("WinSparkle"));
    return s;
}

int __cdecl configRead(const char* name, wchar_t* buf, size_t len, void*) {
    const QMutexLocker lock(&g_storeMutex);
    const std::unique_ptr<QSettings> s(openStore());
    const QVariant v = s->value(QString::fromLatin1(name));
    if (!v.isValid())
        return FALSE;
    const std::wstring value = v.toString().toStdWString();
    if (!buf || value.size() + 1 > len)
        return FALSE;
    std::wmemcpy(buf, value.c_str(), value.size() + 1);
    return TRUE;
}

void __cdecl configWrite(const char* name, const wchar_t* value, void*) {
    const QMutexLocker lock(&g_storeMutex);
    const std::unique_ptr<QSettings> s(openStore());
    s->setValue(QString::fromLatin1(name), QString::fromWCharArray(value));
}

void __cdecl configDelete(const char* name, void*) {
    const QMutexLocker lock(&g_storeMutex);
    const std::unique_ptr<QSettings> s(openStore());
    s->remove(QString::fromLatin1(name));
}

ConfigMethods g_configMethods{&configRead, &configWrite, &configDelete, nullptr};

// ---- callbacks (never on the GUI thread) ----
void __cdecl onError() {
    qCWarning(lcUpdater) << "WinSparkle reported an error (feed, download or signature)";
}

int __cdecl canShutdown() {
    // Lumaflip has no unsaved documents; pending file operations are finished by
    // Application::quit() before the app ends (the installer waits for it).
    return 1;
}

void __cdecl shutdownRequest() {
    // WinSparkle has started the installer: quit like File → Quit (session saved, file
    // operations finished), so the installer can replace the files.
    if (auto* app = QCoreApplication::instance()) {
        QMetaObject::invokeMethod(
            app,
            [] {
                if (Application* a = Application::instance())
                    a->quit();
                else
                    QCoreApplication::quit();
            },
            Qt::QueuedConnection);
    }
}

} // namespace

struct WinSparkleUpdater::Private {
    const Api* api = nullptr;
    QString settingsFile;
    bool started = false;
};

WinSparkleUpdater::WinSparkleUpdater() : d(std::make_unique<Private>()) {}

WinSparkleUpdater::~WinSparkleUpdater() {
    if (d->started)
        d->api->cleanup();
}

std::unique_ptr<WinSparkleUpdater> WinSparkleUpdater::create(const Config& config, QString* error) {
    QString path = config.libraryPath;
    if (path.isEmpty())
        path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("WinSparkle.dll"));
    if (!QFileInfo::exists(path)) {
        *error = QStringLiteral("WinSparkle.dll missing (%1)").arg(path);
        return nullptr;
    }
    const Api* api = loadApi(path, error);
    if (!api)
        return nullptr;
    std::unique_ptr<WinSparkleUpdater> u(new WinSparkleUpdater());
    u->d->api = api;
    u->d->settingsFile = config.settingsFile;
    u->activate();
    if (!api->setEddsaPublicKey(config.publicKey.toLatin1().constData())) {
        *error = QStringLiteral("WinSparkle refused the EdDSA public key");
        return nullptr;
    }
    api->setAppcastUrl(config.feedUrl.toUtf8().constData());
    const std::wstring company = QStringLiteral("The Lumaflip contributors").toStdWString();
    const std::wstring name = config.appName.toStdWString();
    const std::wstring version = config.appVersion.toStdWString();
    api->setAppDetails(company.c_str(), name.c_str(), version.c_str());
    api->setErrorCallback(&onError);
    api->setCanShutdownCallback(&canShutdown);
    api->setShutdownRequestCallback(&shutdownRequest);
    return u;
}

void WinSparkleUpdater::activate() const {
    {
        const QMutexLocker lock(&g_storeMutex);
        g_storeFile = d->settingsFile;
    }
    d->api->setConfigMethods(&g_configMethods);
}

bool WinSparkleUpdater::start(const QString& language, QString* error) {
    if (d->started)
        return true;
    activate();
    if (!language.isEmpty())
        d->api->setLang(language.toLatin1().constData());
    d->api->init();
    d->started = true;
    Q_UNUSED(error);
    return true;
}

bool WinSparkleUpdater::isStarted() const {
    return d->started;
}

bool WinSparkleUpdater::automaticallyChecks() const {
    activate();
    return d->api->getAutomaticCheck() == 1;
}

void WinSparkleUpdater::setAutomaticallyChecks(bool on) {
    activate();
    d->api->setAutomaticCheck(on ? 1 : 0);
}

void WinSparkleUpdater::checkForUpdates() {
    activate();
    d->api->checkWithUi();
}

} // namespace lf
