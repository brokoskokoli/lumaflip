// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared folder snapshots (F-WIN-09, F-OPEN-07): every folder is enumerated (incl. the
// cheap file attributes) once per change, however many windows show it. Collections
// take a snapshot, copy the entries and sort them per window.
//
// - A Collection that shows a folder holds a lease on it (acquire/release, GUI thread).
//   Snapshots live only while a folder is leased, so a folder opened again after all
//   windows closed it is read afresh.
// - get() runs on worker threads. Concurrent calls for the same folder share one
//   enumeration: the first caller enumerates, the others wait for its result.
// - invalidate() (folder watcher, explicit refresh) makes the next get() enumerate again;
//   all collections refreshing after one change share that single enumeration.

#include "FileEntry.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace lf {

class FileAttributeProvider;

struct FolderSnapshot {
    QVector<FileEntry> entries; // unsorted, FileAttributeProvider::fill() applied
    QStringList subfolders;     // F-OPEN-08: names of visible subfolders (no packages, unsorted)
    bool accessDenied = false;  // folder exists but cannot be listed (F-PRIV-01)
    bool privacyDenied = false; // ... because of macOS privacy protection (TCC, EPERM)
    quint64 generation = 0;
    qint64 folderStamp = -1; // modification time of the folder (ns) before enumerating
    quint64 changeSeq = 0;   // FolderCache::localChangeSeq() when the enumeration started
};
using FolderSnapshotPtr = std::shared_ptr<const FolderSnapshot>;

class FolderCache {
public:
    static FolderCache& instance();

    void acquire(const QString& folderPath);
    void release(const QString& folderPath);

    // Current snapshot of `folderPath` (worker threads; blocks while another thread
    // enumerates the same folder). `attributes` (may be null) fills the entries.
    // `revalidate` (an explicit open): a cached snapshot is only taken if the folder's
    // modification time is unchanged - cheap, and correct also on volumes without change
    // notifications (network shares), where invalidate() never comes.
    FolderSnapshotPtr get(const QString& folderPath, FileAttributeProvider* attributes,
                          bool revalidate = false);
    // Modification time of a folder in ns (-1 if it cannot be read).
    static qint64 folderStamp(const QString& folderPath);
    // Worker threads: the current snapshot of `folderPath` no longer matches the folder's
    // modification time (false if there is no current snapshot).
    bool isStale(const QString& folderPath) const;
    // The folder changed: the next get() enumerates again.
    void invalidate(const QString& folderPath);
    // A file operation of this app changed `folderPath` (trash, undo, rename): like
    // invalidate(), and returns a new, larger localChangeSeq(). Snapshots enumerated
    // before carry a smaller FolderSnapshot::changeSeq, so a collection can tell that a
    // result predates its own edit (a folder watcher reload racing with the operation).
    quint64 noteLocalChange(const QString& folderPath);
    static quint64 localChangeSeq(); // any thread

    // Instrumentation for tests and benchmarks: number of enumerations so far.
    int enumerationCount(const QString& folderPath) const;
    int totalEnumerations() const;
    int leaseCount(const QString& folderPath) const;

    FolderCache();
    ~FolderCache();
    FolderCache(const FolderCache&) = delete;
    FolderCache& operator=(const FolderCache&) = delete;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace lf
