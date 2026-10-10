// SPDX-License-Identifier: GPL-3.0-or-later
// System settings the app follows (F-WIN-06).

#include "core/Platform.h"

#import <Foundation/Foundation.h>

namespace lf {

bool systemRestoresWindows() {
    // System Settings → Desktop & Dock → "Fenster beim Beenden einer App schließen" writes
    // NSQuitAlwaysKeepsWindows (inverted) to the global domain; absent = windows close.
    Boolean valid = false;
    const Boolean keep = CFPreferencesGetAppBooleanValue(CFSTR("NSQuitAlwaysKeepsWindows"),
                                                         kCFPreferencesAnyApplication, &valid);
    return valid && keep;
}

} // namespace lf
