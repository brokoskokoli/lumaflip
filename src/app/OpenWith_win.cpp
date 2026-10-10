// SPDX-License-Identifier: GPL-3.0-or-later
// "Open with" on Windows (OpenWith.h): the programs Windows recommends for the file type
// (SHAssocEnumHandlers), the default one first. AppEntry::path holds the handler's id
// (usually the program's path), which Shell::openWith() invokes again.
#include "OpenWith.h"

#include "platform/windows/WinShellUtil.h"

#include <QFileInfo>

#include <algorithm>

namespace lf {

namespace {

AppEntry toEntry(const win::AssocHandler& h) {
    AppEntry e;
    e.name = h.name;
    e.path = h.id;
    e.icon = h.icon;
    e.isDefault = h.isDefault;
    // Lumaflip itself is recognised by its executable (isOwnApplication).
    if (QFileInfo(h.id).fileName().compare(QStringLiteral("Lumaflip.exe"), Qt::CaseInsensitive) == 0)
        e.bundleId = QString::fromLatin1(kLumaflipBundleId);
    return e;
}

} // namespace

QList<AppEntry> queryApplicationsForFile(const QString& filePath) {
    QList<AppEntry> out;
    QList<AppEntry> rest;
    for (const win::AssocHandler& h : win::assocHandlers(filePath)) {
        if (h.isDefault)
            out << toEntry(h);
        else
            rest << toEntry(h);
    }
    std::sort(rest.begin(), rest.end(), [](const AppEntry& a, const AppEntry& b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return out + rest;
}

AppEntry applicationInfo(const QString& appPath) {
    if (appPath.isEmpty())
        return {};
    return toEntry(win::programInfo(appPath));
}

QString ownBundlePath() {
    return win::ownExecutablePath();
}

QString ownBundleId() {
    return QString::fromLatin1(kLumaflipBundleId);
}

} // namespace lf
