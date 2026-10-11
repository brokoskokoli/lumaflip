// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: Live Text (VisionKit, F-MAC-06) does not exist; isAvailable() is false, so the
// menu item and the setting are hidden.
#include "LiveTextOverlay.h"

#include "ImageView.h"

namespace lf {

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

} // namespace lf
