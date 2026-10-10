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

#include "ImageFormats.h"
#include "Platform.h"
#include "StartupTrace.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QtDebug>
#include <algorithm>
#include <atomic>
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
constexpr qint64 kMaxDecodeBytes = qint64(1) << 30;   // P-08: a decoded image never exceeds 1 GB
constexpr qint64 kMinDecodeBytes = qint64(16) << 20;  // ... nor is capped below 16 MB (tiny budgets)
constexpr qint64 kPrestartHeaderBytes = 64;           // like DecoderRegistry

// Score tiers (higher runs first).
constexpr qint64 kTierActiveCurrent = 4'000'000'000'000LL;
constexpr qint64 kTierActivePrefetch = 3'000'000'000'000LL;
constexpr qint64 kTierInactiveCurrent = 2'000'000'000'000LL;

struct Key {
    QString path;
    Quality quality = Quality::Preview;
    int px = 0;   // decode size bucket (long edge); 0 = full resolution
    int page = 0; // F-FMT-04: page of a multi-page file
    bool operator==(const Key& o) const = default;
};

size_t qHash(const Key& k, size_t seed = 0) {
    return qHashMulti(seed, k.path, int(k.quality), k.px, k.page);
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

Key keyFor(const QString& path, Quality q, int screenPx, int page = 0) {
    switch (q) {
    case Quality::Preview:
        return {path, q, kPreviewPx, page};
    case Quality::Screen:
        return {path, q, screenPx, page};
    case Quality::Full:
        break;
    }
    return {path, Quality::Full, 0, page};
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
    int page = 0;        // page of `current` (F-FMT-04)
    QList<Stage> stages; // qualities requested for `current`
    int priority = 0;    // priority passed with the last request()
    QStringList wanted;  // from setWanted, current first
    int screenPx = kDefaultScreenPx;
    bool active = true;
};

// P-08: byte cap of one decoded image for a given cache budget (P-09).
qint64 decodeByteCapFor(qint64 budget) {
    return std::clamp<qint64>(budget / 2, kMinDecodeBytes, kMaxDecodeBytes);
}

// ImageProvider::prestart(): one decode started before the provider exists (P-01).
struct Prestart {
    QMutex mutex;
    QString path;
    int px = 0;
    bool done = false;
    DecodedImagePtr image;
    // Set by the provider that adopted the running decode; called (under `mutex`) with
    // the result on the decoding thread. Cleared when that provider goes away.
    std::function<void(const DecodedImagePtr&)> adopter;
};

QMutex g_prestartMutex;
std::shared_ptr<Prestart> g_prestart; // guarded by g_prestartMutex
std::atomic<bool> g_prestartPending{false};

} // namespace

struct ImageProvider::Private {
    ImageProvider* q;
    DecoderRegistry* registryPtr; // null: DecoderRegistry::instance(), resolved on first use
    QThreadPool pool;
    std::shared_ptr<Prestart> adopted; // prestart decode this provider took over

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
    std::atomic<bool> rawEmbeddedOnly{false};
    std::atomic<qint64> decodeByteCap{kMaxDecodeBytes};
    QMutex mutex;
    QHash<Key, Job> jobs; // pending + running

    QMutex infoMutex;
    QHash<QString, ImageInfo> infoCache;
    QList<QString> infoOrder; // insertion order, for the FIFO bound

    Private(ImageProvider* q_, DecoderRegistry* r) : q(q_), registryPtr(r) {}

    // Any thread. The process-wide registry is built lazily: loading its Qt image plugins
    // takes a while and is not needed for the first image (P-01).
    DecoderRegistry& registry() const { return registryPtr ? *registryPtr : DecoderRegistry::instance(); }

