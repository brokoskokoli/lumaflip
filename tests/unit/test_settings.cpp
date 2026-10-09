// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/InputBindings.h"
#include "core/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace lf;

namespace {

void ensureSettingsTestApp() {
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "lf_unit_tests";
    static char* argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);  // lives for the whole test process
}

QString freshIniPath() {
    static QTemporaryDir dir;  // removed at process exit
    static int counter = 0;
    return dir.filePath(QStringLiteral("settings-%1.ini").arg(++counter));
}

// Records Settings::changed (QSignalSpy would need QtTest, unit tests link only lf_core).
struct ChangeRecorder {
    explicit ChangeRecorder(Settings* st) {
        conn = QObject::connect(st, &Settings::changed, [this](const QString& k) { keys << k; });
    }
    ~ChangeRecorder() { QObject::disconnect(conn); }
    qsizetype count() const { return keys.size(); }
    QStringList keys;
    QMetaObject::Connection conn;
};

class SettingsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureSettingsTestApp();
        ini = freshIniPath();
        Settings::useTestingBackend(ini);
    }
    Settings& s() { return Settings::instance(); }
    QString ini;
};

} // namespace

// F-SET-04
TEST_F(SettingsTest, F_SET_04_SensibleDefaults) {
    EXPECT_EQ(s().value(key::OpenBehavior).toString(), "newWindow");
    EXPECT_EQ(s().value(key::WrapMode).toString(), "stop");
    EXPECT_TRUE(s().value(key::FollowFinder).toBool());
    EXPECT_FALSE(s().value(key::FinderLiveQuery).toBool());
    EXPECT_EQ(s().value(key::Background).toString(), "dark");
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "fit");
    EXPECT_EQ(s().value(key::UpscaleFilter).toString(), "smooth");
    EXPECT_EQ(s().value(key::ActualSizeUnit).toString(), "devicePixels");
    EXPECT_TRUE(s().value(key::RatingStoreSidecar).toBool());
    EXPECT_FALSE(s().value(key::RatingStoreEmbedded).toBool());
    EXPECT_FALSE(s().value(key::RatingStoreFinderTags).toBool());
    EXPECT_EQ(s().value(key::PrefetchAhead).toInt(), 3);
    EXPECT_EQ(s().value(key::PrefetchBehind).toInt(), 1);
    EXPECT_EQ(s().value(key::MemoryBudgetMB).toInt(), 0);
    EXPECT_TRUE(s().value(key::RestoreWindows).toBool());
    EXPECT_FALSE(s().value(key::FitWindowToImage).toBool());
    EXPECT_TRUE(s().value(key::ShowStatusBar).toBool());
    EXPECT_FALSE(s().value(key::ShowInfoPanel).toBool());
    EXPECT_FALSE(s().value(key::ShowFilmstrip).toBool());
    EXPECT_FALSE(s().value(key::RawEmbeddedOnly).toBool());
    EXPECT_EQ(s().value(key::ManualSortKey).toString(), "name");
    EXPECT_TRUE(s().value(key::ManualSortAscending).toBool());
    // F-IN-03 tuning defaults
    EXPECT_FALSE(s().value(key::WheelInverted).toBool());
    EXPECT_EQ(s().value(key::WheelStepsPerImage).toInt(), 1);
    EXPECT_EQ(s().value(key::WheelMinIntervalMs).toInt(), 120);
    EXPECT_EQ(s().value(key::ZoomStepPercent).toInt(), 15);
    EXPECT_EQ(s().value(key::JumpSize).toInt(), 10);
    // Input slot defaults come from InputBindings
    for (const auto& slot : InputBindings::inputSlots()) {
        if (slot.id == "endOfCollection")
            continue;
        EXPECT_EQ(s().value(QString(key::InputPrefix) + slot.id).toString(), slot.defaultChoice) << slot.id.toStdString();
    }
    EXPECT_EQ(s().value(QString(key::ShortcutPrefix) + "zoomActual").toStringList(), QStringList({"Ctrl+0", "3"}));
}

// F-SET-04
TEST_F(SettingsTest, F_SET_04_EveryKnownKeyHasDefault) {
    const QStringList keys = Settings::knownKeys();
    EXPECT_GT(keys.size(), 50);
    for (const auto& k : keys) {
        EXPECT_TRUE(s().defaultValue(k).isValid()) << k.toStdString();
        EXPECT_EQ(s().value(k), s().defaultValue(k)) << k.toStdString();
    }
}

