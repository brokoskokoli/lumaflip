// SPDX-License-Identifier: GPL-3.0-or-later
//
// Image pipeline (SPEC 7.1, F-WIN-09, P-03, P-09).
//
// Design
// - All bookkeeping (clients, cache, priorities) lives on the GUI thread. Worker
//   threads only touch the job table (guarded by `mutex`) and the decoder registry.
// - Jobs are keyed by (path, quality, size bucket) - the same key as the cache - so
//   identical requests from several windows are decoded once (dedupe).
// - Every scheduled job starts one pool runnable; a runnable picks the *currently*
//   best pending job when it gets a thread. Priorities can therefore change after
//   scheduling, and cancelled jobs simply disappear from the table before any
//   expensive work starts.
// - reschedule() recomputes the desired job set from all clients after every
//   change (request, setWanted, removeClient, invalidate, job finished). Pending
//   jobs not desired any more are dropped; running ones finish and are cached.
// - Per-path generation counters discard results of jobs that were started before
//   invalidate(path).
// - Results are emitted in quality order: a stage that arrives after an equal or
//   better cached one is dropped, so clients never see Full followed by Screen.
// - Failures are tracked per stage. A path counts as failed (imageFailed) only when
//   no stage of it is cached or still in flight; otherwise stageFailed is emitted and
//   the stage is not retried until invalidate().
// - Per-path bookkeeping (info cache, failures, generations) is pruned so it stays
//   bounded while browsing through many folders.

#include "ImageProvider.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QtDebug>
#include <algorithm>
#include <limits>

#if defined(Q_OS_DARWIN)
#include <sys/sysctl.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace lf {

namespace {

constexpr int kPreviewPx = 512;
constexpr int kBucketPx = 256;
constexpr int kDefaultScreenPx = 2560;
constexpr qint64 kMaxDefaultBudget = qint64(2) << 30; // 2 GB (P-09)
constexpr int kMaxInfoEntries = 2048;                 // info() cache (FIFO)
constexpr int kMaxPathBookkeeping = 512;              // failed / failedStages / generation before pruning

// Score tiers (higher runs first).
constexpr qint64 kTierActiveCurrent = 4'000'000'000'000LL;
constexpr qint64 kTierActivePrefetch = 3'000'000'000'000LL;
constexpr qint64 kTierInactiveCurrent = 2'000'000'000'000LL;

struct Key {
    QString path;
    Quality quality = Quality::Preview;
    int px = 0; // decode size bucket (long edge); 0 = full resolution
    bool operator==(const Key& o) const = default;
};

size_t qHash(const Key& k, size_t seed = 0) {
    return qHashMulti(seed, k.path, int(k.quality), k.px);
}

int longEdge(QSize s) {
    return std::max(s.width(), s.height());
}

int bucketFor(QSize target) {
    const int m = longEdge(target);
    if (m <= 0)
        return kDefaultScreenPx;
    return ((m + kBucketPx - 1) / kBucketPx) * kBucketPx;
}

Key keyFor(const QString& path, Quality q, int screenPx) {
    switch (q) {
    case Quality::Preview:
        return {path, q, kPreviewPx};
    case Quality::Screen:
        return {path, q, screenPx};
    case Quality::Full:
        break;
    }
    return {path, Quality::Full, 0};
}

qint64 physicalMemory() {
#if defined(Q_OS_DARWIN)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0)
        return qint64(mem);
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX st{};
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st))
        return qint64(st.ullTotalPhys);
#else
    const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0)
        return qint64(pages) * pageSize;
#endif
    return qint64(8) << 30;
}

struct CacheEntry {
    DecodedImagePtr image;
    qint64 bytes = 0;
    quint64 tick = 0;
};

struct Job {
    Key key;
    quint64 generation = 0;
    qint64 score = 0;
    bool running = false;
};

struct Stage {
    Quality quality;
    int px;
};

struct ClientState {
    QString current;     // last path passed to request()
    QList<Stage> stages; // qualities requested for `current`
    int priority = 0;    // priority passed with the last request()
    QStringList wanted;  // from setWanted, current first
    int screenPx = kDefaultScreenPx;
    bool active = true;
};

} // namespace

struct ImageProvider::Private {
    ImageProvider* q;
    DecoderRegistry& registry;
    QThreadPool pool;

    // ---- GUI thread only ----
    QHash<Key, CacheEntry> cache;
    QHash<QString, QList<Key>> cacheKeysByPath;
    qint64 usage = 0;
    qint64 budget = 0;
    quint64 tick = 0;
    QHash<const void*, ClientState> clients;
    QSet<QString> pinned;             // paths currently wanted by any client
    QHash<QString, QString> failed;   // path -> error (nothing of the path can be shown)
    QHash<Key, QString> failedStages; // stage -> error (not retried until invalidate)
    QHash<QString, quint64> generation;

