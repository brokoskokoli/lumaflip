// SPDX-License-Identifier: GPL-3.0-or-later
// Portable trash (F-FILE-01) via QFile::moveToTrash (freedesktop trash / Windows recycle bin).

#include "XdgDesktop.h"
#include "core/Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace lf {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::TrashService)
};

// rename(), or - across file systems (a home trash for a file on another volume) - a copy
// that keeps the modification time, then removing the trashed file.
bool moveBack(const QString& from, const QString& to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    if (QFile::rename(from, to))
        return true;
    const QFileInfo src(from);
    if (!src.isFile() || src.isSymLink())
        return false;
    if (!QFile::copy(from, to))
        return false;
    QFile f(to);
    if (f.open(QIODevice::ReadWrite))
        f.setFileTime(src.lastModified(), QFileDevice::FileModificationTime);
    f.close();
    if (!QFile::remove(from)) {
        QFile::remove(to);
        return false;
    }
    return true;
}

class GenericTrashService final : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QString inTrash;
        if (!QFile::moveToTrash(path, &inTrash)) {
            if (error)
                *error = Tr::tr("„%1“ konnte nicht in den Papierkorb gelegt werden.")
                             .arg(QFileInfo(path).fileName());
            return std::nullopt;
        }
        return inTrash;
    }

    bool restoreFromTrash(const QString& trashedPath, const QString& originalPath, QString* error) override {
        const QFileInfo orig(originalPath);
        if (orig.exists() || orig.isSymLink()) {
            if (error)
                *error = Tr::tr("„%1“ existiert bereits und wird nicht überschrieben.").arg(orig.fileName());
            return false;
        }
        if (trashedPath.isEmpty() || !QFileInfo::exists(trashedPath) ||
            !moveBack(trashedPath, originalPath)) {
            if (error)
                *error = Tr::tr("„%1“ konnte nicht wiederhergestellt werden.").arg(orig.fileName());
            return false;
        }
        // freedesktop trash: drop the matching .trashinfo file (else file managers list a
        // ghost entry).
        const QString info = xdg::trashInfoPath(trashedPath);
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
