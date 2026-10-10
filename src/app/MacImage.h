// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Objective-C++ helper shared by the macOS parts of the app (icons of apps and services).

#include <QImage>

#ifdef __OBJC__
@class NSImage;

namespace lf::mac {
// Renders `image` into a square QImage of `px` device pixels (any thread).
QImage toQImage(NSImage* image, int px, qreal devicePixelRatio);
} // namespace lf::mac
#endif
