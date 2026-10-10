// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Grid view (F-GRID-02): every image of the window's collection as a thumbnail grid.
// Virtualized like the filmstrip (FilmstripModel: only painted cells ask the ThumbnailCache,
// requests that scroll out of view are dropped). Thumbnail size by pinch, ⌘+ / ⌘− (on the
// ladder of core/GridSelection.h); multi-selection with click, ⌘-click, ⇧-click, ⇧-arrows
// and ⌘A (GridSelection). Each cell shows the rating badge (stars) and pick/reject flag.
// Enter, ⌘↓ or a double click opens the image under the cursor; Esc closes the grid.
// Keys the grid does not handle go on to the window (rating, trash, quick move …).

#include "Filmstrip.h"
#include "core/GridSelection.h"

#include <QListView>
#include <QStyledItemDelegate>

#include <functional>

class QTimer;

namespace lf {

class Collection;
class ThumbnailCache;

class GridDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setThumbSize(int logicalPx) { m_thumb = logicalPx; }
    QSize cellSize() const;
    void setSelection(const GridSelection* selection) { m_selection = selection; }

private:
    int m_thumb = 160;
    const GridSelection* m_selection = nullptr;
};

class GridView : public QListView {
    Q_OBJECT
public:
    GridView(Collection& collection, ThumbnailCache& cache, QWidget* parent = nullptr);
    ~GridView() override;

    FilmstripModel* gridModel() const { return m_model; }
    const GridSelection& selection() const { return m_selection; }
    QStringList selectedPaths() const { return m_selection.selectedPaths(); }
    QString cursorPath() const { return m_selection.cursorPath(); }

    // Shown: the cursor and the selection are the collection's current image.
    void activate();
    int thumbSize() const { return m_thumb; } // logical px of the longer thumbnail edge
    void setThumbSize(int logicalPx);         // clamped to the ladder, remembered
    void zoomStep(int steps);
    int columns() const;
    QList<int> visibleRows() const;

    // Selection for tests and the window's commands.
    void clickRow(int row, Qt::KeyboardModifiers modifiers);
    void moveGridCursor(grid::Move move, bool extend);
    void selectAllItems();
    // Keys taken from the menu shortcuts while the grid has the focus (e.g. rating digits,
    // which are zoom shortcuts in the viewer); the window decides.
    void setExtraKeys(std::function<bool(const QKeyEvent*)> wanted) { m_extraKeys = std::move(wanted); }

signals:
    void openRequested(const QString& path);
    void closeRequested();
    void gridSelectionChanged();
    void thumbSizeChanged(int logicalPx);

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void syncPaths();
    void updateThumbRequestSize();
    void scheduleRetain();
    void retainVisible();
    void cursorChanged(bool scroll);
    int rowsPerPage() const;
    bool handlesKey(const QKeyEvent* event) const;

    Collection& m_collection;
    ThumbnailCache& m_cache;
    FilmstripModel* m_model = nullptr;
    GridDelegate* m_delegate = nullptr;
    GridSelection m_selection;
    QTimer* m_retainTimer = nullptr;
    int m_thumb = grid::kDefaultSize;
    double m_pinch = 1.0; // accumulated pinch of one gesture
    int m_pinchBase = grid::kDefaultSize;
    std::function<bool(const QKeyEvent*)> m_extraKeys;
};

} // namespace lf