    // GUI thread: takes over a prestart() decode of `path`. A finished one goes into the
    // cache, a running one becomes a running job of this provider.
    void adoptPrestart(const QString& path) {
        if (!g_prestartPending.load(std::memory_order_acquire))
            return;
        std::shared_ptr<Prestart> st;
        {
            QMutexLocker lock(&g_prestartMutex);
            if (!g_prestart || g_prestart->path != path)
                return;
            st = std::move(g_prestart);
            g_prestart.reset();
            g_prestartPending.store(false, std::memory_order_release);
        }
        QMutexLocker lock(&st->mutex);
        const Key key = keyFor(path, Quality::Screen, st->px);
        if (st->done) {
            if (st->image && !findCached(path, Quality::Screen, st->px))
                insert(key, st->image);
            return;
        }
        Job job;
        job.key = key;
        job.generation = generation.value(path);
        job.score = kTierActiveCurrent;
        job.running = true;
        {
            QMutexLocker jobsLock(&mutex);
            if (jobs.contains(key))
                return; // the same stage is being decoded already
            jobs.insert(key, job);
        }
        adopted = st;
        ImageProvider* self = q;
        st->adopter = [self, job](const DecodedImagePtr& img) {
            // Posted to the provider; Qt drops the call if the provider is gone by then.
            QMetaObject::invokeMethod(
                self, [self, job, img] { self->d->prestartFinished(job, img); }, Qt::QueuedConnection);
        };
    }

    // GUI thread: the adopted prestart decode is done. A failure is not reported: the
    // regular decode (with every backend) runs instead and reports.
    void prestartFinished(const Job& job, const DecodedImagePtr& img) {
        adopted.reset();
        if (img) {
            finished(job, img, {});
            return;
        }
        {
            QMutexLocker lock(&mutex);
            jobs.remove(job.key);
        }
        reschedule();
    }

    // Caller holds `mutex`. A running decode of the same file delivers at least stage `k`.
    bool coveredByRunningJob(const Key& k) const {
        if (k.quality != Quality::Screen)
            return false;
        for (auto it = jobs.cbegin(); it != jobs.cend(); ++it) {
            const Key& o = it.key();
            if (it->running && o.path == k.path && o.page == k.page &&
                (o.quality == Quality::Full || (o.quality == Quality::Screen && o.px >= k.px)))
                return true;
        }
        return false;
    }

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

