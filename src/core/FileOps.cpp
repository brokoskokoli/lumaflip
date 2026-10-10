// SPDX-License-Identifier: GPL-3.0-or-later
//
// File operations with undo (F-FILE-01, F-FILE-04, F-FILE-08).
//
// Every operation is split into the file system work (thread-safe, no signals) and the
// notification (signals, undo stack; GUI thread):
// - the synchronous API runs both right away;
// - the asynchronous API runs the work on a worker thread and the notification when it
//   is done. Asynchronous operations (incl. undo/redo) are queued and run one after the
//   other, so their effects keep the request order.
// A successful operation is pushed as an already-done QUndoCommand. Undo/redo re-run the
// work; if that fails, the command marks itself obsolete (QUndoStack drops it) and
// FileOps::operationFailed is emitted.

#include "FileOps.h"
#include "Platform.h"
#include "Sidecars.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QPointer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QtConcurrent/QtConcurrentRun>
#include <filesystem>
#include <optional>
#include <system_error>

#include <cerrno>
#include <cstdio>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lf {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::FileOps)
};

// Result of the file system part of an operation.
struct Outcome {
    bool ok = false;
    bool noop = false; // nothing to do (e.g. already in the target folder)
    QString error;
    QString location; // trash location / target path
};

Outcome failure(const QString& error) {
    Outcome o;
    o.error = error;
    return o;
}

Outcome success(const QString& location = {}) {
    Outcome o;
    o.ok = true;
    o.location = location;
    return o;
}

std::filesystem::path fsPath(const QString& p) {
    return std::filesystem::path(p.toStdU16String());
}

// True if `a` and `b` refer to the same file (e.g. case-only rename on a
// case-insensitive volume).
bool sameFile(const QString& a, const QString& b) {
    std::error_code ec;
    return std::filesystem::equivalent(fsPath(a), fsPath(b), ec) && !ec;
}

QString targetTakenError(const QString& source, const QString& target) {
    if (QFileInfo(target).exists() || QFileInfo(target).isSymLink()) {
        if (!sameFile(source, target))
            return Tr::tr("„%1“ existiert bereits.").arg(QFileInfo(target).fileName());
    }
    return {};
}

} // namespace

// Test seam: runs between the "target taken?" check and the rename (simulates another
// program creating the target in that moment). Not for production code.
std::function<void(const QString& from, const QString& to)>& fileOpsBeforeRenameHook() {
    static std::function<void(const QString&, const QString&)> hook;
    return hook;
}

