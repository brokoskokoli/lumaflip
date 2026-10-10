// SPDX-License-Identifier: GPL-3.0-or-later
// Portable fallbacks for name collation, Finder order and file attributes (not built on
// macOS). Name order: ICU collator with numeric mode and case-insensitive comparison,
// ties broken by code point order (forced ordering, F-SORT-01).

#include "core/FinderViewSettings.h"
#include "core/Platform.h"

#include <QCollator>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QMimeDatabase>
#include <QMutex>
#include <QTimeZone>

namespace lf {
namespace {

class GenericNameCollator final : public NameCollator {
public:
    GenericNameCollator() : collator_(QLocale::system()) {
        collator_.setNumericMode(true);
        collator_.setCaseSensitivity(Qt::CaseInsensitive);
        collator_.setIgnorePunctuation(false);
    }
    int compare(const QString& a, const QString& b) const override {
        int c;
        {
            QMutexLocker lock(&mutex_); // QCollator is reentrant, not thread-safe
            c = collator_.compare(a, b);
        }
        if (c == 0)
            c = a.compare(b);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

private:
    mutable QMutex mutex_;
    QCollator collator_;
};

class GenericFolderOrderProvider final : public FolderOrderProvider {
public:
    SortSpec finderOrder(const QString& folderPath, bool) override {
        // .DS_Store files copied from a Mac still carry the Finder settings.
        return finder::resolve(finder::readFolderRecords(folderPath), finder::GlobalContext{});
    }
    QHash<QString, QPointF> iconPositions(const QString& folderPath) override {
        return finder::readIconPositions(folderPath);
    }
};

class GenericFileAttributeProvider final : public FileAttributeProvider {
public:
    void fill(FileEntry& entry) override {
        if (!entry.created.isValid())
            entry.created = QFileInfo(entry.path).birthTime(QTimeZone::UTC);
        if (entry.kind.isEmpty())
            entry.kind = kindFor(entry.name);
    }

private:
    QMutex mutex_;
    QHash<QString, QString> kinds_;
    QMimeDatabase mimeDb_;

    QString kindFor(const QString& name) {
        const QString suffix = QFileInfo(name).suffix().toLower();
        QMutexLocker lock(&mutex_);
        auto it = kinds_.constFind(suffix);
        if (it != kinds_.cend())
            return *it;
        const QString kind = mimeDb_.mimeTypeForFile(name, QMimeDatabase::MatchExtension).comment();
        kinds_.insert(suffix, kind);
        return kind;
    }
};

} // namespace

std::unique_ptr<NameCollator> createNameCollator() {
    return std::make_unique<GenericNameCollator>();
}

std::unique_ptr<FolderOrderProvider> createFolderOrderProvider() {
    return std::make_unique<GenericFolderOrderProvider>();
}

std::unique_ptr<FileAttributeProvider> createFileAttributeProvider() {
    return std::make_unique<GenericFileAttributeProvider>();
}

} // namespace lf
