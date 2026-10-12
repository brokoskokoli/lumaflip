// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace lf {

// SPEC 6 / F-SORT-02
enum class SortKey {
    Name,
    DateModified,
    DateCreated,
    DateAdded,
    DateCaptured,
    Size,
    Kind,
    PixelCount,
    Rating,
    FinderIconPosition, // icon view without arrangement: row/column order of Iloc positions
};

// Where a SortSpec came from (shown in the UI, F-SORT-05).
enum class SortSource {
    Manual,              // user chose it for this folder / globally
    FinderLive,          // tier 3: AppleScript query of the front Finder window
    FinderDsStore,       // tier 1: .DS_Store of the folder
    FinderGlobal,        // tier 2: com.apple.finder defaults
    ExplorerLive,        // Windows: the Explorer window showing the folder (F-SORT-04)
    ExplorerUnsupported, // Windows: that window sorts by a column Lumaflip cannot sort by: name
    Fallback,            // tier 4: name ascending
};

// The file manager whose order the mode "Wie im Finder" follows on this platform (labels,
// F-SORT-05): macOS Finder, Windows Explorer, Linux any file manager (whose settings are not
// read). platformFileManager() is implemented by the platform layer (Platform.h).
enum class FileManager { Finder, Explorer, Other };

struct SortSpec {
    SortKey key = SortKey::Name;
    bool ascending = true;
    SortSource source = SortSource::Fallback;

    friend bool operator==(const SortSpec&, const SortSpec&) = default;
};

// Human readable (German UI) description, e.g. "Wie im Finder: Hinzugefügt am ↓". The
// fallback names the file manager of `fm` ("Name ↑ (Explorer-Ansicht nicht gefunden)").
QString describe(const SortSpec& spec, FileManager fm);
QString describe(const SortSpec& spec); // fm = platformFileManager()
QString sortKeyLabel(SortKey key);

} // namespace lf
