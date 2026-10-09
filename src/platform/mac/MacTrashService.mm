// SPDX-License-Identifier: GPL-3.0-or-later
// Trash via NSFileManager (F-FILE-01).

#include "core/Platform.h"

#include <QCoreApplication>
#include <QFileInfo>

#import <Foundation/Foundation.h>

namespace lf {

namespace {

NSURL* fileURL(const QString& path) {
    return [NSURL fileURLWithPath:path.toNSString()];
}

QString describe(NSError* err) {
    return err ? QString::fromNSString(err.localizedDescription) : QStringLiteral("?");
}

class MacTrashService final : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        @autoreleasepool {
            NSURL* resulting = nil;
            NSError* err = nil;
            if (![[NSFileManager defaultManager] trashItemAtURL:fileURL(path)
                                               resultingItemURL:&resulting
                                                          error:&err]) {
                if (error)
                    *error = QCoreApplication::translate(
                                 "lf::TrashService", "„%1“ konnte nicht in den Papierkorb gelegt werden: %2")
                                 .arg(QFileInfo(path).fileName(), describe(err));
                return std::nullopt;
            }
            return resulting ? QString::fromNSString(resulting.path) : QString();
        }
    }

    bool restoreFromTrash(const QString& trashedPath, const QString& originalPath, QString* error) override {
        @autoreleasepool {
            const QFileInfo orig(originalPath);
            if (orig.exists() || orig.isSymLink()) {
                if (error)
                    *error = QCoreApplication::translate(
                                 "lf::TrashService", "„%1“ existiert bereits und wird nicht überschrieben.")
                                 .arg(orig.fileName());
                return false;
            }
            if (trashedPath.isEmpty() || !QFileInfo(trashedPath).exists()) {
                if (error)
                    *error =
                        QCoreApplication::translate("lf::TrashService", "„%1“ ist nicht mehr im Papierkorb.")
                            .arg(orig.fileName());
                return false;
            }
            NSError* err = nil;
            // moveItemAtURL fails if the destination exists - no overwrite.
            if (![[NSFileManager defaultManager] moveItemAtURL:fileURL(trashedPath)
                                                         toURL:fileURL(originalPath)
                                                         error:&err]) {
                if (error)
                    *error = QCoreApplication::translate("lf::TrashService",
                                                         "„%1“ konnte nicht wiederhergestellt werden: %2")
                                 .arg(orig.fileName(), describe(err));
                return false;
            }
            return true;
        }
    }
};

} // namespace

std::unique_ptr<TrashService> createTrashService() {
    return std::make_unique<MacTrashService>();
}

} // namespace lf
