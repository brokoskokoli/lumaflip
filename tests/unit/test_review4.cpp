// SPDX-License-Identifier: GPL-3.0-or-later
// Review round 4 (core): explicit stage marker instead of isPreview (F-INFO-03, F-FMT-03),
// page-level failures (F-FMT-04), thumbnail invalidation incl. disk (F-GRID-01),
// application-scoped singletons (F-WIN-09) and folder snapshots older than an own file
// operation (F-FILE-01).

#include "QtTestApp.h"
#include "core/AppScoped.h"
#include "core/Collection.h"
#include "core/Decoder.h"
#include "core/FolderCache.h"
#include "core/ImageProvider.h"
#include "core/ThumbnailCache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QWaitCondition>

#include <gtest/gtest.h>

#include <atomic>

using namespace lf;
using lf::test::waitUntil;

namespace {

// Pages of `fullSize`; page `failingPage` fails. Images are downscaled to maxPixelSize
// like ImageIO does (isPreview set for every reduced image, as the real backends do).
class PagedDecoder : public Decoder {
public:
    QString name() const override { return QStringLiteral("paged"); }
    int priority() const override { return 1000; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override {
        if (req.page == failingPage) {
            *error = QStringLiteral("Seite kaputt");
            return std::nullopt;
        }
        const QSize full = path.contains(QLatin1String("small")) ? QSize(40, 30) : fullSize;
        DecodedImage d;
        d.fullSize = full;
        const QSize s = req.maxPixelSize > 0
                            ? full.scaled(req.maxPixelSize, req.maxPixelSize, Qt::KeepAspectRatio)
                            : full;
        d.image = QImage(s.boundedTo(full), QImage::Format_RGB32);
        d.image.fill(Qt::darkGreen);
        d.isPreview = d.image.size() != full;
        d.page = req.page;
        d.pageCount = 3;
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }

    QSize fullSize{6000, 4000};
    int failingPage = -1;
};

struct ProviderFixture {
    ProviderFixture() {
        test::ensureQtApp();
        decoder = std::make_shared<PagedDecoder>();
        registry.add(decoder);
        provider = std::make_unique<ImageProvider>(registry);
        QObject::connect(
            provider.get(), &ImageProvider::imageReady,
            [this](const QString&, Quality q, const DecodedImagePtr& img) { ready.append({q, img}); });
        QObject::connect(provider.get(), &ImageProvider::imageFailed,
                         [this](const QString&, const QString&) { ++imageFailed; });
        QObject::connect(
            provider.get(), &ImageProvider::pageFailed,
            [this](const QString&, int page, const QString& e) { pageFailed.append({page, e}); });
        QObject::connect(provider.get(), &ImageProvider::stageFailed,
                         [this](const QString&, Quality q, const QString&) {
                             if (q != Quality::Preview) // a preview failing before the screen stage is normal
                                 ++stageFailed;
                         });
    }
    DecodedImagePtr waitStage(Quality q, int page = 0) {
        DecodedImagePtr out;
        waitUntil(
            [&] {
                for (const auto& r : std::as_const(ready))
                    if (r.first == q && r.second->page == page)
                        out = r.second;
                return out != nullptr;
            },
            5000);
        return out;
    }

