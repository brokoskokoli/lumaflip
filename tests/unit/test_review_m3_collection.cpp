// SPDX-License-Identifier: GPL-3.0-or-later
// Code review M3 (collections and navigation): incremental tree updates and coalesced
// folder changes (#16), the name filter on large collections (#17), RAW+JPEG pairs while a
// load runs (#19), recursion limits (#20), "go to" overflow (#21), the quick move target
// (#22) and the configurable ⌥←/⌥→ folder shortcuts (F-OPEN-09).
// Requirement IDs: F-OPEN-07/08/09, F-NAV-06/08, F-FMT-06, F-CULL-04, F-FILE-08.

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/FolderCache.h"
#include "core/InputBindings.h"
#include "core/NameFilter.h"
#include "core/NavigationOrder.h"
#include "core/Platform.h"
#include "core/QuickMove.h"
#include "core/Settings.h"

#include <gtest/gtest.h>

#include <algorithm>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lf;

namespace {

void touch(const QString& path, const QByteArray& content = "x") {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(content);
}

QStringList relNames(const Collection& c) {
    QStringList out;
    for (const FileEntry& e : c.entries()) {
        const QString rel = c.relativeFolder(e.path);
        out << (rel.isEmpty() ? e.name : rel + u'/' + e.name);
    }
    return out;
}

// Attributes that take time for files whose name contains "slow" (keeps a load running).
class SlowAttributes : public FileAttributeProvider {
public:
    std::atomic<int> delayMs{0};
    std::atomic<int> filled{0};
    void fill(FileEntry& e) override {
        ++filled;
        if (delayMs > 0 && e.name.contains(QLatin1String("slow")))
            QThread::msleep(ulong(delayMs.load()));
    }
};

class ReviewCollectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        test::ensureApp();
        ASSERT_TRUE(dir.isValid());
        root = QDir::cleanPath(dir.path());
        collator = createNameCollator();
    }
    QString path(const QString& rel) const { return root + u'/' + rel; }
    std::unique_ptr<Collection> make(FileAttributeProvider* attrs = nullptr) {
        auto c = std::make_unique<Collection>();
        c->setProviders(collator.get(), &order, attrs ? attrs : &attributes);
        c->setDefaultSort(false, false, SortSpec{SortKey::Name, true, SortSource::Manual});
        return c;
    }
    static bool loaded(Collection& c) {
        return test::waitUntil([&] { return !c.isLoading(); });
    }
    // The app wiring for a change: the shared snapshot is invalidated once, then the
    // window hands the folder to its collection (Application/ViewerWindow::folderChanged).
    static void changed(Collection& c, const QString& folder) {
        FolderCache::instance().invalidate(folder);
        c.folderContentsChanged(folder);
    }
    static bool settled(Collection& c) {
        return test::waitUntil([&] { return !c.hasPendingChanges() && !c.isLoading(); });
    }

    QTemporaryDir dir;
    QString root;
    std::unique_ptr<NameCollator> collator;
    test::FakeOrderProvider order;
    test::FakeAttributeProvider attributes;
};

} // namespace

// ---------------------------------------------------------------------------------------
// #16 recursive watching: coverage, coalescing, incremental updates, sidecars
// ---------------------------------------------------------------------------------------

