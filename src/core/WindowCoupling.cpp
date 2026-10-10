// SPDX-License-Identifier: GPL-3.0-or-later
#include "WindowCoupling.h"

#include "NameFilter.h"

#include <QFileInfo>

#include <algorithm>

namespace lf::coupling {

CoupleBy fromId(const QString& id) {
    return id == QLatin1String("index") ? CoupleBy::Index : CoupleBy::Name;
}

QString toId(CoupleBy by) {
    return by == CoupleBy::Index ? QStringLiteral("index") : QStringLiteral("name");
}

QString matchKey(const QString& fileName) {
    const QString name = QFileInfo(fileName).fileName();
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    return namefilter::fold(dot > 0 ? name.left(dot) : name);
}

int followerIndex(CoupleBy by, int leaderIndex, const QString& leaderPath, const QStringList& followerPaths) {
    const int n = int(followerPaths.size());
    if (n == 0 || leaderIndex < 0)
        return -1;
    if (by == CoupleBy::Index)
        return std::min(leaderIndex, n - 1);
    const QString leaderName = namefilter::fold(QFileInfo(leaderPath).fileName());
    const QString key = matchKey(leaderPath);
    int byKey = -1;
    for (int i = 0; i < n; ++i) {
        const QString name = QFileInfo(followerPaths.at(i)).fileName();
        if (namefilter::fold(name) == leaderName)
            return i;
        if (byKey < 0 && matchKey(name) == key)
            byKey = i;
    }
    return byKey;
}

} // namespace lf::coupling