    // ---- shared with workers ----
    QMutex mutex;
    QHash<Key, Job> jobs; // pending + running

    QMutex infoMutex;
    QHash<QString, ImageInfo> infoCache;
    QList<QString> infoOrder; // insertion order, for the FIFO bound

    Private(ImageProvider* q_, DecoderRegistry& r) : q(q_), registry(r) {}

    // -- cache --
    static bool satisfies(const Key& k, const DecodedImage& im, Quality q, int px) {
        const int have = longEdge(im.image.size());
        const int full = longEdge(im.fullSize);
        const bool complete = full > 0 && have >= full;
        switch (q) {
        case Quality::Preview:
            return true;
        case Quality::Screen:
            return k.quality == Quality::Full || complete ||
                   (k.quality == Quality::Screen && (k.px == 0 || k.px >= px));
        case Quality::Full:
            return k.quality == Quality::Full || complete;
        }
        return false;
    }

    // Best cached entry for `path` that satisfies the request (largest wins).
    const Key* findCached(const QString& path, Quality q, int px) const {
        const Key* best = nullptr;
        int bestEdge = -1;
        const auto keys = cacheKeysByPath.constFind(path);
        if (keys == cacheKeysByPath.cend())
            return nullptr;
        for (const Key& k : *keys) {
            const CacheEntry& e = cache[k];
            if (!satisfies(k, *e.image, q, px))
                continue;
            const int edge = longEdge(e.image->image.size());
            if (edge > bestEdge) {
                best = &k;
                bestEdge = edge;
            }
        }
        return best;
    }

    DecodedImagePtr touch(const Key& k) {
        auto it = cache.find(k);
        it->tick = ++tick;
        return it->image;
    }

    void insert(const Key& k, DecodedImagePtr img) {
        remove(k);
        CacheEntry e{img, img->image.sizeInBytes(), ++tick};
        usage += e.bytes;
        cache.insert(k, e);
        cacheKeysByPath[k.path].append(k);
        // A real stage supersedes the preview of the same file.
        if (k.quality != Quality::Preview)
            remove(keyFor(k.path, Quality::Preview, 0));
    }

    void remove(const Key& k) {
        auto it = cache.find(k);
        if (it == cache.end())
            return;
        usage -= it->bytes;
        cache.erase(it);
        auto p = cacheKeysByPath.find(k.path);
        if (p != cacheKeysByPath.end()) {
            p->removeAll(k);
            if (p->isEmpty())
                cacheKeysByPath.erase(p);
        }
    }

    void removePath(const QString& path) {
        const QList<Key> keys = cacheKeysByPath.value(path);
        for (const Key& k : keys)
            remove(k);
    }

