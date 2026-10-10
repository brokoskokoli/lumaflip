// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: no Dock. A second start of Lumaflip opens its own window (no "reopen" event).
#include "DockReopen.h"

namespace lf {

void installDockReopenHandler(std::function<void()>) {}

bool simulateDockReopenForTesting() {
    return false;
}

} // namespace lf
