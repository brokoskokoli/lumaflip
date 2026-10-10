// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-GRID-02 grid view logic without widgets: the thumbnail size ladder (⌘+ / ⌘− / pinch),
// columns and keyboard movement, and the multi-selection (click, ⌘-click, ⇧-click,
// ⇧-arrows, ⌘A) that follows the collection by path when it changes (sorting, filters,
// files trashed by another window).

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lf {

namespace grid {

// Thumbnail edge lengths (logical px) the grid steps through; the default is 160.
const QVector<int>& sizeSteps();
inline constexpr int kDefaultSize = 160;
// The size `steps` ladder steps away from `current` (a size between two steps snaps to
// the next one in that direction), clamped to the ladder.
int stepSize(int current, int steps);
// Continuous pinch: `current` scaled by `factor`, clamped to the ladder's range.
int scaledSize(int current, double factor);
// Columns that fit into `viewportWidth` with cells of `cellWidth` (at least 1).
int columnsFor(int viewportWidth, int cellWidth);

enum class Move { Left, Right, Up, Down, PageUp, PageDown, Home, End };
// The index after a cursor key. Left/Right walk the reading order (wrapping into the
// previous/next row), Up/Down keep the column (Down on the last full row goes to the last
// item), Page moves by whole rows. -1 for an empty grid.
int moveIndex(int index, Move move, int columns, int count, int rowsPerPage);

} // namespace grid

class GridSelection {
public:
    // New contents; the cursor and the selection become `cursorPath` (if present).
    void reset(const QStringList& paths, const QString& cursorPath);
    // The collection changed: selection and cursor follow their paths; a cursor whose file
    // left moves to the item now at its old position (clamped).
    void setPaths(const QStringList& paths);
    const QStringList& paths() const { return m_paths; }
    int count() const { return int(m_paths.size()); }

    int cursor() const { return m_cursor; } // -1 = none
    QString cursorPath() const;
    int anchor() const { return m_anchor; }
    bool isSelected(int row) const;
    int selectedCount() const { return int(m_selected.size()); }
    QList<int> selectedRows() const;   // ascending
    QStringList selectedPaths() const; // collection order

    // Mouse (Qt on macOS: ControlModifier = ⌘): plain = only this item, ⌘ = toggle it,
    // ⇧ = range from the anchor (replaces the selection), ⌘⇧ = add that range.
    void click(int row, bool command, bool shift);
    // Keyboard: cursor to `row`; `extend` (⇧) selects the range from the anchor.
    void moveTo(int row, bool extend);
    void selectAll();
    void clear(); // nothing selected, the cursor stays

private:
    void selectRange(int from, int to, bool add);

    QStringList m_paths;
    QSet<QString> m_selected;
    int m_cursor = -1;
    int m_anchor = -1;
};

} // namespace lf
