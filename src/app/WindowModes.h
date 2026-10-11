// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Native window modes of a viewer window (NSWindow / HWND / X11-Wayland window behind the
// Qt top-level):
// - F-VIEW-12 "Immer im Vordergrund": the floating window level (per window);
// - F-VIEW-13/14 borderless window: no title bar chrome - the content reaches the top edge
//   of the window and the title strip (app/TitleStrip) fades in over it when the mouse
//   moves. macOS: transparent title bar over a full size content view, title hidden, the
//   system's window buttons fade with the strip. Windows: the caption is removed from the
//   frame (WM_NCCALCSIZE), the resize borders, Aero Snap and the Snap Layouts stay
//   (WM_NCHITTEST of the strip). Linux: a frameless window.
// Both are re-applied after the window changes state (Qt may reset the level, e.g. after
// leaving full screen).

#include "core/TitleStripLayout.h"

#include <QPoint>

#include <functional>

class QObject;
class QWidget;

namespace lf {

void setWindowAlwaysOnTop(QWidget* window, bool on);
// Before the native window exists (macOS, Linux: only Qt's window flags, the window is then
// created borderless; Windows creates it) or on the native window.
void setWindowMinimalChrome(QWidget* window, bool on);

// F-VIEW-14: the system's window buttons (macOS: close/minimize/zoom) fade in/out with the
// title strip over `fadeMs` (0: at once). Hidden buttons take no clicks. In native full
// screen they are always shown (the title bar appears with the menu bar there).
// Elsewhere a no-op: the strip draws its own buttons.
void setWindowButtonsVisible(QWidget* window, bool visible, int fadeMs);
// True where the strip keeps the system's buttons (macOS) instead of drawing its own.
bool titleStripUsesSystemButtons();
// Height of the strip in logical pixels: the system title bar's height (macOS), else the
// usual caption height.
int titleStripHeight(QWidget* window);
// Width kept free at the left edge of the strip for the system's buttons (macOS), else 0.
int titleStripReservedLeft(QWidget* window);
// A double click on the strip: macOS follows the system setting (AppleActionOnDoubleClick:
// zoom, minimize or nothing), elsewhere maximize/restore.
void titleStripDoubleClick(QWidget* window);
// The side of the window buttons: Windows right, Linux after the desktop's setting (GNOME
// button-layout, KDE kwinrc; read on a worker thread), macOS left. `done` is called in
// `receiver`'s thread (at once when known).
void detectTitleButtonSide(QObject* receiver, const std::function<void(TitleButtonSide)>& done);

// Windows: the areas of the strip for the native frame (WM_NCHITTEST, window coordinates in
// logical px): Caption moves the window with Aero Snap (double click maximizes, right click
// opens the system menu), Maximize shows the Snap Layouts on hover. `hitTest` is also
// called with QPoint(-1, -1) when the mouse left the non-client area. Elsewhere a no-op.
enum class TitleStripHit { Client, Caption, Maximize };
void setTitleStripHitTest(QWidget* window, const std::function<TitleStripHit(QPoint)>& hitTest);

// What the native window looks like (tests, F-VIEW-12/13/14).
struct NativeWindowState {
    bool valid = false;    // a native window exists
    bool floating = false; // level above the normal window level
    bool fullSizeContent = false;
    bool titlebarTransparent = false;
    bool titleHidden = false;
    bool fullScreen = false;     // what the native window is (macOS: also during the animation)
    bool buttonsHidden = false;  // macOS: the system's window buttons are faded out
    bool captionRemoved = false; // Windows: the caption is not part of the frame (WM_NCCALCSIZE)
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
