// SPDX-License-Identifier: GPL-3.0-or-later
// Linux platform layer (src/platform/linux, built only on Linux): inotify folder watching
// incl. recursion (F-OPEN-07/08), the freedesktop trash with restore (F-FILE-01), file tags
// in user.xdg.tags (F-INFO-06), birth time via statx (F-SORT-02) and the decoder helper
// sandbox status (SPEC 8.5). Temporary directories only.

#include "QtTestApp.h"
#include "core/Platform.h"
#include "platform/generic/XdgDesktop.h"
#include "platform/linux/LinuxSandbox.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>

#include <gtest/gtest.h>

#include <sys/xattr.h>

using namespace lf;
using namespace lf::test;

namespace {

void touch(const QString& path, const QByteArray& data = "x") {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << path.toStdString();
    f.write(data);
}

struct Recorder {
    QStringList folders, sidecars, rescans;
    explicit Recorder(FolderWatcher& w) {
        QObject::connect(&w, &FolderWatcher::folderChanged, &w, [this](const QString& p) { folders << p; });
        QObject::connect(&w, &FolderWatcher::sidecarChanged, &w, [this](const QString& p) { sidecars << p; });
        QObject::connect(&w, &FolderWatcher::rescanNeeded, &w, [this](const QString& p) { rescans << p; });
    }
    void clear() {
        folders.clear();
        sidecars.clear();
        rescans.clear();
    }
};

} // namespace

TEST(LinuxFolderWatcher, F_OPEN_07_ReportsChangesOfAWatchedFolder) {
    ensureQtApp();
    QTemporaryDir dir;
    const QString root = QDir::cleanPath(dir.path());
    auto watcher = createFolderWatcher();
    Recorder rec(*watcher);
    watcher->watch(root);
    touch(root + "/a.jpg");
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root); }, 3000));
    // Many events are coalesced into few notifications.
    rec.clear();
    for (int i = 0; i < 50; ++i)
        touch(root + QStringLiteral("/b%1.jpg").arg(i));
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root); }, 3000));
    spin(100);
    EXPECT_LE(rec.folders.size(), 5);
    // Renames and deletions count; hidden files (except .DS_Store) do not.
    rec.clear();
    ASSERT_TRUE(QFile::rename(root + "/a.jpg", root + "/c.jpg"));
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root); }, 3000));
    rec.clear();
    touch(root + "/.hidden");
    spin(200);
    EXPECT_TRUE(rec.folders.isEmpty());
    // Sidecars are reported on their own (F-CULL-02 writes do not reload the folder).
    touch(root + "/c.XMP");
    ASSERT_TRUE(waitUntil([&] { return rec.sidecars.contains(root + "/c.XMP"); }, 3000));
    EXPECT_TRUE(rec.folders.isEmpty());
    // After unwatch: silence.
    watcher->unwatch(root);
    rec.clear();
    touch(root + "/d.jpg");
    spin(200);
    EXPECT_TRUE(rec.folders.isEmpty());
}

TEST(LinuxFolderWatcher, F_OPEN_08_WatchesTheWholeTreeIncludingNewFolders) {
    ensureQtApp();
    QTemporaryDir dir;
    const QString root = QDir::cleanPath(dir.path());
    ASSERT_TRUE(QDir(root).mkpath("a/b"));
    auto watcher = createFolderWatcher();
    Recorder rec(*watcher);
    watcher->watchTree(root);
    ASSERT_TRUE(waitUntil([&] { return !watcher->isSettingUp(); }, 5000));
    touch(root + "/a/b/deep.jpg");
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root + "/a/b"); }, 3000));
    // A folder created later is watched as well.
    rec.clear();
    ASSERT_TRUE(QDir(root).mkpath("new"));
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root); }, 3000));
    ASSERT_TRUE(waitUntil([&] { return !watcher->isSettingUp(); }, 5000));
    rec.clear();
    touch(root + "/new/x.jpg");
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root + "/new"); }, 3000));
    // A folder moved in with contents: reported, and watched from then on.
    QTemporaryDir outside;
    ASSERT_TRUE(QDir(outside.path()).mkpath("moved/inner"));
    touch(outside.path() + "/moved/inner/y.jpg");
    rec.clear();
    ASSERT_TRUE(QDir().rename(outside.path() + "/moved", root + "/moved"));
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root + "/moved"); }, 3000));
    ASSERT_TRUE(waitUntil([&] { return !watcher->isSettingUp(); }, 5000));
    rec.clear();
    touch(root + "/moved/inner/z.jpg");
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root + "/moved/inner"); }, 3000));
    // unwatchTree releases everything below.
    watcher->unwatchTree(root);
    rec.clear();
    touch(root + "/a/b/after.jpg");
    spin(200);
    EXPECT_TRUE(rec.folders.isEmpty());
}

