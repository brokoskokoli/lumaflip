// SPDX-License-Identifier: GPL-3.0-or-later
// Core regression tests of the third review round: image metadata never read on the GUI
// thread (F-SORT-02), folder snapshots revalidated on an explicit open (F-WIN-09, B12),
// per-folder sort memory (F-SORT-06), prepared name keys (F-SORT-01, P-05), RAW setting
// changes reaching running decodes (F-FMT-05), the decode byte cap (P-08), presets and
// planned actions (F-IN-18, SPEC 5.2) and the restore default (F-WIN-06).

#include "SortingTestHelpers.h"
#include "core/Collection.h"
#include "core/Decoder.h"
#include "core/FolderCache.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QMutex>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QThread>
#include <QWaitCondition>
#include <gtest/gtest.h>

#include <atomic>

#ifdef Q_OS_UNIX
#include <sys/time.h>
#endif

using namespace lf;
using lf::test::waitUntil;

namespace {

QString makePng(const QString& path, QSize size = QSize(8, 8)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(Qt::red);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

QStringList names(const Collection& c) {
    QStringList out;
    for (const FileEntry& e : c.entries())
        out << e.name;
    return out;
}

bool touchFolder(const QString& folder, int secondsAhead) {
#ifdef Q_OS_UNIX
    struct timeval times[2];
    gettimeofday(&times[0], nullptr);
    times[0].tv_sec += secondsAhead;
    times[1] = times[0];
    return ::utimes(QFile::encodeName(folder).constData(), times) == 0;
#else
    Q_UNUSED(folder);
    Q_UNUSED(secondsAhead);
    return false;
#endif
}

// Records on which thread image properties are read.
class ThreadCheckingProps : public FileAttributeProvider {
public:
    void fill(FileEntry&) override {}
    void fillImageProperties(FileEntry& e) override {
        if (QThread::currentThread() == QCoreApplication::instance()->thread())
            ++guiCalls;
        ++calls;
        QThread::msleep(20);
        e.pixelCount = e.size; // something deterministic
        e.imagePropertiesRead = true;
    }
    std::atomic<int> guiCalls{0};
    std::atomic<int> calls{0};
};

// Decoder for ImageProvider tests: blank images, records requests, optional gate.
class RecordingDecoder : public Decoder {
public:
    struct Call {
        QString path;
        int maxPx = 0;
        qint64 maxBytes = 0;
        bool embedded = false;
    };
    QString name() const override { return QStringLiteral("rec"); }
    int priority() const override { return 100; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req, QString*) override {
        {
            QMutexLocker l(&m);
            calls.append({path, req.maxPixelSize, req.maxBytes, req.preferEmbeddedPreview});
            while (closed)
                gate.wait(&m);
        }
        DecodedImage d;
        d.fullSize = QSize(4000, 3000);
        const int edge = req.maxPixelSize > 0 ? req.maxPixelSize : 4000;
        d.image = QImage(QSize(4000, 3000).scaled(edge, edge, Qt::KeepAspectRatio), QImage::Format_RGB32);
        // The embedded preview is marked by its color (to tell the results apart).
        d.image.fill(req.preferEmbeddedPreview ? Qt::green : Qt::blue);
        d.isPreview = true;
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }
    void close() {
        QMutexLocker l(&m);
        closed = true;
    }
    void open() {
        QMutexLocker l(&m);
        closed = false;
        gate.wakeAll();
    }
    QList<Call> recorded() {
        QMutexLocker l(&m);
        return calls;
    }

private:
    QMutex m;
    QWaitCondition gate;
    bool closed = false;
    QList<Call> calls;
};

} // namespace

// ---- review item 5: replayed edits never read image metadata on the GUI thread ----

TEST(Review3Collection, F_SORT_02_ReplayReadsImagePropertiesOnWorker) {
    test::ensureApp();
    QTemporaryDir dir;
    ASSERT_FALSE(makePng(dir.filePath("IMG_10.png")).isEmpty());
    ASSERT_FALSE(makePng(dir.filePath("IMG_2.png"), QSize(16, 16)).isEmpty());
    ASSERT_FALSE(makePng(dir.filePath("IMG_1.png"), QSize(32, 32)).isEmpty());
    ThreadCheckingProps props;
    const auto collator = createNameCollator();
    {
        Collection c;
        c.setProviders(collator.get(), nullptr, &props);
        c.openFolder(dir.path());
        ASSERT_TRUE(waitUntil([&] { return !c.isLoading() && c.count() == 3; }));
        c.setSortSpec(SortSpec{SortKey::PixelCount, true, SortSource::Manual}); // resort reads metadata
        ASSERT_TRUE(QFile::rename(dir.filePath("IMG_2.png"), dir.filePath("IMG_2b.png")));
        c.renamePath(QDir::cleanPath(dir.filePath("IMG_2.png")), QDir::cleanPath(dir.filePath("IMG_2b.png")));
        ASSERT_TRUE(waitUntil([&] { return !c.isLoading(); }));
        EXPECT_EQ(props.guiCalls.load(), 0);
        EXPECT_EQ(c.count(), 3);
        EXPECT_GE(c.indexOf(QDir::cleanPath(dir.filePath("IMG_2b.png"))), 0);
        // Sorted by the values read on the worker (file size order of the PNGs).
        for (int i = 1; i < c.count(); ++i)
            EXPECT_LE(c.at(i - 1).pixelCount, c.at(i).pixelCount);
    }
    QThread::msleep(50); // let cancelled workers finish before `props` goes away
}

// ---- B12: snapshots of folders without change notifications ----

TEST(Review3FolderCache, F_WIN_09_RevalidateDetectsChangeWithoutWatcherEvent) {
    QTemporaryDir dir;
    const QString folder = QDir::cleanPath(dir.path());
    ASSERT_FALSE(makePng(dir.filePath("a.png")).isEmpty());
    FolderCache& cache = FolderCache::instance();
    cache.acquire(folder);
    const FolderSnapshotPtr first = cache.get(folder, nullptr);
    ASSERT_EQ(first->entries.size(), 1);
    // Changed on disk, no invalidate() (no FSEvents on the volume).
    ASSERT_FALSE(makePng(dir.filePath("b.png")).isEmpty());
    ASSERT_TRUE(touchFolder(folder, 5));
    EXPECT_TRUE(cache.isStale(folder));
    EXPECT_EQ(cache.get(folder, nullptr, false)->entries.size(), 1); // shared snapshot as is
    const int before = cache.enumerationCount(folder);
    EXPECT_EQ(cache.get(folder, nullptr, true)->entries.size(), 2); // explicit open: checked
    EXPECT_EQ(cache.enumerationCount(folder), before + 1);
    EXPECT_FALSE(cache.isStale(folder));
    // Unchanged: the check costs a stat, not an enumeration.
    EXPECT_EQ(cache.get(folder, nullptr, true)->entries.size(), 2);
    EXPECT_EQ(cache.enumerationCount(folder), before + 1);
    cache.release(folder);
}

TEST(Review3FolderCache, F_WIN_09_OpenFolderSeesChangeOtherCollectionMissed) {
    test::ensureApp();
    QTemporaryDir dir;
    ASSERT_FALSE(makePng(dir.filePath("a.png")).isEmpty());
    Collection first;
    first.openFolder(dir.path());
    ASSERT_TRUE(waitUntil([&] { return !first.isLoading(); }));
    ASSERT_EQ(first.count(), 1);
    ASSERT_FALSE(makePng(dir.filePath("b.png")).isEmpty());
    ASSERT_TRUE(touchFolder(dir.path(), 5));
    Collection second; // a new window opening the folder while `first` holds the snapshot
    second.openFolder(dir.path());
    ASSERT_TRUE(waitUntil([&] { return !second.isLoading(); }));
    EXPECT_EQ(second.count(), 2);
}

// ---- B5: F-SORT-06 per-folder sort memory ----

TEST(Review3Collection, F_SORT_06_FolderMemoryWinsOverWindowSort) {
    test::ensureApp();
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath("A") && QDir(dir.path()).mkpath("B"));
    ASSERT_FALSE(makePng(dir.filePath("A/a1.png")).isEmpty());
    ASSERT_FALSE(makePng(dir.filePath("A/a2.png"), QSize(300, 300)).isEmpty());
    ASSERT_FALSE(makePng(dir.filePath("B/b1.png")).isEmpty());
    ASSERT_FALSE(makePng(dir.filePath("B/b2.png"), QSize(300, 300)).isEmpty());
    const QString a = QDir::cleanPath(dir.filePath("A"));
    Collection c;
    c.setDefaultSort(false, false, SortSpec{SortKey::Name, true, SortSource::Manual});
    c.setFolderSortMemory([a](const QString& folder) -> std::optional<FolderSortChoice> {
        if (folder == a)
            return FolderSortChoice{false, SortSpec{SortKey::Size, false, SortSource::Manual}};
        return std::nullopt;
    });
    c.openFolder(dir.filePath("A"));
    ASSERT_TRUE(waitUntil([&] { return !c.isLoading(); }));
    EXPECT_EQ(names(c), (QStringList{"a2.png", "a1.png"}));
    EXPECT_EQ(c.sortSpec().key, SortKey::Size);
    EXPECT_EQ(c.sortSpec().source, SortSource::Manual);
    c.openFolder(dir.filePath("B"));
    ASSERT_TRUE(waitUntil([&] { return !c.isLoading(); }));
    EXPECT_EQ(names(c), (QStringList{"b1.png", "b2.png"}));
    EXPECT_EQ(c.sortSpec().key, SortKey::Name);
}

