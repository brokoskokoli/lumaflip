// SPDX-License-Identifier: GPL-3.0-or-later
// FileOps with undo: trash (F-FILE-01), rename (F-FILE-04), move/copy (F-FILE-08).

#include "QtTestApp.h"
#include "core/FileOps.h"
#include "core/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QUndoStack>
#include <gtest/gtest.h>

using namespace lf;

namespace {

// Passes through to the real trash and remembers every location it returned, so the
// test can remove exactly those files from the user's trash afterwards.
class RecordingTrash : public TrashService {
public:
    RecordingTrash() : m_real(createTrashService()) {}
    ~RecordingTrash() override { cleanup(); }
    void cleanup() {
        for (const QString& p : std::as_const(locations)) {
            if (!p.isEmpty() && QFileInfo(p).isFile())
                QFile::remove(p); // only the exact path the API returned
        }
    }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        auto loc = m_real->moveToTrash(path, error);
        if (loc)
            locations << *loc;
        return loc;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        return m_real->restoreFromTrash(trashed, original, error);
    }
    QStringList locations;

private:
    std::unique_ptr<TrashService> m_real;
};

QString writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    return path;
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

class FileOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        lf::test::ensureQtApp();
        ASSERT_TRUE(dir.isValid());
        ops = std::make_unique<FileOps>(trash);
        QObject::connect(ops.get(), &FileOps::fileRemoved,
                         [this](const QString& p) { log << "removed " + p; });
        QObject::connect(ops.get(), &FileOps::fileAdded, [this](const QString& p) { log << "added " + p; });
        QObject::connect(ops.get(), &FileOps::fileRenamed,
                         [this](const QString& a, const QString& b) { log << "renamed " + a + " " + b; });
        QObject::connect(ops.get(), &FileOps::operationFailed, [this](const QString& e) { errors << e; });
    }
    QString path(const QString& name) const { return QDir(dir.path()).filePath(name); }

    QTemporaryDir dir;
    RecordingTrash trash;
    std::unique_ptr<FileOps> ops;
    QStringList log;
    QStringList errors;
};

} // namespace

TEST_F(FileOpsTest, F_FILE_01_TrashUndoRedo) {
    const QString p = writeFile(path("lumaflip-test-trash.jpg"), "pixels");
    QString err;
    ASSERT_TRUE(ops->moveToTrash(p, &err)) << err.toStdString();
    EXPECT_FALSE(QFileInfo::exists(p));
    ASSERT_EQ(trash.locations.size(), 1);
    const QString loc1 = trash.locations[0];
    EXPECT_TRUE(QFileInfo::exists(loc1)) << loc1.toStdString();
    EXPECT_TRUE(loc1.contains("/.Trash")) << loc1.toStdString();
    EXPECT_EQ(log, QStringList{"removed " + p});
    EXPECT_EQ(ops->undoStack()->count(), 1);

    ops->undoStack()->undo();
    EXPECT_TRUE(errors.isEmpty()) << errors.join("\n").toStdString();
    EXPECT_TRUE(QFileInfo::exists(p));
    EXPECT_EQ(readFile(p), "pixels");
    EXPECT_FALSE(QFileInfo::exists(loc1));
    EXPECT_EQ(log.last(), "added " + p);

    ops->undoStack()->redo();
    EXPECT_FALSE(QFileInfo::exists(p));
    ASSERT_EQ(trash.locations.size(), 2);
    EXPECT_TRUE(QFileInfo::exists(trash.locations[1]));
    EXPECT_EQ(log.last(), "removed " + p);
    trash.cleanup(); // keep the user's trash clean: only the exact returned path
    EXPECT_FALSE(QFileInfo::exists(trash.locations[1]));
}

TEST_F(FileOpsTest, F_FILE_01_UndoFailsIfOriginalPathTaken) {
    const QString p = writeFile(path("lumaflip-test-conflict.jpg"), "old");
    ASSERT_TRUE(ops->moveToTrash(p, nullptr));
    writeFile(p, "new");
    ops->undoStack()->undo();
    ASSERT_EQ(errors.size(), 1);
    EXPECT_EQ(readFile(p), "new"); // not overwritten
    EXPECT_TRUE(QFileInfo::exists(trash.locations[0]));
    EXPECT_EQ(ops->undoStack()->count(), 0); // failed command dropped
}

