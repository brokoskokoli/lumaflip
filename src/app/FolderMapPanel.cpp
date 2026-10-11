// SPDX-License-Identifier: GPL-3.0-or-later
#include "FolderMapPanel.h"

#include "FolderMapView.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/FolderMap.h"
#include "core/ThumbnailCache.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace lf {

namespace {

constexpr int kThumbPx = 320; // device pixels of the hover preview

FolderMapPanel::BackendFactory& backendFactory() {
    static FolderMapPanel::BackendFactory factory;
    return factory;
}

} // namespace

void FolderMapPanel::setBackendFactory(BackendFactory factory) {
    backendFactory() = std::move(factory);
}

FolderMapPanel::FolderMapPanel(ViewerWindow& window, QWidget* parent) : QWidget(parent), m_window(window) {
    setObjectName(QStringLiteral("lfFolderMap"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->setSpacing(4);
    auto* header = new QHBoxLayout;
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("lfFolderMapStatus"));
    m_status->setWordWrap(true);
    header->addWidget(m_status, 1);
    m_close = new QToolButton(this);
    m_close->setObjectName(QStringLiteral("lfFolderMapClose"));
    m_close->setText(QStringLiteral("×"));
    m_close->setAutoRaise(true);
    m_close->setFocusPolicy(Qt::NoFocus);
    header->addWidget(m_close);
    layout->addLayout(header);
    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("lfFolderMapEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    m_empty->hide();
    layout->addWidget(m_empty);
    m_mapHost = new QWidget(this);
    m_mapHost->setObjectName(QStringLiteral("lfFolderMapHost"));
    auto* hostLayout = new QVBoxLayout(m_mapHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_mapHost, 1);

    m_model = new FolderMapModel(&GeoTagCache::instance(), this);
    m_backend = backendFactory() ? backendFactory()(m_mapHost) : createMapViewBackend(m_mapHost);
    m_model->setBackend(m_backend.get());

    Collection* c = window.collection();
    connect(c, &Collection::entriesChanged, this, &FolderMapPanel::syncEntries);
    connect(c, &Collection::loadingFinished, this, &FolderMapPanel::syncEntries);
    connect(c, &Collection::currentChanged, this,
            [this](int, const QString& path) { m_model->setCurrent(path); });
    connect(m_model, &FolderMapModel::countsChanged, this, &FolderMapPanel::updateStatus);
    connect(m_model, &FolderMapModel::scanFinished, this, &FolderMapPanel::updateStatus);
    connect(m_model, &FolderMapModel::jumpRequested, this, [this](const QString& path) {
        if (m_window.collection()->setCurrentPath(path))
            m_window.focusContent(); // ←/→ flip again (not the map)
    });
    connect(m_model, &FolderMapModel::thumbnailWanted, this, &FolderMapPanel::requestThumbnail);
    ThumbnailCache& thumbs = ThumbnailCache::instance();
    connect(&thumbs, &ThumbnailCache::thumbnailReady, this, [this](const QString& path, int size) {
        if (path == m_thumbPath && size == kThumbPx && m_backend)
            m_backend->setThumbnail(path, ThumbnailCache::instance().cached(path, kThumbPx));
    });
    connect(m_close, &QToolButton::clicked, this, &FolderMapPanel::closeRequested);
    syncEntries();
    m_model->setCurrent(c->currentPath());
    retranslateUi();
}

FolderMapPanel::~FolderMapPanel() {
    ThumbnailCache::instance().removeClient(this);
    // The model refers to the backend: detach (stops the map) before either goes.
    m_model->setBackend(nullptr);
    delete m_model;
    m_model = nullptr;
    m_backend.reset();
}

void FolderMapPanel::openMap() {
    m_model->open();
    updateStatus();
}

void FolderMapPanel::closeMap() {
    m_model->close();
    ThumbnailCache::instance().retain(this, {});
    m_thumbPath.clear();
    updateStatus();
}

bool FolderMapPanel::isMapOpen() const {
    return m_model->isOpen();
}

void FolderMapPanel::syncEntries() {
    const Collection* c = m_window.collection();
    m_model->setEntries(c->entries());
    m_model->setCurrent(c->currentPath());
}

void FolderMapPanel::updateStatus() {
    m_status->setText(m_model->statusText());
    if (!m_backend) {
        // No map engine on this platform (MapKit is macOS only): the panel still counts the
        // located images, but cannot draw them.
        m_empty->setVisible(true);
        m_empty->setText(tr("Karten sind auf diesem System nicht verfügbar."));
        return;
    }
    const bool none = m_model->isOpen() && !m_model->isScanning() && m_model->locatedCount() == 0;
    m_empty->setVisible(none);
    if (none)
        m_empty->setText(m_model->totalCount() == 0
                             ? tr("Keine Bilder in dieser Ansicht.")
                             : tr("Keines der Bilder enthält einen Aufnahmeort (GPS)."));
}

void FolderMapPanel::requestThumbnail(const QString& path) {
    m_thumbPath = path;
    ThumbnailCache& thumbs = ThumbnailCache::instance();
    thumbs.retain(this, {path});
    const QImage img = thumbs.request(path, kThumbPx, this);
    if (!img.isNull() && m_backend)
        m_backend->setThumbnail(path, img);
}

void FolderMapPanel::retranslateUi() {
    m_close->setToolTip(tr("Ordnerkarte schließen"));
    m_close->setAccessibleName(tr("Ordnerkarte schließen"));
    updateStatus();
}

void FolderMapPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
}

} // namespace lf
