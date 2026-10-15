// SPDX-License-Identifier: GPL-3.0-or-later
// M3 collection features: subfolders (F-OPEN-08), sibling folders (F-OPEN-09) and RAW+JPEG
// pairs as one element (F-FMT-06).

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/FileBatch.h"
#include "core/FolderCache.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/RatingStore.h"
#include "core/Settings.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

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

// Slow attributes: makes a recursive load take long enough to observe progress and cancel.
class SlowAttributes : public FileAttributeProvider {
public:
    std::atomic<int> delayMs{0};
    void fill(FileEntry&) override {
        if (delayMs > 0)
            QThread::msleep(ulong(delayMs.load()));
    }
};

// Folder watcher stand-in: records watch calls and reports changes on demand.
class FakeWatcher : public FolderWatcher {
public:
    QStringList watched, trees;
    void watch(const QString& f) override { watched << f; }
    void unwatch(const QString& f) override { watched.removeOne(f); }
    void watchTree(const QString& f) override { trees << f; }
    void unwatchTree(const QString& f) override { trees.removeOne(f); }
    void fire(const QString& f) { emit folderChanged(f); }
};

class TrashToFolder : public TrashService {
public:
    explicit TrashToFolder(QString dir) : m_dir(std::move(dir)) {}
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QString to = m_dir + u'/' + QFileInfo(path).fileName();
        if (!QFile::rename(path, to)) {
            if (error)
                *error = QStringLiteral("rename failed");
            return std::nullopt;
        }
        trashed << path;
        return to;
    }
    bool restoreFromTrash(const QString& trashedPath, const QString& originalPath, QString*) override {
        return QFile::rename(trashedPath, originalPath);
    }
    QStringList trashed;

private:
    QString m_dir;
};

class M3CollectionTest : public ::testing::Test {
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

    QTemporaryDir dir;
    QString root;
    std::unique_ptr<NameCollator> collator;
    test::FakeOrderProvider order;
    test::FakeAttributeProvider attributes;
};

} // namespace

// ---------------------------------------------------------------------------------------
// F-OPEN-08 subfolders
// ---------------------------------------------------------------------------------------

TEST(M3CollectionPure, F_OPEN_08_FolderOrderIsPathWiseFinderOrder) {
    auto coll = createNameCollator();
    auto lt = [&](const char* a, const char* b) {
        return collection::compareFolderPaths(QString::fromUtf8(a), QString::fromUtf8(b), *coll) < 0;
    };
    EXPECT_TRUE(lt("/r", "/r/A"));                  // a folder before its subfolders
    EXPECT_TRUE(lt("/r/A/Z", "/r/B"));              // depth-first
    EXPECT_TRUE(lt("/r/Ordner 2", "/r/Ordner 10")); // Finder (numeric) name order
    EXPECT_FALSE(lt("/r/B", "/r/A/Z"));
    EXPECT_EQ(collection::compareFolderPaths("/r/x", "/r/x", *coll), 0);
}

TEST_F(M3CollectionTest, F_OPEN_08_RecursiveEnumerationAndOrder) {
    touch(path("b.jpg"));
    touch(path("a.jpg"));
    touch(path("Ordner 10/d.jpg"));
    touch(path("Ordner 2/c.jpg"));
    touch(path("Ordner 2/Tief/e.jpg"));
    touch(path("Ordner 2/Tief/notes.txt"));
    touch(path(".versteckt/x.jpg"));         // hidden folder
    touch(path("Paket.app/Contents/y.jpg")); // package
    QDir().mkpath(path("leer"));
    auto c = make();
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(relNames(*c),
              (QStringList{"a.jpg", "b.jpg", "Ordner 2/c.jpg", "Ordner 2/Tief/e.jpg", "Ordner 10/d.jpg"}));
    EXPECT_EQ(c->currentPath(), path("a.jpg"));
    const QStringList folders = c->folders();
    EXPECT_EQ(folders.first(), root);
    EXPECT_TRUE(folders.contains(path("Ordner 2/Tief")));
    EXPECT_TRUE(folders.contains(path("leer")));
    EXPECT_FALSE(folders.contains(path(".versteckt")));
    EXPECT_FALSE(folders.contains(path("Paket.app")));
    EXPECT_EQ(c->relativeFolder(path("Ordner 2/Tief/e.jpg")), QStringLiteral("Ordner 2/Tief"));
    EXPECT_EQ(c->relativeFolder(path("a.jpg")), QString());
    // Leases: every folder of the tree keeps its shared snapshot (F-WIN-09).
    EXPECT_GE(FolderCache::instance().leaseCount(path("Ordner 2/Tief")), 1);

    // A manual sort applies within each folder; the folder order stays.
    c->setSortSpec(SortSpec{SortKey::Name, false, SortSource::Manual});
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(relNames(*c),
              (QStringList{"b.jpg", "a.jpg", "Ordner 2/c.jpg", "Ordner 2/Tief/e.jpg", "Ordner 10/d.jpg"}));

    // Off again: only the folder itself, the subfolder leases are given back.
    c->setIncludeSubfolders(false);
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->count(), 2);
    EXPECT_EQ(c->folders(), QStringList{root});
    EXPECT_EQ(FolderCache::instance().leaseCount(path("Ordner 2/Tief")), 0);
}

