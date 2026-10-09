// SPDX-License-Identifier: GPL-3.0-or-later
// F-SORT-01: the platform name collator must order exactly like Finder, i.e. like
// -[NSString localizedStandardCompare:]. Also checks the macOS Finder order provider and
// attribute provider end to end with synthetic .DS_Store files.

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/Platform.h"
#include "platform/mac/MacNameCollator.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

#import <Foundation/Foundation.h>

using namespace lf;

namespace {

QStringList collationSet() {
    return {
        "IMG_2 (1).jpg",
        "IMG_2.jpg",
        "IMG_10.jpg",
        "IMG_1.jpg",
        "img_2.JPG",
        "IMG_02.jpg",
        "IMG_002.jpg",
        "001.jpg",
        "1.jpg",
        "01.jpg",
        "10.jpg",
        "9.jpg",
        "1a.jpg",
        "1 a.jpg",
        "a.jpg",
        "A.jpg",
        "b.jpg",
        "B.jpg",
        QString::fromUtf8("äpfel.jpg"),
        QString::fromUtf8("Äpfel.jpg"),
        "apfel.jpg",
        "Apfel.jpg",
        QString::fromUtf8("über.jpg"),
        "uber.jpg",
        "ueber.jpg",
        QString::fromUtf8("Straße.jpg"),
        "Strasse.jpg",
        QString::fromUtf8("ｆｕｌｌ.jpg"),
        "full.jpg",
        QString::fromUtf8("１２.jpg"),
        "12.jpg",
        QString::fromUtf8("😀.jpg"),
        QString::fromUtf8("😀 2.jpg"),
        QString::fromUtf8("🌈.jpg"),
        "_x.jpg",
        "-x.jpg",
        " x.jpg",
        "x-1.jpg",
        "x_1.jpg",
        "x.1.jpg",
        "x 1.jpg",
        QString::fromUtf8("é.jpg"),
        QString::fromUtf8("é.jpg"),
        "e.jpg",
        "f.jpg",
        QString::fromUtf8("日本.jpg"),
        QString::fromUtf8("中文.jpg"),
        QString::fromUtf8("Ωmega.jpg"),
        "photo 2.png",
        "photo 10.png",
        "Photo 3.PNG",
        "image.jpg",
        "image.jpeg",
        "image.JPG",
    };
}

int nsCompare(const QString& a, const QString& b) {
    @autoreleasepool {
        const NSComparisonResult r = [a.toNSString() localizedStandardCompare:b.toNSString()];
        return r == NSOrderedAscending ? -1 : (r == NSOrderedDescending ? 1 : 0);
    }
}

} // namespace

TEST(NameCollationMac, F_SORT_01_MatchesLocalizedStandardCompare) {
    const auto collator = createNameCollator();
    const QStringList set = collationSet();
    for (const QString& a : set) {
        for (const QString& b : set) {
            EXPECT_EQ(collator->compare(a, b), nsCompare(a, b))
                << a.toStdString() << " vs " << b.toStdString();
        }
    }
}

TEST(NameCollationMac, F_SORT_01_FinderNameOrder) {
    const auto collator = createNameCollator();
    auto less = [&](const QString& a, const QString& b) { return collator->compare(a, b) < 0; };
    EXPECT_TRUE(less("IMG_2.jpg", "IMG_10.jpg"));
    EXPECT_TRUE(less("IMG_1.jpg", "IMG_2.jpg"));
    EXPECT_TRUE(less("IMG_2 (1).jpg", "IMG_2.jpg"));
    EXPECT_TRUE(less("9.jpg", "10.jpg"));
    // Forced ordering: equal only for identical strings, but "001" and "1" stay adjacent.
    EXPECT_NE(collator->compare("001.jpg", "1.jpg"), 0);
    EXPECT_NE(collator->compare("a.jpg", "A.jpg"), 0);
    EXPECT_EQ(collator->compare("a.jpg", "a.jpg"), 0);
    EXPECT_TRUE(less("1.jpg", "2.jpg") && less("001.jpg", "2.jpg"));
    // Umlauts sort with their base letter, not after z.
    EXPECT_TRUE(less(QString::fromUtf8("äpfel.jpg"), "b.jpg"));
    // Width-insensitive: fullwidth digits count as numbers.
    EXPECT_TRUE(less(QString::fromUtf8("２.jpg"), "10.jpg"));
}

