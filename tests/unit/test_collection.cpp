// SPDX-License-Identifier: GPL-3.0-or-later
// Collection: folder enumeration, sorting and navigation
// (F-OPEN-02..07, F-NAV-01..03, F-SORT-02/03/07, F-PRIV-01).

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/Platform.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lf;

namespace {

void touch(const QString& path, const QByteArray& content = "x", const QDateTime& mtime = {}) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << path.toStdString();
    f.write(content);
    f.flush(); // a later flush on close would bump the mtime again
    if (mtime.isValid())
        f.setFileTime(mtime, QFileDevice::FileModificationTime);
}

QStringList names(const QVector<FileEntry>& entries) {
    QStringList out;
    for (const auto& e : entries)
        out << e.name;
    return out;
}

QStringList names(const Collection& c) {
    return names(c.entries());
}

class CollectionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test::ensureApp();
        ASSERT_TRUE(dir.isValid());
        collator = createNameCollator();
    }
    QString path(const QString& name) const { return QDir::cleanPath(dir.path()) + "/" + name; }
    std::unique_ptr<Collection> makeCollection() {
        auto c = std::make_unique<Collection>();
        c->setProviders(collator.get(), &order, &attributes);
        QObject::connect(c.get(), &Collection::loadingFinished, [this] { ++finished; });
        return c;
    }
    bool waitLoaded(Collection& c, int expectedFinished) {
        return test::waitUntil([&] { return finished >= expectedFinished && !c.isLoading(); });
    }

    QTemporaryDir dir;
    std::unique_ptr<NameCollator> collator;
    test::FakeOrderProvider order;
    test::FakeAttributeProvider attributes;
    int finished = 0;
};

