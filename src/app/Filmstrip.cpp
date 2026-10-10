// SPDX-License-Identifier: GPL-3.0-or-later
#include "Filmstrip.h"

#include "core/Collection.h"
#include "core/ThumbnailCache.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

constexpr int kRetainDelayMs = 60;
constexpr int kStripMargin = 12; // vertical room around the cells (scroll bar overlay)

bool isDark(const QPalette& p) {
    return p.color(QPalette::Window).lightnessF() < 0.5;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------

FilmstripModel::FilmstripModel(Collection& collection, ThumbnailCache& cache, QObject* parent)
    : QAbstractListModel(parent), m_collection(collection), m_cache(cache) {
    connect(&m_collection, &Collection::entriesChanged, this, &FilmstripModel::rebuild);
    connect(&m_collection, &Collection::loadingFinished, this, &FilmstripModel::rebuild);
    connect(&m_collection, &Collection::currentChanged, this,
            [this](int index, const QString&) { onCurrentChanged(index); });
    connect(&m_cache, &ThumbnailCache::thumbnailReady, this, &FilmstripModel::onThumbnailReady);
    connect(&m_cache, &ThumbnailCache::thumbnailFailed, this, &FilmstripModel::onThumbnailReady);
    rebuild();
}

FilmstripModel::~FilmstripModel() {
    m_cache.removeClient(this);
}

int FilmstripModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant FilmstripModel::data(const QModelIndex& index, int role) const {
    const int row = index.row();
    if (!index.isValid() || row < 0 || row >= m_rows.size() || row >= m_collection.count())
        return {};
    const FileEntry& e = m_collection.at(row);
    switch (role) {
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
    case Qt::AccessibleTextRole:
        return e.name;
    case PathRole:
        return e.path;
    case CurrentRole:
        return row == m_current;
    case ThumbnailRole:
        ++m_requests;
        return m_cache.request(e.path, m_thumbSize, this);
    default:
        return {};
    }
}

void FilmstripModel::setThumbSize(int px) {
    if (px == m_thumbSize || px <= 0)
        return;
    m_thumbSize = px;
    m_cache.removeClient(this);
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(int(m_rows.size()) - 1), {ThumbnailRole});
}

void FilmstripModel::rebuild() {
    // Insertions, removals and re-sorting all arrive as entriesChanged(): the model is
    // reset (cheap: the view asks only for visible rows again).
    beginResetModel();
    m_rows.clear();
    QHash<QString, QPair<qint64, qint64>> stamps;
    const QVector<FileEntry>& entries = m_collection.entries();
    m_rows.reserve(entries.size());
    stamps.reserve(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const FileEntry& e = entries[i];
        m_rows.insert(e.path, i);
        const QPair<qint64, qint64> stamp{e.size, e.modified.isValid() ? e.modified.toMSecsSinceEpoch() : -1};
        auto old = m_stamps.constFind(e.path);
        // A file that changed on disk gets a new thumbnail.
        if (old != m_stamps.cend() && *old != stamp && old->first >= 0 && stamp.first >= 0)
            m_cache.invalidate(e.path);
        stamps.insert(e.path, stamp);
    }
    m_stamps = std::move(stamps);
    m_current = m_collection.currentIndex();
    endResetModel();
    emit currentRowChanged(m_current);
}

void FilmstripModel::onCurrentChanged(int index) {
    const int old = m_current;
    m_current = index;
    if (old == index)
        return;
    for (int r : {old, index})
        if (r >= 0 && r < m_rows.size())
            emit dataChanged(this->index(r), this->index(r), {CurrentRole});
    emit currentRowChanged(index);
}

void FilmstripModel::onThumbnailReady(const QString& path, int thumbSize) {
    if (thumbSize != m_thumbSize)
        return;
    const int row = m_rows.value(path, -1);
    if (row >= 0)
        emit dataChanged(index(row), index(row), {ThumbnailRole});
}

// ---------------------------------------------------------------------------------------
// Delegate
// ---------------------------------------------------------------------------------------

QSize FilmstripDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return m_cell;
}

void FilmstripDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QPalette& pal = option.palette;
    const bool dark = isDark(pal);
    const bool current = index.data(FilmstripModel::CurrentRole).toBool();
    const bool hover = option.state & QStyle::State_MouseOver;
    const QRectF cell = QRectF(option.rect).adjusted(2, 2, -2, -2);
    const QRectF inner = cell.adjusted(4, 4, -4, -4);

    if (current || hover) {
        QColor bg = current ? pal.color(QPalette::Highlight) : pal.color(QPalette::Text);
        bg.setAlpha(current ? 70 : 26);
        painter->setPen(Qt::NoPen);
        painter->setBrush(bg);
        painter->drawRoundedRect(cell, 6, 6);
    }

    const QImage thumb = index.data(FilmstripModel::ThumbnailRole).value<QImage>();
    QRectF target = inner;
    if (!thumb.isNull()) {
        const QSizeF s = QSizeF(thumb.size()).scaled(inner.size(), Qt::KeepAspectRatio);
        target = QRectF(inner.center() - QPointF(s.width() / 2, s.height() / 2), s);
        painter->drawImage(target, thumb);
    } else {
        // Placeholder while the thumbnail is made (or if the file cannot be read).
        const QSizeF s = QSizeF(4, 3).scaled(inner.size(), Qt::KeepAspectRatio);
        target = QRectF(inner.center() - QPointF(s.width() / 2, s.height() / 2), s);
        painter->setPen(Qt::NoPen);
        painter->setBrush(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 18));
        painter->drawRoundedRect(target, 3, 3);
    }
    if (current) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(pal.color(QPalette::Highlight), 2.5));
        painter->drawRoundedRect(target.adjusted(-2, -2, 2, 2), 3, 3);
    }
    painter->restore();
}

