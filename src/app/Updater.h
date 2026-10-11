// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Automatic updates (SPEC 4.12 F-MAC-08, 10.1) with Sparkle 2 (macOS) or WinSparkle (Windows)
// and an EdDSA-signed appcast on GitHub Pages (core/UpdateConfig.h: one feed per platform).
// The update check is the app's only network traffic and can be switched off (SPEC 10.3):
// "Automatisch nach Updates suchen" in the preferences maps to Sparkle's
// SUEnableAutomaticChecks (WinSparkle: its automatic check); until the user decides, the
// updater asks on the second launch (Info.plist has no SUEnableAutomaticChecks).
//
// Available only if Sparkle is compiled in (LF_ENABLE_SPARKLE) and the bundle's
// Info.plist has SUFeedURL + SUPublicEDKey (LF_SPARKLE_PUBLIC_KEY). Otherwise the
// updater is disabled (reason in the log), the menu item and the preferences are hidden.
// Windows: WinSparkle.dll next to Lumaflip.exe (LF_ENABLE_WINSPARKLE) and the same public key
// compiled in (LF_SPARKLE_PUBLIC_KEY); WinSparkle asks on the second launch as Sparkle does.
// WinSparkle cannot install without asking (it would quit the app in the middle of work), so
// "Updates automatisch installieren" stays disabled there (allowsAutomaticDownloads()).

#include <QObject>
#include <QString>

#include <memory>

class QAction;
class QFormLayout;
class QWidget;

namespace lf {

class SparkleUpdater;
class WinSparkleUpdater;

class Updater : public QObject {
    Q_OBJECT
public:
    // The app's updater (main bundle); created on first use. Starts Sparkle
    // kStartDelayMs after creation (not on the launch path, P-01).
    static Updater& instance();
    // Tests: replaces instance() (nullptr: back to the app's own).
    static void setInstanceForTesting(Updater* updater);
    // An updater for another .app bundle (tests). Never started by itself: no network.
    static std::unique_ptr<Updater> createForBundle(const QString& bundlePath);
    // Windows (tests): a WinSparkle updater for this feed and key with its settings in an INI
    // file instead of the registry. Never started by itself: no network. Elsewhere unavailable.
    static std::unique_ptr<Updater> createForConfiguration(const QString& feedUrl, const QString& publicKey,
                                                           const QString& settingsFile);
    // Tests: WinSparkle.dll from this path instead of the executable's folder.
    static void setWinSparkleLibraryForTesting(const QString& path);

    static constexpr int kStartDelayMs = 3000;

    static bool isCompiledIn(); // LF_HAVE_SPARKLE or LF_HAVE_WINSPARKLE
    // Empty if feed URL and EdDSA public key are usable, otherwise the reason.
    static QString configurationProblem(const QString& feedUrl, const QString& publicKey);

    ~Updater() override;

    bool isAvailable() const;
    QString unavailableReason() const;
    QString bundleId() const;
    bool isStarted() const;

    bool automaticallyChecks() const; // "Automatisch nach Updates suchen"
    void setAutomaticallyChecks(bool on);
    bool automaticallyDownloads() const; // "Updates automatisch installieren"
    void setAutomaticallyDownloads(bool on);
    bool allowsAutomaticDownloads() const;
    bool start();           // idempotent; false if unavailable or Sparkle refused the configuration
    void checkForUpdates(); // menu "Nach Updates suchen …" (starts the updater first if needed)

    // UI hooks: the menu command (visible only if available) and the preference rows
    // in "Allgemein" (only if available).
    static void setUpMenuAction(QAction* action);
    static void addPreferences(QFormLayout* form, QWidget* page);

signals:
    void settingsChanged();

private:
    explicit Updater(const QString& bundlePath);
    Updater(const QString& feedUrl, const QString& publicKey, const QString& settingsFile);

    QString m_reason;
    QString m_bundleId;
    std::unique_ptr<SparkleUpdater> m_sparkle;
    std::unique_ptr<WinSparkleUpdater> m_winSparkle;
};

} // namespace lf
