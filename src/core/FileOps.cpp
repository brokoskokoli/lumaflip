// SPDX-License-Identifier: GPL-3.0-or-later
//
// File operations with undo (F-FILE-01, F-FILE-04, F-FILE-08).
//
// Each public operation is executed immediately (so the caller gets an error) and,
// on success, pushed as an already-done QUndoCommand. Undo/redo re-run the
// operation; if that fails, the command marks itself obsolete (QUndoStack drops it)
// and FileOps::operationFailed is emitted.

#include "FileOps.h"
#include "Platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUndoCommand>
#include <QUndoStack>
#include <filesystem>
#include <system_error>

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::FileOps", text);
}

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
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

bool targetFree(const QString& source, const QString& target, QString* error) {
    if (QFileInfo(target).exists() || QFileInfo(target).isSymLink()) {
        if (!sameFile(source, target)) {
            setError(error, tr("„%1“ existiert bereits.").arg(QFileInfo(target).fileName()));
            return false;
        }
    }
    return true;
}

// Rename/move without overwriting (caller checked the target). Falls back to
// copy + remove across volumes.
bool movePath(const QString& from, const QString& to, QString* error) {
    std::error_code ec;
    std::filesystem::rename(fsPath(from), fsPath(to), ec);
    if (!ec)
        return true;
    if (ec == std::errc::cross_device_link) {
        if (!QFile::copy(from, to)) {
            setError(error, tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(from).fileName()));
            return false;
        }
        if (!QFile::remove(from)) {
            QFile::remove(to);
            setError(error, tr("„%1“ konnte nicht entfernt werden.").arg(QFileInfo(from).fileName()));
            return false;
        }
        return true;
    }
    setError(error, tr("„%1“ konnte nicht verschoben werden: %2")
                        .arg(QFileInfo(from).fileName(), QString::fromStdString(ec.message())));
    return false;
}

QString joinPath(const QString& folder, const QString& name) {
    return QDir(folder).filePath(name);
}

// Base: first redo() is skipped because the operation already ran.
class FileCommand : public QUndoCommand {
public:
    FileCommand(FileOps* ops, const QString& text) : QUndoCommand(text), m_ops(ops) {}
    void redo() final {
        if (m_firstRedo) {
            m_firstRedo = false;
            return;
        }
        QString error;
        if (!apply(&error))
            fail(error);
    }
    void undo() final {
        QString error;
        if (!revert(&error))
            fail(error);
    }

protected:
    virtual bool apply(QString* error) = 0;
    virtual bool revert(QString* error) = 0;
    FileOps* m_ops;

private:
    void fail(const QString& error) {
        setObsolete(true);
        emit m_ops->operationFailed(error);
    }
    bool m_firstRedo = true;
};

class TrashCommand : public FileCommand {
public:
    TrashCommand(FileOps* ops, TrashService& trash, QString path, QString trashed)
        : FileCommand(ops, tr("In den Papierkorb legen")), m_trash(trash), m_path(std::move(path)),
          m_trashed(std::move(trashed)) {}

protected:
    bool apply(QString* error) override {
        auto loc = m_trash.moveToTrash(m_path, error);
        if (!loc)
            return false;
        m_trashed = *loc;
        emit m_ops->fileRemoved(m_path);
        return true;
    }
    bool revert(QString* error) override {
        if (!m_trash.restoreFromTrash(m_trashed, m_path, error))
            return false;
        emit m_ops->fileAdded(m_path);
        return true;
    }

private:
    TrashService& m_trash;
    QString m_path;
    QString m_trashed;
};

class RenameCommand : public FileCommand {
public:
    RenameCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, tr("Umbenennen")), m_from(std::move(from)), m_to(std::move(to)) {}

protected:
    bool apply(QString* error) override { return run(m_from, m_to, error); }
    bool revert(QString* error) override { return run(m_to, m_from, error); }

private:
    bool run(const QString& a, const QString& b, QString* error) {
        if (!targetFree(a, b, error) || !movePath(a, b, error))
            return false;
        emit m_ops->fileRenamed(a, b);
        return true;
    }
    QString m_from, m_to;
};

class MoveCommand : public FileCommand {
public:
    MoveCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, tr("Verschieben")), m_from(std::move(from)), m_to(std::move(to)) {}

protected:
    bool apply(QString* error) override { return run(m_from, m_to, error); }
    bool revert(QString* error) override { return run(m_to, m_from, error); }

