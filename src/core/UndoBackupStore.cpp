// SPDX-License-Identifier: GPL-3.0-or-later
// Undo backups with a byte budget (F-EDIT-02), see UndoBackupStore.h.

#include "UndoBackupStore.h"

#include "LosslessTransform.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStorageInfo>
#include <QThread>
#include <QThreadPool>
#include <QUuid>

#include <algorithm>

namespace lf {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::UndoBackupStore)
};

constexpr QLatin1StringView kLockName(".lock");

// Removes a session folder: only regular files directly inside it, then the folder (no
// recursion into anything a user might have put there by mistake).
bool removeSessionFolder(const QString& path) {
    QDir dir(path);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : entries)
        QFile::remove(fi.filePath());
    return QDir().rmdir(path);
}

// Stale cleanup at application start (worker thread, never in the way of the first frame).
void scheduleStaleCleanup() {
    if (qEnvironmentVariableIsSet("LF_NO_UNDO_CLEANUP"))
        return;
    // An own low-priority thread: never a slot of the global pool that decoders wait for.
    QThread* thread = QThread::create([] { UndoBackupStore::removeStaleSessions(QDir::tempPath()); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowestPriority);
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(scheduleStaleCleanup)

UndoBackupStore::UndoBackupStore(Options options)
    : m_options(std::move(options)), m_deleter(std::make_unique<QThreadPool>()) {
    if (m_options.root.isEmpty())
        m_options.root = QDir::tempPath();
    m_deleter->setMaxThreadCount(1);
}

UndoBackupStore::~UndoBackupStore() {
    m_deleter->waitForDone();
    if (!m_session.isEmpty()) {
        m_lock.reset(); // unlock (removes the lock file)
        removeSessionFolder(m_session);
    }
}

std::shared_ptr<UndoBackupStore> UndoBackupStore::shared() {
    static const std::shared_ptr<UndoBackupStore> store = std::make_shared<UndoBackupStore>(Options{});
    return store;
}

bool UndoBackupStore::ensureSessionLocked() {
    if (!m_session.isEmpty())
        return true;
    if (!QDir().mkpath(m_options.root))
        return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const QString path =
            QDir(m_options.root)
                .filePath(folderPrefix() + QUuid::createUuid().toString(QUuid::Id128).left(12));
        if (!QDir().mkdir(path)) // fails if it exists: never share a folder
            continue;
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        auto lock = std::make_unique<QLockFile>(QDir(path).filePath(QString(kLockName)));
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            QDir().rmdir(path);
            continue;
        }
        m_session = path;
        m_lock = std::move(lock);
        return true;
    }
    return false;
}

quint64 UndoBackupStore::create() {
    std::lock_guard g(m_mutex);
    if (!ensureSessionLocked())
        return 0;
    const quint64 id = m_next++;
    m_entries.insert(id, Entry{});
    m_order.append(id);
    return id;
}

qint64 UndoBackupStore::budgetLocked() const {
    const QStorageInfo volume(m_options.root);
    const qint64 free = volume.isValid() ? volume.bytesAvailable() : 0;
    // Our own copies are part of what is no longer free: the budget covers both.
    const auto share = qint64(double(std::max<qint64>(0, free) + m_used) * m_options.freeFraction);
    return std::max<qint64>(0, std::min(m_options.maxBytes, share));
}

qint64 UndoBackupStore::budgetBytes() const {
    std::lock_guard g(m_mutex);
    return budgetLocked();
}

QStringList UndoBackupStore::dropLocked(quint64 id) {
    auto it = m_entries.find(id);
    if (it == m_entries.end())
        return {};
    QStringList files;
    for (auto c = it->costs.cbegin(); c != it->costs.cend(); ++c)
        m_used -= c.value();
    for (const QString& f : std::as_const(it->files))
        files << f;
    it->files.clear();
    it->costs.clear();
    it->dropped = true;
    return files;
}

