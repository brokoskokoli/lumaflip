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
    FinderIconPosition,  // icon view without arrangement: row/column order of Iloc positions
};

// Where a SortSpec came from (shown in the UI, F-SORT-05).
enum class SortSource {
    Manual,         // user chose it for this folder / globally
    FinderLive,     // tier 3: AppleScript query of the front Finder window
    FinderDsStore,  // tier 1: .DS_Store of the folder
    FinderGlobal,   // tier 2: com.apple.finder defaults
    Fallback,       // tier 4: name ascending
};

struct SortSpec {
    SortKey key = SortKey::Name;
    bool ascending = true;
    SortSource source = SortSource::Fallback;

    friend bool operator==(const SortSpec&, const SortSpec&) = default;
};

// Human readable (German UI) description, e.g. "Wie Finder: Hinzugefügt am ↓".
QString describe(const SortSpec& spec);
QString sortKeyLabel(SortKey key);

} // namespace lf
