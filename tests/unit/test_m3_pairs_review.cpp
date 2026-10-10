// SPDX-License-Identifier: GPL-3.0-or-later
// M3 review fixes around RAW+JPEG pairs, sidecars and ratings (F-FMT-06, F-CULL-02/05,
// F-FILE-04/08/11, F-INFO-04): sidecar ownership, pairs as one transfer unit, literal base
// names, redo rollback, one undo step per pair, bulk rejects, renaming a pair member,
// XMP child elements, PNG XMP and the map consent. All file work in temporary folders; the
// trash is a fake that moves into a temporary folder.

#include "SortingTestHelpers.h"
#include "core/BulkRejects.h"
#include "core/Collection.h"
#include "core/FileBatch.h"
#include "core/FileOps.h"
#include "core/GeoLocation.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/PngXmp.h"
#include "core/RatingStore.h"
#include "core/Settings.h"
#include "core/Sidecars.h"
#include "core/Xmp.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QUuid>

#include <gtest/gtest.h>

using namespace lf;

namespace {

// Like QtTestApp.h's ensureQtApp() (not included: its waitUntil() clashes with
// SortingTestHelpers.h): one QGuiApplication for the whole test binary.
void ensureGuiApp() {
    if (QCoreApplication::instance())
        return;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    static int argc = 1;
    static char name[] = "lf_unit_tests";
    static char* argv[] = {name, nullptr};
    new QGuiApplication(argc, argv);
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

Rating stars(int n, PickFlag flag = PickFlag::None) {
    Rating r;
    r.stars = n;
    r.flag = flag;
    return r;
}

QByteArray sidecarWith(const Rating& r) {
    return xmp::newSidecar(r).toUtf8();
}

class FolderTrash : public TrashService {
public:
    FolderTrash() { dir.setAutoRemove(true); }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QString target =
            QDir(dir.path())
                .filePath(QUuid::createUuid().toString(QUuid::Id128) + "-" + QFileInfo(path).fileName());
        if (!QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("trash failed");
            return std::nullopt;
        }
        ++trashed;
        return target;
    }
    bool restoreFromTrash(const QString& trashedPath, const QString& original, QString* error) override {
        if (QFileInfo::exists(original) || !QFile::rename(trashedPath, original)) {
            if (error)
                *error = QStringLiteral("restore failed");
            return false;
        }
        return true;
    }
    QTemporaryDir dir;
    int trashed = 0;
};

class PairsReviewTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureGuiApp();
        ASSERT_TRUE(dir.isValid());
        root = QDir::cleanPath(dir.path());
        Settings::useTestingBackend(QDir(root).filePath(QStringLiteral("settings.ini")));
    }
    QString path(const QString& rel) const { return root + u'/' + rel; }
    bool run(FileOps& ops, std::shared_ptr<FileBatch> batch) {
        bool done = false, ok = false;
        runBatchAsync(ops, QStringLiteral("x"), std::move(batch), [&](bool success, const QString&) {
            ok = success;
            done = true;
        });
        return lf::test::waitUntil([&] { return done; }) && ok;
    }
    static bool idle(FileOps& ops, int index) {
        return lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == index; });
    }
    // A RAW+JPEG pair with both sidecars in `folder`.
    void makePair(const QString& folder, const QString& base, const QByteArray& tag) {
        ASSERT_TRUE(writeFile(path(folder + base + ".CR3"), "raw-" + tag));
        ASSERT_TRUE(writeFile(path(folder + base + ".JPG"), "jpg-" + tag));
        ASSERT_TRUE(writeFile(path(folder + base + ".xmp"), sidecarWith(stars(5))));
        ASSERT_TRUE(writeFile(path(folder + base + ".JPG.xmp"), sidecarWith(stars(1))));
    }

    QTemporaryDir dir;
    QString root;
};

} // namespace

// ---- 7: the JPEG of a pair never uses the RAW's Adobe sidecar ----

