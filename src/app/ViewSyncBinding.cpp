// SPDX-License-Identifier: GPL-3.0-or-later
#include "ViewSyncBinding.h"

#include "ImageView.h"

#include <QGuiApplication>

namespace lf {

ViewSyncState captureViewState(const ImageView* view) {
    ViewSyncState s;
    s.mode = int(view->zoomMode());
    s.customZoom = view->zoomMode() == ZoomMode::Custom;
    s.factor = view->zoomFactor();
    const QPointF c = view->viewCenter();
    s.center = c.x() < 0 ? QPointF(0.5, 0.5) : c;
    return s;
}

void applyViewState(ImageView* view, const ViewSyncState& state) {
    if (!view->image())
        return; // nothing shown yet: it gets its own view when the image arrives
    if (state.customZoom) {
        view->setZoomFactor(state.factor, QPointF(view->width() / 2.0, view->height() / 2.0));
    } else if (view->zoomMode() != ZoomMode(state.mode)) {
        view->setZoomMode(ZoomMode(state.mode));
    }
    view->setViewCenter(state.center);
}

ViewSyncBinding::ViewSyncBinding(QObject* parent) : QObject(parent) {
    m_independent = [] { return bool(QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier); };
}

void ViewSyncBinding::setViews(const QList<ImageView*>& views) {
    for (const QPointer<ImageView>& v : std::as_const(m_views))
        if (v)
            disconnect(v, nullptr, this, nullptr);
    m_views.clear();
    for (ImageView* v : views) {
        m_views << QPointer<ImageView>(v);
        connect(v, &ImageView::viewTransformChanged, this, [this, v] { onChanged(v); });
    }
    m_sync.setMemberCount(int(views.size()));
    m_sync.reset(views.isEmpty() ? ViewSyncState() : captureViewState(views.first()));
}

QList<ImageView*> ViewSyncBinding::views() const {
    QList<ImageView*> out;
    for (const QPointer<ImageView>& v : m_views)
        out << v.data();
    return out;
}

void ViewSyncBinding::onChanged(ImageView* view) {
    if (m_applying)
        return; // a change we made
    const int member = int(views().indexOf(view));
    if (member < 0)
        return;
    const QVector<ViewSync::Target> targets =
        m_sync.changed(member, captureViewState(view), m_independent && m_independent());
    if (targets.isEmpty())
        return;
    m_applying = true;
    for (const ViewSync::Target& t : targets)
        if (ImageView* v = m_views.value(t.member))
            applyViewState(v, t.state);
    m_applying = false;
    emit synchronized(member);
}

} // namespace lf