// Packages, chflags-hidden folders and dot folders are skipped by the tree reader; their
// constant internal changes (e.g. a photo library) must not concern the collection.
TEST_F(ReviewCollectionTest, F_OPEN_08_CoversFolderSkipsPackagesAndHiddenFolders) {
    touch(path("a.jpg"));
    touch(path("Bibliothek.photoslibrary/database/x.db"));
    touch(path("App.app/Contents/y.jpg"));
    touch(path("Versteckt/Tief/z.jpg"));
    touch(path(".punkt/w.jpg"));
#ifdef UF_HIDDEN
    ASSERT_EQ(::chflags(QFile::encodeName(path("Versteckt")).constData(), UF_HIDDEN), 0);
#endif
    auto c = make();
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
#ifdef UF_HIDDEN
    EXPECT_EQ(relNames(*c), QStringList{"a.jpg"});
#else
    // No chflags hidden flag on this system (Linux): "Versteckt" is an ordinary folder.
    EXPECT_EQ(relNames(*c), (QStringList{"a.jpg", "Versteckt/Tief/z.jpg"}));
#endif
    EXPECT_TRUE(c->coversFolder(root));
    EXPECT_FALSE(c->coversFolder(path("App.app/Contents")));
#ifdef UF_HIDDEN
    EXPECT_FALSE(c->coversFolder(path("Versteckt/Tief")));
#endif
    EXPECT_FALSE(c->coversFolder(path(".punkt")));
    // A folder created after the load is covered (its files are read with it).
    touch(path("Neu/b.jpg"));
    EXPECT_TRUE(c->coversFolder(path("Neu")));
    // A change below a package is dropped without reading anything.
    const int before = FolderCache::instance().totalEnumerations();
    changed(*c, path("App.app/Contents"));
    EXPECT_FALSE(c->hasPendingChanges());
    test::waitUntil([] { return false; }, 400);
    EXPECT_EQ(FolderCache::instance().totalEnumerations(), before);
}

// A burst of changes in one subfolder: coalesced into one update that reads only that
// folder; the rest of the tree is not sorted or read again (no full load).
TEST_F(ReviewCollectionTest, F_OPEN_08_ChangesAreCoalescedAndIncremental) {
    touch(path("a.jpg"));
    touch(path("Sub/b.jpg"));
    touch(path("Sub/d.jpg"));
    touch(path("Zweiter/e.jpg"));
    auto c = make();
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    ASSERT_EQ(c->count(), 4);
    c->setCurrentPath(path("Zweiter/e.jpg"));
    int loads = 0;
    QObject::connect(c.get(), &Collection::loadingFinished, [&] { ++loads; });
    const int rootReads = FolderCache::instance().enumerationCount(root);
    const int subReads = FolderCache::instance().enumerationCount(path("Sub"));
    const int otherReads = FolderCache::instance().enumerationCount(path("Zweiter"));
    for (int i = 0; i < 20; ++i) {
        touch(path(QStringLiteral("Sub/c%1.jpg").arg(i, 2, 10, QLatin1Char('0'))));
        changed(*c, path("Sub"));
    }
    EXPECT_EQ(c->count(), 4) << "applied after the debounce, not per event";
    ASSERT_TRUE(settled(*c));
    EXPECT_EQ(c->count(), 24);
    EXPECT_EQ(FolderCache::instance().enumerationCount(path("Sub")), subReads + 1) << "one read per burst";
    EXPECT_EQ(FolderCache::instance().enumerationCount(root), rootReads);
    EXPECT_EQ(FolderCache::instance().enumerationCount(path("Zweiter")), otherReads);
    EXPECT_EQ(loads, 0) << "no full reload of the tree";
    EXPECT_EQ(c->currentPath(), path("Zweiter/e.jpg"));
    const QStringList names = relNames(*c);
    EXPECT_EQ(names.first(), QStringLiteral("a.jpg"));
    EXPECT_EQ(names.at(1), QStringLiteral("Sub/b.jpg"));
    EXPECT_EQ(names.last(), QStringLiteral("Zweiter/e.jpg")); // folder order kept

    // New subfolder (event of its parent) and a vanished one.
    touch(path("Sub/Neu/f.jpg"));
    changed(*c, path("Sub"));
    ASSERT_TRUE(settled(*c));
    EXPECT_TRUE(c->folders().contains(path("Sub/Neu")));
    EXPECT_TRUE(c->indexOf(path("Sub/Neu/f.jpg")) > c->indexOf(path("Sub/d.jpg")));
    EXPECT_TRUE(c->indexOf(path("Sub/Neu/f.jpg")) < c->indexOf(path("Zweiter/e.jpg")));
    QFile::remove(path("Zweiter/e.jpg"));
    QDir().rmdir(path("Zweiter"));
    changed(*c, root);
    ASSERT_TRUE(settled(*c));
    EXPECT_FALSE(c->folders().contains(path("Zweiter")));
    EXPECT_EQ(c->indexOf(path("Zweiter/e.jpg")), -1);
    EXPECT_EQ(c->count(), 24);
    EXPECT_EQ(loads, 0);
}