TEST_F(PairsReviewTest, F_CULL_02_JpegNextToRawNeverTouchesRawSidecar) {
    const QString raw = path("IMG_1.CR3"), jpg = path("IMG_1.JPG");
    ASSERT_TRUE(writeFile(raw, "raw"));
    ASSERT_TRUE(writeFile(jpg, "jpg"));
    // Lightroom rated the RAW: "IMG_1.xmp".
    ASSERT_TRUE(writeFile(path("IMG_1.xmp"), sidecarWith(stars(5))));
    const QByteArray rawSidecar = readFile(path("IMG_1.xmp"));
    RatingOptions o; // sidecar only
    EXPECT_EQ(ratingio::read(raw, o), stars(5));
    EXPECT_FALSE(ratingio::readSidecar(jpg).has_value()) << "the JPEG must not read the RAW's sidecar";
    EXPECT_EQ(ratingio::sidecarForWriting(jpg), path("IMG_1.JPG.xmp"));
    ASSERT_TRUE(ratingio::write(jpg, stars(1), o).ok());
    EXPECT_EQ(readFile(path("IMG_1.xmp")), rawSidecar); // untouched
    EXPECT_EQ(ratingio::read(raw, o), stars(5));
    EXPECT_EQ(ratingio::read(jpg, o), stars(1));
    // Many files at once (one folder listing): the same answers.
    const QHash<QString, Rating> many = ratingio::readMany({raw, jpg}, o);
    EXPECT_EQ(many.value(raw), stars(5));
    EXPECT_EQ(many.value(jpg), stars(1));
    // A JPEG alone keeps using the Adobe name.
    ASSERT_TRUE(writeFile(path("solo.jpg"), "jpg"));
    ASSERT_TRUE(writeFile(path("solo.xmp"), sidecarWith(stars(3))));
    EXPECT_EQ(ratingio::read(path("solo.jpg"), o), stars(3));
    EXPECT_EQ(ratingio::sidecarForWriting(path("solo.jpg")), path("solo.xmp"));
    // Non-image files with the same base name do not count (Live Photo video).
    ASSERT_TRUE(writeFile(path("live.HEIC"), "heic"));
    ASSERT_TRUE(writeFile(path("live.MOV"), "mov"));
    EXPECT_EQ(ratingio::sidecarForWriting(path("live.HEIC")), path("live.xmp"));
}

// ---- 9: base names are compared literally (no glob patterns) ----

TEST_F(PairsReviewTest, F_FMT_06_GlobCharactersInBaseNames) {
    QStringList bases = {QStringLiteral("Shot [a]"), QStringLiteral("x{1,2}")};
#ifndef Q_OS_WIN
    bases << QStringLiteral("a*b?"); // "*" and "?" are not allowed in Windows file names
#endif
    for (const QString& base : bases) {
        const QString raw = path(base + ".CR3"), jpg = path(base + ".JPG");
        ASSERT_TRUE(writeFile(raw, "raw"));
        ASSERT_TRUE(writeFile(jpg, "jpg"));
        ASSERT_TRUE(writeFile(path(base + ".xmp"), sidecarWith(stars(4))));
        EXPECT_TRUE(filebatch::ownSidecars(jpg).isEmpty()) << base.toStdString();
        EXPECT_EQ(filebatch::ownSidecars(raw), QStringList{path(base + ".xmp")}) << base.toStdString();
        EXPECT_EQ(ratingio::sidecarForWriting(jpg), jpg + ".xmp") << base.toStdString();
        EXPECT_FALSE(ratingio::readSidecar(jpg).has_value()) << base.toStdString();
        EXPECT_EQ(ratingio::readSidecar(raw), stars(4)) << base.toStdString();
    }
    // "Shot [a]" must not be taken for "Shot a" (a glob would match "[a]" as "a").
    ASSERT_TRUE(writeFile(path("Shot a.JPG"), "jpg"));
    ASSERT_TRUE(writeFile(path("Shot a.xmp"), sidecarWith(stars(2))));
    EXPECT_EQ(ratingio::readSidecar(path("Shot a.JPG")), stars(2));
}

