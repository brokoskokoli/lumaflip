// SPDX-License-Identifier: GPL-3.0-or-later
// Interpretation of Finder view settings -> SortSpec (SPEC 6.1, F-SORT-03, F-SORT-05,
// F-SORT-08). Synthetic .DS_Store fixtures only.

#include "SortingTestHelpers.h"
#include "core/FinderViewSettings.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

using namespace lf;
using namespace lf::finder;
using test::blobRecord;

namespace {

const char* const kColumns[] = {"name", "dateModified", "dateCreated", "dateAdded", "size",
                                "kind", "label",        "version",     "comments",  "dateLastOpened"};

// lsvC style: columns array
QVariantMap lsvC(const QString& sortColumn, bool ascending) {
    QVariantList cols;
    for (const char* c : kColumns) {
        const bool asc = QString::fromLatin1(c) == sortColumn ? ascending : !ascending;
        cols.append(QVariantMap{{"identifier", c}, {"ascending", asc}, {"visible", true}, {"width", 100}});
    }
    return {{"sortColumn", sortColumn}, {"columns", cols}, {"textSize", 13.0}, {"viewOptionsVersion", 1}};
}

// lsvp style: columns dict
QVariantMap lsvp(const QString& sortColumn, bool ascending) {
    QVariantMap cols;
    int i = 0;
    for (const char* c : kColumns) {
        const bool asc = QString::fromLatin1(c) == sortColumn ? ascending : !ascending;
        cols.insert(c, QVariantMap{{"ascending", asc}, {"index", i++}, {"visible", true}});
    }
    return {{"sortColumn", sortColumn}, {"columns", cols}};
}

QVariantMap icvp(const QString& arrangeBy) {
    return {{"arrangeBy", arrangeBy}, {"iconSize", 64.0}};
}

FolderRecords recordsFor(const QVector<DsStoreRecord>& recs, const QString& name = "Photos") {
    const auto parsed = dsstore::parse(test::writeDsStore(recs));
    EXPECT_TRUE(parsed.has_value());
    return extractFolderRecords(parsed.value_or(QVector<DsStoreRecord>{}), name);
}

DsStoreRecord vstl(const char* style, const QString& name = "Photos") {
    return {name, "vstl", "type", QString::fromLatin1(style)};
}

QVariantMap globalPrefs(const QString& style, const QString& listColumn = "name", bool listAsc = true,
                        const QString& iconArrange = "none", const QString& galleryArrange = "name") {
    QVariantMap standard{
        {"ExtendedListViewSettingsV2", lsvC(listColumn, listAsc)},
        {"IconViewSettings", QVariantMap{{"arrangeBy", iconArrange}}},
        {"GalleryViewSettings", QVariantMap{{"arrangeBy", galleryArrange}}},
        {"SettingsType", "StandardViewSettings"},
    };
    return {{"FXPreferredViewStyle", style}, {"StandardViewSettings", standard}};
}

struct ColumnCase {
    const char* id;
    SortKey key;
};

} // namespace

TEST(FinderViewSettings, F_SORT_03_ListViewEveryColumnBothDirections_lsvC) {
    const ColumnCase cases[] = {{"name", SortKey::Name},
                                {"dateModified", SortKey::DateModified},
                                {"dateCreated", SortKey::DateCreated},
                                {"dateAdded", SortKey::DateAdded},
                                {"size", SortKey::Size},
                                {"kind", SortKey::Kind},
                                {"dateLastOpened", SortKey::DateModified}};
    for (const auto& c : cases) {
        for (bool asc : {true, false}) {
            const auto rec = recordsFor({vstl("Nlsv"), blobRecord("Photos", "lsvC", lsvC(c.id, asc))});
            const SortSpec s = resolve(rec, {});
            EXPECT_EQ(s.key, c.key) << c.id;
            EXPECT_EQ(s.ascending, asc) << c.id;
            EXPECT_EQ(s.source, SortSource::FinderDsStore) << c.id;
        }
    }
}

TEST(FinderViewSettings, F_SORT_03_ListViewEveryColumnBothDirections_lsvp) {
    const ColumnCase cases[] = {{"name", SortKey::Name},
                                {"dateModified", SortKey::DateModified},
                                {"dateCreated", SortKey::DateCreated},
                                {"dateAdded", SortKey::DateAdded},
                                {"size", SortKey::Size},
                                {"kind", SortKey::Kind}};
    for (const auto& c : cases) {
        for (bool asc : {true, false}) {
            const auto rec = recordsFor({vstl("Nlsv"), blobRecord("Photos", "lsvp", lsvp(c.id, asc))});
            const SortSpec s = resolve(rec, {});
            EXPECT_EQ(s.key, c.key) << c.id;
            EXPECT_EQ(s.ascending, asc) << c.id;
            EXPECT_EQ(s.source, SortSource::FinderDsStore);
        }
    }
}