TEST_F(SettingsTest, SetGetAndChangedSignal) {
    ChangeRecorder spy(&s());
    s().setValue(key::ZoomMode, "actual");
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "actual");
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.keys.at(0), QString(key::ZoomMode));
    // same value again: no signal
    s().setValue(key::ZoomMode, "actual");
    EXPECT_EQ(spy.count(), 1);
    // invalid values are rejected
    s().setValue(key::ZoomMode, "bogus");
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "actual");
    s().setValue(key::PrefetchAhead, 999);
    EXPECT_EQ(s().value(key::PrefetchAhead).toInt(), 3);
    EXPECT_EQ(spy.count(), 1);
    // custom background colour is allowed ("#rrggbb")
    s().setValue(key::Background, "#336699");
    EXPECT_EQ(s().value(key::Background).toString(), "#336699");
    // keys of other modules pass through
    s().setValue("window/someGeometry", QByteArray("abc"));
    EXPECT_EQ(s().value("window/someGeometry").toByteArray(), QByteArray("abc"));
}

TEST_F(SettingsTest, ValuesPersistInBackendFile) {
    s().setValue(key::PrefetchAhead, 7);
    s().setValue(key::ShowFilmstrip, true);
    s().setValue(QString(key::ShortcutPrefix) + "trash", QStringList());
    s().setValue(QString(key::ShortcutPrefix) + "zoomFit", QStringList{"Ctrl+9"});
    Settings::useTestingBackend(ini);  // reopen same file (flushes the old object)
    EXPECT_EQ(s().value(key::PrefetchAhead).toInt(), 7);
    EXPECT_TRUE(s().value(key::ShowFilmstrip).toBool());
    EXPECT_TRUE(s().value(QString(key::ShortcutPrefix) + "trash").toStringList().isEmpty());
    EXPECT_EQ(s().value(QString(key::ShortcutPrefix) + "zoomFit").toStringList(), QStringList{"Ctrl+9"});
}

TEST_F(SettingsTest, TestingBackendIsIsolated) {
    s().setValue(key::PrefetchAhead, 9);
    Settings::useTestingBackend(freshIniPath());
    EXPECT_EQ(s().value(key::PrefetchAhead).toInt(), 3);
}

TEST_F(SettingsTest, ResetToDefaultsByPrefix) {
    s().setValue(key::ZoomMode, "actual");
    s().setValue(key::ShowStatusBar, false);
    s().setValue(key::PrefetchAhead, 5);
    s().setValue(QString(key::InputPrefix) + "wheel", "zoom");
    ChangeRecorder spy(&s());
    s().resetToDefaults("view/");
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "fit");
    EXPECT_TRUE(s().value(key::ShowStatusBar).toBool());
    EXPECT_EQ(s().value(key::PrefetchAhead).toInt(), 5);
    EXPECT_EQ(s().value(QString(key::InputPrefix) + "wheel").toString(), "zoom");
    const QStringList emitted = spy.keys;
    EXPECT_TRUE(emitted.contains(key::ZoomMode));
    EXPECT_TRUE(emitted.contains(key::ShowStatusBar));
    EXPECT_FALSE(emitted.contains(key::PrefetchAhead));

    s().resetToDefaults();
    EXPECT_EQ(s().value(key::PrefetchAhead).toInt(), 3);
    EXPECT_EQ(s().value(QString(key::InputPrefix) + "wheel").toString(), "image");
}

// F-SET-03
TEST_F(SettingsTest, F_SET_03_ExportImportRoundtrip) {
    QTemporaryDir dir;
    const QString file = dir.filePath("export.json");
    s().setValue(key::ZoomMode, "fillWidth");
    s().setValue(key::PrefetchBehind, 4);
    s().setValue(key::FinderLiveQuery, true);
    s().setValue(QString(key::InputPrefix) + "space", "slideshow");
    s().setValue(QString(key::ShortcutPrefix) + "trash", QStringList{"Ctrl+Backspace"});
    QString err;
    ASSERT_TRUE(s().exportTo(file, &err)) << err.toStdString();

    Settings::useTestingBackend(freshIniPath());
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "fit");
    ChangeRecorder spy(&s());
    ASSERT_TRUE(s().importFrom(file, &err)) << err.toStdString();
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "fillWidth");
    EXPECT_EQ(s().value(key::PrefetchBehind).toInt(), 4);
    EXPECT_TRUE(s().value(key::FinderLiveQuery).toBool());
    EXPECT_EQ(s().value(QString(key::InputPrefix) + "space").toString(), "slideshow");
    EXPECT_EQ(s().value(QString(key::ShortcutPrefix) + "trash").toStringList(), QStringList{"Ctrl+Backspace"});
    EXPECT_GE(spy.count(), 5);
}

