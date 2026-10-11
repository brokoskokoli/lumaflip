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
#include <utility>

namespace lf {

namespace {

constexpr char kDirPrefix[] = "lumaflip-archive-";

// The stamp of an archive: a changed archive gets new keys.
struct Stamp {
    QByteArray container;
    QByteArray size;
    QByteArray modified;
};

Stamp stampOf(const QString& container) {
    const QFileInfo fi(container);
    return Stamp{container.toUtf8(), QByteArray::number(fi.size()),
                 QByteArray::number(fi.lastModified(QTimeZone::UTC).toMSecsSinceEpoch())};
}

QString keyFor(const Stamp& stamp, const QString& member) {
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(stamp.container);
    h.addData(QByteArray(1, '\0'));
    h.addData(stamp.size);
    h.addData(stamp.modified);
    h.addData(QByteArray(1, '\0'));
    h.addData(member.normalized(QString::NormalizationForm_C).toUtf8());
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

// The member's own file name, made safe for every file system (no separators, no control
// characters, no leading dot, bounded length); "image.<suffix>" if nothing is left.
QString safeFileName(const QString& member) {
    const qsizetype slash = member.lastIndexOf(u'/');
    QString name = slash < 0 ? member : member.mid(slash + 1);
    for (QChar& c : name)
        if (c.unicode() < 32 || c == u'\\' || c == u':' || c == u'*' || c == u'?' || c == u'"' || c == u'<' ||
            c == u'>' || c == u'|' || c == u'/')
            c = u'_';
    while (name.startsWith(u'.') || name.startsWith(u' '))
        name.remove(0, 1);
    while (name.endsWith(u'.') || name.endsWith(u' '))
        name.chop(1);
    const QString suffix = safeSuffix(member);
    if (name.size() > 120) {
        const QString tail = suffix.isEmpty() ? QString() : u'.' + suffix;
        name = name.left(120 - tail.size()) + tail;
    }
    if (name.isEmpty())
        name = suffix.isEmpty() ? QStringLiteral("image") : QStringLiteral("image.") + suffix;
    return name;
}

bool readsFrontToBack(const QString& container) {
    return vpath::containerKind(container) == vpath::ContainerKind::LibArchive;
}

} // namespace

struct ArchiveCache::Private {
    RunReader reader;
    qint64 maxBytes = 0;
    mutable QMutex mutex;
    QWaitCondition done;
    std::unique_ptr<QTemporaryDir> dir;
    struct Item {
        QString file;
        qint64 bytes = 0;
        quint64 used = 0;
        int pins = 0; // being read: never evicted
    };
    QHash<QString, Item> items;   // key -> extracted file
    QSet<QString> busy;           // keys being extracted
    QSet<QString> busyContainers; // archives being read front to back
    quint64 clock = 0;
    qint64 total = 0;

