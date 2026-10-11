// SPDX-License-Identifier: GPL-3.0-or-later
// macOS-only services without a counterpart on this platform (docs/LINUX.md): Live Text
// (VisionKit), Quick Look, the Services menu, map views (MapKit) and the share sheet. The
// UI hides the entries (MainMenuM4.cpp) or shows its "not available" state.

#include "app/FolderMapView.h"
#include "app/ImageView.h"
#include "app/LiveTextOverlay.h"
#include "app/MacIntegration.h"
#include "app/MapSnapshot.h"
#include "app/Share.h"
#include "core/Wallpaper.h"

#include <QFileInfo>

namespace lf {

// ---- LiveTextOverlay.h ----
struct LiveTextOverlay::Native {};

LiveTextOverlay::LiveTextOverlay(ImageView* view, QObject* parent)
    : QObject(parent), m_native(std::make_unique<Native>()), m_view(view) {}

LiveTextOverlay::~LiveTextOverlay() = default;

bool LiveTextOverlay::isAvailable() {
    return false;
}

bool LiveTextOverlay::availabilityKnown() {
    return true;
}

bool LiveTextOverlay::isSupported() const {
    return false;
}

void LiveTextOverlay::analyze(quint64, const QImage&) {}

void LiveTextOverlay::cancel() {}

void LiveTextOverlay::setVisible(bool visible) {
    m_visible = visible;
}

void LiveTextOverlay::updateGeometry() {}

// ---- FolderMapView.h / MapSnapshot.h: no map engine (the panels show their empty state) ----
std::unique_ptr<MapViewBackend> createMapViewBackend(QWidget*) {
    return nullptr;
}

std::unique_ptr<MapSnapshotProvider> createMapSnapshotProvider() {
    return nullptr;
}

// ---- Share.h: no share sheet ----
QString shareTypeKey(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

QList<ShareService> shareServices(const QStringList&) {
    return {};
}

bool performShareService(const QString&, const QStringList&) {
    return false;
}

bool showSharePicker(QWidget*, const QRect&, const QStringList&) {
    return false;
}

namespace macint {

// ---- MacIntegration.h ----
bool quickLookToggle(QWidget*, const QString&, std::function<void(int)>) {
    return false;
}

void quickLookFollow(QWidget*, const QString&) {}

bool quickLookVisible() {
    return false;
}

void quickLookClose() {}

void installServiceResponder(QWidget*, ServiceContext, ServiceImage) {}

void setServicesProvider(std::function<void(const QStringList&)>) {}

void registerServicesSendTypes() {}

} // namespace macint

} // namespace lf
