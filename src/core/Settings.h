// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Typed application settings (SPEC 4.13, 4.14, 4.15) backed by QSettings.
// All keys and defaults live in Settings.cpp. Emits changed(key) so windows can react.

#include <QObject>
#include <QString>
#include <QVariant>

namespace lf {

namespace key {
// General / windows (4.15)
inline constexpr auto OpenBehavior = "window/openBehavior";        // "newWindow" | "replaceFront" | "newUnlessOpen"
inline constexpr auto FitWindowToImage = "window/fitToImage";      // bool, default false
inline constexpr auto RestoreWindows = "window/restore";           // bool
// Display
inline constexpr auto Background = "view/background";              // "dark" | "light" | "system" | "checker" | "#rrggbb"
inline constexpr auto ZoomMode = "view/zoomMode";                  // "fit" | "fitUpscale" | "actual" | "fillWidth" | "fillHeight" | "fill"
inline constexpr auto UpscaleFilter = "view/upscaleFilter";        // "smooth" | "nearest"
inline constexpr auto ActualSizeUnit = "view/actualSizeUnit";      // "devicePixels" | "points"
inline constexpr auto ShowStatusBar = "view/statusBar";
inline constexpr auto ShowInfoPanel = "view/infoPanel";
inline constexpr auto ShowFilmstrip = "view/filmstrip";
// Navigation & sorting
inline constexpr auto WrapMode = "nav/wrap";                       // "stop" | "wrap" | "nextFolder"
inline constexpr auto FollowFinder = "sort/followFinder";          // bool, default true
inline constexpr auto FinderLiveQuery = "sort/finderLive";         // bool, default false (opt-in)
inline constexpr auto ManualSortKey = "sort/manualKey";
inline constexpr auto ManualSortAscending = "sort/manualAscending";
// Input (4.14) - values are action ids, see InputBindings.h
inline constexpr auto InputPrefix = "input/";
// Performance
inline constexpr auto MemoryBudgetMB = "perf/memoryBudgetMB";      // 0 = auto (25% RAM, max 2048)
inline constexpr auto PrefetchAhead = "perf/prefetchAhead";        // default 3
inline constexpr auto PrefetchBehind = "perf/prefetchBehind";      // default 1
inline constexpr auto RawEmbeddedOnly = "perf/rawEmbeddedOnly";    // bool, default false
// Ratings (F-CULL-02, M3)
inline constexpr auto RatingStoreSidecar = "rating/sidecar";       // default true
inline constexpr auto RatingStoreEmbedded = "rating/embedded";     // default false
inline constexpr auto RatingStoreFinderTags = "rating/finderTags"; // default false
} // namespace key

class Settings : public QObject {
    Q_OBJECT
public:
    static Settings& instance();

    QVariant value(const QString& key) const;          // falls back to default
    void setValue(const QString& key, const QVariant& value);
    QVariant defaultValue(const QString& key) const;
    void resetToDefaults(const QString& prefix = {});  // prefix "" = all

    bool exportTo(const QString& filePath, QString* error) const;   // F-SET-03
    bool importFrom(const QString& filePath, QString* error);

    // Per-folder memory (F-SORT-06): manual sort spec etc., stored in app support dir.
    QVariant folderValue(const QString& folderPath, const QString& key) const;
    void setFolderValue(const QString& folderPath, const QString& key, const QVariant& value);

    // For tests: use an isolated settings file.
    static void useTestingBackend(const QString& iniPath);

signals:
    void changed(const QString& key);

private:
    explicit Settings(QObject* parent = nullptr);
    struct Private;
    Private* d;
};

} // namespace lf
