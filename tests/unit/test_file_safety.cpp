// SPDX-License-Identifier: GPL-3.0-or-later
// File safety of saving the rotation (F-EDIT-02) and of renaming (F-FILE-04): symbolic and
// hard links, data behind the primary JPEG image (MPF gain maps), auxiliary HEIC images,
// undo after outside changes, the undo backup budget, durability (fsync before rename),
// exact re-encoding and the exclusive rename.

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/FileOps.h"
#include "core/LosslessTransform.h"
#include "core/Platform.h"
#include "core/UndoBackupStore.h"

#include <QBuffer>
#include <QColorSpace>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QUndoStack>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lf {
// FileOps.cpp test seam (runs between the "target taken?" check and the rename).
std::function<void(const QString& from, const QString& to)>& fileOpsBeforeRenameHook();
} // namespace lf

using namespace lf;
using namespace lf::test;

namespace {

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

QImage pattern(QSize size) {
    QImage img(size, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(img.rect(), Qt::white);
    const int w = size.width(), h = size.height();
    p.fillRect(0, 0, w / 2, h / 2, Qt::red);
    p.fillRect(w / 2, 0, w - w / 2, h / 2, Qt::green);
    p.fillRect(0, h / 2, w / 2, h - h / 2, Qt::blue);
    return img;
}

QByteArray jpegBytes(const QImage& img) {
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 90);
    return data;
}

std::shared_ptr<UndoBackupStore> makeStore(const QString& root, qint64 maxBytes = qint64(1) << 30) {
    UndoBackupStore::Options o;
    o.root = root;
    o.maxBytes = maxBytes;
    o.freeFraction = 1.0;
    return std::make_shared<UndoBackupStore>(o);
}

RotationRequest rotateRight(const QString& path) {
    RotationRequest r;
    r.path = path;
    r.quarterTurnsCW = 1;
    return r;
}

// A JPEG with an MPF (APP2) segment right after SOI and a second JPEG behind the primary
// one - the structure of Ultra HDR gain-map files and MPO stereo pairs.
QByteArray withMpfSecondImage(const QByteArray& primary, const QByteArray& secondary) {
    QByteArray mp; // MP header ("MM" byte order) + MP index IFD
    auto u16 = [&](quint16 v) { mp.append(char(v >> 8)).append(char(v & 0xFF)); };
    auto u32 = [&](quint32 v) {
        for (int i = 3; i >= 0; --i)
            mp.append(char((v >> (8 * i)) & 0xFF));
    };
    mp.append("MM", 2);
    u16(42);
    u32(8);
    u16(3);
    u16(0xB000); // MPFVersion
    u16(7);
    u32(4);
    mp.append("0100", 4);
    u16(0xB001); // NumberOfImages
    u16(4);
    u32(1);
    u32(2);
    const quint32 entriesOffset = 8 + 2 + 3 * 12 + 4;
    u16(0xB002); // MPEntry
    u16(7);
    u32(32);
    u32(entriesOffset);
    u32(0);                                                         // no next IFD
    const qsizetype segmentSize = 2 + 2 + 4 + (entriesOffset + 32); // marker + length + "MPF\0" + data
    const qsizetype primarySize = primary.size() + segmentSize;
    const qsizetype mpHeaderPos = 2 + 4 + 4; // SOI, marker+length, "MPF\0"
    u32(0x030000);                           // primary image (baseline)
    u32(quint32(primarySize));
    u32(0);
    u32(0);
    u32(0); // second image
    u32(quint32(secondary.size()));
    u32(quint32(primarySize - mpHeaderPos));
    u32(0);
    QByteArray seg;
    seg.append(char(0xFF)).append(char(0xE2));
    const int len = int(2 + 4 + mp.size());
    seg.append(char(len >> 8)).append(char(len & 0xFF));
    seg.append("MPF\0", 4);
    seg.append(mp);
    EXPECT_EQ(seg.size(), segmentSize);
    return primary.left(2) + seg + primary.mid(2) + secondary;
}

#ifdef Q_OS_UNIX
struct stat statOf(const QString& path) {
    struct stat st{};
    ::stat(QFile::encodeName(path).constData(), &st);
    return st;
}
#endif

} // namespace

// ---- 1: symbolic links ----