namespace {

// Renames `from` to `to` without ever replacing an existing `to`: the check is part of the
// rename itself (renamex_np RENAME_EXCL / renameat2 RENAME_NOREPLACE), so a file created
// at `to` after the caller's check is not overwritten. A case-only rename on a
// case-insensitive volume (`to` is `from`) uses a plain rename. Returns 0 or an errno.
int renameNoReplace(const QString& from, const QString& to) {
#if defined(Q_OS_WIN)
    // MoveFileExW without MOVEFILE_REPLACE_EXISTING fails if `to` exists: the check is part
    // of the rename (also for a case-only rename of the same file, which NTFS allows).
    const QString a = QDir::toNativeSeparators(from), b = QDir::toNativeSeparators(to);
    if (MoveFileExW(reinterpret_cast<const wchar_t*>(a.utf16()), reinterpret_cast<const wchar_t*>(b.utf16()),
                    0))
        return 0;
    switch (GetLastError()) {
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_NOT_SAME_DEVICE:
        return EXDEV;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    default:
        return EIO;
    }
#elif !defined(Q_OS_UNIX)
    if (!sameFile(from, to) && (QFileInfo(to).exists() || QFileInfo(to).isSymLink()))
        return EEXIST;
    std::error_code ec;
    std::filesystem::rename(fsPath(from), fsPath(to), ec);
    return ec ? (ec == std::errc::cross_device_link ? EXDEV : EIO) : 0;
#else
    const QByteArray a = QFile::encodeName(from), b = QFile::encodeName(to);
    if (sameFile(from, to))
        return ::rename(a.constData(), b.constData()) == 0 ? 0 : errno;
#if defined(Q_OS_DARWIN)
    if (::renamex_np(a.constData(), b.constData(), RENAME_EXCL) == 0)
        return 0;
    if (errno != ENOTSUP && errno != EINVAL)
        return errno;
#elif defined(Q_OS_LINUX) && defined(RENAME_NOREPLACE)
    if (::renameat2(AT_FDCWD, a.constData(), AT_FDCWD, b.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    if (errno != ENOSYS && errno != EINVAL)
        return errno;
#endif
    // File systems without an exclusive rename: link + unlink is exclusive as well
    // (link() fails if `to` exists); folders and volumes without hard links fall back to
    // check + rename.
    if (::link(a.constData(), b.constData()) == 0) {
        ::unlink(a.constData());
        return 0;
    }
    if (errno == EEXIST)
        return EEXIST;
    struct stat st{};
    if (::lstat(b.constData(), &st) == 0)
        return EEXIST;
    return ::rename(a.constData(), b.constData()) == 0 ? 0 : errno;
#endif
}

// Rename/move without overwriting (the caller checked the target for a friendly message;
// the rename checks again atomically). Falls back to copy + remove across volumes.
QString movePath(const QString& from, const QString& to) {
    if (auto& hook = fileOpsBeforeRenameHook())
        hook(from, to);
    const int err = renameNoReplace(from, to);
    if (err == 0)
        return {};
    if (err == EEXIST)
        return Tr::tr("„%1“ existiert bereits.").arg(QFileInfo(to).fileName());
    if (err == EXDEV) {
        if (!QFile::copy(from, to)) // never overwrites
            return Tr::tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(from).fileName());
        if (!QFile::remove(from)) {
            QFile::remove(to);
            return Tr::tr("„%1“ konnte nicht entfernt werden.").arg(QFileInfo(from).fileName());
        }
        return {};
    }
    return Tr::tr("„%1“ konnte nicht verschoben werden: %2")
        .arg(QFileInfo(from).fileName(), QString::fromLocal8Bit(strerror(err)));
}

QString joinPath(const QString& folder, const QString& name) {
    return QDir(folder).filePath(name);
}

QString sourceError(const QString& path) {
    if (!QFileInfo::exists(path))
        return Tr::tr("„%1“ existiert nicht.").arg(QFileInfo(path).fileName());
    return {};
}

QString folderError(const QString& folder) {
    if (!QFileInfo(folder).isDir())
        return Tr::tr("Der Zielordner „%1“ existiert nicht.").arg(folder);
    return {};
}

QString nameError(const QString& newFileName) {
    if (newFileName.isEmpty() || newFileName == QLatin1String(".") || newFileName == QLatin1String("..") ||
        newFileName.contains(QLatin1Char('/')) || newFileName.contains(QChar(0)))
        return Tr::tr("„%1“ ist kein gültiger Dateiname.").arg(newFileName);
    return {};
}

// ---- file system work (any thread) ----

Outcome trashWork(TrashService& trash, const QString& path) {
    if (const QString e = sourceError(path); !e.isEmpty())
        return failure(e);
    QString error;
    auto loc = trash.moveToTrash(path, &error);
    if (!loc)
        return failure(error);
    return success(*loc);
}

Outcome renameWork(const QString& from, const QString& to) {
    QString e = targetTakenError(from, to);
    if (e.isEmpty())
        e = movePath(from, to);
    return e.isEmpty() ? success(to) : failure(e);
}

// F-FILE-04 with F-CULL-02: the XMP sidecars of the file follow its new name
// ("IMG_1.JPG.xmp" -> "Urlaub.JPG.xmp"; "IMG_1.xmp" if the file owns it, Sidecars.h), so
// the rating stays with the file. A sidecar whose new name is taken stays.
Outcome renameWithSidecarsWork(const QString& from, const QString& to) {
    const QStringList sidecarPaths = sidecars::travelling({from});
    Outcome r = renameWork(from, to);
    if (!r.ok || sidecarPaths.isEmpty())
        return r;
    const QString fromName = QFileInfo(from).fileName();
    const QFileInfo target(to);
    for (const QString& s : sidecarPaths) {
        const QString name = QFileInfo(s).fileName();
        QString newName;
        if (name.size() > fromName.size() && name.startsWith(fromName, Qt::CaseInsensitive) &&
            name[fromName.size()] == u'.')
            newName = target.fileName() + name.mid(fromName.size());
        else
            newName = target.completeBaseName() + QLatin1Char('.') + QFileInfo(s).suffix();
        const QString dest = target.dir().filePath(newName);
        if (dest != s && !QFileInfo::exists(dest))
            fileops::moveNoReplace(s, dest);
    }
    return r;
}

Outcome copyWork(const QString& from, const QString& to) {
    if (const QString e = targetTakenError(from, to); !e.isEmpty())
        return failure(e);
    if (!QFile::copy(from, to))
        return failure(Tr::tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(from).fileName()));
    return success(to);
}

// Base: first redo() is skipped because the operation already ran.
class FileCommand : public QUndoCommand {
public:
    FileCommand(FileOps* ops, const QString& text) : QUndoCommand(text), m_ops(ops) {}
    // File system work for redo (`forward`) or undo as a self-contained closure (copies of
    // the paths, no pointer to this command): the asynchronous API runs it on a worker
    // thread, and the command may be gone by the time it is done.
    virtual std::function<Outcome()> workFor(bool forward) const = 0;
    // GUI thread: the signals for successful work, also self-contained (used when the
    // command is not on the stack any more when its work is done).
    virtual std::function<void(const Outcome&)> notifierFor(bool forward) const = 0;
    // GUI thread: bookkeeping after successful work (default: none) and the signals.
    virtual void notify(bool forward, const Outcome& outcome) { notifierFor(forward)(outcome); }
    // The work of the next undo()/redo() was already done (asynchronously).
    void prepare(const Outcome& outcome) { m_prepared = outcome; }
    // Expires when the command is deleted (e.g. the stack dropped it).
    std::weak_ptr<const char> aliveToken() const { return m_alive; }