// An XMP sidecar does not reload anything; with a rating filter only that image's rating
// is read again.
TEST_F(ReviewCollectionTest, F_CULL_04_SidecarChangeRereadsOneRatingOnly) {
    touch(path("a.jpg"));
    touch(path("b.jpg"));
    touch(path("c.jpg"));
    auto c = make();
    std::atomic<int> reads{0};
    QHash<QString, int> stars{{path("a.jpg"), 3}, {path("b.jpg"), 3}, {path("c.jpg"), 1}};
    QMutex mutex;
    c->setRatingReader([&](const QString& p) {
        ++reads;
        QMutexLocker lock(&mutex);
        Rating r;
        r.stars = stars.value(p);
        return r;
    });
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    RatingFilter min3;
    min3.kind = RatingFilter::Kind::MinStars;
    min3.minStars = 3;
    c->setRatingFilter(min3);
    ASSERT_TRUE(loaded(*c));
    ASSERT_EQ(c->count(), 2);
    int loads = 0;
    QObject::connect(c.get(), &Collection::loadingFinished, [&] { ++loads; });
    reads = 0;
    {
        QMutexLocker lock(&mutex);
        stars[path("c.jpg")] = 4; // e.g. rated in Lightroom
    }
    c->sidecarChanged(path("c.xmp"));
    ASSERT_TRUE(test::waitUntil([&] { return c->count() == 3; }));
    EXPECT_EQ(reads.load(), 1);
    EXPECT_EQ(loads, 0);
    // Without a rating filter a sidecar changes nothing (and reads nothing).
    c->setRatingFilter(RatingFilter{});
    ASSERT_TRUE(loaded(*c));
    reads = 0;
    c->sidecarChanged(path("a.JPG.xmp"));
    test::waitUntil([] { return false; }, 100);
    EXPECT_EQ(reads.load(), 0);
}

// ---------------------------------------------------------------------------------------
// #17 name filter
// ---------------------------------------------------------------------------------------

// 50 000 entries: the GUI thread never spends more than 30 ms per step (the filter runs
// on a worker, the result is swapped in); the order is kept without sorting.
TEST(ReviewNameFilter, F_NAV_08_FiftyThousandEntriesStayResponsive) {
    test::ensureApp();
    QVector<FileEntry> entries;
    entries.reserve(50000);
    for (int i = 0; i < 50000; ++i) {
        FileEntry e;
        e.name = QStringLiteral("Urlaub %1 %2.jpg")
                     .arg(i % 7 == 0 ? QStringLiteral("München") : QStringLiteral("Berlin"))
                     .arg(i, 5, 10, QLatin1Char('0'));
        e.path = QStringLiteral("/review/") + e.name;
        entries.append(e);
    }
    Collection c;
    c.showEntriesForTesting(entries);
    ASSERT_EQ(c.count(), 50000);
    qint64 worstSlice = 0;
    auto step = [&](const QString& query, int expected) {
        QElapsedTimer t;
        t.start();
        c.setNameFilter(query);
        worstSlice = std::max(worstSlice, t.elapsed());
        QElapsedTimer total;
        total.start();
        while (c.count() != expected && total.elapsed() < 20000) {
            QElapsedTimer slice;
            slice.start();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            worstSlice = std::max(worstSlice, slice.elapsed());
            QThread::msleep(1);
        }
        return c.count();
    };
    auto expected = [&](const QString& query) {
        int n = 0;
        for (const FileEntry& e : std::as_const(entries))
            n += namefilter::matches(e.name, query) ? 1 : 0;
        return n;
    };
    const QString munich = QString::fromUtf8("m\xC3\xBCn");
    const QString narrower = QString::fromUtf8("M\xC3\xBCnchen 4");
    auto run = [&] {
        worstSlice = 0;
        EXPECT_EQ(step(munich, expected(munich)), (50000 + 6) / 7);
        EXPECT_EQ(step(narrower, expected(narrower)), expected(narrower));
        EXPECT_EQ(step(QString(), 50000), 50000);
        return worstSlice;
    };
    // LF_TIMING_FACTOR (CI: shared runners, sanitizers) scales the limit. One slow slice
    // can be load from other processes (parallel ctest): a second run decides then, like
    // the benchmark re-run in tools/check.sh.
    const qint64 limit = 30 * std::max(1, qEnvironmentVariableIntValue("LF_TIMING_FACTOR"));
    qint64 worst = run();
    if (worst >= limit)
        worst = std::min(worst, run());
    EXPECT_LT(worst, limit);
    // Order kept: the entries come back in their original order.
    for (int i = 0; i < 50000; i += 4999)
        EXPECT_EQ(c.at(i).path, entries.at(i).path);
}

