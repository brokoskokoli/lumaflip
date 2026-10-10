// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// File operations with undo (F-FILE-01/04/08, F-EDIT-02). Each operation is a
// QUndoCommand pushed onto the shared undo stack; observers are notified so
// collections/caches can update.
//
// The UI uses the asynchronous variants (…Async, undo(), redo()): the file system work
// (trash on network volumes, cross-volume copies) runs on a worker thread, one operation
// after the other in request order; signals, the undo command and the completion
// callback follow on the GUI thread. The synchronous variants remain for callers that
// need the result at once (tests, scripts); they must not be mixed with pending
// asynchronous operations and fail (with an error, nothing done) while isBusy().
// Asynchronous work never touches an undo command directly, so the undo stack may change
// meanwhile without dangling pointers. Operations still queued when FileOps is destroyed
// are carried out then (never dropped); the app waits for them before it quits.

#include <QObject>
#include <QString>

#include <functional>

class QUndoStack;

namespace lf {

class TrashService;

class FileOps : public QObject {
    Q_OBJECT
public:
    // Called on the GUI thread when an asynchronous operation finished.
    using Completion = std::function<void(bool ok, const QString& error)>;

    explicit FileOps(TrashService& trash, QObject* parent = nullptr);
    ~FileOps() override;

    static FileOps& instance();
    QUndoStack* undoStack() const;
    TrashService& trashService() const; // for batches built on runAsync() (FileBatch)

    // Synchronous (blocking) operations.
    bool moveToTrash(const QString& path, QString* error);
    bool rename(const QString& path, const QString& newFileName, QString* error);
    bool moveTo(const QString& path, const QString& targetFolder, QString* error);
    bool copyTo(const QString& path, const QString& targetFolder, QString* error);

    // Asynchronous operations. `origin` (e.g. the window that trashed the file) is kept
    // with the undo command and reported again by fileRestored() on every undo.
    void moveToTrashAsync(const QString& path, QObject* origin = nullptr, const Completion& done = {});
    void renameAsync(const QString& path, const QString& newFileName, const Completion& done = {});
    void moveToAsync(const QString& path, const QString& targetFolder, const Completion& done = {});
    void copyToAsync(const QString& path, const QString& targetFolder, const Completion& done = {});
    // F-EDIT-02: a file changed in place (e.g. a saved rotation). `apply` (do, redo) and
    // `revert` (undo) run on a worker thread and return an error text (empty = success);
    // `cleanup` runs when the undo command is dropped (e.g. to delete backup copies).
    // fileChanged(path) follows every successful step.
    using FileWork = std::function<QString()>;
    void modifyAsync(const QString& path, const QString& undoText, FileWork apply, FileWork revert,
                     Completion done = {}, std::function<void()> cleanup = {});
    // General form of modifyAsync() (F-CULL-02 ratings, F-CULL-05/F-FILE-08/11 batches):
    // `notify(forward)` runs on the GUI thread after every successful step (do and redo:
    // true, undo: false) instead of emitting fileChanged(path). It may emit this object's
    // signals (fileRemoved, fileAdded …) for what the step changed.
    using StepNotifier = std::function<void(bool forward)>;
    void runAsync(const QString& undoText, FileWork apply, FileWork revert, StepNotifier notify,
                  Completion done = {}, std::function<void()> cleanup = {});
    // Undo/redo the next command with its file system work on a worker thread. (The
    // stack's own undo()/redo() still work and run synchronously.)
    void undo();
    void redo();
    bool isBusy() const; // an asynchronous operation is running or queued

signals:
    // Emitted for do, undo and redo alike.
    void fileRemoved(const QString& path);
    void fileAdded(const QString& path);
    void fileRenamed(const QString& oldPath, const QString& newPath);
    // The contents of `path` changed (modifyAsync, its undo and redo): caches must go.
    void fileChanged(const QString& path);
    // A trashed file was put back (undo), right after fileAdded(path). `origin` is the
    // object passed to moveToTrashAsync() (null if gone or not given).
    void fileRestored(const QString& path, QObject* origin);
    // Undo/redo cannot return an error; failures (e.g. the original path is taken
    // again when restoring from the trash) are reported here. The failed command is
    // removed from the undo stack.
    void operationFailed(const QString& message);
    void busyChanged(bool busy);

private:
    struct Private;
    Private* d;
};

namespace fileops {
// Moves/renames `from` to `to` without ever replacing an existing `to` (the check is part of
// the rename; across volumes copy + remove). Empty string on success, else a German error.
QString moveNoReplace(const QString& from, const QString& to);
} // namespace fileops

} // namespace lf
