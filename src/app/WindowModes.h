// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Native window modes of a viewer window (macOS, NSWindow behind the Qt top-level):
// - F-VIEW-12 "Immer im Vordergrund": the floating window level (per window);
// - F-VIEW-13 minimal window: no title bar chrome - the content reaches under a transparent
//   title bar (full size content view), the title is hidden, the window buttons stay.
// Both are re-applied after the window changes state (Qt may reset the level, e.g. after
// leaving full screen).

#include <functional>

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
    bool fullScreen = false; // what the native window is (macOS: also during the animation)
};
NativeWindowState nativeWindowState(QWidget* window);

// F-VIEW-10: macOS animates full screen, and a change requested while the animation runs is
// dropped by AppKit. `onTransition(entering, finished)` is called when a native transition
// starts (finished = false) and when it ended (true). Call again after the native window may
// have changed (show); the observer follows the current NSWindow and goes with `window`.
// Returns whether transitions are reported (false elsewhere: the state changes at once).
bool watchFullScreenTransitions(QWidget* window,
                                std::function<void(bool entering, bool finished)> onTransition);

} // namespace lf