    // Best cached entry for `path` (page `page`) that satisfies the request (largest wins).
    const Key* findCached(const QString& path, Quality q, int px, int page = 0) const {
        const Key* best = nullptr;
        int bestEdge = -1;
        const auto keys = cacheKeysByPath.constFind(path);
        if (keys == cacheKeysByPath.cend())
            return nullptr;
        for (const Key& k : *keys) {
            if (k.page != page)
                continue;
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
            remove(keyFor(k.path, Quality::Preview, 0, k.page));
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

        auto want = [&](const QString& path, Quality qual, int px, qint64 score, int page = 0) {
            if (findCached(path, qual, px, page))
                return;
            const Key k = keyFor(path, qual, px, page);
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
                    const int page = path == cs.current ? cs.page : 0;
                    if (path == cs.current)
                        stages = cs.stages;
                    if (cs.wanted.contains(path) || stages.isEmpty())
                        stages.append({Quality::Screen, cs.screenPx});
                    if (!hasAnyOfPage(path, page))
                        want(path, Quality::Preview, kPreviewPx, base, page);
                    for (int i = 0; i < stages.size(); ++i)
                        want(path, stages[i].quality, stages[i].px, base - 1 - i, page);
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
                if (coveredByRunningJob(it.key()))
                    continue; // e.g. a larger screen stage of the file is being decoded
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

    bool hasAnyOfPage(const QString& path, int page) const {
        const auto keys = cacheKeysByPath.constFind(path);
        if (keys == cacheKeysByPath.cend())
            return false;
        return std::any_of(keys->cbegin(), keys->cend(), [page](const Key& k) { return k.page == page; });
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
        req.page = job.key.page;
        req.maxPixelSize = job.key.px;
        req.maxBytes = decodeByteCap.load(std::memory_order_relaxed); // P-08
        req.preferEmbeddedPreview =
            job.key.quality == Quality::Preview || (rawEmbeddedOnly.load(std::memory_order_relaxed) &&
                                                    isRawSuffix(QFileInfo(job.key.path).suffix().toLower()));
        QString error;
        DecodedImagePtr result;
        if (trace::enabled())
            trace::once(job.key.quality == Quality::Preview  ? "decode start (Preview)"
                        : job.key.quality == Quality::Screen ? "decode start (Screen)"
                                                             : "decode start (Full)");
        if (auto img = registry().decode(job.key.path, req, &error)) {
            img->page = job.key.page; // F-FMT-04: which page this result is cached as
            result = std::make_shared<const DecodedImage>(std::move(*img));
        }

        if (trace::enabled())
            trace::once(job.key.quality == Quality::Preview  ? "decode done (Preview)"
                        : job.key.quality == Quality::Screen ? "decode done (Screen)"
                                                             : "decode done (Full)");
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
        if (findCached(path, job.key.quality, job.key.px, job.key.page)) {
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
    : QObject(parent), d(new Private(this, &registry)) {
    init();
}

ImageProvider::ImageProvider(QObject* parent) : QObject(parent), d(new Private(this, nullptr)) {
    init();
}

void ImageProvider::init() {
    qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
    qRegisterMetaType<lf::Quality>("lf::Quality");
    d->pool.setMaxThreadCount(std::max(2, QThread::idealThreadCount() - 2));
    d->budget = defaultMemoryBudget();
    d->decodeByteCap = decodeByteCapFor(d->budget);
}

qint64 ImageProvider::defaultMemoryBudget() {
    return std::min(physicalMemory() / 4, kMaxDefaultBudget);
}

qint64 ImageProvider::maxDecodeBytes() const {
    return d->decodeByteCap.load();
}

void ImageProvider::prestart(const QString& path, QSize target) {
    std::shared_ptr<Decoder> decoder(createPrimaryDecoder());
    if (!decoder || path.isEmpty())
        return;
    auto st = std::make_shared<Prestart>();
    st->path = path;
    st->px = bucketFor(target);
    {
        QMutexLocker lock(&g_prestartMutex);
        g_prestart = st;
        g_prestartPending.store(true, std::memory_order_release);
    }
    // The global pool is joined when the application object is destroyed.
    QThreadPool::globalInstance()->start([st, decoder] {
        trace::mark("prestart: decode begins");
        DecodedImagePtr result;
        QFile f(st->path);
        const QByteArray header = f.open(QIODevice::ReadOnly) ? f.read(kPrestartHeaderBytes) : QByteArray();
        f.close();
        if (!header.isEmpty() && decoder->canDecode(st->path, header)) {
            DecodeRequest req;
            req.maxPixelSize = st->px;
            req.maxBytes = decodeByteCapFor(defaultMemoryBudget());
            QString error;
            if (auto img = decoder->decode(st->path, req, &error); img && !img->image.isNull())
                result = std::make_shared<const DecodedImage>(std::move(*img));
        }
        trace::mark("prestart: decode done");
        QMutexLocker lock(&st->mutex);
        st->done = true;
        st->image = result;
        if (st->adopter)
            st->adopter(result);
        st->adopter = nullptr;
    });
}

ImageProvider::~ImageProvider() {
    if (d->adopted) {
        QMutexLocker lock(&d->adopted->mutex);
        d->adopted->adopter = nullptr; // the decode may still run; its result is not wanted
    }
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
    static ImageProvider* provider = new ImageProvider(QCoreApplication::instance());
    return *provider;
}

DecodedImagePtr ImageProvider::request(const QString& path, Quality quality, QSize target, const void* client,
                                       int priority, int page) {
    page = std::max(0, page);
    if (page == 0)
        d->adoptPrestart(path);
    const int px = quality == Quality::Preview ? kPreviewPx : bucketFor(target);
    ClientState& cs = d->clients[client];
    if (cs.current != path || cs.page != page) {
        cs.current = path;
        cs.page = page;
        cs.stages.clear();
    }
    cs.priority = priority;
    const Stage stage{quality, quality == Quality::Screen ? px : (quality == Quality::Full ? 0 : kPreviewPx)};
    if (std::none_of(cs.stages.cbegin(), cs.stages.cend(),
                     [&](const Stage& s) { return s.quality == stage.quality && s.px == stage.px; }))
        cs.stages.append(stage);

    DecodedImagePtr hit;
    if (const Key* k = d->findCached(path, quality, px, page)) {
        hit = d->touch(*k);
    } else if (auto f = d->failed.constFind(path); f != d->failed.cend()) {
        const QString err = f.value();
        QMetaObject::invokeMethod(
            this, [this, path, err] { emit imageFailed(path, err); }, Qt::QueuedConnection);
        d->reschedule();
        return nullptr;
    } else if (const Key* lower = d->findCached(path, Quality::Preview, 0, page)) {
        // Hand out the best lower stage right away as a placeholder.
        const Quality lq = lower->quality;
        DecodedImagePtr img = d->touch(*lower);
        QMetaObject::invokeMethod(
            this, [this, path, lq, img] { emit imageReady(path, lq, img); }, Qt::QueuedConnection);
        if (auto f = d->failedStages.constFind(keyFor(path, quality, px, page));
            f != d->failedStages.cend()) {
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
    if (!ordered.isEmpty())
        d->adoptPrestart(ordered.first());
    ClientState& cs = d->clients[client];
    cs.wanted = ordered;
    cs.screenPx = bucketFor(target);
    cs.active = clientActive;
    if (!ordered.isEmpty() && !ordered.contains(cs.current)) {
        cs.current.clear(); // the wanted list is authoritative
        cs.page = 0;
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

DecodedImagePtr ImageProvider::cached(const QString& path, Quality minQuality, QSize target, int page) {
    page = std::max(0, page);
    if (page == 0)
        d->adoptPrestart(path);
    const int px = minQuality == Quality::Preview ? kPreviewPx : bucketFor(target);
    const Key* k = d->findCached(path, minQuality, px, page);
    return k ? d->touch(*k) : nullptr;
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
                ++it; // running: its result is dropped (older generation)
        }
    }
    // Clients that still show or want the file get it decoded again.
    d->reschedule();
    emit invalidated(path);
}

void ImageProvider::setMemoryBudget(qint64 bytes) {
    d->budget = std::max<qint64>(0, bytes);
    d->decodeByteCap = decodeByteCapFor(d->budget);
    d->evict();
}

void ImageProvider::setRawEmbeddedOnly(bool embeddedOnly) {
    if (d->rawEmbeddedOnly.exchange(embeddedOnly) == embeddedOnly)
        return;
    // Everything of RAW files made with the other setting is outdated: cached stages,
    // decodes in flight (their results are dropped) and what windows show (they are
    // told via invalidated() and get the new stages; their wanted files are re-decoded).
    QSet<QString> raws;
    auto consider = [&raws](const QString& p) {
        if (!p.isEmpty() && isRawSuffix(QFileInfo(p).suffix().toLower()))
            raws.insert(p);
    };
    for (auto it = d->cacheKeysByPath.cbegin(); it != d->cacheKeysByPath.cend(); ++it)
        consider(it.key());
    {
        QMutexLocker lock(&d->mutex);
        for (auto it = d->jobs.cbegin(); it != d->jobs.cend(); ++it)
            consider(it.key().path);
    }
    for (const ClientState& cs : std::as_const(d->clients)) {
        consider(cs.current);
        for (const QString& p : cs.wanted)
            consider(p);
    }
    for (const QString& p : std::as_const(raws))
        invalidate(p);
}

bool ImageProvider::rawEmbeddedOnly() const {
    return d->rawEmbeddedOnly.load();
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
    auto result = d->registry().info(path, &error);
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
