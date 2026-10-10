// SPDX-License-Identifier: GPL-3.0-or-later
//
// Folder watching on Windows via ReadDirectoryChangesW (F-OPEN-07, F-OPEN-08).
//
// One stream (directory handle + reader thread) per watched folder and kind (plain or
// tree), shared by all FolderWatcher instances of the process (F-WIN-09: a folder shown in
// several windows is watched once). The reader thread waits for change records and hands
// the classified result to the GUI thread; while a notification is queued, further events
// are absorbed (coalescing). Tree streams watch the whole subtree (bWatchSubtree) and report
// each changed folder; a buffer overflow (the system dropped events) is rescanNeeded(root).
//
// Like on macOS (code review M3 #16), events that cannot change the image list do not
// count as a folder change: names starting with "." (the app's own temporary files while
// saving; .DS_Store still counts) and XMP sidecars, which are reported on their own
// (sidecarChanged).

#include "core/Platform.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <windows.h>

namespace lf {

namespace {

class WinFolderWatcher;

struct Subscriber {
    QPointer<WinFolderWatcher> watcher;
    const WinFolderWatcher* id = nullptr;
    QString folder; // as passed to watch()
    bool tree = false;
};

class Hub;

struct Stream {
    QString key; // cleaned absolute path (forward slashes)
    bool tree = false;
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE stop = nullptr;
    std::thread thread;
    std::atomic_bool pending{false};
    Hub* hub = nullptr;
    QMutex mutex;
    QSet<QString> changed;  // tree: changed folders relative to key ("" = the folder itself)
    QSet<QString> sidecars; // relative to key
    bool dirty = false;     // plain: the folder changed
    bool rescan = false;    // tree: events were dropped
};

enum class EventKind { Change, Sidecar, Ignore };

EventKind classify(const QString& name) {
    if (name.startsWith(u'.'))
        return name == QLatin1String(".DS_Store") ? EventKind::Change : EventKind::Ignore;
    if (name.endsWith(QLatin1String(".xmp"), Qt::CaseInsensitive))
        return EventKind::Sidecar;
    return EventKind::Change;
}

void readLoop(Stream* s);

class Hub : public QObject {
public:
    static Hub& get() {
        static Hub* hub = new Hub; // leaked on purpose: no callback may outlive it
        return *hub;
    }
    void subscribe(WinFolderWatcher* w, const QString& folder, bool tree);
    void unsubscribe(WinFolderWatcher* w, const QString& folder, bool tree);

    void notify(Stream* s) {
        if (s->pending.exchange(true))
            return;
        const QString key = entryKey(s->key, s->tree);
        QMetaObject::invokeMethod(this, [this, key] { deliver(key); }, Qt::QueuedConnection);
    }

private:
    static QString entryKey(const QString& key, bool tree) {
        return tree ? key + QStringLiteral("\x1f*") : key;
    }
    void deliver(const QString& key);
    static bool open(Stream& s);
    static void close(Stream& s);