TEST_F(M3CollectionTest, F_OPEN_08_OpenFileKeepsFileAndFindsSubfolders) {
    touch(path("a.jpg"));
    touch(path("Sub/b.jpg"));
    auto c = make();
    c->setIncludeSubfolders(true);
    c->openFile(path("a.jpg"));
    EXPECT_EQ(c->currentPath(), path("a.jpg")); // shown at once (F-OPEN-03)
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->count(), 2);
    EXPECT_EQ(c->currentPath(), path("a.jpg"));
    EXPECT_TRUE(c->next(WrapMode::Stop));
    EXPECT_EQ(c->currentPath(), path("Sub/b.jpg"));
}

TEST_F(M3CollectionTest, F_OPEN_08_CancellableWithProgress) {
    // 30 folders x 4 images, 8 ms per image: about a second to read everything.
    for (int f = 0; f < 30; ++f)
        for (int i = 0; i < 4; ++i)
            touch(path(QStringLiteral("F%1/img%2.jpg").arg(f, 2, 10, QLatin1Char('0')).arg(i)));
    SlowAttributes slow;
    slow.delayMs = 8;
    auto c = make(&slow);
    QList<int> progress;
    QObject::connect(c.get(), &Collection::loadProgress, [&](int folders, int) { progress << folders; });
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    EXPECT_TRUE(c->isLoading()); // the GUI thread is not blocked by the enumeration
    ASSERT_TRUE(test::waitUntil([&] { return !progress.isEmpty() && progress.last() > 0; }, 5000));
    EXPECT_TRUE(c->isLoading());
    // A new open cancels the running walk: the later folders are never read.
    touch(path("../other_m3_cancel/z.jpg"));
    const QString other = QDir::cleanPath(path("../other_m3_cancel"));
    c->setIncludeSubfolders(false);
    c->openFolder(other);
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->count(), 1);
    QThread::msleep(100); // the cancelled worker stops at the next folder
    EXPECT_EQ(FolderCache::instance().enumerationCount(path("F29")), 0);
    QFile::remove(other + "/z.jpg");
    QDir().rmdir(other);
}

TEST_F(M3CollectionTest, F_OPEN_08_SubfolderChangesViaWatcher) {
    touch(path("a.jpg"));
    touch(path("Sub/b.jpg"));
    auto c = make();
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    ASSERT_EQ(c->count(), 2);

    // The app wiring (Application/ViewerWindow): a recursive collection is watched as a
    // tree; a change invalidates that folder's snapshot and covering windows reload.
    FakeWatcher watcher;
    watcher.watchTree(root);
    QObject::connect(&watcher, &FolderWatcher::folderChanged, [&](const QString& folder) {
        FolderCache::instance().invalidate(folder);
        if (c->coversFolder(folder))
            c->reload();
    });
    EXPECT_EQ(watcher.trees, QStringList{root});

    touch(path("Sub/c.jpg"));
    watcher.fire(path("Sub"));
    ASSERT_TRUE(test::waitUntil([&] { return c->count() == 3 && !c->isLoading(); }));
    EXPECT_EQ(relNames(*c), (QStringList{"a.jpg", "Sub/b.jpg", "Sub/c.jpg"}));

    // A new subfolder appears through the event of its parent.
    touch(path("Sub/Neu/d.jpg"));
    EXPECT_TRUE(c->coversFolder(path("Sub/Neu")));
    EXPECT_FALSE(c->coversFolder(path(".hidden")));
    EXPECT_FALSE(c->coversFolder(QDir::cleanPath(root + "/..")));
    watcher.fire(path("Sub"));
    ASSERT_TRUE(test::waitUntil([&] { return c->count() == 4 && !c->isLoading(); }));
    EXPECT_TRUE(c->folders().contains(path("Sub/Neu")));
}

