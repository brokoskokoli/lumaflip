// SPDX-License-Identifier: GPL-3.0-or-later
//
// Folder watching via FSEvents (F-OPEN-07).
//
// One FSEvents stream per watched folder for the whole process (F-WIN-09: a folder
// shown in several windows is watched once). All FolderWatcher instances subscribe
// to a shared hub that ref-counts streams. Stream callbacks run on a private serial
// dispatch queue; only events for items directly inside the folder (or for the
// folder itself being moved/deleted) count. Events are coalesced: while a
// notification is queued for the GUI thread, further events are absorbed.

#include "MacFolderWatcher.h"
#include "core/Platform.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QPointer>
#include <atomic>
#include <memory>

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace lf {

namespace {

class MacFolderWatcher;

constexpr CFTimeInterval kLatency = 0.2;

QString normalizedFolder(const QString& folder) {
    const QString canonical = QFileInfo(folder).canonicalFilePath(); // FSEvents reports resolved paths
    return QDir::cleanPath(canonical.isEmpty() ? QFileInfo(folder).absoluteFilePath() : canonical);
}

struct Subscriber {
    QPointer<MacFolderWatcher> watcher;
    QString folder; // as passed to watch()
};

class Hub;

struct Stream {
    QString key; // normalized folder path
    QByteArray keyUtf8;
    FSEventStreamRef ref = nullptr;
    std::atomic_bool pending{false}; // a notification is queued for the GUI thread
    Hub* hub = nullptr;
};

// GUI-thread object owning all streams. Lives forever (leaked on purpose so no
// callback can outlive it during shutdown).
class Hub : public QObject {
public:
    static Hub& get() {
        static Hub* hub = new Hub;
        return *hub;
    }

    void subscribe(MacFolderWatcher* w, const QString& folder);
    void unsubscribe(MacFolderWatcher* w, const QString& folder);
    int streamCount() const { return int(m_streams.size()); }

    // Called on the dispatch queue.
    void notify(Stream* s) {
        if (s->pending.exchange(true))
            return; // coalesce
        const QString key = s->key;
        QMetaObject::invokeMethod(this, [this, key] { deliver(key); }, Qt::QueuedConnection);
    }

private:
    Hub()
        : m_queue(dispatch_queue_create("io.github.brokoskokoli.lumaflip.fsevents", DISPATCH_QUEUE_SERIAL)) {}

    void deliver(const QString& key);
    bool open(Stream& s);
    void close(Stream& s);

    struct Entry {
        std::shared_ptr<Stream> stream;
        QList<Subscriber> subscribers;
    };
    QHash<QString, Entry> m_streams;
    dispatch_queue_t m_queue;
};

class MacFolderWatcher final : public FolderWatcher {
public:
    using FolderWatcher::FolderWatcher;
    ~MacFolderWatcher() override {
        for (auto it = m_refs.cbegin(); it != m_refs.cend(); ++it)
            Hub::get().unsubscribe(this, it.key());
    }
    void watch(const QString& folderPath) override {
        if (m_refs[folderPath]++ == 0)
            Hub::get().subscribe(this, folderPath);
    }
    void unwatch(const QString& folderPath) override {
        auto it = m_refs.find(folderPath);
        if (it == m_refs.end())
            return;
        if (--*it == 0) {
            m_refs.erase(it);
            Hub::get().unsubscribe(this, folderPath);
        }
    }
    void notifyChanged(const QString& folder) { emit folderChanged(folder); }

private:
    QHash<QString, int> m_refs;
};

void streamCallback(ConstFSEventStreamRef, void* info, size_t numEvents, void* eventPaths,
                    const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
    auto* s = static_cast<Stream*>(info);
    auto** paths = static_cast<char**>(eventPaths);
    const QByteArray& folder = s->keyUtf8;
    for (size_t i = 0; i < numEvents; ++i) {
        const FSEventStreamEventFlags f = flags[i];
        if (f & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMustScanSubDirs |
                 kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagUserDropped)) {
            s->hub->notify(s);
            return;
        }
        // Direct child?  "<folder>/<name>" without further '/'.
        QByteArray p(paths[i]);
        while (p.size() > 1 && p.endsWith('/'))
            p.chop(1);
        const qsizetype slash = p.lastIndexOf('/');
        if (slash < 0)
            continue;
        const QByteArray parent = slash == 0 ? QByteArray("/") : p.left(slash);
        if (parent == folder) {
            s->hub->notify(s);
            return;
        }
    }
}

bool Hub::open(Stream& s) {
    FSEventStreamContext ctx{0, &s, nullptr, nullptr, nullptr};
    CFStringRef cfPath = s.key.toCFString();
    CFArrayRef paths =
        CFArrayCreate(nullptr, reinterpret_cast<const void**>(&cfPath), 1, &kCFTypeArrayCallBacks);
    s.ref =
        FSEventStreamCreate(nullptr, &streamCallback, &ctx, paths, kFSEventStreamEventIdSinceNow, kLatency,
                            kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    CFRelease(cfPath);
    if (!s.ref)
        return false;
    FSEventStreamSetDispatchQueue(s.ref, m_queue);
    if (!FSEventStreamStart(s.ref)) {
        FSEventStreamInvalidate(s.ref);
        FSEventStreamRelease(s.ref);
        s.ref = nullptr;
        return false;
    }
    return true;
}

void Hub::close(Stream& s) {
    if (!s.ref)
        return;
    FSEventStreamStop(s.ref);
    FSEventStreamInvalidate(s.ref);
    FSEventStreamRelease(s.ref);
    s.ref = nullptr;
    // Wait for a callback that may still be running with this context.
    dispatch_sync(m_queue, ^{});
}

void Hub::subscribe(MacFolderWatcher* w, const QString& folder) {
    const QString key = normalizedFolder(folder);
    Entry& e = m_streams[key];
    e.subscribers.append({w, folder});
    if (!e.stream) {
        e.stream = std::make_shared<Stream>();
        e.stream->key = key;
        e.stream->keyUtf8 = key.toUtf8();
        e.stream->hub = this;
        if (!open(*e.stream))
            qWarning("lf::FolderWatcher: cannot watch %s", qPrintable(folder));
    }
}

void Hub::unsubscribe(MacFolderWatcher* w, const QString& folder) {
    const QString key = normalizedFolder(folder);
    auto it = m_streams.find(key);
    if (it == m_streams.end())
        return;
    it->subscribers.removeIf([&](const Subscriber& s) { return s.watcher == w && s.folder == folder; });
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
    for (const Subscriber& s : subs) {
        if (s.watcher)
            s.watcher->notifyChanged(s.folder);
    }
}

} // namespace

std::unique_ptr<FolderWatcher> createFolderWatcher(QObject* parent) {
    return std::make_unique<MacFolderWatcher>(parent);
}

int mac::activeFolderStreamCount() {
    return Hub::get().streamCount();
}

} // namespace lf
