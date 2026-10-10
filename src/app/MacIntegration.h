// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Native macOS parts of M4 (MacIntegration_mac.mm):
//  - F-MAC-07 Quick Look panel (QLPreviewPanel) for the current file, ⌘Y like Finder;
//  - F-MAC-07 Services: the current image for other apps' services (file URL, TIFF/PNG,
//    core/ServiceTypes.h) and the service "In Lumaflip öffnen" (Info.plist NSServices);
//  - F-FILE-09 the desktop picture service (NSWorkspace).
// A small NSResponder per viewer window (inserted after its NSWindow in the responder chain)
// answers the Services and Quick Look questions of AppKit. On non-cocoa platforms (offscreen
// tests) everything is a no-op.

#include "core/ServiceTypes.h"

#include <QImage>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QWidget;

namespace lf {

class WallpaperService;

namespace macint {

// ---- Quick Look ----
// Opens the panel for `path` (a file) or closes it if it is open. `step(±1)` runs when the
// user presses ←/→ (↑/↓) in the panel; the window flips and calls quickLookFollow().
// Returns false if there is no native window.
bool quickLookToggle(QWidget* window, const QString& path, std::function<void(int)> step);
// The panel shows `path` from now on (if it is open for this window).
void quickLookFollow(QWidget* window, const QString& path);
bool quickLookVisible();
void quickLookClose();

// ---- Services ----
using ServiceContext = std::function<services::Context()>;
using ServiceImage = std::function<QImage()>; // pixels of the shown image (TIFF/PNG)
// Lets other apps' services use the window's current image (idempotent).
void installServiceResponder(QWidget* window, ServiceContext context, ServiceImage image);
// App level, at launch: the provider of "In Lumaflip öffnen". Cheap (AppKit only keeps the
// object) and set before the event loop runs, so the request of a service that launched the
// app (cold start) finds it.
void setServicesProvider(std::function<void(const QStringList& files)> openFiles);
// App level, once, deferred (AppKit builds its Services data - P-01 startup time): the send
// types, so other apps' services work on the windows' images.
void registerServicesSendTypes();

// ---- Desktop picture ----
std::unique_ptr<WallpaperService> createWallpaperService(QWidget* window);

} // namespace macint
} // namespace lf
