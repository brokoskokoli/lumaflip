// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Dock icon clicked while the app runs (NSApplicationDelegate
// applicationShouldHandleReopen:hasVisibleWindows:). Qt only reports it as a forced
// "application active" state change, which cannot be told apart from ⌘-Tab or a click
// on another window of the app; this hooks the delegate method itself.

#include <functional>

namespace lf {

// Calls `handler` (on the GUI thread, queued) whenever the Dock icon is clicked; an empty
// function removes it. The delegate method is extended once (Qt's implementation still
// runs); does nothing without a Cocoa application delegate (e.g. offscreen tests).
void installDockReopenHandler(std::function<void()> handler);
// Tests: calls the application delegate as AppKit does for a Dock click. False if there
// is no delegate.
bool simulateDockReopenForTesting();

} // namespace lf
