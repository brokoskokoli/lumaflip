// SPDX-License-Identifier: GPL-3.0-or-later
#include "Collection.h"

#include "Archive.h"
#include "FolderCache.h"
#include "ImageFormats.h"
#include "NameFilter.h"
#include "Platform.h"
#include "VirtualPath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lf {

namespace {

// Packages are folders the Finder shows as one file ("X.app", "Fotos.photoslibrary"). macOS
// asks Launch Services (bundle bit, registered extensions); elsewhere the usual package
// extensions identify them (Mac folders on a shared or external drive).
bool isPackageFolder(const QFileInfo& fi) {
#ifdef Q_OS_DARWIN
    return fi.isBundle();
#else
    static const QSet<QString> kPackageSuffixes = {
        QStringLiteral("app"),
        QStringLiteral("bundle"),
        QStringLiteral("framework"),
        QStringLiteral("plugin"),
        QStringLiteral("kext"),
        QStringLiteral("photoslibrary"),
        QStringLiteral("photolibrary"),
        QStringLiteral("aplibrary"),
        QStringLiteral("migratedphotolibrary"),
        QStringLiteral("fcpbundle"),
        QStringLiteral("imovielibrary"),
        QStringLiteral("rtfd"),
        QStringLiteral("pkg"),
        QStringLiteral("mpkg"),
        QStringLiteral("xcodeproj"),
        QStringLiteral("xcworkspace"),
        QStringLiteral("playground"),
        QStringLiteral("logicx"),
    };
    return fi.isDir() && kPackageSuffixes.contains(fi.suffix().toLower());
#endif
}

} // namespace

// ---------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------
namespace collection {

Rating ratingOf(const FileEntry& e) {
    Rating r;
    r.stars = std::clamp(e.rating, 0, 5);
    r.flag = e.pick > 0 ? PickFlag::Pick : (e.pick < 0 ? PickFlag::Reject : PickFlag::None);
    return r;
}

void setRating(FileEntry& e, const Rating& r) {
    e.rating = r.stars;
    e.pick = qint8(r.flag);
    e.ratingRead = true;
}

namespace {

bool acceptName(const QString& name) {
    // Hidden files (incl. AppleDouble "._*") and unsupported suffixes (F-OPEN-06).
    if (name.isEmpty() || name.startsWith(u'.'))
        return false;
    return hasImageSuffix(name);
}

#ifdef Q_OS_UNIX
QDateTime fromTimespec(const struct timespec& ts) {
    return QDateTime::fromMSecsSinceEpoch(qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000, QTimeZone::UTC);
}

// F-OPEN-08: a subfolder the Finder shows (not hidden via chflags, not a package such as
// "X.app" or "Fotos.photoslibrary") on the same volume: the recursion stops at mount points
// (another disk, a network share or a disk image mounted inside the tree).
bool acceptSubfolder(int dfd, const char* name, const QString& fullPath, dev_t parentDevice) {
    struct stat st;
    if (::fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
        return false;
    if (st.st_dev != parentDevice)
        return false;
#ifdef UF_HIDDEN
    if (st.st_flags & UF_HIDDEN)
        return false;
#endif
    // Packages carry an extension (or the bundle bit); asking costs a Launch Services call,
    // so only for names with a dot.
    if (fullPath.lastIndexOf(u'.') > fullPath.lastIndexOf(u'/') && isPackageFolder(QFileInfo(fullPath)))
        return false;
    return true;
}
#endif

} // namespace

QVector<FileEntry> enumerateFolder(const QString& folderPath, bool* accessDenied, bool* privacyDenied,
                                   QStringList* subfolders) {
    if (accessDenied)
        *accessDenied = false;
    if (privacyDenied)
        *privacyDenied = false;
    if (subfolders)
        subfolders->clear();
    QVector<FileEntry> out;
    const QString folder = QDir::cleanPath(folderPath);
    const QString prefix = folder.endsWith(u'/') ? folder : folder + u'/';
#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(folder);
    const int dfd = ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR* dir = dfd >= 0 ? ::fdopendir(dfd) : nullptr;
    if (!dir) {
        const int err = errno;
        if (dfd >= 0)
            ::close(dfd);
        if (accessDenied && (err == EACCES || err == EPERM))
            *accessDenied = true; // F-PRIV-01
        // macOS privacy protection (TCC: Desktop, Documents, Downloads, removable and
        // network volumes) denies with EPERM; missing POSIX permissions give EACCES.
        if (privacyDenied && err == EPERM)
            *privacyDenied = true;
        return out;
    }
    struct stat folderStat{};
    const dev_t device = ::fstat(dfd, &folderStat) == 0 ? folderStat.st_dev : dev_t(0);
    errno = 0;
    while (struct dirent* de = ::readdir(dir)) {
        if (de->d_name[0] == '.')
            continue;
        if (de->d_type == DT_DIR && subfolders) {
            // F-OPEN-08: real subfolders only (symbolic links are not followed: no cycles).
            const QString name = QFile::decodeName(de->d_name);
            if (acceptSubfolder(dfd, de->d_name, prefix + name, device))
                subfolders->append(name);
            continue;
        }
        if (de->d_type != DT_REG && de->d_type != DT_LNK && de->d_type != DT_UNKNOWN)
            continue; // directories (incl. packages/bundles), devices, sockets ...
        const QString name = QFile::decodeName(de->d_name);
        if (!acceptName(name))
            continue;
        struct stat st;
        if (de->d_type == DT_UNKNOWN && subfolders &&
            ::fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode)) {
            const QString dirName = QFile::decodeName(de->d_name);
            if (acceptSubfolder(dfd, de->d_name, prefix + dirName, device))
                subfolders->append(dirName);
            continue;
        }
        if (::fstatat(dfd, de->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode))
            continue; // dangling symlink or symlink to a directory
#ifdef UF_HIDDEN
        if (st.st_flags & UF_HIDDEN)
            continue; // hidden via chflags, Finder does not show it either
#endif
        FileEntry e;
        e.name = name;
        e.path = prefix + name;
        e.size = qint64(st.st_size);
#ifdef Q_OS_DARWIN
        e.modified = fromTimespec(st.st_mtimespec);
        e.created = fromTimespec(st.st_birthtimespec);
#else
        e.modified = fromTimespec(st.st_mtim);
#endif
        out.append(std::move(e));
    }
    ::closedir(dir); // also closes dfd
#else
    const QDir dir(folder);
    if (!dir.exists() || !QFileInfo(folder).isReadable()) {
        if (accessDenied && dir.exists())
            *accessDenied = true;
        return out;
    }
    const QFileInfoList infos =
        dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo& fi : infos) {
        if (fi.isDir()) {
            if (subfolders && !fi.isSymLink() && !fi.isHidden() && !isPackageFolder(fi) &&
                !fi.fileName().startsWith(u'.'))
                subfolders->append(fi.fileName());
            continue;
        }
        if (fi.isHidden() || !acceptName(fi.fileName()) || fi.isBundle())
            continue;
        FileEntry e;
        e.name = fi.fileName();
        e.path = prefix + e.name;
        e.size = fi.size();
        e.modified = fi.lastModified(QTimeZone::UTC);
        e.created = fi.birthTime(QTimeZone::UTC);
        out.append(std::move(e));
    }
#endif
    return out;
}

namespace {

// Numeric primary key; `known == false` sorts after all known values in either direction.
struct NumKey {
    qint64 value = 0;
    bool known = false;
};

qint64 msecs(const QDateTime& dt) {
    return dt.toMSecsSinceEpoch();
}

NumKey numKey(const FileEntry& e, SortKey key) {
    switch (key) {
    case SortKey::DateModified:
        return {e.modified.isValid() ? msecs(e.modified) : 0, e.modified.isValid()};
    case SortKey::DateCreated:
        return {e.created.isValid() ? msecs(e.created) : 0, e.created.isValid()};
    case SortKey::DateAdded:
        return {e.added.isValid() ? msecs(e.added) : 0, e.added.isValid()};
    case SortKey::DateCaptured:
        if (e.captured && e.captured->isValid())
            return {msecs(*e.captured), true};
        return {};
    case SortKey::Size:
        return {e.size, e.size >= 0};
    case SortKey::PixelCount:
        return {e.pixelCount, e.pixelCount >= 0};
    case SortKey::Rating:
        if (e.ratingRead) // RatingStore (F-SORT-02, M3): rejects lowest, picks above
            return {rating::sortValue(collection::ratingOf(e)), true};
        return {e.rating, e.rating >= 0};
    default:
        return {};
    }
}

int sign(qint64 v) {
    return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

// Name tie-break (always ascending), then byte order of the path for full determinism.
int tieBreak(const FileEntry& a, const FileEntry& b, const NameCollator& collator) {
    if (const int c = collator.compare(a.name, b.name))
        return c;
    return sign(a.path.compare(b.path));
}

// Primary-key comparison; `unknownA/B` report missing values (sorted last, both directions).
int comparePrimary(const FileEntry& a, const FileEntry& b, const SortSpec& spec, const NameCollator& collator,
                   const NumKey* ka, const NumKey* kb) {
    switch (spec.key) {
    case SortKey::Name: {
        const int c = collator.compare(a.name, b.name);
        return spec.ascending ? c : -c;
    }
    case SortKey::Kind: {
        const bool ea = a.kind.isEmpty(), eb = b.kind.isEmpty();
        if (ea != eb)
            return ea ? 1 : -1;
        const int c = ea ? 0 : collator.compare(a.kind, b.kind);
        return spec.ascending ? c : -c;
    }
    case SortKey::FinderIconPosition: {
        const bool ha = a.finderIconPosition.has_value(), hb = b.finderIconPosition.has_value();
        if (ha != hb)
            return ha ? -1 : 1;
        if (!ha)
            return 0;
        const QPointF pa = *a.finderIconPosition, pb = *b.finderIconPosition;
        int c = pa.y() < pb.y() ? -1 : (pa.y() > pb.y() ? 1 : 0);
        if (c == 0)
            c = pa.x() < pb.x() ? -1 : (pa.x() > pb.x() ? 1 : 0);
        return spec.ascending ? c : -c;
    }
    default: {
        const NumKey x = ka ? *ka : numKey(a, spec.key);
        const NumKey y = kb ? *kb : numKey(b, spec.key);
        if (x.known != y.known)
            return x.known ? -1 : 1;
        const int c = x.known ? (x.value < y.value ? -1 : (x.value > y.value ? 1 : 0)) : 0;
        return spec.ascending ? c : -c;
    }
    }
}

int compareEntries(const FileEntry& a, const FileEntry& b, const SortSpec& spec,
                   const NameCollator& collator) {
    if (const int c = comparePrimary(a, b, spec, collator, nullptr, nullptr))
        return c;
    return tieBreak(a, b, collator);
}

} // namespace

void sortEntries(QVector<FileEntry>& entries, const SortSpec& spec, const NameCollator& collator) {
    const qsizetype n = entries.size();
    if (n < 2)
        return;
    // Precompute numeric keys once (dates are comparatively expensive to convert).
    std::vector<NumKey> keys(static_cast<size_t>(n));
    for (qsizetype i = 0; i < n; ++i)
        keys[size_t(i)] = numKey(entries[i], spec.key);
    // F-SORT-01 / P-05: the names are prepared once for the collator (e.g. one CFString per
    // name instead of two per comparison); results are identical to collator.compare().
    QVector<QString> names;
    names.reserve(n);
    for (const FileEntry& e : std::as_const(entries))
        names.append(e.name);
    const std::unique_ptr<PreparedNames> prepared = collator.prepare(std::move(names));
    std::vector<qsizetype> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), qsizetype(0));
    std::stable_sort(order.begin(), order.end(), [&](qsizetype i, qsizetype j) {
        const FileEntry& a = entries[i];
        const FileEntry& b = entries[j];
        int c;
        if (spec.key == SortKey::Name) {
            c = prepared->compare(i, j);
            c = spec.ascending ? c : -c;
        } else {
            c = comparePrimary(a, b, spec, collator, &keys[size_t(i)], &keys[size_t(j)]);
        }
        if (c == 0) // tie-break: name ascending, then the path bytes (see tieBreak())
            c = prepared->compare(i, j);
        if (c == 0)
            c = sign(a.path.compare(b.path));
        return c < 0;
    });
    QVector<FileEntry> sorted;
    sorted.reserve(n);
    for (qsizetype i : order)
        sorted.append(std::move(entries[i]));
    entries = std::move(sorted);
}

