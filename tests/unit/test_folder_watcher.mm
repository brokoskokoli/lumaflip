// SPDX-License-Identifier: GPL-3.0-or-later
// FSEvents folder watcher (F-OPEN-07, F-WIN-09).

#include "QtTestApp.h"
#include "core/Platform.h"
#include "platform/mac/MacFolderWatcher.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <sys/stat.h>

using namespace lf;
using lf::test::waitUntil;

namespace {

void touch(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write("x");
}

struct Counter {
    explicit Counter(FolderWatcher& w) {
        QObject::connect(&w, &FolderWatcher::folderChanged, [this](const QString& p) {
            ++count;
            last = p;
        });
    }
    int count = 0;
    QString last;
};

// FSEvents may deliver events from just before the stream started; let it settle.
void settle(Counter& c) {
    lf::test::spin(400);
    c.count = 0;
}

} // namespace

TEST(FolderWatcher, F_OPEN_07_CreateDeleteRename) {
    lf::test::ensureQtApp();
    QTemporaryDir dir;
    auto watcher = createFolderWatcher();
    Counter c(*watcher);
    watcher->watch(dir.path());
    settle(c);

    const QString a = QDir(dir.path()).filePath("a.jpg");
    touch(a);
    ASSERT_TRUE(waitUntil([&] { return c.count > 0; }, 2000)) << "create";
    EXPECT_EQ(c.last, dir.path());

    settle(c);
    ASSERT_TRUE(QFile::rename(a, QDir(dir.path()).filePath("b.jpg")));
    ASSERT_TRUE(waitUntil([&] { return c.count > 0; }, 2000)) << "rename";

    settle(c);
    ASSERT_TRUE(QFile::remove(QDir(dir.path()).filePath("b.jpg")));
    ASSERT_TRUE(waitUntil([&] { return c.count > 0; }, 2000)) << "delete";

    watcher->unwatch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), 0);
}

TEST(FolderWatcher, F_OPEN_07_IgnoresSubfolderChanges) {
    lf::test::ensureQtApp();
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkdir("sub"));
    auto watcher = createFolderWatcher();
    Counter c(*watcher);
    watcher->watch(dir.path());
    settle(c);
    touch(QDir(dir.path()).filePath("sub/deep.jpg"));
    lf::test::spin(800);
    EXPECT_EQ(c.count, 0);
    watcher->unwatch(dir.path());
}

// F-WIN-09: a folder shown in several windows is watched only once.
TEST(FolderWatcher, F_WIN_09_SharedStreamPerFolder) {
    lf::test::ensureQtApp();
    QTemporaryDir dir;
    const int base = mac::activeFolderStreamCount();
    auto w1 = createFolderWatcher();
    auto w2 = createFolderWatcher();
    Counter c1(*w1), c2(*w2);
    w1->watch(dir.path());
    w1->watch(dir.path()); // ref-counted
    w2->watch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1);
    settle(c1);
    c2.count = 0;

    touch(QDir(dir.path()).filePath("x.jpg"));
    ASSERT_TRUE(waitUntil([&] { return c1.count > 0 && c2.count > 0; }, 2000));

    w1->unwatch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1); // w1 still holds one ref, w2 one
    w1->unwatch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1); // w2 still watching
    w2.reset();                                          // destruction releases its watches
    EXPECT_EQ(mac::activeFolderStreamCount(), base);
}

// Review finding: FSEvents reports on-disk (often decomposed, NFD) bytes while Qt hands out
// NFC paths, so a folder named "Müll Fotos" created by Finder never matched.
TEST(FolderWatcher, F_OPEN_07_NonAsciiFolderNameNfd) {
    lf::test::ensureQtApp();
    QTemporaryDir dir;
    // Create the folder with decomposed bytes, as Finder does ("u" + U+0308).
    const QByteArray nfdName("Mu\xcc\x88ll Fotos");
    ASSERT_EQ(QString::fromUtf8(nfdName).normalized(QString::NormalizationForm_C),
              QString::fromUtf8("Müll Fotos"));
    const QByteArray nfdPath = dir.path().toUtf8() + '/' + nfdName;
    ASSERT_EQ(::mkdir(nfdPath.constData(), 0755), 0);
    const QString folder = QDir(dir.path()).filePath(QString::fromUtf8("Müll Fotos")); // NFC
    const int base = mac::activeFolderStreamCount();
    auto watcher = createFolderWatcher();
    Counter c(*watcher);
    watcher->watch(folder);
    settle(c);
    touch(QDir(folder).filePath(QString::fromUtf8("Bäume.jpg")));
    ASSERT_TRUE(waitUntil([&] { return c.count > 0; }, 3000)) << "create in NFD folder";
    EXPECT_EQ(c.last, folder);
    watcher->unwatch(folder);
    EXPECT_EQ(mac::activeFolderStreamCount(), base);
}

// Review finding: unwatch() recomputed the canonical key; after the folder was deleted the
// symlink no longer resolved and the stream leaked.
TEST(FolderWatcher, F_OPEN_07_UnwatchAfterDeletionClosesStream) {
    lf::test::ensureQtApp();
    QTemporaryDir dir;
    const int base = mac::activeFolderStreamCount();
    const QString real = QDir(dir.path()).filePath("real");
    const QString link = QDir(dir.path()).filePath("link");
    ASSERT_TRUE(QDir().mkpath(real));
    ASSERT_TRUE(QFile::link(real, link));
    auto watcher = createFolderWatcher();
    watcher->watch(link);
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1);
    ASSERT_TRUE(QDir(real).removeRecursively());
    watcher->unwatch(link);
    EXPECT_EQ(mac::activeFolderStreamCount(), base);
}

// F-OPEN-08: a tree watch (recursive collections) reports the subfolder whose contents
// changed, also for subfolders created after the watch started; one stream for the tree.
TEST(FolderWatcher, F_OPEN_08_TreeWatchReportsSubfolders) {
    lf::test::ensureQtApp();
    QTemporaryDir dir;
    const QString root = dir.path();
    ASSERT_TRUE(QDir(root).mkpath("Sub/Deep"));
    const int streamsBefore = mac::activeFolderStreamCount();
    auto watcher = createFolderWatcher();
    QStringList changed;
    QObject::connect(watcher.get(), &FolderWatcher::folderChanged, [&](const QString& p) { changed << p; });
    watcher->watchTree(root);
    EXPECT_EQ(mac::activeFolderStreamCount(), streamsBefore + 1);
    lf::test::spin(400);
    changed.clear();

    touch(QDir(root).filePath("Sub/Deep/a.jpg"));
    ASSERT_TRUE(waitUntil([&] { return changed.contains(QDir(root).filePath("Sub/Deep")); }, 3000))
        << changed.join(", ").toStdString();

    changed.clear();
    ASSERT_TRUE(QDir(root).mkpath("Neu"));
    touch(QDir(root).filePath("Neu/b.jpg"));
    ASSERT_TRUE(waitUntil([&] { return changed.contains(QDir(root).filePath("Neu")); }, 3000))
        << changed.join(", ").toStdString();
    EXPECT_TRUE(waitUntil([&] { return changed.contains(root); }, 3000)); // "Neu" appeared in the root

    watcher->unwatchTree(root);
    EXPECT_EQ(mac::activeFolderStreamCount(), streamsBefore);
}