    void redo() final {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        run(true);
    }
    void undo() final { run(false); }

protected:
    FileOps* m_ops;

private:
    void run(bool forward) {
        const Outcome o = m_prepared ? *m_prepared : workFor(forward)();
        m_prepared.reset();
        if (o.ok) {
            notify(forward, o);
            return;
        }
        setObsolete(true);
        emit m_ops->operationFailed(o.error);
    }
    std::optional<Outcome> m_prepared;
    bool m_firstRedo = true;
    std::shared_ptr<const char> m_alive = std::make_shared<const char>('\0');
};

class TrashCommand : public FileCommand {
public:
    TrashCommand(FileOps* ops, TrashService& trash, QString path, QString trashed, QObject* origin)
        : FileCommand(ops, Tr::tr("In den Papierkorb legen")), m_trash(trash), m_path(std::move(path)),
          m_trashed(std::move(trashed)), m_origin(origin) {}

    std::function<Outcome()> workFor(bool forward) const override {
        TrashService* trash = &m_trash; // owned by the FileOps' creator, outlives its workers
        const QString path = m_path;
        if (forward)
            return [trash, path] { return trashWork(*trash, path); };
        const QString trashed = m_trashed;
        return [trash, path, trashed] {
            QString error;
            if (!trash->restoreFromTrash(trashed, path, &error))
                return failure(error);
            return success(path);
        };
    }
    std::function<void(const Outcome&)> notifierFor(bool forward) const override {
        const QPointer<FileOps> ops(m_ops);
        const QString path = m_path;
        const QPointer<QObject> origin = m_origin;
        return [ops, path, origin, forward](const Outcome&) {
            if (!ops)
                return;
            if (forward) {
                emit ops->fileRemoved(path);
            } else {
                emit ops->fileAdded(path);
                emit ops->fileRestored(path, origin.data());
            }
        };
    }
    void notify(bool forward, const Outcome& o) override {
        if (forward)
            m_trashed = o.location; // trashed again: a new location in the trash
        FileCommand::notify(forward, o);
    }

private:
    TrashService& m_trash;
    QString m_path;
    QString m_trashed;
    QPointer<QObject> m_origin;
};

class RenameCommand : public FileCommand {
public:
    RenameCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, Tr::tr("Umbenennen")), m_from(std::move(from)), m_to(std::move(to)) {}

