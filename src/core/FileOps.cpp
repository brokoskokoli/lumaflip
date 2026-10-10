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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QList>
#include <QPointer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QtConcurrent/QtConcurrentRun>
#include <filesystem>
#include <optional>
#include <system_error>

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::FileOps", text);
}

// Result of the file system part of an operation.
struct Outcome {
    bool ok = false;
    bool noop = false; // nothing to do (e.g. already in the target folder)
    QString error;
    QString location;  // trash location / target path
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
            return tr("„%1“ existiert bereits.").arg(QFileInfo(target).fileName());
    }
    return {};
}

// Rename/move without overwriting (caller checked the target). Falls back to
// copy + remove across volumes.
QString movePath(const QString& from, const QString& to) {
    std::error_code ec;
    std::filesystem::rename(fsPath(from), fsPath(to), ec);
    if (!ec)
        return {};
    if (ec == std::errc::cross_device_link) {
        if (!QFile::copy(from, to))
            return tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(from).fileName());
        if (!QFile::remove(from)) {
            QFile::remove(to);
            return tr("„%1“ konnte nicht entfernt werden.").arg(QFileInfo(from).fileName());
        }
        return {};
    }
    return tr("„%1“ konnte nicht verschoben werden: %2")
        .arg(QFileInfo(from).fileName(), QString::fromStdString(ec.message()));
}

QString joinPath(const QString& folder, const QString& name) {
    return QDir(folder).filePath(name);
}

QString sourceError(const QString& path) {
    if (!QFileInfo::exists(path))
        return tr("„%1“ existiert nicht.").arg(QFileInfo(path).fileName());
    return {};
}

QString folderError(const QString& folder) {
    if (!QFileInfo(folder).isDir())
        return tr("Der Zielordner „%1“ existiert nicht.").arg(folder);
    return {};
}

QString nameError(const QString& newFileName) {
    if (newFileName.isEmpty() || newFileName == QLatin1String(".") || newFileName == QLatin1String("..") ||
        newFileName.contains(QLatin1Char('/')) || newFileName.contains(QChar(0)))
        return tr("„%1“ ist kein gültiger Dateiname.").arg(newFileName);
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

Outcome copyWork(const QString& from, const QString& to) {
    if (const QString e = targetTakenError(from, to); !e.isEmpty())
        return failure(e);
    if (!QFile::copy(from, to))
        return failure(tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(from).fileName()));
    return success(to);
}

// Base: first redo() is skipped because the operation already ran.
class FileCommand : public QUndoCommand {
public:
    FileCommand(FileOps* ops, const QString& text) : QUndoCommand(text), m_ops(ops) {}
    // File system work for redo (`forward`) or undo. Runs on a worker thread for the
    // asynchronous API: reads only state written before the work started.
    virtual Outcome work(bool forward) const = 0;
    // GUI thread: signals and bookkeeping after successful work.
    virtual void notify(bool forward, const Outcome& outcome) = 0;
    // The work of the next undo()/redo() was already done (asynchronously).
    void prepare(const Outcome& outcome) { m_prepared = outcome; }

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
        const Outcome o = m_prepared ? *m_prepared : work(forward);
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
};

class TrashCommand : public FileCommand {
public:
    TrashCommand(FileOps* ops, TrashService& trash, QString path, QString trashed, QObject* origin)
        : FileCommand(ops, tr("In den Papierkorb legen")), m_trash(trash), m_path(std::move(path)),
          m_trashed(std::move(trashed)), m_origin(origin) {}

    Outcome work(bool forward) const override {
        if (forward)
            return trashWork(m_trash, m_path);
        QString error;
        if (!m_trash.restoreFromTrash(m_trashed, m_path, &error))
            return failure(error);
        return success(m_path);
    }
    void notify(bool forward, const Outcome& o) override {
        if (forward) {
            m_trashed = o.location;
            emit m_ops->fileRemoved(m_path);
        } else {
            emit m_ops->fileAdded(m_path);
            emit m_ops->fileRestored(m_path, m_origin.data());
        }
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
        : FileCommand(ops, tr("Umbenennen")), m_from(std::move(from)), m_to(std::move(to)) {}

    Outcome work(bool forward) const override {
        return forward ? renameWork(m_from, m_to) : renameWork(m_to, m_from);
    }
    void notify(bool forward, const Outcome&) override {
        if (forward)
            emit m_ops->fileRenamed(m_from, m_to);
        else
            emit m_ops->fileRenamed(m_to, m_from);
    }

private:
    QString m_from, m_to;
};

class MoveCommand : public FileCommand {
public:
    MoveCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, tr("Verschieben")), m_from(std::move(from)), m_to(std::move(to)) {}

