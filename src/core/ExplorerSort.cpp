// SPDX-License-Identifier: GPL-3.0-or-later
#include "ExplorerSort.h"

namespace lf::explorer {

std::optional<SortKey> sortKeyFor(const PropertyKey& key) {
    if (key == pkey::ItemNameDisplay || key == pkey::FileName)
        return SortKey::Name;
    if (key == pkey::DateModified)
        return SortKey::DateModified;
    if (key == pkey::DateCreated)
        return SortKey::DateCreated;
    if (key == pkey::Size)
        return SortKey::Size;
    if (key == pkey::ItemTypeText || key == pkey::ItemType)
        return SortKey::Kind;
    // "Aufnahmedatum" and "Datum" (the Pictures folders' default column: date taken, else a
    // file date). Lumaflip has no such fallback: images without a capture date keep their
    // place by name, after the others (docs/WINDOWS.md).
    if (key == pkey::PhotoDateTaken || key == pkey::ItemDate)
        return SortKey::DateCaptured;
    if (key == pkey::Rating)
        return SortKey::Rating;
    return std::nullopt;
}

SortSpec specForView(const std::optional<ColumnSort>& sort, const std::optional<ColumnSort>& group) {
    const std::optional<SortKey> sortKey = sort ? sortKeyFor(sort->key) : std::nullopt;
    const std::optional<SortKey> groupKey = group ? sortKeyFor(group->key) : std::nullopt;
    if (groupKey && groupKey != sortKey)
        return SortSpec{*groupKey, group->ascending, SortSource::ExplorerLive};
    if (sortKey)
        return SortSpec{*sortKey, sort->ascending, SortSource::ExplorerLive};
    if (!sort && !group) // no sort column at all: Explorer lists by name
        return SortSpec{SortKey::Name, true, SortSource::ExplorerLive};
    return SortSpec{SortKey::Name, true, SortSource::ExplorerUnsupported};
}

} // namespace lf::explorer