QString siblingImageFolder(const QString& folderPath, int direction, const NameCollator* collator) {
    constexpr int kMaxCandidates = 500; // do not crawl huge parents
    const QString folder = QDir::cleanPath(folderPath);
    const QFileInfo self(folder);
    const QDir parent = self.dir();
    if (self.isRoot() || parent.path() == folder)
        return {};
    QStringList names;
    const QFileInfoList infos = parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo& fi : infos)
        if (!fi.isHidden() && !isPackageFolder(fi) && !fi.fileName().startsWith(u'.'))
            names << fi.fileName();
    std::sort(names.begin(), names.end(), [collator](const QString& a, const QString& b) {
        return collator ? collator->compare(a, b) < 0 : a.compare(b, Qt::CaseInsensitive) < 0;
    });
    const QString me = self.fileName();
    qsizetype i = names.indexOf(me);
    if (i < 0) // e.g. a different normalization of the name
        i = names.indexOf(me.normalized(QString::NormalizationForm_D));
    if (i < 0)
        return {};
    const int step = direction >= 0 ? 1 : -1;
    int checked = 0;
    for (qsizetype k = i + step; k >= 0 && k < names.size() && checked < kMaxCandidates;
         k += step, ++checked) {
        const QString candidate = parent.filePath(names[k]);
        if (!enumerateFolder(candidate).isEmpty())
            return QDir::cleanPath(candidate);
    }
    return {};
}

int compareFolderPaths(const QString& a, const QString& b, const NameCollator& collator) {
    if (a == b)
        return 0;
    const QStringList pa = a.split(u'/', Qt::SkipEmptyParts);
    const QStringList pb = b.split(u'/', Qt::SkipEmptyParts);
    const qsizetype n = std::min(pa.size(), pb.size());
    for (qsizetype i = 0; i < n; ++i) {
        if (pa[i] == pb[i])
            continue;
        if (const int c = collator.compare(pa[i], pb[i]))
            return c;
        if (const int c = pa[i].compare(pb[i]))
            return c < 0 ? -1 : 1;
    }
    return pa.size() < pb.size() ? -1 : (pa.size() > pb.size() ? 1 : 0); // a folder before its subfolders
}

namespace {
// "IMG_1" of "/a/IMG_1.CR2", folded, with its folder: the key two files of a pair share.
QString pairKey(const QString& path) {
    const qsizetype slash = path.lastIndexOf(u'/');
    const qsizetype dot = path.lastIndexOf(u'.');
    if (dot <= slash + 1)
        return {};
    return path.left(dot).toCaseFolded();
}
QString suffixOf(const QString& path) {
    const qsizetype slash = path.lastIndexOf(u'/');
    const qsizetype dot = path.lastIndexOf(u'.');
    return dot > slash ? path.mid(dot + 1).toLower() : QString();
}
} // namespace

bool isPairJpegSuffix(const QString& lowerSuffix) {
    return lowerSuffix == u"jpg" || lowerSuffix == u"jpeg" || lowerSuffix == u"jpe" || lowerSuffix == u"jfif";
}

bool isPairPartner(const QString& a, const QString& b) {
    const QString sa = suffixOf(a), sb = suffixOf(b);
    const bool pairKinds =
        (isPairJpegSuffix(sa) && isRawSuffix(sb)) || (isRawSuffix(sa) && isPairJpegSuffix(sb));
    return pairKinds && !pairKey(a).isEmpty() && pairKey(a) == pairKey(b);
}

void pairRawJpeg(QVector<FileEntry>& entries, QHash<QString, FileEntry>* partners,
                 const QSet<QString>& showRaw) {
    // Group by folder + base name; only groups with a JPEG and a RAW are touched.
    QHash<QString, std::pair<qsizetype, qsizetype>> groups; // key -> (jpeg index, raw index)
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const QString sfx = suffixOf(entries[i].path);
        const bool jpeg = isPairJpegSuffix(sfx), raw = !jpeg && isRawSuffix(sfx);
        if (!jpeg && !raw)
            continue;
        const QString key = pairKey(entries[i].path);
        if (key.isEmpty())
            continue;
        auto it = groups.find(key);
        if (it == groups.end())
            it = groups.insert(key, {-1, -1});
        qsizetype& slot = jpeg ? it->first : it->second;
        // Several JPEGs/RAWs with one base name (IMG_1.jpg + IMG_1.JPEG): the first name pairs.
        if (slot < 0 || entries[i].name < entries[slot].name)
            slot = i;
    }
    QSet<qsizetype> drop;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        const auto [j, r] = it.value();
        if (j < 0 || r < 0)
            continue;
        // The JPEG is shown (fast); the RAW is shown if it was opened explicitly.
        const bool rawShown = showRaw.contains(entries[r].path);
        const qsizetype shown = rawShown ? r : j, other = rawShown ? j : r;
        FileEntry partner = entries[other];
        partner.pairedPath = entries[shown].path;
        entries[shown].pairedPath = partner.path;
        if (partners)
            partners->insert(partner.path, std::move(partner));
        drop.insert(other);
    }
    if (drop.isEmpty())
        return;
    QVector<FileEntry> kept;
    kept.reserve(entries.size() - drop.size());
    for (qsizetype i = 0; i < entries.size(); ++i)
        if (!drop.contains(i))
            kept.append(std::move(entries[i]));
    entries = std::move(kept);
}

} // namespace collection

// ---------------------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------------------
namespace {

// Container (F-OPEN-10, M4): the images inside an archive or PDF (VirtualPath.h), read on a
// worker like a folder; never watched, no file operations.
enum class Mode { None, File, Files, Folder, Container };
// Modes without a folder of their own (no recursion, no folder sort memory).
bool fileLike(Mode m) {
    return m == Mode::Files || m == Mode::Container;
}
enum class JobKind { Load, Resort };

constexpr int kMaxTreeFolders = 100000; // F-OPEN-08: safety limit of a recursive collection
// F-OPEN-08: a recursive collection stops reading after this many images (a whole disk
// opened by accident); the window says so (Collection::treeLimitReached).
constexpr int kMaxTreeImages = 200000;
// F-NAV-08: up to this many entries the name filter runs on the GUI thread (a few ms);
// larger collections are filtered on a worker.
constexpr qsizetype kSyncNameFilterEntries = 5000;
// Folder change notifications are coalesced for this long before the collection updates.
constexpr int kChangeDebounceMs = 300;

// F-OPEN-08: progress of a recursive load, written by the worker, polled by the GUI thread.
struct LoadCounters {
    std::atomic<int> folders{0};
    std::atomic<int> images{0};
};

QString folderOf(const QString& path) {
    const qsizetype slash = path.lastIndexOf(u'/');
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

FileEntry entryForPath(const QString& path) {
    FileEntry e;
    e.path = path;
    e.name = QFileInfo(path).fileName();
    return e;
}

std::optional<FileEntry> statEntry(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return std::nullopt;
    FileEntry e = entryForPath(path);
    e.size = fi.size();
    e.modified = fi.lastModified(QTimeZone::UTC);
    e.created = fi.birthTime(QTimeZone::UTC);
    return e;
}

QString normalizedPath(const QString& path) {
    if (path.isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).normalized(QString::NormalizationForm_C);
}

QString parentOf(const QString& path) {
    return QFileInfo(path).absolutePath();
}

// Sort keys whose values come from the image metadata (F-SORT-02), read on demand.
bool needsImageProperties(SortKey key) {
    return key == SortKey::DateCaptured || key == SortKey::PixelCount || key == SortKey::Rating;
}

struct Job {
    JobKind kind = JobKind::Load;
    quint64 generation = 0;
    std::shared_ptr<std::atomic<quint64>> latest;
    Mode mode = Mode::None;
    QString folder;              // folder to enumerate / ask Finder about; may be empty
    QString mustInclude;         // explicitly opened file, kept even if filtered
    QStringList files;           // Mode::Files
    QString container;           // Mode::Container: the archive or PDF
    QVector<FileEntry> existing; // JobKind::Resort
    bool followFinder = true;
    bool allowLiveQuery = false;
    SortSpec manualSpec;
    FolderSortMemory folderMemory; // F-SORT-06
    RatingReader ratingReader;     // F-SORT-02 / F-CULL-04 (M3)
    RatingFilter filter;
    QStringList nameTerms;                // F-NAV-08 (folded, NameFilter.h)
    QString keepPath;                     // stays visible although the filter does not match (current image)
    bool revalidate = false;              // explicit open: check a shared snapshot against the folder
    bool recursive = false;               // F-OPEN-08: the folder and all its subfolders
    bool pairing = false;                 // F-FMT-06: RAW+JPEG pairs as one entry
    QSet<QString> showRaw;                // F-FMT-06: pairs whose RAW is shown (by RAW path)
    QHash<QString, quint64> localChanges; // recursive: folder -> FolderCache::noteLocalChange()
    std::shared_ptr<LoadCounters> progress; // recursive: folders/images read so far
    int maxTreeImages = kMaxTreeImages;     // recursive: stop reading after this many images
    NameCollator* collator = nullptr;
    FolderOrderProvider* order = nullptr;
    FileAttributeProvider* attributes = nullptr;
    std::shared_ptr<void> keepAlive; // owns the default providers while the job runs

    bool cancelled() const { return latest->load(std::memory_order_relaxed) != generation; }
};

struct Result {
    quint64 generation = 0;
    bool cancelled = false;
    bool accessDenied = false;
    bool privacyDenied = false;
    QVector<FileEntry> entries;
    QVector<FileEntry> hidden; // F-CULL-04: not matching the rating filter
    SortSpec spec;
    JobKind kind = JobKind::Load;
    QStringList folders;                // F-OPEN-08: folders read (recursive), root first
    QHash<QString, FileEntry> partners; // F-FMT-06: pair members not shown (Load jobs)
    // Folder loads: FolderSnapshot::changeSeq of the snapshot used (max: not from a snapshot).
    quint64 changeSeq = std::numeric_limits<quint64>::max();
    bool truncated = false; // recursive: stopped at Job::maxTreeImages
    QStringList nameTerms;  // the name filter the result was filtered with
    QString containerError; // Mode::Container: why the archive shows no images
};

// F-NAV-08: whether the entry's name contains every term; folds the name once per entry.
bool nameMatches(FileEntry& e, const QStringList& terms) {
    if (terms.isEmpty())
        return true;
    if (e.foldedName.isEmpty())
        e.foldedName = namefilter::fold(e.name);
    return namefilter::matchesFolded(e.foldedName, terms);
}

// Fallback collator if no platform collator is available (should not happen).
class BinaryCollator : public NameCollator {
public:
    int compare(const QString& a, const QString& b) const override {
        const int c = a.compare(b, Qt::CaseInsensitive);
        return c ? c : a.compare(b);
    }
};

// F-OPEN-08: the folder and its subfolders, depth-first in Finder name order (r.folders).
// Every folder comes from the shared FolderCache (one enumeration per folder change). A
// snapshot older than an own edit in that folder is read again. False if cancelled.
bool readTree(const Job& job, Result& r, QVector<FileEntry>& entries) {
    BinaryCollator fallback;
    const NameCollator& coll = job.collator ? *job.collator : fallback;
    QStringList stack{job.folder};
    QSet<QString> seen;
    while (!stack.isEmpty()) {
        if (job.cancelled())
            return false;
        const QString f = stack.takeLast();
        if (seen.contains(f) || seen.size() >= kMaxTreeFolders)
            continue;
        seen.insert(f);
        r.folders.append(f);
        FolderCache& cache = FolderCache::instance();
        FolderSnapshotPtr snap = cache.get(f, job.attributes, job.revalidate);
        if (snap->changeSeq < job.localChanges.value(f, 0)) {
            cache.invalidate(f); // predates an own edit there (trash, rename …)
            snap = cache.get(f, job.attributes);
        }
        if (f == job.folder) {
            r.accessDenied = snap->accessDenied;
            r.privacyDenied = snap->privacyDenied;
        }
        entries += snap->entries;
        if (entries.size() >= job.maxTreeImages) {
            entries.resize(job.maxTreeImages); // F-OPEN-08: safety limit (status message)
            r.truncated = true;
            stack.clear();
        }
        if (job.progress) {
            job.progress->folders.fetch_add(1, std::memory_order_relaxed);
            job.progress->images.fetch_add(int(snap->entries.size()), std::memory_order_relaxed);
        }
        QStringList subs = snap->subfolders;
        // Pushed in reverse name order: the first subfolder is read next (depth-first).
        std::sort(subs.begin(), subs.end(), [&](const QString& a, const QString& b) {
            const int c = coll.compare(a, b);
            return c != 0 ? c > 0 : a.compare(b) > 0;
        });
        const QString prefix = f.endsWith(u'/') ? f : f + u'/';
        for (const QString& sub : std::as_const(subs))
            stack.append(prefix + sub);
    }
    return true;
}

// F-OPEN-08: groups the sorted entries by folder (in r.folders order), keeping the order
// within each folder.
void groupByFolder(QVector<FileEntry>& entries, const QStringList& folders) {
    QHash<QString, int> rank;
    for (int i = 0; i < folders.size(); ++i)
        rank.insert(folders[i], i);
    std::vector<int> keys(size_t(entries.size()));
    for (qsizetype i = 0; i < entries.size(); ++i)
        keys[size_t(i)] = rank.value(folderOf(entries[i].path), std::numeric_limits<int>::max());
    std::vector<qsizetype> order(size_t(entries.size()));
    std::iota(order.begin(), order.end(), qsizetype(0));
    std::stable_sort(order.begin(), order.end(),
                     [&](qsizetype a, qsizetype b) { return keys[size_t(a)] < keys[size_t(b)]; });
    QVector<FileEntry> out;
    out.reserve(entries.size());
    for (qsizetype i : order)
        out.append(std::move(entries[i]));
    entries = std::move(out);
}

Result runJob(Job job) {
    Result r;
    r.generation = job.generation;
    r.kind = job.kind;
    auto cancelled = [&] {
        r.cancelled = job.cancelled();
        return r.cancelled;
    };
    QVector<FileEntry> entries;
    if (job.kind == JobKind::Resort) {
        entries = std::move(job.existing);
    } else if (job.mode == Mode::Container) {
        // F-OPEN-10: the archive's (or PDF's) images, already in member order.
        BinaryCollator fallbackCollator;
        const archive::Listing listing = archive::list(job.container);
        entries = archive::imageEntries(job.container, listing,
                                        job.collator ? *job.collator
                                                     : static_cast<NameCollator&>(fallbackCollator));
        if (!listing.ok)
            r.containerError = listing.error;
        else if (entries.isEmpty())
            r.containerError = archive::messageNoImages();
    } else if (job.mode == Mode::Files) {
        QSet<QString> seen;
        for (const QString& p : std::as_const(job.files)) {
            if (seen.contains(p))
                continue;
            seen.insert(p);
            if (auto e = statEntry(p))
                entries.append(std::move(*e));
        }
    } else {
        if (job.recursive && !job.folder.isEmpty()) {
            if (!readTree(job, r, entries)) {
                r.cancelled = true;
                return r;
            }
        } else {
            // F-WIN-09: one enumeration (+ attributes) per folder change, shared by all windows.
            // An explicit open checks the snapshot against the folder (volumes without change
            // notifications, e.g. network shares, would otherwise show stale contents).
            const FolderSnapshotPtr snap =
                FolderCache::instance().get(job.folder, job.attributes, job.revalidate);
            entries = snap->entries;
            r.changeSeq = snap->changeSeq;
            r.accessDenied = snap->accessDenied;
            r.privacyDenied = snap->privacyDenied;
        }
        if (!job.mustInclude.isEmpty()) {
            const bool present = std::any_of(entries.cbegin(), entries.cend(),
                                             [&](const FileEntry& e) { return e.path == job.mustInclude; });
            if (!present) {
                if (auto e = statEntry(job.mustInclude)) {
                    if (job.attributes)
                        job.attributes->fill(*e);
                    entries.append(std::move(*e));
                }
            }
        }
    }
    if (cancelled())
        return r;

    if (job.kind == JobKind::Load && job.pairing) // F-FMT-06
        collection::pairRawJpeg(entries, &r.partners, job.showRaw);

    if (job.kind == JobKind::Load && job.mode == Mode::Files && job.attributes) {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if ((i & 255) == 255 && cancelled())
                return r;
            job.attributes->fill(entries[i]);
        }
    }

    // F-SORT-06: a choice remembered for this folder wins over the window's sort mode.
    bool followFinder = job.followFinder;
    SortSpec spec = job.manualSpec;
    if (job.folderMemory && !fileLike(job.mode) && !job.folder.isEmpty()) {
        if (const std::optional<FolderSortChoice> remembered = job.folderMemory(job.folder)) {
            followFinder = remembered->followFinder;
            spec = remembered->manual;
            spec.source = SortSource::Manual;
        }
    }
    if (followFinder) {
        if (!job.folder.isEmpty() && job.order)
            spec = job.order->finderOrder(job.folder, job.allowLiveQuery);
        else
            spec = SortSpec{SortKey::Name, true, SortSource::Fallback};
    }
    if (cancelled())
        return r;

    if (needsImageProperties(spec.key) && job.attributes && job.mode != Mode::Container) {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if ((i & 31) == 31 && cancelled())
                return r;
            if (!entries[i].imagePropertiesRead)
                job.attributes->fillImageProperties(entries[i]);
        }
        if (cancelled())
            return r;
    }

