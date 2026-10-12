// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sort order of the Explorer window that shows a folder (SPEC 6.1, F-SORT-04 on
// Windows): IShellWindows → IShellBrowser → IFolderView2 (sort and group columns).

#include "core/ExplorerSort.h"
#include "core/SortSpec.h"

#include <QString>

#include <optional>

namespace lf::win {

// Limit for one query: Explorer answers in its own process; a hung Explorer must not hold
// up opening a folder.
inline constexpr int kExplorerQueryTimeoutMs = 500;

// The order of the Explorer window showing exactly `folderPath` (several: the visible tab of
// the window highest in the z-order, i.e. the one last active). nullopt if no Explorer
// window shows it, Explorer does not answer within `timeoutMs` or COM fails. Blocking: call
// on a worker thread; the COM calls run on a thread of their own (STA).
std::optional<SortSpec> explorerOrder(const QString& folderPath, int timeoutMs = kExplorerQueryTimeoutMs);

// For tests: the same query, also reporting how many Explorer windows were looked at
// (-1: IShellWindows not available).
std::optional<SortSpec> explorerOrder(const QString& folderPath, int timeoutMs, int* windowsSeen);

} // namespace lf::win