// The link stays a link; the real file is rewritten next to itself; undo/redo restore the
// real file, not the link.
TEST(FileSafety, F_EDIT_02_SymlinkKeepsLinkAndRewritesTarget) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    QDir root(dir.path());
    ASSERT_TRUE(root.mkdir("real") && root.mkdir("links"));
    const QString target = root.filePath("real/photo.jpg");
    const QString link = root.filePath("links/photo.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), target));
    ASSERT_TRUE(QFile::link(target, link));
    const QByteArray original = readFile(target);

    const RotationRequest r = rotateRight(link);
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.target, resolvedFilePath(target));
    RotationEdit edit(r, plan, plan.method, makeStore(root.filePath("undo")));
    ASSERT_EQ(edit.apply(), QString());

    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(resolvedFilePath(link), resolvedFilePath(target));
    EXPECT_EQ(jpeg::exifOrientation(readFile(target)), 6);
    EXPECT_EQ(QDir(root.filePath("links")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
              QStringList{"photo.jpg"});
    EXPECT_EQ(QDir(root.filePath("real")).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
              QStringList{"photo.jpg"});

    ASSERT_EQ(edit.revert(), QString());
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(readFile(target), original);
    ASSERT_EQ(edit.apply(), QString()); // redo
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(jpeg::exifOrientation(readFile(link)), 6);

    // restoreFileFrom() on its own resolves the link as well.
    const QString backup = root.filePath("backup.jpg");
    ASSERT_TRUE(writeFile(backup, original));
    ASSERT_EQ(restoreFileFrom(backup, link), QString());
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(readFile(target), original);
}

// ---- 2: data behind the primary JPEG image, auxiliary HEIC images ----

TEST(FileSafety, F_EDIT_02_JpegStructureHelpersFindSecondImages) {
    const QByteArray primary = jpegBytes(pattern(QSize(64, 48)));
    const QByteArray second = jpegBytes(pattern(QSize(16, 16)));
    EXPECT_EQ(jpeg::primaryImageEnd(primary), primary.size());
    EXPECT_FALSE(jpeg::hasAdditionalImages(primary));
    EXPECT_FALSE(jpeg::hasAdditionalImages(primary + QByteArray(64, '\0'))); // padding only
    EXPECT_TRUE(jpeg::hasAdditionalImages(primary + second));                // plain trailer
    const QByteArray mpf = withMpfSecondImage(primary, second);
    EXPECT_TRUE(jpeg::hasAdditionalImages(mpf));
    EXPECT_EQ(jpeg::primaryImageEnd(mpf), mpf.size() - second.size());
    if (!jpeg::dctTransformAvailable())
        return;
    // The failure being guarded against: the DCT transform keeps the primary image only.
    QByteArray out;
    QString error;
    ASSERT_EQ(jpeg::transformDct(mpf, 6, false, &out, &error), jpeg::DctResult::Ok);
    EXPECT_EQ(jpeg::primaryImageEnd(out), out.size()); // the second image is gone ...
    EXPECT_TRUE(out.contains(QByteArray("MPF\0", 4))); // ... but its MPF index still points to it
}

// Lossless JPEG strategy on a file with an MPF second image: no DCT transform (it would drop
// the gain map / second view); the EXIF orientation is set instead, with an explanation, and
// everything behind the primary image stays byte-identical.
TEST(FileSafety, F_EDIT_02_MpfJpegFallsBackToExifAndKeepsSecondImage) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString path = dir.filePath("ultrahdr.jpg");
    const QByteArray primary = readFile(corpusFile("orient_1.jpg")); // has an EXIF orientation tag
    const QByteArray second = jpegBytes(pattern(QSize(16, 16)));
    const QByteArray original = withMpfSecondImage(primary, second);
    ASSERT_TRUE(writeFile(path, original));
    const qsizetype end = jpeg::primaryImageEnd(original);
    ASSERT_GT(end, 0);

    RotationRequest r = rotateRight(path);
    r.strategy = RotationStrategy::LosslessJpeg;
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.method, RotationMethod::PatchExif);
    EXPECT_TRUE(plan.notice.contains(QStringLiteral("Gain-Map")));
    EXPECT_TRUE(plan.notice.contains(QStringLiteral("EXIF")));
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
    const QByteArray after = readFile(path);
    EXPECT_EQ(jpeg::exifOrientation(after), 6);
    EXPECT_EQ(after.size(), original.size());
    EXPECT_EQ(after.mid(jpeg::primaryImageEnd(after)), original.mid(end)); // second image intact
    EXPECT_TRUE(jpeg::hasAdditionalImages(after));

    // A DCT transform requested anyway is refused; the file stays as it is.
    if (jpeg::dctTransformAvailable()) {
        RotationPlan forced = plan;
        forced.stamp = fileStamp(path);
        forced.pixelOp = 6;
        const QByteArray before = readFile(path);
        EXPECT_FALSE(executeRotationSave(r, forced, RotationMethod::JpegDct).isEmpty());
        EXPECT_EQ(readFile(path), before);
    }
}