    // M3: ratings from the RatingStore for sorting by rating and for the filter.
    const bool ratingFiltering = job.ratingReader && job.filter.isActive();
    const bool filtering = ratingFiltering || !job.nameTerms.isEmpty(); // F-NAV-08
    if (job.ratingReader && job.mode != Mode::Container && (spec.key == SortKey::Rating || ratingFiltering)) {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if ((i & 31) == 31 && cancelled())
                return r;
            if (!entries[i].ratingRead)
                collection::setRating(entries[i], job.ratingReader(entries[i].path));
        }
        if (cancelled())
            return r;
    }
    if (spec.key == SortKey::FinderIconPosition) {
        QHash<QString, QPointF> pos;
        if (!job.folder.isEmpty() && job.order)
            pos = job.order->iconPositions(job.folder);
        for (FileEntry& e : entries) {
            auto it = pos.constFind(e.name);
            if (it == pos.cend())
                it = pos.constFind(e.name.normalized(QString::NormalizationForm_C));
            if (it != pos.cend() && parentOf(e.path) == job.folder)
                e.finderIconPosition = *it;
            else
                e.finderIconPosition.reset();
        }
    }
    BinaryCollator fallback;
    if (job.mode == Mode::Container && spec.key == SortKey::Name) {
        // F-OPEN-10: member paths in Finder order (folders inside the archive together);
        // PDF pages in page order.
        if (vpath::containerKind(job.container) == vpath::ContainerKind::Pdf) {
            std::sort(entries.begin(), entries.end(), [&](const FileEntry& a, const FileEntry& b) {
                const int pa = vpath::pdfPageIndex(vpath::split(a.path)->member);
                const int pb = vpath::pdfPageIndex(vpath::split(b.path)->member);
                return spec.ascending ? pa < pb : pa > pb;
            });
        } else {
            archive::sortMemberEntries(entries, job.collator ? *job.collator : fallback, spec.ascending);
        }
    } else {
        collection::sortEntries(entries, spec, job.collator ? *job.collator : fallback);
    }
    if (job.recursive && !fileLike(job.mode)) {
        // F-OPEN-08: folder by folder; a resort works on the folders of the last load.
        if (job.kind == JobKind::Resort) {
            QStringList folders;
            QSet<QString> seen;
            for (const FileEntry& e : std::as_const(entries))
                if (const QString f = folderOf(e.path); !seen.contains(f)) {
                    seen.insert(f);
                    folders << f;
                }
            const NameCollator& coll = job.collator ? *job.collator : fallback;
            std::sort(folders.begin(), folders.end(), [&](const QString& a, const QString& b) {
                return collection::compareFolderPaths(a, b, coll) < 0;
            });
            groupByFolder(entries, folders);
        } else {
            groupByFolder(entries, r.folders);
        }
    }
    if (cancelled())
        return r;
    // Shown and filtered-out entries are sorted together: their ranks let the name filter
    // show and hide entries later without sorting again (F-NAV-08).
    for (qsizetype i = 0; i < entries.size(); ++i)
        entries[i].sortRank = int(i);
    if (filtering) {
        QVector<FileEntry> shown;
        shown.reserve(entries.size());
        for (FileEntry& e : entries) {
            const bool ratingOk =
                !ratingFiltering || job.filter.matches(collection::ratingOf(e)) || e.path == job.keepPath;
            if (ratingOk && nameMatches(e, job.nameTerms))
                shown.append(std::move(e));
            else
                r.hidden.append(std::move(e));
        }
        entries = std::move(shown);
    }
    r.entries = std::move(entries);
    r.spec = spec;
    r.nameTerms = job.nameTerms;
    return r;
}

// Sorted position: folder first (recursive collections), then the sort spec.
bool entryLessFor(const FileEntry& a, const FileEntry& b, const SortSpec& spec, const NameCollator& coll,
                  bool byFolder) {
    if (byFolder) {
        if (const int c = collection::compareFolderPaths(folderOf(a.path), folderOf(b.path), coll))
            return c < 0;
    }
    return collection::compareEntries(a, b, spec, coll) < 0;
}

// ---- F-NAV-08: the name filter as a pure function (GUI thread or worker) ----

struct NameFilterInput {
    QVector<FileEntry> entries;       // shown, in order
    QHash<QString, FileEntry> hidden; // filtered out (rating and/or name)
    QStringList terms;
    bool ratingActive = false;
    RatingFilter filter;
    QString stale;
    SortSpec spec;
    NameCollator* collator = nullptr;
    bool byFolder = false;
    std::shared_ptr<void> keepAlive; // default providers (collator)
};

struct NameFilterOutput {
    QVector<FileEntry> entries;
    QHash<QString, FileEntry> hidden;
    QHash<QString, int> index;
    bool staleLeft = false; // the stale entry was hidden
};

// Entries that no longer match leave, hidden ones that match (name and rating) come back.
// Filtering keeps the order: entries return at the position of their last full sort
// (FileEntry::sortRank) - no comparisons with the collator unless ranks are missing.
NameFilterOutput filterByName(NameFilterInput in) {
    NameFilterOutput out;
    QVector<FileEntry> kept;
    kept.reserve(in.entries.size());
    for (FileEntry& e : in.entries) {
        if (nameMatches(e, in.terms)) {
            kept.append(std::move(e));
        } else {
            if (!in.stale.isEmpty() && e.path == in.stale)
                out.staleLeft = true;
            const QString p = e.path;
            in.hidden.insert(p, std::move(e));
        }
    }
    QVector<FileEntry> back;
    for (auto it = in.hidden.begin(); it != in.hidden.end();) {
        FileEntry& e = it.value();
        const bool ratingOk = !in.ratingActive || !e.ratingRead || in.filter.matches(collection::ratingOf(e));
        if (ratingOk && nameMatches(e, in.terms)) {
            back.append(std::move(e));
            it = in.hidden.erase(it);
        } else {
            ++it;
        }
    }
    bool ranked = std::all_of(back.cbegin(), back.cend(), [](const FileEntry& e) { return e.sortRank >= 0; });
    for (qsizetype i = 0; ranked && i < kept.size(); ++i)
        ranked = kept[i].sortRank >= 0 && (i == 0 || kept[i - 1].sortRank < kept[i].sortRank);
    out.entries.reserve(kept.size() + back.size());
    if (ranked) {
        auto byRank = [](const FileEntry& a, const FileEntry& b) { return a.sortRank < b.sortRank; };
        std::sort(back.begin(), back.end(), byRank);
        std::merge(std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()),
                   std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()),
                   std::back_inserter(out.entries), byRank);
    } else {
        BinaryCollator fallback;
        const NameCollator& coll = in.collator ? *in.collator : fallback;
        auto less = [&](const FileEntry& a, const FileEntry& b) {
            return entryLessFor(a, b, in.spec, coll, in.byFolder);
        };
        std::sort(back.begin(), back.end(), less);
        std::merge(std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()),
                   std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()),
                   std::back_inserter(out.entries), less);
    }
    out.index.reserve(out.entries.size());
    for (int i = 0; i < out.entries.size(); ++i)
        out.index.insert(out.entries[i].path, i);
    out.hidden = std::move(in.hidden);
    return out;
}

// ---- F-OPEN-08: incremental update of a recursive collection (code review M3 #16) ----

// The new contents of one changed folder (or of a new subtree, or a folder that is gone).
struct FolderContent {
    QString folder;
    bool removed = false;               // gone: drop it and every folder below it
    QStringList folders;                // folders these entries come from (subtree: all)
    QVector<FileEntry> shown;           // sorted (subtree: grouped by folder)
    QVector<FileEntry> hidden;          // filtered out
    QHash<QString, FileEntry> partners; // F-FMT-06
};

struct DeltaJob {
    Job base;               // filters, sort spec, providers of the collection
    QStringList folders;    // changed folders
    QSet<QString> known;    // folders of the last load
    quint64 generation = 0; // Private::deltaGeneration
    quint64 version = 0;    // Private::version when the job started
};

struct DeltaResult {
    quint64 generation = 0;
    quint64 version = 0;
    QStringList folders; // as requested
    QVector<FolderContent> contents;
    bool fullReload = false; // the root itself is gone or unreadable
};

