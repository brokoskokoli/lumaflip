// SPDX-License-Identifier: GPL-3.0-or-later
// Core regression tests of the second review round: asynchronous file operations with
// the origin of a trash kept per undo command (F-FILE-01/08), the shared folder snapshot
// cache (F-WIN-09), multi-file collections taking back only their own files (F-OPEN-04),
// renames in place (F-FILE-04), sibling folders (F-NAV-03), POSIX vs privacy denials
// (F-PRIV-01), RAW embedded previews (F-FMT-05) and the settings location (SPEC 8.6).

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/FolderCache.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>
#include <QtConcurrent/QtConcurrentRun>
#include <gtest/gtest.h>

#include <atomic>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lf;
using lf::test::waitUntil;

namespace {

QString writeFile(const QString& path, const QByteArray& data = "x") {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    return QDir::cleanPath(path);
}

// Trash that only renames into a private folder (no user trash involved) and can be slow.
class FolderTrash : public TrashService {
public:
    explicit FolderTrash(QString folder) : m_folder(std::move(folder)) {}
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QThread::msleep(ulong(delayMs.load()));
        ++calls;
        QString target =
            m_folder + QStringLiteral("/%1-%2").arg(calls.load()).arg(QFileInfo(path).fileName());
        if (!QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("nope");
            return std::nullopt;
        }
        return target;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        QThread::msleep(ulong(delayMs.load()));
        if (QFileInfo::exists(original) || !QFile::rename(trashed, original)) {
            if (error)
                *error = QStringLiteral("taken");
            return false;
        }
        return true;
    }
    std::atomic<int> delayMs{0};
    std::atomic<int> calls{0};

private:
    QString m_folder;
};

class ReviewFileOps : public ::testing::Test {
protected:
    void SetUp() override {
        test::ensureApp();
        ASSERT_TRUE(dir.isValid());
        ASSERT_TRUE(QDir(dir.path()).mkdir("trash"));
        trash = std::make_unique<FolderTrash>(dir.filePath("trash"));
        ops = std::make_unique<FileOps>(*trash);
        QObject::connect(ops.get(), &FileOps::fileRemoved,
                         [this](const QString& p) { log << "removed " + p; });
        QObject::connect(ops.get(), &FileOps::fileAdded, [this](const QString& p) { log << "added " + p; });
        QObject::connect(ops.get(), &FileOps::fileRestored,
                         [this](const QString& p, QObject* o) { restored << qMakePair(p, o); });
        QObject::connect(ops.get(), &FileOps::operationFailed, [this](const QString& e) { errors << e; });
    }
    QString path(const QString& n) const { return QDir::cleanPath(dir.path()) + "/" + n; }

    QTemporaryDir dir;
    std::unique_ptr<FolderTrash> trash;
    std::unique_ptr<FileOps> ops;
    QStringList log;
    QStringList errors;
    QList<QPair<QString, QObject*>> restored;
};

} // namespace

// Covers: F-FILE-01 (asynchronous trash; the origin travels with the undo command)
TEST_F(ReviewFileOps, F_FILE_01_AsyncTrashKeepsOriginAcrossUndoRedo) {
    const QString p = writeFile(path("a.jpg"));
    QObject origin;
    trash->delayMs = 300;
    bool done = false, ok = false;
    QElapsedTimer t;
    t.start();
    ops->moveToTrashAsync(p, &origin, [&](bool success, const QString&) {
        done = true;
        ok = success;
    });
    EXPECT_LT(t.elapsed(), 100); // the caller (GUI thread) does not wait
    EXPECT_TRUE(ops->isBusy());
    EXPECT_TRUE(log.isEmpty());
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_TRUE(ok);
    EXPECT_FALSE(QFileInfo::exists(p));
    EXPECT_EQ(log, QStringList{"removed " + p});
    EXPECT_EQ(ops->undoStack()->count(), 1);
    trash->delayMs = 0;

    for (int round = 0; round < 2; ++round) { // undo -> redo -> undo
        ops->undo();
        ASSERT_TRUE(waitUntil([&] { return QFileInfo::exists(p) && !ops->isBusy(); }));
        ASSERT_EQ(restored.size(), round + 1);
        EXPECT_EQ(restored.last().first, p);
        EXPECT_EQ(restored.last().second, &origin);
        if (round == 0) {
            ops->redo();
            ASSERT_TRUE(waitUntil([&] { return !QFileInfo::exists(p) && !ops->isBusy(); }));
        }
    }
    EXPECT_TRUE(errors.isEmpty()) << errors.join("\n").toStdString();
}

