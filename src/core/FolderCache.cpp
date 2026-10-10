// SPDX-License-Identifier: GPL-3.0-or-later
#include "FolderCache.h"

#include "Collection.h"
#include "Platform.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>

namespace lf {

namespace {
constexpr int kMaxCountEntries = 4096; // bound of the instrumentation table
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

FolderSnapshotPtr FolderCache::get(const QString& folderPath, FileAttributeProvider* attributes) {
    QMutexLocker lock(&d->mutex);
    ++d->entries[folderPath].waiters;
    FolderSnapshotPtr result;
    for (;;) {
        Private::Entry& e = d->entries[folderPath]; // re-looked up: the table may rehash while waiting
        if (e.snapshot && e.snapshot->generation >= e.wanted) {
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
        snap->entries = collection::enumerateFolder(folderPath, &snap->accessDenied, &snap->privacyDenied);
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
