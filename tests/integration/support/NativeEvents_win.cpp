// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: no native menu bar with key equivalents (Qt draws the menus itself); the tests
// that need native events skip (nativeEventsAvailable() == false).
#include "support/NativeEvents.h"

#include <QString>

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