namespace {
// inotify watches of this process (all inotify descriptors, /proc/self/fdinfo).
int inotifyWatchCount() {
    int n = 0;
    const QDir fdinfo(QStringLiteral("/proc/self/fdinfo"));
    for (const QString& fd : fdinfo.entryList(QDir::Files)) {
        QFile f(fdinfo.filePath(fd));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        for (const QByteArray& line : f.readAll().split('\n'))
            n += line.startsWith("inotify wd:") ? 1 : 0;
    }
    return n;
}
} // namespace

// Review (ports): the tree is walked on a worker thread (watchTree() returns at once, the GUI
// never waits for a large or slow tree), and folders moved out of a watched tree give their
// inotify watches back (they used to stay until the per-user limit ran out).
TEST(LinuxFolderWatcher, F_OPEN_08_TreeIsWalkedInTheBackgroundAndMovedFoldersAreReleased) {
    ensureQtApp();
    QTemporaryDir dir;
    const QString root = QDir::cleanPath(dir.path());
    for (int i = 0; i < 40; ++i)
        ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("t/f%1/g/h").arg(i)));
    const int before = inotifyWatchCount();
    auto watcher = createFolderWatcher();
    Recorder rec(*watcher);
    watcher->watchTree(root);
    EXPECT_TRUE(watcher->isSettingUp()) << "the walk runs on a worker thread";
    ASSERT_TRUE(waitUntil([&] { return !watcher->isSettingUp(); }, 10000));
    const int watched = inotifyWatchCount() - before;
    EXPECT_EQ(watched, 2 + 40 * 3); // root, t, and f<i>, g, h each
    // Moving a subtree out releases its watches (and reports the change).
    QTemporaryDir outside;
    ASSERT_TRUE(QDir().rename(root + "/t", outside.path() + "/t"));
    ASSERT_TRUE(waitUntil([&] { return rec.folders.contains(root); }, 3000));
    EXPECT_TRUE(waitUntil([&] { return inotifyWatchCount() - before == 1; }, 3000))
        << inotifyWatchCount() - before << " watches left";
    // Changes in the moved folders are not reported any more.
    rec.clear();
    touch(outside.path() + "/t/f1/g/x.jpg");
    spin(200);
    EXPECT_TRUE(rec.folders.isEmpty());
    // Giving up the tree while it is still walked leaves no watch behind.
    for (int i = 0; i < 40; ++i)
        ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("u/f%1/g").arg(i)));
    watcher->unwatchTree(root);
    watcher->watchTree(root + "/u");
    watcher->unwatchTree(root + "/u");
    ASSERT_TRUE(waitUntil([&] { return !watcher->isSettingUp(); }, 10000));
    EXPECT_EQ(inotifyWatchCount(), before);
}

