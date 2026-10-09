// SPDX-License-Identifier: GPL-3.0-or-later
// Portable trash (F-FILE-01) via QFile::moveToTrash (freedesktop trash / Windows recycle bin).

#include "core/Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::TrashService", text);
}

class GenericTrashService final : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QString inTrash;
        if (!QFile::moveToTrash(path, &inTrash)) {
            if (error)
                *error =
                    tr("„%1“ konnte nicht in den Papierkorb gelegt werden.").arg(QFileInfo(path).fileName());
            return std::nullopt;
        }
        return inTrash;
    }

    bool restoreFromTrash(const QString& trashedPath, const QString& originalPath, QString* error) override {
        const QFileInfo orig(originalPath);
        if (orig.exists() || orig.isSymLink()) {
            if (error)
                *error = tr("„%1“ existiert bereits und wird nicht überschrieben.").arg(orig.fileName());
            return false;
        }
        if (trashedPath.isEmpty() || !QFile::rename(trashedPath, originalPath)) {
            if (error)
                *error = tr("„%1“ konnte nicht wiederhergestellt werden.").arg(orig.fileName());
            return false;
        }
        // freedesktop: drop the matching .trashinfo file if present.
        const QFileInfo trashed(trashedPath);
        const QString info =
            trashed.dir().absoluteFilePath(QStringLiteral("../info/%1.trashinfo").arg(trashed.fileName()));
        if (QFileInfo::exists(info))
            QFile::remove(info);
        return true;
    }
};

} // namespace

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<GenericTrashService>();
}

} // namespace lf