    struct Entry {
        std::shared_ptr<Stream> stream;
        QList<Subscriber> subscribers;
    };
    QHash<QString, Entry> m_streams;
};

class WinFolderWatcher final : public FolderWatcher {
public:
    using FolderWatcher::FolderWatcher;
    ~WinFolderWatcher() override {
        for (auto it = m_refs.cbegin(); it != m_refs.cend(); ++it)
            Hub::get().unsubscribe(this, it.key(), false);
        for (auto it = m_treeRefs.cbegin(); it != m_treeRefs.cend(); ++it)
            Hub::get().unsubscribe(this, it.key(), true);
    }
    void watch(const QString& folderPath) override {
        if (m_refs[folderPath]++ == 0)
            Hub::get().subscribe(this, folderPath, false);
    }
    void unwatch(const QString& folderPath) override {
        auto it = m_refs.find(folderPath);
        if (it == m_refs.end())
            return;
        if (--*it == 0) {
            m_refs.erase(it);
            Hub::get().unsubscribe(this, folderPath, false);
        }
    }
    void watchTree(const QString& root) override {
        if (m_treeRefs[root]++ == 0)
            Hub::get().subscribe(this, root, true);
    }
    void unwatchTree(const QString& root) override {
        auto it = m_treeRefs.find(root);
        if (it == m_treeRefs.end())
            return;
        if (--*it == 0) {
            m_treeRefs.erase(it);
            Hub::get().unsubscribe(this, root, true);
        }
    }
    void notifyChanged(const QString& folder) { emit folderChanged(folder); }
    void notifySidecar(const QString& path) { emit sidecarChanged(path); }
    void notifyRescan(const QString& root) { emit rescanNeeded(root); }

private:
    QHash<QString, int> m_refs;
    QHash<QString, int> m_treeRefs;
};

QString normalizedFolder(const QString& folder) {
    const QString canonical = QFileInfo(folder).canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? QFileInfo(folder).absoluteFilePath() : canonical);
}

bool Hub::open(Stream& s) {
    const QString native = QDir::toNativeSeparators(s.key);
    s.dir = CreateFileW(reinterpret_cast<const wchar_t*>(native.utf16()), FILE_LIST_DIRECTORY,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (s.dir == INVALID_HANDLE_VALUE)
        return false;
    s.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.stop) {
        CloseHandle(s.dir);
        s.dir = INVALID_HANDLE_VALUE;
        return false;
    }
    s.thread = std::thread(readLoop, &s);
    return true;
}

void Hub::close(Stream& s) {
    if (s.stop)
        SetEvent(s.stop);
    if (s.thread.joinable())
        s.thread.join();
    if (s.dir != INVALID_HANDLE_VALUE)
        CloseHandle(s.dir);
    if (s.stop)
        CloseHandle(s.stop);
    s.dir = INVALID_HANDLE_VALUE;
    s.stop = nullptr;
}

void readLoop(Stream* s) {
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                              FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_ATTRIBUTES;
    // 64 KiB: the limit for network shares; DWORD aligned.
    std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return;
    for (;;) {
        ResetEvent(ov.hEvent);
        if (!ReadDirectoryChangesW(s->dir, buffer.data(), DWORD(buffer.size() * sizeof(DWORD)), s->tree ? TRUE : FALSE,
                                   kFilter, nullptr, &ov, nullptr)) {
            // The folder is gone (deleted, renamed, volume removed): one last notification.
            {
                QMutexLocker lock(&s->mutex);
                if (s->tree)
                    s->rescan = true;
                else
                    s->dirty = true;
            }
            s->hub->notify(s);
            WaitForSingleObject(s->stop, INFINITE);
            break;
        }
        HANDLE handles[2] = {ov.hEvent, s->stop};
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
            CancelIoEx(s->dir, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(s->dir, &ov, &ignored, TRUE);
            break;
        }
        DWORD bytes = 0;
        const bool ok = GetOverlappedResult(s->dir, &ov, &bytes, FALSE);
        bool any = false;
        {
            QMutexLocker lock(&s->mutex);
            if (!ok || bytes == 0) {
                // Overflow (or an error): events were dropped.
                if (s->tree)
                    s->rescan = true;
                else
                    s->dirty = true;
                any = true;
            } else {
                const auto* base = reinterpret_cast<const char*>(buffer.data());
                for (DWORD offset = 0;;) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
                    const QString rel = QDir::fromNativeSeparators(
                        QString::fromWCharArray(info->FileName, int(info->FileNameLength / sizeof(WCHAR))));
                    const qsizetype slash = rel.lastIndexOf(u'/');
                    const QString parent = slash < 0 ? QString() : rel.left(slash);
                    const QString name = rel.mid(slash + 1);
                    switch (classify(name)) {
                    case EventKind::Ignore:
                        break;
                    case EventKind::Sidecar:
                        s->sidecars.insert(rel);
                        any = true;
                        break;
                    case EventKind::Change:
                        if (s->tree) {
                            s->changed.insert(parent);
                            // A folder created or renamed below: its contents are new as well.
                            if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                                const QString full = QDir::toNativeSeparators(s->key + u'/' + rel);
                                const DWORD attrs =
                                    GetFileAttributesW(reinterpret_cast<const wchar_t*>(full.utf16()));
                                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                                    s->changed.insert(rel);
                            }
                        } else {
                            s->dirty = true;
                        }
                        any = true;
                        break;
                    }
                    if (!info->NextEntryOffset)
                        break;
                    offset += info->NextEntryOffset;
                }
            }
        }
        if (any)
            s->hub->notify(s);
    }
    CloseHandle(ov.hEvent);
}

void Hub::subscribe(WinFolderWatcher* w, const QString& folder, bool tree) {
    const QString key = normalizedFolder(folder);
    Entry& e = m_streams[entryKey(key, tree)];
    e.subscribers.append({w, w, folder, tree});
    if (!e.stream) {
        e.stream = std::make_shared<Stream>();
        e.stream->key = key;
        e.stream->tree = tree;
        e.stream->hub = this;
        if (!open(*e.stream))
            qWarning("lf::FolderWatcher: cannot watch %s", qPrintable(folder));
    }
}

void Hub::unsubscribe(WinFolderWatcher* w, const QString& folder, bool tree) {
    auto it = m_streams.begin();
    for (; it != m_streams.end(); ++it) {
        const auto& subs = it->subscribers;
        if (std::any_of(subs.cbegin(), subs.cend(), [&](const Subscriber& s) {
                return s.id == w && s.folder == folder && s.tree == tree;
            }))
            break;
    }
    if (it == m_streams.end())
        return;
    it->subscribers.removeIf(
        [&](const Subscriber& s) { return s.id == w && s.folder == folder && s.tree == tree; });
    it->subscribers.removeIf([](const Subscriber& s) { return s.watcher.isNull(); });
    if (it->subscribers.isEmpty()) {
        close(*it->stream);
        m_streams.erase(it);
    }
}

void Hub::deliver(const QString& key) {
    auto it = m_streams.find(key);
    if (it == m_streams.end())
        return;
    it->stream->pending.store(false);
    const QList<Subscriber> subs = it->subscribers; // slots may (un)watch
    QSet<QString> changed, sidecars;
    bool dirty = false, rescan = false;
    {
        QMutexLocker lock(&it->stream->mutex);
        changed.swap(it->stream->changed);
        sidecars.swap(it->stream->sidecars);
        dirty = std::exchange(it->stream->dirty, false);
        rescan = std::exchange(it->stream->rescan, false);
    }
    QStringList rels(changed.cbegin(), changed.cend());
    rels.sort(); // a folder before its subfolders
    const QStringList files(sidecars.cbegin(), sidecars.cend());
    for (const Subscriber& s : subs) {
        if (!s.watcher)
            continue;
        if (rescan)
            s.watcher->notifyRescan(s.folder);
        else if (dirty)
            s.watcher->notifyChanged(s.folder);
        for (const QString& rel : std::as_const(rels))
            if (s.watcher && !rescan)
                s.watcher->notifyChanged(rel.isEmpty() ? s.folder : QDir(s.folder).filePath(rel));
        for (const QString& file : files)
            if (s.watcher)
                s.watcher->notifySidecar(QDir(s.folder).filePath(file));
    }
}

} // namespace

std::unique_ptr<FolderWatcher> createFolderWatcher(QObject* parent) {
    return std::make_unique<WinFolderWatcher>(parent);
}

} // namespace lf
