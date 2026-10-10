// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The ordered list of images a window flips through (SPEC 2, 4.1, 4.2, 6).
// Lives on the GUI thread; folder enumeration and sorting run on a worker and
// results are applied via queued signals.

#include "FileEntry.h"
#include "Rating.h"
#include "SortSpec.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace lf {

class NameCollator;
class FolderOrderProvider;
class FileAttributeProvider;

enum class WrapMode { Stop, Wrap }; // F-NAV-03 (NextFolder: M3, handled by the window)

// F-SORT-06: the sort choice remembered for one folder.
struct FolderSortChoice {
    bool followFinder = true; // "Wie Finder"
    SortSpec manual;          // otherwise
    friend bool operator==(const FolderSortChoice&, const FolderSortChoice&) = default;
};
// Looks up the remembered choice of a folder; called on worker threads.
using FolderSortMemory = std::function<std::optional<FolderSortChoice>(const QString& folderPath)>;
// Reads the rating of a file (RatingStore, F-CULL-02); called on worker threads.
using RatingReader = std::function<Rating(const QString& path)>;

class Collection : public QObject {
    Q_OBJECT
public:
    explicit Collection(QObject* parent = nullptr);
    ~Collection() override;

    // F-OPEN-02/03: show `filePath` immediately (count()==1, currentIndex()==0), then
    // enumerate the folder asynchronously; when done, `entries` are replaced, sorted,
    // and current index points at `filePath` again. Emits loadingFinished().
    void openFile(const QString& filePath);
    // F-OPEN-04: exactly these files, sorted with the current spec. A single file is
    // shown with its folder (F-OPEN-02) unless `keepAsFileSet` (a restored multi-file
    // window that has one file left, F-WIN-06).
    void openFiles(const QStringList& filePaths, bool keepAsFileSet = false);
    // F-OPEN-05: folder contents, current = first (or the last image with `selectLast`,
    // used when flipping backwards into the previous folder, F-NAV-03 "nextFolder").
    void openFolder(const QString& folderPath, bool selectLast = false);
    // F-WIN-09 / F-WIN-03: take over the state of `other` (input, entries, sort mode,
    // current file) without enumerating the folder again. If `other` is still
    // loading, the same load is started for this collection.
    void copyFrom(const Collection& other);

    QString folderPath() const; // empty for openFiles() with mixed folders
    bool isFolderCollection() const;
    bool isLoading() const;

    int count() const;
    const FileEntry& at(int index) const;
    const QVector<FileEntry>& entries() const;
    int indexOf(const QString& path) const;

    int currentIndex() const; // -1 if empty
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
    void setSortSpec(const SortSpec& spec); // manual; re-sorts, keeps current file
    void setFollowFinder(bool follow, bool allowLiveQuery);
    bool followsFinder() const;
    // The window's sort mode for the next open*(), without re-sorting the current entries
    // (a manual choice for one folder does not carry over to the next folder, F-SORT-06).
    void setDefaultSort(bool followFinder, bool allowLiveQuery, const SortSpec& manual);
    // F-SORT-06: a folder's remembered choice wins over the window's sort mode (folder
    // collections only). Empty = none.
    void setFolderSortMemory(FolderSortMemory memory);

    // F-OPEN-07: re-enumerate the folder, keep current file (or move to the
    // neighbour if it vanished). Invalidates the shared folder snapshot (FolderCache).
    void refresh();
    // Like refresh(), but takes the shared snapshot as it is: after one folder change the
    // app invalidates the FolderCache once and every window showing the folder reloads,
    // so the folder is enumerated only once (F-WIN-09).
    void reload();
    // Remove/insert an entry without re-enumeration (after trash/undo/move).
    void removePath(const QString& path);
    // The file's attributes are read on a worker thread; the entry appears (and becomes
    // current with `makeCurrent`) when they are known. A multi-file collection (F-OPEN-04)
    // only takes back files it contained, unless `addToFileSet`; a folder collection takes
    // files of its folder.
    void insertPath(const QString& path, bool addToFileSet = false, bool makeCurrent = false);
    // F-FILE-04: the entry keeps its attributes and its "current" state, only name and
    // position change. Files not in this collection are handled like insertPath(to).
    void renamePath(const QString& from, const QString& to);
    // True while insertPath() results are outstanding (tests).
    bool hasPendingInserts() const;

    // ---- ratings (M3) ----
    // F-SORT-02 / F-CULL-04: ratings for SortKey::Rating and the filter come from `reader`
    // (read on the worker with the entries). Without a reader the embedded rating of the
    // FileAttributeProvider is used for sorting and the filter is not applied.
    void setRatingReader(RatingReader reader);
    // F-CULL-04: shows only matching images (reloads). The explicitly opened file stays.
    // An image whose new rating no longer matches (updateRating) stays visible while it is
    // the current one and disappears when another image becomes current.
    void setRatingFilter(const RatingFilter& filter);
    RatingFilter ratingFilter() const;
    int totalCount() const;       // shown + hidden by the filter
    QStringList allPaths() const; // shown + hidden by the filter (F-CULL-05)
    // A rating changed (RatingStore::ratingChanged): updates the entry, applies the filter.
    void updateRating(const QString& path, const Rating& rating);