    std::function<Outcome()> workFor(bool forward) const override {
        const QString a = forward ? m_from : m_to;
        const QString b = forward ? m_to : m_from;
        return [a, b] { return renameWithSidecarsWork(a, b); };
    }
    std::function<void(const Outcome&)> notifierFor(bool forward) const override {
        const QPointer<FileOps> ops(m_ops);
        const QString a = forward ? m_from : m_to;
        const QString b = forward ? m_to : m_from;
        return [ops, a, b](const Outcome&) {
            if (ops)
                emit ops->fileRenamed(a, b);
        };
    }

private:
    QString m_from, m_to;
};

class MoveCommand : public FileCommand {
public:
    MoveCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, Tr::tr("Verschieben")), m_from(std::move(from)), m_to(std::move(to)) {}

    std::function<Outcome()> workFor(bool forward) const override {
        const QString a = forward ? m_from : m_to;
        const QString b = forward ? m_to : m_from;
        return [a, b] { return renameWork(a, b); };
    }
    std::function<void(const Outcome&)> notifierFor(bool forward) const override {
        // Different folders: observers of each folder see a removal / an addition.
        const QPointer<FileOps> ops(m_ops);
        const QString a = forward ? m_from : m_to;
        const QString b = forward ? m_to : m_from;
        return [ops, a, b](const Outcome&) {
            if (!ops)
                return;
            emit ops->fileRemoved(a);
            emit ops->fileAdded(b);
        };
    }

private:
    QString m_from, m_to;
};

class CopyCommand : public FileCommand {
public:
    CopyCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, Tr::tr("Kopieren")), m_from(std::move(from)), m_to(std::move(to)) {}

    std::function<Outcome()> workFor(bool forward) const override {
        const QString from = m_from, to = m_to;
        if (forward)
            return [from, to] { return copyWork(from, to); };
        return [to] {
            if (!QFile::remove(to))
                return failure(
                    Tr::tr("Die Kopie „%1“ konnte nicht entfernt werden.").arg(QFileInfo(to).fileName()));
            return success(to);
        };
    }
    std::function<void(const Outcome&)> notifierFor(bool forward) const override {
        const QPointer<FileOps> ops(m_ops);
        const QString to = m_to;
        return [ops, to, forward](const Outcome&) {
            if (!ops)
                return;
            if (forward)
                emit ops->fileAdded(to);
            else
                emit ops->fileRemoved(to);
        };
    }

private:
    QString m_from, m_to;
};

// F-EDIT-02: a file rewritten in place; the caller supplies the work for both directions.
class ModifyCommand : public FileCommand {
public:
    ModifyCommand(FileOps* ops, const QString& text, QString path, FileOps::FileWork apply,
                  FileOps::FileWork revert, std::function<void()> cleanup, FileOps::StepNotifier notify = {})
        : FileCommand(ops, text), m_path(std::move(path)), m_apply(std::move(apply)),
          m_revert(std::move(revert)), m_cleanup(std::move(cleanup)), m_notify(std::move(notify)) {}
    ~ModifyCommand() override {
        if (m_cleanup)
            m_cleanup();
    }

    std::function<Outcome()> workFor(bool forward) const override {
        const FileOps::FileWork work = forward ? m_apply : m_revert;
        return [work] {
            const QString e = work ? work() : QString();
            return e.isEmpty() ? success() : failure(e);
        };
    }
    std::function<void(const Outcome&)> notifierFor(bool forward) const override {
        const QPointer<FileOps> ops(m_ops);
        const QString path = m_path;
        const FileOps::StepNotifier notify = m_notify;
        return [ops, path, notify, forward](const Outcome&) {
            if (!ops)
                return;
            if (notify)
                notify(forward);
            else
                emit ops->fileChanged(path);
        };
    }

private:
    QString m_path;
    FileOps::FileWork m_apply, m_revert;
    std::function<void()> m_cleanup;
    FileOps::StepNotifier m_notify;
};

