// SPDX-License-Identifier: GPL-3.0-or-later
#include "ArchiveCache.h"

#include "Archive.h"
#include "VirtualPath.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QWaitCondition>

#include <algorithm>

namespace lf {

struct ArchiveCache::Private {
    Reader reader;
    qint64 maxBytes = 0;
    mutable QMutex mutex;
    QWaitCondition done;
    std::unique_ptr<QTemporaryDir> dir;
    struct Item {
        QString file;
        qint64 bytes = 0;
        quint64 used = 0;
    };
    QHash<QString, Item> items; // key -> extracted file
    QSet<QString> busy;         // keys being extracted
    quint64 clock = 0;
    qint64 total = 0;

    // Under the mutex.
    bool ensureDir() {
        if (dir && dir->isValid())
            return true;
        dir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/lumaflip-archive-XXXXXX"));
        if (!dir->isValid())
            return false;
        QFile::setPermissions(dir->path(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return true;
    }

    // Under the mutex: drops the least recently used files until the budget holds.
    void evict(const QString& keep) {
        while (total > maxBytes && items.size() > 1) {
            auto oldest = items.end();
            for (auto it = items.begin(); it != items.end(); ++it)
                if (it.key() != keep && (oldest == items.end() || it->used < oldest->used))
                    oldest = it;
            if (oldest == items.end())
                return;
            QFile::remove(oldest->file);
            total -= oldest->bytes;
            items.erase(oldest);
        }
    }
};

namespace {

QString keyFor(const vpath::Parts& parts) {
    const QFileInfo fi(parts.container);
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(parts.container.toUtf8());
    h.addData(QByteArray(1, '\0'));
    h.addData(QByteArray::number(fi.size()));
    h.addData(QByteArray::number(fi.lastModified(QTimeZone::UTC).toMSecsSinceEpoch()));
    h.addData(QByteArray(1, '\0'));
    h.addData(parts.member.normalized(QString::NormalizationForm_C).toUtf8());
    return QString::fromLatin1(h.result().toHex().left(40));
}

// A safe file name suffix for the extracted file (decoders look at it): letters and
// digits only.
QString safeSuffix(const QString& member) {
    const qsizetype slash = member.lastIndexOf(u'/');
    const qsizetype dot = member.lastIndexOf(u'.');
    if (dot < 0 || dot < slash)
        return {};
    QString s = member.mid(dot + 1).toLower();
    if (s.size() > 8)
        return {};
    for (const QChar c : std::as_const(s))
        if (!(c.isLetterOrNumber() && c.unicode() < 128))
            return {};
    return s;
}

} // namespace

ArchiveCache& ArchiveCache::instance() {
    static ArchiveCache cache;
    return cache;
}

ArchiveCache::ArchiveCache(Reader reader, qint64 maxBytes) : d(std::make_unique<Private>()) {
    d->reader = reader ? std::move(reader) : Reader(&archive::read);
    d->maxBytes = maxBytes;
}

ArchiveCache::~ArchiveCache() = default;

QString ArchiveCache::materialize(const QString& memberPath, QString* error) {
    const auto parts = vpath::split(memberPath);
    if (!parts || vpath::containerKind(parts->container) == vpath::ContainerKind::Pdf) {
        if (error)
            *error = archive::messageUnsupportedArchive();
        return {};
    }
    const QString key = keyFor(*parts);
    QMutexLocker lock(&d->mutex);
    while (d->busy.contains(key))
        d->done.wait(&d->mutex);
    if (auto it = d->items.find(key); it != d->items.end()) {
        if (QFileInfo::exists(it->file)) {
            it->used = ++d->clock;
            return it->file;
        }
        d->total -= it->bytes;
        d->items.erase(it);
    }
    if (!d->ensureDir()) {
        if (error)
            *error = archive::messageDamagedArchive();
        return {};
    }
    const QString suffix = safeSuffix(parts->member);
    const QString file = d->dir->filePath(suffix.isEmpty() ? key : key + u'.' + suffix);
    d->busy.insert(key);
    lock.unlock();

    QString readError;
    std::optional<QByteArray> data = d->reader(parts->container, parts->member, &readError);
    bool ok = data.has_value();
    if (ok) {
        QSaveFile out(file);
        ok = out.open(QIODevice::WriteOnly) && out.write(*data) == data->size() && out.commit();
        if (!ok)
            readError = archive::messageDamagedArchive();
    }

    lock.relock();
    d->busy.remove(key);
    d->done.wakeAll();
    if (!ok) {
        if (error)
            *error = readError.isEmpty() ? archive::messageDamagedArchive() : readError;
        return {};
    }
    const qint64 bytes = data->size();
    d->items.insert(key, Private::Item{file, bytes, ++d->clock});
    d->total += bytes;
    d->evict(key);
    return file;
}

bool ArchiveCache::contains(const QString& memberPath) const {
    const auto parts = vpath::split(memberPath);
    if (!parts)
        return false;
    QMutexLocker lock(&d->mutex);
    return d->items.contains(keyFor(*parts));
}

QString ArchiveCache::directory() const {
    QMutexLocker lock(&d->mutex);
    return d->dir && d->dir->isValid() ? d->dir->path() : QString();
}

qint64 ArchiveCache::cachedBytes() const {
    QMutexLocker lock(&d->mutex);
    return d->total;
}

void ArchiveCache::clear() {
    QMutexLocker lock(&d->mutex);
    for (const auto& item : std::as_const(d->items))
        QFile::remove(item.file);
    d->items.clear();
    d->total = 0;
}

} // namespace lf
