// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WinSparkle backend of lf::Updater on Windows (F-MAC-08, docs/WINDOWS.md), the counterpart
// of platform/mac/SparkleUpdater. WinSparkle.dll (tools/fetch_winsparkle.py, >= 0.9 for
// EdDSA) is loaded at run time from the folder of Lumaflip.exe - nothing is linked, a build or
// an installation without the DLL simply has no updater.
//
// WinSparkle keeps one configuration per process. Its settings (automatic checks, last check,
// skipped version) go through win_sparkle_set_config_methods into a QSettings store:
// HKCU\Software\Lumaflip\WinSparkle for the app, an INI file for tests. Nothing here touches
// the network until start() or checkForUpdates().

#include <QString>

#include <memory>

namespace lf {

class WinSparkleUpdater {
public:
    struct Config {
        QString feedUrl;      // https appcast (appcast-windows.xml), or file: in tests
        QString publicKey;    // base64 EdDSA public key (the same key as Sparkle on macOS)
        QString appName;      // shown in WinSparkle's windows
        QString appVersion;   // compared with sparkle:version of the appcast items
        QString settingsFile; // empty: registry (app); otherwise an INI file (tests)
        QString libraryPath;  // empty: WinSparkle.dll next to the executable
    };

    // Loads WinSparkle.dll and configures it (feed, key, app details, callbacks). nullptr and
    // *error on failure (DLL missing, too old, key refused).
    static std::unique_ptr<WinSparkleUpdater> create(const Config& config, QString* error);
    ~WinSparkleUpdater(); // win_sparkle_cleanup() if started
    WinSparkleUpdater(const WinSparkleUpdater&) = delete;
    WinSparkleUpdater& operator=(const WinSparkleUpdater&) = delete;

    // win_sparkle_init(): the periodic check (if enabled) or, on the second launch, the
    // permission prompt - as Sparkle on macOS. `language` as update::winSparkleLanguage().
    bool start(const QString& language, QString* error);
    bool isStarted() const;

    bool automaticallyChecks() const; // win_sparkle_get_automatic_check_for_updates
    void setAutomaticallyChecks(bool on);
    void checkForUpdates(); // win_sparkle_check_update_with_ui (user-initiated, with UI)

private:
    WinSparkleUpdater();
    void activate() const; // routes WinSparkle's settings to this updater's store

    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
