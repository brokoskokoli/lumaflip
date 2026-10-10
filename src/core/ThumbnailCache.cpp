// SPDX-License-Identifier: GPL-3.0-or-later
#include "ThumbnailCache.h"

#include "AppScoped.h"
#include "Decoder.h"

#include <QBuffer>
#include <QCache>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QTimeZone>

#include <algorithm>
#include <atomic>

namespace lf {

namespace {

constexpr int kJpegQuality = 85;
const char* const kSuffix = ".thumb";

QString& defaultDirectoryOverride() {
    static QString dir;
    return dir;
}

QString memKey(const QString& path, int thumbSize) {
    return path + QLatin1Char('\n') + QString::number(thumbSize);
}

struct Job {
    QString path;
    int thumbSize = 0;
    QSet<const void*> clients;
    quint64 seq = 0;        // newest request first (what the user looks at now)
    quint64 generation = 0; // of `path` when the job started (invalidate() bumps it)
};

constexpr int kMaxGenerations = 4096; // bound of the per-path generation table

// First 16 hex digits of the SHA-1 of the path: prefix of all disk files of one path, so
// invalidate() finds them whatever thumbnail size or file stamp they were made for.
QString pathTag(const QString& path) {
    return QString::fromLatin1(
        QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

struct Result {
    QImage image;
    bool fromDisk = false;
};

// Decoded image -> sRGB 8-bit thumbnail with the longer edge <= thumbSize.
QImage toThumbnail(QImage img, int thumbSize) {
    if (img.isNull())
        return {};
    const bool alpha = img.hasAlphaChannel();
    const QImage::Format fmt = alpha ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32;
    if (img.colorSpace().isValid() && img.colorSpace() != QColorSpace(QColorSpace::SRgb))
        img = img.convertedToColorSpace(QColorSpace::SRgb, fmt);
    else if (img.format() != fmt)
        img = img.convertToFormat(fmt);
    if (std::max(img.width(), img.height()) > thumbSize)
        img = img.scaled(thumbSize, thumbSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img.setColorSpace(QColorSpace::SRgb);
    return img;
}

} // namespace

struct ThumbnailCache::Private {
    ThumbnailCache* q;
    DecoderRegistry& registry;
    Options options;
    QThreadPool pool;

    // ---- GUI thread ----
    struct MemEntry {
        QImage image;
    };
    QCache<QString, MemEntry> memory;
    QSet<QString> failed; // memKeys
    quint64 seq = 0;

    // ---- shared with workers ----
    mutable QMutex mutex;
    QHash<QString, Job> queued; // memKey -> job
    QSet<QString> running;      // memKeys
    int runners = 0;
    // invalidate(): results of jobs started before are dropped (generation), and the
    // path's disk files are deleted before anything reads or writes them again.
    QHash<QString, quint64> generations;
    QSet<QString> purgeDisk; // paths whose disk files must go
    std::atomic<int> decodes{0};
    std::atomic<int> hits{0};

    mutable QMutex diskMutex;
    qint64 diskBytes = -1; // -1: folder not scanned yet
    bool dirCreated = false;

    Private(ThumbnailCache* owner, DecoderRegistry& reg, Options opts)
        : q(owner), registry(reg), options(std::move(opts)) {
        pool.setMaxThreadCount(std::max(1, options.threads));
        pool.setThreadPriority(QThread::LowestPriority);
        memory.setMaxCost(int(std::max<qint64>(1, options.memoryBudget >> 10))); // KiB
    }

    // ---- workers ----
    void startRunners() { // mutex held
        while (runners < pool.maxThreadCount() && runners < queued.size()) {
            ++runners;
            pool.start([this] { runLoop(); });
        }
    }

    void runLoop() {
        for (;;) {
            Job job;
            {
                QMutexLocker lock(&mutex);
                if (queued.isEmpty()) {
                    --runners;
                    return;
                }
                auto best = queued.begin();
                for (auto it = queued.begin(); it != queued.end(); ++it)
                    if (it->seq > best->seq)
                        best = it;
                job = best.value();
                job.generation = generations.value(job.path);
                running.insert(best.key());
                queued.erase(best);
            }
            purgeIfPending(job.path);
            const Result r = generate(job.path, job.thumbSize, job.generation);
            const QString key = memKey(job.path, job.thumbSize);
            QMetaObject::invokeMethod(q, [this, key, job, r] { deliver(key, job, r); }, Qt::QueuedConnection);
        }
    }

    QString diskPath(const QString& path, qint64 size, qint64 mtime, int thumbSize) const {
        return options.directory + QLatin1Char('/') + diskFileName(path, size, mtime, thumbSize);
    }

    // Any thread: deletes the disk files of `path` if invalidate() asked for it.
    void purgeIfPending(const QString& path) {
        {
            QMutexLocker lock(&mutex);
            if (!purgeDisk.remove(path))
                return;
        }
        if (options.directory.isEmpty())
            return;
        QMutexLocker lock(&diskMutex);
        const QDir dir(options.directory);
        const QFileInfoList files =
            dir.entryInfoList({pathTag(path) + QStringLiteral("-*") + QLatin1String(kSuffix)}, QDir::Files);
        for (const QFileInfo& fi : files) {
            const qint64 size = fi.size();
            if (QFile::remove(fi.absoluteFilePath()) && diskBytes >= 0)
                diskBytes = std::max<qint64>(0, diskBytes - size);
        }
    }

    bool isCurrent(const QString& path, quint64 generation) const {
        QMutexLocker lock(&mutex);
        return generations.value(path) == generation && !purgeDisk.contains(path);
    }

    Result generate(const QString& path, int thumbSize, quint64 generation) {
        Result r;
        const QFileInfo fi(path);
        if (!fi.isFile())
            return r;
        const qint64 size = fi.size();
        const qint64 mtime = fi.lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
        const QString file = diskPath(path, size, mtime, thumbSize);
        const bool disk = !options.directory.isEmpty();
        if (disk) {
            QFile f(file);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray bytes = f.readAll();
                // Last use = modification time of the cache file (LRU eviction order).
                f.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
                f.close();
                QImage img = QImage::fromData(bytes);
                if (!img.isNull()) {
                    img.setColorSpace(QColorSpace::SRgb);
                    r.image = std::move(img);
                    r.fromDisk = true;
                    ++hits;
                    return r;
                }
                QFile::remove(f.fileName()); // damaged entry
            }
        }
        DecodeRequest req;
        req.maxPixelSize = thumbSize;
        req.preferEmbeddedPreview = true; // EXIF/RAW previews are much faster (SPEC 7.1)
        req.maxBytes = qint64(64) << 20;
        QString error;
        auto decoded = registry.decode(path, req, &error);
        if (decoded) {
            const QSize full = decoded->fullSize.isEmpty() ? decoded->image.size() : decoded->fullSize;
            const int got = std::max(decoded->image.width(), decoded->image.height());
            // An embedded preview much smaller than wanted (old cameras: 160 px) is not enough.
            if (got * 2 < thumbSize && std::max(full.width(), full.height()) > got) {
                req.preferEmbeddedPreview = false;
                if (auto better = registry.decode(path, req, &error))
                    decoded = std::move(better);
            }
        }
        if (!decoded)
            return r;
        ++decodes;
        r.image = toThumbnail(decoded->image, thumbSize);
        if (disk && !r.image.isNull())
            store(file, r.image, path, generation);
        return r;
    }

    // Writes the thumbnail unless `path` was invalidated since the job started (checked
    // under diskMutex, which a purge holds as well: a stale thumbnail is never kept).
    void store(const QString& file, const QImage& image, const QString& path, quint64 generation) {
        QByteArray bytes;
        {
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            const bool alpha = image.hasAlphaChannel();
            if (!image.save(&buf, alpha ? "PNG" : "JPEG", alpha ? -1 : kJpegQuality))
                return;
        }
        QMutexLocker lock(&diskMutex);
        if (!isCurrent(path, generation))
            return;
        if (!dirCreated) {
            if (!QDir().mkpath(options.directory))
                return;
            dirCreated = true;
        }
        scanLocked();
        const qint64 old = QFileInfo(file).size(); // 0 if missing
        QSaveFile f(file);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
            return;
        diskBytes += bytes.size() - old;
        if (diskBytes > options.diskBudget)
            evictLocked(file);
    }

    void scanLocked() { // diskMutex held
        if (diskBytes >= 0)
            return;
        qint64 total = 0;
        const QDir dir(options.directory);
        for (const QFileInfo& fi :
             dir.entryInfoList({QStringLiteral("*") + QLatin1String(kSuffix)}, QDir::Files))
            total += fi.size();
        diskBytes = total;
    }

    // Removes the least recently used files until 90 % of the budget is left. `keep` (the
    // file just written) stays.
    void evictLocked(const QString& keep) {
        const QDir dir(options.directory);
        const QFileInfoList files =
            dir.entryInfoList({QStringLiteral("*") + QLatin1String(kSuffix)}, QDir::Files,
                              QDir::Time | QDir::Reversed); // oldest first
        qint64 total = 0;
        for (const QFileInfo& fi : files)
            total += fi.size();
        const qint64 target = options.diskBudget / 10 * 9;
        for (const QFileInfo& fi : files) {
            if (total <= target)
                break;
            if (fi.absoluteFilePath() == QFileInfo(keep).absoluteFilePath())
                continue;
            if (QFile::remove(fi.absoluteFilePath()))
                total -= fi.size();
        }
        diskBytes = total;
    }

    // ---- GUI thread ----
    void deliver(const QString& key, const Job& job, const Result& r) {
        {
            QMutexLocker lock(&mutex);
            running.remove(key);
            if (generations.value(job.path) != job.generation) {
                // Started before invalidate(): the result may show the old file. Generated
                // again for the clients that asked (unless they asked again meanwhile).
                if (!job.clients.isEmpty() && !queued.contains(key)) {
                    Job again = job;
                    again.seq = ++seq;
                    queued.insert(key, again);
                    startRunners();
                }
                return;
            }
        }
        if (r.image.isNull()) {
            failed.insert(key);
            emit q->thumbnailFailed(job.path, job.thumbSize);
            return;
        }
        const qint64 kib = std::max<qint64>(1, r.image.sizeInBytes() >> 10);
        memory.insert(key, new MemEntry{r.image}, int(kib));
        emit q->thumbnailReady(job.path, job.thumbSize);
    }
};

ThumbnailCache::ThumbnailCache(DecoderRegistry& registry, Options options, QObject* parent)
    : QObject(parent), d(std::make_unique<Private>(this, registry, std::move(options))) {}

ThumbnailCache::~ThumbnailCache() {
    {
        QMutexLocker lock(&d->mutex);
        d->queued.clear();
    }
    d->pool.waitForDone();
}

ThumbnailCache& ThumbnailCache::instance() {
    // Owned by the application object so the worker threads are joined before Qt shuts
    // down; created again for a new application object (AppScoped).
    static AppScoped<ThumbnailCache> cache([](QObject* parent) {
        Options o;
        o.directory = defaultDirectory();
        o.threads = std::clamp(QThread::idealThreadCount() / 4, 1, 2);
        return new ThumbnailCache(DecoderRegistry::instance(), o, parent);
    });
    return cache.get();
}

QString ThumbnailCache::defaultDirectory() {
    if (!defaultDirectoryOverride().isEmpty())
        return defaultDirectoryOverride();
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/thumbs");
}

void ThumbnailCache::setDefaultDirectory(const QString& directory) {
    defaultDirectoryOverride() = directory;
}

QString ThumbnailCache::diskFileName(const QString& path, qint64 fileSize, qint64 modifiedMs, int thumbSize) {
    return pathTag(path) + QLatin1Char('-') +
           QString::fromLatin1(cacheKey(path, fileSize, modifiedMs, thumbSize)) + QLatin1String(kSuffix);
}

QByteArray ThumbnailCache::cacheKey(const QString& path, qint64 fileSize, qint64 modifiedMs, int thumbSize) {
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(path.toUtf8());
    h.addData(QByteArrayView("\0", 1));
    h.addData(QByteArray::number(fileSize));
    h.addData(QByteArrayView("\0", 1));
    h.addData(QByteArray::number(modifiedMs));
    h.addData(QByteArrayView("\0", 1));
    h.addData(QByteArray::number(thumbSize));
    return h.result().toHex();
}

QImage ThumbnailCache::request(const QString& path, int thumbSize, const void* client) {
    const QString key = memKey(path, thumbSize);
    if (const auto* e = d->memory.object(key)) // also marks it as recently used
        return e->image;
    if (d->failed.contains(key))
        return {};
    QMutexLocker lock(&d->mutex);
    if (d->running.contains(key))
        return {};
    Job& job = d->queued[key];
    job.path = path;
    job.thumbSize = thumbSize;
    job.clients.insert(client);
    job.seq = ++d->seq;
    d->startRunners();
    return {};
}

QImage ThumbnailCache::cached(const QString& path, int thumbSize) const {
    const auto* e = d->memory.object(memKey(path, thumbSize));
    return e ? e->image : QImage();
}

void ThumbnailCache::retain(const void* client, const QStringList& paths) {
    const QSet<QString> keep(paths.cbegin(), paths.cend());
    QMutexLocker lock(&d->mutex);
    for (auto it = d->queued.begin(); it != d->queued.end();) {
        if (it->clients.contains(client) && !keep.contains(it->path)) {
            it->clients.remove(client);
            if (it->clients.isEmpty()) {
                it = d->queued.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void ThumbnailCache::removeClient(const void* client) {
    retain(client, {});
}

void ThumbnailCache::invalidate(const QString& path) {
    const QString prefix = path + QLatin1Char('\n');
    const QStringList keys = d->memory.keys();
    for (const QString& k : keys)
        if (k.startsWith(prefix))
            d->memory.remove(k);
    for (auto it = d->failed.begin(); it != d->failed.end();)
        it = it->startsWith(prefix) ? d->failed.erase(it) : std::next(it);
    {
        QMutexLocker lock(&d->mutex);
        if (d->generations.size() >= kMaxGenerations && !d->generations.contains(path)) {
            // Only jobs compare generations: entries of paths without a job can go.
            QSet<QString> busy;
            for (const Job& j : std::as_const(d->queued))
                busy.insert(j.path);
            for (const QString& k : std::as_const(d->running))
                busy.insert(k.section(QLatin1Char('\n'), 0, 0));
            d->generations.removeIf([&](const auto& it) { return !busy.contains(it.key()); });
        }
        ++d->generations[path];
        if (d->options.directory.isEmpty())
            return;
        d->purgeDisk.insert(path);
    }
    // The disk files go on a worker (no file system access on the GUI thread); a job of
    // the path purges first if it gets there earlier.
    d->pool.start([priv = d.get(), path] { priv->purgeIfPending(path); });
}

int ThumbnailCache::pendingJobs() const {
    QMutexLocker lock(&d->mutex);
    return int(d->queued.size() + d->running.size());
}

int ThumbnailCache::decodeCount() const {
    return d->decodes.load();
}

int ThumbnailCache::diskHits() const {
    return d->hits.load();
}

qint64 ThumbnailCache::diskUsage() const {
    if (d->options.directory.isEmpty())
        return 0;
    QMutexLocker lock(&d->diskMutex);
    d->scanLocked();
    return d->diskBytes;
}

QString ThumbnailCache::directory() const {
    return d->options.directory;
}

void ThumbnailCache::clearMemory() {
    d->memory.clear();
    d->failed.clear();
}

} // namespace lf