TEST_F(PairsReviewTest, F_FMT_06_BaseNamesNfcAndCaseFolded) {
    // "Café" decomposed (NFD) for the RAW, precomposed (NFC) for the JPEG.
    const QString nfd = QStringLiteral("Café"), nfc = QStringLiteral("Café");
    ASSERT_TRUE(writeFile(path(nfd + ".CR3"), "raw"));
    ASSERT_TRUE(writeFile(path(nfc + ".JPG"), "jpg"));
    EXPECT_EQ(ratingio::sidecarForWriting(path(nfc + ".JPG")), path(nfc + ".JPG.xmp"));
    if (sidecars::isCaseInsensitiveVolume(root)) {
        ASSERT_TRUE(writeFile(path("img_2.cr3"), "raw"));
        ASSERT_TRUE(writeFile(path("IMG_2.JPG"), "jpg"));
        EXPECT_EQ(ratingio::sidecarForWriting(path("IMG_2.JPG")), path("IMG_2.JPG.xmp"));
    }
}

// ---- 8: a pair is one transfer unit ----

TEST_F(PairsReviewTest, F_FILE_11_PairKeepBothRenamesBothConsistently) {
    FolderTrash trash;
    FileOps ops(trash);
    makePair(QString(), QStringLiteral("IMG_1"), "new");
    ASSERT_TRUE(writeFile(path("Ziel/IMG_1.CR3"), "old-raw"));
    ASSERT_TRUE(writeFile(path("Ziel/IMG_1.JPG"), "old-jpg"));
    ASSERT_TRUE(writeFile(path("Ziel/IMG_1.xmp"), sidecarWith(stars(2))));
    const QString target = path("Ziel");
    QVector<TransferItem> items{{path("IMG_1.CR3"), target, ConflictChoice::Skip},
                                {path("IMG_1.JPG"), target, ConflictChoice::Skip}};
    // One question for both files.
    EXPECT_EQ(filebatch::conflicts(items, TransferKind::Move), QStringList{path("IMG_1.CR3")});
    EXPECT_EQ(filebatch::unitNames(items, TransferKind::Move, path("IMG_1.JPG")),
              (QStringList{QStringLiteral("IMG_1.CR3"), QStringLiteral("IMG_1.JPG")}));
    items[0].onConflict = ConflictChoice::KeepBoth;
    auto batch = std::make_shared<FileBatch>(trash, TransferKind::Move, items);
    ASSERT_TRUE(run(ops, batch));
    EXPECT_EQ(batch->doneUnits(), 1);
    EXPECT_EQ(readFile(target + "/IMG_1 2.CR3"), QByteArray("raw-new"));
    EXPECT_EQ(readFile(target + "/IMG_1 2.JPG"), QByteArray("jpg-new"));
    EXPECT_EQ(xmp::readRating(QString::fromUtf8(readFile(target + "/IMG_1 2.xmp"))), stars(5));
    EXPECT_EQ(xmp::readRating(QString::fromUtf8(readFile(target + "/IMG_1 2.JPG.xmp"))), stars(1));
    EXPECT_EQ(readFile(target + "/IMG_1.CR3"), QByteArray("old-raw")); // the old ones stay
    EXPECT_EQ(xmp::readRating(QString::fromUtf8(readFile(target + "/IMG_1.xmp"))), stars(2));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.xmp")));
    // The renamed files read their own ratings.
    RatingOptions o;
    EXPECT_EQ(ratingio::read(target + "/IMG_1 2.CR3", o), stars(5));
    EXPECT_EQ(ratingio::read(target + "/IMG_1 2.JPG", o), stars(1));
    ops.undo();
    ASSERT_TRUE(idle(ops, 0));
    for (const char* n : {"IMG_1.CR3", "IMG_1.JPG", "IMG_1.xmp", "IMG_1.JPG.xmp"})
        EXPECT_TRUE(QFileInfo::exists(path(QString::fromLatin1(n)))) << n;
    EXPECT_FALSE(QFileInfo::exists(target + "/IMG_1 2.CR3"));
    EXPECT_FALSE(QFileInfo::exists(target + "/IMG_1 2.xmp"));
}