// ---- whole operations: work + what to do afterwards ----

struct Operation {
    std::function<Outcome()> work;
    std::function<void(const Outcome&)> apply; // GUI thread, only on success (and !noop)
};

// One queued asynchronous operation.
struct Task {
    // GUI thread, when the task's turn has come: the file system work (empty: nothing to do).
    std::function<std::function<Outcome()>()> start;
    // GUI thread, with the outcome of the work.
    std::function<void(const Outcome&)> finish;
};

} // namespace

struct FileOps::Private {
    FileOps* q;
    TrashService& trash;
    QUndoStack* stack;
    QList<Task> queue; // queued asynchronous operations (not started yet)
    bool running = false;
    QFuture<Outcome> current; // work of the running operation

    void setError(QString* error, const QString& msg) const {
        if (error)
            *error = msg;
    }

    // The synchronous API must not interleave with queued asynchronous operations: their
    // work was planned against the undo stack as it was when they started.
    bool refuseWhileBusy(QString* error) const {
        if (!running && queue.isEmpty())
            return false;
        qWarning("lf::FileOps: synchronous operation refused while asynchronous ones are pending");
        setError(error,
                 Tr::tr("Ein anderer Dateivorgang läuft noch. Bitte warte, bis er abgeschlossen ist."));
        return true;
    }

    // Synchronous: work + apply.
    bool runNow(const Operation& op, QString* error) {
        if (refuseWhileBusy(error))
            return false;
        const Outcome o = op.work();
        if (!o.ok) {
            setError(error, o.error);
            return false;
        }
        if (!o.noop)
            op.apply(o);
        return true;
    }

    void enqueue(Task task) {
        queue.append(std::move(task));
        if (!running)
            next();
    }

    void next() {
        for (;;) {
            if (queue.isEmpty()) {
                if (running) {
                    running = false;
                    emit q->busyChanged(false);
                }
                return;
            }
            if (!running) {
                running = true;
                emit q->busyChanged(true);
            }
            const Task task = queue.takeFirst();
            std::function<Outcome()> work = task.start();
            if (!work)
                continue; // nothing to do (e.g. undo with an empty stack)
            auto* w = new QFutureWatcher<Outcome>(q);
            QObject::connect(w, &QFutureWatcher<Outcome>::finished, q, [this, w, finish = task.finish] {
                const Outcome o = w->result();
                w->deleteLater();
                current = {};
                finish(o);
                next();
            });
            current = QtConcurrent::run(std::move(work));
            w->setFuture(current);
            return;
        }
    }

    void startAsync(const Operation& op, const FileOps::Completion& done) {
        enqueue({[work = op.work] { return work; },
                 [op, done](const Outcome& o) {
                     if (o.ok && !o.noop)
                         op.apply(o);
                     if (done)
                         done(o.ok, o.error);
                 }});
    }

    // Undo (`forward` false) or redo of the next command, work on a worker thread. What to
    // do is decided when the task starts (earlier queued operations may change the stack).
    void startStep(bool forward) {
        struct Plan {
            int index = -1; // stack index when the work started
            int at = -1;    // command undone/redone
            const QUndoCommand* command = nullptr;
            std::weak_ptr<const char> alive;
            std::function<void(const Outcome&)> notifier;
        };
        auto plan = std::make_shared<Plan>();
        enqueue({[this, forward, plan]() -> std::function<Outcome()> {
                     const int idx = stack->index();
                     const int at = forward ? idx : idx - 1;
                     if (at < 0 || at >= stack->count())
                         return {};
                     const auto* cmd = dynamic_cast<const FileCommand*>(stack->command(at));
                     if (!cmd) { // not ours: let the stack run it
                         forward ? stack->redo() : stack->undo();
                         return {};
                     }
                     plan->index = idx;
                     plan->at = at;
                     plan->command = cmd;
                     plan->alive = cmd->aliveToken();
                     plan->notifier = cmd->notifierFor(forward);
                     return cmd->workFor(forward); // copies: no command pointer on the worker
                 },
                 [this, forward, plan](const Outcome& o) { finishStep(forward, *plan, o); }});
    }

