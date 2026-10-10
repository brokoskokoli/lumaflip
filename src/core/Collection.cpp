// SPDX-License-Identifier: GPL-3.0-or-later
#include "Collection.h"

#include "FolderCache.h"
#include "ImageFormats.h"
#include "NameFilter.h"
#include "Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTimeZone>
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
#endif

} // namespace

QVector<FileEntry> enumerateFolder(const QString& folderPath, bool* accessDenied, bool* privacyDenied) {
    if (accessDenied)
        *accessDenied = false;
    if (privacyDenied)
        *privacyDenied = false;
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
    errno = 0;
    while (struct dirent* de = ::readdir(dir)) {
        if (de->d_name[0] == '.')
            continue;
        if (de->d_type != DT_REG && de->d_type != DT_LNK && de->d_type != DT_UNKNOWN)
            continue; // directories (incl. packages/bundles), devices, sockets ...
        const QString name = QFile::decodeName(de->d_name);
        if (!acceptName(name))
            continue;
        struct stat st;
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
    const QFileInfoList infos = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo& fi : infos) {
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
        if (!fi.isHidden() && !fi.isBundle() && !fi.fileName().startsWith(u'.'))
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

} // namespace collection

// ---------------------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------------------
namespace {

enum class Mode { None, File, Files, Folder };
enum class JobKind { Load, Resort };

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
    QVector<FileEntry> existing; // JobKind::Resort
    bool followFinder = true;
    bool allowLiveQuery = false;
    SortSpec manualSpec;
    FolderSortMemory folderMemory; // F-SORT-06
    RatingReader ratingReader;     // F-SORT-02 / F-CULL-04 (M3)
    RatingFilter filter;
    QStringList nameTerms;         // F-NAV-08 (folded, NameFilter.h)
    QString keepPath;              // stays visible although the filter does not match (current image)
    bool revalidate = false;       // explicit open: check a shared snapshot against the folder
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
    // Folder loads: FolderSnapshot::changeSeq of the snapshot used (max: not from a snapshot).
    quint64 changeSeq = std::numeric_limits<quint64>::max();
};

// Fallback collator if no platform collator is available (should not happen).
class BinaryCollator : public NameCollator {
public:
    int compare(const QString& a, const QString& b) const override {
        const int c = a.compare(b, Qt::CaseInsensitive);
        return c ? c : a.compare(b);
    }
};

Result runJob(Job job) {
    Result r;
    r.generation = job.generation;
    auto cancelled = [&] {
        r.cancelled = job.cancelled();
        return r.cancelled;
    };
    QVector<FileEntry> entries;
    if (job.kind == JobKind::Resort) {
        entries = std::move(job.existing);
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
        // F-WIN-09: one enumeration (+ attributes) per folder change, shared by all windows.
        // An explicit open checks the snapshot against the folder (volumes without change
        // notifications, e.g. network shares, would otherwise show stale contents).
        const FolderSnapshotPtr snap =
            FolderCache::instance().get(job.folder, job.attributes, job.revalidate);
        entries = snap->entries;
        r.changeSeq = snap->changeSeq;
        r.accessDenied = snap->accessDenied;
        r.privacyDenied = snap->privacyDenied;
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
    if (job.folderMemory && job.mode != Mode::Files && !job.folder.isEmpty()) {
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

    if (needsImageProperties(spec.key) && job.attributes) {
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
    if (job.ratingReader && (spec.key == SortKey::Rating || ratingFiltering)) {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if ((i & 31) == 31 && cancelled())
                return r;
            if (!entries[i].ratingRead)
                collection::setRating(entries[i], job.ratingReader(entries[i].path));
        }
        if (cancelled())
            return r;
    }
    if (filtering) {
        QVector<FileEntry> shown;
        shown.reserve(entries.size());
        for (FileEntry& e : entries) {
            const bool ratingOk =
                !ratingFiltering || job.filter.matches(collection::ratingOf(e)) || e.path == job.keepPath;
            if (ratingOk && namefilter::matchesTerms(e.name, job.nameTerms))
                shown.append(std::move(e));
            else
                r.hidden.append(std::move(e));
        }
        entries = std::move(shown);
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
    collection::sortEntries(entries, spec, job.collator ? *job.collator : fallback);
    if (cancelled())
        return r;
    r.entries = std::move(entries);
    r.spec = spec;
    return r;
}

} // namespace

struct Collection::Private {
    Collection* q = nullptr;
    Mode mode = Mode::None;
    QString folder;
    QString openedFile;
    QStringList files;
    QSet<QString> fileSet; // Mode::Files: every file the collection was opened with (F-OPEN-04)
    QString leased;        // folder leased in the FolderCache
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

    void noteLocalChange() {
        if ((mode == Mode::File || mode == Mode::Folder) && !folder.isEmpty())
            lastLocalChange = FolderCache::instance().noteLocalChange(folder);
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

    // Keeps exactly one FolderCache lease: on the shown folder of a folder collection.
    void updateLease() {
        const QString want = (mode == Mode::File || mode == Mode::Folder) ? folder : QString();
        if (want == leased)
            return;
        FolderCache& cache = FolderCache::instance();
        cache.acquire(want);
        cache.release(leased);
        leased = want;
    }

    void rebuildIndex() {
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
        if (kind == JobKind::Resort)
            j.existing = entries;
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
        if (mode == Mode::None)
            return;
        loading = true;
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
        hidden.clear();
        for (const FileEntry& e : r.hidden)
            if (!removedDuringJob.contains(e.path))
                hidden.insert(e.path, e);
        if (!stale.isEmpty() && !std::any_of(entries.cbegin(), entries.cend(),
                                             [&](const FileEntry& e) { return e.path == stale; }))
            stale.clear();
        replayEditsDuringJob(r.spec);
        rebuildIndex();
        if (selectFirstOnLoad) {
            current = entries.isEmpty() ? -1 : 0;
            selectFirstOnLoad = false;
        } else if (selectLastOnLoad) {
            current = int(entries.size()) - 1;
            selectLastOnLoad = false;
        } else {
            const auto it = indexByPath.constFind(oldPath);
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
        if (!removedDuringJob.isEmpty()) {
            entries.removeIf([&](const FileEntry& e) { return removedDuringJob.contains(e.path); });
            files.removeIf([&](const QString& p) { return removedDuringJob.contains(p); });
        }
        if (!insertedDuringJob.isEmpty()) {
            QSet<QString> present;
            for (const FileEntry& e : std::as_const(entries))
                present.insert(e.path);
            BinaryCollator fallback;
            const NameCollator& coll = collator ? *collator : fallback;
            for (auto it = insertedDuringJob.begin(); it != insertedDuringJob.end(); ++it) {
                if (present.contains(it.key()))
                    continue;
                FileEntry e = it.value();
                // Image metadata is read by a resort on a worker (apply()), never here on
                // the GUI thread; until then the entry sorts with the unknown values.
                if (needsImageProperties(sortSpec.key) && attributes && !e.imagePropertiesRead)
                    resortForProperties = true;
                const auto pos = std::upper_bound(
                    entries.begin(), entries.end(), e, [&](const FileEntry& a, const FileEntry& b) {
                        return collection::compareEntries(a, b, sortSpec, coll) < 0;
                    });
                entries.insert(pos, std::move(e));
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
    // F-NAV-08: a changed name filter applied to the entries in memory (no new job).
    void applyNameFilter();

    void reset(Mode m) {
        cancelPending();
        mode = m;
        folder.clear();
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
    }

    // Inserts a stat'ed entry at its sorted position (GUI thread).
    void insertEntry(FileEntry e, bool makeCurrent) {
        const QString path = e.path;
        if (indexByPath.contains(path)) {
            if (makeCurrent)
                setCurrent(indexByPath.value(path));
            return;
        }
        noteLocalChange();
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
                return collection::compareEntries(a, b, spec, coll) < 0;
            });
        const int at = int(pos - entries.begin());
        entries.insert(at, std::move(e));
        rebuildIndex();
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
}

Collection::~Collection() {
    // Running workers notice the new generation and stop early; they keep the default
    // providers alive through Job::keepAlive. Injected providers must outlive pending loads.
    d->cancelPending();
    FolderCache::instance().release(d->leased);
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
    d->files = o.files;
    d->fileSet = o.fileSet;
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
    if (!d->leased.isEmpty())
        FolderCache::instance().invalidate(d->leased);
    d->start(JobKind::Load);
}

void Collection::reload() {
    if (d->mode == Mode::None)
        return;
    d->start(JobKind::Load);
}

void Collection::removePath(const QString& path) {
    d->noteLocalChange();
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
    } else if (parentOf(path) != d->folder) {
        return; // not part of this folder collection
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
    d->noteLocalChange();
    const int i = indexOf(fromPath);
    const QString to = normalizedPath(toPath);
    if (i < 0 && d->hidden.contains(fromPath)) {
        // Hidden by the rating filter: stays hidden under the new name.
        FileEntry e = d->hidden.take(fromPath);
        if (!to.isEmpty() && (d->mode == Mode::Files || parentOf(to) == d->folder)) {
            e.path = to;
            e.name = QFileInfo(to).fileName();
            d->hidden.insert(to, e);
        }
        return;
    }
    if (d->stale == fromPath)
        d->stale = to;
    if (i < 0) {
        insertPath(to);
        return;
    }
    const bool stays = !to.isEmpty() && (d->mode == Mode::Files || parentOf(to) == d->folder);
    if (!stays) {
        removePath(fromPath);
        return;
    }
    const QString from = d->entries[i].path;
    FileEntry e = d->entries[i];
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
        const auto pos = std::upper_bound(d->entries.begin(), d->entries.end(), e,
                                          [&](const FileEntry& a, const FileEntry& b) {
                                              return collection::compareEntries(a, b, d->spec, collator) < 0;
                                          });
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
    out.reserve(d->entries.size() + d->hidden.size());
    for (const FileEntry& e : std::as_const(d->entries))
        out << e.path;
    for (auto it = d->hidden.cbegin(); it != d->hidden.cend(); ++it)
        out << it.key();
    return out;
}

void Collection::updateRating(const QString& path, const Rating& r) {
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
    if (d->watcher) {
        d->start(JobKind::Load); // the running job filters with the old terms
        return;
    }
    d->applyNameFilter();
}

QString Collection::nameFilter() const {
    return d->nameFilter;
}

void Collection::Private::applyNameFilter() {
    const QString oldPath = current >= 0 && current < entries.size() ? entries[current].path : QString();
    const int oldIndex = current;
    // Visible entries that no longer match leave (also the current one and a stale one).
    QVector<FileEntry> kept;
    kept.reserve(entries.size());
    for (FileEntry& e : entries) {
        if (namefilter::matchesTerms(e.name, nameTerms)) {
            kept.append(std::move(e));
        } else {
            if (e.path == stale)
                stale.clear();
            const QString p = e.path;
            hidden.insert(p, std::move(e));
        }
    }
    // Hidden entries that match the name and the rating filter come back, sorted.
    QVector<FileEntry> back;
    for (auto it = hidden.begin(); it != hidden.end();) {
        const FileEntry& e = it.value();
        const bool ratingOk =
            !(ratingReader && filter.isActive()) || !e.ratingRead || filter.matches(collection::ratingOf(e));
        if (ratingOk && namefilter::matchesTerms(e.name, nameTerms)) {
            back.append(it.value());
            it = hidden.erase(it);
        } else {
            ++it;
        }
    }
    BinaryCollator fallback;
    const NameCollator& coll = collator ? *collator : fallback;
    auto less = [&](const FileEntry& a, const FileEntry& b) {
        return collection::compareEntries(a, b, spec, coll) < 0;
    };
    std::sort(back.begin(), back.end(), less);
    entries.clear();
    entries.reserve(kept.size() + back.size());
    std::merge(std::make_move_iterator(kept.begin()), std::make_move_iterator(kept.end()),
               std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()),
               std::back_inserter(entries), less);
    rebuildIndex();
    const auto it = indexByPath.constFind(oldPath);
    current = it != indexByPath.cend() ? *it : (entries.isEmpty() ? -1 : 0);
    emit q->entriesChanged();
    const QString newPath = current >= 0 ? entries[current].path : QString();
    if (current != oldIndex || newPath != oldPath)
        emit q->currentChanged(current, newPath);
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