    Outcome work(bool forward) const override {
        return forward ? renameWork(m_from, m_to) : renameWork(m_to, m_from);
    }
    void notify(bool forward, const Outcome&) override {
        // Different folders: observers of each folder see a removal / an addition.
        const QString& a = forward ? m_from : m_to;
        const QString& b = forward ? m_to : m_from;
        emit m_ops->fileRemoved(a);
        emit m_ops->fileAdded(b);
    }

private:
    QString m_from, m_to;
};

class CopyCommand : public FileCommand {
public:
    CopyCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, tr("Kopieren")), m_from(std::move(from)), m_to(std::move(to)) {}

    Outcome work(bool forward) const override {
        if (forward)
            return copyWork(m_from, m_to);
        if (!QFile::remove(m_to))
            return failure(tr("Die Kopie „%1“ konnte nicht entfernt werden.").arg(QFileInfo(m_to).fileName()));
        return success(m_to);
    }
    void notify(bool forward, const Outcome&) override {
        if (forward)
            emit m_ops->fileAdded(m_to);
        else
            emit m_ops->fileRemoved(m_to);
    }

private:
    QString m_from, m_to;
};

// ---- whole operations: work + what to do afterwards ----

struct Operation {
    std::function<Outcome()> work;
    std::function<void(const Outcome&)> apply; // GUI thread, only on success (and !noop)
};

} // namespace

struct FileOps::Private {
    FileOps* q;
    TrashService& trash;
    QUndoStack* stack;
    QList<std::function<void()>> queue; // starters of queued asynchronous operations
    bool running = false;
    QPointer<QFutureWatcher<Outcome>> current;

    void setError(QString* error, const QString& msg) const {
        if (error)
            *error = msg;
    }

    // Synchronous: work + apply.
    bool runNow(const Operation& op, QString* error) {
        const Outcome o = op.work();
        if (!o.ok) {
            setError(error, o.error);
            return false;
        }
        if (!o.noop)
            op.apply(o);
        return true;
    }

    void enqueue(std::function<void()> starter) {
        queue.append(std::move(starter));
        if (!running)
            next();
    }

    void next() {
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
        const std::function<void()> starter = queue.takeFirst();
        starter(); // eventually calls next() again
    }

    // Runs `work` on a worker thread, then `finish` on the GUI thread, then the next operation.
    void runAsync(std::function<Outcome()> work, std::function<void(const Outcome&)> finish) {
        auto* w = new QFutureWatcher<Outcome>(q);
        current = w;
        QObject::connect(w, &QFutureWatcher<Outcome>::finished, q, [this, w, finish] {
            const Outcome o = w->result();
            w->deleteLater();
            finish(o);
            next();
        });
        w->setFuture(QtConcurrent::run(std::move(work)));
    }

    void startAsync(const Operation& op, const FileOps::Completion& done) {
        enqueue([this, op, done] {
            runAsync(op.work, [op, done](const Outcome& o) {
                if (o.ok && !o.noop)
                    op.apply(o);
                if (done)
                    done(o.ok, o.error);
            });
        });
    }

    // Undo (`forward` false) or redo of the next command, work on a worker thread.
    void startStep(bool forward) {
        enqueue([this, forward] {
            const int idx = stack->index();
            const int at = forward ? idx : idx - 1;
            if (at < 0 || at >= stack->count()) {
                next();
                return;
            }
            auto* cmd = dynamic_cast<FileCommand*>(const_cast<QUndoCommand*>(stack->command(at)));
            if (!cmd) { // not ours: let the stack run it
                forward ? stack->redo() : stack->undo();
                next();
                return;
            }
            runAsync([cmd, forward] { return cmd->work(forward); },
                     [this, cmd, forward, idx, at](const Outcome& o) {
                         if (stack->index() != idx || stack->command(at) != cmd)
                             return; // the stack changed meanwhile (synchronous use)
                         cmd->prepare(o);
                         forward ? stack->redo() : stack->undo();
                     });
        });
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
                    return renameWork(path, target);
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
                        return failure(tr("„%1“ existiert bereits.").arg(QFileInfo(target).fileName()));
                    return copyWork(path, target);
                },
                [this, path](const Outcome& r) {
                    emit q->fileAdded(r.location);
                    stack->push(new CopyCommand(q, path, r.location));
                }};
    }
};

FileOps::FileOps(TrashService& trash, QObject* parent)
    : QObject(parent), d(new Private{this, trash, new QUndoStack(this), {}, false, {}}) {}

FileOps::~FileOps() {
    // A running worker uses the trash service and the commands: let it finish first.
    d->queue.clear();
    if (d->current)
        d->current->waitForFinished();
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
