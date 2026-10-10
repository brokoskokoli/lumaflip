// SPDX-License-Identifier: GPL-3.0-or-later
// Export / convert (F-EDIT-04), portable part: format list, file name pattern, planning
// (never the source, unique names), conflict choices, cancel between images.

#include "ExifTestWriter.h"
#include "core/ImageExport.h"
#include "core/Platform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace lf;
using namespace lf::test;

namespace {

QByteArray readBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

// Moves trashed files into a folder (no real trash in tests).
class ExportTestTrash : public TrashService {
public:
    explicit ExportTestTrash(QString folder) : m_folder(std::move(folder)) { QDir().mkpath(m_folder); }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        const QString to = QDir(m_folder).filePath(QFileInfo(path).fileName());
        if (!QFile::rename(path, to)) {
            if (error)
                *error = QStringLiteral("rename failed");
            return std::nullopt;
        }
        trashed << path;
        return to;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString*) override {
        return QFile::rename(trashed, original);
    }
    QStringList trashed;

private:
    QString m_folder;
};

exporting::Options pngOptions() {
    exporting::Options o;
    o.formatId = QStringLiteral("png");
    o.metadata = exporting::MetadataMode::RemoveAll;
    return o;
}

bool canWrite(const QString& id) {
    for (const exporting::Format& f : exporting::availableFormats())
        if (f.id == id)
            return true;
    return false;
}

} // namespace

TEST(Export, F_EDIT_04_FormatListFollowsEncoders) {
    const QVector<exporting::Format> some =
        exporting::availableFormats({QStringLiteral("public.png"), QStringLiteral("public.jpeg")});
    ASSERT_EQ(some.size(), 2);
    EXPECT_EQ(some[0].id, QStringLiteral("jpeg")); // menu order, not encoder order
    EXPECT_EQ(some[1].id, QStringLiteral("png"));
    // JPEG XL only when an encoder exists (macOS ImageIO decodes it but cannot write it).
    EXPECT_TRUE(exporting::availableFormats({QStringLiteral("public.heic")}).first().gainMap);
    for (const exporting::Format& f : exporting::availableFormats({}))
        ADD_FAILURE() << f.id.toStdString();
    const QStringList encoders = platform::exportTypeIdentifiers();
    for (const exporting::Format& f : exporting::availableFormats())
        EXPECT_TRUE(encoders.contains(f.typeId)) << f.id.toStdString();
    EXPECT_TRUE(canWrite(QStringLiteral("png")));
    EXPECT_TRUE(canWrite(QStringLiteral("jpeg")));
    EXPECT_EQ(exporting::formatById(QStringLiteral("tiff")).suffix, QStringLiteral("tif"));
    EXPECT_TRUE(exporting::formatById(QStringLiteral("nope")).id.isEmpty());
    for (auto m : {exporting::MetadataMode::KeepAll, exporting::MetadataMode::RemoveGps,
                   exporting::MetadataMode::RemoveAll})
        EXPECT_EQ(exporting::metadataModeFromId(exporting::metadataModeId(m)), m);
    for (auto m : {exporting::ColorMode::Keep, exporting::ColorMode::SRgb, exporting::ColorMode::DisplayP3})
        EXPECT_EQ(exporting::colorModeFromId(exporting::colorModeId(m)), m);
}

TEST(Export, F_EDIT_04_NamePatternAndPlanning) {
    exporting::Options o;
    o.formatId = QStringLiteral("png");
    EXPECT_EQ(exporting::targetFileName("/a/IMG_1.jpg", o, 0, 1), QStringLiteral("IMG_1-export.png"));
    o.pattern = QStringLiteral("{n} {name} ({format})");
    EXPECT_EQ(exporting::targetFileName("/a/IMG_1.jpg", o, 4, 120), QStringLiteral("005 IMG_1 (png).png"));
    o.pattern = QStringLiteral("a/b:c");
    EXPECT_EQ(exporting::targetFileName("/a/x.jpg", o, 0, 1), QStringLiteral("a-b-c.png"));
    o.pattern = QStringLiteral("   ");
    EXPECT_EQ(exporting::targetFileName("/a/x.jpg", o, 0, 1), QStringLiteral("x-export.png"));
    o.pattern = QStringLiteral("..hidden");
    EXPECT_EQ(exporting::targetFileName("/a/x.jpg", o, 0, 1), QStringLiteral("hidden.png"));
    // Members of archives and PDF pages.
    EXPECT_EQ(exporting::sourceBaseName("/c/Band.cbz#/k/012.jpg"), QStringLiteral("012"));
    EXPECT_EQ(exporting::sourceBaseName("/d/Plan.pdf#/0003"), QStringLiteral("Plan-3"));
    EXPECT_EQ(exporting::targetFolder("/c/Band.cbz#/k/012.jpg", exporting::Options{}), QStringLiteral("/c"));

    // Planning: never the source, unique inside the batch.
    QSet<QString> existing{QStringLiteral("/p/a.png"), QStringLiteral("/p/out/b.png")};
    auto exists = [&](const QString& p) { return existing.contains(p); };
    exporting::Options same;
    same.formatId = QStringLiteral("png");
    same.pattern = QStringLiteral("{name}");
    const auto items = exporting::planItems({"/p/a.png", "/q/a.jpg"}, same, exists);
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].target, QStringLiteral("/p/a 2.png")); // would be the source itself
    EXPECT_EQ(items[1].target, QStringLiteral("/q/a.png"));
    same.folder = QStringLiteral("/p/out");
    const auto batch = exporting::planItems({"/x/b.jpg", "/y/b.jpg", "/z/c.jpg"}, same, exists);
    EXPECT_EQ(batch[0].target, QStringLiteral("/p/out/b.png")); // exists: a conflict to ask about
    EXPECT_EQ(batch[1].target, QStringLiteral("/p/out/b 2.png"));
    EXPECT_EQ(exporting::conflicts(batch, exists), (QVector<int>{0}));
}

