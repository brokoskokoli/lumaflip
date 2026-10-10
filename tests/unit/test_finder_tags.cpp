// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-06: reading and writing Finder tags of temporary files (TagService).

#include "core/Platform.h"

#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace lf;

namespace {

QString makeFile(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write("x");
    return path;
}

} // namespace

TEST(TagService, F_INFO_06_ReadWriteTagsOnTempFile) {
    auto service = createTagService();
    ASSERT_TRUE(service);
    if (!service->isSupported())
        GTEST_SKIP() << "no file tags on this platform";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = makeFile(dir, QStringLiteral("bild.jpg"));
    ASSERT_FALSE(path.isEmpty());

    QString error;
    auto tags = service->tags(path, &error);
    ASSERT_TRUE(tags.has_value()) << error.toStdString();
    EXPECT_TRUE(tags->isEmpty());

    const QStringList wanted{QStringLiteral("Lumaflip Test"), QStringLiteral("Grün"),
                             QStringLiteral("Ürlaub 2024")};
    ASSERT_TRUE(service->setTags(path, wanted, &error)) << error.toStdString();
    tags = service->tags(path, &error);
    ASSERT_TRUE(tags.has_value());
    EXPECT_EQ(*tags, wanted);

    // Removing one keeps the others; an empty list removes all.
    ASSERT_TRUE(service->setTags(path, {QStringLiteral("Grün")}, &error));
    EXPECT_EQ(service->tags(path, &error).value_or(QStringList()), QStringList{QStringLiteral("Grün")});
    ASSERT_TRUE(service->setTags(path, {}, &error));
    EXPECT_TRUE(service->tags(path, &error).value_or(QStringList{QStringLiteral("?")}).isEmpty());
}

TEST(TagService, F_INFO_06_MissingFileReportsError) {
    auto service = createTagService();
    if (!service->isSupported())
        GTEST_SKIP() << "no file tags on this platform";
    QTemporaryDir dir;
    const QString missing = dir.filePath(QStringLiteral("gibt-es-nicht.jpg"));
    QString error;
    EXPECT_FALSE(service->tags(missing, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
    error.clear();
    EXPECT_FALSE(service->setTags(missing, {QStringLiteral("A")}, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(TagService, F_INFO_06_TagsAreIndependentPerFile) {
    auto service = createTagService();
    if (!service->isSupported())
        GTEST_SKIP() << "no file tags on this platform";
    QTemporaryDir dir;
    const QString a = makeFile(dir, QStringLiteral("a.png"));
    const QString b = makeFile(dir, QStringLiteral("b.png"));
    QString error;
    ASSERT_TRUE(service->setTags(a, {QStringLiteral("Rot")}, &error));
    EXPECT_EQ(service->tags(a, &error).value_or(QStringList()), QStringList{QStringLiteral("Rot")});
    EXPECT_TRUE(service->tags(b, &error).value_or(QStringList{QStringLiteral("?")}).isEmpty());
}
