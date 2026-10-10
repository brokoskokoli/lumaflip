// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Several file operations as one undo step (F-FILE-08 quick move/copy, F-FILE-11 name
// conflicts, F-CULL-05 "all rejects to the trash / to _Rejected/"). Runs on the FileOps
// queue (FileOps::runAsync): the work happens on its worker, undo/redo with ⌘Z/⇧⌘Z.
//
// - Conflicts (target name taken) are resolved per item: Replace (the old target goes to
//   the trash, undo brings it back), KeepBoth ("name 2.jpg", like the Finder) or Skip.
// - XMP sidecars travel with moved/copied/trashed images (F-CULL-02: the rating stays with
//   the file): "IMG_1.jpg.xmp" always, "IMG_1.xmp" only if no other file of the folder
//   shares the base name (RAW+JPEG pairs share the Adobe sidecar). A sidecar whose target
//   is taken stays where it is.
// - All or nothing: if one item fails, the items done before are rolled back and the error
//   is returned; nothing is put on the undo stack.
// - Target folders that do not exist are created (e.g. "_Rejected") and removed again by
//   undo if they are empty then.

#include "FileOps.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <mutex>

namespace lf {

class TrashService;

enum class ConflictChoice { Replace, KeepBoth, Skip }; // F-FILE-11
enum class TransferKind { Move, Copy, Trash };

struct TransferItem {
    QString source;
    QString targetFolder; // ignored for Trash
    ConflictChoice onConflict = ConflictChoice::Skip;
};

namespace filebatch {
// F-FILE-11 "Beide behalten": "name 2.jpg", "name 3.jpg" … - the first name not taken in
// `folder`. `exists` defaults to the file system.
QString keepBothName(const QString& folder, const QString& fileName,
                     const std::function<bool(const QString&)>& exists = {});
// Items whose target name is taken by another file (the UI asks about these first).
QStringList conflicts(const QVector<TransferItem>& items, TransferKind kind);
// The sidecars that belong to `imagePath` alone (see above).
QStringList ownSidecars(const QString& imagePath);
} // namespace filebatch

class FileBatch {
public:
    struct Step {
        QString from, to;                          // to: trash location for Trash
        QString replacedFrom;                      // a replaced target ...
        QString replacedTrash;                     // ... and where it went in the trash
        QVector<QPair<QString, QString>> sidecars; // moved/copied along (from, to)
    };

    FileBatch(TrashService& trash, TransferKind kind, QVector<TransferItem> items);

    // For FileOps::runAsync (worker thread, one after the other).
    QString apply();
    QString revert();

    TransferKind kind() const { return m_kind; }
    QVector<Step> steps() const; // what the last apply() did
    QStringList skipped() const; // sources skipped (conflict "Skip", source gone)
    // GUI thread, after a step: emits fileRemoved/fileAdded on `ops` for what changed.
    void notify(FileOps& ops, bool forward) const;

private:
    QString doStep(const TransferItem& item, Step* step);
    QString redoStep(Step& step);
    QString undoStep(const Step& step);

    TrashService& m_trash;
    TransferKind m_kind;
    QVector<TransferItem> m_items;
    mutable std::mutex m_mutex;
    bool m_planned = false;
    QVector<Step> m_steps;
    QStringList m_skipped;
    QStringList m_createdFolders;
};

// Queues `batch` on `ops` as one undo step named `undoText`; `done` on the GUI thread.
void runBatchAsync(FileOps& ops, const QString& undoText, std::shared_ptr<FileBatch> batch,
                   FileOps::Completion done = {});

} // namespace lf