private:
    bool run(const QString& a, const QString& b, QString* error) {
        if (!targetFree(a, b, error) || !movePath(a, b, error))
            return false;
        // Different folders: observers of each folder see a removal / an addition.
        emit m_ops->fileRemoved(a);
        emit m_ops->fileAdded(b);
        return true;
    }
    QString m_from, m_to;
};

class CopyCommand : public FileCommand {
public:
    CopyCommand(FileOps* ops, QString from, QString to)
        : FileCommand(ops, tr("Kopieren")), m_from(std::move(from)), m_to(std::move(to)) {}

protected:
    bool apply(QString* error) override {
        if (!targetFree(m_from, m_to, error))
            return false;
        if (!QFile::copy(m_from, m_to)) {
            setError(error, tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(m_from).fileName()));
            return false;
        }
        emit m_ops->fileAdded(m_to);
        return true;
    }
    bool revert(QString* error) override {
        if (!QFile::remove(m_to)) {
            setError(error,
                     tr("Die Kopie „%1“ konnte nicht entfernt werden.").arg(QFileInfo(m_to).fileName()));
            return false;
        }
        emit m_ops->fileRemoved(m_to);
        return true;
    }

private:
    QString m_from, m_to;
};

bool checkSource(const QString& path, QString* error) {
    if (!QFileInfo::exists(path)) {
        setError(error, tr("„%1“ existiert nicht.").arg(QFileInfo(path).fileName()));
        return false;
    }
    return true;
}

bool checkFolder(const QString& folder, QString* error) {
    if (!QFileInfo(folder).isDir()) {
        setError(error, tr("Der Zielordner „%1“ existiert nicht.").arg(folder));
        return false;
    }
    return true;
}

} // namespace

struct FileOps::Private {
    TrashService& trash;
    QUndoStack* stack;
};

FileOps::FileOps(TrashService& trash, QObject* parent)
    : QObject(parent), d(new Private{trash, new QUndoStack(this)}) {}

FileOps::~FileOps() {
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
    if (!checkSource(path, error))
        return false;
    auto loc = d->trash.moveToTrash(path, error);
    if (!loc)
        return false;
    emit fileRemoved(path);
    d->stack->push(new TrashCommand(this, d->trash, path, *loc));
    return true;
}

bool FileOps::rename(const QString& path, const QString& newFileName, QString* error) {
    if (!checkSource(path, error))
        return false;
    if (newFileName.isEmpty() || newFileName == QLatin1String(".") || newFileName == QLatin1String("..") ||
        newFileName.contains(QLatin1Char('/')) || newFileName.contains(QChar(0))) {
        setError(error, tr("„%1“ ist kein gültiger Dateiname.").arg(newFileName));
        return false;
    }
    const QString target = joinPath(QFileInfo(path).absolutePath(), newFileName);
    if (target == path)
        return true;
    if (!targetFree(path, target, error) || !movePath(path, target, error))
        return false;
    emit fileRenamed(path, target);
    d->stack->push(new RenameCommand(this, path, target));
    return true;
}

bool FileOps::moveTo(const QString& path, const QString& targetFolder, QString* error) {
    if (!checkSource(path, error) || !checkFolder(targetFolder, error))
        return false;
    const QString target = joinPath(targetFolder, QFileInfo(path).fileName());
    if (sameFile(path, target))
        return true; // already there
    if (!targetFree(path, target, error) || !movePath(path, target, error))
        return false;
    emit fileRemoved(path);
    emit fileAdded(target);
    d->stack->push(new MoveCommand(this, path, target));
    return true;
}

bool FileOps::copyTo(const QString& path, const QString& targetFolder, QString* error) {
    if (!checkSource(path, error) || !checkFolder(targetFolder, error))
        return false;
    const QString target = joinPath(targetFolder, QFileInfo(path).fileName());
    if (QFileInfo(target).exists() || QFileInfo(target).isSymLink()) {
        setError(error, tr("„%1“ existiert bereits.").arg(QFileInfo(target).fileName()));
        return false;
    }
    if (!QFile::copy(path, target)) {
        setError(error, tr("„%1“ konnte nicht kopiert werden.").arg(QFileInfo(path).fileName()));
        return false;
    }
    emit fileAdded(target);
    d->stack->push(new CopyCommand(this, path, target));
    return true;
}

} // namespace lf
