// SPDX-License-Identifier: GPL-3.0-or-later
// Fills "Date Added", creation date and the localized kind of FileEntry objects.
// One getattrlist(2) call per file (no Foundation round trips); kinds are cached per
// file name extension, so filling 10 000 entries stays in the low milliseconds.

#include "core/Platform.h"

#include <QFile>
#include <QHash>
#include <QMutex>
#include <QTimeZone>

#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cstring>
#include <sys/attr.h>
#include <unistd.h>

namespace lf {
namespace {

QDateTime fromTimespec(const struct timespec& ts) {
    return QDateTime::fromMSecsSinceEpoch(qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000, QTimeZone::UTC);
}

class MacFileAttributeProvider final : public FileAttributeProvider {
  public:
    void fill(FileEntry& entry) override {
        fillTimes(entry);
        if (entry.kind.isEmpty())
            entry.kind = kindFor(entry.name);
    }

  private:
    QMutex mutex_;
    QHash<QString, QString> kinds_; // lower-case suffix -> localized description

    static void fillTimes(FileEntry& entry) {
        struct attrlist req = {};
        req.bitmapcount = ATTR_BIT_MAP_COUNT;
        req.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_CRTIME | ATTR_CMN_ADDEDTIME;
        alignas(8) char buf[128] = {};
        const QByteArray path = QFile::encodeName(entry.path);
        if (getattrlist(path.constData(), &req, buf, sizeof buf, 0) != 0)
            return;
        // Layout: u_int32_t length, attribute_set_t returned, then the attributes in bit
        // order; only those flagged in `returned` are present.
        u_int32_t length = 0;
        attribute_set_t returned = {};
        std::memcpy(&length, buf, sizeof length);
        std::memcpy(&returned, buf + sizeof length, sizeof returned);
        const char* p = buf + sizeof length + sizeof returned;
        const char* end = buf + std::min<size_t>(length, sizeof buf);
        struct timespec ts;
        if (returned.commonattr & ATTR_CMN_CRTIME) {
            if (p + sizeof ts > end)
                return;
            std::memcpy(&ts, p, sizeof ts);
            entry.created = fromTimespec(ts);
            p += sizeof ts;
        }
        if (returned.commonattr & ATTR_CMN_ADDEDTIME) {
            if (p + sizeof ts > end)
                return;
            std::memcpy(&ts, p, sizeof ts);
            entry.added = fromTimespec(ts);
        }
    }

    QString kindFor(const QString& name) {
        const qsizetype dot = name.lastIndexOf(u'.');
        const QString suffix = dot >= 0 ? name.mid(dot + 1).toLower() : QString();
        {
            QMutexLocker lock(&mutex_);
            const auto it = kinds_.constFind(suffix);
            if (it != kinds_.cend())
                return *it;
        }
        QString kind;
        @autoreleasepool {
            UTType* type = suffix.isEmpty() ? nil : [UTType typeWithFilenameExtension:suffix.toNSString()];
            if (type && type.localizedDescription)
                kind = QString::fromNSString(type.localizedDescription);
        }
        QMutexLocker lock(&mutex_);
        kinds_.insert(suffix, kind);
        return kind;
    }
};

} // namespace

std::unique_ptr<FileAttributeProvider> createFileAttributeProvider() {
    return std::make_unique<MacFileAttributeProvider>();
}

} // namespace lf