// Reads only the changed folders (each one from the shared FolderCache) and the subfolders
// that are new; subfolders that disappeared are reported as removed.
DeltaResult runDelta(DeltaJob dj) {
    DeltaResult out;
    out.generation = dj.generation;
    out.version = dj.version;
    out.folders = dj.folders;
    QSet<QString> handled;
    for (const QString& f : std::as_const(dj.folders)) {
        if (dj.base.cancelled())
            return out;
        if (handled.contains(f))
            continue;
        handled.insert(f);
        if (!QFileInfo(f).isDir()) {
            if (f == dj.base.folder) {
                out.fullReload = true;
                return out;
            }
            FolderContent gone;
            gone.folder = f;
            gone.removed = true;
            out.contents.append(gone);
            continue;
        }
        FolderCache& cache = FolderCache::instance();
        FolderSnapshotPtr snap = cache.get(f, dj.base.attributes);
        if (snap->changeSeq < dj.base.localChanges.value(f, 0)) {
            cache.invalidate(f); // predates an own edit there
            snap = cache.get(f, dj.base.attributes);
        }
        if (f == dj.base.folder && snap->accessDenied) {
            out.fullReload = true;
            return out;
        }
        Job j = dj.base;
        j.kind = JobKind::Load;
        j.folder = f;
        j.recursive = false;
        j.followFinder = false; // the collection's current (resolved) spec
        j.folderMemory = {};
        j.revalidate = false;
        j.progress.reset();
        if (f != dj.base.folder) {
            j.mustInclude.clear();
            j.order = nullptr; // Finder icon positions belong to the opened folder only
        }
        Result r = runJob(j);
        if (r.cancelled)
            return out;
        FolderContent c;
        c.folder = f;
        c.folders = {f};
        c.shown = std::move(r.entries);
        c.hidden = r.hidden;
        c.partners = r.partners;
        out.contents.append(std::move(c));
        // Subfolders: new ones are read with everything below them, vanished ones leave.
        const QString prefix = f.endsWith(u'/') ? f : f + u'/';
        QSet<QString> present;
        for (const QString& sub : std::as_const(snap->subfolders)) {
            const QString path = prefix + sub;
            present.insert(path);
            if (dj.known.contains(path) || handled.contains(path))
                continue;
            handled.insert(path);
            Job t = j;
            t.folder = path;
            t.recursive = true;
            t.mustInclude.clear();
            t.order = nullptr;
            Result rt = runJob(t);
            if (rt.cancelled)
                return out;
            FolderContent n;
            n.folder = path;
            n.folders = rt.folders;
            n.shown = std::move(rt.entries);
            n.hidden = rt.hidden;
            n.partners = rt.partners;
            out.contents.append(std::move(n));
        }
        for (const QString& k : std::as_const(dj.known)) {
            if (k != f && folderOf(k) == f && !present.contains(k) && !handled.contains(k)) {
                handled.insert(k);
                FolderContent gone;
                gone.folder = k;
                gone.removed = true;
                out.contents.append(gone);
            }
        }
    }
    return out;
}

} // namespace

struct Collection::Private {
    Collection* q = nullptr;
    Mode mode = Mode::None;
    QString folder;
    QString openedFile;
    QStringList files;
    QSet<QString> fileSet; // Mode::Files: every file the collection was opened with (F-OPEN-04)
    QString container;     // Mode::Container: the archive or PDF (F-OPEN-10)
    QSet<QString> leased;  // folders leased in the FolderCache (the folder; recursive: the tree)
    QVector<FileEntry> entries;
    QHash<QString, int> indexByPath;
    int current = -1;
    bool loading = false;
    bool selectFirstOnLoad = false;
    bool selectLastOnLoad = false;
    // insertPath(): path -> token of the outstanding attribute read (latest wins).
    QHash<QString, quint64> pendingInserts;
    quint64 insertToken = 0;
    // FolderCache::noteLocalChange() of this collection's last own edit (removePath,
    // insertPath, renamePath): a folder snapshot enumerated before it is outdated.
    quint64 lastLocalChange = 0;

    // `path`: the file that changed (recursive collections note the change for its folder).
    void noteLocalChange(const QString& path = QString()) {
        if ((mode != Mode::File && mode != Mode::Folder) || folder.isEmpty())
            return;
        if (recursive) {
            const QString f = path.isEmpty() ? folder : folderOf(path);
            localChanges.insert(f, FolderCache::instance().noteLocalChange(f));
            return;
        }
        lastLocalChange = FolderCache::instance().noteLocalChange(folder);
    }

    // ---- M3: subfolders (F-OPEN-08) and RAW+JPEG pairs (F-FMT-06) ----
    bool recursive = false;
    QStringList treeFolders;              // folders of the last recursive load, root first
    QHash<QString, quint64> localChanges; // recursive: folder -> noteLocalChange()
    std::shared_ptr<LoadCounters> progress = std::make_shared<LoadCounters>();
    QTimer* progressTimer = nullptr;
    bool pairing = false;
    QHash<QString, FileEntry> partners; // pair members not shown, by path
    QSet<QString> showRaw;              // pairs whose RAW is shown (RAW paths)

    // `parent` belongs to this folder collection. Recursive: a folder of the last load, or a
    // new folder below the root that the tree reader would take (code review M3 #16: not
    // hidden, no package such as "Fotos.photoslibrary" whose internals change constantly,
    // same volume). Unknown folders are checked once (lstat, cached).
    bool coversFolder(const QString& parent) const {
        if (parent == folder)
            return true;
        if (!recursive || folder.isEmpty())
            return false;
        const QString prefix = folder.endsWith(u'/') ? folder : folder + u'/';
        if (!parent.startsWith(prefix))
            return false;
        if (treeSet.contains(parent))
            return true;
        QString path = folder;
        for (const QString& part : parent.mid(prefix.size()).split(u'/', Qt::SkipEmptyParts)) {
            path = (path.endsWith(u'/') ? path : path + u'/') + part;
            if (treeSet.contains(path) || acceptedFolders.contains(path))
                continue;
            if (part.startsWith(u'.') || excludedFolders.contains(path))
                return false;
            switch (treeFolderState(path)) {
            case FolderState::Accepted:
                acceptedFolders.insert(path);
                break;
            case FolderState::Excluded:
                excludedFolders.insert(path);
                return false;
            case FolderState::Missing:
                return false;
            }
        }
        return true;
    }
    enum class FolderState { Accepted, Excluded, Missing };
    // Like the tree reader's acceptSubfolder(): a real folder, not hidden, no package, on
    // the volume of its parent.
    static FolderState treeFolderState(const QString& path) {
#ifdef Q_OS_UNIX
        struct stat st{}, parentSt{};
        if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
            return FolderState::Missing;
        if (!S_ISDIR(st.st_mode))
            return FolderState::Excluded;
#ifdef UF_HIDDEN
        if (st.st_flags & UF_HIDDEN)
            return FolderState::Excluded;
#endif
        if (::stat(QFile::encodeName(parentOf(path)).constData(), &parentSt) == 0 &&
            parentSt.st_dev != st.st_dev)
            return FolderState::Excluded;
#else
        if (!QFileInfo(path).isDir())
            return FolderState::Missing;
#endif
        if (path.lastIndexOf(u'.') > path.lastIndexOf(u'/') && isPackageFolder(QFileInfo(path)))
            return FolderState::Excluded;
        return FolderState::Accepted;
    }

    // Sorted position: folder first (recursive), then the sort spec.
    bool entryLess(const FileEntry& a, const FileEntry& b, const SortSpec& sortSpec,
                   const NameCollator& coll) const {
        return entryLessFor(a, b, sortSpec, coll, recursive && !fileLike(mode));
    }

    // F-FMT-06: candidates for a file that may complete a pair: pair key -> path of a shown
    // JPEG/RAW (code review M3 #19: O(1) instead of a scan). Entries added later are added
    // here; removed ones are skipped on lookup (indexByPath).
    QMultiHash<QString, QString> pairIndex;
    void indexPairCandidate(const FileEntry& e) {
        if (!pairing || !e.pairedPath.isEmpty())
            return;
        const QString sfx = collection::suffixOf(e.path);
        if (!collection::isPairJpegSuffix(sfx) && !isRawSuffix(sfx))
            return;
        if (const QString key = collection::pairKey(e.path); !key.isEmpty())
            pairIndex.insert(key, e.path);
    }
    void rebuildPairIndex() {
        pairIndex.clear();
        if (!pairing)
            return;
        for (const FileEntry& e : std::as_const(entries))
            indexPairCandidate(e);
    }
    // Pairs `e` with a shown entry it completes, without signals. Returns the index of that
    // entry (its contents may have been replaced by `e`), -1 if `e` pairs with nothing.
    int pairWithShown(FileEntry& e) {
        if (!pairing)
            return -1;
        const QString key = collection::pairKey(e.path);
        if (key.isEmpty())
            return -1;
        for (auto it = pairIndex.constFind(key); it != pairIndex.cend() && it.key() == key; ++it) {
            const int i = indexByPath.value(it.value(), -1);
            if (i < 0 || i >= entries.size() || entries[i].path != it.value())
                continue;
            FileEntry& other = entries[i];
            if (!other.pairedPath.isEmpty() || !collection::isPairPartner(other.path, e.path))
                continue;
            const bool eIsJpeg = collection::isPairJpegSuffix(QFileInfo(e.path).suffix().toLower()) &&
                                 !showRaw.contains(other.path);
            if (!eIsJpeg) { // the RAW joins the shown JPEG
                e.pairedPath = other.path;
                other.pairedPath = e.path;
                partners.insert(e.path, e);
                return i;
            }
            // The JPEG arrives for a shown RAW: the JPEG is shown, the RAW becomes its partner.
            FileEntry raw = other;
            raw.pairedPath = e.path;
            e.pairedPath = raw.path;
            partners.insert(raw.path, raw);
            other = e;
            other.sortRank = -1;
            return i;
        }
        return -1;
    }

    // F-FMT-06: a file that arrives (insertPath) may complete a pair with a shown entry.
    // True if `e` was attached (no new entry needed).
    bool attachToPair(FileEntry& e) {
        const FileEntry arrived = e;
        const QString shownBefore =
            current >= 0 && current < entries.size() ? entries[current].path : QString();
        const int i = pairWithShown(e);
        if (i < 0)
            return false;
        ++version;
        if (watcher) {
            // A running load works on a snapshot that may predate this file: replay it.
            removedDuringJob.remove(arrived.path);
            insertedDuringJob.insert(arrived.path, arrived);
        }
        const bool replaced = entries[i].path == e.path; // the JPEG took the shown RAW's place
        if (replaced) {
            rebuildIndex();
            indexPairCandidate(entries[i]);
        }
        emit q->entriesChanged();
        if (replaced && i == current && shownBefore != entries[i].path)
            emit q->currentChanged(current, entries[i].path);
        return true;
    }

    SortSpec spec{SortKey::Name, true, SortSource::Fallback};
    SortSpec manualSpec{SortKey::Name, true, SortSource::Manual};
    bool followFinder = true;
    bool allowLiveQuery = false;
    FolderSortMemory folderMemory;
    // M3 ratings: reader, filter (F-CULL-04), entries hidden by it, and the current image
    // whose new rating no longer matches (hidden once another image becomes current).
    RatingReader ratingReader;
    RatingFilter filter;
    QHash<QString, FileEntry> hidden;
    QString stale;
    QString nameFilter;          // F-NAV-08: as typed
    QStringList nameTerms;       // folded terms of nameFilter
    bool revalidateNext = false; // the next load comes from an explicit open
    // replayEditsDuringJob() inserted entries whose image properties are still unknown
    // (needed by the sort key): a resort on a worker reads them (never the GUI thread).
    bool resortForProperties = false;

    // ---- code review M3 #16/#17 ----
    quint64 version = 0;                   // bumped by every change of entries/hidden/partners
    QSet<QString> treeSet;                 // treeFolders as a set (coversFolder)
    mutable QSet<QString> acceptedFolders; // new folders below the root, checked once
    mutable QSet<QString> excludedFolders; // ... skipped by the tree reader (package, hidden)
    QSet<QString> pendingFolders;          // changed folders waiting for the debounce timer
    QTimer* changeTimer = nullptr;
    quint64 deltaGeneration = 0;
    bool deltaRunning = false;
    quint64 nameFilterGeneration = 0;
    int maxTreeImages = kMaxTreeImages;

    std::shared_ptr<std::atomic<quint64>> latest = std::make_shared<std::atomic<quint64>>(0);
    quint64 generation = 0;
    QPointer<QFutureWatcher<Result>> watcher;
    // removePath()/insertPath() calls while a job runs: the job works on a snapshot taken
    // before them, so apply() replays them onto its result.
    QSet<QString> removedDuringJob;
    QHash<QString, FileEntry> insertedDuringJob;

    // Default providers; shared with running jobs so they outlive the Collection if needed.
    struct OwnProviders {
        std::unique_ptr<NameCollator> collator;
        std::unique_ptr<FolderOrderProvider> order;
        std::unique_ptr<FileAttributeProvider> attributes;
    };
    std::shared_ptr<OwnProviders> own;
    NameCollator* collator = nullptr;
    FolderOrderProvider* order = nullptr;
    FileAttributeProvider* attributes = nullptr;

    // FolderCache leases: the shown folder of a folder collection (recursive: and the
    // subfolders of the last load, so their snapshots are shared and kept, F-WIN-09).
    void updateLease() {
        QSet<QString> want;
        if ((mode == Mode::File || mode == Mode::Folder) && !folder.isEmpty()) {
            want.insert(folder);
            if (recursive)
                for (const QString& f : std::as_const(treeFolders))
                    want.insert(f);
        }
        if (want == leased)
            return;
        FolderCache& cache = FolderCache::instance();
        for (const QString& f : std::as_const(want))
            if (!leased.contains(f))
                cache.acquire(f);
        for (const QString& f : std::as_const(leased))
            if (!want.contains(f))
                cache.release(f);
        leased = std::move(want);
    }

    void rebuildIndex() {
        ++version;
        indexByPath.clear();
        indexByPath.reserve(entries.size());
        for (int i = 0; i < entries.size(); ++i)
            indexByPath.insert(entries[i].path, i);
    }

