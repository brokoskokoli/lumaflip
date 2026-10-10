// SPDX-License-Identifier: GPL-3.0-or-later
// Folder watching on Linux with inotify (F-OPEN-07, F-OPEN-08; Platform.h FolderWatcher).
// Replaces src/platform/generic/GenericFolderWatcher.cpp in Linux builds.
//
// - watch(folder): one inotify watch on the folder (reference counted).
// - watchTree(root): a watch on every folder below `root` (symlinks are not followed);
//   folders created or moved in later get their watches when the event arrives.
// - Events are classified like on macOS (MacFolderWatcher.mm): "*.xmp" files are reported
//   as sidecarChanged(path), other hidden files are ignored, everything else counts as a
//   change of its folder. Changes are coalesced per folder for a few milliseconds.
// - IN_Q_OVERFLOW (the kernel dropped events): rescanNeeded(root) for every tree,
//   folderChanged() for every plain folder.
// - When the per-user watch limit (fs.inotify.max_user_watches) is reached, deeper folders
//   of a tree stay unwatched (logged once); they are still read at every rescan.

#include "core/Platform.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/inotify.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcWatch, "lf.watch")

namespace lf {

namespace {

constexpr int kCoalesceMs = 30;
constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB |
                           IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_EXCL_UNLINK;

QString clean(const QString& path) {
    return QDir::cleanPath(path);
}

bool isBelow(const QString& path, const QString& root) {
    return path == root || path.startsWith(root.endsWith(u'/') ? root : root + u'/');
}

class LinuxFolderWatcher final : public FolderWatcher {
public:
    explicit LinuxFolderWatcher(QObject* parent) : FolderWatcher(parent) {
        m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_fd < 0) {
            qCWarning(lcWatch) << "inotify_init1 failed:" << strerror(errno);
            return;
        }
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { readEvents(); });
        m_flush.setSingleShot(true);
        m_flush.setInterval(kCoalesceMs);
        connect(&m_flush, &QTimer::timeout, this, [this] { flush(); });
    }

    ~LinuxFolderWatcher() override {
        if (m_fd >= 0)
            close(m_fd);
    }

    void watch(const QString& folderPath) override {
        const QString p = clean(folderPath);
        if (m_plainRefs[p]++ == 0)
            addWatch(p);
    }

    void unwatch(const QString& folderPath) override {
        const QString p = clean(folderPath);
        auto it = m_plainRefs.find(p);
        if (it == m_plainRefs.end())
            return;
        if (--*it > 0)
            return;
        m_plainRefs.erase(it);
        if (!coveredByTree(p))
            removeWatch(p);
    }

    void watchTree(const QString& root) override {
        const QString r = clean(root);
        if (m_treeRefs[r]++ > 0)
            return;
        addTree(r);
    }

    void unwatchTree(const QString& root) override {
        const QString r = clean(root);
        auto it = m_treeRefs.find(r);
        if (it == m_treeRefs.end())
            return;
        if (--*it > 0)
            return;
        m_treeRefs.erase(it);
        const QStringList watched = m_wdByPath.keys();
        for (const QString& p : watched)
            if (isBelow(p, r) && !m_plainRefs.contains(p) && !coveredByTree(p))
                removeWatch(p);
    }

private:
    bool coveredByTree(const QString& p) const {
        for (auto it = m_treeRefs.cbegin(); it != m_treeRefs.cend(); ++it)
            if (isBelow(p, it.key()))
                return true;
        return false;
    }

    // The tree root a folder belongs to (the deepest one), empty if none.
    QString treeOf(const QString& p) const {
        QString best;
        for (auto it = m_treeRefs.cbegin(); it != m_treeRefs.cend(); ++it)
            if (isBelow(p, it.key()) && it.key().size() > best.size())
                best = it.key();
        return best;
    }

    bool addWatch(const QString& p) {
        if (m_fd < 0 || m_wdByPath.contains(p))
            return m_fd >= 0;
        const int wd = inotify_add_watch(m_fd, QFile::encodeName(p).constData(), kMask);
        if (wd < 0) {
            if (errno == ENOSPC && !m_warnedLimit) {
                m_warnedLimit = true;
                qCWarning(lcWatch) << "inotify watch limit reached (fs.inotify.max_user_watches):" << p
                                   << "and further folders are not watched";
            } else if (errno != ENOENT && errno != ENOTDIR) {
                qCInfo(lcWatch) << "inotify_add_watch" << p << strerror(errno);
            }
            return false;
        }
        m_pathByWd.insert(wd, p);
        m_wdByPath.insert(p, wd);
        return true;
    }