FileEntry entry(const QString& name, qint64 size = -1) {
    FileEntry e;
    e.name = name;
    e.path = "/x/" + name;
    e.size = size;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------------------
// enumerateFolder
// ---------------------------------------------------------------------------------------

// Covers: F-OPEN-06
TEST_F(CollectionTest, F_OPEN_06_EnumerateFiltersHiddenPackagesAndNonImages) {
    touch(path("a.jpg"));
    touch(path("B.PNG"));
    touch(path("c.heic"));
    touch(path(".hidden.jpg"));
    touch(path("._a.jpg")); // AppleDouble
    touch(path("notes.txt"));
    touch(path("noext"));
    touch(path(".DS_Store"));
    ASSERT_TRUE(QDir().mkpath(path("folder.jpg")));
    ASSERT_TRUE(QDir().mkpath(path("Package.app/Contents")));
    ASSERT_TRUE(QDir().mkpath(path("Library.photoslibrary")));
    ASSERT_TRUE(QFile::link(path("a.jpg"), path("link.jpg")));
    ASSERT_TRUE(QFile::link(path("missing.jpg"), path("dangling.jpg")));
#ifdef Q_OS_MACOS
    touch(path("flagged.jpg"));
    ASSERT_EQ(::chflags(QFile::encodeName(path("flagged.jpg")).constData(), UF_HIDDEN), 0);
#endif
    bool denied = true;
    auto entries = collection::enumerateFolder(dir.path(), &denied);
    EXPECT_FALSE(denied);
    collection::sortEntries(entries, SortSpec{}, *collator);
    EXPECT_EQ(names(entries), (QStringList{"a.jpg", "B.PNG", "c.heic", "link.jpg"}));
    for (const auto& e : entries) {
        EXPECT_EQ(e.path, path(e.name));
        EXPECT_EQ(e.size, 1);
        EXPECT_TRUE(e.modified.isValid());
    }
}

TEST_F(CollectionTest, F_PRIV_01_EnumerateReportsAccessDenied) {
#ifdef Q_OS_UNIX
    if (::geteuid() == 0)
        GTEST_SKIP() << "root ignores permissions";
    const QString locked = path("locked");
    ASSERT_TRUE(QDir().mkpath(locked));
    touch(locked + "/a.jpg");
    ASSERT_EQ(::chmod(QFile::encodeName(locked).constData(), 0), 0);
    bool denied = false;
    const auto entries = collection::enumerateFolder(locked, &denied);
    ::chmod(QFile::encodeName(locked).constData(), 0755);
    EXPECT_TRUE(denied);
    EXPECT_TRUE(entries.isEmpty());
    // A missing folder is not "access denied".
    collection::enumerateFolder(path("does-not-exist"), &denied);
    EXPECT_FALSE(denied);
#endif
}

// ---------------------------------------------------------------------------------------
// sortEntries
// ---------------------------------------------------------------------------------------

TEST_F(CollectionTest, F_SORT_01_NameOrderIsFinderLike) {
    QVector<FileEntry> v = {entry("IMG_10.jpg"), entry("img_2.jpg"), entry("IMG_1.jpg"), entry("b.jpg"),
                            entry("A.jpg")};
    collection::sortEntries(v, SortSpec{SortKey::Name, true}, *collator);
    EXPECT_EQ(names(v), (QStringList{"A.jpg", "b.jpg", "IMG_1.jpg", "img_2.jpg", "IMG_10.jpg"}));
    collection::sortEntries(v, SortSpec{SortKey::Name, false}, *collator);
    EXPECT_EQ(names(v), (QStringList{"IMG_10.jpg", "img_2.jpg", "IMG_1.jpg", "b.jpg", "A.jpg"}));
}

TEST_F(CollectionTest, F_SORT_02_EveryKeyBothDirectionsWithNameTieBreak) {
    const QDateTime base(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC);
    auto make = [&](const QString& name, int v) {
        FileEntry e = entry(name, v);
        e.modified = base.addSecs(v);
        e.created = base.addSecs(v * 2);
        e.added = base.addSecs(v * 3);
        e.captured = base.addSecs(v * 4);
        e.kind = QString("Kind %1").arg(v);
        e.pixelCount = v * 100;
        e.rating = v % 6;
        return e;
    };
    // c and b share value 2 -> tie broken by name ascending in both directions.
    const QVector<FileEntry> input = {make("c.jpg", 2), make("a.jpg", 1), make("d.jpg", 3), make("b.jpg", 2)};
    for (SortKey key :
         {SortKey::DateModified, SortKey::DateCreated, SortKey::DateAdded, SortKey::DateCaptured,
          SortKey::Size, SortKey::Kind, SortKey::PixelCount, SortKey::Rating}) {
        QVector<FileEntry> v = input;
        collection::sortEntries(v, SortSpec{key, true}, *collator);
        EXPECT_EQ(names(v), (QStringList{"a.jpg", "b.jpg", "c.jpg", "d.jpg"})) << int(key);
        collection::sortEntries(v, SortSpec{key, false}, *collator);
        EXPECT_EQ(names(v), (QStringList{"d.jpg", "b.jpg", "c.jpg", "a.jpg"})) << int(key);
    }
}

TEST_F(CollectionTest, F_SORT_02_UnknownValuesSortLast) {
    QVector<FileEntry> v = {entry("a.jpg", -1), entry("b.jpg", 5), entry("c.jpg", 1), entry("d.jpg", -1)};
    collection::sortEntries(v, SortSpec{SortKey::Size, true}, *collator);
    EXPECT_EQ(names(v), (QStringList{"c.jpg", "b.jpg", "a.jpg", "d.jpg"}));
    collection::sortEntries(v, SortSpec{SortKey::Size, false}, *collator);
    EXPECT_EQ(names(v), (QStringList{"b.jpg", "c.jpg", "a.jpg", "d.jpg"}));
    // Captured date unknown for everyone -> pure name order.
    collection::sortEntries(v, SortSpec{SortKey::DateCaptured, false}, *collator);
    EXPECT_EQ(names(v), (QStringList{"a.jpg", "b.jpg", "c.jpg", "d.jpg"}));
}

TEST_F(CollectionTest, F_SORT_03_IconPositionOrderRowsThenColumns) {
    QVector<FileEntry> v = {entry("a.jpg"), entry("b.jpg"), entry("c.jpg"), entry("d.jpg"), entry("e.jpg")};
    v[0].finderIconPosition = QPointF(300, 100); // row 1, right
    v[1].finderIconPosition = QPointF(100, 100); // row 1, left
    v[2].finderIconPosition = QPointF(50, 250);  // row 2
    // d, e without position -> after, by name
    collection::sortEntries(v, SortSpec{SortKey::FinderIconPosition, true}, *collator);
    EXPECT_EQ(names(v), (QStringList{"b.jpg", "a.jpg", "c.jpg", "d.jpg", "e.jpg"}));
}

TEST_F(CollectionTest, F_SORT_02_SortIsDeterministic) {
    QVector<FileEntry> v;
    for (int i = 0; i < 200; ++i)
        v.append(entry(QString("f%1.jpg").arg((i * 37) % 200), i % 3));
    QVector<FileEntry> w = v;
    std::reverse(w.begin(), w.end());
    collection::sortEntries(v, SortSpec{SortKey::Size, true}, *collator);
    collection::sortEntries(w, SortSpec{SortKey::Size, true}, *collator);
    EXPECT_EQ(names(v), names(w));
}

// ---------------------------------------------------------------------------------------
// Collection
// ---------------------------------------------------------------------------------------

// Covers: F-OPEN-02, F-OPEN-03
TEST_F(CollectionTest, F_OPEN_02_F_OPEN_03_OpenFileShowsImmediatelyThenFolder) {
    for (const char* n : {"IMG_10.jpg", "IMG_2.jpg", "IMG_1.jpg", "notes.txt", "x.png"})
        touch(path(n));
    order.delayMs = 50;
    auto c = makeCollection();
    QStringList currents;
    QObject::connect(c.get(), &Collection::currentChanged,
                     [&](int, const QString& p) { currents << QFileInfo(p).fileName(); });
    c->openFile(path("IMG_2.jpg"));
    // Visible at once, before enumeration finished.
    EXPECT_EQ(c->count(), 1);
    EXPECT_EQ(c->currentIndex(), 0);
    EXPECT_EQ(c->currentPath(), path("IMG_2.jpg"));
    EXPECT_TRUE(c->isLoading());
    EXPECT_TRUE(c->isFolderCollection());
    EXPECT_EQ(c->folderPath(), QDir::cleanPath(dir.path()));
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(names(*c), (QStringList{"IMG_1.jpg", "IMG_2.jpg", "IMG_10.jpg", "x.png"}));
    EXPECT_EQ(c->currentIndex(), 1);
    EXPECT_EQ(c->currentPath(), path("IMG_2.jpg"));
    EXPECT_EQ(c->sortSpec().source, SortSource::FinderDsStore);
    EXPECT_EQ(currents, (QStringList{"IMG_2.jpg", "IMG_2.jpg"}));
}

TEST_F(CollectionTest, F_OPEN_02_ExplicitlyOpenedUnlistedFileIsKept) {
    touch(path("a.jpg"));
    touch(path(".secret.jpg"));
    auto c = makeCollection();
    c->openFile(path(".secret.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(c->count(), 2);
    EXPECT_EQ(c->currentPath(), path(".secret.jpg"));
}

// Covers: F-OPEN-04
TEST_F(CollectionTest, F_OPEN_04_OpenFilesIsExactlyThoseFiles) {
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg", "d.jpg"})
        touch(path(n));
    auto c = makeCollection();
    c->openFiles({path("d.jpg"), path("b.jpg"), path("b.jpg"), path("missing.jpg")});
    EXPECT_FALSE(c->isFolderCollection());
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(names(*c), (QStringList{"b.jpg", "d.jpg"}));
    EXPECT_EQ(c->currentPath(), path("d.jpg")); // the first requested file stays current
    EXPECT_EQ(c->folderPath(), QDir::cleanPath(dir.path()));
}

// Covers: F-OPEN-05
TEST_F(CollectionTest, F_OPEN_05_OpenFolderStartsAtFirst) {
    for (const char* n : {"b.jpg", "a.jpg", "c.jpg"})
        touch(path(n));
    auto c = makeCollection();
    c->openFolder(dir.path());
    EXPECT_EQ(c->count(), 0);
    EXPECT_EQ(c->currentIndex(), -1);
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "b.jpg", "c.jpg"}));
    EXPECT_EQ(c->currentIndex(), 0);
}

