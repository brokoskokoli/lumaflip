// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-VIEW-14: zoom and visible part per file for the running session (memory only, never on
// disk). A window remembers the view of every file the user zoomed or panned; coming back to
// the file restores it. The state is the one of ViewSync (mode, factor 1.0 = 100 %, center
// relative to the full image), so it does not depend on the window size, the screen or the
// decoded stage (preview, screen, full) and survives resizes and full screen changes.
//
// Bounded: the least recently used entry goes when `capacity` is reached. Pure logic.

#include "ViewSync.h"

#include <QHash>
#include <QString>

#include <optional>

namespace lf {

class ZoomMemory {
public:
    static constexpr int kDefaultCapacity = 500;
    explicit ZoomMemory(int capacity = kDefaultCapacity);

    // Remembers (or replaces) the view of `path` (page `page` of a multi-page file).
    void remember(const QString& path, int page, const ViewSyncState& state);
    // The remembered view; counts as a use (LRU).
    std::optional<ViewSyncState> recall(const QString& path, int page);
    bool contains(const QString& path, int page) const;
    // Forgets one page, or every page of `path` (page < 0): deleted, rewritten, reset.
    void forget(const QString& path, int page = -1);
    // F-FILE-04: the views follow a renamed file (all pages).
    void rename(const QString& from, const QString& to);
    void clear();

    int size() const { return int(m_entries.size()); }
    int capacity() const { return m_capacity; }
    void setCapacity(int capacity); // drops the least recently used entries above it

private:
    struct Entry {
        QString path;
        int page = 0;
        ViewSyncState state;
        quint64 used = 0;
    };
    static QString keyOf(const QString& path, int page);
    void evict();

    QHash<QString, Entry> m_entries;
    int m_capacity;
    quint64 m_clock = 0;
};

} // namespace lf