// ---------------------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------------------

Filmstrip::Filmstrip(Collection& collection, ThumbnailCache& cache, QWidget* parent)
    : QListView(parent), m_collection(collection), m_cache(cache) {
    setObjectName(QStringLiteral("lfFilmstrip"));
    setAccessibleName(tr("Filmstreifen"));
    m_model = new FilmstripModel(collection, cache, this);
    m_delegate = new FilmstripDelegate(this);
    m_delegate->setCellSize(m_cell);
    setItemDelegate(m_delegate);
    setModel(m_model);
    setViewMode(QListView::ListMode);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setUniformItemSizes(true); // virtualized: no per-item size queries
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::NoFocus); // arrow keys keep flipping images
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setFixedHeight(m_cell.height() + kStripMargin);

    m_retainTimer = new QTimer(this);
    m_retainTimer->setSingleShot(true);
    m_retainTimer->setInterval(kRetainDelayMs);
    connect(m_retainTimer, &QTimer::timeout, this, &Filmstrip::retainVisible);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &Filmstrip::scheduleRetain);
    connect(m_model, &FilmstripModel::currentRowChanged, this, [this] { scrollToCurrent(); });
    connect(m_model, &QAbstractItemModel::modelReset, this, &Filmstrip::scheduleRetain);
}

Filmstrip::~Filmstrip() = default;

QSize Filmstrip::sizeHint() const {
    return {m_cell.width() * 6, m_cell.height() + kStripMargin};
}

QList<int> Filmstrip::visibleRows() const {
    QList<int> rows;
    const int n = m_model->rowCount();
    if (n == 0 || !isVisible())
        return rows;
    const QRect vp = viewport()->rect();
    const int y = vp.center().y();
    QModelIndex first = indexAt(QPoint(vp.left() + 1, y));
    QModelIndex last = indexAt(QPoint(vp.right() - 1, y));
    const int from = first.isValid() ? first.row() : 0;
    const int to = last.isValid() ? last.row() : n - 1;
    for (int r = from; r <= to && r < n; ++r)
        if (visualRect(m_model->index(r)).intersects(vp))
            rows << r;
    return rows;
}

void Filmstrip::scrollToCurrent() {
    const int row = m_model->currentRow();
    if (row < 0 || row >= m_model->rowCount())
        return;
    scrollTo(m_model->index(row), QAbstractItemView::PositionAtCenter);
}

void Filmstrip::scheduleRetain() {
    m_retainTimer->start();
}

void Filmstrip::retainVisible() {
    // Thumbnails that scrolled out of view are not made any more (only visible ones).
    QStringList paths;
    for (int r : visibleRows())
        paths << m_model->index(r).data(FilmstripModel::PathRole).toString();
    m_cache.retain(m_model->client(), paths);
}

void Filmstrip::updateThumbSize() {
    const qreal dpr = window() && window()->windowHandle() ? window()->windowHandle()->devicePixelRatio()
                                                           : devicePixelRatioF();
    const int want = int(std::ceil(std::max(m_cell.width(), m_cell.height()) * dpr));
    // A few fixed sizes keep the disk cache shared between displays.
    int px = 384;
    for (int b : {128, 192, 256, 384})
        if (want <= b) {
            px = b;
            break;
        }
    m_model->setThumbSize(px);
}

void Filmstrip::wheelEvent(QWheelEvent* event) {
    // A vertical wheel scrolls the strip sideways; it never flips images.
    const QPoint pd = event->pixelDelta();
    const QPoint ad = event->angleDelta();
    int dx = 0;
    if (!pd.isNull())
        dx = std::abs(pd.x()) > std::abs(pd.y()) ? pd.x() : pd.y();
    else
        dx = int(
            std::lround((std::abs(ad.x()) > std::abs(ad.y()) ? ad.x() : ad.y()) / 120.0 * m_cell.width()));
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - dx);
    event->accept();
}

void Filmstrip::resizeEvent(QResizeEvent* event) {
    QListView::resizeEvent(event);
    scheduleRetain();
}

void Filmstrip::showEvent(QShowEvent* event) {
    QListView::showEvent(event);
    updateThumbSize();
    QTimer::singleShot(0, this, [this] { scrollToCurrent(); });
    scheduleRetain();
}

void Filmstrip::hideEvent(QHideEvent* event) {
    QListView::hideEvent(event);
    m_retainTimer->stop();
    m_cache.retain(m_model->client(), {}); // nothing is visible
}

void Filmstrip::changeEvent(QEvent* event) {
    QListView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        viewport()->update();
}

void Filmstrip::mousePressEvent(QMouseEvent* event) {
    // Click to jump; no selection, no keyboard focus.
    const QModelIndex idx = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && idx.isValid())
        m_collection.setCurrentIndex(idx.row());
    event->accept();
}

} // namespace lf
