// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Several file operations as one undo step (F-FILE-08 quick move/copy, F-FILE-11 name
// conflicts, F-CULL-05 "all rejects to the trash / to _Rejected/"). Runs on the FileOps
// queue (FileOps::runAsync): the work happens on its worker, undo/redo with ⌘Z/⇧⌘Z.
//
// - Units: items of one source folder with the same base name and the same target (both
//   files of a RAW+JPEG pair, F-FMT-06) are ONE unit. A unit has one conflict choice,
//   is skipped, replaced or renamed as a whole ("IMG_1 2.CR3" + "IMG_1 2.JPG" + their
//   sidecars) and trashed together.
// - Conflicts (a target name of the unit is taken) are resolved per unit with the choice of
//   its first item: Replace (the old targets - and the sidecars they had under the new
//   names - go to the trash, undo brings them back), KeepBoth ("name 2.jpg", like the
//   Finder: the first number free for every file of the unit) or Skip.
// - XMP sidecars travel with the unit (F-CULL-02: the rating stays with the file), see
//   sidecars::travelling(): "IMG_1.jpg.xmp" always, "IMG_1.xmp" with the file(s) owning it
//   (the RAW of a pair). A sidecar whose target is taken (without Replace) stays where it is.
// - All or nothing: if one unit fails, the units done before are rolled back and the error
//   is returned; nothing is put on the undo stack. A failing redo restores what it had
//   changed (also the replaced targets it had put into the trash again).
// - Target folders that do not exist are created (e.g. "_Rejected") and removed again by
//   undo if they are empty then.

#include "FileOps.h"
#include "Sidecars.h"

#include <QHash>
#include <QPair>
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
// The units of `items` (indices into `items`, in item order; see above).
QVector<QVector<int>> units(const QVector<TransferItem>& items, TransferKind kind);
// Units whose target names are taken by other files, as the source of the unit's first
// item (the UI asks about these first and sets that item's onConflict).
QStringList conflicts(const QVector<TransferItem>& items, TransferKind kind);
// The file names of the unit `source` belongs to ("IMG_1.CR3", "IMG_1.JPG").
QStringList unitNames(const QVector<TransferItem>& items, TransferKind kind, const QString& source);
// The sidecars that belong to `imagePath` alone (sidecars::travelling()).
QStringList ownSidecars(const QString& imagePath);
} // namespace filebatch

class FileBatch {
public:
    struct Step {                                  // one unit
        QVector<QPair<QString, QString>> files;    // (from, to); to: trash location for Trash
        QVector<QPair<QString, QString>> sidecars; // moved/copied/trashed along (from, to)
        QVector<QPair<QString, QString>> replaced; // targets put into the trash (path, trash location)
    };

    FileBatch(TrashService& trash, TransferKind kind, QVector<TransferItem> items);

    // For FileOps::runAsync (worker thread, one after the other).
    QString apply();
    QString revert();

    TransferKind kind() const { return m_kind; }
    QVector<Step> steps() const; // what the last apply() did (one per unit)
    int doneUnits() const;       // units done (a pair counts once)
    QStringList skipped() const; // sources skipped (conflict "Skip", source gone)
    // GUI thread, after a step: emits fileRemoved/fileAdded on `ops` for what changed.
    void notify(FileOps& ops, bool forward) const;

private:
    QString doStep(const QVector<int>& unit, Step* step);
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
    QHash<QString, sidecars::Folder> m_listings; // source folder -> listing (first apply)
};

// Queues `batch` on `ops` as one undo step named `undoText`; `done` on the GUI thread.
void runBatchAsync(FileOps& ops, const QString& undoText, const std::shared_ptr<FileBatch>& batch,
                   FileOps::Completion done = {});

} // namespace lf