TEST(FinderViewSettings, F_SORT_03_UnsupportedColumnsFallBackToName) {
    for (const char* col : {"label", "version", "comments"}) {
        const auto rec = recordsFor({vstl("Nlsv"), blobRecord("Photos", "lsvC", lsvC(col, false))});
        const SortSpec s = resolve(rec, {});
        EXPECT_EQ(s.key, SortKey::Name) << col;
        EXPECT_TRUE(s.ascending) << col;
    }
}

TEST(FinderViewSettings, F_SORT_03_lsvCPreferredOverLsvp) {
    const auto rec = recordsFor({vstl("Nlsv"), blobRecord("Photos", "lsvp", lsvp("size", true)),
                                 blobRecord("Photos", "lsvC", lsvC("dateAdded", false))});
    const SortSpec s = resolve(rec, {});
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_FALSE(s.ascending);
}

TEST(FinderViewSettings, F_SORT_03_IconViewArrangeBy) {
    struct Case {
        const char* arrange;
        SortKey key;
        bool asc;
    } cases[] = {
        {"name", SortKey::Name, true},
        {"kind", SortKey::Kind, true},
        {"dateModified", SortKey::DateModified, false},
        {"dateCreated", SortKey::DateCreated, false},
        {"dateAdded", SortKey::DateAdded, false},
        {"dateLastOpened", SortKey::DateModified, false},
        {"size", SortKey::Size, false},
        {"label", SortKey::Name, true},
        {"none", SortKey::FinderIconPosition, true},
        {"grid", SortKey::FinderIconPosition, true},
    };
    for (const auto& c : cases) {
        const auto rec = recordsFor({vstl("icnv"), blobRecord("Photos", "icvp", icvp(c.arrange))});
        const SortSpec s = resolve(rec, {});
        EXPECT_EQ(s.key, c.key) << c.arrange;
        EXPECT_EQ(s.ascending, c.asc) << c.arrange;
        EXPECT_EQ(s.source, SortSource::FinderDsStore) << c.arrange;
    }
}

TEST(FinderViewSettings, F_SORT_03_NoVstlUsesPreferredViewStyle) {
    // Folder has both icon and list settings but no vstl: the global preferred style picks.
    const auto rec = recordsFor(
        {blobRecord("Photos", "icvp", icvp("dateAdded")), blobRecord("Photos", "lsvC", lsvC("size", true))});
    SortSpec s = resolve(rec, {globalPrefs("Nlsv"), {}});
    EXPECT_EQ(s.key, SortKey::Size);
    EXPECT_TRUE(s.ascending);
    s = resolve(rec, {globalPrefs("icnv"), {}});
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_EQ(s.source, SortSource::FinderDsStore);
}

TEST(FinderViewSettings, F_SORT_03_MissingRecordUsesGlobalDefaults) {
    const FolderRecords none;
    SortSpec s = resolve(none, {globalPrefs("Nlsv", "dateModified", false), {}});
    EXPECT_EQ(s.key, SortKey::DateModified);
    EXPECT_FALSE(s.ascending);
    EXPECT_EQ(s.source, SortSource::FinderGlobal);

    s = resolve(none, {globalPrefs("icnv", "name", true, "kind"), {}});
    EXPECT_EQ(s.key, SortKey::Kind);
    EXPECT_EQ(s.source, SortSource::FinderGlobal);

    s = resolve(none, {globalPrefs("glyv", "name", true, "none", "dateCreated"), {}});
    EXPECT_EQ(s.key, SortKey::DateCreated);
    EXPECT_FALSE(s.ascending);

    s = resolve(none, {globalPrefs("glyv", "name", true, "none", "none"), {}});
    EXPECT_EQ(s.key, SortKey::Name); // gallery without arrangement shows names in order

    // Old-style global ListViewSettings (columns dict) only.
    QVariantMap old{{"FXPreferredViewStyle", "Nlsv"},
                    {"StandardViewSettings", QVariantMap{{"ListViewSettings", lsvp("kind", false)}}}};
    s = resolve(none, {old, {}});
    EXPECT_EQ(s.key, SortKey::Kind);
    EXPECT_FALSE(s.ascending);
}

