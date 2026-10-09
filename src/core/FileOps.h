// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// File operations with undo (F-FILE-01/04/08, F-EDIT-02). Each operation is a
// QUndoCommand pushed onto the shared undo stack; observers are notified so
// collections/caches can update.

#include <QObject>
#include <QString>

class QUndoStack;

namespace lf {

class TrashService;

class FileOps : public QObject {
    Q_OBJECT
public:
    explicit FileOps(TrashService& trash, QObject* parent = nullptr);
    ~FileOps() override;

    static FileOps& instance();
    QUndoStack* undoStack() const;

    bool moveToTrash(const QString& path, QString* error);
    bool rename(const QString& path, const QString& newFileName, QString* error);
    bool moveTo(const QString& path, const QString& targetFolder, QString* error);
    bool copyTo(const QString& path, const QString& targetFolder, QString* error);

signals:
    // Emitted for do, undo and redo alike.
    void fileRemoved(const QString& path);
    void fileAdded(const QString& path);
    void fileRenamed(const QString& oldPath, const QString& newPath);
    // Undo/redo cannot return an error; failures (e.g. the original path is taken
    // again when restoring from the trash) are reported here. The failed command is
    // removed from the undo stack.
    void operationFailed(const QString& message);

private:
    struct Private;
    Private* d;
};

} // namespace lf
