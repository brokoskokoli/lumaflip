// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-WIN-05: macOS window tabs. Viewer windows share one tabbing identifier; whether they
// may become tabs follows the setting key::AllowTabs (default off: NSWindowTabbingMode
// "disallowed"; on: "preferred", i.e. new windows open as tabs like in Finder).
// F-WIN-04: the system's window tiling (macOS 15: Fenster → Bewegen & Größe ändern,
// green button, Globe-key shortcuts) works for resizable primary windows; configure()
// keeps viewer windows that way.

class QWidget;

namespace lf {

enum class TabCommand { MergeAllWindows, MoveTabToNewWindow, SelectNextTab, SelectPreviousTab, ToggleTabBar };

// Values of NSWindowTabbingMode.
enum class NativeTabbingMode { Unknown = -1, Automatic = 0, Preferred = 1, Disallowed = 2 };

// Sets tabbing mode/identifier and the collection behavior on the native window of the
// top-level `window` (creates the native window if needed). No-op on other platforms.
void configureViewerWindow(QWidget* window, bool allowTabs);
NativeTabbingMode nativeTabbingMode(QWidget* window);
// Runs a tab command on the native window; false if not possible (no native window,
// tabs not allowed for it).
bool performTabCommand(QWidget* window, TabCommand command);
// Windows in the tab group of `window` (1 = no tabs; 0 = no native window).
int tabGroupSize(QWidget* window);
// The native window can take part in system tiling / full screen (resizable, primary).
bool supportsSystemTiling(QWidget* window);

} // namespace lf
