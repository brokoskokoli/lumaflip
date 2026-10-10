// SPDX-License-Identifier: GPL-3.0-or-later
// Real Finder .DS_Store fixtures (SPEC F-SORT-08, F-SORT-03).
//
// tools/make_finder_fixtures.sh lets Finder itself write .DS_Store files for neutral test
// folders and stores them per macOS major version:
//   tests/data/finder/macos-<major>/<case>/parent.DS_Store  (<case>/.DS_Store: record "Bilder")
//                                          folder.DS_Store  (<case>/Bilder/.DS_Store: ".", Iloc)
//                                          expected.json    (expected SortSpec + order + metadata)
// For every fixture present, the .DS_Store files are read like the app reads a folder
// (parent record merged with the folder's own record, no global Finder settings) and the
// result must be the expected SortSpec from tier 1; sorting the recorded file metadata
// with that spec must give the expected order.
// Without fixtures the test is skipped; LF_REQUIRE_FINDER_FIXTURES=1 makes that a
// failure. LF_FINDER_FIXTURES_DIR points the test at another directory (e.g. freshly
// generated fixtures before committing them).

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/FinderViewSettings.h"
#include "core/Platform.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdio>

using namespace lf;
using namespace lf::finder;

namespace {

QString fixtureRoot() {
    const QString env = qEnvironmentVariable("LF_FINDER_FIXTURES_DIR");
    return env.isEmpty() ? QStringLiteral(LF_TEST_DATA_DIR "/finder") : env;
}

// Every directory below `root` (depth 1 or 2) that holds an expected.json.
QStringList fixtureDirs(const QString& root) {
    QStringList dirs;
    QDirIterator it(root, {QStringLiteral("expected.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = QFileInfo(it.next()).absolutePath();
        const int depth = QDir(root).relativeFilePath(dir).count(QLatin1Char('/')) + 1;
        if (depth <= 2)
            dirs.append(dir);
    }
    dirs.sort();
    return dirs;
}

std::optional<SortKey> keyFromName(const QString& n) {
    static const QHash<QString, SortKey> keys = {
        {QStringLiteral("Name"), SortKey::Name},
        {QStringLiteral("DateModified"), SortKey::DateModified},
        {QStringLiteral("DateCreated"), SortKey::DateCreated},
        {QStringLiteral("DateAdded"), SortKey::DateAdded},
        {QStringLiteral("Size"), SortKey::Size},
        {QStringLiteral("Kind"), SortKey::Kind},
        {QStringLiteral("FinderIconPosition"), SortKey::FinderIconPosition},
    };
    if (const auto it = keys.find(n); it != keys.end())
        return *it;
    return std::nullopt;
}

QString keyName(SortKey key) {
    switch (key) {
    case SortKey::Name:
        return QStringLiteral("Name");
    case SortKey::DateModified:
        return QStringLiteral("DateModified");
    case SortKey::DateCreated:
        return QStringLiteral("DateCreated");
    case SortKey::DateAdded:
        return QStringLiteral("DateAdded");
    case SortKey::Size:
        return QStringLiteral("Size");
    case SortKey::Kind:
        return QStringLiteral("Kind");
    case SortKey::FinderIconPosition:
        return QStringLiteral("FinderIconPosition");
    default:
        return QStringLiteral("other(%1)").arg(int(key));
    }
}

QString describeSpec(const SortSpec& s) {
    return keyName(s.key) + (s.ascending ? QStringLiteral(" asc") : QStringLiteral(" desc")) +
           (s.source == SortSource::FinderDsStore ? QStringLiteral(" (.DS_Store)")
                                                  : QStringLiteral(" (not tier 1)"));
}

// Reads `file` (optional) and extracts the records of `name`; adds a failure if the file
// exists but cannot be parsed.
FolderRecords recordsFrom(const QString& file, const QString& name, QStringList& failures,
                          QVector<DsStoreRecord>* all = nullptr) {
    if (!QFileInfo::exists(file))
        return {};
    QString error;
    const auto recs = dsstore::readFile(file, &error);
    if (!recs) {
        failures.append(QStringLiteral("%1: not parsed: %2").arg(QFileInfo(file).fileName(), error));
        return {};
    }
    if (all)
        *all = *recs;
    return extractFolderRecords(*recs, name);
}

// Checks one fixture directory; returns the failures (empty = OK).
QStringList checkFixture(const QString& dir, const NameCollator* collator) {
    QStringList failures;
    QFile jf(dir + QStringLiteral("/expected.json"));
    if (!jf.open(QIODevice::ReadOnly))
        return {QStringLiteral("expected.json not readable")};
    QJsonParseError perr{};
    const QJsonObject obj = QJsonDocument::fromJson(jf.readAll(), &perr).object();
    if (perr.error != QJsonParseError::NoError)
        return {QStringLiteral("expected.json: ") + perr.errorString()};
    const QJsonObject exp = obj.value(QLatin1String("expected")).toObject();
    const auto key = keyFromName(exp.value(QLatin1String("key")).toString());
    if (!key || !exp.value(QLatin1String("ascending")).isBool())
        return {QStringLiteral("expected.json: missing/unknown expected.key or expected.ascending")};
    const SortSpec want{*key, exp.value(QLatin1String("ascending")).toBool(), SortSource::FinderDsStore};

    // Same lookup as finder::readFolderRecords(): <parent>/.DS_Store record <folder>,
    // merged with <folder>/.DS_Store record ".".
    const QString folder = obj.value(QLatin1String("folder")).toString(QStringLiteral("Bilder"));
    const QString parentFile = dir + QStringLiteral("/parent.DS_Store");
    const QString folderFile = dir + QStringLiteral("/folder.DS_Store");
    if (!QFileInfo::exists(parentFile) && !QFileInfo::exists(folderFile))
        return {QStringLiteral("neither parent.DS_Store nor folder.DS_Store present")};
    QVector<DsStoreRecord> folderRecords;
    const FolderRecords fromParent = recordsFrom(parentFile, folder, failures);
    const FolderRecords own = recordsFrom(folderFile, QStringLiteral("."), failures, &folderRecords);
    const SortSpec got = resolve(mergeRecords(fromParent, own), GlobalContext{});
    if (got != want) {
        failures.append(QStringLiteral("SortSpec %1, expected %2 (Finder: %3)")
                            .arg(describeSpec(got), describeSpec(want),
                                 obj.value(QLatin1String("finderSetting")).toString()));
        return failures;
    }
    if (!collator)
        return failures;

    // Order: sort the recorded metadata with the spec and compare.
    const QHash<QString, QPointF> positions = iconPositions(folderRecords);
    QVector<FileEntry> entries;
    for (const QJsonValue& v : obj.value(QLatin1String("files")).toArray()) {
        const QJsonObject f = v.toObject();
        FileEntry e;
        e.name = f.value(QLatin1String("name")).toString();
        e.path = QStringLiteral("/fixture/") + e.name;
        e.size = qint64(f.value(QLatin1String("size")).toDouble(-1));
        e.modified = QDateTime::fromString(f.value(QLatin1String("modified")).toString(), Qt::ISODateWithMs);
        e.created = QDateTime::fromString(f.value(QLatin1String("created")).toString(), Qt::ISODateWithMs);
        e.added = QDateTime::fromString(f.value(QLatin1String("added")).toString(), Qt::ISODateWithMs);
        e.kind = f.value(QLatin1String("kind")).toString();
        if (const auto it = positions.find(e.name); it != positions.end())
            e.finderIconPosition = *it;
        entries.append(e);
    }
    collection::sortEntries(entries, got, *collator);
    QStringList order, expected;
    for (const FileEntry& e : std::as_const(entries))
        order.append(e.name);
    for (const QJsonValue& v : obj.value(QLatin1String("order")).toArray())
        expected.append(v.toString());
    if (order != expected)
        failures.append(QStringLiteral("order\n  got      %1\n  expected %2 (%3)")
                            .arg(order.join(QLatin1String(" | ")), expected.join(QLatin1String(" | ")),
                                 obj.value(QLatin1String("orderSource")).toString()));
    return failures;
}

const NameCollator* finderCollator() {
#ifdef LF_GENERIC_PLATFORM
    // The portable name comparison is not Finder's: only the SortSpec is checked there.
    return nullptr;
#else
    static const std::unique_ptr<NameCollator> collator = createNameCollator();
    return collator.get();
#endif
}

void writeJson(const QString& path, const QJsonObject& obj) {
    ASSERT_TRUE(test::writeFile(path, QJsonDocument(obj).toJson()));
}

QJsonObject fileJson(const char* name, qint64 size, const char* modified) {
    return {{"name", name},        {"size", double(size)}, {"modified", modified},
            {"created", modified}, {"added", modified},    {"kind", "JPEG image"}};
}

} // namespace

TEST(FinderFixtures, F_SORT_08_F_SORT_03_RealFinderDsStoreFiles) {
    const QString root = fixtureRoot();
    const QStringList dirs = fixtureDirs(root);
    if (dirs.isEmpty()) {
        if (qEnvironmentVariableIntValue("LF_REQUIRE_FINDER_FIXTURES") == 1)
            FAIL() << "no Finder fixtures in " << root.toStdString()
                   << " (LF_REQUIRE_FINDER_FIXTURES=1) - create them with tools/make_finder_fixtures.sh";
        GTEST_SKIP() << "no Finder fixtures in " << root.toStdString()
                     << " - create them with tools/make_finder_fixtures.sh (docs/TESTING-M1.md, section 4)";
    }
    for (const QString& dir : dirs) {
        const QString name = QDir(root).relativeFilePath(dir);
        for (const QString& f : checkFixture(dir, finderCollator()))
            ADD_FAILURE() << name.toStdString() << ": " << f.toStdString();
    }
    std::printf("[          ] %lld Finder fixture(s) checked in %s\n", static_cast<long long>(dirs.size()),
                qPrintable(root));
}

// The checker itself, on synthetic fixtures in the same layout (so a broken checker
// cannot silently accept real fixtures).
TEST(FinderFixtures, F_SORT_08_CheckerOnSyntheticFixtures) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString list = tmp.filePath(QStringLiteral("macos-0/list-modified-desc"));
    const QString icon = tmp.filePath(QStringLiteral("macos-0/icon-none"));
    ASSERT_TRUE(QDir().mkpath(list) && QDir().mkpath(icon));

    const QVariantMap lsvC{
        {"sortColumn", "dateModified"},
        {"columns", QVariantList{QVariantMap{{"identifier", "dateModified"}, {"ascending", false}},
                                 QVariantMap{{"identifier", "name"}, {"ascending", true}}}}};
    ASSERT_TRUE(test::writeFile(list + "/parent.DS_Store",
                                test::writeDsStore({{"Bilder", "vstl", "type", QStringLiteral("Nlsv")},
                                                    test::blobRecord("Bilder", "lsvC", lsvC)})));
    const QJsonArray files{fileJson("IMG_2.jpg", 10, "2024-01-01T10:00:00.000Z"),
                           fileJson("IMG_10.jpg", 20, "2024-03-01T10:00:00.000Z"),
                           fileJson("a b.jpg", 30, "2024-02-01T10:00:00.000Z")};
    QJsonObject listJson{{"folder", "Bilder"},
                         {"expected", QJsonObject{{"key", "DateModified"}, {"ascending", false}}},
                         {"order", QJsonArray{"IMG_10.jpg", "a b.jpg", "IMG_2.jpg"}},
                         {"files", files}};
    writeJson(list + "/expected.json", listJson);

    ASSERT_TRUE(test::writeFile(
        icon + "/parent.DS_Store",
        test::writeDsStore({{"Bilder", "vstl", "type", QStringLiteral("icnv")},
                            test::blobRecord("Bilder", "icvp", QVariantMap{{"arrangeBy", "none"}})})));
    ASSERT_TRUE(test::writeFile(
        icon + "/folder.DS_Store",
        test::writeDsStore({test::ilocRecord("IMG_10.jpg", 80, 220), test::ilocRecord("IMG_2.jpg", 240, 60),
                            test::ilocRecord("a b.jpg", 80, 60)})));
    writeJson(icon + "/expected.json",
              QJsonObject{{"folder", "Bilder"},
                          {"expected", QJsonObject{{"key", "FinderIconPosition"}, {"ascending", true}}},
                          {"order", QJsonArray{"a b.jpg", "IMG_2.jpg", "IMG_10.jpg"}},
                          {"files", files}});

    EXPECT_EQ(fixtureDirs(tmp.path()), (QStringList{icon, list}));
    EXPECT_EQ(checkFixture(list, finderCollator()), QStringList());
    EXPECT_EQ(checkFixture(icon, finderCollator()), QStringList());

    // Wrong expectations are reported: direction, then order.
    listJson["expected"] = QJsonObject{{"key", "DateModified"}, {"ascending", true}};
    writeJson(list + "/expected.json", listJson);
    EXPECT_EQ(checkFixture(list, finderCollator()).size(), 1);
    listJson["expected"] = QJsonObject{{"key", "DateModified"}, {"ascending", false}};
    listJson["order"] = QJsonArray{"IMG_2.jpg", "IMG_10.jpg", "a b.jpg"};
    writeJson(list + "/expected.json", listJson);
    EXPECT_EQ(checkFixture(list, finderCollator()).size(), finderCollator() ? 1 : 0);

    // Unparsable .DS_Store: reported, not crashing (F-SORT-08).
    ASSERT_TRUE(test::writeFile(list + "/parent.DS_Store", QByteArray("\0\0\0\1Bud1garbage", 15)));
    EXPECT_FALSE(checkFixture(list, finderCollator()).isEmpty());
}