    void cancelPending() {
        ++generation;
        latest->store(generation);
        if (watcher) {
            watcher->disconnect(q);
            watcher->deleteLater();
            watcher = nullptr;
        }
    }

    Job makeJob(JobKind kind) {
        Job j;
        j.kind = kind;
        j.generation = generation;
        j.latest = latest;
        j.mode = mode;
        j.folder = folder;
        j.mustInclude = mode == Mode::File ? openedFile : QString();
        j.files = files;
        j.container = container;
        if (kind == JobKind::Resort) {
            // The filtered-out entries are sorted along (they keep their place for the
            // filters, and are not lost by a resort).
            j.existing = entries;
            for (auto it = hidden.cbegin(); it != hidden.cend(); ++it)
                j.existing.append(it.value());
        }
        j.maxTreeImages = maxTreeImages;
        j.followFinder = followFinder;
        j.allowLiveQuery = allowLiveQuery;
        j.manualSpec = manualSpec;
        j.folderMemory = folderMemory;
        j.ratingReader = ratingReader;
        j.filter = filter;
        j.nameTerms = nameTerms;
        {
            const QString cur = current >= 0 && current < entries.size() ? entries[current].path : QString();
            if (!stale.isEmpty())
                j.keepPath = stale;
            else if (!cur.isEmpty() && cur == openedFile)
                j.keepPath = cur; // the explicitly opened file while it is shown (F-OPEN-02)
        }
        j.revalidate = kind == JobKind::Load && revalidateNext;
        revalidateNext = false;
        j.recursive = recursive && !fileLike(mode);
        j.pairing = pairing;
        j.showRaw = showRaw;
        if (const QString cur = current >= 0 && current < entries.size() ? entries[current].path : QString();
            !cur.isEmpty() && !entries[current].pairedPath.isEmpty() &&
            !collection::isPairJpegSuffix(QFileInfo(cur).suffix().toLower()))
            j.showRaw.insert(cur); // the shown RAW of a pair stays shown
        if (mode == Mode::File && !collection::isPairJpegSuffix(QFileInfo(openedFile).suffix().toLower()))
            j.showRaw.insert(openedFile); // an explicitly opened RAW stays shown (F-OPEN-02)
        j.localChanges = localChanges;
        if (kind == JobKind::Load && j.recursive) {
            progress = std::make_shared<LoadCounters>();
            j.progress = progress;
        }
        j.collator = collator;
        j.order = order;
        j.attributes = attributes;
        j.keepAlive = own;
        return j;
    }

    void start(JobKind kind) {
        cancelPending();
        removedDuringJob.clear();
        insertedDuringJob.clear();
        ++deltaGeneration; // a running incremental update is superseded
        deltaRunning = false;
        if (kind == JobKind::Load) {
            pendingFolders.clear(); // the load reads them
            if (changeTimer)
                changeTimer->stop();
        }
        if (mode == Mode::None)
            return;
        loading = true;
        if (kind == JobKind::Load && recursive && !fileLike(mode) && progressTimer)
            progressTimer->start(); // F-OPEN-08: progress for the status bar
        auto* w = new QFutureWatcher<Result>(q);
        watcher = w;
        QObject::connect(w, &QFutureWatcher<Result>::finished, q, [this, w] {
            const Result r = w->result();
            w->deleteLater();
            if (watcher == w)
                watcher = nullptr;
            apply(r);
        });
        w->setFuture(QtConcurrent::run(runJob, makeJob(kind)));
    }

    int neighbourIndex(const QVector<FileEntry>& old, int oldIndex) const {
        if (entries.isEmpty())
            return -1;
        if (oldIndex < 0 || oldIndex >= old.size())
            return 0;
        for (int i = oldIndex + 1; i < old.size(); ++i) {
            if (const auto it = indexByPath.constFind(old[i].path); it != indexByPath.cend())
                return *it;
        }
        for (int i = oldIndex - 1; i >= 0; --i) {
            if (const auto it = indexByPath.constFind(old[i].path); it != indexByPath.cend())
                return *it;
        }
        return 0;
    }

    void apply(const Result& r) {
        if (r.generation != generation || r.cancelled)
            return;
        if (r.changeSeq < lastLocalChange) {
            // The snapshot was enumerated before a file operation of this app changed the
            // folder (e.g. a watcher reload that shared an older enumeration): it may bring
            // back a just-trashed file. Read again (a new enumeration always carries a
            // newer changeSeq, so this happens at most once per edit).
            if (!folder.isEmpty())
                FolderCache::instance().invalidate(folder);
            start(JobKind::Load);
            return;
        }
        const QString oldPath = current >= 0 && current < entries.size() ? entries[current].path : QString();
        const int oldIndex = current;
        const QVector<FileEntry> old = std::move(entries);
        entries = r.entries;
        bool foldersDiffer = false;
        if (r.kind == JobKind::Load) {
            partners = r.partners; // F-FMT-06
            acceptedFolders.clear();
            excludedFolders.clear();
            if (recursive && !fileLike(mode) && treeFolders != r.folders) {
                treeFolders = r.folders; // F-OPEN-08
                treeSet = QSet<QString>(treeFolders.cbegin(), treeFolders.cend());
                updateLease();
                foldersDiffer = true;
            }
        }
        if (progressTimer)
            progressTimer->stop();
        hidden.clear();
        for (const FileEntry& e : r.hidden)
            if (!removedDuringJob.contains(e.path))
                hidden.insert(e.path, e);
        if (!stale.isEmpty() && !std::any_of(entries.cbegin(), entries.cend(),
                                             [&](const FileEntry& e) { return e.path == stale; }))
            stale.clear();
        rebuildIndex();
        rebuildPairIndex();
        replayEditsDuringJob(r.spec);
        rebuildIndex();
        if (selectFirstOnLoad) {
            current = entries.isEmpty() ? -1 : 0;
            selectFirstOnLoad = false;
        } else if (selectLastOnLoad) {
            current = int(entries.size()) - 1;
            selectLastOnLoad = false;
        } else {
            auto it = indexByPath.constFind(oldPath);
            if (it == indexByPath.cend() && partners.contains(oldPath)) // F-FMT-06: now a partner
                it = indexByPath.constFind(partners.value(oldPath).pairedPath);
            current = it != indexByPath.cend() ? *it : neighbourIndex(old, oldIndex);
        }
        const bool specChanged = !(spec == r.spec);
        spec = r.spec;
        loading = false;
        emit q->entriesChanged();
        const QString newPath = current >= 0 ? entries[current].path : QString();
        if (current != oldIndex || newPath != oldPath)
            emit q->currentChanged(current, newPath);
        if (specChanged)
            emit q->sortSpecChanged(spec);
        if (r.accessDenied)
            emit q->accessDenied(folder, r.privacyDenied);
        if (foldersDiffer)
            emit q->foldersChanged();
        if (r.truncated)
            emit q->treeLimitReached(int(entries.size() + hidden.size()));
        if (!r.containerError.isEmpty() && r.kind == JobKind::Load)
            emit q->containerFailed(container, r.containerError); // F-OPEN-10
        if (r.nameTerms != nameTerms)
            scheduleNameFilter(); // typed while the job ran (it filtered with older terms)
        if (resortForProperties) {
            // Entries added meanwhile need image metadata for this sort key: read on a
            // worker by a resort; loading finishes with it.
            resortForProperties = false;
            start(JobKind::Resort);
            return;
        }
        emit q->loadingFinished();
    }

    // Re-applies removals/insertions made while the job ran (its snapshot predates them).
    void replayEditsDuringJob(const SortSpec& sortSpec) {
        for (const QString& p : std::as_const(removedDuringJob)) {
            // F-FMT-06: a pair member removed while the job ran no longer pairs.
            if (auto it = partners.find(p); it != partners.end()) {
                const QString shown = it->pairedPath;
                partners.erase(it);
                for (FileEntry& e : entries)
                    if (e.path == shown)
                        e.pairedPath.clear();
            }
        }
        // F-FMT-06 (code review M3 #19): the shown member of a pair was removed while the job
        // ran: its partner takes the place (as removePath() did on the old entries).
        for (const QString& p : std::as_const(removedDuringJob)) {
            const int i = indexByPath.value(p, -1);
            if (i < 0 || i >= entries.size() || entries[i].pairedPath.isEmpty())
                continue;
            auto pit = partners.find(entries[i].pairedPath);
            if (pit == partners.end() || removedDuringJob.contains(pit.key()))
                continue;
            FileEntry other = *pit;
            partners.erase(pit);
            other.pairedPath.clear();
            showRaw.remove(other.path);
            showRaw.remove(p);
            entries[i] = std::move(other);
        }
        if (!removedDuringJob.isEmpty()) {
            entries.removeIf([&](const FileEntry& e) { return removedDuringJob.contains(e.path); });
            files.removeIf([&](const QString& p) { return removedDuringJob.contains(p); });
            rebuildIndex();
        }
        if (!insertedDuringJob.isEmpty()) {
            QSet<QString> present;
            for (const FileEntry& e : std::as_const(entries))
                present.insert(e.path);
            BinaryCollator fallback;
            const NameCollator& coll = collator ? *collator : fallback;
            for (auto it = insertedDuringJob.begin(); it != insertedDuringJob.end(); ++it) {
                if (present.contains(it.key()) || partners.contains(it.key()))
                    continue;
                FileEntry e = it.value();
                e.sortRank = -1;
                if (pairWithShown(e) >= 0) // F-FMT-06: completes a pair of the result
                    continue;
                // Image metadata is read by a resort on a worker (apply()), never here on
                // the GUI thread; until then the entry sorts with the unknown values.
                if (needsImageProperties(sortSpec.key) && attributes && !e.imagePropertiesRead)
                    resortForProperties = true;
                const auto pos = std::upper_bound(
                    entries.begin(), entries.end(), e,
                    [&](const FileEntry& a, const FileEntry& b) { return entryLess(a, b, sortSpec, coll); });
                const qsizetype at = pos - entries.begin();
                entries.insert(at, std::move(e));
                rebuildIndex();
                indexPairCandidate(entries[at]);
            }
        }
        removedDuringJob.clear();
        insertedDuringJob.clear();
    }

    void setCurrent(int index) {
        if (index == current)
            return;
        current = index;
        const QString path = current >= 0 ? entries[current].path : QString();
        if (!stale.isEmpty() && stale != path)
            dropStale(path);
        emit q->currentChanged(current, path);
    }

    // F-CULL-04: the previously current image no longer matches the filter: hide it now.
    void dropStale(const QString& currentPath) {
        const QString s = std::exchange(stale, QString());
        const auto it = indexByPath.constFind(s);
        if (it == indexByPath.cend())
            return;
        const int i = *it;
        hidden.insert(s, entries[i]);
        entries.removeAt(i);
        rebuildIndex();
        current = currentPath.isEmpty() ? -1 : indexByPath.value(currentPath, -1);
        emit q->entriesChanged();
    }

    bool hiddenByFilter(const FileEntry& e) const {
        return (ratingReader && filter.isActive() && e.ratingRead &&
                !filter.matches(collection::ratingOf(e))) ||
               !namefilter::matchesTerms(e.name, nameTerms);
    }
    // F-NAV-08: a changed name filter applied to the entries in memory (no new job): on
    // the GUI thread for small collections, else on a worker (the result is taken if
    // nothing changed meanwhile). A running load is never restarted for it.
    void scheduleNameFilter();
    void applyNameFilterOutput(NameFilterOutput out);
    // Code review M3 #16: coalesced folder changes; incremental update of a recursive tree.
    void processPendingChanges();
    void startDelta(const QStringList& folders);
    void applyDelta(const DeltaResult& r);

    void reset(Mode m) {
        cancelPending();
        mode = m;
        folder.clear();
        container.clear();
        openedFile.clear();
        files.clear();
        fileSet.clear();
        entries.clear();
        indexByPath.clear();
        current = -1;
        selectFirstOnLoad = false;
        selectLastOnLoad = false;
        loading = false;
        removedDuringJob.clear();
        insertedDuringJob.clear();
        pendingInserts.clear();
        hidden.clear();
        stale.clear();
        resortForProperties = false;
        revalidateNext = false;
        lastLocalChange = 0; // edits of the previous contents do not concern the new ones
        localChanges.clear();
        treeFolders.clear();
        treeSet.clear();
        acceptedFolders.clear();
        excludedFolders.clear();
        partners.clear();
        pairIndex.clear();
        showRaw.clear();
        pendingFolders.clear();
        ++deltaGeneration;
        deltaRunning = false;
        ++nameFilterGeneration;
        ++version;
        if (changeTimer)
            changeTimer->stop();
        if (progressTimer)
            progressTimer->stop();
    }