// Review finding: the collator snapshotted the locale at construction, so a language or
// region change in System Settings never reached a running window. It must re-fetch the
// current locale after NSCurrentLocaleDidChangeNotification (and only then: P-05).
TEST(NameCollationMac, F_SORT_01_FollowsLocaleChanges) {
    const auto collator = createNameCollator();
    (void)collator->compare("a.jpg", "b.jpg");
    const int fetched = mac::nameCollatorLocaleFetches();
    for (int i = 0; i < 1000; ++i)
        (void)collator->compare("a.jpg", "b.jpg");
    EXPECT_EQ(mac::nameCollatorLocaleFetches(), fetched); // cached between changes
    [[NSNotificationCenter defaultCenter] postNotificationName:NSCurrentLocaleDidChangeNotification object:nil];
    EXPECT_LT(collator->compare("a.jpg", "b.jpg"), 0);
    EXPECT_EQ(mac::nameCollatorLocaleFetches(), fetched + 1);
    // Results still match NSString with the (re-fetched) current locale.
    EXPECT_EQ(collator->compare(QString::fromUtf8("äpfel.jpg"), "b.jpg"),
              nsCompare(QString::fromUtf8("äpfel.jpg"), "b.jpg"));
}

TEST(NameCollationMac, F_SORT_01_StrictWeakOrderingOnSet) {
    const auto collator = createNameCollator();
    const QStringList set = collationSet();
    for (const QString& a : set)
        for (const QString& b : set) {
            EXPECT_EQ(collator->compare(a, b), -collator->compare(b, a));
            if (a != b)
                EXPECT_NE(collator->compare(a, b), 0) << a.toStdString() << " / " << b.toStdString();
            for (const QString& c : set) {
                if (collator->compare(a, b) < 0 && collator->compare(b, c) < 0)
                    EXPECT_LT(collator->compare(a, c), 0);
            }
        }
}

TEST(MacFolderOrder, F_SORT_03_ReadsSyntheticDsStoreOfParent) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString folder = QDir::cleanPath(tmp.path()) + "/Bilder";
    ASSERT_TRUE(QDir().mkpath(folder));
    QVariantList cols{QVariantMap{{"identifier", "dateAdded"}, {"ascending", false}}};
    ASSERT_TRUE(test::writeFile(
        tmp.filePath(".DS_Store"),
        test::writeDsStore({{"Bilder", "vstl", "type", QStringLiteral("Nlsv")},
                            test::blobRecord("Bilder", "lsvC",
                                             QVariantMap{{"sortColumn", "dateAdded"}, {"columns", cols}})})));
    const auto provider = createFolderOrderProvider();
    const SortSpec s = provider->finderOrder(folder, false);
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_FALSE(s.ascending);
    EXPECT_EQ(s.source, SortSource::FinderDsStore);
    EXPECT_EQ(describe(s), QString::fromUtf8("Wie Finder: Hinzugefügt am ↓"));
}

TEST(MacFolderOrder, F_SORT_03_NoDsStoreGivesGlobalOrFallback) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto provider = createFolderOrderProvider();
    const SortSpec s = provider->finderOrder(tmp.path(), false);
    // Depends on the user's Finder defaults, but never tier 1 and never live.
    EXPECT_TRUE(s.source == SortSource::FinderGlobal || s.source == SortSource::Fallback);
}

TEST(MacFileAttributes, F_SORT_02_FillsAddedDateAndKind) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(test::writeFile(tmp.filePath("a.jpg"), "x"));
    bool denied = false;
    auto entries = collection::enumerateFolder(tmp.path(), &denied);
    ASSERT_EQ(entries.size(), 1);
    const auto provider = createFileAttributeProvider();
    provider->fill(entries[0]);
    EXPECT_TRUE(entries[0].created.isValid());
    EXPECT_TRUE(entries[0].added.isValid());
    EXPECT_FALSE(entries[0].kind.isEmpty());
    EXPECT_LE(qAbs(entries[0].added.secsTo(QDateTime::currentDateTimeUtc())), 120);
}