TEST(FinderViewSettings, F_SORT_03_ListViewWithoutFolderListSettingsUsesGlobalList) {
    const auto rec = recordsFor({vstl("Nlsv"), blobRecord("Photos", "icvp", icvp("name"))});
    const SortSpec s = resolve(rec, {globalPrefs("icnv", "dateAdded", false), {}});
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_FALSE(s.ascending);
    EXPECT_EQ(s.source, SortSource::FinderGlobal);
}

TEST(FinderViewSettings, F_SORT_03_ColumnViewUsesGlobalArrangeBy) {
    const auto rec = recordsFor({vstl("clmv")});
    QVariantMap prefs = globalPrefs("Nlsv");
    prefs.insert("StandardViewOptions",
                 QVariantMap{{"ColumnViewOptions", QVariantMap{{"ArrangeBy", "modd"}}}});
    SortSpec s = resolve(rec, {prefs, {}});
    EXPECT_EQ(s.key, SortKey::DateModified);
    EXPECT_EQ(s.source, SortSource::FinderGlobal);
    prefs.insert("StandardViewOptions",
                 QVariantMap{{"ColumnViewOptions", QVariantMap{{"ArrangeBy", "dnam"}}}});
    s = resolve(rec, {prefs, {}});
    EXPECT_EQ(s.key, SortKey::Name);
    EXPECT_TRUE(s.ascending);
}

TEST(FinderViewSettings, F_SORT_03_DesktopUsesDesktopViewSettings) {
    QVariantMap prefs = globalPrefs("Nlsv", "size", true);
    prefs.insert("DesktopViewSettings",
                 QVariantMap{{"IconViewSettings", QVariantMap{{"arrangeBy", "dateAdded"}}}});
    SortSpec s = resolve(FolderRecords{}, {prefs, "DesktopViewSettings"});
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_FALSE(s.ascending);
    EXPECT_EQ(s.source, SortSource::FinderGlobal);
}

TEST(FinderViewSettings, F_SORT_03_ICloudSettingsBeforeStandard) {
    QVariantMap prefs = globalPrefs("Nlsv", "size", true);
    prefs.insert("ICloudViewSettings",
                 QVariantMap{{"ExtendedListViewSettingsV2", lsvC("dateModified", true)}});
    SortSpec s = resolve(FolderRecords{}, {prefs, "ICloudViewSettings"});
    EXPECT_EQ(s.key, SortKey::DateModified);
    EXPECT_TRUE(s.ascending);
}

TEST(FinderViewSettings, F_SORT_08_GarbageFallsBackToNextTier) {
    // Corrupt blob (not a plist) -> ignored -> global; nothing global -> fallback.
    QVector<DsStoreRecord> recs = {vstl("Nlsv"),
                                   {"Photos", "lsvC", "blob", QByteArray("bplist00garbage")},
                                   {"Photos", "icvp", "blob", QByteArray("\x00\x01", 2)}};
    const auto rec = recordsFor(recs);
    SortSpec s = resolve(rec, {globalPrefs("Nlsv", "kind", false), {}});
    EXPECT_EQ(s.key, SortKey::Kind);
    EXPECT_EQ(s.source, SortSource::FinderGlobal);
    s = resolve(rec, {});
    EXPECT_EQ(s.key, SortKey::Name);
    EXPECT_TRUE(s.ascending);
    EXPECT_EQ(s.source, SortSource::Fallback);
    // Unknown sortColumn and unknown view style.
    const auto weird = recordsFor({vstl("XXXX"), blobRecord("Photos", "lsvC", lsvC("mystery", true))});
    s = resolve(weird, {});
    EXPECT_EQ(s.source, SortSource::Fallback);
    // Type confusion inside the plist must not matter.
    QVariantMap confused{{"sortColumn", QVariantList{1, 2}}, {"columns", "nope"}};
    const auto conf = recordsFor({vstl("Nlsv"), blobRecord("Photos", "lsvC", confused)});
    s = resolve(conf, {});
    EXPECT_EQ(s.source, SortSource::Fallback);
}

TEST(FinderViewSettings, F_SORT_03_ReadsParentThenOwnDsStore) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString folder = tmp.filePath("Photos");
    ASSERT_TRUE(QDir().mkpath(folder));

    // Only the folder's own "." record.
    ASSERT_TRUE(test::writeFile(
        folder + "/.DS_Store",
        test::writeDsStore({vstl("Nlsv", "."), blobRecord(".", "lsvC", lsvC("size", false))})));
    SortSpec s = resolve(readFolderRecords(folder), {});
    EXPECT_EQ(s.key, SortKey::Size);
    EXPECT_EQ(s.source, SortSource::FinderDsStore);

    // Parent record wins.
    ASSERT_TRUE(test::writeFile(
        tmp.filePath(".DS_Store"),
        test::writeDsStore({vstl("Nlsv"), blobRecord("Photos", "lsvC", lsvC("dateAdded", true)),
                            blobRecord("Other", "lsvC", lsvC("kind", true))})));
    s = resolve(readFolderRecords(folder), {});
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_TRUE(s.ascending);

    // Corrupt parent file -> own record still used.
    ASSERT_TRUE(test::writeFile(tmp.filePath(".DS_Store"), QByteArray("\x00\x00\x00\x01"
                                                                      "Bud1 garbage",
                                                                      16)));
    s = resolve(readFolderRecords(folder), {});
    EXPECT_EQ(s.key, SortKey::Size);
}

