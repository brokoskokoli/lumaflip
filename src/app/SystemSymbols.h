// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SF Symbols (NSImage imageWithSystemSymbolName) as Qt icons, e.g. for the toolbar of the
// preferences window. Symbols are template images: they are drawn in one color, which the
// caller takes from the current palette (and renews on palette/theme changes, F-MAC-02).

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QString>

namespace lf {

// The symbol `name` drawn in black on transparent, centered in a square of
// `pixelSize` x `pixelSize` device pixels; null if the system does not know the symbol
// (or on other platforms).
QImage systemSymbolMask(const QString& name, int pixelSize);
// The symbol in `color` as an icon (logical size `size` points, rendered for 1x and 2x).
// Null icon if the symbol is unavailable.
QIcon systemSymbolIcon(const QString& name, int size, const QColor& color);

} // namespace lf
