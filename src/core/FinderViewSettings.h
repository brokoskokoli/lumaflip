// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable interpretation of Finder view settings (SPEC 6.1, tiers 1, 2 and 4).
//
// Tier 1: per-folder settings from .DS_Store records. The settings of folder X are stored
//         in <parent of X>/.DS_Store under the name X; fallback: X/.DS_Store record ".".
//         vstl = view style; list view: lsvC (columns array) / lsvp, lsvP (columns dict)
//         with sortColumn + ascending; icon view: icvp.arrangeBy ("none"/"grid" -> Iloc
//         icon positions); gallery view: glvp.arrangeBy.
// Tier 2: global defaults from the com.apple.finder preferences (FXPreferredViewStyle,
//         StandardViewSettings, StandardViewOptions, special places such as
//         DesktopViewSettings / ICloudViewSettings).
// Tier 4: name ascending.
//
// Everything here is pure and works on already parsed data, so it is fully unit-testable
// and portable; the macOS provider only supplies the inputs.

#include "DsStore.h"
#include "SortSpec.h"

#include <QHash>
#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <optional>

namespace lf::finder {

enum class ViewStyle { Unknown, Icon, List, Column, Gallery };

ViewStyle viewStyleFromCode(const QString& code); // "icnv", "Nlsv", "clmv", "glyv", "Flwv"

// The view-relevant records of one folder.
struct FolderRecords {
    bool found = false;             // at least one view record exists
    std::optional<ViewStyle> style; // vstl
    QVariantMap lsvC, lsvp, lsvP;   // decoded list view plists (empty if absent/garbage)
    QVariantMap icvp;               // icon view plist
    QVariantMap glvp;               // gallery view plist
};

// Collects the records of `name` ("." = the folder itself) from a parsed .DS_Store.
FolderRecords extractFolderRecords(const QVector<DsStoreRecord>& records, const QString& name);
// Combines tier-1 sources: `primary` (parent .DS_Store) wins per field over `secondary` (".").
FolderRecords mergeRecords(const FolderRecords& primary, const FolderRecords& secondary);

// Sort key for a list view column identifier ("name", "dateModified", ...).
// dateLastOpened -> DateModified (closest available), version/comments/label/... -> Name.
std::optional<SortKey> keyForColumn(const QString& identifier);
// Sort spec for an icon/gallery view arrangement ("name", "dateAdded", "none", ...).
// Finder arranges dates and sizes newest/largest first, everything else ascending.
std::optional<SortSpec> specForArrangeBy(const QString& arrangeBy);
// Sort spec for a list view settings plist (lsvC/lsvp/lsvP or the global
// ListViewSettings / ExtendedListViewSettingsV2). nullopt if unusable.
std::optional<SortSpec> specForListSettings(const QVariantMap& settings);
// Column view "ArrangeBy" four-char codes from StandardViewOptions ("dnam", "modd", ...).
std::optional<SortSpec> specForColumnArrangeBy(const QString& code);

// Global settings (tier 2). `finderPrefs` holds the top-level keys of com.apple.finder
// (FXPreferredViewStyle, StandardViewSettings, StandardViewOptions, DesktopViewSettings,
// ICloudViewSettings, ...). `specialKey` names the settings dict for special places
// ("DesktopViewSettings", "ICloudViewSettings") or is empty.
struct GlobalContext {
    QVariantMap finderPrefs;
    QString specialKey;
};

// Full tier 1 -> 2 -> 4 resolution. Never fails; source tells which tier won.
SortSpec resolve(const FolderRecords& folder, const GlobalContext& global);

// Tier-1 records of `folderPath`: <parent>/.DS_Store entry <name>, merged with the
// <folder>/.DS_Store entry "." as fallback.
// `folderPath` must be absolute. Robust against missing/garbage files (F-SORT-08).
FolderRecords readFolderRecords(const QString& folderPath);

// Icon positions (Iloc) of the files inside `folderPath` (from <folder>/.DS_Store).
QHash<QString, QPointF> readIconPositions(const QString& folderPath);
QHash<QString, QPointF> iconPositions(const QVector<DsStoreRecord>& records);

// Tier 3 helper: interprets the output of the live Finder AppleScript query
// (lines: target POSIX path, view, sort column or arrangement, direction).
// Returns nullopt unless the target equals `folderPath` and the data is understood.
std::optional<SortSpec> parseLiveQueryOutput(const QString& output, const QString& folderPath);

} // namespace lf::finder
