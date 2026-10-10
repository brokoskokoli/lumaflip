// SPDX-License-Identifier: GPL-3.0-or-later
#include "WindowPanels.h"

#include "Filmstrip.h"
#include "InfoPanel.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/ThumbnailCache.h"

#include <QDockWidget>

namespace lf {

namespace {

constexpr int kInfoWidth = 290;

std::shared_ptr<TagService> sharedTagService() {
    static std::shared_ptr<TagService> service(createTagService());
    return service;
}

QDockWidget* makeDock(const QString& title, const char* name, QWidget* parent) {
    auto* dock = new QDockWidget(title, parent);
    dock->setObjectName(QString::fromLatin1(name));
    // A fixed part of the window layout (SPEC 5.1), not a floating tool window.
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setTitleBarWidget(new QWidget(dock));
    return dock;
}

} // namespace

WindowPanels::WindowPanels(ViewerWindow& window, ImageProvider& provider)
    : QObject(&window), m_window(window), m_provider(provider) {
    const Settings& st = Settings::instance();
    m_infoOn = st.value(key::ShowInfoPanel).toBool();
    m_filmOn = st.value(key::ShowFilmstrip).toBool();
    connect(&m_window, &ViewerWindow::imageShown, this, [this](const QString& path, Quality quality) {
        if (m_info)
            m_info->imageShown(path, quality);
    });
    Collection* c = m_window.collection();
    connect(c, &Collection::currentChanged, this, [this] { updateInfoFile(); });
    connect(c, &Collection::entriesChanged, this, [this] {
        updateInfoFile();
        if (m_restored)
            apply(); // the filmstrip hides on the empty start screen
    });
}

WindowPanels::~WindowPanels() = default;

void WindowPanels::restore() {
    if (m_restored)
        return;
    m_restored = true;
    apply();
}

void WindowPanels::toggleInfoPanel() {
    setInfoPanelEnabled(!m_infoOn);
}

void WindowPanels::toggleFilmstrip() {
    setFilmstripEnabled(!m_filmOn);
}

void WindowPanels::setInfoPanelEnabled(bool on) {
    m_infoOn = on;
    Settings::instance().setValue(key::ShowInfoPanel, on); // new windows start like this one
    m_restored = true;
    apply();
}

void WindowPanels::setFilmstripEnabled(bool on) {
    m_filmOn = on;
    Settings::instance().setValue(key::ShowFilmstrip, on);
    m_restored = true;
    apply();
}

bool WindowPanels::infoPanelShown() const {
    return m_infoDock && !m_infoDock->isHidden();
}

bool WindowPanels::filmstripShown() const {
    return m_filmDock && !m_filmDock->isHidden();
}

void WindowPanels::setChromeVisible(bool visible) {
    if (m_chrome == visible)
        return;
    m_chrome = visible;
    if (m_restored)
        apply();
}

bool WindowPanels::underMouse() const {
    return (m_infoDock && m_infoDock->isVisible() && m_infoDock->underMouse()) ||
           (m_filmDock && m_filmDock->isVisible() && m_filmDock->underMouse());
}

bool WindowPanels::isRevealEdge(QPoint p, int edgePx) const {
    // The info panel lives at the right edge (bottom/top are handled with the status bar).
    return m_infoOn && p.x() >= m_window.width() - edgePx;
}

void WindowPanels::ensureInfoPanel() {
    if (m_infoDock)
        return;
    m_infoDock = makeDock(tr("Informationen"), "lfInfoDock", &m_window);
    m_infoDock->setAllowedAreas(Qt::RightDockWidgetArea);
    m_info = new InfoPanel(m_provider, sharedTagService(), m_infoDock);
    m_info->setMinimumWidth(220);
    m_infoDock->setWidget(m_info);
    m_infoDock->hide();
    m_window.addDockWidget(Qt::RightDockWidgetArea, m_infoDock);
    m_window.resizeDocks({m_infoDock}, {kInfoWidth}, Qt::Horizontal);
    updateInfoFile();
}

void WindowPanels::ensureFilmstrip() {
    if (m_filmDock)
        return;
    // SPEC 5.1: the filmstrip spans the whole width below image and info panel.
    m_window.setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    m_window.setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    m_filmDock = makeDock(tr("Filmstreifen"), "lfFilmDock", &m_window);
    m_filmDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_film = new Filmstrip(*m_window.collection(), ThumbnailCache::instance(), m_filmDock);
    m_filmDock->setWidget(m_film);
    m_filmDock->hide();
    m_window.addDockWidget(Qt::BottomDockWidgetArea, m_filmDock);
}

void WindowPanels::apply() {
    if (m_infoOn)
        ensureInfoPanel();
    if (m_filmOn)
        ensureFilmstrip();
    const Collection* c = m_window.collection();
    const bool hasContent = c->count() > 0 || c->isLoading();
    if (m_infoDock)
        m_infoDock->setVisible(m_infoOn && m_chrome);
    if (m_filmDock)
        m_filmDock->setVisible(m_filmOn && m_chrome && hasContent);
}

void WindowPanels::updateInfoFile() {
    if (!m_info)
        return;
    const Collection* c = m_window.collection();
    const int i = c->currentIndex();
    m_info->setFile(c->currentPath(), i >= 0 && i < c->count() ? &c->at(i) : nullptr);
}

} // namespace lf