TEST(FinderViewSettings, F_SORT_03_IconPositionsFromIloc) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(test::writeFile(tmp.filePath(".DS_Store"),
                                test::writeDsStore({test::ilocRecord("a.jpg", 100, 50),
                                                    test::ilocRecord("b.jpg", -20, 300),
                                                    {"c.jpg", "Iloc", "blob", QByteArray("\x01", 1)}})));
    const auto pos = readIconPositions(tmp.path());
    ASSERT_EQ(pos.size(), 2);
    EXPECT_EQ(pos.value("a.jpg"), QPointF(100, 50));
    EXPECT_EQ(pos.value("b.jpg"), QPointF(-20, 300));
}

// Review finding: Finder writes record names as on disk (decomposed, NFD) while Qt hands
// out NFC names, so settings of "Müll Fotos" and icon positions of "Bäume.jpg" were missed.
TEST(FinderViewSettings, F_SORT_08_NonAsciiNfdRecordNames) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString nfdFolder = QString::fromUtf8("Mu\xcc\x88ll Fotos");
    const QString nfcFolder = QString::fromUtf8("Müll Fotos");
    ASSERT_NE(nfdFolder, nfcFolder);
    const QString folder = tmp.filePath(nfcFolder);
    ASSERT_TRUE(QDir().mkpath(folder));
    ASSERT_TRUE(test::writeFile(tmp.filePath(".DS_Store"),
                                test::writeDsStore({vstl("Nlsv", nfdFolder),
                                                    blobRecord(nfdFolder, "lsvC", lsvC("dateAdded", false))})));
    const SortSpec s = resolve(readFolderRecords(folder), {});
    EXPECT_EQ(s.key, SortKey::DateAdded);
    EXPECT_FALSE(s.ascending);
    EXPECT_EQ(s.source, SortSource::FinderDsStore);

    // Either spelling of the folder name finds the record.
    EXPECT_TRUE(recordsFor({vstl("Nlsv", nfdFolder)}, nfcFolder).found);
    EXPECT_TRUE(recordsFor({vstl("Nlsv", nfdFolder)}, nfdFolder).found);

    ASSERT_TRUE(test::writeFile(folder + "/.DS_Store",
                                test::writeDsStore({test::ilocRecord(QString::fromUtf8("Ba\xcc\x88ume.jpg"), 7, 9),
                                                    test::ilocRecord("plain.jpg", 1, 2)})));
    const auto pos = readIconPositions(folder);
    ASSERT_EQ(pos.size(), 2);
    EXPECT_EQ(pos.value(QString::fromUtf8("Bäume.jpg")), QPointF(7, 9));
}

// Covers: F-SORT-04 (output parsing; the osascript call itself is a manual check)
TEST(FinderViewSettings, F_SORT_04_ParseLiveQueryOutput) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    auto s = parseLiveQueryOutput(dir + "/\nlist\nmodification date column\nnormal\n", dir);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->key, SortKey::DateModified);
    EXPECT_FALSE(s->ascending);
    EXPECT_EQ(s->source, SortSource::FinderLive);
    s = parseLiveQueryOutput(dir + "/\nlist\nname column\nreversed\n", dir);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->key, SortKey::Name);
    EXPECT_FALSE(s->ascending);
    s = parseLiveQueryOutput(dir + "\nicon\narranged by date added\n", dir);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->key, SortKey::DateAdded);
    s = parseLiveQueryOutput(dir + "\nicon\nnot arranged\n", dir);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->key, SortKey::FinderIconPosition);
    // Different folder in the front window -> not used.
    EXPECT_FALSE(parseLiveQueryOutput("/some/other/folder/\nlist\nname column\nnormal", dir).has_value());
    EXPECT_FALSE(parseLiveQueryOutput("", dir).has_value());
    EXPECT_FALSE(parseLiveQueryOutput(dir + "\nother\n", dir).has_value());
    EXPECT_FALSE(parseLiveQueryOutput(dir + "\nlist\nbogus column\nnormal", dir).has_value());
}
