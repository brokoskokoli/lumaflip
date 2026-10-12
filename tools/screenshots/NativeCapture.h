// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// macOS part of the screenshot tool: native views that a widget grab cannot see (MapKit's
// map view in the folder map) are taken from the window server's image of the window.

#include <QImage>

class QWidget;

namespace lf::shots {

// The window as the window server composes it, without its shadow (`screencapture -l`).
// Null if the capture failed or is not allowed (Screen Recording permission).
QImage captureNativeWindow(QWidget* window);

} // namespace lf::shots