    template <typename Plan> void finishStep(bool forward, const Plan& plan, const Outcome& o) {
        const bool unchanged = !plan.alive.expired() && stack->index() == plan.index &&
                               plan.at < stack->count() && stack->command(plan.at) == plan.command;
        if (unchanged) {
            auto* cmd = const_cast<FileCommand*>(static_cast<const FileCommand*>(plan.command));
            cmd->prepare(o);
            forward ? stack->redo() : stack->undo(); // notifies (or drops a failed command)
            return;
        }
        // The stack changed while the work ran (only possible through QUndoStack directly).
        // The file system did change: tell every window, and start the history afresh
        // rather than leaving commands that no longer match the files.
        if (o.ok) {
            if (plan.notifier)
                plan.notifier(o);
        }
        stack->clear();
        emit q->operationFailed(o.ok ? Tr::tr("Der Verlauf zum Widerrufen wurde zurückgesetzt, weil er sich "
                                              "während des Vorgangs geändert hat.")
                                     : o.error);
    }

    // ---- operations ----

    Operation trashOp(const QString& path, QObject* origin) {
        QPointer<QObject> o(origin);
        return {[this, path] { return trashWork(trash, path); },
                [this, path, o](const Outcome& r) {
                    emit q->fileRemoved(path);
                    stack->push(new TrashCommand(q, trash, path, r.location, o.data()));
                }};
    }

    Operation renameOp(const QString& path, const QString& newFileName) {
        return {[path, newFileName] {
                    if (const QString e = sourceError(path); !e.isEmpty())
                        return failure(e);
                    if (const QString e = nameError(newFileName); !e.isEmpty())
                        return failure(e);
                    const QString target = joinPath(QFileInfo(path).absolutePath(), newFileName);
                    if (target == path) {
                        Outcome o = success(target);
                        o.noop = true;
                        return o;
                    }
                    return renameWithSidecarsWork(path, target);
                },
                [this, path](const Outcome& r) {
                    emit q->fileRenamed(path, r.location);
                    stack->push(new RenameCommand(q, path, r.location));
                }};
    }

    Operation moveOp(const QString& path, const QString& targetFolder) {
        return {[path, targetFolder] {
                    QString e = sourceError(path);
                    if (e.isEmpty())
                        e = folderError(targetFolder);
                    if (!e.isEmpty())
                        return failure(e);
                    const QString target = joinPath(targetFolder, QFileInfo(path).fileName());
                    if (sameFile(path, target)) {
                        Outcome o = success(target);
                        o.noop = true; // already there
                        return o;
                    }
                    return renameWork(path, target);
                },
                [this, path](const Outcome& r) {
                    emit q->fileRemoved(path);
                    emit q->fileAdded(r.location);
                    stack->push(new MoveCommand(q, path, r.location));
                }};
    }

    Operation copyOp(const QString& path, const QString& targetFolder) {
        return {[path, targetFolder] {
                    QString e = sourceError(path);
                    if (e.isEmpty())
                        e = folderError(targetFolder);
                    if (!e.isEmpty())
                        return failure(e);
                    const QString target = joinPath(targetFolder, QFileInfo(path).fileName());
                    if (QFileInfo(target).exists() || QFileInfo(target).isSymLink())
                        return failure(Tr::tr("„%1“ existiert bereits.").arg(QFileInfo(target).fileName()));
                    return copyWork(path, target);
                },
                [this, path](const Outcome& r) {
                    emit q->fileAdded(r.location);
                    stack->push(new CopyCommand(q, path, r.location));
                }};
    }
};

void FileOps::modifyAsync(const QString& path, const QString& undoText, FileWork apply, FileWork revert,
                          Completion done, std::function<void()> cleanup) {
    // The command owns `cleanup`; if the operation fails, nobody does: run it then.
    auto pendingCleanup = std::make_shared<std::function<void()>>(std::move(cleanup));
    Operation op{[apply] {
                     const QString e = apply ? apply() : QString();
                     return e.isEmpty() ? success() : failure(e);
                 },
                 [this, path, undoText, apply, revert, pendingCleanup](const Outcome&) {
                     emit fileChanged(path);
                     d->stack->push(new ModifyCommand(this, undoText, path, apply, revert,
                                                      std::exchange(*pendingCleanup, {})));
                 }};
    d->startAsync(op, [pendingCleanup, done](bool ok, const QString& error) {
        if (!ok && *pendingCleanup)
            std::exchange(*pendingCleanup, {})();
        if (done)
            done(ok, error);
    });
}