    // Inserts a stat'ed entry at its sorted position (GUI thread).
    void insertEntry(FileEntry e, bool makeCurrent) {
        const QString path = e.path;
        e.sortRank = -1; // not part of the last full sort
        ++version;
        if (indexByPath.contains(path)) {
            if (makeCurrent)
                setCurrent(indexByPath.value(path));
            return;
        }
        noteLocalChange(path);
        if (partners.contains(path)) { // already part of a pair (F-FMT-06)
            if (makeCurrent)
                setCurrent(indexByPath.value(partners.value(path).pairedPath, current));
            return;
        }
        if (attachToPair(e))
            return;
        if (watcher) {
            removedDuringJob.remove(path);
            insertedDuringJob.insert(path, e);
        }
        if (mode == Mode::Files && !files.contains(path))
            files.append(path);
        if (hiddenByFilter(e) && !makeCurrent) {
            hidden.insert(path, std::move(e)); // F-CULL-04
            return;
        }
        BinaryCollator fallback;
        const NameCollator& coll = collator ? *collator : fallback;
        const auto pos =
            std::upper_bound(entries.begin(), entries.end(), e, [&](const FileEntry& a, const FileEntry& b) {
                return entryLess(a, b, spec, coll);
            });
        const int at = int(pos - entries.begin());
        entries.insert(at, std::move(e));
        rebuildIndex();
        indexPairCandidate(entries[at]);
        const int oldCurrent = current;
        if (current >= at)
            ++current;
        if (makeCurrent)
            current = at;
        emit q->entriesChanged();
        if (current != oldCurrent || makeCurrent)
            emit q->currentChanged(current, current >= 0 ? entries[current].path : QString());
    }
};

Collection::Collection(QObject* parent) : QObject(parent), d(new Private) {
    d->q = this;
    d->own = std::make_shared<Private::OwnProviders>();
    d->own->collator = createNameCollator();
    d->own->order = createFolderOrderProvider();
    d->own->attributes = createFileAttributeProvider();
    d->collator = d->own->collator.get();
    d->order = d->own->order.get();
    d->attributes = d->own->attributes.get();
    d->progressTimer = new QTimer(this); // F-OPEN-08: polls the worker's counters
    d->progressTimer->setInterval(250);
    connect(d->progressTimer, &QTimer::timeout, this, [this] {
        emit loadProgress(d->progress->folders.load(std::memory_order_relaxed),
                          d->progress->images.load(std::memory_order_relaxed));
    });
    d->changeTimer = new QTimer(this); // code review M3 #16: coalesces folder changes
    d->changeTimer->setSingleShot(true);
    d->changeTimer->setInterval(kChangeDebounceMs);
    connect(d->changeTimer, &QTimer::timeout, this, [this] { d->processPendingChanges(); });
}

Collection::~Collection() {
    // Running workers notice the new generation and stop early; they keep the default
    // providers alive through Job::keepAlive. Injected providers must outlive pending loads.
    d->cancelPending();
    for (const QString& f : std::as_const(d->leased))
        FolderCache::instance().release(f);
    delete d;
}

void Collection::openFile(const QString& filePath) {
    const QString path = normalizedPath(filePath);
    if (path.isEmpty()) {
        d->reset(Mode::None);
        d->updateLease();
        emit entriesChanged();
        emit currentChanged(-1, QString());
        return;
    }
    d->reset(Mode::File);
    d->folder = parentOf(path);
    d->openedFile = path;
    d->updateLease();
    d->entries = {entryForPath(path)};
    d->rebuildIndex();
    d->current = 0;
    emit entriesChanged();
    emit currentChanged(0, path);
    d->revalidateNext = true;
    d->start(JobKind::Load);
}

void Collection::openFiles(const QStringList& filePaths, bool keepAsFileSet) {
    QStringList paths;
    QSet<QString> seen;
    for (const QString& p : filePaths) {
        const QString n = normalizedPath(p);
        if (!n.isEmpty() && !seen.contains(n)) {
            seen.insert(n);
            paths.append(n);
        }
    }
    if (paths.size() == 1 && !keepAsFileSet) {
        // A single file is a folder collection (F-OPEN-02); several are exactly those (F-OPEN-04).
        openFile(paths.first());
        return;
    }
    d->reset(Mode::Files);
    d->updateLease();
    d->files = paths;
    d->fileSet = QSet<QString>(paths.cbegin(), paths.cend());
    QString common = paths.isEmpty() ? QString() : parentOf(paths.first());
    for (const QString& p : std::as_const(paths)) {
        if (parentOf(p) != common) {
            common.clear();
            break;
        }
    }
    d->folder = common;
    for (const QString& p : std::as_const(paths))
        d->entries.append(entryForPath(p));
    d->rebuildIndex();
    d->current = d->entries.isEmpty() ? -1 : 0;
    emit entriesChanged();
    emit currentChanged(d->current, d->current >= 0 ? d->entries[0].path : QString());
    d->start(JobKind::Load);
}

void Collection::openFolder(const QString& folderPath, bool selectLast) {
    d->reset(Mode::Folder);
    d->folder = normalizedPath(folderPath);
    d->updateLease();
    d->selectFirstOnLoad = !selectLast;
    d->selectLastOnLoad = selectLast;
    emit entriesChanged();
    emit currentChanged(-1, QString());
    d->revalidateNext = true;
    d->start(JobKind::Load);
}

void Collection::openContainer(const QString& containerPath, const QString& selectMember) {
    d->reset(Mode::Container);
    d->container = normalizedPath(containerPath);
    d->updateLease();
    const QString wanted = selectMember.isEmpty() ? QString() : vpath::join(d->container, selectMember);
    d->selectFirstOnLoad = wanted.isEmpty();
    if (!wanted.isEmpty()) {
        // Shown at once (like openFile(), F-OPEN-03); the listing places it.
        d->entries = {entryForPath(wanted)};
        d->entries[0].name = vpath::memberFileName(wanted);
        d->rebuildIndex();
        d->current = 0;
    }
    emit entriesChanged();
    emit currentChanged(d->current, currentPath());
    d->start(JobKind::Load);
}

QString Collection::containerPath() const {
    return d->mode == Mode::Container ? d->container : QString();
}

bool Collection::isContainerCollection() const {
    return d->mode == Mode::Container;
}

void Collection::setDefaultSort(bool followFinder, bool allowLiveQuery, const SortSpec& manual) {
    d->followFinder = followFinder;
    d->allowLiveQuery = allowLiveQuery;
    d->manualSpec = manual;
    d->manualSpec.source = SortSource::Manual;
}

void Collection::setFolderSortMemory(FolderSortMemory memory) {
    d->folderMemory = std::move(memory);
}

void Collection::copyFrom(const Collection& other) {
    if (&other == this)
        return;
    const Private& o = *other.d;
    d->reset(o.mode);
    d->folder = o.folder;
    d->openedFile = o.openedFile;
    d->container = o.container; // F-OPEN-10
    d->files = o.files;
    d->fileSet = o.fileSet;
    d->recursive = o.recursive; // F-OPEN-08
    d->treeFolders = o.treeFolders;
    d->treeSet = o.treeSet;
    d->localChanges = o.localChanges;
    d->pairing = o.pairing; // F-FMT-06
    d->partners = o.partners;
    d->showRaw = o.showRaw;
    d->updateLease();
    d->followFinder = o.followFinder;
    d->allowLiveQuery = o.allowLiveQuery;
    d->manualSpec = o.manualSpec;
    d->ratingReader = o.ratingReader;
    d->filter = o.filter;
    d->nameFilter = o.nameFilter;
    d->nameTerms = o.nameTerms;
    d->hidden = o.hidden;
    d->stale = o.stale;
    const bool specChanged = !(d->spec == o.spec);
    d->spec = o.spec;
    d->entries = o.entries;
    d->lastLocalChange = o.lastLocalChange; // the copied entries contain those edits
    d->rebuildIndex();
    d->rebuildPairIndex();
    d->current = o.current;
    d->selectFirstOnLoad = o.selectFirstOnLoad;
    d->selectLastOnLoad = o.selectLastOnLoad;
    emit entriesChanged();
    emit currentChanged(d->current, currentPath());
    if (specChanged)
        emit sortSpecChanged(d->spec);
    if (d->mode == Mode::None)
        return;
    if (o.loading) {
        d->start(JobKind::Load);
        return;
    }
    emit loadingFinished();
}

QString Collection::folderPath() const {
    return d->folder;
}
bool Collection::isFolderCollection() const {
    return d->mode == Mode::File || d->mode == Mode::Folder;
}
bool Collection::isLoading() const {
    return d->loading;
}
int Collection::count() const {
    return int(d->entries.size());
}

const FileEntry& Collection::at(int index) const {
    Q_ASSERT(index >= 0 && index < d->entries.size());
    return d->entries[index];
}

const QVector<FileEntry>& Collection::entries() const {
    return d->entries;
}

int Collection::indexOf(const QString& path) const {
    auto it = d->indexByPath.constFind(path);
    if (it == d->indexByPath.cend())
        it = d->indexByPath.constFind(normalizedPath(path));
    return it == d->indexByPath.cend() ? -1 : *it;
}

int Collection::currentIndex() const {
    return d->current;
}

QString Collection::currentPath() const {
    return d->current >= 0 ? d->entries[d->current].path : QString();
}

void Collection::setCurrentIndex(int index) {
    if (index < 0 || index >= d->entries.size())
        return;
    d->setCurrent(index);
}

bool Collection::setCurrentPath(const QString& path) {
    const int i = indexOf(path);
    if (i < 0)
        return false;
    d->setCurrent(i);
    return true;
}

bool Collection::next(WrapMode wrap) {
    return jump(1, wrap);
}
bool Collection::previous(WrapMode wrap) {
    return jump(-1, wrap);
}

bool Collection::first() {
    if (d->entries.isEmpty() || d->current == 0)
        return false;
    d->setCurrent(0);
    return true;
}

bool Collection::last() {
    const int lastIndex = int(d->entries.size()) - 1;
    if (lastIndex < 0 || d->current == lastIndex)
        return false;
    d->setCurrent(lastIndex);
    return true;
}

bool Collection::jump(int delta, WrapMode wrap) {
    const int n = int(d->entries.size());
    if (n == 0 || delta == 0)
        return false;
    const int cur = std::max(d->current, 0);
    int target;
    if (wrap == WrapMode::Wrap) {
        target = int(((qint64(cur) + delta) % n + n) % n);
    } else {
        target = int(std::clamp<qint64>(qint64(cur) + delta, 0, n - 1));
    }
    if (target == d->current)
        return false;
    d->setCurrent(target);
    return true;
}

SortSpec Collection::sortSpec() const {
    return d->spec;
}

void Collection::setSortSpec(const SortSpec& spec) {
    SortSpec s = spec;
    s.source = SortSource::Manual;
    const bool changed = d->followFinder || !(d->spec == s);
    d->followFinder = false;
    d->manualSpec = s;
    d->spec = s;
    if (changed)
        emit sortSpecChanged(s);
    if (d->mode == Mode::None)
        return;
    // While the initial load is running the entries are incomplete: restart it.
    d->start(d->loading ? JobKind::Load : JobKind::Resort);
}

void Collection::setFollowFinder(bool follow, bool allowLiveQuery) {
    const bool changed = d->followFinder != follow || d->allowLiveQuery != allowLiveQuery;
    d->followFinder = follow;
    d->allowLiveQuery = allowLiveQuery;
    if (!follow && changed) {
        d->spec = d->manualSpec;
        emit sortSpecChanged(d->spec);
    }
    if (!changed || d->mode == Mode::None)
        return;
    d->start(d->loading ? JobKind::Load : JobKind::Resort);
}

bool Collection::followsFinder() const {
    return d->followFinder;
}

void Collection::refresh() {
    if (d->mode == Mode::None)
        return;
    for (const QString& f : std::as_const(d->leased))
        FolderCache::instance().invalidate(f);
    d->start(JobKind::Load);
}

void Collection::reload() {
    if (d->mode == Mode::None)
        return;
    if (d->recursive)
        d->revalidateNext = true; // F-OPEN-08: a stat per subfolder catches dropped events
    d->start(JobKind::Load);
}

