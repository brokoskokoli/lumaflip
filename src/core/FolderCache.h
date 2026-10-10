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
#include <QVector>
#include <memory>

namespace lf {

class FileAttributeProvider;

struct FolderSnapshot {
    QVector<FileEntry> entries; // unsorted, FileAttributeProvider::fill() applied
    bool accessDenied = false;  // folder exists but cannot be listed (F-PRIV-01)
    bool privacyDenied = false; // ... because of macOS privacy protection (TCC, EPERM)
    quint64 generation = 0;
};
using FolderSnapshotPtr = std::shared_ptr<const FolderSnapshot>;

class FolderCache {
public:
    static FolderCache& instance();

    void acquire(const QString& folderPath);
    void release(const QString& folderPath);

    // Current snapshot of `folderPath` (worker threads; blocks while another thread
    // enumerates the same folder). `attributes` (may be null) fills the entries.
    FolderSnapshotPtr get(const QString& folderPath, FileAttributeProvider* attributes);
    // The folder changed: the next get() enumerates again.
    void invalidate(const QString& folderPath);

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
