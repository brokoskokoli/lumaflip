// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows Explorer sort columns -> SortSpec (SPEC 6.1, F-SORT-04 on Windows). Pure and
// portable so it is tested on every platform; the COM query that reads the columns of the
// Explorer window lives in src/platform/windows/WinExplorerOrder.cpp.

#include "SortSpec.h"

#include <QUuid>

#include <optional>

namespace lf::explorer {

// A Windows PROPERTYKEY (propkey.h): format ID and property ID.
struct PropertyKey {
    QUuid fmtid;
    quint32 pid = 0;
    friend bool operator==(const PropertyKey&, const PropertyKey&) = default;
};

// The property keys Lumaflip can sort by (values from propkey.h; the Windows unit test
// compares them with the SDK's PKEY_* constants).
namespace pkey {
// FMTID_Storage {B725F130-47EF-101A-A5F1-02608C9EEBAC}
inline constexpr QUuid kStorage{0xb725f130, 0x47ef, 0x101a, 0xa5, 0xf1, 0x02, 0x60, 0x8c, 0x9e, 0xeb, 0xac};
inline const PropertyKey ItemNameDisplay{kStorage, 10};
inline const PropertyKey Size{kStorage, 12};
inline const PropertyKey DateModified{kStorage, 14};
inline const PropertyKey DateCreated{kStorage, 15};
inline const PropertyKey ItemTypeText{kStorage, 4};
inline const PropertyKey FileName{
    {0x41cf5ae0, 0xf75a, 0x4806, 0xbd, 0x87, 0x59, 0xc7, 0xd9, 0x24, 0x8e, 0xb9}, 100};
inline const PropertyKey ItemType{
    {0x28636aa6, 0x953d, 0x11d2, 0xb5, 0xd6, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0xd0}, 11};
inline const PropertyKey ItemDate{
    {0xf7db74b4, 0x4287, 0x4103, 0xaf, 0xba, 0xf1, 0xb1, 0x3d, 0xcd, 0x75, 0xcf}, 100};
inline const PropertyKey PhotoDateTaken{
    {0x14b81da1, 0x0135, 0x4d31, 0x96, 0xd9, 0x6c, 0xbf, 0xc9, 0x67, 0x1a, 0x99}, 36867};
inline const PropertyKey Rating{{0x64440492, 0x4c8b, 0x11d1, 0x8b, 0x70, 0x08, 0x00, 0x2b, 0x2c, 0xd3, 0xb6},
                                9};
} // namespace pkey

// The SortKey for an Explorer column, nullopt for columns Lumaflip cannot sort by (Tags,
// Authors, Dimensions, Date accessed …).
std::optional<SortKey> sortKeyFor(const PropertyKey& key);

struct ColumnSort {
    PropertyKey key;
    bool ascending = true; // SORT_ASCENDING: increasing values (A→Z, oldest/smallest first)
};

// The order of an Explorer view: `sort` = its first sort column (nullopt if none), `group`
// = its "Group by" column (nullopt if not grouped). Explorer lists groups first and sorts
// inside each group; when the group column is one Lumaflip can sort by and differs from
// the sort column, the group column decides (the order of the groups is what one sees
// first), otherwise the sort column. Neither usable: name ascending, ExplorerUnsupported.
SortSpec specForView(const std::optional<ColumnSort>& sort, const std::optional<ColumnSort>& group);

} // namespace lf::explorer