// Covers: F-NAV-01, F-NAV-02, F-NAV-03
TEST_F(CollectionTest, F_NAV_01_F_NAV_02_F_NAV_03_Navigation) {
    for (const char* n : {"1.jpg", "2.jpg", "3.jpg", "4.jpg"})
        touch(path(n));
    auto c = makeCollection();
    c->openFolder(dir.path());
    ASSERT_TRUE(waitLoaded(*c, 1));
    ASSERT_EQ(c->count(), 4);
    EXPECT_FALSE(c->previous(WrapMode::Stop));
    EXPECT_TRUE(c->next(WrapMode::Stop));
    EXPECT_EQ(c->currentIndex(), 1);
    EXPECT_TRUE(c->last());
    EXPECT_EQ(c->currentIndex(), 3);
    EXPECT_FALSE(c->last());
    EXPECT_FALSE(c->next(WrapMode::Stop));
    EXPECT_TRUE(c->next(WrapMode::Wrap));
    EXPECT_EQ(c->currentIndex(), 0);
    EXPECT_TRUE(c->previous(WrapMode::Wrap));
    EXPECT_EQ(c->currentIndex(), 3);
    EXPECT_TRUE(c->first());
    EXPECT_FALSE(c->first());
    EXPECT_TRUE(c->jump(10, WrapMode::Stop));
    EXPECT_EQ(c->currentIndex(), 3);
    EXPECT_TRUE(c->jump(-2, WrapMode::Stop));
    EXPECT_EQ(c->currentIndex(), 1);
    EXPECT_TRUE(c->jump(5, WrapMode::Wrap));
    EXPECT_EQ(c->currentIndex(), 2);
    EXPECT_TRUE(c->jump(-7, WrapMode::Wrap));
    EXPECT_EQ(c->currentIndex(), 3);
    EXPECT_FALSE(c->jump(0, WrapMode::Wrap));
    EXPECT_TRUE(c->setCurrentPath(path("2.jpg")));
    EXPECT_EQ(c->currentIndex(), 1);
    EXPECT_FALSE(c->setCurrentPath(path("nope.jpg")));
    c->setCurrentIndex(99);
    EXPECT_EQ(c->currentIndex(), 1);
    EXPECT_EQ(c->indexOf(path("4.jpg")), 3);
}