void Collection::removePath(const QString& path) {
    d->noteLocalChange(path);
    ++d->version;
    // F-FMT-06: a pair member goes; the pair becomes a single entry.
    const QString partnerKey = d->partners.contains(path) ? path : normalizedPath(path);
    if (auto pit = d->partners.find(partnerKey); pit != d->partners.end()) {
        d->pendingInserts.remove(partnerKey);
        if (d->watcher)
            d->removedDuringJob.insert(partnerKey);
        const QString shown = pit->pairedPath;
        d->partners.erase(pit);
        d->showRaw.remove(partnerKey);
        if (const int i = indexOf(shown); i >= 0) {
            d->entries[i].pairedPath.clear();
            emit entriesChanged();
        } else if (auto hit = d->hidden.find(shown); hit != d->hidden.end()) {
            hit->pairedPath.clear();
        }
        return;
    }
    if (const int i = indexOf(path); i >= 0 && !d->entries[i].pairedPath.isEmpty()) {
        // The shown member goes: the other one takes its place.
        FileEntry other = d->partners.take(d->entries[i].pairedPath);
        if (!other.path.isEmpty()) {
            if (d->watcher) {
                // Code review M3 #19: a running load must not bring the removed file back;
                // apply() lets the partner take its place in the result as well.
                d->removedDuringJob.insert(d->entries[i].path);
                d->insertedDuringJob.remove(d->entries[i].path);
            }
            other.pairedPath.clear();
            other.sortRank = -1;
            d->showRaw.remove(other.path);
            d->showRaw.remove(d->entries[i].path);
            const bool wasCurrent = i == d->current;
            d->entries[i] = std::move(other);
            d->rebuildIndex();
            emit entriesChanged();
            if (wasCurrent)
                emit currentChanged(d->current, currentPath());
            return;
        }
    }
    d->pendingInserts.remove(normalizedPath(path)); // a pending insertPath() must not bring it back
    d->hidden.remove(path);
    d->hidden.remove(normalizedPath(path));
    if (d->stale == path || d->stale == normalizedPath(path))
        d->stale.clear();
    const int i = indexOf(path);
    if (d->watcher) {
        // A pending load/resort must not bring the entry back (e.g. a just-trashed file).
        const QString p = i >= 0 ? d->entries[i].path : normalizedPath(path);
        d->removedDuringJob.insert(p);
        d->insertedDuringJob.remove(p);
    }
    if (i < 0)
        return;
    const QString removed = d->entries[i].path;
    d->files.removeAll(removed);
    const int oldCurrent = d->current;
    d->entries.removeAt(i);
    d->rebuildIndex();
    const int n = int(d->entries.size());
    if (n == 0)
        d->current = -1;
    else if (i < oldCurrent)
        d->current = oldCurrent - 1;
    else if (i == oldCurrent)
        d->current = std::min(i, n - 1); // the next image takes its place (F-OPEN-07)
    emit entriesChanged();
    if (d->current != oldCurrent || i == oldCurrent)
        emit currentChanged(d->current, currentPath());
}

void Collection::insertPath(const QString& filePath, bool addToFileSet, bool makeCurrent) {
    const QString path = normalizedPath(filePath);
    if (path.isEmpty() || d->mode == Mode::None)
        return;
    if (d->mode == Mode::Files) {
        if (!addToFileSet && !d->fileSet.contains(path))
            return; // F-OPEN-04: only the files this collection was opened with
        d->fileSet.insert(path);
    } else if (!d->coversFolder(parentOf(path))) {
        return; // not part of this folder collection (F-OPEN-08: or one of its subfolders)
    }
    if (d->indexByPath.contains(path)) {
        if (makeCurrent)
            setCurrentPath(path);
        return;
    }
    // stat + attributes (+ image properties for metadata sort keys) on a worker thread:
    // network volumes must not block the GUI.
    const quint64 token = ++d->insertToken;
    d->pendingInserts.insert(path, token);
    FileAttributeProvider* attributes = d->attributes;
    const bool needProps = needsImageProperties(d->spec.key);
    const RatingReader reader = d->ratingReader && (d->spec.key == SortKey::Rating || d->filter.isActive())
                                    ? d->ratingReader
                                    : RatingReader();
    std::shared_ptr<void> keepAlive = d->own;
    using Watcher = QFutureWatcher<std::optional<FileEntry>>;
    auto* w = new Watcher(this);
    connect(w, &Watcher::finished, this, [this, w, path, token, makeCurrent] {
        w->deleteLater();
        if (d->pendingInserts.value(path) != token)
            return; // superseded, removed meanwhile or the collection was reopened
        d->pendingInserts.remove(path);
        std::optional<FileEntry> e = w->result();
        if (e)
            d->insertEntry(std::move(*e), makeCurrent);
    });
    w->setFuture(
        QtConcurrent::run([path, attributes, needProps, reader, keepAlive]() -> std::optional<FileEntry> {
            auto e = statEntry(path);
            if (e && attributes) {
                attributes->fill(*e);
                if (needProps)
                    attributes->fillImageProperties(*e);
            }
            if (e && reader)
                collection::setRating(*e, reader(path));
            return e;
        }));
}

bool Collection::hasPendingInserts() const {
    return !d->pendingInserts.isEmpty();
}

void Collection::renamePath(const QString& fromPath, const QString& toPath) {
    d->noteLocalChange(fromPath);
    d->noteLocalChange(toPath);
    ++d->version;
    if (d->partners.contains(fromPath)) {
        // F-FMT-06: a renamed pair member no longer pairs (base names differ).
        removePath(fromPath);
        insertPath(toPath);
        return;
    }
    const int i = indexOf(fromPath);
    const QString to = normalizedPath(toPath);
    // F-FMT-06: the shown member of a pair was renamed. If the new name still pairs (e.g. only
    // the case changed), the partner points to the new name; otherwise the pair ends and the
    // partner becomes an entry of its own (`freed`, inserted at the end).
    FileEntry freed;
    auto repair = [&](FileEntry& e, const QString& oldPath) {
        if (e.pairedPath.isEmpty())
            return;
        const auto pit = d->partners.find(e.pairedPath);
        if (pit == d->partners.end()) {
            e.pairedPath.clear();
            return;
        }
        if (!to.isEmpty() && collection::isPairPartner(to, pit->path)) {
            pit->pairedPath = to;
            if (d->showRaw.remove(oldPath))
                d->showRaw.insert(to);
            return;
        }
        freed = std::move(*pit);
        d->partners.erase(pit);
        freed.pairedPath.clear();
        d->showRaw.remove(oldPath);
        d->showRaw.remove(freed.path);
        e.pairedPath.clear();
    };
    if (i < 0 && d->hidden.contains(fromPath)) {
        // Hidden by the rating filter: stays hidden under the new name.
        FileEntry e = d->hidden.take(fromPath);
        repair(e, fromPath);
        if (!to.isEmpty() && (d->mode == Mode::Files || d->coversFolder(parentOf(to)))) {
            e.path = to;
            e.name = QFileInfo(to).fileName();
            d->hidden.insert(to, e);
        }
        if (!freed.path.isEmpty())
            d->insertEntry(std::move(freed), false);
        return;
    }
    if (d->stale == fromPath)
        d->stale = to;
    if (i < 0) {
        insertPath(to);
        return;
    }
    const bool stays = !to.isEmpty() && (d->mode == Mode::Files || d->coversFolder(parentOf(to)));
    if (!stays) {
        removePath(fromPath);
        return;
    }
    const QString from = d->entries[i].path;
    FileEntry e = d->entries[i];
    repair(e, from);
    e.path = to;
    e.name = QFileInfo(to).fileName();
    e.finderIconPosition.reset();
    const bool wasCurrent = i == d->current;
    const QString curPath = currentPath();
    d->pendingInserts.remove(from);
    d->pendingInserts.remove(to);
    if (d->watcher) {
        d->removedDuringJob.insert(from);
        d->insertedDuringJob.remove(from);
        d->removedDuringJob.remove(to);
        d->insertedDuringJob.insert(to, e);
    }
    if (d->mode == Mode::Files) {
        for (QString& f : d->files)
            if (f == from)
                f = to;
        d->fileSet.remove(from);
        d->fileSet.insert(to);
    }
    d->entries.removeAt(i);
    d->indexByPath.remove(from);
    if (!d->indexByPath.contains(to)) {
        BinaryCollator fallback;
        const NameCollator& collator = d->collator ? *d->collator : fallback;
        const auto pos = std::upper_bound(
            d->entries.begin(), d->entries.end(), e,
            [&](const FileEntry& a, const FileEntry& b) { return d->entryLess(a, b, d->spec, collator); });
        d->entries.insert(pos, std::move(e));
    }
    d->rebuildIndex();
    const int oldCurrent = d->current;
    if (d->entries.isEmpty())
        d->current = -1;
    else if (wasCurrent)
        d->current = d->indexByPath.value(to, 0);
    else
        d->current = d->indexByPath.value(curPath, std::min<int>(d->current, int(d->entries.size()) - 1));
    emit entriesChanged();
    if (d->current != oldCurrent || wasCurrent)
        emit currentChanged(d->current, currentPath());
    if (!freed.path.isEmpty())
        d->insertEntry(std::move(freed), false); // the former partner shows up on its own
}

void Collection::setRatingReader(RatingReader reader) {
    d->ratingReader = std::move(reader);
}

void Collection::setRatingFilter(const RatingFilter& filter) {
    if (d->filter == filter)
        return;
    d->filter = filter;
    d->stale.clear();
    if (d->mode == Mode::None)
        return;
    d->start(JobKind::Load); // the snapshot is shared (FolderCache): no new enumeration
}

RatingFilter Collection::ratingFilter() const {
    return d->filter;
}

int Collection::totalCount() const {
    return int(d->entries.size() + d->hidden.size());
}

QStringList Collection::allPaths() const {
    QStringList out;
    out.reserve(d->entries.size() + d->hidden.size() + d->partners.size());
    for (const FileEntry& e : std::as_const(d->entries))
        out << e.path;
    for (auto it = d->hidden.cbegin(); it != d->hidden.cend(); ++it)
        out << it.key();
    for (auto it = d->partners.cbegin(); it != d->partners.cend(); ++it)
        out << it.key(); // F-FMT-06: the other files of pairs
    return out;
}

void Collection::updateRating(const QString& path, const Rating& r) {
    ++d->version;
    const int i = indexOf(path);
    if (i < 0) {
        const QString key = d->hidden.contains(path) ? path : normalizedPath(path);
        auto it = d->hidden.find(key);
        if (it == d->hidden.end())
            return;
        collection::setRating(*it, r);
        if ((!d->filter.isActive() || d->filter.matches(r)) &&
            namefilter::matchesTerms(it->name, d->nameTerms)) {
            FileEntry e = d->hidden.take(key);
            d->insertEntry(std::move(e), false); // matches again: back at its sorted place
        }
        return;
    }
    collection::setRating(d->entries[i], r);
    if (d->spec.key == SortKey::Rating)
        d->entries[i].sortRank = -1; // its place in a full sort changed
    if (!d->ratingReader || !d->filter.isActive() || d->filter.matches(r)) {
        if (d->stale == d->entries[i].path)
            d->stale.clear();
        return;
    }
    if (i == d->current) {
        d->stale = d->entries[i].path; // hidden when another image becomes current
        return;
    }
    const QString cur = currentPath();
    d->hidden.insert(d->entries[i].path, d->entries[i]);
    d->entries.removeAt(i);
    d->rebuildIndex();
    d->current = cur.isEmpty() ? -1 : d->indexByPath.value(cur, -1);
    emit entriesChanged();
}

void Collection::setNameFilter(const QString& query) {
    d->nameFilter = query;
    const QStringList t = namefilter::terms(query);
    if (t == d->nameTerms)
        return;
    d->nameTerms = t;
    if (d->mode == Mode::None)
        return;
    // A running load is not restarted per key (code review M3 #17): apply() filters its
    // result again when the terms changed meanwhile.
    if (d->watcher)
        return;
    d->scheduleNameFilter();
}

QString Collection::nameFilter() const {
    return d->nameFilter;
}

void Collection::Private::scheduleNameFilter() {
    if (watcher)
        return; // apply() runs it with the job's result
    NameFilterInput in;
    in.entries = entries; // implicitly shared: no copy on the GUI thread
    in.hidden = hidden;
    in.terms = nameTerms;
    in.ratingActive = ratingReader && filter.isActive();
    in.filter = filter;
    in.stale = stale;
    in.spec = spec;
    in.collator = collator;
    in.byFolder = recursive && !fileLike(mode);
    in.keepAlive = own;
    const quint64 gen = ++nameFilterGeneration;
    if (entries.size() + hidden.size() <= kSyncNameFilterEntries) {
        applyNameFilterOutput(filterByName(std::move(in)));
        return;
    }
    const quint64 ver = version;
    using Watcher = QFutureWatcher<NameFilterOutput>;
    auto* w = new Watcher(q);
    QObject::connect(w, &Watcher::finished, q, [this, w, gen, ver] {
        w->deleteLater();
        if (gen != nameFilterGeneration)
            return; // typed on, or the collection was reopened
        if (ver != version || watcher) {
            scheduleNameFilter(); // entries changed meanwhile: filter the current ones
            return;
        }
        applyNameFilterOutput(w->result());
    });
    w->setFuture(QtConcurrent::run(filterByName, std::move(in)));
}

void Collection::Private::applyNameFilterOutput(NameFilterOutput out) {
    const QString oldPath = current >= 0 && current < entries.size() ? entries[current].path : QString();
    const int oldIndex = current;
    entries = std::move(out.entries);
    hidden = std::move(out.hidden);
    indexByPath = std::move(out.index);
    ++version;
    if (out.staleLeft)
        stale.clear();
    rebuildPairIndex();
    const auto it = indexByPath.constFind(oldPath);
    current = it != indexByPath.cend() ? *it : (entries.isEmpty() ? -1 : 0);
    emit q->entriesChanged();
    const QString newPath = current >= 0 ? entries[current].path : QString();
    if (current != oldIndex || newPath != oldPath)
        emit q->currentChanged(current, newPath);
}

// ---- code review M3 #16: folder changes ----