// ---- B13: precomputed name keys, identical order ----

TEST(Review3Sorting, F_SORT_01_PreparedNamesMatchCompare) {
    const auto collator = createNameCollator();
    QVector<QString> names = {"IMG_2 (1).jpg",
                              "IMG_2.jpg",
                              "IMG_10.jpg",
                              "img_2.JPG",
                              "IMG_02.jpg",
                              "001.jpg",
                              "1.jpg",
                              "01.jpg",
                              "a.jpg",
                              "A.jpg",
                              "photo 10.png",
                              "photo 2.png",
                              QString::fromUtf8("äpfel.jpg"),
                              QString::fromUtf8("Äpfel.jpg"),
                              QString::fromUtf8("ｆｕｌｌ.jpg"),
                              "full.jpg",
                              QString::fromUtf8("é.jpg"),
                              QString::fromUtf8("é.jpg"),
                              "e.jpg",
                              QString::fromUtf8("😀.jpg")};
    QRandomGenerator rng(42);
    for (int i = 0; i < 200; ++i)
        names << QStringLiteral("IMG_%1%2.jpg").arg(rng.bounded(5000)).arg(i % 3 ? "" : " Kopie");
    const auto prepared = collator->prepare(names);
    for (int i = 0; i < names.size(); ++i)
        for (int j = 0; j < names.size(); ++j)
            ASSERT_EQ(prepared->compare(i, j), collator->compare(names[i], names[j]))
                << names[i].toStdString() << " vs " << names[j].toStdString();
}

