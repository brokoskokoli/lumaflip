// SPDX-License-Identifier: GPL-3.0-or-later
// F-CULL-05 bulk actions on rejects (see BulkRejects.h).

#include "BulkRejects.h"

#include <QDir>
#include <QFileInfo>

namespace lf::bulkrejects {

QString rejectedFolder(const QString& imagePath) {
    const QString folder = QFileInfo(imagePath).absolutePath();
    const QString name = QString::fromLatin1(kRejectedFolderName);
    for (const QString& part : QDir::fromNativeSeparators(folder).split(QLatin1Char('/'), Qt::SkipEmptyParts))
        if (part.compare(name, Qt::CaseInsensitive) == 0)
            return {};
    return QDir(folder).filePath(name);
}

Plan plan(const QVector<QPair<QString, QString>>& shown, const QHash<QString, Rating>& ratings,
          bool toTrash) {
    Plan p;
    for (const auto& [path, partner] : shown) {
        if (!ratings.value(path).isReject())
            continue;
        QString target;
        if (!toTrash) {
            target = rejectedFolder(path);
            if (target.isEmpty())
                continue; // already in a "_Rejected" folder
        }
        // The partner first: removing it only turns the pair into a single entry.
        if (!partner.isEmpty())
            p.items.append({partner, target, ConflictChoice::Skip});
        p.items.append({path, target, ConflictChoice::Skip});
        ++p.images;
    }
    return p;
}

} // namespace lf::bulkrejects