namespace {
void writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(data);
}
} // namespace

// F-SET-03
TEST_F(SettingsTest, F_SET_03_ImportRejectsInvalidFiles) {
    QTemporaryDir dir;
    QString err;
    s().setValue(key::ZoomMode, "actual");

    EXPECT_FALSE(s().importFrom(dir.filePath("missing.json"), &err));
    EXPECT_FALSE(err.isEmpty());

    writeFile(dir.filePath("garbage.json"), "this is { not json");
    err.clear();
    EXPECT_FALSE(s().importFrom(dir.filePath("garbage.json"), &err));
    EXPECT_FALSE(err.isEmpty());

    writeFile(dir.filePath("other.json"), R"({"foo": 1})");
    err.clear();
    EXPECT_FALSE(s().importFrom(dir.filePath("other.json"), &err));
    EXPECT_FALSE(err.isEmpty());

    // wrong types / out of range values: nothing is applied, error names the key
    writeFile(dir.filePath("badtypes.json"), R"({"format":"lumaflip-settings","version":1,"settings":{
        "view/zoomMode":"fill", "perf/prefetchAhead":"three", "view/statusBar":1,
        "input/wheel":"explode", "nav/wrap":"wrap"}})");
    err.clear();
    EXPECT_FALSE(s().importFrom(dir.filePath("badtypes.json"), &err));
    EXPECT_TRUE(err.contains("perf/prefetchAhead"));
    EXPECT_TRUE(err.contains("view/statusBar"));
    EXPECT_TRUE(err.contains("input/wheel"));
    EXPECT_EQ(s().value(key::ZoomMode).toString(), "actual");
    EXPECT_EQ(s().value(key::WrapMode).toString(), "stop");

    // unknown keys are ignored, known ones applied
    writeFile(dir.filePath("unknown.json"), R"({"format":"lumaflip-settings","version":1,"settings":{
        "future/thing":42, "nav/wrap":"wrap"}})");
    err.clear();
    EXPECT_TRUE(s().importFrom(dir.filePath("unknown.json"), &err)) << err.toStdString();
    EXPECT_EQ(s().value(key::WrapMode).toString(), "wrap");
    EXPECT_FALSE(s().value("future/thing").isValid());
}

// F-SORT-06
TEST_F(SettingsTest, F_SORT_06_FolderValuesStoredPerFolder) {
    QTemporaryDir dir;
    QDir(dir.path()).mkpath("a");
    QDir(dir.path()).mkpath("b");
    const QString a = dir.filePath("a");
    const QString b = dir.filePath("b");
    EXPECT_FALSE(s().folderValue(a, "sortKey").isValid());
    s().setFolderValue(a, "sortKey", "dateAdded");
    s().setFolderValue(a, "ascending", false);
    s().setFolderValue(b, "sortKey", "size");
    EXPECT_EQ(s().folderValue(a, "sortKey").toString(), "dateAdded");
    EXPECT_FALSE(s().folderValue(a, "ascending").toBool());
    EXPECT_EQ(s().folderValue(b, "sortKey").toString(), "size");
    // same folder via a non-canonical path
    EXPECT_EQ(s().folderValue(a + "/../a/", "sortKey").toString(), "dateAdded");
    // stored outside the folder, survives reloading
    EXPECT_FALSE(QFile::exists(a + "/.lumaflip"));
    Settings::useTestingBackend(ini);
    EXPECT_EQ(s().folderValue(a, "sortKey").toString(), "dateAdded");
    // removing
    s().setFolderValue(a, "sortKey", QVariant());
    EXPECT_FALSE(s().folderValue(a, "sortKey").isValid());
    EXPECT_EQ(s().folderValue(b, "sortKey").toString(), "size");
}

TEST_F(SettingsTest, ReadsFromWorkerThreadsAreSafe) {
    s().setValue(key::PrefetchAhead, 6);
    std::atomic<int> bad{0};
    std::vector<QThread*> threads;
    for (int t = 0; t < 4; ++t) {
        threads.push_back(QThread::create([&bad] {
            for (int i = 0; i < 2000; ++i)
                if (Settings::instance().value(key::PrefetchAhead).toInt() != 6)
                    ++bad;
        }));
        threads.back()->start();
    }
    for (int i = 0; i < 500; ++i)
        s().setValue(key::ZoomMode, i % 2 ? "actual" : "fit");
    for (auto* th : threads) {
        th->wait();
        delete th;
    }
    EXPECT_EQ(bad.load(), 0);
}
