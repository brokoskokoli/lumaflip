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

    // Synchronous (blocking) operations.
    bool moveToTrash(const QString& path, QString* error);
    bool rename(const QString& path, const QString& newFileName, QString* error);
    bool moveTo(const QString& path, const QString& targetFolder, QString* error);
    bool copyTo(const QString& path, const QString& targetFolder, QString* error);

    // Asynchronous operations. `origin` (e.g. the window that trashed the file) is kept
    // with the undo command and reported again by fileRestored() on every undo.
    void moveToTrashAsync(const QString& path, QObject* origin = nullptr, Completion done = {});
    void renameAsync(const QString& path, const QString& newFileName, Completion done = {});
    void moveToAsync(const QString& path, const QString& targetFolder, Completion done = {});
    void copyToAsync(const QString& path, const QString& targetFolder, Completion done = {});
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

} // namespace lf
