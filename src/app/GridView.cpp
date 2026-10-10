// SPDX-License-Identifier: GPL-3.0-or-later
#include "GridView.h"

#include "core/Collection.h"
#include "core/Rating.h"
#include "core/Settings.h"
#include "core/ThumbnailCache.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
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
constexpr int kPad = 8;      // around the thumbnail inside a cell
constexpr int kTextLine = 18; // file name below the thumbnail

bool isDark(const QPalette& p) {
    return p.color(QPalette::Base).lightnessF() < 0.5;
}

void drawBadge(QPainter* p, const QRectF& r, const QColor& fill, const QString& text) {
    QPainterPath path;
    path.addRoundedRect(r, r.height() / 2, r.height() / 2);
    p->fillPath(path, fill);
    p->setPen(Qt::white);
    p->drawText(r, Qt::AlignCenter, text);
}

} // namespace

// ---------------------------------------------------------------------------------------
// Delegate
// ---------------------------------------------------------------------------------------

QSize GridDelegate::cellSize() const {
    return {m_thumb + 2 * kPad, m_thumb + 2 * kPad + kTextLine};
}

QSize GridDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const {
    return cellSize();
}

void GridDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QPalette& pal = option.palette;
    const bool dark = isDark(pal);
    const int row = index.row();
    const bool selected = m_selection && m_selection->isSelected(row);
    const bool cursor = m_selection && m_selection->cursor() == row;
    const bool hover = option.state & QStyle::State_MouseOver;
    const QRectF cell = QRectF(option.rect).adjusted(3, 3, -3, -3);
    if (selected || hover) {
        QColor bg = selected ? pal.color(QPalette::Highlight) : pal.color(QPalette::Text);
        bg.setAlpha(selected ? 90 : 22);
        painter->setPen(Qt::NoPen);
        painter->setBrush(bg);
        painter->drawRoundedRect(cell, 8, 8);
    }
    if (cursor && (option.state & QStyle::State_HasFocus || m_selection->selectedCount() != 1)) {
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(pal.color(QPalette::Highlight), 2));
        painter->drawRoundedRect(cell.adjusted(1, 1, -1, -1), 8, 8);
    }

    const QRectF inner(option.rect.left() + kPad, option.rect.top() + kPad, m_thumb, m_thumb);
    const QImage thumb = index.data(FilmstripModel::ThumbnailRole).value<QImage>();
    const QVariant ratingData = index.data(FilmstripModel::RatingRole);
    const Rating rating = ratingData.canConvert<Rating>() ? ratingData.value<Rating>() : Rating();
    QRectF target = inner;
    if (!thumb.isNull()) {
        const QSizeF s = QSizeF(thumb.size()).scaled(inner.size(), Qt::KeepAspectRatio);
        target = QRectF(inner.center() - QPointF(s.width() / 2, s.height() / 2), s);
        if (rating.isReject())
            painter->setOpacity(0.45); // rejects are dimmed (F-CULL-01)
        painter->drawImage(target, thumb);
        painter->setOpacity(1.0);
    } else {
        const QSizeF s = QSizeF(4, 3).scaled(inner.size(), Qt::KeepAspectRatio);
        target = QRectF(inner.center() - QPointF(s.width() / 2, s.height() / 2), s);
        painter->setPen(Qt::NoPen);
        painter->setBrush(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 18));
        painter->drawRoundedRect(target, 4, 4);
        if (index.data(FilmstripModel::FailedRole).toBool()) {
            const double d = std::clamp(std::min(target.width(), target.height()) * 0.3, 14.0, 28.0);
            const QRectF mark(target.center() - QPointF(d / 2, d / 2), QSizeF(d, d));
            painter->setBrush(QColor(214, 64, 54, 220));
            painter->drawEllipse(mark);
            QFont f = painter->font();
            f.setBold(true);
            f.setPixelSize(int(d * 0.7));
            painter->setFont(f);
            painter->setPen(Qt::white);
            painter->drawText(mark, Qt::AlignCenter, QStringLiteral("!"));
        }
    }

    // Badges (F-GRID-02: rating and pick/reject at a glance).
    QFont small = option.font;
    small.setPixelSize(std::clamp(m_thumb / 12, 10, 14));
    small.setBold(true);
    painter->setFont(small);
    const QFontMetricsF fm(small);
    const double bh = fm.height() + 4;
    if (rating.stars > 0) {
        const QString stars = QString(rating.stars, QChar(0x2605));
        const double bw = fm.horizontalAdvance(stars) + 10;
        drawBadge(painter, QRectF(target.left() + 4, target.bottom() - bh - 4, bw, bh), QColor(20, 20, 20, 190),
                  stars);
    }
    if (rating.isPick() || rating.isReject()) {
        const QString mark = rating.isPick() ? QStringLiteral("✓") : QStringLiteral("✕");
        const QColor fill = rating.isPick() ? QColor(46, 160, 67, 230) : QColor(214, 64, 54, 230);
        drawBadge(painter, QRectF(target.right() - bh - 4, target.top() + 4, bh, bh), fill, mark);
    }

    // File name.
    QFont nameFont = option.font;
    painter->setFont(nameFont);
    painter->setPen(pal.color(QPalette::Text));
    const QRectF textRect(option.rect.left() + 4, inner.bottom() + 2, option.rect.width() - 8, kTextLine);
    const QString name = index.data(Qt::DisplayRole).toString();
    painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(name, Qt::ElideMiddle, int(textRect.width())));
    painter->restore();
}

