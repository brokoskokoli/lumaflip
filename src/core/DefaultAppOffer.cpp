// SPDX-License-Identifier: GPL-3.0-or-later
#include "DefaultAppOffer.h"

#include "Settings.h"
#include "VirtualPath.h"

#include <QDir>
#include <QFileInfo>

namespace lf::defaultapp {

Decision beforeQuery(const Situation& s) {
    if (s.environmentOff)
        return Decision::Environment;
    if (!s.enabled)
        return Decision::Disabled;
    if (s.containerMember)
        return Decision::ContainerMember;
    if (s.fullScreen)
        return Decision::FullScreen;
    return Decision::Show;
}

Decision afterQuery(const Situation& s, bool installed, const DefaultViewerStatus& status,
                    const QStringList& askedTypes) {
    // The situation may have changed while the system was asked (full screen entered,
    // "Nicht mehr fragen" in another window).
    if (const Decision d = beforeQuery(s); d != Decision::Show)
        return d;
    if (!installed)
        return Decision::NotInstalled;
    if (status.state == DefaultViewerStatus::State::Unknown || status.type.isEmpty())
        return Decision::Unknown;
    if (status.state == DefaultViewerStatus::State::IsDefault)
        return Decision::IsDefault;
    if (askedTypes.contains(status.type, Qt::CaseInsensitive))
        return Decision::AlreadyAsked;
    return Decision::Show;
}

bool isEnabled() {
    return Settings::instance().value(QString::fromLatin1(key::OfferDefaultApp)).toBool();
}

void setEnabled(bool on) {
    Settings::instance().setValue(QString::fromLatin1(key::OfferDefaultApp), on);
}

QStringList askedTypes() {
    return Settings::instance().value(QString::fromLatin1(kAskedTypesKey)).toStringList();
}

void markAsked(const QString& type) {
    if (type.isEmpty())
        return;
    QStringList asked = askedTypes();
    if (asked.contains(type, Qt::CaseInsensitive))
        return;
    asked << type;
    Settings::instance().setValue(QString::fromLatin1(kAskedTypesKey), asked);
}

bool disabledByEnvironment() {
    return qEnvironmentVariableIntValue(kDisableEnv) == 1;
}

Situation situationFor(const QString& path, bool fullScreen) {
    Situation s;
    s.enabled = isEnabled();
    s.environmentOff = disabledByEnvironment();
    s.containerMember = vpath::isMember(path) || vpath::isContainerPath(path);
    s.fullScreen = fullScreen;
    return s;
}

bool isInsideAny(const QString& path, const QStringList& folders) {
    if (path.isEmpty())
        return false;
    const QString p = QDir::cleanPath(path);
    for (const QString& f : folders) {
        if (f.isEmpty())
            continue;
        QString root = QDir::cleanPath(f);
        if (!root.endsWith(QLatin1Char('/')))
            root += QLatin1Char('/');
        if (p.startsWith(root) && p.size() > root.size())
            return true;
    }
    return false;
}

bool isInBuildTree(const QString& dir, int levels) {
    QDir d(dir);
    for (int i = 0; i <= levels; ++i) {
        if (QFileInfo::exists(d.filePath(QStringLiteral("CMakeCache.txt"))))
            return true;
        if (!d.cdUp())
            break;
    }
    return false;
}

} // namespace lf::defaultapp