#ifndef LF_GENERIC_PLATFORM
// A real ISO gain-map JPEG written by ImageIO (corpus): the lossless strategy falls back to
// the orientation tag; the gain map survives.
TEST(FileSafety, F_EDIT_02_GainMapJpegKeepsGainMap) {
    LF_REQUIRE_CORPUS();
    if (!QFileInfo::exists(corpusFile("gainmap.jpg")))
        GTEST_SKIP() << "gainmap.jpg not in the corpus (ImageIO gain map encoder missing)";
    QTemporaryDir dir;
    const QString path = dir.filePath("gainmap.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("gainmap.jpg"), path));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray original = readFile(path);
    ASSERT_TRUE(jpeg::hasAdditionalImages(original));
    RotationRequest r = rotateRight(path);
    r.strategy = RotationStrategy::LosslessJpeg;
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    EXPECT_NE(plan.method, RotationMethod::JpegDct);
    EXPECT_FALSE(plan.notice.isEmpty());
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
    const QByteArray after = readFile(path);
    EXPECT_EQ(after.mid(jpeg::primaryImageEnd(after)), original.mid(jpeg::primaryImageEnd(original)));
    EXPECT_EQ(jpeg::scanData(after), jpeg::scanData(original));
}

// HEIC with an ISO gain map: the platform's metadata copy would lose (or cannot keep) the
// auxiliary image - refused with an explanation, the file is untouched.
TEST(FileSafety, F_EDIT_02_HeicGainMapNeverLost) {
    LF_REQUIRE_CORPUS();
    if (!QFileInfo::exists(corpusFile("gainmap.heic")))
        GTEST_SKIP() << "gainmap.heic not in the corpus";
    QTemporaryDir dir;
    const QString path = dir.filePath("gainmap.heic");
    ASSERT_TRUE(QFile::copy(corpusFile("gainmap.heic"), path));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray original = readFile(path);
    const RotationRequest r = rotateRight(path);
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    ASSERT_EQ(plan.method, RotationMethod::MetadataCopy);
    const QString result = executeRotationSave(r, plan, plan.method);
    SCOPED_TRACE(result.toStdString());
    if (result.isEmpty()) {
        // Only acceptable if the gain map is still there (checked by the platform layer):
        // a newer ImageIO may keep it.
        SUCCEED();
    } else {
        EXPECT_TRUE(result.contains(QStringLiteral("Gain-Map"))) << result.toStdString();
        EXPECT_EQ(readFile(path), original);
    }
    // No temporary file is left behind (ImageIO leaves its own one after a failed HEIC edit).
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"gainmap.heic"});
}
#endif

// ---- 3: undo/redo after outside changes ----

TEST(FileSafety, F_EDIT_02_UndoRefusedAfterOutsideChange) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString path = dir.filePath("photo.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), path));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    auto store = makeStore(dir.filePath("undo"));
    const RotationRequest r = rotateRight(path);
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready);
    RotationEdit edit(r, plan, plan.method, store);
    ASSERT_EQ(edit.apply(), QString());

    // Another program changes one byte, keeps size, inode and modification date: only the
    // content hash notices.
    QByteArray changed = readFile(path);
    const QDateTime mtime = QFileInfo(path).lastModified();
    changed[changed.size() - 3] = char(changed[changed.size() - 3] ^ 0x55);
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_EQ(f.write(changed), changed.size());
        f.close();
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_TRUE(f.setFileTime(mtime, QFileDevice::FileModificationTime));
    }
    EXPECT_EQ(edit.revert(), RotationEdit::messageChangedUndo());
    EXPECT_EQ(readFile(path), changed); // the outside change is kept
    EXPECT_EQ(RotationEdit::messageChangedUndo(),
              QStringLiteral("Datei wurde inzwischen verändert – Rückgängig nicht möglich"));

    // Redo after an outside change following an undo.
    const QString p2 = dir.filePath("second.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), p2));
    QFile::setPermissions(p2, QFile::ReadOwner | QFile::WriteOwner);
    const RotationRequest r2 = rotateRight(p2);
    RotationEdit edit2(r2, planRotationSave(r2), RotationMethod::PatchExif, store);
    ASSERT_EQ(edit2.apply(), QString());
    ASSERT_EQ(edit2.revert(), QString());
    ASSERT_TRUE(writeFile(p2, readFile(p2) + "x"));
    EXPECT_EQ(edit2.apply(), RotationEdit::messageChangedRedo());
    EXPECT_TRUE(readFile(p2).endsWith("x"));
}

