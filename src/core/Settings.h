// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed application settings (SPEC 4.13, 4.14, 4.15) backed by QSettings.
// All keys and defaults live in Settings.cpp. Emits changed(key) so windows can react.

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <optional>

class QSettings;

namespace lf {

namespace key {
// General: UI language (F-I18N-03), "system" or a code of uiLanguageCodes() (Language.h)
inline constexpr auto Language = "general/language";
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
// M3: magnifier (F-VIEW-11), grid view (F-GRID-02), coupled windows (F-WIN-07)
inline constexpr auto LoupeSize = "view/loupeSize";     // "small" | "medium" | "large", default "medium"
inline constexpr auto LoupeZoom = "view/loupeZoom";     // "100" | "200" (percent), default "200"
inline constexpr auto GridThumbSize = "view/gridThumb"; // 80..400 logical px, default 160
inline constexpr auto CoupleBy = "window/coupleBy";     // "name" | "index", default "name"
inline constexpr auto HdrEnabled = "view/hdr";          // bool, default true (F-COL-04 HDR toggle)
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
inline constexpr auto PairRawJpeg = "files/pairRawJpeg";        // F-FMT-06: bool, default false
// Ratings (F-CULL-02, M3)
inline constexpr auto RatingStoreSidecar = "rating/sidecar";       // default true
inline constexpr auto RatingStoreEmbedded = "rating/embedded";     // default false
inline constexpr auto RatingStoreFinderTags = "rating/finderTags"; // default false
// Culling and quick move (M3: F-CULL-02/03/06, F-FILE-08)
inline constexpr auto RatingTagStarSymbol = "rating/tagStarSymbol";   // default "★" (n stars = n times)
inline constexpr auto RatingTagPick = "rating/tagPick";               // default "Pick"
inline constexpr auto RatingTagReject = "rating/tagReject";           // default "Reject"
inline constexpr auto RatingTagStarColor = "rating/tagStarColor";     // 0..7 Finder label, default 0
inline constexpr auto RatingTagPickColor = "rating/tagPickColor";     // default 2 (green)
inline constexpr auto RatingTagRejectColor = "rating/tagRejectColor"; // default 6 (red)
inline constexpr auto RatingKeepModDate = "rating/keepModDate";       // bool, default true (embedded)
inline constexpr auto CullAutoAdvance = "culling/autoAdvance";        // bool, default false (F-CULL-03)
inline constexpr auto CullLockZoom = "culling/lockZoom";              // bool, default true (F-CULL-06)
inline constexpr auto CullShowHelp = "culling/showHelp";              // bool, default true (F-CULL-06)
// "files/quickTarget1" … "files/quickTarget9": folder paths, "" = not set (F-FILE-08)
inline constexpr auto QuickTargetPrefix = "files/quickTarget";
// Slideshow (F-SLIDE-01/02)
inline constexpr auto SlideshowIntervalSec = "slideshow/intervalSec"; // 1..60, default 3
inline constexpr auto SlideshowRepeat = "slideshow/repeat";           // bool, default true
inline constexpr auto SlideshowShuffle = "slideshow/shuffle";         // bool, default false
inline constexpr auto SlideshowCrossfade = "slideshow/crossfade";     // bool, default true
// Saving the rotation (F-EDIT-02)
inline constexpr auto RotationSaveStrategy = "edit/rotationStrategy"; // "exif" | "lossless" (JPEG DCT)
inline constexpr auto KeepModificationDate = "edit/keepModDate";      // bool, default false
// M4: archives and PDF (F-OPEN-10), Live Text (F-MAC-06), wallpaper (F-FILE-09), custom
// actions (F-FILE-10)
inline constexpr auto ArchiveRightToLeft =
    "archive/rightToLeft";                              // bool, default false: ←/→ swapped in cbz/cbr/cb7
inline constexpr auto LiveTextEnabled = "mac/liveText"; // bool, default true (VisionKit may be used)
inline constexpr auto WallpaperAllScreens = "files/wallpaperAllScreens"; // bool, default false
inline constexpr auto CustomActions = "actions/custom"; // JSON list (core/CustomActions.h), default "[]"
inline constexpr auto CustomActionTimeoutSec = "actions/timeoutSec"; // 1..3600, default 60
// M4: crop (F-EDIT-03), export (F-EDIT-04), photo overlays (F-INFO-07)
inline constexpr auto CropAspect = "crop/aspect";     // "free" | "original" | "1:1" | "4:3" | "3:2" | "16:9"
inline constexpr auto CropPortrait = "crop/portrait"; // bool, default false
inline constexpr auto CropGrid = "crop/grid";         // bool, default true (rule of thirds)
inline constexpr auto ExportFormat = "export/format"; // "jpeg" | "heic" | "avif" | "jxl" | "png" | "tiff"
inline constexpr auto ExportQuality = "export/quality";                         // 1..100, default 90
inline constexpr auto ExportMaxEdge = "export/maxEdge";                         // px, 0 = original size
inline constexpr auto ExportMetadata = "export/metadata";                       // "all" | "noGps" | "none"
inline constexpr auto ExportColor = "export/colorSpace";                        // "keep" | "srgb" | "p3"
inline constexpr auto ExportHdr = "export/hdr";                                 // "keep" | "sdr"
inline constexpr auto ExportPattern = "export/namePattern";                     // default "{name}-export"
inline constexpr auto ExportFolder = "export/folder";                           // "" = next to the source
inline constexpr auto OverlayClipping = "overlay/clipping";                     // bool, default false
inline constexpr auto OverlayPeaking = "overlay/peaking";                       // bool, default false
inline constexpr auto OverlayAfPoints = "overlay/afPoints";                     // bool, default false
inline constexpr auto OverlayClipHighPercent = "overlay/clipHigh";              // 50..100, default 98
inline constexpr auto OverlayClipLowPercent = "overlay/clipLow";                // 0..50, default 2
inline constexpr auto OverlayClipHdrReference = "overlay/clipHdrReference";     // "headroom" | "sdr"
inline constexpr auto OverlayPeakingSensitivity = "overlay/peakingSensitivity"; // 0..100, default 50
inline constexpr auto OverlayPeakingColor = "overlay/peakingColor"; // "#rrggbb", default "#ff2020"
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
    // F-SET-03 in two steps, so the UI does the file I/O on a worker thread: the export
    // contents (any thread) and the file write; the file read and applying the contents
    // (GUI thread: emits changed()).
    QByteArray exportData() const;
    static bool writeExport(const QString& filePath, const QByteArray& data, QString* error);
    static std::optional<QByteArray> readImport(const QString& filePath, QString* error);
    bool importData(const QByteArray& json, QString* error);

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