TEST(LinuxTrash, F_FILE_01_MovesToFreedesktopTrashAndRestores) {
    QTemporaryDir dir;
    const QByteArray savedHome = qgetenv("XDG_DATA_HOME");
    const bool hadHome = qEnvironmentVariableIsSet("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(dir.filePath("data")));
    QDir().mkpath(dir.filePath("data"));
    QDir().mkpath(dir.filePath("photos"));
    const QString file = dir.filePath("photos/Urlaub ä.jpg");
    touch(file, "image");
    auto trash = createTrashService();
    QString error;
    const auto where = trash->moveToTrash(file, &error);
    ASSERT_TRUE(where.has_value()) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(file));
    EXPECT_TRUE(QFileInfo::exists(*where));
    EXPECT_TRUE(where->startsWith(dir.filePath("data/Trash/files/"))) << where->toStdString();
    // The .trashinfo file records the original location (file managers can restore it).
    EXPECT_EQ(xdg::trashInfoOriginalPath(*where).value_or(QString()), QFileInfo(file).absoluteFilePath());
    // Restore (⌘Z): back in place, the info file is gone.
    ASSERT_TRUE(trash->restoreFromTrash(*where, file, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_FALSE(QFileInfo::exists(*where));
    EXPECT_FALSE(QFileInfo::exists(xdg::trashInfoPath(*where)));
    // Never overwrites a file that took the place in the meantime.
    const auto again = trash->moveToTrash(file, &error);
    ASSERT_TRUE(again.has_value());
    touch(file, "new");
    EXPECT_FALSE(trash->restoreFromTrash(*again, file, &error));
    EXPECT_FALSE(error.isEmpty());
    if (hadHome)
        qputenv("XDG_DATA_HOME", savedHome);
    else
        qunsetenv("XDG_DATA_HOME");
}

TEST(LinuxTags, F_INFO_06_TagsAreStoredInUserXdgTags) {
    QTemporaryDir dir;
    const QString file = dir.filePath("t.jpg");
    touch(file);
    if (setxattr(QFile::encodeName(file).constData(), "user.lf.probe", "1", 1, 0) != 0)
        GTEST_SKIP() << "no user extended attributes on " << dir.path().toStdString();
    auto tags = createTagService();
    ASSERT_TRUE(tags->isSupported());
    QString error;
    ASSERT_TRUE(tags->setTags(file, {"Urlaub", "Familie, Freunde", "Urlaub"}, &error)) << error.toStdString();
    char buf[256] = {};
    const ssize_t n = getxattr(QFile::encodeName(file).constData(), "user.xdg.tags", buf, sizeof buf - 1);
    ASSERT_GT(n, 0);
    // Comma-separated like KDE Baloo/Dolphin; commas inside a tag are dropped.
    EXPECT_EQ(QByteArray(buf, int(n)), QByteArray("Urlaub,Familie Freunde"));
    // Written by another program (Dolphin): read back.
    ASSERT_EQ(setxattr(QFile::encodeName(file).constData(), "user.xdg.tags", "rot,blau", 8, 0), 0);
    EXPECT_EQ(tags->tags(file, &error).value_or(QStringList()), (QStringList{"rot", "blau"}));
    ASSERT_TRUE(tags->setTags(file, {}, &error));
    EXPECT_LT(getxattr(QFile::encodeName(file).constData(), "user.xdg.tags", buf, sizeof buf), 0);
}

TEST(LinuxFileAttributes, F_SORT_02_BirthTimeFromStatx) {
    QTemporaryDir dir;
    const QString file = dir.filePath("b.jpg");
    touch(file);
    FileEntry e;
    e.path = file;
    e.name = QStringLiteral("b.jpg");
    createFileAttributeProvider()->fill(e);
    EXPECT_FALSE(e.kind.isEmpty());
    if (!QFileInfo(file).birthTime().isValid())
        GTEST_SKIP() << "file system without birth time";
    ASSERT_TRUE(e.created.isValid());
    EXPECT_LE(qAbs(e.created.secsTo(QDateTime::currentDateTimeUtc())), 120);
}

TEST(LinuxSandbox, ReportsLandlockAbi) {
    // Informational: which Landlock ABI the CI kernel has (0 = none; seccomp is required).
    const int abi = linuxsandbox::landlockAbiVersion();
    std::printf("Landlock ABI: %d\n", abi);
    EXPECT_GE(abi, 0);
}
