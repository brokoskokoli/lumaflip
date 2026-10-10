// SPDX-License-Identifier: GPL-3.0-or-later
// F-GRID-01 / SPEC 7.1, 8.6: thumbnail cache (memory LRU, disk cache, size bound,
// low-priority generation, dropping requests that are no longer visible).

#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/ThumbnailCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimeZone>

#include <gtest/gtest.h>

#include <atomic>

using namespace lf;

namespace {

// Decodes every file to a solid 800×600 image whose color depends on the file name.
class CountingDecoder : public Decoder {
public:
    QString name() const override { return QStringLiteral("counting"); }
    int priority() const override { return 1000; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req, QString*) override {
        ++started;
        if (gate)
            gate->acquire();
        ++decodes;
        lastRequest = req;
        DecodedImage d;
        d.fullSize = QSize(800, 600);
        const QSize s = req.maxPixelSize > 0
                            ? d.fullSize.scaled(req.maxPixelSize, req.maxPixelSize, Qt::KeepAspectRatio)
                            : d.fullSize;
        d.image = QImage(s, QImage::Format_RGB32);
        const uint h = qHash(QFileInfo(path).fileName());
        d.image.fill(QColor::fromHsv(int(h % 360), 200, 200));
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }

    std::atomic<int> started{0};
    std::atomic<int> decodes{0};
    DecodeRequest lastRequest;
    QSemaphore* gate = nullptr;
};

class ThumbnailCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        test::ensureQtApp();
        ASSERT_TRUE(files.isValid());
        ASSERT_TRUE(cacheDir.isValid());
        decoder = std::make_shared<CountingDecoder>();
        registry.add(decoder);
    }

    QString makeFile(const QString& name) {
        const QString path = QDir::cleanPath(files.filePath(name));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(name.toUtf8()); // contents do not matter for the fake decoder
        return path;
    }

    ThumbnailCacheOptions options(qint64 diskBudget = qint64(500) << 20) const {
        ThumbnailCacheOptions o;
        o.directory = cacheDir.path() + QStringLiteral("/thumbs");
        o.diskBudget = diskBudget;
        o.threads = 1;
        return o;
    }

    static bool waitReady(ThumbnailCache& cache, const QString& path, int size) {
        return test::waitUntil([&] { return !cache.cached(path, size).isNull(); }, 5000);
    }

    QString diskFile(const ThumbnailCache& cache, const QString& path, int size) const {
        const QFileInfo fi(path);
        return cache.directory() + QLatin1Char('/') +
               QString::fromLatin1(ThumbnailCache::cacheKey(
                   path, fi.size(), fi.lastModified(QTimeZone::UTC).toMSecsSinceEpoch(), size)) +
               QStringLiteral(".thumb");
    }

    QTemporaryDir files;
    QTemporaryDir cacheDir;
    std::shared_ptr<CountingDecoder> decoder;
    DecoderRegistry registry;
    int client = 0;
};

} // namespace

TEST_F(ThumbnailCacheTest, F_GRID_01_GeneratesSmallThumbnailOnce) {
    ThumbnailCache cache(registry, options());
    const QString p = makeFile(QStringLiteral("a.jpg"));
    int ready = 0;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, [&] { ++ready; });
    EXPECT_TRUE(cache.request(p, 128, &client).isNull()); // generated on a worker
    ASSERT_TRUE(waitReady(cache, p, 128));
    EXPECT_EQ(ready, 1);
    const QImage t = cache.request(p, 128, &client);
    ASSERT_FALSE(t.isNull());
    EXPECT_EQ(std::max(t.width(), t.height()), 128);
    EXPECT_EQ(t.size(), QSize(128, 96));
    EXPECT_EQ(decoder->decodes.load(), 1);
    EXPECT_EQ(decoder->lastRequest.maxPixelSize, 128); // decode-time downsampling
    EXPECT_TRUE(decoder->lastRequest.preferEmbeddedPreview);
    // Another size is another thumbnail.
    EXPECT_TRUE(cache.request(p, 256, &client).isNull());
    ASSERT_TRUE(waitReady(cache, p, 256));
    EXPECT_EQ(decoder->decodes.load(), 2);
}

