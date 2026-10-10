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
//
// F-OPEN-08: a tree subscription (watchTree) uses one stream for a folder and everything
// below it; the changed folders are collected and each one is reported.

#include "MacFolderWatcher.h"
#include "core/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <algorithm>
#include <atomic>
#include <memory>
#include <sys/stat.h>

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
    const MacFolderWatcher* id = nullptr; // identity even while the watcher is being destroyed
    QString folder;                       // as passed to watch()
    bool tree = false;                    // watchTree()
};

class Hub;

struct Stream {
    QString key; // normalized folder path (NFC, as Qt reports it)
    QByteArray keyUtf8;
    QString keyNfcFolded; // NFC + case folded, for volumes that report other spellings
    dev_t dev = 0;        // file system identity of the folder at subscribe time
    ino_t ino = 0;
    bool hasIdentity = false;
    FSEventStreamRef ref = nullptr;
    std::atomic_bool pending{false}; // a notification is queued for the GUI thread
    Hub* hub = nullptr;
    bool tree = false; // F-OPEN-08: the folder and all folders below it
    QMutex changedMutex;
    QSet<QString> changed; // tree: changed folders relative to key ("" = the folder itself)
};

// GUI-thread object owning all streams. Lives forever (leaked on purpose so no
// callback can outlive it during shutdown).
class Hub : public QObject {
public:
    static Hub& get() {
        static Hub* hub = new Hub;
        return *hub;
    }

    void subscribe(MacFolderWatcher* w, const QString& folder, bool tree = false);
    void unsubscribe(MacFolderWatcher* w, const QString& folder, bool tree = false);
    int streamCount() const { return int(m_streams.size()); }

    // Called on the dispatch queue.
    void notify(Stream* s) {
        if (s->pending.exchange(true))
            return; // coalesce
        const QString key = s->tree ? treeKey(s->key) : s->key;
        QMetaObject::invokeMethod(this, [this, key] { deliver(key); }, Qt::QueuedConnection);
    }
    // Tree streams live next to plain ones in m_streams under this key.
    static QString treeKey(const QString& key) { return key + QStringLiteral("\x1f*"); }

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
        for (auto it = m_treeRefs.cbegin(); it != m_treeRefs.cend(); ++it)
            Hub::get().unsubscribe(this, it.key(), true);
    }
    void watch(const QString& folderPath) override {
        if (m_refs[folderPath]++ == 0)
            Hub::get().subscribe(this, folderPath);
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
    QHash<QString, int> m_treeRefs;
};

// FSEvents reports the on-disk bytes of a path. Names created by Finder are usually
// decomposed (NFD: "Mu\u0308ll"), while Qt hands out NFC, and case-insensitive volumes may
// report another case. `parent` matches the watched folder if it is byte-equal, NFC-equal, or
// case-insensitively equal and the same file system object.
bool sameFolder(const Stream& s, const QByteArray& parent) {
    if (parent == s.keyUtf8)
        return true;
    const QString nfc = QString::fromUtf8(parent).normalized(QString::NormalizationForm_C);
    if (nfc == s.key)
        return true;
    if (!s.hasIdentity || nfc.toCaseFolded() != s.keyNfcFolded)
        return false;
    struct stat st{};
    return ::stat(parent.constData(), &st) == 0 && st.st_dev == s.dev && st.st_ino == s.ino;
}

// F-OPEN-08: the folder `parent` relative to the tree root (nullopt: outside the tree).
std::optional<QString> treeRelative(const Stream& s, const QByteArray& parent) {
    if (sameFolder(s, parent))
        return QString();
    const QString nfc = QString::fromUtf8(parent).normalized(QString::NormalizationForm_C);
    const QString prefix = s.key.endsWith(u'/') ? s.key : s.key + u'/';
    if (nfc.startsWith(prefix))
        return nfc.mid(prefix.size());
    if (s.hasIdentity && nfc.toCaseFolded().startsWith(prefix.toCaseFolded()))
        return nfc.mid(prefix.size()); // case-insensitive volume reporting another case
    return std::nullopt;
}

void treeCallback(Stream* s, size_t numEvents, char** paths, const FSEventStreamEventFlags flags[]) {
    bool any = false;
    {
        QMutexLocker lock(&s->changedMutex);
        for (size_t i = 0; i < numEvents; ++i) {
            const FSEventStreamEventFlags f = flags[i];
            if (f & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMustScanSubDirs |
                     kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagUserDropped)) {
                s->changed.insert(QString()); // the folder itself (collections revalidate the tree)
                any = true;
                continue;
            }
            QByteArray p(paths[i]);
            while (p.size() > 1 && p.endsWith('/'))
                p.chop(1);
            const qsizetype slash = p.lastIndexOf('/');
            if (slash < 0)
                continue;
            if (const auto rel = treeRelative(*s, slash == 0 ? QByteArray("/") : p.left(slash))) {
                s->changed.insert(*rel);
                any = true;
            }
        }
    }
    if (any)
        s->hub->notify(s);
}

void streamCallback(ConstFSEventStreamRef, void* info, size_t numEvents, void* eventPaths,
                    const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
    auto* s = static_cast<Stream*>(info);
    auto** paths = static_cast<char**>(eventPaths);
    if (s->tree) {
        treeCallback(s, numEvents, paths, flags);
        return;
    }
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
        if (sameFolder(*s, parent)) {
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
    dispatch_sync(m_queue, ^{
                  });
}

void Hub::subscribe(MacFolderWatcher* w, const QString& folder, bool tree) {
    const QString key = normalizedFolder(folder).normalized(QString::NormalizationForm_C);
    Entry& e = m_streams[tree ? treeKey(key) : key];
    e.subscribers.append({w, w, folder, tree});
    if (!e.stream) {
        e.stream = std::make_shared<Stream>();
        e.stream->tree = tree;
        e.stream->key = key;
        e.stream->keyUtf8 = key.toUtf8();
        e.stream->keyNfcFolded = key.toCaseFolded();
        struct stat st{};
        if (::stat(QFile::encodeName(key).constData(), &st) == 0) {
            e.stream->dev = st.st_dev;
            e.stream->ino = st.st_ino;
            e.stream->hasIdentity = true;
        }
        e.stream->hub = this;
        if (!open(*e.stream))
            qWarning("lf::FolderWatcher: cannot watch %s", qPrintable(folder));
    }
}

void Hub::unsubscribe(MacFolderWatcher* w, const QString& folder, bool tree) {
    // Find the stream by the subscription itself, not by recomputing the canonical path: after
    // the folder was deleted or a symlink changed, it no longer resolves to the same key.
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
    if (it->stream->tree) {
        QSet<QString> changed;
        {
            QMutexLocker lock(&it->stream->changedMutex);
            changed.swap(it->stream->changed);
        }
        QStringList rels(changed.cbegin(), changed.cend());
        rels.sort(); // a folder before its subfolders
        for (const Subscriber& s : subs)
            for (const QString& rel : std::as_const(rels))
                if (s.watcher)
                    s.watcher->notifyChanged(rel.isEmpty() ? s.folder : QDir(s.folder).filePath(rel));
        return;
    }
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
