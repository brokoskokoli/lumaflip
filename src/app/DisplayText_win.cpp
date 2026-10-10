// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: there are no UTType identifiers; DisplayText.cpp falls back to its own type
// names and extensions (DisplayText.h).
#include "DisplayText.h"

namespace lf {

QString systemTypeDescription(const QString&) {
    return {};
}

QStringList systemTypeExtensions(const QString&) {
    return {};
}

} // namespace lf