    // P-09: drop least recently used entries of files nobody wants.
    void evict() {
        while (usage > budget) {
            const Key* victim = nullptr;
            quint64 oldest = std::numeric_limits<quint64>::max();
            for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
                if (pinned.contains(it.key().path))
                    continue;
                if (it->tick < oldest) {
                    oldest = it->tick;
                    victim = &it.key();
                }
            }
            if (!victim)
                return; // everything left is wanted - never evict those
            remove(Key(*victim));
        }
    }

    // -- scheduling --
    void reschedule() {
        QHash<Key, qint64> desired;
        pinned.clear();

        auto want = [&](const QString& path, Quality qual, int px, qint64 score) {
            if (findCached(path, qual, px))
                return;
            const Key k = keyFor(path, qual, px);
            if (failedStages.contains(k))
                return;
            auto it = desired.find(k);
            if (it == desired.end())
                desired.insert(k, score);
            else
                *it = std::max(*it, score);
        };

        for (auto c = clients.cbegin(); c != clients.cend(); ++c) {
            const ClientState& cs = *c;
            QStringList order;
            if (!cs.current.isEmpty())
                order << cs.current;
            for (const QString& p : cs.wanted)
                if (!order.contains(p))
                    order << p;
            if (!cs.active && order.size() > 1)
                order = order.mid(0, 1); // F-WIN-09: hidden windows keep only the current image

            for (int rank = 0; rank < order.size(); ++rank) {
                const QString& path = order[rank];
                pinned.insert(path);
                if (failed.contains(path))
                    continue;
                const bool hasAny = cacheKeysByPath.contains(path);
                if (rank == 0) {
                    const qint64 base = (cs.active ? kTierActiveCurrent : kTierInactiveCurrent) +
                                        qint64(std::clamp(cs.priority, -1000, 1000)) * 1'000'000;
                    QList<Stage> stages;
                    if (path == cs.current)
                        stages = cs.stages;
                    if (cs.wanted.contains(path) || stages.isEmpty())
                        stages.append({Quality::Screen, cs.screenPx});
                    if (!hasAny)
                        want(path, Quality::Preview, kPreviewPx, base);
                    for (int i = 0; i < stages.size(); ++i)
                        want(path, stages[i].quality, stages[i].px, base - 1 - i);
                } else {
                    // Prefetch neighbours: all previews first (fast flipping), then screens.
                    const qint64 base = kTierActivePrefetch - qint64(rank) * 10;
                    if (!hasAny)
                        want(path, Quality::Preview, kPreviewPx, base);
                    want(path, Quality::Screen, cs.screenPx, base - 1'000'000);
                }
            }
        }

        int toStart = 0;
        {
            QMutexLocker lock(&mutex);
            for (auto it = jobs.begin(); it != jobs.end();) {
                if (!it->running && !desired.contains(it.key()))
                    it = jobs.erase(it); // cancelled before it started
                else
                    ++it;
            }
            for (auto it = desired.cbegin(); it != desired.cend(); ++it) {
                auto j = jobs.find(it.key());
                if (j != jobs.end()) {
                    j->score = it.value();
                    continue;
                }
                Job job;
                job.key = it.key();
                job.generation = generation.value(it.key().path);
                job.score = it.value();
                jobs.insert(job.key, job);
                ++toStart;
            }
        }
        for (int i = 0; i < toStart; ++i)
            pool.start([this] { runNext(); });
        prune();
    }

    // Drops per-path bookkeeping of files nobody wants, has cached or is decoding.
    void prune() {
        if (failed.size() <= kMaxPathBookkeeping && failedStages.size() <= kMaxPathBookkeeping &&
            generation.size() <= kMaxPathBookkeeping)
            return;
        QSet<QString> busy;
        {
            QMutexLocker lock(&mutex);
            for (auto it = jobs.cbegin(); it != jobs.cend(); ++it)
                busy.insert(it.key().path);
        }
        auto keep = [&](const QString& path) {
            return pinned.contains(path) || cacheKeysByPath.contains(path) || busy.contains(path);
        };
        failed.removeIf([&](const auto& it) { return !keep(it.key()); });
        failedStages.removeIf([&](const auto& it) { return !keep(it.key().path); });
        // A generation is only compared against jobs of the same path, so it can go once
        // no job of that path exists any more.
        generation.removeIf([&](const auto& it) { return !busy.contains(it.key()); });
    }

    bool hasJobsFor(const QString& path) {
        QMutexLocker lock(&mutex);
        for (auto it = jobs.cbegin(); it != jobs.cend(); ++it)
            if (it.key().path == path)
                return true;
        return false;
    }

    // Worker thread: take the best pending job and decode it.
    void runNext() {
        Job job;
        {
            QMutexLocker lock(&mutex);
            Job* best = nullptr;
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                if (!it->running && (!best || it->score > best->score))
                    best = &*it;
            }
            if (!best)
                return; // the job this runnable was started for got cancelled
            best->running = true;
            job = *best;
        }

        DecodeRequest req;
        req.maxPixelSize = job.key.px;
        req.preferEmbeddedPreview = job.key.quality == Quality::Preview;
        QString error;
        DecodedImagePtr result;
        if (auto img = registry.decode(job.key.path, req, &error))
            result = std::make_shared<const DecodedImage>(std::move(*img));

        QMetaObject::invokeMethod(
            q, [this, job, result, error] { finished(job, result, error); }, Qt::QueuedConnection);
    }

    // GUI thread.
    void finished(const Job& job, const DecodedImagePtr& img, const QString& error) {
        {
            QMutexLocker lock(&mutex);
            jobs.remove(job.key);
        }
        const QString& path = job.key.path;
        if (job.generation != generation.value(path)) {
            reschedule(); // stale (invalidated meanwhile)
            return;
        }
        if (!img) {
            if (failed.contains(path))
                return; // already reported as a whole
            failedStages.insert(job.key, error);
            reschedule(); // never re-queues the failed stage
            if (cacheKeysByPath.contains(path) || hasJobsFor(path)) {
                // Something else of this file is shown or may still arrive.
                emit q->stageFailed(path, job.key.quality, error);
                return;
            }
            failed.insert(path, error);
            emit q->imageFailed(path, error);
            return;
        }
        // Keep stages monotonic: a result arriving after an equal or better cached stage
        // (e.g. Screen finishing after Full) would downgrade the view.
        if (findCached(path, job.key.quality, job.key.px)) {
            reschedule();
            return;
        }
        insert(job.key, img);
        reschedule();
        evict();
        emit q->imageReady(path, job.key.quality, img);
    }
};

ImageProvider::ImageProvider(DecoderRegistry& registry, QObject* parent)
    : QObject(parent), d(new Private(this, registry)) {
    qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
    qRegisterMetaType<lf::Quality>("lf::Quality");
    d->pool.setMaxThreadCount(std::max(2, QThread::idealThreadCount() - 2));
    d->budget = std::min(physicalMemory() / 4, kMaxDefaultBudget);
}

