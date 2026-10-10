// SPDX-License-Identifier: GPL-3.0-or-later
// System settings the app follows (F-WIN-06, F-I18N-03).

#include "core/Language.h"
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

namespace {

// Marker next to AppleLanguages: the tag Lumaflip wrote (setAppKitLanguage()).
CFStringRef const kWrittenKey = CFSTR("LumaflipWrittenAppleLanguage");

// The app's own domain, or `domain` (tests). Retained by the caller's CFBridgingRelease.
CFStringRef domainRef(const QString& domain) {
    return domain.isEmpty() ? kCFPreferencesCurrentApplication
                            : static_cast<CFStringRef>(CFBridgingRetain(domain.toNSString()));
}

void releaseDomain(const QString& domain, CFStringRef ref) {
    if (!domain.isEmpty())
        CFRelease(ref);
}

id copyValue(CFStringRef key, CFStringRef domain) {
    return CFBridgingRelease(
        CFPreferencesCopyValue(key, domain, kCFPreferencesCurrentUser, kCFPreferencesAnyHost));
}

QStringList stringList(id value) {
    QStringList out;
    if ([value isKindOfClass:[NSArray class]])
        for (id lang in static_cast<NSArray*>(value))
            if ([lang isKindOfClass:[NSString class]])
                out << QString::fromNSString(static_cast<NSString*>(lang));
    return out;
}

QString string(id value) {
    return [value isKindOfClass:[NSString class]] ? QString::fromNSString(static_cast<NSString*>(value))
                                                  : QString();
}

} // namespace

QStringList appLanguagePreference(const QString& domain) {
    // Only the app's own domain (not the global one, where the system-wide list lives):
    // macOS writes the per-app choice there. The system-wide order comes from
    // QLocale::uiLanguages().
    @autoreleasepool {
        CFStringRef d = domainRef(domain);
        // Pick up changes of other processes (System Settings, `defaults`).
        CFPreferencesSynchronize(d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        QStringList out = stringList(copyValue(CFSTR("AppleLanguages"), d));
        const QString written = string(copyValue(kWrittenKey, d));
        releaseDomain(domain, d);
        if (!written.isEmpty() && out == QStringList{written})
            return {}; // Lumaflip's own explicit choice, not a per-app language of macOS
        return out;
    }
}

bool setAppKitLanguage(const QString& code, const QString& domain) {
    @autoreleasepool {
        CFStringRef d = domainRef(domain);
        const QString written = string(copyValue(kWrittenKey, d));
        const QStringList current = stringList(copyValue(CFSTR("AppleLanguages"), d));
        if (code.isEmpty() || code == QLatin1String(kSystemLanguage)) {
            // Back to the system language: remove only what Lumaflip wrote.
            if (!written.isEmpty() && current == QStringList{written})
                CFPreferencesSetValue(CFSTR("AppleLanguages"), nullptr, d, kCFPreferencesCurrentUser,
                                      kCFPreferencesAnyHost);
            CFPreferencesSetValue(kWrittenKey, nullptr, d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        } else {
            NSString* tag = appleLanguageTag(code).toNSString();
            CFPreferencesSetValue(CFSTR("AppleLanguages"), (__bridge CFArrayRef) @[ tag ], d,
                                  kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
            CFPreferencesSetValue(kWrittenKey, (__bridge CFStringRef)tag, d, kCFPreferencesCurrentUser,
                                  kCFPreferencesAnyHost);
        }
        const bool ok = CFPreferencesSynchronize(d, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        releaseDomain(domain, d);
        return ok;
    }
}

} // namespace lf