bool UndoBackupStore::addCopy(quint64 id, const QString& source, const QString& name, bool costsFullSize,
                              QString* error) {
    auto fail = [error](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    const qint64 size = QFileInfo(source).size();
    QString path;
    QStringList toDelete;
    {
        std::lock_guard g(m_mutex);
        auto it = m_entries.find(id);
        if (it == m_entries.end() || it->dropped || m_session.isEmpty())
            return fail(Tr::tr("Die Sicherungskopie für „Widerrufen“ existiert nicht mehr."));
        // The previous copy of that name goes first.
        if (it->files.contains(name)) {
            toDelete << it->files.take(name);
            m_used -= it->costs.take(name);
        }
        const qint64 budget = budgetLocked();
        if (size > budget) {
            deleteLater(toDelete);
            return fail(
                Tr::tr("Nicht genug Speicherplatz für die Sicherungskopie (Widerrufen) – nichts gesichert."));
        }
        // Room for the full size (a clone may cost nothing, which is only known afterwards).
        for (qsizetype i = 0; i < m_order.size() && m_used + size > budget;) {
            const quint64 old = m_order.at(i);
            if (old == id) {
                ++i;
                continue;
            }
            toDelete << dropLocked(old);
            m_entries.remove(old); // its RotationEdit sees a missing entry = dropped
            m_order.removeAt(i);
        }
        if (m_used + size > budget) {
            deleteLater(toDelete);
            return fail(
                Tr::tr("Nicht genug Speicherplatz für die Sicherungskopie (Widerrufen) – nichts gesichert."));
        }
        it = m_entries.find(id); // removing other entries may have moved this one
        path = QDir(m_session).filePath(QStringLiteral("%1-%2").arg(id).arg(name));
        m_used += size; // reserved while copying
        it->files.insert(name, path);
        it->costs.insert(name, size);
    }
    // Old files are deleted before copying: the space is needed now, so synchronously.
    for (const QString& f : std::as_const(toDelete))
        QFile::remove(f);
    QFile::remove(path);
    bool cloned = false;
    QString copyError;
    const bool ok = platform::copyWithMetadata(source, path, &copyError, &cloned);
    std::lock_guard g(m_mutex);
    auto it = m_entries.find(id);
    const bool stillThere = it != m_entries.end() && !it->dropped && it->files.value(name) == path;
    if (!ok || !stillThere) {
        if (stillThere) {
            it->files.remove(name);
            m_used -= it->costs.take(name);
        }
        QFile::remove(path);
        return fail(ok ? Tr::tr("Die Sicherungskopie für „Widerrufen“ existiert nicht mehr.") : copyError);
    }
    if (cloned && !costsFullSize) {
        m_used -= it->costs.value(name);
        it->costs.insert(name, 0);
    }
    return true;
}

QString UndoBackupStore::file(quint64 id, const QString& name) const {
    std::lock_guard g(m_mutex);
    const auto it = m_entries.constFind(id);
    if (it == m_entries.cend() || it->dropped)
        return {};
    return it->files.value(name);
}

bool UndoBackupStore::isDropped(quint64 id) const {
    std::lock_guard g(m_mutex);
    const auto it = m_entries.constFind(id);
    return it == m_entries.cend() || it->dropped;
}

void UndoBackupStore::release(quint64 id) {
    QStringList files;
    {
        std::lock_guard g(m_mutex);
        files = dropLocked(id);
        m_entries.remove(id);
        m_order.removeAll(id);
    }
    deleteLater(files);
}

void UndoBackupStore::deleteLater(const QStringList& paths) {
    if (paths.isEmpty())
        return;
    m_deleter->start([paths] {
        for (const QString& p : paths)
            QFile::remove(p);
    });
}

qint64 UndoBackupStore::usedBytes() const {
    std::lock_guard g(m_mutex);
    return m_used;
}

QString UndoBackupStore::sessionPath() const {
    std::lock_guard g(m_mutex);
    return m_session;
}

void UndoBackupStore::waitForDeletions() {
    m_deleter->waitForDone();
}

int UndoBackupStore::removeStaleSessions(const QString& root) {
    const QDir dir(root);
    int removed = 0;
    const QFileInfoList candidates = dir.entryInfoList({folderPrefix() + QLatin1Char('*')},
                                                       QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    const QDateTime dayAgo = QDateTime::currentDateTimeUtc().addDays(-1);
    for (const QFileInfo& fi : candidates) {
        if (fi.isSymLink() || !fi.isDir() || !fi.isWritable())
            continue; // not ours
        const QString lockPath = QDir(fi.filePath()).filePath(QString(kLockName));
        bool stale = false;
        if (QFileInfo::exists(lockPath)) {
            QLockFile lock(lockPath);
            // Only a lock whose process is gone counts as stale (never by age).
            lock.setStaleLockTime(0);
            if (lock.tryLock(0)) {
                stale = true;
                lock.unlock();
            }
        } else {
            // Without a lock (older versions, or a session that is just being created):
            // only when nothing changed in it for a day.
            stale = fi.lastModified().toUTC() < dayAgo;
        }
        if (stale && removeSessionFolder(fi.filePath()))
            ++removed;
    }
    return removed;
}

} // namespace lf