ImageProvider::~ImageProvider() {
    {
        QMutexLocker lock(&d->mutex);
        d->jobs.clear(); // pending runnables find nothing to do
    }
    d->pool.clear();
    d->pool.waitForDone();
    delete d;
}

ImageProvider& ImageProvider::instance() {
    // Parented to the application so worker threads are joined before Qt shuts down.
    static ImageProvider* provider =
        new ImageProvider(DecoderRegistry::instance(), QCoreApplication::instance());
    return *provider;
}

DecodedImagePtr ImageProvider::request(const QString& path, Quality quality, QSize target, const void* client,
                                       int priority) {
    const int px = quality == Quality::Preview ? kPreviewPx : bucketFor(target);
    ClientState& cs = d->clients[client];
    if (cs.current != path) {
        cs.current = path;
        cs.stages.clear();
    }
    cs.priority = priority;
    const Stage stage{quality, quality == Quality::Screen ? px : (quality == Quality::Full ? 0 : kPreviewPx)};
    if (std::none_of(cs.stages.cbegin(), cs.stages.cend(),
                     [&](const Stage& s) { return s.quality == stage.quality && s.px == stage.px; }))
        cs.stages.append(stage);

    DecodedImagePtr hit;
    if (const Key* k = d->findCached(path, quality, px)) {
        hit = d->touch(*k);
    } else if (auto f = d->failed.constFind(path); f != d->failed.cend()) {
        const QString err = f.value();
        QMetaObject::invokeMethod(
            this, [this, path, err] { emit imageFailed(path, err); }, Qt::QueuedConnection);
        d->reschedule();
        return nullptr;
    } else if (const Key* lower = d->findCached(path, Quality::Preview, 0)) {
        // Hand out the best lower stage right away as a placeholder.
        const Quality lq = lower->quality;
        DecodedImagePtr img = d->touch(*lower);
        QMetaObject::invokeMethod(
            this, [this, path, lq, img] { emit imageReady(path, lq, img); }, Qt::QueuedConnection);
        if (auto f = d->failedStages.constFind(keyFor(path, quality, px)); f != d->failedStages.cend()) {
            const QString err = f.value();
            QMetaObject::invokeMethod(
                this, [this, path, quality, err] { emit stageFailed(path, quality, err); },
                Qt::QueuedConnection);
        }
    }
    d->reschedule();
    return hit;
}

void ImageProvider::setWanted(const void* client, const QStringList& ordered, QSize target,
                              bool clientActive) {
    ClientState& cs = d->clients[client];
    cs.wanted = ordered;
    cs.screenPx = bucketFor(target);
    cs.active = clientActive;
    if (!ordered.isEmpty() && !ordered.contains(cs.current)) {
        cs.current.clear(); // the wanted list is authoritative
        cs.stages.clear();
    }
    d->reschedule();
    d->evict();
}

void ImageProvider::removeClient(const void* client) {
    d->clients.remove(client);
    d->reschedule();
    d->evict();
}

void ImageProvider::invalidate(const QString& path) {
    ++d->generation[path];
    d->removePath(path);
    d->failed.remove(path);
    d->failedStages.removeIf([&](const auto& it) { return it.key().path == path; });
    {
        QMutexLocker lock(&d->infoMutex);
        if (d->infoCache.remove(path))
            d->infoOrder.removeOne(path);
    }
    {
        QMutexLocker lock(&d->mutex);
        for (auto it = d->jobs.begin(); it != d->jobs.end();) {
            if (!it->running && it.key().path == path)
                it = d->jobs.erase(it);
            else
                ++it;
        }
    }
    d->reschedule();
}

void ImageProvider::setMemoryBudget(qint64 bytes) {
    d->budget = std::max<qint64>(0, bytes);
    d->evict();
}

qint64 ImageProvider::memoryUsage() const {
    return d->usage;
}

std::optional<ImageInfo> ImageProvider::info(const QString& path) {
    {
        QMutexLocker lock(&d->infoMutex);
        auto it = d->infoCache.constFind(path);
        if (it != d->infoCache.cend())
            return *it;
    }
    QString error;
    auto result = d->registry.info(path, &error);
    if (result) {
        QMutexLocker lock(&d->infoMutex);
        if (!d->infoCache.contains(path))
            d->infoOrder.append(path);
        d->infoCache.insert(path, *result);
        while (d->infoOrder.size() > kMaxInfoEntries)
            d->infoCache.remove(d->infoOrder.takeFirst());
    }
    return result;
}

int ImageProvider::bookkeepingEntries() const {
    QMutexLocker lock(&d->infoMutex);
    return int(d->infoCache.size() + d->failed.size() + d->failedStages.size() + d->generation.size());
}

} // namespace lf