// Covers: F-OPEN-07, F-SORT-07
TEST_F(CollectionTest, F_OPEN_07_F_SORT_07_RefreshKeepsCurrentAndHandlesRemoval) {
    for (const char* n : {"a.jpg", "c.jpg", "e.jpg"})
        touch(path(n));
    auto c = makeCollection();
    c->openFile(path("c.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    touch(path("b.jpg"));
    touch(path("d.jpg"));
    c->refresh();
    ASSERT_TRUE(waitLoaded(*c, 2));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "b.jpg", "c.jpg", "d.jpg", "e.jpg"}));
    EXPECT_EQ(c->currentPath(), path("c.jpg"));
    // Current file deleted externally -> move to the next image.
    ASSERT_TRUE(QFile::remove(path("c.jpg")));
    ASSERT_TRUE(QFile::remove(path("d.jpg")));
    c->refresh();
    ASSERT_TRUE(waitLoaded(*c, 3));
    EXPECT_EQ(c->currentPath(), path("e.jpg"));
    // Last one deleted -> previous neighbour.
    ASSERT_TRUE(QFile::remove(path("e.jpg")));
    c->refresh();
    ASSERT_TRUE(waitLoaded(*c, 4));
    EXPECT_EQ(c->currentPath(), path("b.jpg"));
}

TEST_F(CollectionTest, F_OPEN_07_RemoveAndInsertPath) {
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        touch(path(n));
    auto c = makeCollection();
    c->openFile(path("b.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    int currentSignals = 0;
    QObject::connect(c.get(), &Collection::currentChanged, [&] { ++currentSignals; });
    c->removePath(path("b.jpg")); // current removed -> next takes its place
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "c.jpg"}));
    EXPECT_EQ(c->currentPath(), path("c.jpg"));
    EXPECT_EQ(currentSignals, 1);
    c->insertPath(path("b.jpg")); // undo: back at its sorted position, current unchanged
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "b.jpg", "c.jpg"}));
    EXPECT_EQ(c->currentPath(), path("c.jpg"));
    EXPECT_EQ(c->currentIndex(), 2);
    c->insertPath(path("b.jpg")); // duplicate ignored
    EXPECT_EQ(c->count(), 3);
    c->removePath(path("a.jpg"));
    EXPECT_EQ(c->currentPath(), path("c.jpg"));
    EXPECT_EQ(c->currentIndex(), 1);
    c->removePath(path("c.jpg")); // last removed -> previous
    EXPECT_EQ(c->currentPath(), path("b.jpg"));
    c->removePath(path("b.jpg"));
    EXPECT_EQ(c->count(), 0);
    EXPECT_EQ(c->currentIndex(), -1);
}