// Through FileOps: the failed undo is reported and the command dropped from the stack.
TEST(FileSafety, F_EDIT_02_FileOpsDropsUndoAfterOutsideChange) {
    LF_REQUIRE_CORPUS();
    test::ensureQtApp();
    QTemporaryDir dir;
    const QString path = dir.filePath("photo.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), path));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    auto trash = createTrashService();
    FileOps ops(*trash);
    QStringList errors;
    QObject::connect(&ops, &FileOps::operationFailed, [&](const QString& e) { errors << e; });
    const RotationRequest r = rotateRight(path);
    auto edit = std::make_shared<RotationEdit>(r, planRotationSave(r), RotationMethod::PatchExif,
                                               makeStore(dir.filePath("undo")));
    bool done = false, ok = false;
    ops.modifyAsync(
        path, QStringLiteral("Drehung speichern"), [edit] { return edit->apply(); },
        [edit] { return edit->revert(); },
        [&](bool success, const QString&) {
            done = true;
            ok = success;
        });
    ASSERT_TRUE(test::waitUntil([&] { return done; }, 5000));
    ASSERT_TRUE(ok);
    ASSERT_EQ(ops.undoStack()->count(), 1);
    ASSERT_TRUE(writeFile(path, "edited elsewhere"));
    ops.undo();
    ASSERT_TRUE(test::waitUntil([&] { return !errors.isEmpty(); }, 5000));
    EXPECT_EQ(errors.last(), RotationEdit::messageChangedUndo());
    EXPECT_EQ(readFile(path), QByteArray("edited elsewhere"));
    EXPECT_TRUE(test::waitUntil([&] { return ops.undoStack()->count() == 0; }, 2000));
}

// ---- 4: undo backup budget, clones, stale sessions ----

TEST(FileSafety, F_EDIT_02_UndoBackupsStayWithinBudget) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const qint64 size = QFileInfo(corpusFile("orient_1.jpg")).size();
    // Each edit: "before" counts fully, "after" only if it is no clone. Room for 2.5 files.
    auto store = makeStore(dir.filePath("undo"), 5 * size / 2);
    std::vector<std::unique_ptr<RotationEdit>> edits;
    QStringList files;
    for (int i = 0; i < 4; ++i) {
        const QString p = dir.filePath(QStringLiteral("p%1.jpg").arg(i));
        ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), p));
        QFile::setPermissions(p, QFile::ReadOwner | QFile::WriteOwner);
        const RotationRequest r = rotateRight(p);
        edits.push_back(
            std::make_unique<RotationEdit>(r, planRotationSave(r), RotationMethod::PatchExif, store));
        ASSERT_EQ(edits.back()->apply(), QString()) << i;
        files << p;
        EXPECT_LE(store->usedBytes(), store->budgetBytes());
    }
    // The oldest edit lost its backups: undo says so and leaves the file alone.
    const QByteArray saved = readFile(files.first());
    EXPECT_EQ(edits.front()->revert(), RotationEdit::messageBackupDropped());
    EXPECT_EQ(readFile(files.first()), saved);
    // The newest one still works.
    ASSERT_EQ(edits.back()->revert(), QString());
    EXPECT_EQ(jpeg::exifOrientation(readFile(files.last())), 1);

    // Too large for the budget at all: nothing is saved.
    auto tiny = makeStore(dir.filePath("tiny"), size / 2);
    const QString big = dir.filePath("big.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), big));
    QFile::setPermissions(big, QFile::ReadOwner | QFile::WriteOwner);
    const QByteArray bigBefore = readFile(big);
    const RotationRequest rb = rotateRight(big);
    RotationEdit refused(rb, planRotationSave(rb), RotationMethod::PatchExif, tiny);
    EXPECT_FALSE(refused.apply().isEmpty());
    EXPECT_EQ(readFile(big), bigBefore);

    // Released entries are deleted (on a worker); the session folder goes with the store.
    const QString session = store->sessionPath();
    edits.clear();
    store->waitForDeletions();
    EXPECT_EQ(store->usedBytes(), 0);
    EXPECT_EQ(QDir(session).entryList(QDir::Files), QStringList());
    store.reset();
    EXPECT_FALSE(QFileInfo::exists(session));
}