// ---------------------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------------------

GridView::GridView(Collection& collection, ThumbnailCache& cache, QWidget* parent)
    : QListView(parent), m_collection(collection), m_cache(cache) {
    setObjectName(QStringLiteral("lfGridView"));
    setAccessibleName(tr("Raster"));
    m_model = new FilmstripModel(collection, cache, this);
    m_delegate = new GridDelegate(this);
    m_delegate->setSelection(&m_selection);
    setItemDelegate(m_delegate);
    setModel(m_model);
    setViewMode(QListView::ListMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true); // virtualized: no per-item size queries
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::NoSelection); // GridSelection
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setAttribute(Qt::WA_AcceptTouchEvents, false);

    m_retainTimer = new QTimer(this);
    m_retainTimer->setSingleShot(true);
    m_retainTimer->setInterval(kRetainDelayMs);
    connect(m_retainTimer, &QTimer::timeout, this, &GridView::retainVisible);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &GridView::scheduleRetain);
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        syncPaths();
        scheduleRetain();
    });
    setThumbSize(Settings::instance().value(key::GridThumbSize).toInt());
    syncPaths();
}

GridView::~GridView() = default;

void GridView::syncPaths() {
    QStringList paths;
    paths.reserve(m_collection.count());
    for (const FileEntry& e : m_collection.entries())
        paths << e.path;
    if (m_selection.count() == 0 && m_selection.cursor() < 0)
        m_selection.reset(paths, m_collection.currentPath());
    else
        m_selection.setPaths(paths);
    viewport()->update();
    emit gridSelectionChanged();
}

void GridView::activate() {
    syncPaths();
    QStringList paths = m_selection.paths();
    m_selection.reset(paths, m_collection.currentPath());
    cursorChanged(true);
    setFocus(Qt::OtherFocusReason);
}

void GridView::setThumbSize(int logicalPx) {
    const int s = grid::stepSize(logicalPx, 0);
    const bool changed = s != m_thumb;
    m_thumb = s;
    m_delegate->setThumbSize(s);
    setGridSize(m_delegate->cellSize());
    updateThumbRequestSize();
    doItemsLayout();
    if (changed) {
        Settings::instance().setValue(key::GridThumbSize, s);
        if (m_selection.cursor() >= 0)
            scrollTo(m_model->index(m_selection.cursor()), QAbstractItemView::EnsureVisible);
        emit thumbSizeChanged(s);
    }
    scheduleRetain();
}

void GridView::zoomStep(int steps) {
    setThumbSize(grid::stepSize(m_thumb, steps));
}

int GridView::columns() const {
    return grid::columnsFor(viewport()->width(), m_delegate->cellSize().width());
}

int GridView::rowsPerPage() const {
    return std::max(1, viewport()->height() / std::max(1, m_delegate->cellSize().height()));
}

void GridView::updateThumbRequestSize() {
    const qreal dpr = window() && window()->windowHandle() ? window()->windowHandle()->devicePixelRatio()
                                                           : devicePixelRatioF();
    const int want = int(std::ceil(m_thumb * dpr));
    // A few fixed sizes keep the thumbnail disk cache shared (also with the filmstrip).
    int px = 768;
    for (int b : {128, 192, 256, 384, 512, 768})
        if (want <= b) {
            px = b;
            break;
        }
    m_model->setThumbSize(px);
}

QList<int> GridView::visibleRows() const {
    QList<int> rows;
    const int n = m_model->rowCount();
    if (n == 0 || !isVisible())
        return rows;
    const QRect vp = viewport()->rect();
    const QModelIndex first = indexAt(vp.topLeft() + QPoint(kPad, kPad));
    const QModelIndex last = indexAt(vp.bottomRight() - QPoint(kPad, kPad));
    const int from = first.isValid() ? first.row() : 0;
    const int to = last.isValid() ? last.row() : n - 1;
    for (int r = std::max(0, from - columns()); r <= to + columns() && r < n; ++r)
        if (visualRect(m_model->index(r)).intersects(vp))
            rows << r;
    return rows;
}

void GridView::scheduleRetain() {
    m_retainTimer->start();
}

void GridView::retainVisible() {
    QStringList paths;
    for (int r : visibleRows())
        paths << m_model->index(r).data(FilmstripModel::PathRole).toString();
    m_cache.retain(m_model->client(), paths);
}