TEST_F(PairsReviewTest, F_FILE_11_PairConflictOfOneMemberRenamesBoth) {
    FolderTrash trash;
    FileOps ops(trash);
    makePair(QString(), QStringLiteral("IMG_1"), "new");
    ASSERT_TRUE(writeFile(path("Ziel/IMG_1.JPG"), "old-jpg")); // only the JPEG name is taken
    const QString target = path("Ziel");
    QVector<TransferItem> items{{path("IMG_1.CR3"), target, ConflictChoice::KeepBoth},
                                {path("IMG_1.JPG"), target, ConflictChoice::Skip}};
    ASSERT_TRUE(run(ops, std::make_shared<FileBatch>(trash, TransferKind::Copy, items)));
    EXPECT_EQ(readFile(target + "/IMG_1 2.CR3"), QByteArray("raw-new"));
    EXPECT_EQ(readFile(target + "/IMG_1 2.JPG"), QByteArray("jpg-new"));
    EXPECT_TRUE(QFileInfo::exists(target + "/IMG_1 2.xmp"));     // the shared sidecar is copied
    EXPECT_TRUE(QFileInfo::exists(target + "/IMG_1 2.JPG.xmp")); // and the JPEG's own
    EXPECT_FALSE(QFileInfo::exists(target + "/IMG_1.CR3"));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.xmp"))); // copy: the sources stay
}

TEST_F(PairsReviewTest, F_FILE_11_PairSkipAndReplaceAsOneUnit) {
    FolderTrash trash;
    FileOps ops(trash);
    makePair(QString(), QStringLiteral("IMG_1"), "new");
    ASSERT_TRUE(writeFile(path("Ziel/IMG_1.CR3"), "old-raw"));
    const QString target = path("Ziel");
    // Skip: neither file moves (no half pair in the target).
    QVector<TransferItem> items{{path("IMG_1.CR3"), target, ConflictChoice::Skip},
                                {path("IMG_1.JPG"), target, ConflictChoice::Skip}};
    auto skip = std::make_shared<FileBatch>(trash, TransferKind::Move, items);
    ASSERT_TRUE(run(ops, skip));
    EXPECT_EQ(skip->skipped(), (QStringList{path("IMG_1.CR3"), path("IMG_1.JPG")}));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.JPG")));
    EXPECT_FALSE(QFileInfo::exists(target + "/IMG_1.JPG"));
    // Replace: the old RAW goes to the trash, both new files (and sidecars) arrive.
    items[0].onConflict = ConflictChoice::Replace;
    ASSERT_TRUE(run(ops, std::make_shared<FileBatch>(trash, TransferKind::Move, items)));
    EXPECT_EQ(readFile(target + "/IMG_1.CR3"), QByteArray("raw-new"));
    EXPECT_EQ(readFile(target + "/IMG_1.JPG"), QByteArray("jpg-new"));
    EXPECT_TRUE(QFileInfo::exists(target + "/IMG_1.xmp"));
    EXPECT_TRUE(QFileInfo::exists(target + "/IMG_1.JPG.xmp"));
    EXPECT_EQ(trash.trashed, 1);
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy(); }));
    EXPECT_EQ(readFile(target + "/IMG_1.CR3"), QByteArray("old-raw"));
    EXPECT_EQ(readFile(path("IMG_1.CR3")), QByteArray("raw-new"));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.xmp")));
    EXPECT_FALSE(QFileInfo::exists(target + "/IMG_1.JPG"));
}

TEST_F(PairsReviewTest, F_FMT_06_PairTrashIsOneStepWithAllSidecars) {
    FolderTrash trash;
    FileOps ops(trash);
    makePair(QString(), QStringLiteral("IMG_1"), "x");
    QVector<TransferItem> items{{path("IMG_1.CR3"), {}, ConflictChoice::Skip},
                                {path("IMG_1.JPG"), {}, ConflictChoice::Skip}};
    ASSERT_TRUE(run(ops, std::make_shared<FileBatch>(trash, TransferKind::Trash, items)));
    for (const char* n : {"IMG_1.CR3", "IMG_1.JPG", "IMG_1.xmp", "IMG_1.JPG.xmp"})
        EXPECT_FALSE(QFileInfo::exists(path(QString::fromLatin1(n)))) << n;
    EXPECT_EQ(trash.trashed, 4);
    EXPECT_EQ(ops.undoStack()->count(), 1);
    ops.undo();
    ASSERT_TRUE(idle(ops, 0));
    for (const char* n : {"IMG_1.CR3", "IMG_1.JPG", "IMG_1.xmp", "IMG_1.JPG.xmp"})
        EXPECT_TRUE(QFileInfo::exists(path(QString::fromLatin1(n)))) << n;
}

// ---- 10: a failing redo puts the replaced target back ----

