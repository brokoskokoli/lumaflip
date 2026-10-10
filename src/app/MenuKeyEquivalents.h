// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-01: menu key equivalents with Shift + letter (⇧⌘R, ⇧⌘Z, ⇧⌘F, ⇧⌘O …).
//
// Qt's Cocoa menu writes such a shortcut as keyEquivalent "r" (lower case) plus
// NSEventModifierFlagShift in keyEquivalentModifierMask. AppKit (verified on macOS 27)
// encodes Shift for letters only by the case of the key equivalent and ignores the Shift
// bit when matching: the item then answers ⌘R, and ⇧⌘R (characters "R") matches nothing.
// With "Im Finder zeigen" (⇧⌘R) in the Ablage menu before "Rechts drehen" (⌘R) in the
// Bild menu, a real ⌘R revealed the file in the Finder; ⌘F started the slideshow (⇧⌘F).
//
// The fix rewrites such items to the upper-case letter (AppKit's convention, the Shift bit
// stays). Qt re-syncs native items whenever an action changes (enabled state, text), so it
// runs again right before AppKit matches a key: from a local key-down monitor.

namespace lf {

// Rewrites the shifted-letter key equivalents of the main menu; returns how many items
// were changed. No-op (0) without a Cocoa main menu.
int fixShiftedMenuKeyEquivalents();
// Installs the key-down monitor (once per process) that runs the fix before every key
// press is dispatched. No-op on other platforms or without NSApp.
void installMenuKeyEquivalentFix();

} // namespace lf
