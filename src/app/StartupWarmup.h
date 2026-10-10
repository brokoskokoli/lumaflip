// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// P-01: work that the first window needs anyway is started on background threads while
// the GUI thread creates the application object and the window.

#include <QSize>
#include <QStringList>

namespace lf::startup {

// Before the QApplication: loads the GPU driver (Metal device) in the background.
void warmUpGpu();
// Before the QApplication: the first image file among the command line arguments
// (`argv` without the program name; options starting with '-' are skipped) or empty.
QString firstImageArgument(const QStringList& arguments);
// Device pixel size the first window will decode for (remembered by the last window).
QSize expectedDecodeTarget();
// Right after the QApplication: font database and decoder registry in the background.
void warmUpAfterApplication();

} // namespace lf::startup
