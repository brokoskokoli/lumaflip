// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Native window modes of a viewer window (macOS, NSWindow behind the Qt top-level):
// - F-VIEW-12 "Immer im Vordergrund": the floating window level (per window);
// - F-VIEW-13 minimal window: no title bar chrome - the content reaches under a transparent
//   title bar (full size content view), the title is hidden, the window buttons stay.
// Both are re-applied after the window changes state (Qt may reset the level, e.g. after
// leaving full screen).

class QWidget;

namespace lf {

void setWindowAlwaysOnTop(QWidget* window, bool on);
void setWindowMinimalChrome(QWidget* window, bool on);

// What the native window looks like (tests, F-VIEW-12/13).
struct NativeWindowState {
    bool valid = false;    // a native window exists
    bool floating = false; // level above the normal window level
    bool fullSizeContent = false;
    bool titlebarTransparent = false;
    bool titleHidden = false;
};
NativeWindowState nativeWindowState(QWidget* window);

} // namespace lf