#ifndef LF_GENERIC_PLATFORM
// On APFS the copy for redo is a clone of the saved file: it costs nothing in the budget.
TEST(FileSafety, F_EDIT_02_UndoBackupsPreferClones) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString p = dir.filePath("p.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), p));
    QFile::setPermissions(p, QFile::ReadOwner | QFile::WriteOwner);
    bool cloned = false;
    QString error;
    ASSERT_TRUE(platform::copyWithMetadata(p, dir.filePath("probe"), &error, &cloned));
    if (!cloned)
        GTEST_SKIP() << "temporary folder does not support clones";
    auto store = makeStore(dir.filePath("undo"));
    const RotationRequest r = rotateRight(p);
    RotationEdit edit(r, planRotationSave(r), RotationMethod::PatchExif, store);
    ASSERT_EQ(edit.apply(), QString());
    EXPECT_EQ(store->usedBytes(), QFileInfo(p).size()); // "before" fully, "after" a clone
}
#endif

TEST(FileSafety, F_EDIT_02_StaleUndoSessionsRemovedAtStart) {
    QTemporaryDir dir;
    QDir root(dir.path());
    auto live = makeStore(dir.path());
    ASSERT_NE(live->create(), 0u); // creates the locked session folder
    const QString liveSession = live->sessionPath();

    // Crashed instance: lock file of a process that does not exist.
    const QString dead = root.filePath(UndoBackupStore::folderPrefix() + "dead");
    ASSERT_TRUE(root.mkdir(QFileInfo(dead).fileName()));
    ASSERT_TRUE(writeFile(QDir(dead).filePath(".lock"),
                          "99999999\nlf_unit_tests\n" + QSysInfo::machineHostName().toUtf8() + "\n"));
    ASSERT_TRUE(writeFile(QDir(dead).filePath("1-before"), "backup"));
    // Older version without a lock, untouched for two days.
    const QString old = root.filePath(UndoBackupStore::folderPrefix() + "old");
    ASSERT_TRUE(root.mkdir(QFileInfo(old).fileName()));
    ASSERT_TRUE(writeFile(QDir(old).filePath("before.jpg"), "x"));
#ifdef Q_OS_UNIX
    const qint64 twoDaysAgo = QDateTime::currentSecsSinceEpoch() - 2 * 86400;
    const struct timespec times[2] = {{twoDaysAgo, 0}, {twoDaysAgo, 0}};
    ASSERT_EQ(::utimensat(AT_FDCWD, QFile::encodeName(old).constData(), times, 0), 0);
#else
    GTEST_SKIP() << "folder dates";
#endif
    // Lock-less but recent (maybe just being created), and folders that are not ours.
    const QString fresh = root.filePath(UndoBackupStore::folderPrefix() + "fresh");
    ASSERT_TRUE(root.mkdir(QFileInfo(fresh).fileName()));
    ASSERT_TRUE(root.mkdir("Other-undo-folder"));

    EXPECT_EQ(UndoBackupStore::removeStaleSessions(dir.path()), 2);
    EXPECT_FALSE(QFileInfo::exists(dead));
    EXPECT_FALSE(QFileInfo::exists(old));
    EXPECT_TRUE(QFileInfo::exists(fresh));
    EXPECT_TRUE(QFileInfo::exists(liveSession));
    EXPECT_TRUE(QFileInfo::exists(root.filePath("Other-undo-folder")));
}

// ---- 5: durability ----

// The replacement is flushed (F_FULLFSYNC) before the rename, the folder after it.
TEST(FileSafety, F_EDIT_02_FullSyncBeforeRename) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString path = dir.filePath("photo.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), path));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    QStringList steps;
    platform::setDurabilityObserver([&](platform::DurabilityStep s, const QString& p) {
        static const char* names[] = {"sync", "rename", "inplace", "folder"};
        steps << QStringLiteral("%1 %2").arg(QLatin1String(names[int(s)]), QFileInfo(p).fileName());
    });
    const RotationRequest r = rotateRight(path);
    const RotationPlan plan = planRotationSave(r);
    const QString result = executeRotationSave(r, plan, plan.method);
    platform::setDurabilityObserver({});
    ASSERT_EQ(result, QString());
    ASSERT_EQ(steps.size(), 3) << steps.join(" | ").toStdString();
    EXPECT_TRUE(steps[0].startsWith("sync .photo.lumaflip-")) << steps[0].toStdString();
    EXPECT_EQ(steps[1], QStringLiteral("rename photo.jpg"));
    EXPECT_TRUE(steps[2].startsWith("folder "));
}