void GridView::cursorChanged(bool scroll) {
    if (scroll && m_selection.cursor() >= 0)
        scrollTo(m_model->index(m_selection.cursor()), QAbstractItemView::EnsureVisible);
    viewport()->update();
    emit gridSelectionChanged();
}

void GridView::clickRow(int row, Qt::KeyboardModifiers mods) {
    m_selection.click(row, mods & Qt::ControlModifier, mods & Qt::ShiftModifier);
    cursorChanged(false);
}

void GridView::moveGridCursor(grid::Move move, bool extend) {
    const int to = grid::moveIndex(m_selection.cursor(), move, columns(), m_selection.count(), rowsPerPage());
    if (to < 0)
        return;
    m_selection.moveTo(to, extend);
    cursorChanged(true);
}

void GridView::selectAllItems() {
    m_selection.selectAll();
    cursorChanged(false);
}

bool GridView::handlesKey(const QKeyEvent* e) const {
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
        return mods == Qt::NoModifier || mods == Qt::ShiftModifier ||
               (mods == Qt::ControlModifier && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down));
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Escape:
        return mods == Qt::NoModifier;
    case Qt::Key_A:
        return mods == Qt::ControlModifier;
    default:
        return false;
    }
}

bool GridView::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        // Arrows, Enter, Esc … are menu shortcuts in the viewer; in the grid they move the
        // cursor (Enter opens, Esc closes).
        auto* ke = static_cast<QKeyEvent*>(event);
        if (handlesKey(ke) || (m_extraKeys && m_extraKeys(ke))) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(event);
        if (g->gestureType() == Qt::BeginNativeGesture) {
            m_pinch = 1.0;
            m_pinchBase = m_thumb;
        } else if (g->gestureType() == Qt::ZoomNativeGesture) {
            m_pinch *= 1.0 + g->value();
            setThumbSize(grid::scaledSize(m_pinchBase, m_pinch));
        }
        event->accept();
        return true;
    }
    return QListView::event(event);
}

void GridView::keyPressEvent(QKeyEvent* event) {
    if (!handlesKey(event)) {
        event->ignore(); // the window handles it (rating, trash, G, quick move …)
        return;
    }
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool command = event->modifiers() & Qt::ControlModifier;
    switch (event->key()) {
    case Qt::Key_Left:
        moveGridCursor(grid::Move::Left, shift);
        break;
    case Qt::Key_Right:
        moveGridCursor(grid::Move::Right, shift);
        break;
    case Qt::Key_Up:
        moveGridCursor(command ? grid::Move::Home : grid::Move::Up, shift);
        break;
    case Qt::Key_Down:
        if (command) { // ⌘↓ opens, like in the Finder
            if (!cursorPath().isEmpty())
                emit openRequested(cursorPath());
            break;
        }
        moveGridCursor(grid::Move::Down, shift);
        break;
    case Qt::Key_Home:
        moveGridCursor(grid::Move::Home, shift);
        break;
    case Qt::Key_End:
        moveGridCursor(grid::Move::End, shift);
        break;
    case Qt::Key_PageUp:
        moveGridCursor(grid::Move::PageUp, shift);
        break;
    case Qt::Key_PageDown:
        moveGridCursor(grid::Move::PageDown, shift);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!cursorPath().isEmpty())
            emit openRequested(cursorPath());
        break;
    case Qt::Key_Escape:
        emit closeRequested();
        break;
    case Qt::Key_A:
        selectAllItems();
        break;
    default:
        break;
    }
    event->accept();
}

void GridView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const QModelIndex idx = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && idx.isValid())
        clickRow(idx.row(), event->modifiers());
    event->accept();
}

void GridView::mouseDoubleClickEvent(QMouseEvent* event) {
    const QModelIndex idx = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && idx.isValid())
        emit openRequested(idx.data(FilmstripModel::PathRole).toString());
    event->accept();
}

void GridView::mouseMoveEvent(QMouseEvent* event) {
    event->accept(); // no rubber band / drag selection (hover still updates via the viewport)
}

void GridView::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) { // ⌘ + wheel: thumbnail size
        const int dy = event->angleDelta().y();
        if (dy != 0)
            zoomStep(dy > 0 ? 1 : -1);
        event->accept();
        return;
    }
    QListView::wheelEvent(event);
}

void GridView::resizeEvent(QResizeEvent* event) {
    QListView::resizeEvent(event);
    scheduleRetain();
}

void GridView::showEvent(QShowEvent* event) {
    QListView::showEvent(event);
    updateThumbRequestSize();
    scheduleRetain();
}

void GridView::hideEvent(QHideEvent* event) {
    QListView::hideEvent(event);
    m_retainTimer->stop();
    m_cache.retain(m_model->client(), {});
}

} // namespace lf
