// SPDX-License-Identifier: GPL-3.0-or-later
#include "CompareView.h"

#include "ImageView.h"
#include "ViewSyncBinding.h"
#include "core/RatingStore.h"

#include <QFileInfo>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>

namespace lf {

CompareView::CompareView(ImageProvider& provider, QWidget* parent) : QFrame(parent), m_provider(provider) {
    setObjectName(QStringLiteral("lfCompareView"));
    setAccessibleName(tr("Vergleich"));
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(4, 4, 4, 4);
    grid->setSpacing(4);
    m_sync = new ViewSyncBinding(this);
    connect(&provider, &ImageProvider::imageReady, this, &CompareView::onImageReady);
    connect(&provider, &ImageProvider::imageFailed, this, &CompareView::onImageFailed);
    connect(&RatingStore::instance(), &RatingStore::ratingChanged, this, [this](const QString& path) {
        for (const Pane& p : std::as_const(m_panes))
            if (p.path == path) {
                updateCaptions();
                return;
            }
    });
}

CompareView::~CompareView() {
    m_provider.removeClient(this);
}

QSize CompareView::decodeTarget() const {
    QSize s = m_panes.isEmpty() || !m_panes.first().view->isVisible() ? size() : m_panes.first().view->size();
    s = s.expandedTo(QSize(400, 300));
    const qreal dpr = window() && window()->windowHandle() ? window()->windowHandle()->devicePixelRatio()
                                                           : devicePixelRatioF();
    return (QSizeF(s) * dpr).toSize();
}

void CompareView::setPaths(const QStringList& paths, int focus) {
    const QStringList wanted = paths.mid(0, kMaxPanes);
    auto* grid = static_cast<QGridLayout*>(layout());
    // Panes are rebuilt (2–4 views; cheap compared to decoding).
    for (Pane& p : m_panes)
        delete p.frame;
    m_panes.clear();
    const int n = int(wanted.size());
    const int columns = n == 4 ? 2 : std::max(1, n);
    QList<ImageView*> views;
    for (int i = 0; i < n; ++i) {
        Pane p;
        p.path = wanted.at(i);
        p.frame = new QFrame(this);
        p.frame->setObjectName(QStringLiteral("lfComparePane%1").arg(i));
        p.frame->setAttribute(Qt::WA_StyledBackground);
        auto* v = new QVBoxLayout(p.frame);
        v->setContentsMargins(2, 2, 2, 2);
        v->setSpacing(2);
        p.view = new ImageView(p.frame);
        p.view->setObjectName(QStringLiteral("lfComparePaneView%1").arg(i));
        p.view->setFocusPolicy(Qt::NoFocus); // keys go to the compare view (Tab, rating)
        p.view->setDragOutEnabled(false);    // ⌥-drag moves one pane (F-GRID-03)
        p.view->setMouseTracking(true);
        p.view->installEventFilter(this);
        for (QWidget* child : p.view->findChildren<QWidget*>())
            child->installEventFilter(this);
        p.caption = new QLabel(p.frame);
        p.caption->setObjectName(QStringLiteral("lfComparePaneCaption%1").arg(i));
        p.caption->setAlignment(Qt::AlignCenter);
        p.caption->setTextFormat(Qt::PlainText);
        v->addWidget(p.view, 1);
        v->addWidget(p.caption);
        grid->addWidget(p.frame, i / columns, i % columns);
        const int pane = i;
        connect(p.view, &ImageView::userInput, this, [this, pane](const InputEvent& ev, QPointF pos) {
            setFocusedPane(pane);
            emit paneInput(pane, ev, pos);
        });
        connect(p.view, &ImageView::needsHigherResolution, this,
                [this, pane] { request(pane, Quality::Full); });
        views << p.view;
        m_panes.append(p);
    }
    m_sync->setViews(views);
    m_provider.setWanted(this, wanted, decodeTarget(), true);
    for (int i = 0; i < n; ++i) {
        // A cached stage is shown at once, the better one follows (imageReady).
        if (auto img = m_provider.cached(m_panes[i].path))
            showStage(i, img->quality, img);
        request(i, Quality::Screen);
    }
    m_focus = std::clamp(focus, 0, std::max(0, n - 1));
    updateFrames();
    updateCaptions();
    emit focusChanged(m_focus);
}

QStringList CompareView::paths() const {
    QStringList out;
    for (const Pane& p : m_panes)
        out << p.path;
    return out;
}

ImageView* CompareView::paneView(int pane) const {
    return pane >= 0 && pane < m_panes.size() ? m_panes.at(pane).view : nullptr;
}

QString CompareView::paneCaption(int pane) const {
    return pane >= 0 && pane < m_panes.size() ? m_panes.at(pane).caption->text() : QString();
}

QString CompareView::focusedPath() const {
    return m_focus >= 0 && m_focus < m_panes.size() ? m_panes.at(m_focus).path : QString();
}

void CompareView::setFocusedPane(int pane) {
    if (pane < 0 || pane >= m_panes.size() || pane == m_focus)
        return;
    m_focus = pane;
    updateFrames();
    emit focusChanged(pane);
}

void CompareView::cycleFocus(int direction) {
    const int n = int(m_panes.size());
    if (n == 0)
        return;
    setFocusedPane(((m_focus + direction) % n + n) % n);
}

void CompareView::updateFrames() {
    for (int i = 0; i < m_panes.size(); ++i) {
        const bool focused = i == m_focus;
        const QColor c = palette().color(QPalette::Highlight);
        m_panes[i].frame->setStyleSheet(
            focused ? QStringLiteral("QFrame#lfComparePane%1 { border: 2px solid %2; border-radius: 4px; }")
                          .arg(i)
                          .arg(c.name())
                    : QStringLiteral("QFrame#lfComparePane%1 { border: 2px solid transparent; }").arg(i));
    }
}

void CompareView::updateCaptions() {
    RatingStore& store = RatingStore::instance();
    for (Pane& p : m_panes) {
        const QString name = QFileInfo(p.path).fileName();
        const std::optional<Rating> r = store.cached(p.path);
        if (!r)
            store.request(p.path); // ratingChanged() updates the caption
        //: compare pane caption: file name — rating (stars, Pick/Reject)
        p.caption->setText(r ? tr("%1 — %2").arg(name, rating::describe(*r)) : name);
    }
}

void CompareView::applyViewSettings(const QString& background, bool upscaleNearest, bool devicePixels) {
    for (Pane& p : m_panes) {
        p.view->setBackground(background);
        p.view->setUpscaleNearest(upscaleNearest);
        p.view->setActualSizeInDevicePixels(devicePixels);
    }
}

void CompareView::stop() {
    m_provider.removeClient(this);
    for (Pane& p : m_panes)
        p.view->clear();
}

void CompareView::request(int pane, Quality quality) {
    if (pane < 0 || pane >= m_panes.size())
        return;
    const QString path = m_panes[pane].path;
    const QSize target = quality == Quality::Full ? QSize() : decodeTarget();
    if (auto img = m_provider.request(path, quality, target, this))
        showStage(pane, quality, img);
}

void CompareView::showStage(int pane, Quality quality, const DecodedImagePtr& image) {
    if (!image || image->image.isNull() || image->page != 0)
        return;
    Pane& p = m_panes[pane];
    if (p.hasImage && int(quality) <= int(p.shown) && p.view->image() &&
        image->image.width() <= p.view->image()->image.width())
        return; // not better than what is shown
    p.view->setImage(image, p.hasImage);
    p.hasImage = true;
    p.shown = quality;
    // A new pane image joins the shared view of the others (except the first image).
    if (!m_sync->isApplying() && pane < m_sync->views().size()) {
        const ViewSyncState shared = m_sync->math().stateFor(pane);
        if (shared.customZoom)
            applyViewState(p.view, shared);
    }
}

void CompareView::onImageReady(const QString& path, Quality quality, const DecodedImagePtr& image) {
    for (int i = 0; i < m_panes.size(); ++i)
        if (m_panes[i].path == path)
            showStage(i, quality, image);
}

void CompareView::onImageFailed(const QString& path, const QString& error) {
    for (Pane& p : m_panes)
        if (p.path == path && !p.hasImage)
            p.view->showError(QFileInfo(path).fileName(), error);
}

bool CompareView::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
        const bool own =
            (ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab || ke->key() == Qt::Key_Escape) &&
            (mods == Qt::NoModifier || mods == Qt::ShiftModifier);
        if (own || (m_extraKeys && m_extraKeys(ke))) {
            event->accept();
            return true;
        }
    }
    return QFrame::event(event);
}

bool CompareView::focusNextPrevChild(bool) {
    return false; // Tab cycles the panes (keyPressEvent)
}

void CompareView::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Tab:
        cycleFocus(1);
        event->accept();
        return;
    case Qt::Key_Backtab:
        cycleFocus(-1);
        event->accept();
        return;
    case Qt::Key_Escape:
        emit closeRequested();
        event->accept();
        return;
    default:
        event->ignore(); // the window: rating, zoom commands for the focused pane …
        return;
    }
}

bool CompareView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        auto* w = qobject_cast<QWidget*>(watched);
        for (int i = 0; i < m_panes.size(); ++i)
            if (w && (w == m_panes[i].view || m_panes[i].view->isAncestorOf(w))) {
                setFocusedPane(i);
                setFocus(Qt::MouseFocusReason);
                break;
            }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace lf
