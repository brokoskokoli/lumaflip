// SPDX-License-Identifier: GPL-3.0-or-later
// NativeEvents.h without AppKit: no native event injection (the tests that need it skip).

#include "NativeEvents.h"

namespace lf::test {

bool nativeEventsAvailable() {
    return false;
}

bool sendNativeKey(QWidget*, Qt::Key, Qt::KeyboardModifiers) {
    return false;
}

bool sendNativeShortcut(QWidget*, const QKeySequence&) {
    return false;
}

QString nativeKeyEquivalent(QAction*) {
    return QStringLiteral("<no native item>");
}

} // namespace lf::test