    // ---- name filter (F-NAV-08, M3) ----
    // Shows only images whose file name contains every word of `query` (case and
    // diacritics ignored, NameFilter.h); combines with the rating filter. Applied at once
    // (no re-enumeration); an image that no longer matches is hidden even if it is current
    // (the first match becomes current). Empty = off.
    void setNameFilter(const QString& query);
    QString nameFilter() const;

    // ---- subfolders (F-OPEN-08, M3) ----
    // Folder collections include all subfolders (recursive, per window/session): reloads.
    // Folders are read depth-first on a worker (cancellable, progress via loadProgress());
    // hidden folders and packages are skipped, symbolic links are not followed. Order:
    // folder by folder, path-wise in Finder name order (a folder's own images before those
    // of its subfolders, collection::compareFolderPaths()); within a folder the sort spec of
    // the collection applies ("Wie Finder": the order Finder uses for the opened folder).
    void setIncludeSubfolders(bool on);
    bool includesSubfolders() const;
    // Folders the collection reads: the folder, recursive also every subfolder of the last
    // load (root first). Empty for multi-file collections. Watch these (F-OPEN-07).
    QStringList folders() const;
    // A change in `folderPath` concerns this collection (its folder or, recursive, any
    // non-hidden subfolder - also one that is new).
    bool coversFolder(const QString& folderPath) const;
    // Subfolder of `path` relative to folderPath() ("2024/Urlaub"); empty for the folder
    // itself and for non-recursive collections.
    QString relativeFolder(const QString& path) const;

    // ---- RAW+JPEG pairs (F-FMT-06, M3) ----
    // Pairs with the same base name in one folder are one entry (the JPEG, fast); default
    // off (both shown). Reloads from the shared snapshot.
    void setPairRawJpeg(bool on);
    bool pairsRawJpeg() const;
    // The other file of the pair `path` belongs to (shown, hidden or not shown); else empty.
    QString pairedPath(const QString& path) const;
    // `paths` plus their pair members (each pair: the member not shown first). File
    // operations (trash, move, rating) use this so they apply to both files.
    QStringList withPairs(const QStringList& paths) const;
    // Shows the other member of the pair (JPEG <-> RAW) at the same position; it stays
    // shown across reloads. False if `path` is no pair.
    bool togglePairMember(const QString& path);

    // Injection for tests; defaults are created via Platform.h factories.
    void setProviders(NameCollator* collator, FolderOrderProvider* order,
                      FileAttributeProvider* attributes); // non-owning, may be null

signals:
    void loadingFinished();
    void entriesChanged(); // list or order changed
    void currentChanged(int index, const QString& path);
    void sortSpecChanged(const lf::SortSpec& spec);
    // F-PRIV-01: the folder cannot be listed. `privacyProtected`: macOS privacy protection
    // (TCC, EPERM) rather than plain POSIX permissions (EACCES).
    void accessDenied(const QString& folderPath, bool privacyProtected);
    // F-OPEN-08: the set of folders() changed (watch them).
    void foldersChanged();
    // F-OPEN-08: while a recursive load runs (about every 250 ms).
    void loadProgress(int foldersRead, int imagesFound);

private:
    struct Private;
    Private* d;
};

// Pure, synchronous helpers (unit-testable, used by Collection internally).
namespace collection {
// Lists supported image files of a folder (no hidden files, no AppleDouble, no packages).
// `privacyDenied`: the denial is EPERM (macOS privacy protection), not EACCES.
// `subfolders` (optional, F-OPEN-08): names of the visible subfolders (no hidden folders,
// no packages, symbolic links not followed).
QVector<FileEntry> enumerateFolder(const QString& folderPath, bool* accessDenied = nullptr,
                                   bool* privacyDenied = nullptr, QStringList* subfolders = nullptr);
void sortEntries(QVector<FileEntry>& entries, const SortSpec& spec, const NameCollator& collator);
// F-NAV-03 "Zum nächsten Ordner": the next (`direction` > 0) or previous sibling folder
// of `folderPath` in Finder name order that contains images; empty if there is none.
// Blocking (lists folders): call on a worker thread.
QString siblingImageFolder(const QString& folderPath, int direction, const NameCollator* collator);
// F-OPEN-08: order of folders in a recursive collection: path-wise in Finder name order,
// a folder before its subfolders (depth-first, like an expanded Finder list view).
int compareFolderPaths(const QString& a, const QString& b, const NameCollator& collator);
// F-FMT-06: RAW+JPEG pairs (same folder and base name, case-insensitive; JPEG = jpg, jpeg,
// jpe, jfif; RAW = isRawSuffix()). Each pair becomes one entry: the JPEG (or the RAW if it
// is in `showRaw`) with FileEntry::pairedPath set; the other file goes to `partners` (keyed
// by its path, its pairedPath pointing back).
bool isPairJpegSuffix(const QString& lowerSuffix);
bool isPairPartner(const QString& a, const QString& b);
void pairRawJpeg(QVector<FileEntry>& entries, QHash<QString, FileEntry>* partners,
                 const QSet<QString>& showRaw = {});
// M3: the rating an entry carries (RatingStore values, F-CULL-02) and setting it.
Rating ratingOf(const FileEntry& entry);
void setRating(FileEntry& entry, const Rating& rating);
} // namespace collection

} // namespace lf
