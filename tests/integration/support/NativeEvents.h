// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test helper: real native input events. On macOS the events are built as NSEvents and
// dispatched through [NSApp sendEvent:], i.e. the same path a key press takes in the
// running app (window key equivalents, main menu key equivalents, Qt's key handling).
// QTest::keyClick() bypasses all of that and cannot catch menu key-equivalent bugs.
// Other platforms: every function returns false (tests skip).

#include <QKeySequence>
#include <QString>

class QAction;
class QWidget;

namespace lf::test {

// True if real native events can be sent (macOS with the cocoa platform plugin).
bool nativeEventsAvailable();

// Sends key down + key up for `key` with `mods` (Qt modifiers: Ctrl = ⌘, Meta = ⌃) to the
// native window of `window` through [NSApp sendEvent:]. Letters, digits, punctuation of
// the US layout, arrows, Backspace/Delete, Return, Escape, Home/End, Page Up/Down and Space
// are supported; false for other keys or without a native window.
bool sendNativeKey(QWidget* window, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier);
// The first key combination of `seq`.
bool sendNativeShortcut(QWidget* window, const QKeySequence& seq);

// The key equivalent of the NSMenuItem behind `action` in the main menu, e.g. "⇧⌘r"
// ("" if none, "<no native item>" if the action has no native menu item).
QString nativeKeyEquivalent(QAction* action);

} // namespace lf::test
