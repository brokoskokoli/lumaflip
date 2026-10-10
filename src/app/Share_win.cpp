// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: no NSSharingService list. The share menu offers no services (Share.cpp shows only
// its portable entries); the system share sheet is not wired up yet (see docs/WINDOWS.md).
#include "Share.h"

#include <QFileInfo>

namespace lf {

QString shareTypeKey(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

QList<ShareService> shareServices(const QStringList&) {
    return {};
}

bool performShareService(const QString&, const QStringList&) {
    return false;
}

bool showSharePicker(QWidget*, const QRect&, const QStringList&) {
    return false;
}

} // namespace lf
