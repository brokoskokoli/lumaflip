// SPDX-License-Identifier: GPL-3.0-or-later
#include "ZoomMemoryController.h"

#include "ImageView.h"
#include "ViewSyncBinding.h"
#include "core/Settings.h"

namespace lf {

ZoomMemoryController::ZoomMemoryController(ImageView* view, QObject* parent) : QObject(parent), m_view(view) {
    Settings& st = Settings::instance();
    m_enabled = st.value(key::RememberZoom).toBool();
    connect(m_view, &ImageView::viewTransformChanged, this, &ZoomMemoryController::onViewChanged);
    connect(&st, &Settings::changed, this, [this](const QString& k) {
        if (k != QLatin1String(key::RememberZoom))
            return;
        m_enabled = Settings::instance().value(key::RememberZoom).toBool();
        if (!m_enabled)
            m_memory.clear();
    });
}

bool ZoomMemoryController::differsFromStandard() const {
    // A standard mode that shows the whole image has nothing to remember; one that shows a
    // part of it (100 % of a large image) may have been panned.
    return m_view->image() && (m_view->zoomMode() != m_view->standardZoomMode() || m_view->isZoomed());
}

bool ZoomMemoryController::zoomChanged() const {
    return m_touched && !m_path.isEmpty() && differsFromStandard();
}

void ZoomMemoryController::onViewChanged() {
    if (m_switching || m_path.isEmpty() || !m_view->image())
        return;
    m_touched = true;
    if (!m_enabled || m_view->zoomLocked())
        return; // F-VIEW-05 wins: the locked view belongs to every image
    if (differsFromStandard())
        m_memory.remember(m_path, m_page, captureViewState(m_view));
    else
        m_memory.forget(m_path, m_page);
}

void ZoomMemoryController::beginImageChange() {
    m_switching = true;
}

void ZoomMemoryController::imageShown(const QString& path, int page) {
    m_switching = false;
    m_path = path;
    m_page = page;
    m_touched = false;
    if (!m_enabled || m_view->zoomLocked())
        return;
    if (const std::optional<ViewSyncState> s = m_memory.recall(path, page)) {
        m_view->restoreView(s->customZoom ? ZoomMode::Custom : ZoomMode(s->mode), s->factor, s->center);
        m_touched = true;
    }
}

void ZoomMemoryController::standardModeChosen() {
    if (!m_path.isEmpty())
        m_memory.forget(m_path, m_page);
    m_touched = false;
}

void ZoomMemoryController::resetToStandard() {
    m_view->resetToStandardZoom();
    standardModeChosen();
}

void ZoomMemoryController::fileRemoved(const QString& path) {
    m_memory.forget(path);
}

void ZoomMemoryController::fileRenamed(const QString& from, const QString& to) {
    m_memory.rename(from, to);
    if (m_path == from)
        m_path = to;
}

void ZoomMemoryController::fileRewritten(const QString& path) {
    m_memory.forget(path); // another size or orientation: the old view does not fit
}

} // namespace lf