// Covers: F-FILE-01 (a failed asynchronous trash pushes nothing)
TEST_F(ReviewFileOps, F_FILE_01_AsyncFailureLeavesNoCommand) {
    bool done = false;
    QString error;
    ops->moveToTrashAsync(path("missing.jpg"), nullptr, [&](bool ok, const QString& e) {
        done = true;
        EXPECT_FALSE(ok);
        error = e;
    });
    ASSERT_TRUE(waitUntil([&] { return done; }));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(ops->undoStack()->count(), 0);
    EXPECT_TRUE(log.isEmpty());
}

// Covers: F-FILE-01, F-FILE-08 (operations keep the request order; undo is queued behind)
TEST_F(ReviewFileOps, F_FILE_08_AsyncOperationsRunInOrder) {
    ASSERT_TRUE(QDir(dir.path()).mkdir("target"));
    const QString a = writeFile(path("a.jpg"), "A");
    const QString b = writeFile(path("b.jpg"), "B");
    trash->delayMs = 100;
    ops->moveToTrashAsync(a);
    ops->copyToAsync(b, path("target"));
    ops->moveToAsync(b, path("target/sub-missing")); // fails: no such folder
    ops->undo();                                     // undoes the copy (the latest command)
    ASSERT_TRUE(waitUntil([&] { return !ops->isBusy(); }));
    EXPECT_FALSE(QFileInfo::exists(a));
    EXPECT_FALSE(QFileInfo::exists(path("target/b.jpg")));
    EXPECT_TRUE(QFileInfo::exists(b));
    EXPECT_EQ(ops->undoStack()->count(), 2);
    EXPECT_EQ(ops->undoStack()->index(), 1);
    ops->undo();
    ASSERT_TRUE(waitUntil([&] { return !ops->isBusy(); }));
    EXPECT_TRUE(QFileInfo::exists(a));
    // Undo on the worker that fails (original taken) drops the command like before.
    ops->redo();
    ASSERT_TRUE(waitUntil([&] { return !ops->isBusy(); }));
    EXPECT_FALSE(QFileInfo::exists(a));
    writeFile(a, "new");
    ops->undo();
    ASSERT_TRUE(waitUntil([&] { return !ops->isBusy(); }));
    EXPECT_EQ(errors.size(), 1);
    EXPECT_EQ(ops->undoStack()->index(), 0);
}

// ---- FolderCache (F-WIN-09) ----

namespace {
class CountingAttributes : public FileAttributeProvider {
public:
    void fill(FileEntry&) override {
        ++fills;
        QThread::msleep(2);
    }
    std::atomic<int> fills{0};
};
} // namespace

// Covers: F-WIN-09 (one enumeration per folder change, shared by concurrent readers)
TEST(ReviewFolderCache, F_WIN_09_ConcurrentReadersShareOneEnumeration) {
    test::ensureApp();
    QTemporaryDir dir;
    for (int i = 0; i < 40; ++i)
        writeFile(dir.filePath(QStringLiteral("f%1.jpg").arg(i)));
    const QString folder = QDir::cleanPath(dir.path());
    FolderCache cache;
    CountingAttributes attrs;
    cache.acquire(folder);
    QList<QFuture<int>> readers;
    for (int i = 0; i < 4; ++i)
        readers << QtConcurrent::run([&] { return int(cache.get(folder, &attrs)->entries.size()); });
    for (auto& f : readers)
        EXPECT_EQ(f.result(), 40);
    EXPECT_EQ(cache.enumerationCount(folder), 1);
    EXPECT_EQ(attrs.fills.load(), 40); // attributes once, not per reader
    EXPECT_EQ(cache.get(folder, &attrs)->entries.size(), 40);
    EXPECT_EQ(cache.enumerationCount(folder), 1);
    // A change: the next reader enumerates once more.
    writeFile(dir.filePath("new.jpg"));
    cache.invalidate(folder);
    EXPECT_EQ(cache.get(folder, &attrs)->entries.size(), 41);
    EXPECT_EQ(cache.get(folder, &attrs)->entries.size(), 41);
    EXPECT_EQ(cache.enumerationCount(folder), 2);
    // Without a lease nothing is kept: opening the folder again reads it again.
    cache.release(folder);
    EXPECT_EQ(cache.leaseCount(folder), 0);
    cache.get(folder, &attrs);
    EXPECT_EQ(cache.enumerationCount(folder), 3);
}

