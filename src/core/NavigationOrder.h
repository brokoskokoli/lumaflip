// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-NAV-06: "Gehe zu Bild n" (⌘G) and the shuffled browsing order of a window. Pure logic.
//
// The shuffled order sorts the paths by a key derived from a seed and the path, so it is
// the same for the whole session (one seed per app run) and stays stable when files are
// added or removed: every other file keeps its place relative to the rest.

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace lf {

namespace nav {
// Text of the ⌘G field -> 0-based index: "12" (1-based position), "+5" / "-3" (relative to
// `current`, 0-based), clamped to the collection; nullopt if it is no number or `count` is 0.
std::optional<int> parseGoTo(const QString& text, int current, int count);
} // namespace nav

class ShuffleOrder {
public:
    explicit ShuffleOrder(quint64 seed = sessionSeed());
    // One random seed per process (the same for every window of the session).
    static quint64 sessionSeed();

    quint64 seed() const { return m_seed; }
    void setPaths(const QStringList& paths);
    const QStringList& order() const { return m_order; }
    int positionOf(const QString& path) const; // -1 if unknown

    // The path `delta` steps from `from` in shuffled order; `wrap` continues at the other
    // end. Without wrap the result is clamped and `*hitEnd` set when the end stopped it.
    // `from` not in the order: starts at the first (delta > 0) or last path.
    QString step(const QString& from, int delta, bool wrap, bool* hitEnd = nullptr) const;
    QString first() const { return m_order.isEmpty() ? QString() : m_order.first(); }
    QString last() const { return m_order.isEmpty() ? QString() : m_order.last(); }

    quint64 keyOf(const QString& path) const; // sort key (tests)

private:
    quint64 m_seed;
    QStringList m_order;
    QHash<QString, int> m_pos;
};

} // namespace lf
