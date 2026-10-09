// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Introspection for tests of the Finder-compatible name collator (F-SORT-01).

namespace lf::mac {

// How often name collators fetched the current locale (once initially, then after each
// NSCurrentLocaleDidChangeNotification). Thread-safe.
int nameCollatorLocaleFetches();

} // namespace lf::mac