void Collection::folderContentsChanged(const QString& folderPath) {
    if (d->mode != Mode::File && d->mode != Mode::Folder)
        return;
    const QString f = QDir::cleanPath(folderPath);
    if (!d->coversFolder(f))
        return;
    d->pendingFolders.insert(f);
    if (!d->changeTimer->isActive())
        d->changeTimer->start(); // coalesce: one update per burst (copying 500 files)
}

bool Collection::hasPendingChanges() const {
    return !d->pendingFolders.isEmpty() || d->deltaRunning;
}

void Collection::Private::processPendingChanges() {
    if (pendingFolders.isEmpty() || (mode != Mode::File && mode != Mode::Folder))
        return;
    if (deltaRunning)
        return; // started again when the running update is applied
    QStringList folders(pendingFolders.cbegin(), pendingFolders.cend());
    pendingFolders.clear();
    if (!recursive || watcher || treeFolders.isEmpty()) {
        q->reload(); // one folder (or a load is running anyway): read it again
        return;
    }
    // F-OPEN-08: only the changed folders (and new subfolders) are read again; the rest of
    // the tree keeps its entries and order.
    std::sort(folders.begin(), folders.end());
    startDelta(folders);
}

void Collection::Private::startDelta(const QStringList& folders) {
    DeltaJob dj;
    dj.base = makeJob(JobKind::Load);
    dj.base.manualSpec = spec; // the resolved spec of the last load ("Wie Finder" included)
    dj.base.manualSpec.source = spec.source;
    dj.folders = folders;
    dj.known = treeSet;
    dj.generation = ++deltaGeneration;
    dj.version = version;
    deltaRunning = true;
    using Watcher = QFutureWatcher<DeltaResult>;
    auto* w = new Watcher(q);
    QObject::connect(w, &Watcher::finished, q, [this, w] {
        w->deleteLater();
        const DeltaResult r = w->result();
        if (r.generation != deltaGeneration)
            return;
        deltaRunning = false;
        applyDelta(r);
        if (!pendingFolders.isEmpty() && !changeTimer->isActive())
            changeTimer->start();
    });
    w->setFuture(QtConcurrent::run(runDelta, std::move(dj)));
}

void Collection::Private::applyDelta(const DeltaResult& r) {
    if (r.fullReload) {
        q->reload();
        return;
    }
    if (r.version != version) {
        // Edited meanwhile (trash, rename …): the result may predate that; read again.
        for (const QString& f : r.folders)
            pendingFolders.insert(f);
        return;
    }
    // Folders whose old contents go: every changed folder, and removed folders with
    // everything below them.
    QSet<QString> replaced;
    QStringList removedRoots;
    for (const FolderContent& c : r.contents) {
        if (c.removed)
            removedRoots << c.folder;
        else
            replaced.insert(c.folder);
    }
    auto isBelowRemoved = [&](const QString& f) {
        return std::any_of(removedRoots.cbegin(), removedRoots.cend(), [&](const QString& root) {
            return f == root || f.startsWith(root.endsWith(u'/') ? root : root + u'/');
        });
    };
    auto drop = [&](const QString& path) {
        const QString f = folderOf(path);
        return replaced.contains(f) || isBelowRemoved(f);
    };
    const QString oldPath = current >= 0 && current < entries.size() ? entries[current].path : QString();
    const int oldIndex = current;
    const QVector<FileEntry> old = entries;
    entries.removeIf([&](const FileEntry& e) { return drop(e.path); });
    hidden.removeIf([&](const QHash<QString, FileEntry>::iterator& it) { return drop(it.key()); });
    partners.removeIf([&](const QHash<QString, FileEntry>::iterator& it) { return drop(it.key()); });

    // The tree's folders.
    const QStringList oldFolders = treeFolders;
    treeFolders.removeIf([&](const QString& f) { return isBelowRemoved(f); });
    QSet<QString> present(treeFolders.cbegin(), treeFolders.cend());
    for (const FolderContent& c : r.contents)
        for (const QString& f : c.folders)
            if (!c.removed && !present.contains(f)) {
                present.insert(f);
                treeFolders << f;
            }
    BinaryCollator fallback;
    const NameCollator& coll = collator ? *collator : fallback;
    if (treeFolders != oldFolders) {
        std::sort(treeFolders.begin(), treeFolders.end(), [&](const QString& a, const QString& b) {
            return collection::compareFolderPaths(a, b, coll) < 0;
        });
        treeSet = present;
    }
    QHash<QString, int> folderRank;
    for (int i = 0; i < treeFolders.size(); ++i)
        folderRank.insert(treeFolders[i], i);
    auto rankOf = [&](const FileEntry& e) { return folderRank.value(folderOf(e.path), -1); };

    // Insert each folder's (or new subtree's) sorted block where its folder belongs: the
    // entries are grouped by folder in tree order.
    for (const FolderContent& c : r.contents) {
        if (c.removed)
            continue;
        for (FileEntry e : c.hidden) {
            e.sortRank = -1;
            hidden.insert(e.path, std::move(e));
        }
        for (auto it = c.partners.cbegin(); it != c.partners.cend(); ++it)
            partners.insert(it.key(), it.value());
        if (c.shown.isEmpty())
            continue;
        const int rank = folderRank.value(c.folders.value(0, c.folder), -1);
        const auto pos = std::lower_bound(entries.begin(), entries.end(), rank,
                                          [&](const FileEntry& e, int r2) { return rankOf(e) < r2; });
        QVector<FileEntry> block = c.shown;
        for (FileEntry& e : block)
            e.sortRank = -1;
        const qsizetype at = pos - entries.begin();
        QVector<FileEntry> merged;
        merged.reserve(entries.size() + block.size());
        merged.append(entries.mid(0, at));
        merged.append(block);
        merged.append(entries.mid(at));
        entries = std::move(merged);
    }
    if (treeFolders != oldFolders)
        updateLease();
    if (!stale.isEmpty() &&
        !std::any_of(entries.cbegin(), entries.cend(), [&](const FileEntry& e) { return e.path == stale; }))
        stale.clear();
    rebuildIndex();
    rebuildPairIndex();
    auto it = indexByPath.constFind(oldPath);
    if (it == indexByPath.cend() && partners.contains(oldPath))
        it = indexByPath.constFind(partners.value(oldPath).pairedPath);
    current = it != indexByPath.cend() ? *it : neighbourIndex(old, oldIndex);
    emit q->entriesChanged();
    const QString newPath = current >= 0 ? entries[current].path : QString();
    if (current != oldIndex || newPath != oldPath)
        emit q->currentChanged(current, newPath);
    if (treeFolders != oldFolders)
        emit q->foldersChanged();
    if (entries.size() + hidden.size() > maxTreeImages)
        q->reload(); // the limit applies (status message)
}

void Collection::sidecarChanged(const QString& sidecarPath) {
    // Code review M3 #16: an XMP sidecar (e.g. written by this app's own rating) does not
    // change the image list; only with an active rating filter the rating of the image(s)
    // it belongs to is read again (worker) and the filter applied.
    if (!d->ratingReader || !d->filter.isActive() || (d->mode != Mode::File && d->mode != Mode::Folder))
        return;
    const QString sidecar = QDir::cleanPath(sidecarPath);
    const QString folderOfSidecar = folderOf(sidecar);
    if (!d->coversFolder(folderOfSidecar))
        return;
    // "IMG_1.xmp" belongs to IMG_1.* (Adobe), "IMG_1.JPG.xmp" to IMG_1.JPG.
    QString base = QFileInfo(sidecar).fileName();
    base.chop(4); // ".xmp"
    QStringList targets;
    auto consider = [&](const QString& path) {
        if (folderOf(path) != folderOfSidecar)
            return;
        const QString name = QFileInfo(path).fileName();
        if (name.compare(base, Qt::CaseInsensitive) == 0 ||
            QFileInfo(name).completeBaseName().compare(base, Qt::CaseInsensitive) == 0)
            targets << path;
    };
    for (const FileEntry& e : std::as_const(d->entries))
        consider(e.path);
    for (auto it = d->hidden.cbegin(); it != d->hidden.cend(); ++it)
        consider(it.key());
    if (targets.isEmpty())
        return;
    const RatingReader reader = d->ratingReader;
    using Ratings = QList<std::pair<QString, Rating>>;
    auto* w = new QFutureWatcher<Ratings>(this);
    connect(w, &QFutureWatcher<Ratings>::finished, this, [this, w] {
        w->deleteLater();
        for (const auto& [path, rating] : w->result())
            updateRating(path, rating);
    });
    w->setFuture(QtConcurrent::run([targets, reader] {
        Ratings out;
        for (const QString& t : targets)
            out.append({t, reader(t)});
        return out;
    }));
}

void Collection::setMaxTreeImages(int maxImages) {
    d->maxTreeImages = std::max(1, maxImages);
}

void Collection::showEntriesForTesting(const QVector<FileEntry>& entries) {
    d->reset(Mode::Files);
    d->updateLease();
    d->entries = entries;
    for (int i = 0; i < d->entries.size(); ++i) {
        d->files << d->entries[i].path;
        d->entries[i].sortRank = i;
    }
    d->fileSet = QSet<QString>(d->files.cbegin(), d->files.cend());
    d->rebuildIndex();
    d->current = d->entries.isEmpty() ? -1 : 0;
    emit entriesChanged();
    emit currentChanged(d->current, currentPath());
}

// ---- M3: subfolders (F-OPEN-08) and RAW+JPEG pairs (F-FMT-06) ----

void Collection::setIncludeSubfolders(bool on) {
    if (d->recursive == on)
        return;
    d->recursive = on;
    d->treeFolders.clear();
    d->localChanges.clear();
    d->updateLease();
    emit foldersChanged();
    if (d->mode == Mode::File || d->mode == Mode::Folder) {
        d->revalidateNext = true;
        d->start(JobKind::Load);
    }
}

bool Collection::includesSubfolders() const {
    return d->recursive;
}

QStringList Collection::folders() const {
    if (d->mode != Mode::File && d->mode != Mode::Folder)
        return {};
    if (d->recursive && !d->treeFolders.isEmpty())
        return d->treeFolders;
    return d->folder.isEmpty() ? QStringList() : QStringList{d->folder};
}

bool Collection::coversFolder(const QString& folderPath) const {
    if (d->mode != Mode::File && d->mode != Mode::Folder)
        return false;
    return d->coversFolder(QDir::cleanPath(folderPath));
}

QString Collection::relativeFolder(const QString& path) const {
    if (!d->recursive || d->folder.isEmpty())
        return {};
    const QString parent = folderOf(path);
    const QString prefix = d->folder.endsWith(u'/') ? d->folder : d->folder + u'/';
    return parent.startsWith(prefix) ? parent.mid(prefix.size()) : QString();
}

void Collection::setPairRawJpeg(bool on) {
    if (d->pairing == on)
        return;
    d->pairing = on;
    if (!on) {
        d->partners.clear();
        d->showRaw.clear();
    }
    if (d->mode != Mode::None)
        d->start(JobKind::Load); // shared snapshots: no new enumeration
}

bool Collection::pairsRawJpeg() const {
    return d->pairing;
}

QString Collection::pairedPath(const QString& path) const {
    if (const int i = indexOf(path); i >= 0)
        return d->entries[i].pairedPath;
    if (const auto it = d->partners.constFind(path); it != d->partners.cend())
        return it->pairedPath;
    if (const auto it = d->hidden.constFind(path); it != d->hidden.cend())
        return it->pairedPath;
    return {};
}

QStringList Collection::withPairs(const QStringList& paths) const {
    QStringList out;
    QSet<QString> seen;
    for (const QString& p : paths) {
        // The member that is not shown first: removing it only turns the pair into a single
        // entry, then the shown one leaves (no flash of the other file).
        const QString other = pairedPath(p);
        for (const QString& x : {other, p})
            if (!x.isEmpty() && !seen.contains(x)) {
                seen.insert(x);
                out << x;
            }
    }
    return out;
}

bool Collection::togglePairMember(const QString& path) {
    ++d->version;
    const int i = indexOf(path);
    if (i < 0 || d->entries[i].pairedPath.isEmpty())
        return false;
    FileEntry other = d->partners.take(d->entries[i].pairedPath);
    if (other.path.isEmpty())
        return false;
    FileEntry shown = d->entries[i];
    other.pairedPath = shown.path;
    shown.pairedPath = other.path;
    const QString otherPath = other.path;
    if (collection::isPairJpegSuffix(QFileInfo(otherPath).suffix().toLower()))
        d->showRaw.remove(shown.path);
    else
        d->showRaw.insert(otherPath);
    d->partners.insert(shown.path, std::move(shown));
    d->entries[i] = std::move(other);
    d->rebuildIndex();
    emit entriesChanged();
    if (i == d->current)
        emit currentChanged(d->current, otherPath);
    return true;
}

void Collection::setProviders(NameCollator* collator, FolderOrderProvider* order,
                              FileAttributeProvider* attributes) {
    d->collator = collator;
    d->order = order;
    d->attributes = attributes;
}

} // namespace lf

// Compile the moc output into this translation unit (keeps lf_core linkable per module).
#include "moc_Collection.cpp"
