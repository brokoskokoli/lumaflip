// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-INFO-04: the map renderer of the info panel (macOS: MapKit's MKMapSnapshotter, which
// loads map tiles from Apple's servers). Only MapPreviewController calls it, and only after
// the user clicked "Karte anzeigen" (SPEC 10.3).

#include "core/GeoLocation.h"

#include <memory>

namespace lf {

std::unique_ptr<MapSnapshotProvider> createMapSnapshotProvider();

} // namespace lf
