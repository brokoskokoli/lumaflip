// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Filmstrip (F-GRID-01): one row of thumbnails of the window's collection below the
// image. Virtualized (QListView over a model of the collection): only items that are
// painted request their thumbnail from the ThumbnailCache, requests that scroll out of
// view are dropped. The current image is highlighted and kept in view; a click makes
// an image current.

#include <QAbstractListModel>
#include <QHash>
#include <QListView>
#include <QSet>
#include <QStyledItemDelegate>

class QTimer;

namespace lf {

class Collection;
class ThumbnailCache;

class FilmstripModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { PathRole = Qt::UserRole + 1, CurrentRole, ThumbnailRole, FailedRole };

    FilmstripModel(Collection& collection, ThumbnailCache& cache, QObject* parent = nullptr);
    ~FilmstripModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    // ThumbnailRole requests the thumbnail (only the delegate asks, for painted items).
    QVariant data(const QModelIndex& index, int role) const override;

    int thumbSize() const { return m_thumbSize; } // device pixels of the longer edge
    void setThumbSize(int px);
    int currentRow() const { return m_current; }
    int requestCount() const { return m_requests; } // thumbnail requests (tests)
    const void* client() const { return this; }

signals:
    void currentRowChanged(int row);

private:
    void rebuild();
    void onCurrentChanged(int index);
    void onThumbnailReady(const QString& path, int thumbSize);
    void onThumbnailFailed(const QString& path, int thumbSize);

    Collection& m_collection;
    ThumbnailCache& m_cache;
    QHash<QString, int> m_rows;                     // path -> row
    QHash<QString, QPair<qint64, qint64>> m_stamps; // path -> (size, mtime ms) at the last rebuild
    QSet<QString> m_failed;                         // no thumbnail possible (unreadable file): error marker
    int m_current = -1;
    int m_thumbSize = 192;
    mutable int m_requests = 0;
};

class FilmstripDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setCellSize(QSize size) { m_cell = size; }

private:
    QSize m_cell{96, 76};
};

class Filmstrip : public QListView {
    Q_OBJECT
public:
    Filmstrip(Collection& collection, ThumbnailCache& cache, QWidget* parent = nullptr);
    ~Filmstrip() override;

    FilmstripModel* filmModel() const { return m_model; }
    QSize cellSize() const { return m_cell; }
    // Rows currently (at least partly) visible.
    QList<int> visibleRows() const;
    // Scrolls so the current image is visible (after collection changes).
    void scrollToCurrent();
    QSize sizeHint() const override;

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateThumbSize();
    void scheduleRetain();
    void retainVisible();

    Collection& m_collection;
    ThumbnailCache& m_cache;
    FilmstripModel* m_model = nullptr;
    FilmstripDelegate* m_delegate = nullptr;
    QTimer* m_retainTimer = nullptr;
    QSize m_cell{96, 76};
};

} // namespace lf