TEST_F(CollectionTest, F_SORT_02_ManualSortKeepsCurrent) {
    const QDateTime base(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC);
    touch(path("a.jpg"), "1", base.addDays(3));
    touch(path("b.jpg"), "1", base.addDays(1));
    touch(path("c.jpg"), "1", base.addDays(2));
    auto c = makeCollection();
    c->openFile(path("c.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    QVector<SortSpec> specs;
    QObject::connect(c.get(), &Collection::sortSpecChanged, [&](const SortSpec& s) { specs << s; });
    c->setSortSpec({SortKey::DateModified, false, SortSource::FinderGlobal});
    EXPECT_FALSE(c->followsFinder());
    EXPECT_EQ(c->sortSpec().source, SortSource::Manual);
    ASSERT_TRUE(waitLoaded(*c, 2));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "c.jpg", "b.jpg"}));
    EXPECT_EQ(c->currentPath(), path("c.jpg"));
    ASSERT_FALSE(specs.isEmpty());
    EXPECT_EQ(specs.first().key, SortKey::DateModified);
    // Back to "Wie Finder".
    c->setFollowFinder(true, false);
    ASSERT_TRUE(waitLoaded(*c, 3));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "b.jpg", "c.jpg"}));
    EXPECT_EQ(c->sortSpec().source, SortSource::FinderDsStore);
    EXPECT_EQ(c->currentPath(), path("c.jpg"));
}

TEST_F(CollectionTest, F_SORT_03_FollowFinderUsesProviderAndAttributes) {
    const QDateTime base(QDate(2024, 1, 1), QTime(0, 0), QTimeZone::UTC);
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        touch(path(n));
    attributes.added = {{"a.jpg", base.addDays(1)}, {"b.jpg", base.addDays(3)}, {"c.jpg", base.addDays(2)}};
    order.setSpec({SortKey::DateAdded, false, SortSource::FinderDsStore});
    auto c = makeCollection();
    c->setFollowFinder(true, true);
    c->openFolder(dir.path());
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(names(*c), (QStringList{"b.jpg", "c.jpg", "a.jpg"}));
    EXPECT_TRUE(order.lastAllowLive.load());
    EXPECT_EQ(describe(c->sortSpec()), QString::fromUtf8("Wie Finder: Hinzugefügt am ↓"));
}

TEST_F(CollectionTest, F_SORT_03_IconPositionsAppliedFromProvider) {
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        touch(path(n));
    order.setSpec({SortKey::FinderIconPosition, true, SortSource::FinderDsStore});
    order.positions = {{"c.jpg", QPointF(10, 10)}, {"a.jpg", QPointF(200, 10)}};
    auto c = makeCollection();
    c->openFolder(dir.path());
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(names(*c), (QStringList{"c.jpg", "a.jpg", "b.jpg"}));
}