// ---- 6: hard links ----

#ifdef Q_OS_UNIX
// A file with two hard links: both names see the saved rotation, the inode (file ID) stays,
// and undo restores both again.
TEST(FileSafety, F_EDIT_02_HardlinkKeepsInodeAndAllNames) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString a = dir.filePath("a.jpg"), b = dir.filePath("b.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), a));
    QFile::setPermissions(a, QFile::ReadOwner | QFile::WriteOwner);
    ASSERT_EQ(::link(QFile::encodeName(a).constData(), QFile::encodeName(b).constData()), 0);
    const QByteArray original = readFile(a);
    const struct stat before = statOf(a);
    ASSERT_EQ(before.st_nlink, 2);

    QStringList steps;
    platform::setDurabilityObserver(
        [&](platform::DurabilityStep s, const QString&) { steps << QString::number(int(s)); });
    const RotationRequest r = rotateRight(a);
    RotationEdit edit(r, planRotationSave(r), RotationMethod::PatchExif, makeStore(dir.filePath("undo")));
    const QString result = edit.apply();
    platform::setDurabilityObserver({});
    ASSERT_EQ(result, QString());
    EXPECT_EQ(steps, (QStringList{QString::number(int(platform::DurabilityStep::FullSync)),
                                  QString::number(int(platform::DurabilityStep::InPlaceCopy))}));

    const struct stat after = statOf(a);
    EXPECT_EQ(after.st_ino, before.st_ino);
    EXPECT_EQ(after.st_nlink, 2);
    EXPECT_EQ(statOf(b).st_ino, before.st_ino);
    EXPECT_EQ(jpeg::exifOrientation(readFile(a)), 6);
    EXPECT_EQ(jpeg::exifOrientation(readFile(b)), 6);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), (QStringList{"a.jpg", "b.jpg"}));

    ASSERT_EQ(edit.revert(), QString());
    EXPECT_EQ(readFile(b), original);
    EXPECT_EQ(statOf(a).st_ino, before.st_ino);
    ASSERT_EQ(edit.apply(), QString());
    EXPECT_EQ(jpeg::exifOrientation(readFile(b)), 6);
    EXPECT_EQ(statOf(b).st_ino, before.st_ino);
}
#endif

// ---- 8: exact re-encoding ----

namespace {

// Moves the pixels of any byte-aligned format by `op` (EXIF orientation code).
QImage moved(const QImage& img, int op) {
    static const int m[9][4] = {{1, 0, 0, 1}, {1, 0, 0, 1},  {-1, 0, 0, 1},  {-1, 0, 0, -1}, {1, 0, 0, -1},
                                {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    const int* t = m[op];
    const int w = img.width(), h = img.height(), B = img.depth() / 8;
    const bool swap = t[0] == 0;
    QImage out(swap ? h : w, swap ? w : h, img.format());
    out.setColorTable(img.colorTable());
    const int minX = std::min(0, t[0] * (w - 1)) + std::min(0, t[1] * (h - 1));
    const int minY = std::min(0, t[2] * (w - 1)) + std::min(0, t[3] * (h - 1));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int X = t[0] * x + t[1] * y - minX, Y = t[2] * x + t[3] * y - minY;
            std::memcpy(out.scanLine(Y) + X * B, img.constScanLine(y) + x * B, size_t(B));
        }
    return out;
}

QString firstDifference(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.format() != b.format())
        return QStringLiteral("size/format %1x%2/%3 vs %4x%5/%6")
            .arg(a.width())
            .arg(a.height())
            .arg(int(a.format()))
            .arg(b.width())
            .arg(b.height())
            .arg(int(b.format()));
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y))
                return QStringLiteral("at %1,%2: %3 vs %4")
                    .arg(x)
                    .arg(y)
                    .arg(a.pixel(x, y), 8, 16)
                    .arg(b.pixel(x, y), 8, 16);
    return QStringLiteral("same pixels (color space or table differs?)");
}