TEST_F(M3CollectionTest, F_OPEN_08_EditsInSubfolders) {
    touch(path("a.jpg"));
    touch(path("Sub/b.jpg"));
    touch(path("Sub/d.jpg"));
    auto c = make();
    c->setIncludeSubfolders(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    // insertPath into a subfolder: placed within its folder.
    touch(path("Sub/c.jpg"));
    c->insertPath(path("Sub/c.jpg"));
    ASSERT_TRUE(test::waitUntil([&] { return !c->hasPendingInserts(); }));
    EXPECT_EQ(relNames(*c), (QStringList{"a.jpg", "Sub/b.jpg", "Sub/c.jpg", "Sub/d.jpg"}));
    // A file trashed in a subfolder does not come back with a reload of the shared,
    // older snapshot.
    QFile::remove(path("Sub/b.jpg"));
    c->removePath(path("Sub/b.jpg"));
    c->reload();
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(relNames(*c), (QStringList{"a.jpg", "Sub/c.jpg", "Sub/d.jpg"}));
    // Files outside the tree are not taken.
    touch(path("../outside_m3.jpg"));
    c->insertPath(path("../outside_m3.jpg"));
    test::waitUntil([&] { return !c->hasPendingInserts(); });
    EXPECT_EQ(c->count(), 3);
    QFile::remove(QDir::cleanPath(path("../outside_m3.jpg")));
}

// ---------------------------------------------------------------------------------------
// F-OPEN-09 sibling folders
// ---------------------------------------------------------------------------------------

TEST(M3Navigation, F_OPEN_09_OptionArrowsAreFolderActions) {
    test::ensureApp();
    QTemporaryDir tmp;
    Settings::useTestingBackend(tmp.filePath(QStringLiteral("input.ini")));
    InputBindings b;
    InputEvent ev;
    ev.kind = InputKind::Key;
    ev.key = Qt::Key_Right;
    ev.modifiers = Qt::AltModifier;
    EXPECT_EQ(b.resolve(ev), Action::NextFolder);
    ev.key = Qt::Key_Left;
    EXPECT_EQ(b.resolve(ev), Action::PreviousFolder);
    EXPECT_EQ(actionFromId(QStringLiteral("nextFolder")), Action::NextFolder); // configurable action
    EXPECT_EQ(actionFromId(QStringLiteral("previousFolder")), Action::PreviousFolder);
}

TEST_F(M3CollectionTest, F_OPEN_09_SiblingFoldersSkipEmptyHiddenAndPackages) {
    for (const char* f : {"A", "B leer", "C.app", ".D", "E"})
        QDir().mkpath(path(QString::fromLatin1(f)));
    touch(path("A/1.jpg"));
    touch(path("C.app/2.jpg"));
    touch(path(".D/3.jpg"));
    touch(path("E/Sub/4.jpg")); // images only in a subfolder: E itself has none
    touch(path("F/5.jpg"));
    EXPECT_EQ(collection::siblingImageFolder(path("A"), 1, collator.get()), path("F"));
    EXPECT_EQ(collection::siblingImageFolder(path("F"), -1, collator.get()), path("A"));
    EXPECT_EQ(collection::siblingImageFolder(path("A"), -1, collator.get()), QString());
}

// ---------------------------------------------------------------------------------------
// F-FMT-06 RAW+JPEG pairs
// ---------------------------------------------------------------------------------------

TEST(M3PairsPure, F_FMT_06_PairingRules) {
    auto e = [](const char* p) {
        FileEntry x;
        x.path = QString::fromUtf8(p);
        x.name = QFileInfo(x.path).fileName();
        return x;
    };
    QVector<FileEntry> list{e("/a/IMG_1.CR2"), e("/a/img_1.jpg"),  e("/a/IMG_2.NEF"),
                            e("/a/IMG_3.JPG"), e("/b/IMG_2.jpg"),  e("/a/IMG_4.png"),
                            e("/a/IMG_4.dng"), e("/a/IMG_5.jpeg"), e("/a/IMG_5.ARW")};
    QHash<QString, FileEntry> partners;
    collection::pairRawJpeg(list, &partners);
    QStringList shown;
    for (const FileEntry& x : list)
        shown << x.path;
    EXPECT_EQ(shown, (QStringList{"/a/img_1.jpg", "/a/IMG_2.NEF", "/a/IMG_3.JPG", "/b/IMG_2.jpg",
                                  "/a/IMG_4.png", "/a/IMG_4.dng", "/a/IMG_5.jpeg"}));
    EXPECT_EQ(list[0].pairedPath, QStringLiteral("/a/IMG_1.CR2"));
    EXPECT_EQ(partners.value("/a/IMG_1.CR2").pairedPath, QStringLiteral("/a/img_1.jpg"));
    EXPECT_TRUE(list[1].pairedPath.isEmpty()); // other folder: no pair
    EXPECT_TRUE(list[4].pairedPath.isEmpty());
    EXPECT_EQ(partners.size(), 2);
    // The RAW stays shown when asked (explicitly opened / toggled).
    QVector<FileEntry> again{e("/a/IMG_1.CR2"), e("/a/IMG_1.jpg")};
    collection::pairRawJpeg(again, nullptr, {QStringLiteral("/a/IMG_1.CR2")});
    ASSERT_EQ(again.size(), 1);
    EXPECT_EQ(again[0].path, QStringLiteral("/a/IMG_1.CR2"));
    EXPECT_TRUE(collection::isPairPartner("/x/A.JPG", "/x/a.nef"));
    EXPECT_FALSE(collection::isPairPartner("/x/A.JPG", "/y/A.nef"));
    EXPECT_FALSE(collection::isPairPartner("/x/A.JPG", "/x/A.png"));
}

TEST_F(M3CollectionTest, F_FMT_06_PairsAsOneElementOptional) {
    touch(path("IMG_1.CR2"));
    touch(path("IMG_1.JPG"));
    touch(path("IMG_2.JPG"));
    auto c = make();
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->count(), 3); // default: both shown
    EXPECT_FALSE(c->pairsRawJpeg());

    c->setPairRawJpeg(true);
    ASSERT_TRUE(loaded(*c));
    ASSERT_EQ(c->count(), 2);
    EXPECT_EQ(c->at(0).path, path("IMG_1.JPG"));
    EXPECT_EQ(c->at(0).pairedPath, path("IMG_1.CR2"));
    EXPECT_EQ(c->pairedPath(path("IMG_1.CR2")), path("IMG_1.JPG"));
    EXPECT_EQ(c->totalCount(), 2);
    EXPECT_TRUE(c->allPaths().contains(path("IMG_1.CR2")));

    // Toggle: the RAW is shown at the same position and stays shown across reloads.
    c->setCurrentIndex(0);
    QString current;
    QObject::connect(c.get(), &Collection::currentChanged, [&](int, const QString& p) { current = p; });
    EXPECT_TRUE(c->togglePairMember(path("IMG_1.JPG")));
    EXPECT_EQ(current, path("IMG_1.CR2"));
    EXPECT_EQ(c->currentIndex(), 0);
    EXPECT_EQ(c->at(0).pairedPath, path("IMG_1.JPG"));
    c->reload();
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->currentPath(), path("IMG_1.CR2"));
    EXPECT_TRUE(c->togglePairMember(path("IMG_1.CR2")));
    EXPECT_EQ(c->currentPath(), path("IMG_1.JPG"));
    EXPECT_FALSE(c->togglePairMember(path("IMG_2.JPG"))); // no pair

    c->setPairRawJpeg(false);
    ASSERT_TRUE(loaded(*c));
    EXPECT_EQ(c->count(), 3);
}

