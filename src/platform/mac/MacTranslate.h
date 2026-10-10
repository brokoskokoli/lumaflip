// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Translated NSString texts for Objective-C++ code that hands text to AppKit directly
// (alerts, menu items, panels built with Cocoa APIs) - F-I18N-01. Never put user-visible
// text into an @"…" literal; tools/i18n_check.py reports such literals. Use
//
//     alert.messageText = lf::mac::translate("lf::Context", "Text");
//
// lupdate extracts calls named translate("context", "source"), so the catalogue entry
// gets the given context. Plural forms: translate("ctx", "%n Bilder", nullptr, n).

#include <QCoreApplication>
#include <QString>

#import <Foundation/Foundation.h>

namespace lf::mac {

inline NSString* translate(const char* context, const char* source, const char* disambiguation = nullptr,
                           int n = -1) {
    return QCoreApplication::translate(context, source, disambiguation, n).toNSString();
}

} // namespace lf::mac
