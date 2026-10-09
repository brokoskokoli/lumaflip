// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Output color space of a widget's window (F-COL-01/02).
//
// On macOS Qt (6.11, Metal) composites widget windows into a CAMetalLayer whose
// `colorspace` Qt sets to the NSWindow's colorSpace (= the screen's ICC profile,
// e.g. "Color LCD" which parses as Display P3). All widget content, including the
// QRhiWidget texture, is therefore interpreted *in the display's color space* and
// the canvas has to deliver pixels encoded in exactly that space.

#include <QColorSpace>

class QWidget;

namespace lf {

// Color space of the window that shows `widget` (or of the main screen if the
// widget is not in a native window yet). Never invalid (falls back to sRGB).
QColorSpace windowColorSpace(const QWidget* widget);

} // namespace lf
