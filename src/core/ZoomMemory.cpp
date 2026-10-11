// SPDX-License-Identifier: GPL-3.0-or-later
#include "ZoomMemory.h"

#include <QList>

#include <algorithm>
#include <iterator>

namespace lf {

ZoomMemory::ZoomMemory(int capacity) : m_capacity(std::max(1, capacity)) {}

QString ZoomMemory::keyOf(const QString& path, int page) {
    // '\n' never occurs in file names shown here, so path and page cannot collide.
    return QString::number(std::max(0, page)) + QLatin1Char('\n') + path;
}

void ZoomMemory::remember(const QString& path, int page, const ViewSyncState& state) {
    if (path.isEmpty())
        return;
    Entry& e = m_entries[keyOf(path, page)];
    e.path = path;
    e.page = std::max(0, page);
    e.state = state;
    e.used = ++m_clock;
    evict();
}

std::optional<ViewSyncState> ZoomMemory::recall(const QString& path, int page) {
    auto it = m_entries.find(keyOf(path, page));
    if (it == m_entries.end())
        return std::nullopt;
    it->used = ++m_clock;
    return it->state;
}

bool ZoomMemory::contains(const QString& path, int page) const {
    return m_entries.contains(keyOf(path, page));
}

void ZoomMemory::forget(const QString& path, int page) {
    if (page >= 0) {
        m_entries.remove(keyOf(path, page));
        return;
    }
    for (auto it = m_entries.begin(); it != m_entries.end();)
        it = it->path == path ? m_entries.erase(it) : std::next(it);
}

void ZoomMemory::rename(const QString& from, const QString& to) {
    if (from == to || from.isEmpty() || to.isEmpty())
        return;
    QList<Entry> moved;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->path == from) {
            moved << *it;
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
    forget(to); // views of a file that was replaced by the rename are outdated
    for (Entry e : std::as_const(moved)) {
        e.path = to;
        m_entries.insert(keyOf(to, e.page), e);
    }
}

void ZoomMemory::clear() {
    m_entries.clear();
}

void ZoomMemory::setCapacity(int capacity) {
    m_capacity = std::max(1, capacity);
    evict();
}

void ZoomMemory::evict() {
    while (m_entries.size() > m_capacity) {
        auto oldest = m_entries.begin();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
            if (it->used < oldest->used)
                oldest = it;
        m_entries.erase(oldest);
    }
}

} // namespace lf
