// SPDX-License-Identifier: GPL-3.0-or-later
// Portable fallbacks for name collation, Finder order and file attributes (not built on
// macOS). Name order: runs of digits compare by value, the text between them with a
// case-insensitive collator, ties broken by code point order (forced ordering, F-SORT-01).
// The digit handling is done here: QCollator's numeric mode needs ICU, which Qt builds
// do not always have (the POSIX backend ignores it).

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
        collator_.setCaseSensitivity(Qt::CaseInsensitive);
        collator_.setIgnorePunctuation(false);
    }
    int compare(const QString& a, const QString& b) const override {
        int c = naturalCompare(a, b);
        if (c == 0)
            c = a.compare(b);
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }

private:
    static bool isDigit(QChar ch) { return ch >= u'0' && ch <= u'9'; }

    // Digit runs by value (leading zeros ignored), text runs with the collator.
    int naturalCompare(const QString& a, const QString& b) const {
        qsizetype i = 0, j = 0;
        const qsizetype na = a.size(), nb = b.size();
        QMutexLocker lock(&mutex_); // QCollator is reentrant, not thread-safe
        while (i < na && j < nb) {
            const bool da = isDigit(a.at(i)), db = isDigit(b.at(j));
            qsizetype ei = i, ej = j;
            while (ei < na && isDigit(a.at(ei)) == da)
                ++ei;
            while (ej < nb && isDigit(b.at(ej)) == db)
                ++ej;
            const QStringView ra = QStringView(a).mid(i, ei - i), rb = QStringView(b).mid(j, ej - j);
            int c = 0;
            if (da && db) {
                qsizetype za = 0, zb = 0;
                while (za + 1 < ra.size() && ra.at(za) == u'0')
                    ++za;
                while (zb + 1 < rb.size() && rb.at(zb) == u'0')
                    ++zb;
                const QStringView va = ra.mid(za), vb = rb.mid(zb);
                c = va.size() != vb.size() ? (va.size() < vb.size() ? -1 : 1) : va.compare(vb);
            } else {
                c = collator_.compare(ra, rb);
            }
            if (c != 0)
                return c;
            i = ei;
            j = ej;
        }
        return (i < na) - (j < nb); // the shorter name first
    }

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