TEST_F(M3CollectionTest, F_FMT_06_OpenedRawStaysShown) {
    touch(path("IMG_1.NEF"));
    touch(path("IMG_1.jpg"));
    auto c = make();
    c->setPairRawJpeg(true);
    c->openFile(path("IMG_1.NEF"));
    ASSERT_TRUE(loaded(*c));
    ASSERT_EQ(c->count(), 1);
    EXPECT_EQ(c->currentPath(), path("IMG_1.NEF"));
    EXPECT_EQ(c->at(0).pairedPath, path("IMG_1.jpg"));
}

TEST_F(M3CollectionTest, F_FMT_06_OperationsApplyToBoth) {
    touch(path("IMG_1.CR2"));
    touch(path("IMG_1.JPG"));
    touch(path("IMG_2.JPG"));
    auto c = make();
    c->setPairRawJpeg(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    // The paths an operation works on: the pair member not shown first.
    EXPECT_EQ(c->withPairs({path("IMG_1.JPG"), path("IMG_2.JPG")}),
              (QStringList{path("IMG_1.CR2"), path("IMG_1.JPG"), path("IMG_2.JPG")}));

    // Rating (the UI rates the shown file, then its partner): both files get their own
    // sidecar (F-CULL-02 names: the RAW the Adobe one, the JPEG "IMG_1.JPG.xmp").
    RatingOptions opts;
    Rating r;
    r.stars = 4;
    QStringList rated = c->withPairs({path("IMG_1.JPG")});
    std::reverse(rated.begin(), rated.end());
    for (const QString& p : std::as_const(rated))
        EXPECT_TRUE(ratingio::write(p, r, opts).ok()) << p.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.xmp")));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.JPG.xmp")));
    EXPECT_EQ(ratingio::read(path("IMG_1.CR2"), opts).stars, 4);
    EXPECT_EQ(ratingio::read(path("IMG_1.JPG"), opts).stars, 4);

    // Trash: one batch for both files (and their sidecars), the collection follows.
    QTemporaryDir trashDir;
    TrashToFolder trash(trashDir.path());
    QVector<TransferItem> items;
    for (const QString& p : c->withPairs({path("IMG_1.JPG")}))
        items.append({p, {}, ConflictChoice::Skip});
    FileBatch batch(trash, TransferKind::Trash, items);
    ASSERT_TRUE(batch.apply().isEmpty());
    EXPECT_TRUE(trash.trashed.contains(path("IMG_1.CR2")));
    EXPECT_TRUE(trash.trashed.contains(path("IMG_1.JPG")));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.CR2")));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.JPG")));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.xmp")));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.JPG.xmp")));
    QStringList currents;
    QObject::connect(c.get(), &Collection::currentChanged, [&](int, const QString& p) { currents << p; });
    c->setCurrentIndex(0);
    currents.clear();
    for (const TransferItem& it : std::as_const(items))
        c->removePath(it.source);
    EXPECT_EQ(c->count(), 1);
    EXPECT_EQ(c->currentPath(), path("IMG_2.JPG"));
    EXPECT_FALSE(currents.contains(path("IMG_1.CR2"))); // the RAW never flashed up

    // Undo brings both back as one pair again.
    ASSERT_TRUE(batch.revert().isEmpty());
    c->insertPath(path("IMG_1.JPG"));
    ASSERT_TRUE(test::waitUntil([&] { return !c->hasPendingInserts(); }));
    c->insertPath(path("IMG_1.CR2"));
    ASSERT_TRUE(test::waitUntil([&] { return !c->hasPendingInserts(); }));
    ASSERT_EQ(c->count(), 2);
    EXPECT_EQ(c->at(0).path, path("IMG_1.JPG"));
    EXPECT_EQ(c->at(0).pairedPath, path("IMG_1.CR2"));
}

TEST_F(M3CollectionTest, F_FMT_06_RemovingOneMemberKeepsTheOther) {
    touch(path("IMG_1.CR2"));
    touch(path("IMG_1.JPG"));
    auto c = make();
    c->setPairRawJpeg(true);
    c->openFolder(root);
    ASSERT_TRUE(loaded(*c));
    c->setCurrentIndex(0);
    // The JPEG is deleted outside the app: the RAW takes its place.
    c->removePath(path("IMG_1.JPG"));
    ASSERT_EQ(c->count(), 1);
    EXPECT_EQ(c->currentPath(), path("IMG_1.CR2"));
    EXPECT_TRUE(c->at(0).pairedPath.isEmpty());
}