// Covers: F-WIN-09 (collections on one folder: one enumeration; refresh() re-reads)
TEST(ReviewFolderCache, F_WIN_09_CollectionsShareSnapshot) {
    test::ensureApp();
    QTemporaryDir dir;
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        writeFile(dir.filePath(QString::fromLatin1(n)));
    const QString folder = QDir::cleanPath(dir.path());
    const int before = FolderCache::instance().enumerationCount(folder);
    Collection c1, c2, c3;
    c1.openFile(folder + "/a.jpg");
    c2.openFile(folder + "/b.jpg"); // while c1 still loads
    ASSERT_TRUE(waitUntil([&] { return !c1.isLoading() && !c2.isLoading(); }));
    c3.openFolder(folder);
    ASSERT_TRUE(waitUntil([&] { return !c3.isLoading(); }));
    EXPECT_EQ(c1.count(), 3);
    EXPECT_EQ(c2.count(), 3);
    EXPECT_EQ(c3.count(), 3);
    EXPECT_EQ(FolderCache::instance().enumerationCount(folder) - before, 1);
    EXPECT_EQ(FolderCache::instance().leaseCount(folder), 3);
    // Shared reload after one invalidation (what the app does per watcher event).
    writeFile(folder + "/d.jpg");
    FolderCache::instance().invalidate(folder);
    c1.reload();
    c2.reload();
    c3.reload();
    ASSERT_TRUE(waitUntil([&] { return !c1.isLoading() && !c2.isLoading() && !c3.isLoading(); }));
    EXPECT_EQ(c1.count(), 4);
    EXPECT_EQ(c3.count(), 4);
    EXPECT_EQ(FolderCache::instance().enumerationCount(folder) - before, 2);
    c1.refresh(); // explicit refresh reads again
    ASSERT_TRUE(waitUntil([&] { return !c1.isLoading(); }));
    EXPECT_EQ(FolderCache::instance().enumerationCount(folder) - before, 3);
}

// ---- Collection ----

// Covers: F-OPEN-04, F-FILE-01 (multi-file collections only take back their own files)
TEST(ReviewCollection, F_OPEN_04_InsertOnlyOwnFiles) {
    test::ensureApp();
    QTemporaryDir dir;
    const QString a = writeFile(dir.filePath("a.jpg"));
    const QString b = writeFile(dir.filePath("b.jpg"));
    const QString c = writeFile(dir.filePath("c.jpg"));
    Collection col;
    col.openFiles({a, b});
    ASSERT_TRUE(waitUntil([&] { return !col.isLoading(); }));
    col.insertPath(c); // same folder, but not opened: ignored
    col.removePath(b);
    col.insertPath(b, false, true); // its own file: back and current
    ASSERT_TRUE(waitUntil([&] { return !col.hasPendingInserts(); }));
    EXPECT_EQ(col.count(), 2);
    EXPECT_EQ(col.indexOf(c), -1);
    EXPECT_EQ(col.currentPath(), QDir::cleanPath(b));
    col.insertPath(c, true); // explicitly added
    ASSERT_TRUE(waitUntil([&] { return !col.hasPendingInserts(); }));
    EXPECT_EQ(col.count(), 3);
}