TEST(Review3Sorting, F_SORT_01_SortWithPreparedKeysIsFinderOrder) {
    const auto collator = createNameCollator();
    QVector<FileEntry> entries;
    const QStringList input = {"IMG_10.jpg",    "IMG_2.jpg", "IMG_1.png", "img_2.JPG",
                               "IMG_2 (1).jpg", "a.jpg",     "A.jpg"};
    for (const QString& n : input) {
        FileEntry e;
        e.name = n;
        e.path = QStringLiteral("/x/") + n;
        entries << e;
    }
    collection::sortEntries(entries, SortSpec{SortKey::Name, true, SortSource::Manual}, *collator);
    for (int i = 1; i < entries.size(); ++i) {
        const int c = collator->compare(entries[i - 1].name, entries[i].name);
        EXPECT_TRUE(c < 0 || (c == 0 && entries[i - 1].path < entries[i].path))
            << entries[i - 1].name.toStdString() << " / " << entries[i].name.toStdString();
    }
    // Descending: exactly reversed (names are distinct under forced ordering).
    QVector<FileEntry> desc = entries;
    collection::sortEntries(desc, SortSpec{SortKey::Name, false, SortSource::Manual}, *collator);
    for (int i = 0; i < entries.size(); ++i)
        EXPECT_EQ(desc[i].name, entries[entries.size() - 1 - i].name);
}

// ---- review item 16: the RAW setting reaches running decodes and shown images ----

TEST(Review3Provider, F_FMT_05_RawSettingInvalidatesRunningDecodes) {
    test::ensureApp();
    RecordingDecoder* dec = new RecordingDecoder;
    DecoderRegistry registry;
    registry.add(std::shared_ptr<Decoder>(dec));
    ImageProvider provider(registry);
    QStringList invalidated;
    QObject::connect(&provider, &ImageProvider::invalidated, [&](const QString& p) { invalidated << p; });
    QList<DecodedImagePtr> screens;
    QObject::connect(&provider, &ImageProvider::imageReady,
                     [&](const QString&, Quality q, const DecodedImagePtr& i) {
                         if (q == Quality::Screen)
                             screens << i;
                     });
    const QString raw = QStringLiteral("/nowhere/shot.cr2");
    int client = 0;
    dec->close();
    provider.request(raw, Quality::Screen, QSize(1000, 800), &client);
    ASSERT_TRUE(waitUntil([&] { return dec->recorded().size() >= 2; })); // preview + screen running
    provider.setRawEmbeddedOnly(true);                                   // while they run
    EXPECT_TRUE(invalidated.contains(raw));
    dec->open();
    ASSERT_TRUE(waitUntil([&] { return !screens.isEmpty(); }));
    // The screen stage that reached the client was decoded with the new setting.
    EXPECT_EQ(screens.last()->image.pixelColor(0, 0), QColor(Qt::green));
    // A cached RAW stage is replaced when the setting changes back (no new request()).
    screens.clear();
    invalidated.clear();
    provider.setRawEmbeddedOnly(false);
    EXPECT_TRUE(invalidated.contains(raw));
    ASSERT_TRUE(waitUntil([&] { return !screens.isEmpty(); }));
    EXPECT_EQ(screens.last()->image.pixelColor(0, 0), QColor(Qt::blue));
}