TEST_F(PairsReviewTest, F_FILE_11_FailedRedoRestoresReplacedTarget) {
    FolderTrash trash;
    ASSERT_TRUE(writeFile(path("r.jpg"), "new"));
    ASSERT_TRUE(writeFile(path("Ziel/r.jpg"), "old"));
    FileBatch batch(trash, TransferKind::Move, {{path("r.jpg"), path("Ziel"), ConflictChoice::Replace}});
    ASSERT_TRUE(batch.apply().isEmpty());
    EXPECT_EQ(readFile(path("Ziel/r.jpg")), QByteArray("new"));
    ASSERT_TRUE(batch.revert().isEmpty());
    EXPECT_EQ(readFile(path("Ziel/r.jpg")), QByteArray("old"));
    ASSERT_TRUE(QFile::remove(path("r.jpg"))); // the source went meanwhile: redo fails
    EXPECT_FALSE(batch.apply().isEmpty());
    EXPECT_EQ(readFile(path("Ziel/r.jpg")), QByteArray("old")) << "the replaced file stays in the trash";
}

// ---- 11: rating a pair is one undo step ----

TEST_F(PairsReviewTest, F_CULL_01_RatingPairIsOneUndoStep) {
    FolderTrash trash;
    FileOps ops(trash);
    const QString raw = path("IMG_1.CR3"), jpg = path("IMG_1.JPG");
    ASSERT_TRUE(writeFile(raw, "raw"));
    ASSERT_TRUE(writeFile(jpg, "jpg"));
    RatingStore store;
    store.setOptions(RatingOptions{});
    bool done = false;
    store.changeRatings(
        ops, {jpg, raw}, [](const Rating& r) { return rating::apply(r, Action::Rate3); },
        [&](bool ok, const QString&, const QString&, const Rating& now) {
            EXPECT_TRUE(ok);
            EXPECT_EQ(now, stars(3));
            done = true;
        });
    ASSERT_TRUE(lf::test::waitUntil([&] { return done; }));
    EXPECT_EQ(ops.undoStack()->count(), 1);
    EXPECT_EQ(ratingio::read(raw, store.options()), stars(3));
    EXPECT_EQ(ratingio::read(jpg, store.options()), stars(3));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.xmp")));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.JPG.xmp")));
    ops.undo();
    ASSERT_TRUE(idle(ops, 0));
    EXPECT_EQ(ratingio::read(raw, store.options()), Rating());
    EXPECT_EQ(ratingio::read(jpg, store.options()), Rating());
}

// ---- 12: bulk rejects ----

TEST(BulkRejects, F_CULL_05_OnlyShownAndNeverRejectedTwice) {
#ifdef Q_OS_WIN
    GTEST_SKIP() << "POSIX absolute paths in the test data (\"/p/a.jpg\" is \"<drive>:/p/a.jpg\" on Windows)";
#endif
    EXPECT_EQ(bulkrejects::rejectedFolder("/p/a.jpg"), QStringLiteral("/p/_Rejected"));
    EXPECT_TRUE(bulkrejects::rejectedFolder("/p/_Rejected/a.jpg").isEmpty());
    EXPECT_TRUE(bulkrejects::rejectedFolder("/p/_rejected/sub/a.jpg").isEmpty());
    const QHash<QString, Rating> ratings{{"/p/a.jpg", stars(0, PickFlag::Reject)},
                                         {"/p/b.jpg", stars(4)},
                                         {"/p/IMG_1.JPG", stars(2, PickFlag::Reject)},
                                         {"/p/_Rejected/c.jpg", stars(0, PickFlag::Reject)},
                                         {"/p/hidden.jpg", stars(0, PickFlag::Reject)}};
    // "hidden.jpg" is not shown (filters): it is not part of the action.
    const QVector<QPair<QString, QString>> shown{
        {"/p/a.jpg", {}}, {"/p/b.jpg", {}}, {"/p/IMG_1.JPG", "/p/IMG_1.CR3"}, {"/p/_Rejected/c.jpg", {}}};
    const bulkrejects::Plan move = bulkrejects::plan(shown, ratings, false);
    EXPECT_EQ(move.images, 2);
    ASSERT_EQ(move.items.size(), 3);
    EXPECT_EQ(move.items[0].source, QStringLiteral("/p/a.jpg"));
    EXPECT_EQ(move.items[0].targetFolder, QStringLiteral("/p/_Rejected"));
    EXPECT_EQ(move.items[1].source, QStringLiteral("/p/IMG_1.CR3")); // the pair goes together
    EXPECT_EQ(move.items[2].source, QStringLiteral("/p/IMG_1.JPG"));
    const bulkrejects::Plan trash = bulkrejects::plan(shown, ratings, true);
    EXPECT_EQ(trash.images, 3); // the trash takes rejects inside "_Rejected" as well
}