TEST_F(FileOpsTest, F_FILE_01_MissingFileIsError) {
    QString err;
    EXPECT_FALSE(ops->moveToTrash(path("does-not-exist.jpg"), &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_EQ(ops->undoStack()->count(), 0);
}

TEST_F(FileOpsTest, F_FILE_04_RenameUndoRedo) {
    const QString a = writeFile(path("a.jpg"), "A");
    const QString b = path("b.jpg");
    QString err;
    ASSERT_TRUE(ops->rename(a, "b.jpg", &err)) << err.toStdString();
    EXPECT_FALSE(QFileInfo::exists(a));
    EXPECT_EQ(readFile(b), "A");
    EXPECT_EQ(log.last(), "renamed " + a + " " + b);

    ops->undoStack()->undo();
    EXPECT_EQ(readFile(a), "A");
    EXPECT_FALSE(QFileInfo::exists(b));
    EXPECT_EQ(log.last(), "renamed " + b + " " + a);

    ops->undoStack()->redo();
    EXPECT_EQ(readFile(b), "A");
    EXPECT_EQ(log.last(), "renamed " + a + " " + b);
    EXPECT_EQ(log.size(), 3);
}

TEST_F(FileOpsTest, F_FILE_04_RenameConflictsAndInvalidNames) {
    const QString a = writeFile(path("a.jpg"), "A");
    writeFile(path("b.jpg"), "B");
    QString err;
    EXPECT_FALSE(ops->rename(a, "b.jpg", &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_EQ(readFile(path("b.jpg")), "B");
    EXPECT_FALSE(ops->rename(a, "x/y.jpg", &err));
    EXPECT_FALSE(ops->rename(a, "", &err));
    EXPECT_FALSE(ops->rename(a, "..", &err));
    EXPECT_EQ(readFile(a), "A");
    EXPECT_EQ(ops->undoStack()->count(), 0);
    EXPECT_TRUE(log.isEmpty());
}

TEST_F(FileOpsTest, F_FILE_04_CaseOnlyRename) {
    const QString a = writeFile(path("case.jpg"), "A");
    QString err;
    ASSERT_TRUE(ops->rename(a, "CASE.jpg", &err)) << err.toStdString();
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).contains("CASE.jpg"));
    ops->undoStack()->undo();
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).contains("case.jpg"));
}

TEST_F(FileOpsTest, F_FILE_08_MoveUndoAndConflict) {
    ASSERT_TRUE(QDir(dir.path()).mkdir("target"));
    const QString a = writeFile(path("m.jpg"), "M");
    const QString moved = path("target/m.jpg");
    QString err;
    ASSERT_TRUE(ops->moveTo(a, path("target"), &err)) << err.toStdString();
    EXPECT_FALSE(QFileInfo::exists(a));
    EXPECT_EQ(readFile(moved), "M");
    EXPECT_EQ(log, (QStringList{"removed " + a, "added " + moved}));

    ops->undoStack()->undo();
    EXPECT_EQ(readFile(a), "M");
    EXPECT_FALSE(QFileInfo::exists(moved));
    EXPECT_EQ(log.mid(2), (QStringList{"removed " + moved, "added " + a}));

    // Conflict: the UI decides (F-FILE-11), FileOps never overwrites.
    writeFile(moved, "other");
    EXPECT_FALSE(ops->moveTo(a, path("target"), &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_EQ(readFile(moved), "other");
    EXPECT_EQ(readFile(a), "M");

    EXPECT_FALSE(ops->moveTo(a, path("no-such-folder"), &err));
}

TEST_F(FileOpsTest, F_FILE_08_CopyUndoRedoAndConflict) {
    ASSERT_TRUE(QDir(dir.path()).mkdir("target"));
    const QString a = writeFile(path("c.jpg"), "C");
    const QString copy = path("target/c.jpg");
    QString err;
    ASSERT_TRUE(ops->copyTo(a, path("target"), &err)) << err.toStdString();
    EXPECT_EQ(readFile(a), "C");
    EXPECT_EQ(readFile(copy), "C");
    EXPECT_EQ(log, QStringList{"added " + copy});

    ops->undoStack()->undo();
    EXPECT_FALSE(QFileInfo::exists(copy));
    EXPECT_EQ(readFile(a), "C");
    EXPECT_EQ(log.last(), "removed " + copy);

    ops->undoStack()->redo();
    EXPECT_EQ(readFile(copy), "C");

    EXPECT_FALSE(ops->copyTo(a, path("target"), &err)); // exists now
    EXPECT_FALSE(err.isEmpty());
}
