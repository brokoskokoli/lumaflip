// SPDX-License-Identifier: GPL-3.0-or-later
// .DS_Store reader (F-SORT-08). All fixtures are synthetic (SortingTestHelpers.h) -
// real .DS_Store files contain private file names and must not be committed.

#include "SortingTestHelpers.h"
#include "core/DsStore.h"

#include <gtest/gtest.h>

#include <QRandomGenerator>
#include <QTemporaryDir>

using namespace lf;

namespace {

QVector<DsStoreRecord> allTypeRecords() {
    return {
        {".", "vstl", "type", QStringLiteral("Nlsv")},
        {".", "dscl", "bool", true},
        {QString::fromUtf8("Ürlaub 😀"), "vSrn", "long", qint64(1)},
        {"a.jpg", "fwi0", "shor", qint64(-2)},
        {"a.jpg", "Iloc", "blob", QByteArray::fromHex("0000004000000020ffffffffffff0000")},
        {"b.png", "ph1S", "comp", qint64(1) << 40},
        {"b.png", "modD", "dutc", qint64(0x0000D5A4B3C2A1F0LL)},
        {"c.tif", "cmmt", "ustr", QString::fromUtf8("Kommentar äöü")},
    };
}

QVector<DsStoreRecord> manyRecords(int n) {
    QVector<DsStoreRecord> r;
    for (int i = 0; i < n; ++i)
        r.append({QStringLiteral("file%1.jpg").arg(i, 4, 10, QLatin1Char('0')), "Iloc", "blob",
                  QByteArray::fromHex("0000001000000020ffffffffffff0000")});
    return r;
}

} // namespace

TEST(DsStore, F_SORT_08_ParsesAllValueTypes) {
    const auto input = allTypeRecords();
    QString err;
    const auto recs = dsstore::parse(test::writeDsStore(input), &err);
    ASSERT_TRUE(recs.has_value()) << err.toStdString();
    ASSERT_EQ(recs->size(), input.size());
    for (qsizetype i = 0; i < input.size(); ++i) {
        EXPECT_EQ((*recs)[i].fileName, input[i].fileName);
        EXPECT_EQ((*recs)[i].code, input[i].code);
        EXPECT_EQ((*recs)[i].type, input[i].type);
    }
    EXPECT_EQ((*recs)[0].value.toString(), "Nlsv");
    EXPECT_TRUE((*recs)[1].value.toBool());
    EXPECT_EQ((*recs)[2].value.toLongLong(), 1);
    EXPECT_EQ((*recs)[3].value.toLongLong(), -2);
    EXPECT_EQ((*recs)[4].value.toByteArray().size(), 16);
    EXPECT_EQ((*recs)[5].value.toLongLong(), qint64(1) << 40);
    EXPECT_EQ((*recs)[6].value.toLongLong(), qint64(0x0000D5A4B3C2A1F0LL));
    EXPECT_EQ((*recs)[7].value.toString(), QString::fromUtf8("Kommentar äöü"));
}

TEST(DsStore, F_SORT_08_ParsesMultiLevelTree) {
    const auto input = manyRecords(500);
    const auto recs = dsstore::parse(test::writeDsStore(input, 37));
    ASSERT_TRUE(recs.has_value());
    ASSERT_EQ(recs->size(), 500);
    for (int i = 0; i < 500; ++i)
        EXPECT_EQ((*recs)[i].fileName, input[i].fileName); // in-order traversal
}

TEST(DsStore, F_SORT_08_EmptyStore) {
    const auto recs = dsstore::parse(test::writeDsStore({}));
    ASSERT_TRUE(recs.has_value());
    EXPECT_TRUE(recs->isEmpty());
}

