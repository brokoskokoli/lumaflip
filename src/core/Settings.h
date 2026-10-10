// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed application settings (SPEC 4.13, 4.14, 4.15) backed by QSettings.
// All keys and defaults live in Settings.cpp. Emits changed(key) so windows can react.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

class QSettings;

namespace lf {

namespace key {
// General / windows (4.15)
inline constexpr auto OpenBehavior = "window/openBehavior"; // "newWindow" | "replaceFront" | "newUnlessOpen"
inline constexpr auto FitWindowToImage = "window/fitToImage";     // bool, default false
inline constexpr auto RestoreWindows = "window/restore";          // bool (F-WIN-06)
inline constexpr auto TitleShowFolder = "window/titleShowFolder"; // bool, default false (F-WIN-10)
inline constexpr auto AllowTabs = "window/allowTabs";             // bool, default false (F-WIN-05)
// QSize (device px) the last window decoded for; read at launch to start decoding the
// opened image before the window exists (P-01). Not a user setting.
inline constexpr auto DecodeTargetHint = "window/decodeTarget";
// Files
inline constexpr auto ConfirmTrash = "files/confirmTrash"; // bool, default false (F-FILE-01)
inline constexpr auto EditorApp = "files/editor"; // app bundle path, "" = system default app (F-FILE-05)
// Display
inline constexpr auto Background = "view/background"; // "dark" | "light" | "system" | "checker" | "#rrggbb"
inline constexpr auto ZoomMode =
    "view/zoomMode"; // "fit" | "fitUpscale" | "actual" | "fillWidth" | "fillHeight" | "fill"
inline constexpr auto UpscaleFilter = "view/upscaleFilter";   // "smooth" | "nearest"
inline constexpr auto ActualSizeUnit = "view/actualSizeUnit"; // "devicePixels" | "points"
inline constexpr auto ShowStatusBar = "view/statusBar";
inline constexpr auto ShowInfoPanel = "view/infoPanel";
inline constexpr auto ShowFilmstrip = "view/filmstrip";
// Navigation & sorting
inline constexpr auto WrapMode = "nav/wrap";               // "stop" | "wrap" | "nextFolder"
inline constexpr auto FollowFinder = "sort/followFinder";  // bool, default true
inline constexpr auto FinderLiveQuery = "sort/finderLive"; // bool, default false (opt-in)
inline constexpr auto ManualSortKey = "sort/manualKey";    // "name" | "dateModified" | "dateCreated" |
                                                           // "dateAdded" | "dateCaptured" | "size" |
                                                           // "kind" | "pixelCount" | "rating"
inline constexpr auto ManualSortAscending = "sort/manualAscending"; // bool, default true
// Input (4.14) - values are choice ids, see InputBindings.h ("input/<slotId>",
// "input/shortcut/<actionId>" = list of portable key sequence strings)
inline constexpr auto InputPrefix = "input/";
inline constexpr auto ShortcutPrefix = "input/shortcut/";
inline constexpr auto WheelInverted = "input/wheelTuning.inverted";        // bool, default false (F-IN-03)
inline constexpr auto WheelStepsPerImage = "input/wheelTuning.steps";      // 1..5, default 1
inline constexpr auto WheelMinIntervalMs = "input/wheelTuning.intervalMs"; // 0..300, default 120
inline constexpr auto ZoomStepPercent = "input/wheelTuning.zoomStep";      // 5..50, default 15
inline constexpr auto JumpSize = "input/jumpSize";                         // 2..100, default 10 (F-IN-04)
// Performance
inline constexpr auto MemoryBudgetMB = "perf/memoryBudgetMB";   // 0 = auto (25% RAM, max 2048)
inline constexpr auto PrefetchAhead = "perf/prefetchAhead";     // default 3
inline constexpr auto PrefetchBehind = "perf/prefetchBehind";   // default 1
inline constexpr auto RawEmbeddedOnly = "perf/rawEmbeddedOnly"; // bool, default false
// Ratings (F-CULL-02, M3)
inline constexpr auto RatingStoreSidecar = "rating/sidecar";       // default true
inline constexpr auto RatingStoreEmbedded = "rating/embedded";     // default false
inline constexpr auto RatingStoreFinderTags = "rating/finderTags"; // default false
// Slideshow (F-SLIDE-01/02)
inline constexpr auto SlideshowIntervalSec = "slideshow/intervalSec"; // 1..60, default 3
inline constexpr auto SlideshowRepeat = "slideshow/repeat";           // bool, default true
inline constexpr auto SlideshowShuffle = "slideshow/shuffle";         // bool, default false
inline constexpr auto SlideshowCrossfade = "slideshow/crossfade";     // bool, default true
// Saving the rotation (F-EDIT-02)
inline constexpr auto RotationSaveStrategy = "edit/rotationStrategy"; // "exif" | "lossless" (JPEG DCT)
inline constexpr auto KeepModificationDate = "edit/keepModDate";      // bool, default false
} // namespace key

class Settings : public QObject {
    Q_OBJECT
public:
    static Settings& instance();

    QVariant value(const QString& key) const; // falls back to default
    void setValue(const QString& key, const QVariant& value);
    QVariant defaultValue(const QString& key) const;
    void resetToDefaults(const QString& prefix = {}); // prefix "" = all
    static QStringList knownKeys();                   // every key with a default (sorted)

    bool exportTo(const QString& filePath, QString* error) const; // F-SET-03
    bool importFrom(const QString& filePath, QString* error);

    // Per-folder memory (F-SORT-06): manual sort spec etc., stored in app support dir.
    QVariant folderValue(const QString& folderPath, const QString& key) const;
    void setFolderValue(const QString& folderPath, const QString& key, const QVariant& value);

    // For tests: use an isolated settings file.
    static void useTestingBackend(const QString& iniPath);
    // For tests: the system setting the default of key::RestoreWindows follows (F-WIN-06,
    // NSQuitAlwaysKeepsWindows): -1 = ask the system, 0/1 = pretend it is off/on.
    static void setSystemRestoreWindowsForTesting(int value);

    // SPEC 8.6: settings in ~/Library/Preferences/io.github.brokoskokoli.lumaflip.plist,
    // app data in ~/Library/Application Support/Lumaflip/. main() calls
    // configureApplication() before anything reads settings; it sets the application
    // identity (display name stays "Lumaflip").
    static void configureApplication();
    static QString nativeSettingsPath(); // file of the native (default) backend
    static QString appDataPath();        // folder for per-folder data etc.
    // Copies values of older locations once (settings of 0.1: "…Lumaflip.plist", app
    // data "Application Support/Lumaflip/Lumaflip"). Returns the number of copied values.
    static int migrateLegacyLocations();
    // Building blocks of the migration (testable with INI files): copies keys missing in
    // `to`, marks `to` as migrated; does nothing if `to` was migrated before.
    static int migrateValues(QSettings& from, QSettings& to);
    static bool migrateFile(const QString& oldPath, const QString& newPath);

signals:
    void changed(const QString& key);

private:
    explicit Settings(QObject* parent = nullptr);
    struct Private;
    Private* d;
};

} // namespace lf