// Typing while a load runs does not restart it (each key used to start a new job that
// read every rating again).
TEST_F(ReviewCollectionTest, F_NAV_08_TypingDoesNotRestartARunningLoad) {
    for (int i = 0; i < 20; ++i)
        touch(path(QStringLiteral("bild%1.jpg").arg(i, 2, 10, QLatin1Char('0'))));
    auto c = make();
    std::atomic<int> reads{0};
    c->setRatingReader([&](const QString&) {
        ++reads;
        QThread::msleep(10);
        Rating r;
        r.stars = 3;
        return r;
    });
    RatingFilter min1;
    min1.kind = RatingFilter::Kind::MinStars;
    min1.minStars = 1;
    c->setRatingFilter(min1);
    c->openFolder(root);
    EXPECT_TRUE(c->isLoading());
    for (const char* q : {"b", "bi", "bil", "bild", "bild1"}) {
        c->setNameFilter(QString::fromLatin1(q));
        test::waitUntil([] { return false; }, 15);
    }
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(reads.load(), 20) << "every rating read exactly once";
    EXPECT_EQ(c->count(), 10); // bild10 … bild19
}

// ---------------------------------------------------------------------------------------
// #19 pairs while a load runs
// ---------------------------------------------------------------------------------------

// The shown JPEG of a pair is trashed while a (tree) load runs on an older snapshot: it
// must not come back; the RAW takes its place.
TEST_F(ReviewCollectionTest, F_FMT_06_RemovedShownPairMemberStaysGoneAfterLoad) {
    touch(path("Sub/IMG_1.JPG"));
    touch(path("Sub/IMG_1.CR3"));
    for (int i = 0; i < 10; ++i)
        touch(path(QStringLiteral("Sub/slow%1.jpg").arg(i)));
    SlowAttributes slow;
    auto c = make(&slow);
    c->setPairRawJpeg(true);
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    ASSERT_GE(c->indexOf(path("Sub/IMG_1.JPG")), 0);
    // Reload with a slow enumeration; remove the JPEG once the enumeration listed it.
    slow.delayMs = 30;
    slow.filled = 0;
    FolderCache::instance().invalidate(path("Sub"));
    c->reload();
    ASSERT_TRUE(test::waitUntil([&] { return slow.filled.load() > 0; }));
    ASSERT_TRUE(c->isLoading());
    QFile::remove(path("Sub/IMG_1.JPG"));
    c->removePath(path("Sub/IMG_1.JPG"));
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->indexOf(path("Sub/IMG_1.JPG")), -1) << "the trashed JPEG came back";
    EXPECT_GE(c->indexOf(path("Sub/IMG_1.CR3")), 0);
    EXPECT_TRUE(c->pairedPath(path("Sub/IMG_1.CR3")).isEmpty());
}

