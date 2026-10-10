// SPDX-License-Identifier: GPL-3.0-or-later
#include "GridSelection.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace lf {

namespace grid {

const QVector<int>& sizeSteps() {
    static const QVector<int> steps = {80, 112, 136, 160, 200, 256, 320, 400};
    return steps;
}

int stepSize(int current, int steps) {
    const QVector<int>& s = sizeSteps();
    if (steps == 0)
        return std::clamp(current, s.first(), s.last());
    if (steps > 0) {
        auto it = std::upper_bound(s.begin(), s.end(), current); // first step > current
        int i = int(it - s.begin()) + steps - 1;
        return s.at(std::clamp(i, 0, int(s.size()) - 1));
    }
    auto it = std::lower_bound(s.begin(), s.end(), current); // first step >= current
    int i = int(it - s.begin()) + steps;
    return s.at(std::clamp(i, 0, int(s.size()) - 1));
}

int scaledSize(int current, double factor) {
    const QVector<int>& s = sizeSteps();
    if (!(factor > 0))
        return std::clamp(current, s.first(), s.last());
    return std::clamp(int(std::lround(current * factor)), s.first(), s.last());
}

int columnsFor(int viewportWidth, int cellWidth) {
    if (cellWidth <= 0)
        return 1;
    return std::max(1, viewportWidth / cellWidth);
}

int moveIndex(int index, Move move, int columns, int count, int rowsPerPage) {
    if (count <= 0)
        return -1;
    columns = std::max(1, columns);
    rowsPerPage = std::max(1, rowsPerPage);
    if (index < 0 || index >= count)
        return move == Move::End ? count - 1 : 0;
    switch (move) {
    case Move::Left:
        return std::max(0, index - 1);
    case Move::Right:
        return std::min(count - 1, index + 1);
    case Move::Up:
        return index - columns >= 0 ? index - columns : index;
    case Move::Down:
        if (index + columns < count)
            return index + columns;
        // Last (partial) row below: go to its last item, like Finder; already there: stay.
        return (index / columns) < ((count - 1) / columns) ? count - 1 : index;
    case Move::PageUp:
        return std::max(index % columns, index - columns * rowsPerPage);
    case Move::PageDown: {
        const int to = index + columns * rowsPerPage;
        return to < count ? to : count - 1;
    }
    case Move::Home:
        return 0;
    case Move::End:
        return count - 1;
    }
    return index;
}

} // namespace grid

void GridSelection::reset(const QStringList& paths, const QString& cursorPath) {
    m_paths = paths;
    m_selected.clear();
    m_cursor = int(m_paths.indexOf(cursorPath));
    if (m_cursor < 0 && !m_paths.isEmpty())
        m_cursor = 0;
    m_anchor = m_cursor;
    if (m_cursor >= 0)
        m_selected.insert(m_paths.at(m_cursor));
}

void GridSelection::setPaths(const QStringList& paths) {
    const QString cursorPath = this->cursorPath();
    const QString anchorPath = m_anchor >= 0 && m_anchor < m_paths.size() ? m_paths.at(m_anchor) : QString();
    const int oldCursor = m_cursor;
    m_paths = paths;
    QSet<QString> present(m_paths.cbegin(), m_paths.cend());
    m_selected.removeIf([&](const QString& p) { return !present.contains(p); });
    m_cursor = cursorPath.isEmpty() ? -1 : int(m_paths.indexOf(cursorPath));
    if (m_cursor < 0 && !m_paths.isEmpty())
        m_cursor = std::clamp(oldCursor, 0, int(m_paths.size()) - 1);
    m_anchor = anchorPath.isEmpty() ? -1 : int(m_paths.indexOf(anchorPath));
    if (m_anchor < 0)
        m_anchor = m_cursor;
}

QString GridSelection::cursorPath() const {
    return m_cursor >= 0 && m_cursor < m_paths.size() ? m_paths.at(m_cursor) : QString();
}

bool GridSelection::isSelected(int row) const {
    return row >= 0 && row < m_paths.size() && m_selected.contains(m_paths.at(row));
}

QList<int> GridSelection::selectedRows() const {
    QList<int> rows;
    for (int i = 0; i < m_paths.size(); ++i)
        if (m_selected.contains(m_paths.at(i)))
            rows << i;
    return rows;
}

QStringList GridSelection::selectedPaths() const {
    QStringList out;
    for (const QString& p : m_paths)
        if (m_selected.contains(p))
            out << p;
    return out;
}

void GridSelection::selectRange(int from, int to, bool add) {
    if (!add)
        m_selected.clear();
    if (from > to)
        std::swap(from, to);
    from = std::max(0, from);
    to = std::min(int(m_paths.size()) - 1, to);
    for (int i = from; i <= to; ++i)
        m_selected.insert(m_paths.at(i));
}

void GridSelection::click(int row, bool command, bool shift) {
    if (row < 0 || row >= m_paths.size())
        return;
    const QString& p = m_paths.at(row);
    if (shift) {
        const int anchor = m_anchor >= 0 ? m_anchor : row;
        selectRange(anchor, row, command);
        m_cursor = row; // the anchor stays for the next ⇧-click
        m_anchor = anchor;
        return;
    }
    if (command) {
        if (m_selected.contains(p))
            m_selected.remove(p);
        else
            m_selected.insert(p);
    } else {
        m_selected.clear();
        m_selected.insert(p);
    }
    m_cursor = row;
    m_anchor = row;
}

void GridSelection::moveTo(int row, bool extend) {
    if (row < 0 || row >= m_paths.size())
        return;
    if (extend) {
        const int anchor = m_anchor >= 0 ? m_anchor : (m_cursor >= 0 ? m_cursor : row);
        selectRange(anchor, row, false);
        m_anchor = anchor;
    } else {
        m_selected.clear();
        m_selected.insert(m_paths.at(row));
        m_anchor = row;
    }
    m_cursor = row;
}

void GridSelection::selectAll() {
    m_selected = QSet<QString>(m_paths.cbegin(), m_paths.cend());
}

void GridSelection::clear() {
    m_selected.clear();
}

} // namespace lf