    // Under the mutex.
    bool ensureDir() {
        // Also when the directory went away (swept by another instance, cleaned by the
        // system): a new one, the old entries are noticed as missing.
        if (dir && dir->isValid() && QFileInfo(dir->path()).isDir())
            return true;
        dir = std::make_unique<QTemporaryDir>(QDir::tempPath() + u'/' + QLatin1String(kDirPrefix) +
                                              QStringLiteral("XXXXXX"));
        if (!dir->isValid())
            return false;
        QFile::setPermissions(dir->path(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return true;
    }

    static void removeFile(const QString& file) {
        QFile::remove(file);
        QDir().rmdir(QFileInfo(file).absolutePath()); // the key's folder
    }

    // Under the mutex: drops the least recently used files until the budget holds; pinned
    // files (being read) and the newest one stay.
    void evict() {
        while (total > maxBytes) {
            auto oldest = items.end();
            for (auto it = items.begin(); it != items.end(); ++it)
                if (it->pins == 0 && it->used != clock && (oldest == items.end() || it->used < oldest->used))
                    oldest = it;
            if (oldest == items.end())
                return;
            removeFile(oldest->file);
            total -= oldest->bytes;
            items.erase(oldest);
        }
    }

    // Under the mutex: the cached file of `key` if it is still there.
    Item* find(const QString& key) {
        auto it = items.find(key);
        if (it == items.end())
            return nullptr;
        if (QFileInfo::exists(it->file))
            return &*it;
        total -= it->bytes;
        items.erase(it);
        return nullptr;
    }
};

// ---- Pin --------------------------------------------------------------------------------

ArchiveCache::Pin::Pin(Pin&& other) noexcept
    : m_cache(std::exchange(other.m_cache, nullptr)), m_key(std::move(other.m_key)),
      m_file(std::move(other.m_file)) {}

ArchiveCache::Pin& ArchiveCache::Pin::operator=(Pin&& other) noexcept {
    if (this != &other) {
        release();
        m_cache = std::exchange(other.m_cache, nullptr);
        m_key = std::move(other.m_key);
        m_file = std::move(other.m_file);
    }
    return *this;
}

ArchiveCache::Pin::~Pin() {
    release();
}

void ArchiveCache::Pin::release() {
    if (!m_cache)
        return;
    QMutexLocker lock(&m_cache->d->mutex);
    if (auto it = m_cache->d->items.find(m_key); it != m_cache->d->items.end() && it->pins > 0)
        --it->pins;
    m_cache->d->evict();
    m_cache = nullptr;
}

// ---- ArchiveCache -----------------------------------------------------------------------

ArchiveCache& ArchiveCache::instance() {
    static ArchiveCache cache;
    return cache;
}

ArchiveCache::ArchiveCache(Reader reader, qint64 maxBytes, RunReader runReader)
    : d(std::make_unique<Private>()) {
    if (reader)
        d->reader = [reader =
                         std::move(reader)](const QString& container, const QString& member,
                                            QString* error) -> std::optional<QVector<archive::Extracted>> {
            auto data = reader(container, member, error);
            if (!data)
                return std::nullopt;
            return QVector<archive::Extracted>{archive::Extracted{member, std::move(*data)}};
        };
    else
        d->reader = runReader ? std::move(runReader) : RunReader(&archive::readRun);
    d->maxBytes = maxBytes;
}

ArchiveCache::~ArchiveCache() = default;

ArchiveCache::Pin ArchiveCache::acquire(const QString& memberPath, QString* error) {
    const auto parts = vpath::split(memberPath);
    if (!parts || vpath::containerKind(parts->container) == vpath::ContainerKind::Pdf) {
        if (error)
            *error = archive::messageUnsupportedArchive();
        return {};
    }
    const Stamp stamp = stampOf(parts->container);
    const QString key = keyFor(stamp, parts->member);
    const bool sequential = readsFrontToBack(parts->container);
    auto pinned = [&](Private::Item& item) {
        item.used = ++d->clock;
        ++item.pins;
        Pin pin;
        pin.m_cache = this;
        pin.m_key = key;
        pin.m_file = item.file;
        return pin;
    };

    QMutexLocker lock(&d->mutex);
    // One extraction per member; an archive read front to back by one thread at a time (the
    // pass of another thread may bring this member along).
    while (d->busy.contains(key) || (sequential && d->busyContainers.contains(parts->container)))
        d->done.wait(&d->mutex);
    if (Private::Item* item = d->find(key))
        return pinned(*item);
    if (!d->ensureDir()) {
        if (error)
            *error = archive::messageDamagedArchive();
        return {};
    }
    const QString base = d->dir->path();
    d->busy.insert(key);
    if (sequential)
        d->busyContainers.insert(parts->container);
    lock.unlock();

    QString readError;
    std::optional<QVector<archive::Extracted>> run = d->reader(parts->container, parts->member, &readError);
    struct Written {
        QString key;
        QString file;
        qint64 bytes = 0;
    };
    QVector<Written> written;
    bool ok = run.has_value() && !run->isEmpty();
    if (ok) {
        for (qsizetype i = 0; i < run->size(); ++i) {
            const archive::Extracted& e = run->at(i);
            const QString k = i == 0 ? key : keyFor(stamp, e.name);
            if (i > 0) {
                QMutexLocker check(&d->mutex);
                if (d->items.contains(k) || d->busy.contains(k))
                    continue; // there already (or on its way)
            }
            const QString folder = base + u'/' + k;
            const QString file = folder + u'/' + safeFileName(i == 0 ? parts->member : e.name);
            QSaveFile out(file);
            const bool made = QDir().mkpath(folder);
            if (made) // private like the cache folder (POSIX; Windows keeps the profile's ACLs)
                QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner);
            const bool good =
                made && out.open(QIODevice::WriteOnly) && out.write(e.data) == e.data.size() && out.commit();
            if (!good) {
                if (i == 0) {
                    ok = false;
                    readError = archive::messageDamagedArchive();
                    break;
                }
                continue;
            }
            written.append(Written{k, file, qint64(e.data.size())});
        }
    }
    run.reset(); // the bytes are on disk now

    lock.relock();
    d->busy.remove(key);
    if (sequential)
        d->busyContainers.remove(parts->container);
    d->done.wakeAll();
    if (!ok) {
        if (error)
            *error = readError.isEmpty() ? archive::messageDamagedArchive() : readError;
        return {};
    }
    // Newest use for the wanted member, the ones after it in archive order just before.
    const quint64 first = d->clock + 1;
    d->clock += quint64(written.size()) + 1;
    for (qsizetype i = written.size() - 1; i >= 0; --i) {
        const Written& w = written[i];
        if (auto it = d->items.find(w.key); it != d->items.end()) {
            d->total -= it->bytes;
            it->file = w.file;
            it->bytes = w.bytes;
        } else {
            d->items.insert(w.key, Private::Item{w.file, w.bytes, 0, 0});
        }
        d->total += w.bytes;
        // Following members are used after the wanted one: the later, the sooner they go.
        d->items[w.key].used = i == 0 ? d->clock : first + quint64(written.size() - 1 - i);
    }
    Private::Item& item = d->items[key];
    Pin pin = pinned(item);
    d->evict();
    return pin;
}

QString ArchiveCache::materialize(const QString& memberPath, QString* error) {
    return acquire(memberPath, error).file();
}

QString ArchiveCache::cachedFile(const QString& memberPath) const {
    const auto parts = vpath::split(memberPath);
    if (!parts || vpath::containerKind(parts->container) == vpath::ContainerKind::Pdf)
        return {};
    const QString key = keyFor(stampOf(parts->container), parts->member);
    QMutexLocker lock(&d->mutex);
    const auto it = d->items.constFind(key);
    return it == d->items.cend() ? QString() : it->file;
}

bool ArchiveCache::contains(const QString& memberPath) const {
    return !cachedFile(memberPath).isEmpty();
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
    for (auto it = d->items.begin(); it != d->items.end();) {
        if (it->pins > 0) {
            ++it;
            continue;
        }
        Private::removeFile(it->file);
        d->total -= it->bytes;
        it = d->items.erase(it);
    }
}

int ArchiveCache::sweepStaleDirectories(qint64 minAgeSecs, const QString& tempDir) {
    const QDir temp(tempDir.isEmpty() ? QDir::tempPath() : tempDir);
    const QString own = QDir::cleanPath(instance().directory());
    const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(-minAgeSecs);
    int removed = 0;
    const QFileInfoList dirs = temp.entryInfoList({QLatin1String(kDirPrefix) + u'*'},
                                                  QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& fi : dirs) {
        if (QDir::cleanPath(fi.absoluteFilePath()) == own || fi.lastModified(QTimeZone::UTC) > limit)
            continue;
        if (QDir(fi.absoluteFilePath()).removeRecursively())
            ++removed;
    }
    return removed;
}

} // namespace lf