TEST(DsStore, F_SORT_08_RejectsGarbage) {
    EXPECT_FALSE(dsstore::parse(QByteArray()).has_value());
    EXPECT_FALSE(dsstore::parse(QByteArray(4096, '\0')).has_value());
    EXPECT_FALSE(dsstore::parse(QByteArray("this is not a DS_Store file at all......")).has_value());
    QByteArray good = test::writeDsStore(allTypeRecords());
    QByteArray badMagic = good;
    badMagic[5] = 'X';
    EXPECT_FALSE(dsstore::parse(badMagic).has_value());
    QByteArray badOffsets = good;
    badOffsets[16] = char(0x7f); // header offset copy mismatch
    EXPECT_FALSE(dsstore::parse(badOffsets).has_value());
}

TEST(DsStore, F_SORT_08_TruncatedFilesNeverCrash) {
    const QByteArray full = test::writeDsStore(manyRecords(60), 7);
    ASSERT_TRUE(dsstore::parse(full).has_value());
    for (qsizetype n = 0; n < full.size(); n += 3)
        (void)dsstore::parse(full.left(n));
    SUCCEED();
}

TEST(DsStore, F_SORT_08_RandomMutationsNeverCrash) {
    QVector<QByteArray> seeds = {test::writeDsStore(allTypeRecords()),
                                 test::writeDsStore(manyRecords(80), 9)};
    QRandomGenerator rng(4711);
    for (const QByteArray& seed : seeds) {
        for (int iter = 0; iter < 5000; ++iter) {
            QByteArray m = seed;
            const int flips = 1 + int(rng.bounded(8));
            for (int i = 0; i < flips; ++i) {
                // Bias mutations towards the structural header/table/node area.
                const quint32 limit =
                    quint32(rng.bounded(4) == 0 ? m.size() : std::min<qsizetype>(m.size(), 2048));
                m[qsizetype(rng.bounded(limit))] = char(rng.bounded(256));
            }
            (void)dsstore::parse(m);
        }
    }
    SUCCEED();
}

TEST(DsStore, F_SORT_08_NodeCycleIsRejected) {
    // Make the B-tree root point to itself as its rightmost child.
    QByteArray file = test::writeDsStore(manyRecords(20), 5);
    const auto ok = dsstore::parse(file);
    ASSERT_TRUE(ok.has_value());
    // Locate the root node (block 2) via the block table and patch its "P" pointer to 2.
    const quint32 rootOff =
        (quint8(file[8]) << 24) | (quint8(file[9]) << 16) | (quint8(file[10]) << 8) | quint8(file[11]);
    const qsizetype tableEntry = 4 + qsizetype(rootOff) + 8 + 2 * 4;
    const quint32 addr = (quint8(file[tableEntry]) << 24) | (quint8(file[tableEntry + 1]) << 16) |
                         (quint8(file[tableEntry + 2]) << 8) | quint8(file[tableEntry + 3]);
    const qsizetype nodeOff = 4 + qsizetype(addr & ~0x1fu);
    file[nodeOff] = 0;
    file[nodeOff + 1] = 0;
    file[nodeOff + 2] = 0;
    file[nodeOff + 3] = 2;
    EXPECT_FALSE(dsstore::parse(file).has_value());
}

TEST(DsStore, F_SORT_08_UnknownTypeKeepsOtherRecords) {
    QVector<DsStoreRecord> recs = manyRecords(30);
    recs[3] = {"zzz", "xxxx", "????", QByteArray("\x01\x02\x03\x04", 4)};
    const auto parsed = dsstore::parse(test::writeDsStore(recs, 10));
    ASSERT_TRUE(parsed.has_value());
    // The node containing the unknown record is cut short, all other nodes survive.
    EXPECT_GE(parsed->size(), 20);
    EXPECT_LT(parsed->size(), 30);
}

TEST(DsStore, F_SORT_08_ReadFileMissingAndPresent) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(dsstore::readFile(dir.filePath("nope/.DS_Store")).has_value());
    const QString path = dir.filePath(".DS_Store");
    ASSERT_TRUE(test::writeFile(path, test::writeDsStore(allTypeRecords())));
    const auto recs = dsstore::readFile(path);
    ASSERT_TRUE(recs.has_value());
    EXPECT_EQ(recs->size(), allTypeRecords().size());
}