// Covers: F-FILE-04 (rename in place: current stays, new position by name)
TEST(ReviewCollection, F_FILE_04_RenameKeepsCurrent) {
    test::ensureApp();
    QTemporaryDir dir;
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"})
        writeFile(dir.filePath(QString::fromLatin1(n)));
    const QString folder = QDir::cleanPath(dir.path());
    Collection col;
    col.setSortSpec({SortKey::Name, true, SortSource::Manual});
    col.openFile(folder + "/a.jpg");
    ASSERT_TRUE(waitUntil([&] { return !col.isLoading(); }));
    ASSERT_TRUE(QFile::rename(folder + "/a.jpg", folder + "/z.jpg"));
    col.renamePath(folder + "/a.jpg", folder + "/z.jpg");
    EXPECT_EQ(col.currentPath(), folder + "/z.jpg");
    EXPECT_EQ(col.currentIndex(), 2);
    EXPECT_EQ(col.count(), 3);
}

// Covers: F-NAV-03 ("Zum nächsten Ordner": siblings with images in name order)
TEST(ReviewCollection, F_NAV_03_SiblingImageFolder) {
    test::ensureApp();
    QTemporaryDir dir;
    const QString root = QDir::cleanPath(dir.path());
    for (const char* f : {"Ordner 1", "Ordner 2", "Ordner 10", "leer"})
        ASSERT_TRUE(QDir(root).mkdir(QString::fromLatin1(f)));
    writeFile(root + "/Ordner 1/a.jpg");
    writeFile(root + "/Ordner 10/b.jpg");
    writeFile(root + "/leer/notes.txt");
    auto collator = createNameCollator();
    // Finder order: Ordner 1, Ordner 2 (no images), Ordner 10, leer (no images).
    EXPECT_EQ(collection::siblingImageFolder(root + "/Ordner 1", 1, collator.get()), root + "/Ordner 10");
    EXPECT_EQ(collection::siblingImageFolder(root + "/Ordner 10", -1, collator.get()), root + "/Ordner 1");
    EXPECT_EQ(collection::siblingImageFolder(root + "/Ordner 10", 1, collator.get()), QString());
    EXPECT_EQ(collection::siblingImageFolder(root + "/Ordner 1", -1, collator.get()), QString());
}

// Covers: F-PRIV-01 (missing POSIX permissions are not reported as a privacy denial)
TEST(ReviewCollection, F_PRIV_01_PosixDenialIsNotPrivacy) {
#ifdef Q_OS_UNIX
    if (::geteuid() == 0)
        GTEST_SKIP() << "root ignores permissions";
    QTemporaryDir dir;
    const QString locked = dir.filePath("locked");
    ASSERT_TRUE(QDir().mkpath(locked));
    writeFile(locked + "/a.jpg");
    ASSERT_EQ(::chmod(QFile::encodeName(locked).constData(), 0100), 0);
    bool denied = false, privacy = true;
    collection::enumerateFolder(locked, &denied, &privacy);
    ::chmod(QFile::encodeName(locked).constData(), 0755);
    EXPECT_TRUE(denied);
    EXPECT_FALSE(privacy); // EACCES; macOS privacy protection answers EPERM
#endif
}

// ---- RAW embedded previews (F-FMT-05 setting) ----

namespace {
class RecordingDecoder : public Decoder {
public:
    QString name() const override { return "rec"; }
    int priority() const override { return 100; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req, QString*) override {
        {
            QMutexLocker l(&m);
            requests << qMakePair(path, req.preferEmbeddedPreview);
        }
        DecodedImage d;
        d.fullSize = QSize(4000, 3000);
        d.image =
            QImage(req.maxPixelSize > 0 ? QSize(req.maxPixelSize, req.maxPixelSize * 3 / 4) : QSize(400, 300),
                   QImage::Format_RGB32);
        d.image.fill(Qt::black);
        d.isPreview = true;
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }
    bool screenUsedEmbedded(const QString& path) {
        QMutexLocker l(&m);
        int embedded = 0, total = 0;
        for (const auto& r : requests)
            if (r.first == path) {
                ++total;
                embedded += r.second ? 1 : 0;
            }
        return total > 0 && embedded == total;
    }
    QMutex m;
    QList<QPair<QString, bool>> requests;
};
} // namespace

