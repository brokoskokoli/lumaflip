// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Backup copies for undoing in-place file edits (F-EDIT-02, saved rotation).
//
// Every edit gets an entry (create()) holding its copies ("before", "after" …) in one
// session folder "<root>/Lumaflip-undo-<random>" that a QLockFile marks as in use. The
// copies are APFS clones where possible (same volume: no extra space until the original
// changes) and plain copies otherwise.
//
// Budget: the copies of all entries together stay below budgetBytes() = min(maxBytes,
// freeFraction × free space of the root volume). A clone of a file that is not replaced
// costs nothing; every other copy costs its size (a clone of a file that is replaced
// right after - the "before" copy - ends up owning the blocks, so it counts fully). To
// make room, the oldest entries are dropped (their files deleted, isDropped() true): those
// undo steps are no longer available and say so when they are tried.
//
// Files of released or dropped entries are deleted on a worker thread. Session folders of
// crashed instances (lock not held by a running process) are removed by
// removeStaleSessions(), which runs once at application start (worker thread).
//
// Thread-safe.

#include <QHash>
#include <QList>
#include <QString>

#include <memory>
#include <mutex>

class QLockFile;
class QThreadPool;

namespace lf {

class UndoBackupStore {
public:
    struct Options {
        QString root;                                 // default: QDir::tempPath()
        qint64 maxBytes = qint64(1024) * 1024 * 1024; // 1 GiB
        double freeFraction = 0.10;                   // of the free space on the root volume
    };

    explicit UndoBackupStore(Options options);
    ~UndoBackupStore(); // deletes the session folder (synchronously)
    UndoBackupStore(const UndoBackupStore&) = delete;
    UndoBackupStore& operator=(const UndoBackupStore&) = delete;

    // The app-wide store (in QDir::tempPath()).
    static std::shared_ptr<UndoBackupStore> shared();

    // New, empty entry; 0 if the session folder cannot be created.
    quint64 create();
    // Copies `source` as `name` into entry `id` (replacing an older copy of that name).
    // Drops older entries if the budget needs it; fails (German message) if the copy alone
    // does not fit or cannot be written. `costsFullSize`: count the copy at its full size
    // even if it is a clone (the source is about to be replaced).
    bool addCopy(quint64 id, const QString& source, const QString& name, bool costsFullSize, QString* error);
    // Path of the copy `name` of entry `id`; empty if there is none or the entry was dropped.
    QString file(quint64 id, const QString& name) const;
    bool isDropped(quint64 id) const;
    // The entry is not needed any more: its files are deleted on a worker thread.
    void release(quint64 id);

    qint64 usedBytes() const;
    qint64 budgetBytes() const;
    QString sessionPath() const;
    // Waits until all deletions started so far are done (tests, shutdown).
    void waitForDeletions();

    // Removes "<root>/Lumaflip-undo-*" folders of other, no longer running instances
    // (stale lock) and lock-less ones older than a day. Returns the number removed.
    static int removeStaleSessions(const QString& root);
    static QString folderPrefix() { return QStringLiteral("Lumaflip-undo-"); }

private:
    struct Entry {
        QHash<QString, QString> files; // name -> path
        QHash<QString, qint64> costs;  // name -> counted bytes
        bool dropped = false;
    };
    bool ensureSessionLocked();
    qint64 budgetLocked() const;
    QStringList dropLocked(quint64 id); // the files to delete
    void deleteLater(const QStringList& paths);

    Options m_options;
    mutable std::mutex m_mutex;
    QString m_session;
    std::unique_ptr<QLockFile> m_lock;
    QHash<quint64, Entry> m_entries;
    QList<quint64> m_order; // creation order (oldest first)
    quint64 m_next = 1;
    qint64 m_used = 0;
    std::unique_ptr<QThreadPool> m_deleter; // one thread: deletions in order
};

} // namespace lf