// Covers: F-OPEN-03
TEST_F(CollectionTest, F_OPEN_03_NewerOpenCancelsOlder) {
    QTemporaryDir other;
    ASSERT_TRUE(other.isValid());
    for (const char* n : {"a.jpg", "b.jpg"})
        touch(path(n));
    touch(other.filePath("z1.jpg"));
    touch(other.filePath("z2.jpg"));
    order.delayMs = 150;
    auto c = makeCollection();
    c->openFile(path("a.jpg"));
    c->openFile(other.filePath("z2.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    // Give a stale result the chance to arrive (it must be ignored).
    test::waitUntil([] { return false; }, 300);
    EXPECT_EQ(finished, 1);
    EXPECT_EQ(names(*c), (QStringList{"z1.jpg", "z2.jpg"}));
    EXPECT_EQ(c->currentPath(), QDir::cleanPath(other.filePath("z2.jpg")));
}

namespace {

// Attribute provider whose fill() blocks (once armed) until released: holds a load job
// after enumeration so GUI-thread edits can race it deterministically.
class BlockingAttributeProvider : public FileAttributeProvider {
  public:
    void fill(FileEntry&) override {
        if (!armed.load())
            return;
        if (entered.fetch_add(1) == 0)
            started.release();
        release.acquire();
        release.release(); // let every further fill() pass
    }
    std::atomic<bool> armed{false};
    std::atomic<int> entered{0};
    QSemaphore started;
    QSemaphore release;
};

} // namespace

// Review finding: removePath()/insertPath() during a pending load were overwritten by the
// job's stale snapshot, e.g. a just-trashed file reappeared.
TEST_F(CollectionTest, F_OPEN_07_EditsDuringPendingLoadSurvive) {
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        touch(path(n));
    BlockingAttributeProvider blocking;
    auto c = std::make_unique<Collection>();
    c->setProviders(collator.get(), &order, &blocking);
    QObject::connect(c.get(), &Collection::loadingFinished, [this] { ++finished; });
    c->openFile(path("a.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    ASSERT_EQ(names(*c), (QStringList{"a.jpg", "b.jpg", "c.jpg"}));

    blocking.armed = true;
    c->refresh();
    ASSERT_TRUE(blocking.started.tryAcquire(1, 5000)); // the job has enumerated a, b, c
    blocking.armed = false;                            // insertPath() fills on this thread
    // Trash b, restore-like insert of d, both while the job is blocked.
    ASSERT_TRUE(QFile::remove(path("b.jpg")));
    c->removePath(path("b.jpg"));
    touch(path("d.jpg"));
    c->insertPath(path("d.jpg"));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "c.jpg", "d.jpg"}));
    blocking.release.release();
    ASSERT_TRUE(waitLoaded(*c, 2));
    EXPECT_EQ(names(*c), (QStringList{"a.jpg", "c.jpg", "d.jpg"}));
    EXPECT_EQ(c->indexOf(path("b.jpg")), -1);
}

TEST_F(CollectionTest, F_OPEN_07_RemoveDuringPendingResortSurvives) {
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        touch(path(n));
    auto c = makeCollection();
    c->openFile(path("a.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 1));
    order.delayMs = 0;
    // The resort job snapshots the entries synchronously; the removal happens after it.
    c->setSortSpec({SortKey::Name, false, SortSource::Manual});
    c->removePath(path("b.jpg"));
    ASSERT_TRUE(waitLoaded(*c, 2));
    EXPECT_EQ(names(*c), (QStringList{"c.jpg", "a.jpg"}));
}

// Non-ASCII names: files created with decomposed (NFD) bytes, as Finder does, opened via
// the NFC spelling Qt and most callers use. The opened file must not appear twice.
TEST_F(CollectionTest, F_OPEN_02_NonAsciiNfdNamesOpenOnce) {
#ifdef Q_OS_UNIX
    const QByteArray nfdA("A\xcc\x88pfel.jpg"); // "Äpfel.jpg" decomposed
    const QByteArray nfdB("Mu\xcc\x88ll.jpg");  // "Müll.jpg" decomposed
    for (const QByteArray& n : {nfdA, nfdB}) {
        const QByteArray p = QFile::encodeName(QDir::cleanPath(dir.path())) + '/' + n;
        const int fd = ::open(p.constData(), O_CREAT | O_WRONLY, 0644);
        ASSERT_GE(fd, 0);
        ::close(fd);
    }
    auto c = makeCollection();
    const QString opened = path(QString::fromUtf8("Müll.jpg")); // NFC
    c->openFile(opened);
    ASSERT_TRUE(waitLoaded(*c, 1));
    EXPECT_EQ(c->count(), 2) << names(*c).join(u'|').toStdString();
    EXPECT_GE(c->indexOf(opened), 0);
    EXPECT_EQ(c->currentIndex(), c->indexOf(opened));
    QStringList nfc;
    for (const QString& n : names(*c))
        nfc << n.normalized(QString::NormalizationForm_C);
    EXPECT_EQ(nfc, (QStringList{QString::fromUtf8("Äpfel.jpg"), QString::fromUtf8("Müll.jpg")}));
#endif
}

// Covers: F-PRIV-01
TEST_F(CollectionTest, F_PRIV_01_AccessDeniedKeepsOpenedFile) {
#ifdef Q_OS_UNIX
    if (::geteuid() == 0)
        GTEST_SKIP() << "root ignores permissions";
    const QString locked = path("locked");
    ASSERT_TRUE(QDir().mkpath(locked));
    touch(locked + "/a.jpg");
    touch(locked + "/b.jpg");
    // Directory readable for stat but not listable: x without r.
    ASSERT_EQ(::chmod(QFile::encodeName(locked).constData(), 0100), 0);
    auto c = makeCollection();
    QStringList denied;
    QObject::connect(c.get(), &Collection::accessDenied, [&](const QString& f) { denied << f; });
    c->openFile(locked + "/b.jpg");
    const bool ok = waitLoaded(*c, 1);
    ::chmod(QFile::encodeName(locked).constData(), 0755);
    ASSERT_TRUE(ok);
    EXPECT_EQ(denied, QStringList{locked});
    EXPECT_EQ(c->count(), 1);
    EXPECT_EQ(c->currentPath(), locked + "/b.jpg");
#endif
}
