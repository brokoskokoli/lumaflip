// SPDX-License-Identifier: GPL-3.0-or-later
// FSEvents folder watcher (F-OPEN-07, F-WIN-09).

#include "QtTestApp.h"
#include "core/Platform.h"
#include "platform/mac/MacFolderWatcher.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

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
    w1->watch(dir.path());  // ref-counted
    w2->watch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1);
    settle(c1);
    c2.count = 0;

    touch(QDir(dir.path()).filePath("x.jpg"));
    ASSERT_TRUE(waitUntil([&] { return c1.count > 0 && c2.count > 0; }, 2000));

    w1->unwatch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1);  // w1 still holds one ref, w2 one
    w1->unwatch(dir.path());
    EXPECT_EQ(mac::activeFolderStreamCount(), base + 1);  // w2 still watching
    w2.reset();  // destruction releases its watches
    EXPECT_EQ(mac::activeFolderStreamCount(), base);
}
