// SPDX-License-Identifier: GPL-3.0-or-later
// UTType descriptions and extensions (DisplayText.h).

#include "DisplayText.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace lf {

QString systemTypeDescription(const QString& identifier) {
    @autoreleasepool {
        UTType* type = [UTType typeWithIdentifier:identifier.toNSString()];
        return type && type.localizedDescription ? QString::fromNSString(type.localizedDescription) : QString();
    }
}

QStringList systemTypeExtensions(const QString& identifier) {
    @autoreleasepool {
        QStringList out;
        UTType* type = [UTType typeWithIdentifier:identifier.toNSString()];
        for (NSString* ext in type.tags[UTTagClassFilenameExtension])
            if (!out.contains(QString::fromNSString(ext).toLower()))
                out << QString::fromNSString(ext).toLower();
        return out;
    }
}

} // namespace lf