// ---- 13: renaming the shown member of a pair ----

TEST_F(PairsReviewTest, F_FMT_06_RenamingShownMemberUpdatesPair) {
    auto collator = createNameCollator();
    test::FakeOrderProvider order;
    test::FakeAttributeProvider attributes;
    for (const char* n : {"IMG_1.CR2", "IMG_1.JPG", "IMG_2.CR2", "IMG_2.JPG"})
        ASSERT_TRUE(writeFile(path(QString::fromLatin1(n)), "x"));
    ASSERT_TRUE(writeFile(path("IMG_1.JPG.xmp"), sidecarWith(stars(4))));
    Collection c;
    c.setProviders(collator.get(), &order, &attributes);
    c.setDefaultSort(false, false, SortSpec{SortKey::Name, true, SortSource::Manual});
    c.setPairRawJpeg(true);
    c.openFolder(root);
    ASSERT_TRUE(test::waitUntil([&] { return !c.isLoading(); }));
    ASSERT_EQ(c.count(), 2);
    // A new name that no longer pairs: the RAW becomes an entry of its own; the JPEG's
    // sidecar follows the file (FileOps rename).
    FolderTrash trash;
    FileOps ops(trash);
    QString error;
    ASSERT_TRUE(ops.rename(path("IMG_1.JPG"), QStringLiteral("Urlaub.JPG"), &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path("Urlaub.JPG.xmp")));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.JPG.xmp")));
    EXPECT_EQ(ratingio::read(path("Urlaub.JPG"), RatingOptions{}), stars(4));
    c.renamePath(path("IMG_1.JPG"), path("Urlaub.JPG"));
    EXPECT_TRUE(c.pairedPath(path("Urlaub.JPG")).isEmpty());
    EXPECT_TRUE(c.pairedPath(path("IMG_1.CR2")).isEmpty());
    EXPECT_GE(c.indexOf(path("IMG_1.CR2")), 0);
    EXPECT_EQ(c.withPairs({path("Urlaub.JPG")}), QStringList{path("Urlaub.JPG")});
    EXPECT_EQ(c.count(), 3);
    // A new name that still pairs (case only): the partner points to the new name.
    c.renamePath(path("IMG_2.JPG"), path("IMG_2.jpg"));
    EXPECT_EQ(c.pairedPath(path("IMG_2.jpg")), path("IMG_2.CR2"));
    EXPECT_EQ(c.pairedPath(path("IMG_2.CR2")), path("IMG_2.jpg"));
    EXPECT_EQ(c.withPairs({path("IMG_2.jpg")}), (QStringList{path("IMG_2.CR2"), path("IMG_2.jpg")}));
    // Undo of the rename brings the sidecar back to the old name.
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy(); }));
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.JPG.xmp")));
}

// ---- 14: XMP child elements keep their start tag ----