QImage readBack(const QString& path) {
    QImageReader r(path);
    r.setAutoTransform(false);
    return r.read();
}

struct ReencodeCase {
    const char* name;
    QImage image;
    const char* format;
    QImage::Format compareAs;
    bool mustBeExact; // on macOS
};

std::vector<ReencodeCase> reencodeCases() {
    std::vector<ReencodeCase> cases;
    QImage gray(20, 10, QImage::Format_Grayscale8);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x)
            gray.scanLine(y)[x] = uchar(x * 12 + y);
    cases.push_back({"gray.png", gray, "png", QImage::Format_Grayscale8, true});

    QImage indexed(20, 10, QImage::Format_Indexed8);
    indexed.setColorTable({qRgb(10, 20, 30), qRgb(200, 0, 0), qRgb(0, 180, 0), qRgb(1, 2, 250)});
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x)
            indexed.setPixel(x, y, uint((x / 5 + y) % 4));
    cases.push_back({"indexed.png", indexed, "png", QImage::Format_Indexed8, false});

    // Straight (not premultiplied) alpha with values premultiplication would destroy.
    QImage alpha(20, 10, QImage::Format_RGBA8888);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x) {
            uchar* p = alpha.scanLine(y) + 4 * x;
            p[0] = uchar(200 + x);
            p[1] = uchar(100 + y);
            p[2] = uchar(7 * x);
            p[3] = uchar((x + y) % 4 == 0 ? 3 : 128 + x);
        }
    cases.push_back({"alpha.png", alpha, "png", QImage::Format_RGBA8888, true});

    QImage deep(20, 10, QImage::Format_RGBA64);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x) {
            auto* p = reinterpret_cast<quint16*>(deep.scanLine(y)) + 4 * x;
            p[0] = quint16(0x1234 + 977 * x);
            p[1] = quint16(0xABCD - 311 * y);
            p[2] = quint16(17 * x * y);
            p[3] = quint16(x == 0 ? 0x0101 : 0xFFFF - x);
        }
    cases.push_back({"deep.png", deep, "png", QImage::Format_RGBA64, true});

    QImage tiff16(20, 10, QImage::Format_RGBX64);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x) {
            auto* p = reinterpret_cast<quint16*>(tiff16.scanLine(y)) + 4 * x;
            p[0] = quint16(0x0102 * x + 3);
            p[1] = quint16(0x2001 + 5 * y);
            p[2] = quint16(0xFFF0 - x * y);
            p[3] = 0xFFFF;
        }
    cases.push_back({"rgb16.tiff", tiff16, "tiff", QImage::Format_RGBX64, true});
    return cases;
}

} // namespace

// Re-encoding without an orientation tag never converts silently: the pixels are moved as
// they are (gray stays gray, the palette stays, 16 bit stays, straight alpha is not
// premultiplied) - or the codec is reported as inexact (then the user is asked).
TEST(FileSafety, F_EDIT_02_ReencodeKeepsPixelsExactly) {
    QTemporaryDir dir;
    for (const ReencodeCase& c : reencodeCases()) {
        SCOPED_TRACE(c.name);
        const QString src = dir.filePath(QString::fromLatin1(c.name));
        {
            QImageWriter w(src, c.format);
            if (!w.write(c.image)) {
                ADD_FAILURE() << "cannot write test image: " << w.errorString().toStdString();
                continue;
            }
        }
        const QImage stored = readBack(src);
        ASSERT_FALSE(stored.isNull());
        for (int op : {6, 3, 2, 8}) {
            SCOPED_TRACE(op);
            const QString dst =
                dir.filePath(QStringLiteral("out-%1-%2").arg(op).arg(QString::fromLatin1(c.name)));
            QString error;
            bool inexact = false;
            const bool ok = platform::reencodeTransformed(src, dst, op, true, &error, &inexact);
            if (!ok) {
                EXPECT_TRUE(inexact) << error.toStdString();
                EXPECT_FALSE(QFileInfo::exists(dst));
#ifndef LF_GENERIC_PLATFORM
                EXPECT_FALSE(c.mustBeExact) << error.toStdString();
#endif
                continue;
            }
            const QImage back = readBack(dst);
            ASSERT_FALSE(back.isNull());
            // Pixel values only: ImageIO tags the file with the color space it decoded the
            // untagged source in (same numbers, same meaning).
            QImage expected = moved(stored.convertToFormat(c.compareAs), op);
            QImage actual = back.convertToFormat(c.compareAs);
            expected.setColorSpace(QColorSpace());
            actual.setColorSpace(QColorSpace());
            EXPECT_EQ(actual.size(), expected.size());
            EXPECT_TRUE(actual == expected) << firstDifference(actual, expected).toStdString();
            if (c.compareAs == QImage::Format_Indexed8) {
                EXPECT_EQ(back.format(), QImage::Format_Indexed8);
                EXPECT_EQ(back.colorTable(), stored.colorTable());
            }
            if (c.compareAs == QImage::Format_Grayscale8)
                EXPECT_TRUE(back.isGrayscale());
        }
    }
}

