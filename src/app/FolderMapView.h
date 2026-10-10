// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-INFO-08: the map view of the folder map (macOS: MapKit's MKMapView, embedded with
// QWidget::createWindowContainer around the NSView). A thin layer: everything it decides
// is in core/FolderMap.h (FolderMapModel), it only draws and reports clicks/hovers.
//
// - Markers (MKMarkerAnnotationView) cluster when they come close (clusteringIdentifier,
//   MKClusterAnnotation shows the count); a click on a cluster zooms in on its members.
// - The current image is a red marker that is never clustered and drawn on top.
// - Hovering a marker shows its thumbnail in a small preview above it.
// - The MKMapView exists only between start() and stop(): before the user opens the map
//   (SPEC 10.3) nothing is created, so nothing is loaded from the network.

#include "core/FolderMap.h"

#include <memory>

class QWidget;

namespace lf {

// `host` must have a layout; the map widget is added to it on start() and removed on stop().
std::unique_ptr<MapViewBackend> createMapViewBackend(QWidget* host);

} // namespace lf