// The RAW of a pair arrives (insertPath) while a load runs on a snapshot without it: after
// the load the pair is complete.
TEST_F(ReviewCollectionTest, F_FMT_06_PairCompletedDuringLoadSurvives) {
    // In a tree: the load works on folder snapshots taken before the RAW existed.
    touch(path("Sub/IMG_2.JPG"));
    for (int i = 0; i < 10; ++i)
        touch(path(QStringLiteral("Sub/slow%1.jpg").arg(i)));
    SlowAttributes slow;
    auto c = make(&slow);
    c->setPairRawJpeg(true);
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    slow.delayMs = 40;
    slow.filled = 0;
    FolderCache::instance().invalidate(path("Sub"));
    c->reload();
    ASSERT_TRUE(test::waitUntil([&] { return slow.filled.load() > 0; }));
    touch(path("Sub/IMG_2.CR3"));
    c->insertPath(path("Sub/IMG_2.CR3"));
    ASSERT_TRUE(test::waitUntil([&] { return !c->hasPendingInserts(); }));
    ASSERT_TRUE(c->isLoading()) << "the insert must land while the load runs";
    EXPECT_EQ(c->pairedPath(path("Sub/IMG_2.JPG")), path("Sub/IMG_2.CR3"));
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->pairedPath(path("Sub/IMG_2.JPG")), path("Sub/IMG_2.CR3")) << "lost by the load";
    EXPECT_EQ(c->indexOf(path("Sub/IMG_2.CR3")), -1); // the partner, not a second entry
}

// ---------------------------------------------------------------------------------------
// #20 recursion limits
// ---------------------------------------------------------------------------------------

TEST_F(ReviewCollectionTest, F_OPEN_08_ImageLimitStopsTheTreeWithAMessage) {
    for (const char* f : {"a.jpg", "b.jpg", "S1/c.jpg", "S1/d.jpg", "S2/e.jpg"})
        touch(path(QString::fromLatin1(f)));
    auto c = make();
    int limit = -1;
    QObject::connect(c.get(), &Collection::treeLimitReached, [&](int n) { limit = n; });
    c->setMaxTreeImages(3);
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(relNames(*c), (QStringList{"a.jpg", "b.jpg", "S1/c.jpg"}));
    EXPECT_EQ(limit, 3);
}

