// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-11 without AppKit: the app keeps its own list (RecentItems.cpp); there is no
// system-wide list to add to (the freedesktop recently-used.xbel is left to the file
// managers).

#include "app/RecentItems.h"

namespace lf {

bool runsAsAppBundle() {
    return false;
}

void noteSystemRecentDocument(const QString&) {}

void clearSystemRecentDocuments() {}

} // namespace lf