TEST_F(ThumbnailCacheTest, F_GRID_01_DiskCacheHitAfterRestart) {
    const QString p = makeFile(QStringLiteral("b.png"));
    {
        ThumbnailCache cache(registry, options());
        cache.request(p, 192, &client);
        ASSERT_TRUE(waitReady(cache, p, 192));
        EXPECT_EQ(cache.decodeCount(), 1);
        EXPECT_TRUE(QFileInfo::exists(diskFile(cache, p, 192)));
        EXPECT_GT(cache.diskUsage(), 0);
    }
    ThumbnailCache again(registry, options());
    again.request(p, 192, &client);
    ASSERT_TRUE(waitReady(again, p, 192));
    EXPECT_EQ(again.diskHits(), 1);
    EXPECT_EQ(again.decodeCount(), 0);
    EXPECT_EQ(decoder->decodes.load(), 1); // not decoded again
    EXPECT_EQ(again.cached(p, 192).size(), QSize(192, 144));

    // A changed file (size, modification time) has another key: decoded again.
    test::spin(20);
    {
        QFile f(p);
        ASSERT_TRUE(f.open(QIODevice::Append));
        f.write("more");
    }
    again.invalidate(p);
    again.request(p, 192, &client);
    ASSERT_TRUE(waitReady(again, p, 192));
    EXPECT_EQ(decoder->decodes.load(), 2);
}

TEST_F(ThumbnailCacheTest, F_GRID_01_CacheKeyCoversPathSizeTimeAndThumbSize) {
    const QByteArray k = ThumbnailCache::cacheKey(QStringLiteral("/a.jpg"), 10, 1000, 128);
    EXPECT_EQ(k.size(), 40); // hex SHA-1
    EXPECT_EQ(k, ThumbnailCache::cacheKey(QStringLiteral("/a.jpg"), 10, 1000, 128));
    EXPECT_NE(k, ThumbnailCache::cacheKey(QStringLiteral("/b.jpg"), 10, 1000, 128));
    EXPECT_NE(k, ThumbnailCache::cacheKey(QStringLiteral("/a.jpg"), 11, 1000, 128));
    EXPECT_NE(k, ThumbnailCache::cacheKey(QStringLiteral("/a.jpg"), 10, 1001, 128));
    EXPECT_NE(k, ThumbnailCache::cacheKey(QStringLiteral("/a.jpg"), 10, 1000, 192));
    EXPECT_TRUE(ThumbnailCache::defaultDirectory().endsWith(QStringLiteral("/thumbs")));
}

TEST_F(ThumbnailCacheTest, F_GRID_01_DiskCacheSizeBoundLruEviction) {
    // Measure the size of one cache file (all thumbnails here are equally simple).
    qint64 one = 0;
    {
        QTemporaryDir probeDir;
        ThumbnailCacheOptions o = options();
        o.directory = probeDir.path();
        ThumbnailCache probe(registry, o);
        const QString p = makeFile(QStringLiteral("probe.jpg"));
        probe.request(p, 128, &client);
        ASSERT_TRUE(waitReady(probe, p, 128));
        one = probe.diskUsage();
    }
    ASSERT_GT(one, 0);
    const qint64 budget = one * 7 / 2; // room for three
    QStringList paths;
    for (int i = 1; i <= 4; ++i)
        paths << makeFile(QStringLiteral("img%1.jpg").arg(i));
    {
        ThumbnailCache cache(registry, options(budget));
        for (int i = 0; i < 3; ++i) {
            cache.request(paths[i], 128, &client);
            ASSERT_TRUE(waitReady(cache, paths[i], 128));
            test::spin(30); // distinct last-use times
        }
        EXPECT_LE(cache.diskUsage(), budget);
    }
    {
        // Using img1 again (new cache object: read from disk) makes img2 the oldest.
        ThumbnailCache cache(registry, options(budget));
        cache.request(paths[0], 128, &client);
        ASSERT_TRUE(waitReady(cache, paths[0], 128));
        EXPECT_EQ(cache.diskHits(), 1);
        test::spin(30);
        cache.request(paths[3], 128, &client);
        ASSERT_TRUE(waitReady(cache, paths[3], 128));
        EXPECT_LE(cache.diskUsage(), budget);
        EXPECT_TRUE(QFileInfo::exists(diskFile(cache, paths[0], 128)));  // recently used
        EXPECT_FALSE(QFileInfo::exists(diskFile(cache, paths[1], 128))); // least recently used
        EXPECT_TRUE(QFileInfo::exists(diskFile(cache, paths[2], 128)));
        EXPECT_TRUE(QFileInfo::exists(diskFile(cache, paths[3], 128))); // just written
    }
}