// Mount points (another volume inside the tree) are not entered.
TEST(ReviewCollectionPure, F_OPEN_08_RecursionStopsAtMountPoints) {
#ifdef Q_OS_MACOS
    // The system's own volumes mounted below /System/Volumes (VM, Preboot, …): another
    // st_dev than the folder they are listed in.
    struct stat parent{};
    ASSERT_EQ(::stat("/System/Volumes", &parent), 0);
    QString mountPoint;
    for (const char* name : {"VM", "Preboot", "Update", "xarts", "iSCPreboot", "Hardware"}) {
        struct stat st{};
        const QByteArray full = QByteArray("/System/Volumes/") + name;
        if (::lstat(full.constData(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev != parent.st_dev &&
            !(st.st_flags & UF_HIDDEN)) {
            mountPoint = QString::fromLatin1(name);
            break;
        }
    }
    if (mountPoint.isEmpty())
        GTEST_SKIP() << "no mount point to test with";
    QStringList subfolders;
    collection::enumerateFolder(QStringLiteral("/System/Volumes"), nullptr, nullptr, &subfolders);
    EXPECT_FALSE(subfolders.contains(mountPoint)) << subfolders.join(u',').toStdString();
#else
    GTEST_SKIP() << "macOS volume layout";
#endif
}

// ---------------------------------------------------------------------------------------
// #21 / #22 / F-OPEN-09
// ---------------------------------------------------------------------------------------

TEST(ReviewNavigation, F_NAV_06_GoToSaturatesInsteadOfOverflowing) {
    EXPECT_EQ(nav::parseGoTo(QStringLiteral("+9223372036854775807"), 5, 100), 99);
    EXPECT_EQ(nav::parseGoTo(QStringLiteral("+99999999999999999999999"), 5, 100), 99);
    EXPECT_EQ(nav::parseGoTo(QStringLiteral("-99999999999999999999999"), 5, 100), 0);
    EXPECT_EQ(nav::parseGoTo(QStringLiteral("99999999999999999999999"), 5, 100), 99);
    EXPECT_EQ(nav::parseGoTo(QStringLiteral("+3"), 5, 100), 8);
    EXPECT_EQ(nav::parseGoTo(QString::fromUtf8("\xE2\x88\x92"
                                               "2"),
                             5, 100),
              3); // U+2212
    EXPECT_FALSE(nav::parseGoTo(QStringLiteral("+"), 5, 100).has_value());
    EXPECT_FALSE(nav::parseGoTo(QStringLiteral("1e5"), 5, 100).has_value());
    EXPECT_FALSE(nav::parseGoTo(QStringLiteral("--3"), 5, 100).has_value());
}

// `M 5` into a target that is not set (or fails) must not make `M M` repeat it.
TEST(ReviewQuickMove, F_FILE_08_TargetRememberedOnlyBySuccess) {
    QuickChord::setLastSlot(2);
    QuickChord c;
    c.start(QuickOp::Move);
    const QuickChord::Result r = c.feed(Qt::Key_5, Qt::NoModifier);
    EXPECT_EQ(r.kind, QuickChord::Kind::Execute);
    EXPECT_EQ(r.slot, 5);
    EXPECT_EQ(QuickChord::lastSlot(), 2) << "remembered before the move ran";
    c.start(QuickOp::Move);
    const QuickChord::Result again = c.feed(Qt::Key_M, Qt::NoModifier);
    EXPECT_EQ(again.slot, 2);
    QuickChord::setLastSlot(0);
}

// ⌥←/⌥→ are ordinary, configurable shortcuts of the folder actions (not wired into the
// arrow slot), only while not zoomed; zoomed they belong to the arrow slot (F-IN-07).
TEST(ReviewNavigation, F_OPEN_09_OptionArrowsAreConfigurableShortcuts) {
    test::ensureApp();
    QTemporaryDir tmp;
    Settings::useTestingBackend(tmp.filePath(QStringLiteral("input.ini")));
    InputBindings b;
    EXPECT_TRUE(InputBindings::defaultShortcuts(Action::NextFolder)
                    .contains(QKeySequence(Qt::AltModifier | Qt::Key_Right)));
    EXPECT_TRUE(InputBindings::defaultShortcuts(Action::PreviousFolder)
                    .contains(QKeySequence(Qt::AltModifier | Qt::Key_Left)));
    InputEvent ev;
    ev.kind = InputKind::Key;
    ev.key = Qt::Key_Right;
    ev.modifiers = Qt::AltModifier;
    EXPECT_EQ(b.resolve(ev), Action::NextFolder);
    ev.zoomed = true;
    EXPECT_NE(b.resolve(ev), Action::NextFolder); // F-IN-07 (pan or flip)
    EXPECT_NE(b.resolve(ev), Action::None);
    ev.zoomed = false;
    // Rebound: the old keys do nothing, the new ones switch folders.
    b.setShortcuts(Action::NextFolder, {QKeySequence(Qt::ShiftModifier | Qt::Key_Right)});
    EXPECT_EQ(b.resolve(ev), Action::None);
    ev.modifiers = Qt::ShiftModifier;
    EXPECT_EQ(b.resolve(ev), Action::NextFolder);
    ev.key = Qt::Key_Left;
    ev.modifiers = Qt::AltModifier;
    EXPECT_EQ(b.resolve(ev), Action::PreviousFolder);
    EXPECT_TRUE(b.conflictsWith(Action::NextFolder).isEmpty());
}
