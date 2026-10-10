// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sparkle 2 backend of lf::Updater (F-MAC-08). Only compiled with LF_HAVE_SPARKLE
// (CMake option LF_ENABLE_SPARKLE); plain C++ interface, Objective-C++ inside.

#include <QString>

#include <memory>

namespace lf {

// What a bundle's Info.plist says about updates.
struct SparkleBundleInfo {
    bool isBundle = false; // a real .app bundle with a bundle identifier
    QString bundleId;
    QString feedUrl;   // SUFeedURL
    QString publicKey; // SUPublicEDKey
};

// `bundlePath` empty: the running app (NSBundle.mainBundle).
SparkleBundleInfo readSparkleBundleInfo(const QString& bundlePath);

class SparkleUpdater {
public:
    // Main bundle: SPUStandardUpdaterController (standard Sparkle UI). Any other bundle
    // (tests): SPUUpdater with SPUStandardUserDriver for that bundle. Never starts the
    // updater by itself - no network access until start().
    explicit SparkleUpdater(const QString& bundlePath);
    ~SparkleUpdater();
    SparkleUpdater(const SparkleUpdater&) = delete;
    SparkleUpdater& operator=(const SparkleUpdater&) = delete;

    // -[SPUUpdater startUpdater:]: validates the configuration and schedules the
    // automatic checks (or the permission prompt on the second launch).
    bool start(QString* error);
    bool isStarted() const;

    bool automaticallyChecks() const; // SPUUpdater.automaticallyChecksForUpdates (SUEnableAutomaticChecks)
    void setAutomaticallyChecks(bool on);
    bool automaticallyDownloads() const; // SPUUpdater.automaticallyDownloadsUpdates (SUAutomaticallyUpdate)
    void setAutomaticallyDownloads(bool on);
    bool allowsAutomaticDownloads() const; // SPUUpdater.allowsAutomaticUpdates
    bool canCheckForUpdates() const;
    void checkForUpdates(); // user-initiated check with Sparkle's UI

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
