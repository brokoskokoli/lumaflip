// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ordered list of images a window flips through (SPEC 2, 4.1, 4.2, 6).
// Lives on the GUI thread; folder enumeration and sorting run on a worker and
// results are applied via queued signals.

#include "FileEntry.h"
#include "SortSpec.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace lf {

class NameCollator;
class FolderOrderProvider;
class FileAttributeProvider;

enum class WrapMode { Stop, Wrap };  // F-NAV-03 (NextFolder: M3, handled by the window)

class Collection : public QObject {
    Q_OBJECT
public:
    explicit Collection(QObject* parent = nullptr);
    ~Collection() override;

    // F-OPEN-02/03: show `filePath` immediately (count()==1, currentIndex()==0), then
    // enumerate the folder asynchronously; when done, `entries` are replaced, sorted,
    // and current index points at `filePath` again. Emits loadingFinished().
    void openFile(const QString& filePath);
    // F-OPEN-04: exactly these files, sorted with the current spec.
    void openFiles(const QStringList& filePaths);
    // F-OPEN-05: folder contents, current = first.
    void openFolder(const QString& folderPath);

    QString folderPath() const;      // empty for openFiles() with mixed folders
    bool isFolderCollection() const;
    bool isLoading() const;

    int count() const;
    const FileEntry& at(int index) const;
    const QVector<FileEntry>& entries() const;
    int indexOf(const QString& path) const;

    int currentIndex() const;        // -1 if empty
    QString currentPath() const;
    void setCurrentIndex(int index);
    bool setCurrentPath(const QString& path);

    // Navigation (F-NAV-01/02). Return false if nothing changed (end reached with Stop).
    bool next(WrapMode wrap);
    bool previous(WrapMode wrap);
    bool first();
    bool last();
    bool jump(int delta, WrapMode wrap);

    // Sorting (F-SORT-*). `followFinder` = mode "Wie Finder".
    SortSpec sortSpec() const;
    void setSortSpec(const SortSpec& spec);   // manual; re-sorts, keeps current file
    void setFollowFinder(bool follow, bool allowLiveQuery);
    bool followsFinder() const;

    // F-OPEN-07: re-enumerate the folder, keep current file (or move to the
    // neighbour if it vanished). Called by the window when the watcher fires.
    void refresh();
    // Remove/insert an entry without re-enumeration (after trash/undo/move).
    void removePath(const QString& path);
    void insertPath(const QString& path);

    // Injection for tests; defaults are created via Platform.h factories.
    void setProviders(NameCollator* collator, FolderOrderProvider* order,
                      FileAttributeProvider* attributes);  // non-owning, may be null

signals:
    void loadingFinished();
    void entriesChanged();                 // list or order changed
    void currentChanged(int index, const QString& path);
    void sortSpecChanged(const lf::SortSpec& spec);
    void accessDenied(const QString& folderPath);  // F-PRIV-01

private:
    struct Private;
    Private* d;
};

// Pure, synchronous helpers (unit-testable, used by Collection internally).
namespace collection {
// Lists supported image files of a folder (no hidden files, no AppleDouble, no packages).
QVector<FileEntry> enumerateFolder(const QString& folderPath, bool* accessDenied = nullptr);
void sortEntries(QVector<FileEntry>& entries, const SortSpec& spec, const NameCollator& collator);
} // namespace collection

} // namespace lf