TEST_F(ThumbnailCacheTest, F_GRID_01_RetainDropsRequestsOutOfView) {
    QSemaphore gate;
    decoder->gate = &gate;
    ThumbnailCache cache(registry, options());
    QStringList paths;
    for (int i = 0; i < 10; ++i)
        paths << makeFile(QStringLiteral("r%1.jpg").arg(i));
    for (const QString& p : paths)
        cache.request(p, 128, &client);
    // One job runs (blocked in the decoder), nine are queued.
    ASSERT_TRUE(test::waitUntil([&] { return decoder->started.load() == 1; }));
    EXPECT_EQ(cache.pendingJobs(), 10);
    // Scrolled: only the last one is still visible.
    cache.retain(&client, {paths.last()});
    EXPECT_LE(cache.pendingJobs(), 2);
    gate.release(100);
    ASSERT_TRUE(test::waitUntil([&] { return cache.pendingJobs() == 0; }, 5000));
    EXPECT_LE(decoder->decodes.load(), 2);
    EXPECT_FALSE(cache.cached(paths.last(), 128).isNull()); // the visible one was made
    decoder->gate = nullptr;
}

TEST_F(ThumbnailCacheTest, F_GRID_01_NewestRequestFirstAndFailuresNotRetried) {
    QSemaphore gate;
    decoder->gate = &gate;
    ThumbnailCache cache(registry, options());
    const QString first = makeFile(QStringLiteral("first.jpg"));
    const QString older = makeFile(QStringLiteral("older.jpg"));
    const QString newest = makeFile(QStringLiteral("newest.jpg"));
    QStringList order;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady,
                     [&](const QString& p, int) { order << QFileInfo(p).fileName(); });
    cache.request(first, 128, &client);
    ASSERT_TRUE(test::waitUntil([&] { return decoder->started.load() == 1; })); // running
    cache.request(older, 128, &client);
    cache.request(newest, 128, &client); // what the user looks at now
    gate.release(100);
    ASSERT_TRUE(test::waitUntil([&] { return order.size() == 3; }, 5000));
    EXPECT_EQ(order, (QStringList{"first.jpg", "newest.jpg", "older.jpg"}));
    decoder->gate = nullptr;

    // A missing file fails once and is not requested again until invalidated.
    const QString gone = files.filePath(QStringLiteral("gone.jpg"));
    int failed = 0;
    QObject::connect(&cache, &ThumbnailCache::thumbnailFailed, [&] { ++failed; });
    cache.request(gone, 128, &client);
    ASSERT_TRUE(test::waitUntil([&] { return failed == 1; }, 5000));
    cache.request(gone, 128, &client);
    EXPECT_EQ(cache.pendingJobs(), 0);
    cache.invalidate(gone);
    cache.request(gone, 128, &client);
    EXPECT_EQ(cache.pendingJobs(), 1);
    ASSERT_TRUE(test::waitUntil([&] { return failed == 2; }, 5000));
}
