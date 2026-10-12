// SPDX-License-Identifier: GPL-3.0-or-later
// Explorer sort and group columns -> SortSpec (core/ExplorerSort.h, F-SORT-04 on Windows).
// Pure mapping, tested on every platform; the COM query is tested in test_windows_platform.cpp.

#include "core/ExplorerSort.h"

#include <gtest/gtest.h>

using namespace lf;
using namespace lf::explorer;

namespace {
const PropertyKey kAuthors{QUuid(0xf29f85e0, 0x4ff9, 0x1068, 0xab, 0x91, 0x08, 0x00, 0x2b, 0x27, 0xb3, 0xd9),
                           4};
const PropertyKey kDateAccessed{pkey::kStorage, 16};

ColumnSort col(const PropertyKey& k, bool asc) {
    return ColumnSort{k, asc};
}
} // namespace

TEST(ExplorerSort, F_SORT_04_PropertyKeysMapToSortKeys) {
    EXPECT_EQ(sortKeyFor(pkey::ItemNameDisplay), SortKey::Name);
    EXPECT_EQ(sortKeyFor(pkey::FileName), SortKey::Name);
    EXPECT_EQ(sortKeyFor(pkey::DateModified), SortKey::DateModified);
    EXPECT_EQ(sortKeyFor(pkey::DateCreated), SortKey::DateCreated);
    EXPECT_EQ(sortKeyFor(pkey::Size), SortKey::Size);
    EXPECT_EQ(sortKeyFor(pkey::ItemTypeText), SortKey::Kind);
    EXPECT_EQ(sortKeyFor(pkey::ItemType), SortKey::Kind);
    EXPECT_EQ(sortKeyFor(pkey::PhotoDateTaken), SortKey::DateCaptured);
    EXPECT_EQ(sortKeyFor(pkey::ItemDate), SortKey::DateCaptured);
    EXPECT_EQ(sortKeyFor(pkey::Rating), SortKey::Rating);
    EXPECT_FALSE(sortKeyFor(kAuthors).has_value());
    EXPECT_FALSE(sortKeyFor(kDateAccessed).has_value());
    EXPECT_FALSE(sortKeyFor(PropertyKey{}).has_value());
    // Same property ID, other format ID: not the same column.
    EXPECT_FALSE(sortKeyFor(PropertyKey{pkey::ItemDate.fmtid, 10}).has_value());
}

TEST(ExplorerSort, F_SORT_04_SortColumnAndDirection) {
    EXPECT_EQ(specForView(col(pkey::DateModified, false), std::nullopt),
              (SortSpec{SortKey::DateModified, false, SortSource::ExplorerLive}));
    EXPECT_EQ(specForView(col(pkey::ItemNameDisplay, true), std::nullopt),
              (SortSpec{SortKey::Name, true, SortSource::ExplorerLive}));
    EXPECT_EQ(specForView(col(pkey::Size, true), std::nullopt),
              (SortSpec{SortKey::Size, true, SortSource::ExplorerLive}));
    // No sort column: Explorer lists by name.
    EXPECT_EQ(specForView(std::nullopt, std::nullopt),
              (SortSpec{SortKey::Name, true, SortSource::ExplorerLive}));
    // A column Lumaflip cannot sort by: name, and the status says so.
    EXPECT_EQ(specForView(col(kAuthors, false), std::nullopt),
              (SortSpec{SortKey::Name, true, SortSource::ExplorerUnsupported}));
}

TEST(ExplorerSort, F_SORT_04_GroupingDecidesWhenUsable) {
    // Grouped by the sort column (Downloads: date modified): one order.
    EXPECT_EQ(specForView(col(pkey::DateModified, false), col(pkey::DateModified, false)),
              (SortSpec{SortKey::DateModified, false, SortSource::ExplorerLive}));
    // Grouped by type, sorted by name: the groups come first.
    EXPECT_EQ(specForView(col(pkey::ItemNameDisplay, true), col(pkey::ItemTypeText, false)),
              (SortSpec{SortKey::Kind, false, SortSource::ExplorerLive}));
    // Grouped by a column Lumaflip cannot sort by: the sort column.
    EXPECT_EQ(specForView(col(pkey::Size, false), col(kAuthors, true)),
              (SortSpec{SortKey::Size, false, SortSource::ExplorerLive}));
    // Sort column unusable, group usable: the group.
    EXPECT_EQ(specForView(col(kAuthors, true), col(pkey::DateCreated, true)),
              (SortSpec{SortKey::DateCreated, true, SortSource::ExplorerLive}));
    EXPECT_EQ(specForView(col(kAuthors, true), col(kDateAccessed, true)),
              (SortSpec{SortKey::Name, true, SortSource::ExplorerUnsupported}));
}