// ---- P-08: the decode byte cap follows the memory budget ----

TEST(Review3Provider, P_08_DecodeByteCapFollowsBudget) {
    test::ensureApp();
    RecordingDecoder* dec = new RecordingDecoder;
    DecoderRegistry registry;
    registry.add(std::shared_ptr<Decoder>(dec));
    ImageProvider provider(registry);
    provider.setMemoryBudget(qint64(512) << 20);
    EXPECT_EQ(provider.maxDecodeBytes(), qint64(256) << 20);
    provider.setMemoryBudget(qint64(8) << 30);
    EXPECT_EQ(provider.maxDecodeBytes(), qint64(1) << 30); // never above 1 GB
    provider.setMemoryBudget(qint64(600) << 20);
    int client = 0;
    provider.request(QStringLiteral("/nowhere/a.png"), Quality::Full, {}, &client);
    ASSERT_TRUE(waitUntil([&] {
        for (const auto& c : dec->recorded())
            if (c.maxPx == 0)
                return true;
        return false;
    }));
    for (const auto& c : dec->recorded())
        EXPECT_EQ(c.maxBytes, qint64(300) << 20); // every decode carries the cap
}

// ---- B3 / B4: presets and planned actions ----

TEST(Review3Input, F_IN_18_PresetSkipsUnavailableChoices) {
    QTemporaryDir dir;
    Settings::useTestingBackend(dir.filePath("p.ini"));
    EXPECT_FALSE(InputBindings::isChoiceAvailable("middleClick", "magnifier"));
    EXPECT_TRUE(InputBindings::isChoiceAvailable("ratingKeys", "always")); // M3
    EXPECT_TRUE(InputBindings::isChoiceAvailable("ratingKeys", "cullingOnly"));
    InputBindings::applyPreset("photographer");
    InputBindings b;
    EXPECT_EQ(b.choice("wheel"), "zoom"); // available: applied
    EXPECT_EQ(b.choice("click"), "fitActual");
    EXPECT_EQ(b.choice("ratingKeys"), "always"); // M3: ratings are implemented
    for (const InputSlot& s : InputBindings::inputSlots())
        EXPECT_TRUE(InputBindings::isChoiceAvailable(s.id, b.choice(s.id))) << s.id.toStdString();
}

TEST(Review3Input, F_SET_02_PlannedActionsAreNotAvailable) {
    for (Action a : {Action::ToggleGrid, Action::Magnifier})
        EXPECT_FALSE(InputBindings::isActionAvailable(a)) << actionId(a).toStdString();
    // M2: slideshow, animation, pages and saving the rotation are implemented.
    for (Action a : {Action::NextImage, Action::ZoomFit, Action::Trash, Action::ToggleFullScreen,
                     Action::CopyImage, Action::ShowShortcuts, Action::NextFolder, Action::ToggleInfoPanel,
                     Action::ToggleFilmstrip, Action::Rename, Action::OpenInEditor, Action::DragOut,
                     Action::ToggleSlideshow, Action::ToggleAnimation, Action::NextFrame,
                     Action::PreviousFrame, Action::NextPage, Action::PreviousPage, Action::SaveRotation})
        EXPECT_TRUE(InputBindings::isActionAvailable(a)) << actionId(a).toStdString();
}

// ---- B1: F-WIN-06 default ----

TEST(Review3Settings, F_WIN_06_RestoreDefaultFollowsSystemSetting) {
    QTemporaryDir dir;
    Settings::useTestingBackend(dir.filePath("r.ini"));
    Settings& s = Settings::instance();
    Settings::setSystemRestoreWindowsForTesting(0);
    EXPECT_FALSE(s.value(key::RestoreWindows).toBool());
    Settings::setSystemRestoreWindowsForTesting(1);
    EXPECT_TRUE(s.value(key::RestoreWindows).toBool());
    EXPECT_TRUE(s.defaultValue(key::RestoreWindows).toBool());
    s.setValue(key::RestoreWindows, false);
    EXPECT_FALSE(s.value(key::RestoreWindows).toBool());
    Settings::setSystemRestoreWindowsForTesting(-1);
    s.resetToDefaults(key::RestoreWindows);
    EXPECT_EQ(s.value(key::RestoreWindows).toBool(), systemRestoresWindows());
}