    DecoderRegistry registry;
    std::shared_ptr<PagedDecoder> decoder;
    std::unique_ptr<ImageProvider> provider;
    QList<QPair<Quality, DecodedImagePtr>> ready;
    QList<QPair<int, QString>> pageFailed;
    int imageFailed = 0;
    int stageFailed = 0;
    int client = 0;
};

} // namespace

// ---- A: the stage marker ----

// Covers: F-INFO-03, F-FMT-03 (a downscaled Screen stage is Screen quality, not a preview)
TEST(Review4Provider, F_INFO_03_StageMarkerIndependentOfIsPreview) {
    ProviderFixture f;
    const QString big = QStringLiteral("/x/big24mp.jpg");
    f.provider->request(big, Quality::Screen, QSize(1920, 1080), &f.client);
    const DecodedImagePtr screen = f.waitStage(Quality::Screen);
    ASSERT_TRUE(screen);
    EXPECT_TRUE(screen->isPreview); // the decoder's view: smaller than the file
    EXPECT_EQ(screen->quality, Quality::Screen);
    EXPECT_LT(screen->image.width(), 6000);
    // The cached stage carries the marker as well (what the info panel gates on).
    const DecodedImagePtr cached = f.provider->cached(big, Quality::Screen, QSize(1920, 1080));
    ASSERT_TRUE(cached);
    EXPECT_EQ(cached->quality, Quality::Screen);
    // The preview stage, if it was delivered, is marked as such.
    for (const auto& r : std::as_const(f.ready))
        if (r.first == Quality::Preview)
            EXPECT_EQ(r.second->quality, Quality::Preview);
    // A small file: its preview stage already is the full image.
    const QString small = QStringLiteral("/x/small.png");
    f.provider->request(small, Quality::Screen, QSize(1920, 1080), &f.client);
    ASSERT_TRUE(waitUntil([&] { return f.provider->cached(small, Quality::Screen) != nullptr; }, 5000));
    EXPECT_EQ(f.provider->cached(small, Quality::Screen)->quality, Quality::Full);
    // Full resolution on request.
    f.provider->request(big, Quality::Full, {}, &f.client);
    const DecodedImagePtr full = f.waitStage(Quality::Full);
    ASSERT_TRUE(full);
    EXPECT_EQ(full->quality, Quality::Full);
}

// ---- C: failures per page ----

// Covers: F-FMT-04 (a broken page fails alone; the file and its other pages stay usable)
TEST(Review4Provider, F_FMT_04_PageFailureOnlyForThatPage) {
    ProviderFixture f;
    f.decoder->failingPage = 1;
    const QString tif = QStringLiteral("/x/pages.tif");
    f.provider->request(tif, Quality::Screen, QSize(1920, 1080), &f.client, 0, 0);
    ASSERT_TRUE(f.waitStage(Quality::Screen, 0));
    // Page 1 fails while page 0 is cached: a page failure, not a stage failure of the
    // file, and not imageFailed (which means "nothing of the file").
    f.provider->request(tif, Quality::Screen, QSize(1920, 1080), &f.client, 0, 1);
    ASSERT_TRUE(waitUntil([&] { return !f.pageFailed.isEmpty(); }, 5000));
    EXPECT_EQ(f.pageFailed.first().first, 1);
    EXPECT_FALSE(f.pageFailed.first().second.isEmpty()); // the user facing message of the registry
    test::spin(50);
    EXPECT_EQ(f.imageFailed, 0);
    EXPECT_EQ(f.stageFailed, 0); // no "Volle Auflösung nicht verfügbar" for a broken page
    // Asking for page 1 again reports it again (not retried); page 0 still works and
    // page 2 decodes normally.
    f.pageFailed.clear();
    f.provider->request(tif, Quality::Screen, QSize(1920, 1080), &f.client, 0, 1);
    ASSERT_TRUE(waitUntil([&] { return !f.pageFailed.isEmpty(); }, 5000));
    EXPECT_EQ(f.pageFailed.first().first, 1);
    EXPECT_TRUE(f.provider->request(tif, Quality::Screen, QSize(1920, 1080), &f.client, 0, 0));
    f.provider->request(tif, Quality::Screen, QSize(1920, 1080), &f.client, 0, 2);
    EXPECT_TRUE(f.waitStage(Quality::Screen, 2));
    EXPECT_EQ(f.imageFailed, 0);
    // Page 0 failing is a failure of the file as well (imageFailed for page-less clients).
    f.decoder->failingPage = 0;
    const QString other = QStringLiteral("/x/other.tif");
    f.pageFailed.clear();
    f.provider->request(other, Quality::Screen, QSize(1920, 1080), &f.client, 0, 0);
    ASSERT_TRUE(waitUntil([&] { return f.imageFailed == 1; }, 5000));
    ASSERT_FALSE(f.pageFailed.isEmpty());
    EXPECT_EQ(f.pageFailed.first().first, 0);
}

// ---- G: thumbnails ----

namespace {

// Thumbnail color = first byte of the file (so a rewrite with the same size shows).
class ContentDecoder : public Decoder {
public:
    QString name() const override { return QStringLiteral("content"); }
    int priority() const override { return 1000; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req, QString*) override {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return std::nullopt;
        const QByteArray head = f.read(1);
        const char first = head.isEmpty() ? 0 : head.at(0);
        f.close();
        ++started;
        if (gate)
            gate->acquire();
        DecodedImage d;
        d.fullSize = QSize(400, 300);
        const int px = req.maxPixelSize > 0 ? req.maxPixelSize : 400;
        d.image = QImage(d.fullSize.scaled(px, px, Qt::KeepAspectRatio), QImage::Format_RGB32);
        d.image.fill(first == 'B' ? Qt::blue : Qt::red);
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }
    std::atomic<int> started{0};
    QSemaphore* gate = nullptr;
};

// Rewrites `path` with `byte` + the same length and modification time (like a rotation
// saved with "keep modification date" that does not change the file size).
bool rewriteKeepingStamp(const QString& path, char byte) {
    const QFileInfo before(path);
    const QDateTime mtime = before.lastModified(QTimeZone::UTC);
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    QByteArray data = f.readAll();
    data[0] = byte;
    f.seek(0);
    f.write(data);
    f.close();
    QFile g(path);
    if (!g.open(QIODevice::ReadWrite) || !g.setFileTime(mtime, QFileDevice::FileModificationTime))
        return false;
    g.close();
    const QFileInfo after(path);
    return after.size() == before.size() && after.lastModified(QTimeZone::UTC) == mtime;
}

bool isBlue(const QImage& img) {
    if (img.isNull())
        return false;
    const QColor c = img.pixelColor(img.width() / 2, img.height() / 2); // JPEG on disk: not exact
    return c.blue() > 200 && c.red() < 60;
}

struct ThumbFixture {
    ThumbFixture() {
        test::ensureQtApp();
        decoder = std::make_shared<ContentDecoder>();
        registry.add(decoder);
        file = QDir::cleanPath(files.filePath(QStringLiteral("photo.jpg")));
        QFile f(file);
        if (f.open(QIODevice::WriteOnly))
            f.write("R-some-image-bytes");
    }
    ThumbnailCacheOptions options() const {
        ThumbnailCacheOptions o;
        o.directory = cacheDir.path() + QStringLiteral("/thumbs");
        o.threads = 1;
        return o;
    }
    QString diskFile(int size) const {
        const QFileInfo fi(file);
        return cacheDir.path() + QStringLiteral("/thumbs/") +
               ThumbnailCache::diskFileName(file, fi.size(),
                                            fi.lastModified(QTimeZone::UTC).toMSecsSinceEpoch(), size);
    }
    QTemporaryDir files, cacheDir;
    std::shared_ptr<ContentDecoder> decoder;
    DecoderRegistry registry;
    QString file;
    int client = 0;
};

} // namespace

// Covers: F-GRID-01 (rewritten with the same size and date: every size is generated anew,
// not read back from the disk cache)
TEST(Review4Thumbnails, F_GRID_01_InvalidateDropsDiskEntriesOfAllSizes) {
    ThumbFixture f;
    {
        ThumbnailCache cache(f.registry, f.options());
        for (int size : {128, 192}) {
            cache.request(f.file, size, &f.client);
            ASSERT_TRUE(waitUntil([&] { return !cache.cached(f.file, size).isNull(); }, 5000));
            EXPECT_FALSE(isBlue(cache.cached(f.file, size)));
            EXPECT_TRUE(QFileInfo::exists(f.diskFile(size)));
        }
        ASSERT_TRUE(rewriteKeepingStamp(f.file, 'B'));
        cache.invalidate(f.file);
        EXPECT_TRUE(cache.cached(f.file, 128).isNull());
        // The disk files of both sizes go (on a worker).
        ASSERT_TRUE(waitUntil(
            [&] { return !QFileInfo::exists(f.diskFile(128)) && !QFileInfo::exists(f.diskFile(192)); },
            5000));
        cache.request(f.file, 192, &f.client);
        ASSERT_TRUE(waitUntil([&] { return !cache.cached(f.file, 192).isNull(); }, 5000));
        EXPECT_TRUE(isBlue(cache.cached(f.file, 192)));
    }
    // A new cache (next launch) reads the new thumbnail from disk, not the old one.
    ThumbnailCache again(f.registry, f.options());
    again.request(f.file, 192, &f.client);
    ASSERT_TRUE(waitUntil([&] { return !again.cached(f.file, 192).isNull(); }, 5000));
    EXPECT_TRUE(isBlue(again.cached(f.file, 192)));
    EXPECT_EQ(again.diskHits(), 1);
    // The other size was never written again: generated afresh.
    again.request(f.file, 128, &f.client);
    ASSERT_TRUE(waitUntil([&] { return !again.cached(f.file, 128).isNull(); }, 5000));
    EXPECT_TRUE(isBlue(again.cached(f.file, 128)));
}

// Covers: F-GRID-01 (a job running while the file changes cannot put the old thumbnail
// back - neither into memory nor onto disk)
TEST(Review4Thumbnails, F_GRID_01_RunningJobCannotReinsertStaleThumbnail) {
    ThumbFixture f;
    QSemaphore gate;
    f.decoder->gate = &gate;
    ThumbnailCache cache(f.registry, f.options());
    int readyCount = 0;
    QObject::connect(&cache, &ThumbnailCache::thumbnailReady, [&] { ++readyCount; });
    cache.request(f.file, 192, &f.client);
    ASSERT_TRUE(waitUntil([&] { return f.decoder->started.load() == 1; }, 5000)); // read "R", now blocked
    ASSERT_TRUE(rewriteKeepingStamp(f.file, 'B'));
    cache.invalidate(f.file);
    gate.release(10); // the stale job finishes; its result is dropped and the job runs again
    ASSERT_TRUE(
        waitUntil([&] { return !cache.cached(f.file, 192).isNull() && cache.pendingJobs() == 0; }, 5000));
    EXPECT_TRUE(isBlue(cache.cached(f.file, 192)));
    EXPECT_EQ(f.decoder->started.load(), 2);
    EXPECT_EQ(readyCount, 1); // the stale red one was never announced
    // On disk: only the new one.
    ThumbnailCache again(f.registry, f.options());
    again.request(f.file, 192, &f.client);
    ASSERT_TRUE(waitUntil([&] { return !again.cached(f.file, 192).isNull(); }, 5000));
    EXPECT_TRUE(isBlue(again.cached(f.file, 192)));
    EXPECT_EQ(again.diskHits(), 1);
}

// ---- F: application-scoped singletons ----

// Covers: F-WIN-09 (shared services follow the application object: created again for a new
// one instead of dangling)
TEST(Review4Singletons, F_WIN_09_AppScopedRecreatedWithNewParent) {
    test::ensureQtApp();
    int created = 0;
    AppScoped<QObject> scoped([&](QObject* parent) {
        ++created;
        return new QObject(parent);
    });
    auto* app1 = new QObject;
    QObject* a = &scoped.get(app1);
    EXPECT_EQ(&scoped.get(app1), a); // one per parent
    EXPECT_EQ(a->parent(), app1);
    EXPECT_EQ(created, 1);
    delete app1; // the "application object" goes, and the object with it
    EXPECT_EQ(scoped.existing(), nullptr);
    QObject app2;
    QObject* b = &scoped.get(&app2);
    EXPECT_EQ(created, 2);
    EXPECT_EQ(b->parent(), &app2);
    EXPECT_EQ(scoped.existing(), b);
    // The real singletons are owned by the application object.
    EXPECT_EQ(ImageProvider::instance().parent(), QCoreApplication::instance());
    EXPECT_EQ(ThumbnailCache::instance().parent(), QCoreApplication::instance());
}

// ---- H: folder snapshots older than an own file operation ----

// Covers: F-FILE-01, F-WIN-09 (a reload after the app trashed a file - e.g. for a watcher
// event from before - never brings the file back from an older shared snapshot)
TEST(Review4Collection, F_FILE_01_ReloadAfterOwnEditIgnoresOlderSnapshot) {
    test::ensureQtApp();
    QTemporaryDir dir;
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"}) {
        QFile f(dir.filePath(QString::fromLatin1(n)));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString folder = QDir::cleanPath(dir.path());
    Collection c1, c2;
    c1.openFolder(folder);
    ASSERT_TRUE(waitUntil([&] { return !c1.isLoading(); }, 5000));
    c2.openFolder(folder); // shares the snapshot
    ASSERT_TRUE(waitUntil([&] { return !c2.isLoading(); }, 5000));
    ASSERT_EQ(c1.count(), 3);
    const quint64 before = FolderCache::localChangeSeq();
    // FileOps moved b.jpg away; every window removes it.
    ASSERT_TRUE(QFile::remove(folder + QStringLiteral("/b.jpg")));
    c1.removePath(folder + QStringLiteral("/b.jpg"));
    c2.removePath(folder + QStringLiteral("/b.jpg"));
    EXPECT_GT(FolderCache::localChangeSeq(), before);
    EXPECT_EQ(c2.count(), 2);
    // A reload for an older watcher event: without the change sequence it took the cached
    // pre-trash snapshot and b.jpg came back until the next event.
    c2.reload();
    ASSERT_TRUE(waitUntil([&] { return !c2.isLoading(); }, 5000));
    EXPECT_EQ(c2.count(), 2);
    EXPECT_EQ(c2.indexOf(folder + QStringLiteral("/b.jpg")), -1);
    // Snapshots enumerated after the edit carry a newer sequence number.
    const FolderSnapshotPtr snap = FolderCache::instance().get(folder, nullptr);
    ASSERT_TRUE(snap);
    EXPECT_GE(snap->changeSeq, FolderCache::localChangeSeq());
}