TEST(Export, F_EDIT_04_ConflictChoicesAndSourceUntouched) {
    QTemporaryDir dir;
    const QString src = dir.filePath("photo.png");
    ASSERT_TRUE(noisyPattern(QSize(40, 30)).save(src, "PNG"));
    const QByteArray original = readBytes(src);
    ExportTestTrash trash(dir.filePath("trash"));
    exporting::Options o = pngOptions();
    o.pattern = QStringLiteral("{name}-export");
    const QString existing = dir.filePath("photo-export.png");
    std::atomic_bool cancel{false};

    for (ConflictChoice choice : {ConflictChoice::Skip, ConflictChoice::KeepBoth, ConflictChoice::Replace}) {
        ASSERT_TRUE(writeBytes(existing, "old"));
        auto items = exporting::planItems({src}, o);
        ASSERT_EQ(exporting::conflicts(items).size(), 1);
        items[0].onConflict = choice;
        const exporting::Result r = exporting::run(items, o, cancel, {}, &trash);
        EXPECT_EQ(readBytes(src), original); // never touched
        switch (choice) {
        case ConflictChoice::Skip:
            EXPECT_EQ(r.skipped, 1);
            EXPECT_EQ(readBytes(existing), QByteArray("old"));
            break;
        case ConflictChoice::KeepBoth:
            EXPECT_EQ(r.written, 1);
            EXPECT_EQ(readBytes(existing), QByteArray("old"));
            EXPECT_TRUE(QFileInfo::exists(dir.filePath("photo-export 2.png")));
            break;
        case ConflictChoice::Replace:
            EXPECT_EQ(r.written, 1);
            EXPECT_EQ(trash.trashed, QStringList{existing});
            EXPECT_EQ(QImage(existing).size(), QSize(40, 30));
            break;
        }
        QFile::remove(existing);
    }
    // "Ersetzen" on the source itself (pattern = the same name and format) keeps both.
    exporting::Options same = pngOptions();
    same.pattern = QStringLiteral("{name}");
    QVector<exporting::Item> items{{src, src, ConflictChoice::Replace}};
    const exporting::Result r = exporting::run(items, same, cancel, {}, &trash);
    EXPECT_EQ(r.written, 1);
    EXPECT_EQ(readBytes(src), original);
    EXPECT_TRUE(QFileInfo::exists(dir.filePath("photo 2.png")));
    // No temporary files left behind.
    for (const QString& name : QDir(dir.path()).entryList(QDir::Files | QDir::Hidden))
        EXPECT_FALSE(name.startsWith(QLatin1Char('.'))) << name.toStdString();
}

TEST(Export, F_EDIT_04_CancelStopsBetweenImages) {
    QTemporaryDir dir;
    QStringList sources;
    for (int i = 0; i < 4; ++i) {
        const QString p = dir.filePath(QStringLiteral("s%1.png").arg(i));
        ASSERT_TRUE(noisyPattern(QSize(20, 20), quint32(i)).save(p, "PNG"));
        sources << p;
    }
    exporting::Options o = pngOptions();
    o.folder = dir.filePath("out");
    const auto items = exporting::planItems(sources, o);
    std::atomic_bool cancel{false};
    QList<int> seen;
    const exporting::Result r = exporting::run(
        items, o, cancel,
        [&](int done, int total, const QString&) {
            EXPECT_EQ(total, 4);
            seen << done;
            if (done == 1)
                cancel = true; // while the second image is about to start
        },
        nullptr);
    EXPECT_TRUE(r.cancelled);
    EXPECT_EQ(r.written, 2); // the started one finishes, nothing after it
    EXPECT_EQ(QDir(o.folder).entryList(QDir::Files).size(), 2);
    EXPECT_EQ(seen, (QList<int>{0, 1}));
}

TEST(Export, F_EDIT_04_MaxSizeAndFormatApplied) {
    QTemporaryDir dir;
    const QString src = dir.filePath("big.png");
    ASSERT_TRUE(noisyPattern(QSize(300, 200)).save(src, "PNG"));
    exporting::Options o;
    o.formatId = QStringLiteral("jpeg");
    o.maxLongEdge = 120;
    o.quality = 80;
    const QString out = dir.filePath("small.jpg");
    ASSERT_EQ(exporting::exportOne(src, out, o), QString());
    const QImage img(out);
    EXPECT_EQ(img.size(), QSize(120, 80));
    EXPECT_TRUE(readBytes(out).startsWith("\xFF\xD8"));
    // Never enlarged; an existing target is never overwritten by exportOne().
    o.maxLongEdge = 1000;
    const QString out2 = dir.filePath("same.jpg");
    ASSERT_EQ(exporting::exportOne(src, out2, o), QString());
    EXPECT_EQ(QImage(out2).size(), QSize(300, 200));
    EXPECT_FALSE(exporting::exportOne(src, out2, o).isEmpty());
}