TEST(XmpReview, F_CULL_02_ChildElementKeepsStartTag) {
    const Rating four = stars(4);
    // Namespace declared on the element itself (darktable/digiKam style).
    const QString own = QStringLiteral("<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
                                       "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                                       "<rdf:Description rdf:about=\"\"><xmp:Rating "
                                       "xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">3</xmp:Rating>"
                                       "</rdf:Description></rdf:RDF></x:xmpmeta>");
    EXPECT_EQ(xmp::readRating(own), stars(3));
    auto out = xmp::writeRating(own, four);
    ASSERT_TRUE(out);
    EXPECT_TRUE(xmp::isWellFormed(*out));
    EXPECT_TRUE(out->contains(
        QStringLiteral("<xmp:Rating xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">4</xmp:Rating>")));
    EXPECT_EQ(xmp::readRating(*out), four);
    // Default namespace on the element.
    const QString dflt = QString(own).replace(
        QStringLiteral("<xmp:Rating xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">3</xmp:Rating>"),
        QStringLiteral("<Rating xmlns=\"http://ns.adobe.com/xap/1.0/\">3</Rating>"));
    out = xmp::writeRating(dflt, four);
    ASSERT_TRUE(out);
    EXPECT_TRUE(out->contains(QStringLiteral("<Rating xmlns=\"http://ns.adobe.com/xap/1.0/\">4</Rating>")));
    EXPECT_EQ(xmp::readRating(*out), four);
    // Capture One style: attributes on the property element, other content around it.
    const QString c1 =
        QStringLiteral("<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                       "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Capture One\">\n"
                       " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                       "  <rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
                       "    xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\" photoshop:Urgency=\"2\">\n"
                       "   <xmp:Rating xml:space=\"preserve\">2</xmp:Rating>\n"
                       "   <xmp:Label>Green</xmp:Label>\n"
                       "  </rdf:Description>\n"
                       " </rdf:RDF>\n"
                       "</x:xmpmeta>\n<?xpacket end=\"w\"?>");
    out = xmp::writeRating(c1, four);
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, QString(c1).replace(QStringLiteral("preserve\">2<"), QStringLiteral("preserve\">4<")));
    // Self-closing element: becomes an element with content, attributes kept.
    const QString empty = QString(own).replace(
        QStringLiteral("<xmp:Rating xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">3</xmp:Rating>"),
        QStringLiteral("<xmp:Rating xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"/>"));
    out = xmp::writeRating(empty, four);
    ASSERT_TRUE(out);
    EXPECT_TRUE(out->contains(
        QStringLiteral("<xmp:Rating xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">4</xmp:Rating>")));
    EXPECT_EQ(xmp::readRating(*out), four);
    // Clearing removes the element (with its own declaration) and stays well-formed.
    out = xmp::writeRating(own, Rating());
    ASSERT_TRUE(out);
    EXPECT_TRUE(xmp::isWellFormed(*out));
    EXPECT_FALSE(xmp::readRating(*out).has_value());
}

TEST_F(PairsReviewTest, F_CULL_02_Utf16SidecarStaysUtf16) {
    const QString img = path("u.jpg");
    ASSERT_TRUE(writeFile(img, "jpg"));
    const QString text =
        QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n") + xmp::newSidecar(stars(2));
    QByteArray u16("\xFF\xFE", 2);
    u16.append(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
    ASSERT_TRUE(writeFile(path("u.xmp"), u16));
    ASSERT_TRUE(ratingio::writeSidecar(img, stars(5)).isEmpty());
    const QByteArray after = readFile(path("u.xmp"));
    EXPECT_TRUE(after.startsWith("\xFF\xFE"));
    EXPECT_EQ(ratingio::readSidecar(img), stars(5));
}

// ---- 15: XMP in PNG files ----

TEST(PngXmp, F_CULL_02_ChunkWriterIsLossless) {
    QImage img(17, 9, QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixel(x, y, qRgba(x * 13, y * 27, (x * y) % 256, 128 + x));
    QByteArray png;
    QBuffer buf(&png);
    ASSERT_TRUE(buf.open(QIODevice::WriteOnly));
    ASSERT_TRUE(img.save(&buf, "PNG"));
    EXPECT_TRUE(pngxmp::isPng(png));
    EXPECT_EQ(pngxmp::read(png), QString());
    EXPECT_EQ(pngxmp::crc32("IEND"), 0xAE426082u);
    const QString packet = xmp::wrapPacket(xmp::newSidecar(stars(3)));
    const auto with = pngxmp::withPacket(png, packet);
    ASSERT_TRUE(with);
    EXPECT_EQ(pngxmp::read(*with), packet);
    EXPECT_LT(with->indexOf("iTXt"), with->indexOf("IDAT")); // in front of the image data
    EXPECT_EQ(QImage::fromData(*with, "PNG").convertToFormat(QImage::Format_ARGB32), img);
    // Replacing keeps one chunk; the rest of the file is unchanged.
    const auto again = pngxmp::withPacket(*with, QStringLiteral("<x:xmpmeta/>"));
    ASSERT_TRUE(again);
    EXPECT_EQ(again->count("XML:com.adobe.xmp"), 1);
    EXPECT_EQ(again->size() - with->size(),
              qsizetype(QStringLiteral("<x:xmpmeta/>").toUtf8().size()) - packet.toUtf8().size());
    EXPECT_FALSE(pngxmp::read(QByteArray("not a png")).has_value());
    EXPECT_FALSE(pngxmp::withPacket(png.left(png.size() - 6), packet).has_value()); // truncated
}

#ifdef Q_OS_MACOS
TEST_F(PairsReviewTest, F_CULL_02_PngEmbeddedRatingReadsBackPixelsIdentical) {
    QImage img(31, 20, QImage::Format_RGB32);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixel(x, y, qRgb(x * 8, y * 12, 200 - x));
    const QString file = path("p.png");
    ASSERT_TRUE(img.save(file, "PNG"));
    const QByteArray before = readFile(file);
    QString reason;
    ASSERT_TRUE(ratingio::canEmbed(file, &reason)) << reason.toStdString();
    bool refused = false;
    EXPECT_EQ(ratingio::writeEmbedded(file, stars(4, PickFlag::Pick), true, &refused), QString());
    EXPECT_FALSE(refused);
    EXPECT_EQ(ratingio::readEmbedded(file), stars(4, PickFlag::Pick)); // ImageIO reads it
    const QByteArray after = readFile(file);
    EXPECT_EQ(after.mid(after.indexOf("IDAT") - 4), before.mid(before.indexOf("IDAT") - 4)); // image data
    EXPECT_EQ(QImage(file).convertToFormat(QImage::Format_RGB32), img);
    // Changing and clearing work as well (the chunk is replaced).
    EXPECT_EQ(ratingio::writeEmbedded(file, stars(1), true, &refused), QString());
    EXPECT_EQ(ratingio::readEmbedded(file), stars(1));
    EXPECT_EQ(ratingio::writeEmbedded(file, Rating(), true, &refused), QString());
    EXPECT_FALSE(ratingio::readEmbedded(file).value_or(Rating()).stars);
}
#endif

// ---- 18: map consent ----

namespace {
class CountingMapProvider : public MapSnapshotProvider {
public:
    void request(const GeoPoint&, QSize, qreal, QObject*, Done done) override {
        ++requests;
        last = std::move(done);
    }
    void cancel() override { ++cancels; }
    int requests = 0;
    int cancels = 0;
    Done last;
};
} // namespace

TEST(MapConsent, F_INFO_04_NoRequestWhileHiddenAndRevoke) {
    ensureGuiApp();
    CountingMapProvider provider;
    MapPreviewController map(&provider);
    map.setActive(false); // the info panel is hidden
    map.setLocation(GeoPoint{1, 2});
    map.showMap(); // consent given earlier in the window …
    EXPECT_EQ(provider.requests, 0) << "hidden panel: no request";
    map.setLocation(GeoPoint{3, 4}); // … flipping with the panel hidden: still none
    EXPECT_EQ(provider.requests, 0);
    map.setActive(true); // the panel shows up: the shown image's map
    EXPECT_EQ(provider.requests, 1);
    EXPECT_EQ(map.state(), MapPreviewController::State::Loading);
    map.setActive(false); // hidden while loading: dropped
    EXPECT_EQ(map.state(), MapPreviewController::State::Idle);
    provider.last(QImage(4, 4, QImage::Format_RGB32), {}); // a late answer is ignored
    EXPECT_EQ(map.state(), MapPreviewController::State::Idle);
    map.setActive(true);
    EXPECT_EQ(provider.requests, 2);
    QImage tile(4, 4, QImage::Format_RGB32);
    tile.fill(Qt::green);
    provider.last(tile, {});
    EXPECT_EQ(map.state(), MapPreviewController::State::Ready);
    // "Karte ausblenden": consent gone, map gone, the next image asks again.
    map.hideMap();
    EXPECT_FALSE(map.hasConsent());
    EXPECT_EQ(map.state(), MapPreviewController::State::Idle);
    EXPECT_TRUE(map.image().isNull());
    map.setLocation(GeoPoint{5, 6});
    EXPECT_EQ(provider.requests, 2);
    EXPECT_EQ(map.state(), MapPreviewController::State::Idle);
}
