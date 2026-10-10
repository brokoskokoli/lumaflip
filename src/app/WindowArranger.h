// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-WIN-04: Fenster → Anordnen. All viewer windows of the front window's screen are laid
// out side by side, stacked, in a grid (2×2, 3×2 …) or cascaded within the screen's
// available area. The geometry is a pure function (tested with any screen rectangle).

#include <QList>
#include <QRect>
#include <QSize>

class QWidget;

namespace lf {

class Application;

enum class Arrangement { SideBySide, Stacked, Grid, Cascade };

inline constexpr int kArrangeCascadeStepPx = 26; // like new windows (F-WIN-08)

// Columns × rows of the grid for `count` windows: 2 → 2×1, 3/4 → 2×2, 5/6 → 3×2, 7–9 → 3×3 …
QSize gridShape(int count);
// Outer frames (incl. title bar) of `count` windows in `area`, in window order. Cascade uses
// `cascadeSize` (clamped to the area; empty = 2/3 of the area).
QList<QRect> arrangedFrames(Arrangement arrangement, int count, const QRect& area, QSize cascadeSize = {},
                            int cascadeStep = kArrangeCascadeStepPx);
// Applies the frames to the windows (normal state first; frame margins = title bar).
void applyFrames(const QList<QWidget*>& windows, const QList<QRect>& frames);
// Menu command: the viewer windows on the front window's screen (front window first,
// minimized and full screen ones are left alone). Returns the number arranged.
int arrangeViewerWindows(Application& app, Arrangement arrangement);

} // namespace lf