// The plan only promises "lossless" after the codec passed the exact check.
TEST(FileSafety, F_EDIT_02_PlanAsksWhenReencodeWouldNotBeExact) {
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "type detection by the platform";
#endif
    QTemporaryDir dir;
    const QString bmp = dir.filePath("p.bmp");
    ASSERT_TRUE(pattern(QSize(20, 10)).save(bmp));
    const RotationRequest r = rotateRight(bmp);
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.method, RotationMethod::ReencodeLossless);
    // Nothing was written next to the file while planning.
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"p.bmp"});

    // A palette BMP: ImageIO's BMP writer stores no palette - that would be a silent
    // conversion, so the user is asked (lossy re-encode) instead.
    QImage indexed(20, 10, QImage::Format_Indexed8);
    indexed.setColorTable({qRgb(10, 20, 30), qRgb(200, 0, 0), qRgb(0, 180, 0), qRgb(1, 2, 250)});
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x)
            indexed.setPixel(x, y, uint((x / 5 + y) % 4));
    const QString pal = dir.filePath("palette.bmp");
    ASSERT_TRUE(indexed.save(pal));
    const RotationPlan palPlan = planRotationSave(rotateRight(pal));
    EXPECT_EQ(palPlan.status, RotationPlan::NeedsReencode);
    EXPECT_EQ(palPlan.method, RotationMethod::ReencodeLossy);
    EXPECT_TRUE(palPlan.message.contains(QStringLiteral("Palette"))) << palPlan.message.toStdString();
}

// ---- 9: plan/commit gap ----

TEST(FileSafety, F_EDIT_02_ChangeBetweenPlanAndSaveIsNotOverwritten) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString path = dir.filePath("photo.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), path));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    const RotationRequest r = rotateRight(path);
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready);
    ASSERT_TRUE(plan.stamp.isValid());
    // While the user looks at a question, another program saves its own version.
    const QByteArray other = readFile(corpusFile("orient_3.jpg"));
    ASSERT_TRUE(writeFile(path, other));
    const QString result = executeRotationSave(r, plan, plan.method);
    EXPECT_TRUE(result.contains(QStringLiteral("inzwischen verändert"))) << result.toStdString();
    EXPECT_EQ(readFile(path), other);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"photo.jpg"});
}

// ---- 10: exclusive rename ----

// Another program creates the target name between FileOps' check and its rename: the file
// is not overwritten (renamex_np RENAME_EXCL), the rename fails with a message.
TEST(FileSafety, F_FILE_04_RenameNeverReplacesFileCreatedMeanwhile) {
    test::ensureQtApp();
    QTemporaryDir dir;
    const QString a = dir.filePath("a.jpg"), b = dir.filePath("b.jpg");
    ASSERT_TRUE(writeFile(a, "mine"));
    auto trash = createTrashService();
    FileOps ops(*trash);
    fileOpsBeforeRenameHook() = [&](const QString&, const QString& to) {
        if (to == b)
            writeFile(b, "theirs");
    };
    QString error;
    const bool ok = ops.rename(a, "b.jpg", &error);
    fileOpsBeforeRenameHook() = {};
    EXPECT_FALSE(ok);
    EXPECT_TRUE(error.contains(QStringLiteral("existiert bereits"))) << error.toStdString();
    EXPECT_EQ(readFile(b), QByteArray("theirs"));
    EXPECT_EQ(readFile(a), QByteArray("mine"));
    EXPECT_EQ(ops.undoStack()->count(), 0);

    // The case-only rename (same file on a case-insensitive volume) still works.
    ASSERT_TRUE(ops.rename(a, "A2.jpg", &error)) << error.toStdString();
    ASSERT_TRUE(ops.rename(dir.filePath("A2.jpg"), "a2.jpg", &error)) << error.toStdString();
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).contains("a2.jpg"));
}
