// SPDX-License-Identifier: GPL-3.0-or-later
#include "FolderCache.h"

#include "Collection.h"
#include "Platform.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QTimeZone>
#include <QWaitCondition>

#include <atomic>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace lf {

namespace {
constexpr int kMaxCountEntries = 4096; // bound of the instrumentation table
std::atomic<quint64> g_localChangeSeq{0};
} // namespace

struct FolderCache::Private {
    struct Entry {
        int leases = 0;
        int waiters = 0;            // threads inside get() for this folder
        quint64 wanted = 1;         // generation a fresh snapshot must have
        bool inFlight = false;      // an enumeration is running
        FolderSnapshotPtr snapshot; // may be stale (generation < wanted)
    };

    mutable QMutex mutex;
    QWaitCondition changed;
    QHash<QString, Entry> entries;
    QHash<QString, int> counts;
    int total = 0;

    // Caller holds `mutex`.
    void dropIfUnused(const QString& folder) {
        auto it = entries.find(folder);
        if (it != entries.end() && it->leases <= 0 && it->waiters <= 0 && !it->inFlight)
            entries.erase(it);
    }
};

FolderCache::FolderCache() : d(std::make_unique<Private>()) {}
FolderCache::~FolderCache() = default;

FolderCache& FolderCache::instance() {
    static FolderCache* cache = new FolderCache; // lives until process exit (workers may use it)
    return *cache;
}

void FolderCache::acquire(const QString& folderPath) {
    if (folderPath.isEmpty())
        return;
    QMutexLocker lock(&d->mutex);
    ++d->entries[folderPath].leases;
}

void FolderCache::release(const QString& folderPath) {
    if (folderPath.isEmpty())
        return;
    QMutexLocker lock(&d->mutex);
    auto it = d->entries.find(folderPath);
    if (it == d->entries.end())
        return;
    --it->leases;
    d->dropIfUnused(folderPath);
}

void FolderCache::invalidate(const QString& folderPath) {
    QMutexLocker lock(&d->mutex);
    auto it = d->entries.find(folderPath);
    if (it != d->entries.end())
        ++it->wanted;
}

quint64 FolderCache::noteLocalChange(const QString& folderPath) {
    // Bumped before the invalidation: an enumeration that starts after it reads a sequence
    // number at least as large.
    const quint64 seq = g_localChangeSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    invalidate(folderPath);
    return seq;
}

quint64 FolderCache::localChangeSeq() {
    return g_localChangeSeq.load(std::memory_order_acquire);
}

qint64 FolderCache::folderStamp(const QString& folderPath) {
#ifdef Q_OS_UNIX
    struct stat st;
    if (::stat(QFile::encodeName(folderPath).constData(), &st) != 0)
        return -1;
#ifdef Q_OS_DARWIN
    return qint64(st.st_mtimespec.tv_sec) * 1'000'000'000 + st.st_mtimespec.tv_nsec;
#else
    return qint64(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
#endif
#else
    const QDateTime t = QFileInfo(folderPath).lastModified(QTimeZone::UTC);
    return t.isValid() ? t.toMSecsSinceEpoch() * 1'000'000 : -1;
#endif
}

bool FolderCache::isStale(const QString& folderPath) const {
    FolderSnapshotPtr snap;
    {
        QMutexLocker lock(&d->mutex);
        auto it = d->entries.constFind(folderPath);
        if (it == d->entries.cend() || !it->snapshot || it->snapshot->generation < it->wanted)
            return false; // nothing current to compare (the next get() enumerates anyway)
        snap = it->snapshot;
    }
    const qint64 stamp = folderStamp(folderPath);
    return stamp < 0 || stamp != snap->folderStamp;
}

FolderSnapshotPtr FolderCache::get(const QString& folderPath, FileAttributeProvider* attributes,
                                   bool revalidate) {
    QMutexLocker lock(&d->mutex);
    ++d->entries[folderPath].waiters;
    FolderSnapshotPtr result;
    bool checked = !revalidate;
    for (;;) {
        Private::Entry& e = d->entries[folderPath]; // re-looked up: the table may rehash while waiting
        if (e.snapshot && e.snapshot->generation >= e.wanted) {
            if (!checked) {
                // B12: compare the folder's modification time (outside the lock: a stat
                // on a network volume may take a moment).
                checked = true;
                const FolderSnapshotPtr snap = e.snapshot;
                lock.unlock();
                const qint64 stamp = folderStamp(folderPath);
                lock.relock();
                Private::Entry& again = d->entries[folderPath];
                if (again.snapshot == snap && (stamp < 0 || stamp != snap->folderStamp)) {
                    ++again.wanted; // changed without a watcher event: enumerate again
                    continue;
                }
                continue; // re-evaluate (another thread may have replaced the snapshot)
            }
            result = e.snapshot;
            break;
        }
        if (e.inFlight) {
            d->changed.wait(&d->mutex); // share the running enumeration (or wait for it to end)
            continue;
        }
        e.inFlight = true;
        const quint64 generation = e.wanted;
        lock.unlock();

        auto snap = std::make_shared<FolderSnapshot>();
        snap->generation = generation;
        snap->changeSeq = localChangeSeq();          // before listing: an edit while listing counts as newer
        snap->folderStamp = folderStamp(folderPath); // before: a change while enumerating is seen next time
        snap->entries = collection::enumerateFolder(folderPath, &snap->accessDenied, &snap->privacyDenied,
                                                    &snap->subfolders);
        if (attributes) {
            for (FileEntry& entry : snap->entries)
                attributes->fill(entry);
        }

        lock.relock();
        Private::Entry& done = d->entries[folderPath];
        done.inFlight = false;
        done.snapshot = snap;
        ++d->total;
        if (d->counts.size() >= kMaxCountEntries && !d->counts.contains(folderPath))
            d->counts.clear();
        ++d->counts[folderPath];
        d->changed.wakeAll();
        // A change during the enumeration: this caller gets the result anyway (its
        // collection is refreshed again by that change); later callers enumerate anew.
        result = snap;
        break;
    }
    --d->entries[folderPath].waiters;
    d->dropIfUnused(folderPath);
    return result;
}

int FolderCache::enumerationCount(const QString& folderPath) const {
    QMutexLocker lock(&d->mutex);
    return d->counts.value(folderPath);
}

int FolderCache::totalEnumerations() const {
    QMutexLocker lock(&d->mutex);
    return d->total;
}

int FolderCache::leaseCount(const QString& folderPath) const {
    QMutexLocker lock(&d->mutex);
    return d->entries.value(folderPath).leases;
}

} // namespace lf