    void removeWatch(const QString& p) {
        const auto it = m_wdByPath.find(p);
        if (it == m_wdByPath.end())
            return;
        inotify_rm_watch(m_fd, *it);
        m_pathByWd.remove(*it);
        m_wdByPath.erase(it);
    }

    void addTree(const QString& root) {
        if (!addWatch(root))
            return;
        QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::NoSymLinks,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString d = clean(it.next());
            if (QFileInfo(d).fileName().startsWith(u'.'))
                continue; // hidden folders are not part of the collection either
            addWatch(d);
        }
    }

    void forgetBelow(const QString& p) {
        const QStringList watched = m_wdByPath.keys();
        for (const QString& w : watched)
            if (isBelow(w, p)) {
                m_pathByWd.remove(m_wdByPath.value(w));
                m_wdByPath.remove(w);
            }
    }

    void readEvents() {
        alignas(inotify_event) char buf[16384];
        for (;;) {
            const ssize_t n = ::read(m_fd, buf, sizeof buf);
            if (n <= 0)
                break;
            for (ssize_t off = 0; off < n;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
                handle(*ev);
                off += ssize_t(sizeof(inotify_event) + ev->len);
            }
        }
        if (!m_pendingFolders.isEmpty() || !m_pendingSidecars.isEmpty() || !m_pendingRescans.isEmpty())
            m_flush.start();
    }

    void handle(const inotify_event& ev) {
        if (ev.mask & IN_Q_OVERFLOW) {
            for (auto it = m_treeRefs.cbegin(); it != m_treeRefs.cend(); ++it)
                m_pendingRescans.insert(it.key());
            for (auto it = m_plainRefs.cbegin(); it != m_plainRefs.cend(); ++it)
                m_pendingFolders.insert(it.key());
            return;
        }
        const auto wit = m_pathByWd.constFind(ev.wd);
        if (wit == m_pathByWd.cend())
            return;
        const QString dir = *wit;
        if (ev.mask & IN_IGNORED) {
            m_wdByPath.remove(dir);
            m_pathByWd.remove(ev.wd);
            return;
        }
        if (ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            m_pendingFolders.insert(dir);
            return;
        }
        const QString name = ev.len > 0 ? QFile::decodeName(ev.name) : QString();
        if (name.isEmpty()) {
            m_pendingFolders.insert(dir);
            return;
        }
        const bool isDir = ev.mask & IN_ISDIR;
        const QString child = dir + u'/' + name;
        if (isDir) {
            if (name.startsWith(u'.'))
                return;
            const QString tree = treeOf(dir);
            if (!tree.isEmpty()) {
                if (ev.mask & (IN_CREATE | IN_MOVED_TO)) {
                    addTree(child);
                    m_pendingFolders.insert(child); // files may already be in it
                } else if (ev.mask & (IN_DELETE | IN_MOVED_FROM)) {
                    forgetBelow(child);
                }
            }
            m_pendingFolders.insert(dir);
            return;
        }
        if (name.startsWith(u'.')) {
            if (name == QLatin1String(".DS_Store"))
                m_pendingFolders.insert(dir);
            return;
        }
        if (name.size() > 4 && name.right(4).compare(QLatin1String(".xmp"), Qt::CaseInsensitive) == 0) {
            m_pendingSidecars.insert(child);
            return;
        }
        m_pendingFolders.insert(dir);
    }

    void flush() {
        const QSet<QString> rescans = std::exchange(m_pendingRescans, {});
        const QSet<QString> folders = std::exchange(m_pendingFolders, {});
        const QSet<QString> sidecars = std::exchange(m_pendingSidecars, {});
        for (const QString& r : rescans)
            emit rescanNeeded(r);
        for (const QString& f : folders)
            emit folderChanged(f);
        for (const QString& s : sidecars)
            emit sidecarChanged(s);
    }

    int m_fd = -1;
    QSocketNotifier* m_notifier = nullptr;
    QTimer m_flush;
    bool m_warnedLimit = false;
    QHash<int, QString> m_pathByWd;
    QHash<QString, int> m_wdByPath;
    QHash<QString, int> m_plainRefs;
    QHash<QString, int> m_treeRefs;
    QSet<QString> m_pendingFolders, m_pendingSidecars, m_pendingRescans;
};

} // namespace

std::unique_ptr<FolderWatcher> createFolderWatcher(QObject* parent) {
    return std::make_unique<LinuxFolderWatcher>(parent);
}

} // namespace lf