// Covers: F-FMT-05 (setting "RAW: nur eingebettete Vorschau" affects every stage of RAW files)
TEST(ReviewImageProvider, F_FMT_05_RawEmbeddedOnly) {
    test::ensureApp();
    DecoderRegistry registry;
    auto decoder = std::make_shared<RecordingDecoder>();
    registry.add(decoder);
    ImageProvider provider(registry);
    int ready = 0;
    QObject::connect(&provider, &ImageProvider::imageReady, [&](const QString&, Quality q, DecodedImagePtr) {
        if (q != Quality::Preview)
            ++ready;
    });
    int client = 0;
    provider.setRawEmbeddedOnly(true);
    EXPECT_TRUE(provider.rawEmbeddedOnly());
    provider.request("/raw/a.cr3", Quality::Screen, QSize(1920, 1080), &client);
    ASSERT_TRUE(waitUntil([&] { return ready >= 1; }));
    EXPECT_TRUE(decoder->screenUsedEmbedded("/raw/a.cr3"));
    // JPEGs are not affected; with the setting off RAWs are developed.
    provider.request("/raw/b.jpg", Quality::Screen, QSize(1920, 1080), &client);
    ASSERT_TRUE(waitUntil([&] { return ready >= 2; }));
    EXPECT_FALSE(decoder->screenUsedEmbedded("/raw/b.jpg"));
    provider.setRawEmbeddedOnly(false);
    provider.request("/raw/c.nef", Quality::Screen, QSize(1920, 1080), &client);
    ASSERT_TRUE(waitUntil([&] { return ready >= 3; }));
    EXPECT_FALSE(decoder->screenUsedEmbedded("/raw/c.nef"));
}

// ---- Settings location (SPEC 8.6) ----

// Covers: F-SET-04 (settings in io.github.brokoskokoli.lumaflip.plist, data in Application Support/Lumaflip)
TEST(ReviewSettings, F_SET_04_LocationsFollowSpec86) {
    test::ensureApp();
    const QString org = QCoreApplication::organizationName();
    const QString domain = QCoreApplication::organizationDomain();
    const QString name = QCoreApplication::applicationName();
    Settings::configureApplication();
#ifdef Q_OS_MACOS
    EXPECT_TRUE(
        Settings::nativeSettingsPath().endsWith("/Library/Preferences/io.github.brokoskokoli.lumaflip.plist"))
        << Settings::nativeSettingsPath().toStdString();
    EXPECT_TRUE(Settings::appDataPath().endsWith("/Library/Application Support/Lumaflip"))
        << Settings::appDataPath().toStdString();
#endif
    EXPECT_EQ(QCoreApplication::applicationName(), "Lumaflip"); // display name unchanged
    QCoreApplication::setOrganizationName(org);
    QCoreApplication::setOrganizationDomain(domain);
    QCoreApplication::setApplicationName(name);
}

// Covers: F-SET-04 (old values are migrated once, nothing overwritten)
TEST(ReviewSettings, F_SET_04_LegacyValuesMigratedOnce) {
    test::ensureApp();
    QTemporaryDir dir;
    QSettings oldS(dir.filePath("old.ini"), QSettings::IniFormat);
    oldS.setValue("view/zoomMode", "actual");
    oldS.setValue("window/openBehavior", "replaceFront");
    QSettings newS(dir.filePath("new.ini"), QSettings::IniFormat);
    newS.setValue("view/zoomMode", "fit"); // already set in the new location: kept
    EXPECT_EQ(Settings::migrateValues(oldS, newS), 1);
    EXPECT_EQ(newS.value("view/zoomMode").toString(), "fit");
    EXPECT_EQ(newS.value("window/openBehavior").toString(), "replaceFront");
    oldS.setValue("view/background", "light");
    EXPECT_EQ(Settings::migrateValues(oldS, newS), 0); // only once
    EXPECT_FALSE(newS.contains("view/background"));
    // App data file.
    QDir().mkpath(dir.filePath("Lumaflip"));
    writeFile(dir.filePath("Lumaflip/Lumaflip-old.json"), "{\"a\":1}");
    const QString newFile = dir.filePath("data/folders.json");
    EXPECT_TRUE(Settings::migrateFile(dir.filePath("Lumaflip/Lumaflip-old.json"), newFile));
    EXPECT_TRUE(QFileInfo::exists(newFile));
    EXPECT_FALSE(Settings::migrateFile(dir.filePath("Lumaflip/Lumaflip-old.json"), newFile)); // exists
}
