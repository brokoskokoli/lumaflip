// SPDX-License-Identifier: GPL-3.0-or-later
// Binary plist parser (used for .DS_Store blobs, F-SORT-08).

#include "SortingTestHelpers.h"
#include "core/BinaryPlist.h"

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QRandomGenerator>

using namespace lf;
using lf::test::BplistWriter;

namespace {

QVariantMap sampleListSettings() {
    QVariantList columns;
    columns.append(QVariantMap{{"identifier", "name"}, {"ascending", true}, {"width", 300}});
    columns.append(QVariantMap{{"identifier", "dateAdded"}, {"ascending", false}, {"visible", true}});
    return QVariantMap{{"sortColumn", "dateAdded"},
                       {"columns", columns},
                       {"textSize", 13.0},
                       {"calculateAllSizes", false},
                       {"viewOptionsVersion", 1}};
}

} // namespace

TEST(BinaryPlist, F_SORT_08_ParsesAllObjectTypes) {
    const QDateTime when(QDate(2024, 5, 17), QTime(12, 30, 15), QTimeZone::UTC);
    QVariantMap root{
        {"ascii", "hello"},
        {"utf16", QString::fromUtf8("Grüße 😀")},
        {"int", qint64(-42)},
        {"big", qint64(1) << 40},
        {"real", 2.5},
        {"t", true},
        {"f", false},
        {"date", when},
        {"data", QByteArray("\x00\x01\x02", 3)},
        {"uid", QVariant::fromValue(PlistUid{7})},
        {"long", QString(40, QChar('x'))}, // length >= 15 uses extended length int
        {"nested", sampleListSettings()},
    };
    QString err;
    const auto v = bplist::parse(BplistWriter::write(root), &err);
    ASSERT_TRUE(v.has_value()) << err.toStdString();
    const QVariantMap m = v->toMap();
    EXPECT_EQ(m.value("ascii").toString(), "hello");
    EXPECT_EQ(m.value("utf16").toString(), QString::fromUtf8("Grüße 😀"));
    EXPECT_EQ(m.value("int").toLongLong(), -42);
    EXPECT_EQ(m.value("big").toLongLong(), qint64(1) << 40);
    EXPECT_DOUBLE_EQ(m.value("real").toDouble(), 2.5);
    EXPECT_EQ(m.value("t").typeId(), QMetaType::Bool);
    EXPECT_TRUE(m.value("t").toBool());
    EXPECT_FALSE(m.value("f").toBool());
    EXPECT_EQ(m.value("date").toDateTime(), when);
    EXPECT_EQ(m.value("data").toByteArray(), QByteArray("\x00\x01\x02", 3));
    EXPECT_EQ(m.value("uid").value<PlistUid>().value, 7u);
    EXPECT_EQ(m.value("long").toString().size(), 40);
    const QVariantMap nested = m.value("nested").toMap();
    EXPECT_EQ(nested.value("sortColumn").toString(), "dateAdded");
    EXPECT_EQ(nested.value("columns").toList().size(), 2);
}

TEST(BinaryPlist, F_SORT_08_SmallIntsAndFloat32) {
    // Hand-built: array [int8 200, int16 513, float32 1.5]
    QByteArray b("bplist00");
    const int o0 = int(b.size());
    b.append("\xA3\x01\x02\x03", 4);
    const int o1 = int(b.size());
    b.append("\x10\xC8", 2);
    const int o2 = int(b.size());
    b.append("\x11\x02\x01", 3);
    const int o3 = int(b.size());
    b.append("\x22\x3F\xC0\x00\x00", 5);
    const int table = int(b.size());
    for (int o : {o0, o1, o2, o3})
        b.append(char(o));
    b.append(QByteArray(6, '\0'));
    b.append(char(1));
    b.append(char(1));
    test::putBE(b, 4, 8);
    test::putBE(b, 0, 8);
    test::putBE(b, quint64(table), 8);
    const auto v = bplist::parse(b);
    ASSERT_TRUE(v.has_value());
    const QVariantList l = v->toList();
    ASSERT_EQ(l.size(), 3);
    EXPECT_EQ(l[0].toLongLong(), 200);
    EXPECT_EQ(l[1].toLongLong(), 513);
    EXPECT_DOUBLE_EQ(l[2].toDouble(), 1.5);
}

TEST(BinaryPlist, F_SORT_08_RejectsBadMagicAndShortInput) {
    EXPECT_FALSE(bplist::parse(QByteArray()).has_value());
    EXPECT_FALSE(bplist::parse(QByteArray("bplist00")).has_value());
    QByteArray good = BplistWriter::write(QVariantMap{{"a", 1}});
    good[0] = 'x';
    EXPECT_FALSE(bplist::parse(good).has_value());
}

TEST(BinaryPlist, F_SORT_08_TruncatedInputNeverCrashes) {
    const QByteArray full = BplistWriter::write(sampleListSettings());
    ASSERT_TRUE(bplist::parse(full).has_value());
    for (qsizetype n = 0; n < full.size(); ++n) {
        const auto v = bplist::parse(full.left(n));
        (void)v; // must simply not crash; most prefixes are rejected
    }
    for (qsizetype n = 1; n < full.size(); ++n)
        (void)bplist::parse(full.mid(n));
    SUCCEED();
}