QString fileops::moveNoReplace(const QString& from, const QString& to) {
    return movePath(from, to);
}

void FileOps::runAsync(const QString& undoText, FileWork apply, FileWork revert, StepNotifier notify,
                       Completion done, std::function<void()> cleanup) {
    auto pendingCleanup = std::make_shared<std::function<void()>>(std::move(cleanup));
    Operation op{[apply] {
                     const QString e = apply ? apply() : QString();
                     return e.isEmpty() ? success() : failure(e);
                 },
                 [this, undoText, apply, revert, notify, pendingCleanup](const Outcome&) {
                     if (notify)
                         notify(true);
                     d->stack->push(new ModifyCommand(this, undoText, QString(), apply, revert,
                                                      std::exchange(*pendingCleanup, {}), notify));
                 }};
    d->startAsync(op, [pendingCleanup, done](bool ok, const QString& error) {
        if (!ok && *pendingCleanup)
            std::exchange(*pendingCleanup, {})();
        if (done)
            done(ok, error);
    });
}

FileOps::FileOps(TrashService& trash, QObject* parent)
    : QObject(parent), d(new Private{this, trash, new QUndoStack(this), {}, false, {}}) {}

FileOps::~FileOps() {
    // The running worker uses the trash service: let it finish first. Queued operations
    // are carried out now instead of being dropped silently (the app waits for them before
    // it quits, see Application::quit(); this is the last resort). Nobody listens to
    // notifications any more at this point, so only the file system work runs.
    d->current.waitForFinished();
    while (!d->queue.isEmpty()) {
        const Task task = d->queue.takeFirst();
        if (const std::function<Outcome()> work = task.start()) {
            const Outcome o = work();
            if (!o.ok)
                qWarning("lf::FileOps: queued operation failed at shutdown: %s", qPrintable(o.error));
        }
    }
    delete d;
}

FileOps& FileOps::instance() {
    static std::unique_ptr<TrashService> trash = createTrashService();
    static FileOps* ops = new FileOps(*trash, QCoreApplication::instance());
    return *ops;
}

QUndoStack* FileOps::undoStack() const {
    return d->stack;
}

TrashService& FileOps::trashService() const {
    return d->trash;
}

bool FileOps::moveToTrash(const QString& path, QString* error) {
    return d->runNow(d->trashOp(path, nullptr), error);
}

bool FileOps::rename(const QString& path, const QString& newFileName, QString* error) {
    return d->runNow(d->renameOp(path, newFileName), error);
}

bool FileOps::moveTo(const QString& path, const QString& targetFolder, QString* error) {
    return d->runNow(d->moveOp(path, targetFolder), error);
}

bool FileOps::copyTo(const QString& path, const QString& targetFolder, QString* error) {
    return d->runNow(d->copyOp(path, targetFolder), error);
}

void FileOps::moveToTrashAsync(const QString& path, QObject* origin, Completion done) {
    d->startAsync(d->trashOp(path, origin), done);
}

void FileOps::renameAsync(const QString& path, const QString& newFileName, Completion done) {
    d->startAsync(d->renameOp(path, newFileName), done);
}

void FileOps::moveToAsync(const QString& path, const QString& targetFolder, Completion done) {
    d->startAsync(d->moveOp(path, targetFolder), done);
}

void FileOps::copyToAsync(const QString& path, const QString& targetFolder, Completion done) {
    d->startAsync(d->copyOp(path, targetFolder), done);
}

void FileOps::undo() {
    d->startStep(false);
}

void FileOps::redo() {
    d->startStep(true);
}

bool FileOps::isBusy() const {
    return d->running || !d->queue.isEmpty();
}

} // namespace lf
