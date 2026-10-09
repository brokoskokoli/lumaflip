// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Introspection for tests of the FSEvents folder watcher (F-OPEN-07, F-WIN-09).

namespace lf::mac {

// Number of FSEvents streams currently open (one per watched folder, shared by all
// FolderWatcher instances). GUI thread only.
int activeFolderStreamCount();

} // namespace lf::mac