TEST(BinaryPlist, F_SORT_08_RandomMutationsNeverCrash) {
    const QByteArray full = BplistWriter::write(sampleListSettings());
    QRandomGenerator rng(1234);
    for (int iter = 0; iter < 20000; ++iter) {
        QByteArray m = full;
        const int flips = 1 + int(rng.bounded(6));
        for (int i = 0; i < flips; ++i)
            m[qsizetype(rng.bounded(quint32(m.size())))] = char(rng.bounded(256));
        (void)bplist::parse(m);
    }
    for (int iter = 0; iter < 2000; ++iter) {
        QByteArray junk("bplist00");
        const int len = int(rng.bounded(200));
        for (int i = 0; i < len; ++i)
            junk.append(char(rng.bounded(256)));
        (void)bplist::parse(junk);
    }
    SUCCEED();
}

TEST(BinaryPlist, F_SORT_08_RejectsCyclesAndHugeCounts) {
    // Object 0: array containing itself.
    QByteArray b("bplist00");
    b.append("\xA1\x00", 2);
    const int table = int(b.size());
    b.append(char(8));
    b.append(QByteArray(6, '\0'));
    b.append(char(1));
    b.append(char(1));
    test::putBE(b, 1, 8);
    test::putBE(b, 0, 8);
    test::putBE(b, quint64(table), 8);
    QString err;
    EXPECT_FALSE(bplist::parse(b, &err).has_value());
    EXPECT_FALSE(err.isEmpty());

    // Array claiming 2^32 elements.
    QByteArray h("bplist00");
    h.append("\xAF\x13\x00\x00\x00\x01\x00\x00\x00\x00", 10);
    const int t2 = int(h.size());
    h.append(char(8));
    h.append(QByteArray(6, '\0'));
    h.append(char(1));
    h.append(char(1));
    test::putBE(h, 1, 8);
    test::putBE(h, 0, 8);
    test::putBE(h, quint64(t2), 8);
    EXPECT_FALSE(bplist::parse(h).has_value());

    // Trailer with absurd object count / offset table outside the file.
    QByteArray t = BplistWriter::write(QVariantList{1, 2});
    QByteArray bad = t;
    bad.replace(bad.size() - 24, 8, QByteArray::fromHex("7fffffffffffffff"));
    EXPECT_FALSE(bplist::parse(bad).has_value());
    bad = t;
    bad.replace(bad.size() - 8, 8, QByteArray::fromHex("00000000ffffffff"));
    EXPECT_FALSE(bplist::parse(bad).has_value());
}

TEST(BinaryPlist, F_SORT_08_SharedSubobjectsDoNotExplode) {
    // 40 levels where each array references the next one twice: 2^40 visits without a cap.
    const int levels = 40;
    QByteArray b("bplist00");
    QVector<int> offs;
    for (int i = 0; i < levels; ++i) {
        offs.append(int(b.size()));
        b.append(char(0xA2));
        b.append(char(i + 1));
        b.append(char(i + 1));
    }
    offs.append(int(b.size()));
    b.append(char(0x09));
    const int table = int(b.size());
    for (int o : offs)
        test::putBE(b, quint64(o), 2);
    b.append(QByteArray(6, '\0'));
    b.append(char(2));
    b.append(char(1));
    test::putBE(b, quint64(offs.size()), 8);
    test::putBE(b, 0, 8);
    test::putBE(b, quint64(table), 8);
    EXPECT_FALSE(bplist::parse(b).has_value());
}

namespace {

// Root array of `refs` references to one data object of `dataBytes` bytes (4-byte refs).
QByteArray sharedDataPlist(int refs, int dataBytes) {
    QByteArray b("bplist00");
    const quint64 o0 = quint64(b.size());
    b.append(char(0xAF));
    b.append(char(0x12)); // 4-byte count
    test::putBE(b, quint64(refs), 4);
    for (int i = 0; i < refs; ++i)
        test::putBE(b, 1, 4);
    const quint64 o1 = quint64(b.size());
    b.append(char(0x4F));
    b.append(char(0x12));
    test::putBE(b, quint64(dataBytes), 4);
    b.append(QByteArray(dataBytes, 'x'));
    const quint64 table = quint64(b.size());
    test::putBE(b, o0, 8);
    test::putBE(b, o1, 8);
    b.append(QByteArray(6, '\0'));
    b.append(char(8));
    b.append(char(4));
    test::putBE(b, 2, 8);
    test::putBE(b, 0, 8);
    test::putBE(b, table, 8);
    return b;
}

} // namespace

TEST(BinaryPlist, ReviewSharedDataIsNotCopiedPerReference) {
    // Many references to one 64 KiB data object: the parsed list must share one buffer.
    const QByteArray plist = sharedDataPlist(100, 64 * 1024);
    QString err;
    const auto v = bplist::parse(plist, &err);
    ASSERT_TRUE(v.has_value()) << err.toStdString();
    const QVariantList l = v->toList();
    ASSERT_EQ(l.size(), 100);
    const QByteArray first = l.first().toByteArray();
    EXPECT_EQ(first.size(), 64 * 1024);
    for (const QVariant& e : l)
        EXPECT_EQ(e.toByteArray().constData(), first.constData()); // implicit sharing, no copy
}

TEST(BinaryPlist, ReviewOutputBudgetRejectsReferenceAmplification) {
    // 800 KB input whose logical expansion is 200k x 4 KiB (~800 MB): must fail, quickly.
    const QByteArray plist = sharedDataPlist(200'000, 4096);
    QElapsedTimer t;
    t.start();
    QString err;
    EXPECT_FALSE(bplist::parse(plist, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_LT(t.elapsed(), 5000); // generous: the old code took seconds and ~900 MB
}
