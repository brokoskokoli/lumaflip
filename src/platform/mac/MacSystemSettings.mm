// SPDX-License-Identifier: GPL-3.0-or-later
// System settings the app follows (F-WIN-06, F-I18N-03).

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

QStringList appLanguagePreference() {
    // Only the app's own domain (not the global one, where the system-wide list lives):
    // macOS writes the per-app choice there. The system-wide order comes from
    // QLocale::uiLanguages().
    @autoreleasepool {
        QStringList out;
        id value = CFBridgingRelease(
            CFPreferencesCopyValue(CFSTR("AppleLanguages"), kCFPreferencesCurrentApplication,
                                   kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
        if ([value isKindOfClass:[NSArray class]])
            for (id lang in static_cast<NSArray*>(value))
                if ([lang isKindOfClass:[NSString class]])
                    out << QString::fromNSString(static_cast<NSString*>(lang));
        return out;
    }
}

} // namespace lf
