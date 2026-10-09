// SPDX-License-Identifier: GPL-3.0-or-later
// ImageProvider: staged loading, cache, dedupe, cancellation, memory budget
// (SPEC 7.1, P-03, P-04, P-09, F-WIN-09).

#include "QtTestApp.h"
#include "core/ImageProvider.h"

#include <QList>
#include <QMutex>
#include <QSet>
#include <QWaitCondition>
#include <gtest/gtest.h>

using namespace lf;
using lf::test::waitUntil;

namespace {

// Decoder that accepts every path, produces blank images of the requested size and
// records every decode. Can block decodes on a gate and fail selected paths.
class FakeDecoder : public Decoder {
public:
    struct Call {
        QString path;
        int maxPx;
        bool preview;
    };

    QString name() const override { return "fake"; }
    int priority() const override { return 100; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString& path, QString* error) override {
        if (failing.contains(path)) {
            *error = "nope";
            return std::nullopt;
        }
        QMutexLocker l(&m);
        ++infoCalls;
        ImageInfo i;
        i.pixelSize = i.displaySize = fullSize;
        return i;
    }
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override {
        const bool isScreen = !req.preferEmbeddedPreview && req.maxPixelSize > 0;
        {
            QMutexLocker l(&m);
            calls.append({path, req.maxPixelSize, req.preferEmbeddedPreview});
            ++waiting;
            while (gateClosed || (isScreen && screenGateClosed))
                gate.wait(&m);
            --waiting;
        }
        QThread::msleep(ulong(req.preferEmbeddedPreview ? previewMs : screenMs));
        if (failing.contains(path) || (req.maxPixelSize == 0 && failingFull.contains(path)) ||
            (req.preferEmbeddedPreview && failingPreview.contains(path))) {
            *error = "kaputt";
            return std::nullopt;
        }
        DecodedImage d;
        d.fullSize = fullSize;
        const QSize s = req.maxPixelSize > 0
                            ? fullSize.scaled(req.maxPixelSize, req.maxPixelSize, Qt::KeepAspectRatio)
                            : fullSize;
        d.image = QImage(s.boundedTo(fullSize), QImage::Format_RGB32);
        d.image.fill(Qt::black);
        d.isPreview = d.image.size() != fullSize;
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }

    void closeGate() {
        QMutexLocker l(&m);
        gateClosed = true;
    }
    void openGate() {
        QMutexLocker l(&m);
        gateClosed = false;
        gate.wakeAll();
    }
    void closeScreenGate() {
        QMutexLocker l(&m);
        screenGateClosed = true;
    }
    void openScreenGate() {
        QMutexLocker l(&m);
        screenGateClosed = false;
        gate.wakeAll();
    }
    // Number of decode calls currently blocked on a gate.
    int blocked() {
        QMutexLocker l(&m);
        return waiting;
    }
    QList<Call> recorded() {
        QMutexLocker l(&m);
        return calls;
    }
    int count(const QString& path, bool preview) {
        int n = 0;
        for (const Call& c : recorded())
            n += (c.path == path && c.preview == preview) ? 1 : 0;
        return n;
    }
    QSet<QString> paths() {
        QSet<QString> s;
        for (const Call& c : recorded())
            s.insert(c.path);
        return s;
    }

    QSize fullSize{4000, 3000};
    int previewMs = 5;
    int screenMs = 30;
    QSet<QString> failing;        // set before use
    QSet<QString> failingFull;    // only full-resolution decodes fail
    QSet<QString> failingPreview; // only embedded-preview decodes fail
    int infoCalls = 0;

private:
    QMutex m;
    QWaitCondition gate;
    bool gateClosed = false;
    bool screenGateClosed = false;
    int waiting = 0;
    QList<Call> calls;
};

struct Ready {
    QString path;
    Quality quality;
    DecodedImagePtr image;
};

class ImageProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        lf::test::ensureQtApp();
        decoder = std::make_shared<FakeDecoder>();
        registry.add(decoder);
        provider = std::make_unique<ImageProvider>(registry);
        QObject::connect(
            provider.get(), &ImageProvider::imageReady,
            [this](const QString& p, Quality q, DecodedImagePtr img) { ready.append({p, q, img}); });
        QObject::connect(provider.get(), &ImageProvider::imageFailed,
                         [this](const QString& p, const QString& e) { failed.append({p, e}); });
        QObject::connect(provider.get(), &ImageProvider::stageFailed,
                         [this](const QString& p, Quality q, const QString&) { stageFailures.append({p, q}); });
    }
    // Pool size of ImageProvider (see its constructor).
    static int poolSize() { return std::max(2, QThread::idealThreadCount() - 2); }
    void TearDown() override {
        decoder->openGate();
        provider.reset();
    }

    bool hasReady(const QString& path, Quality q) const {
        return std::any_of(ready.cbegin(), ready.cend(),
                           [&](const Ready& r) { return r.path == path && r.quality == q; });
    }
    bool waitReady(const QString& path, Quality q, int ms = 3000) {
        return waitUntil([&] { return hasReady(path, q); }, ms);
    }

    DecoderRegistry registry;
    std::shared_ptr<FakeDecoder> decoder;
    std::unique_ptr<ImageProvider> provider;
    QList<Ready> ready;
    QList<QPair<QString, QString>> failed;
    QList<QPair<QString, Quality>> stageFailures;
    const QSize target{1920, 1080};
    int clientA = 0, clientB = 0;
};

} // namespace

// SPEC 7.1 staged display, P-04: preview first, then the screen-sized decode.
TEST_F(ImageProviderTest, P_04_StagesPreviewThenScreen) {
    EXPECT_EQ(provider->request("/x/a.jpg", Quality::Screen, target, &clientA), nullptr);
    ASSERT_TRUE(waitReady("/x/a.jpg", Quality::Screen));
    ASSERT_GE(ready.size(), 1);
    // Stages are monotonic: if the preview was emitted, it came first.
    if (ready.size() == 2) {
        EXPECT_EQ(ready[0].quality, Quality::Preview);
        EXPECT_LE(std::max(ready[0].image->image.width(), ready[0].image->image.height()), 512);
    }
    EXPECT_EQ(ready.last().quality, Quality::Screen);
    EXPECT_EQ(decoder->count("/x/a.jpg", true), 1);
    // Screen decode uses decode-time downsampling to the (bucketed) target size.
    const QImage& screen = ready.last().image->image;
    EXPECT_GE(std::max(screen.width(), screen.height()), 1920);
    EXPECT_LT(std::max(screen.width(), screen.height()), 4000);
}

TEST_F(ImageProviderTest, P_04_PreviewEmittedBeforeScreenWhenScreenIsSlow) {
    decoder->screenMs = 200;
    provider->request("/x/a.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitReady("/x/a.jpg", Quality::Screen));
    ASSERT_EQ(ready.size(), 2);
    EXPECT_EQ(ready[0].quality, Quality::Preview);
    EXPECT_EQ(ready[1].quality, Quality::Screen);
}

// P-03: switching to a preloaded image is served from the cache synchronously.
TEST_F(ImageProviderTest, P_03_CacheHitReturnsImmediately) {
    provider->setWanted(&clientA, {"/x/a.jpg", "/x/b.jpg"}, target, true);
    ASSERT_TRUE(waitReady("/x/b.jpg", Quality::Screen));
    ASSERT_TRUE(waitReady("/x/a.jpg", Quality::Screen));
    const int calls = int(decoder->recorded().size());
    ready.clear();

    DecodedImagePtr hit = provider->request("/x/b.jpg", Quality::Screen, target, &clientA);
    ASSERT_NE(hit, nullptr);
    EXPECT_FALSE(hit->image.isNull());
    // A preview request is satisfied by the better cached stage too.
    EXPECT_NE(provider->request("/x/b.jpg", Quality::Preview, target, &clientA), nullptr);
    lf::test::spin(100);
    EXPECT_EQ(decoder->recorded().size(), calls);
    EXPECT_TRUE(ready.isEmpty());
}

// F-WIN-09: two windows requesting the same file share one decode.
TEST_F(ImageProviderTest, F_WIN_09_DedupeAcrossClients) {
    decoder->closeGate();
    provider->request("/x/a.jpg", Quality::Screen, target, &clientA);
    provider->request("/x/a.jpg", Quality::Screen, target, &clientB);
    provider->setWanted(&clientB, {"/x/a.jpg"}, target, true);
    ASSERT_TRUE(waitUntil([&] { return decoder->blocked() == 2; }, 5000)); // preview + screen
    decoder->openGate();
    ASSERT_TRUE(waitReady("/x/a.jpg", Quality::Screen));
    lf::test::spin(50);
    EXPECT_EQ(decoder->count("/x/a.jpg", true), 1);
    EXPECT_EQ(decoder->count("/x/a.jpg", false), 1);
    EXPECT_NE(provider->request("/x/a.jpg", Quality::Screen, target, &clientB), nullptr);
}

// SPEC 7.1 "Abbruch": jobs that are no longer wanted never start.
TEST_F(ImageProviderTest, CancelsJobsNoLongerWanted) {
    decoder->closeGate();
    QStringList far;
    for (int i = 0; i < 60; ++i)
        far << QStringLiteral("/old/%1.jpg").arg(i);
    provider->setWanted(&clientA, far, target, true);
    // Wait until every pool thread blocks in the decoder: no further job can be picked.
    ASSERT_TRUE(waitUntil([&] { return decoder->blocked() == poolSize(); }, 5000));
    const QSet<QString> startedBefore = decoder->paths();
    ASSERT_FALSE(startedBefore.isEmpty());
    ASSERT_LT(startedBefore.size(), far.size());

    const QStringList now = {"/new/a.jpg", "/new/b.jpg"};
    provider->setWanted(&clientA, now, target, true);
    decoder->openGate();
    ASSERT_TRUE(waitReady("/new/a.jpg", Quality::Screen));
    ASSERT_TRUE(waitReady("/new/b.jpg", Quality::Screen));
    lf::test::spin(100);

    for (const QString& p : decoder->paths()) {
        if (p.startsWith("/old/"))
            EXPECT_TRUE(startedBefore.contains(p)) << "stale job ran: " << p.toStdString();
    }
}

// The current image of the active window is decoded before prefetch neighbours.
TEST_F(ImageProviderTest, ActiveCurrentBeforeNeighbours) {
    decoder->closeGate();
    QStringList list;
    for (int i = 0; i < 40; ++i)
        list << QStringLiteral("/n/%1.jpg").arg(i);
    provider->setWanted(&clientA, list, target, true);
    ASSERT_TRUE(waitUntil([&] { return decoder->blocked() == poolSize(); }, 5000));
    // Saturated pool: switching to a new current image must still get it decoded first.
    provider->setWanted(&clientA, QStringList{"/n/new.jpg"} + list, target, true);
    const int before = int(decoder->recorded().size());
    decoder->openGate();
    ASSERT_TRUE(waitReady("/n/new.jpg", Quality::Screen, 5000));
    // Picks happen in priority order; recording may interleave by at most the pool size.
    const auto calls = decoder->recorded();
    const int window = poolSize();
    int first = -1;
    for (int i = before; i < calls.size() && first < 0; ++i)
        if (calls[i].path == "/n/new.jpg")
            first = i;
    ASSERT_GE(first, 0);
    EXPECT_LT(first, before + window);
}

// P-09: memory budget evicts least recently used images, but never wanted ones.
TEST_F(ImageProviderTest, P_09_BudgetEvictsButKeepsWanted) {
    provider->setMemoryBudget(1); // everything over budget
    provider->setWanted(&clientA, {"/m/a.jpg", "/m/b.jpg"}, target, true);
    ASSERT_TRUE(waitReady("/m/a.jpg", Quality::Screen));
    ASSERT_TRUE(waitReady("/m/b.jpg", Quality::Screen));
    lf::test::spin(50);
    const qint64 two = provider->memoryUsage();
    EXPECT_GT(two, 1); // wanted images stay despite the budget
    EXPECT_NE(provider->request("/m/a.jpg", Quality::Screen, target, &clientA), nullptr);

    provider->setWanted(&clientA, {"/m/c.jpg"}, target, true);
    ASSERT_TRUE(waitReady("/m/c.jpg", Quality::Screen));
    lf::test::spin(50);
    EXPECT_LT(provider->memoryUsage(), two);
    // c is kept, a and b were evicted.
    EXPECT_NE(provider->request("/m/c.jpg", Quality::Screen, target, &clientA), nullptr);
    provider->removeClient(&clientA);
    EXPECT_EQ(provider->memoryUsage(), 0);

    // With a generous budget, unwanted images stay cached (LRU).
    provider->setMemoryBudget(qint64(1) << 30);
    provider->setWanted(&clientA, {"/m/d.jpg"}, target, true);
    ASSERT_TRUE(waitReady("/m/d.jpg", Quality::Screen));
    provider->setWanted(&clientA, {"/m/e.jpg"}, target, true);
    ASSERT_TRUE(waitReady("/m/e.jpg", Quality::Screen));
    EXPECT_NE(provider->request("/m/d.jpg", Quality::Screen, target, &clientB), nullptr);
}

// F-WIN-09: hidden/inactive windows only keep their current image loaded.
TEST_F(ImageProviderTest, F_WIN_09_InactiveClientOnlyCurrent) {
    provider->setWanted(&clientB, {"/i/a.jpg", "/i/b.jpg", "/i/c.jpg"}, target, false);
    ASSERT_TRUE(waitReady("/i/a.jpg", Quality::Screen));
    lf::test::spin(150);
    EXPECT_EQ(decoder->paths(), QSet<QString>{"/i/a.jpg"});

    // Becoming active prefetches the neighbours.
    provider->setWanted(&clientB, {"/i/a.jpg", "/i/b.jpg", "/i/c.jpg"}, target, true);
    EXPECT_TRUE(waitReady("/i/b.jpg", Quality::Screen));
    EXPECT_TRUE(waitReady("/i/c.jpg", Quality::Screen));
}

TEST_F(ImageProviderTest, InvalidateDropsCache) {
    provider->request("/v/a.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitReady("/v/a.jpg", Quality::Screen));
    ASSERT_TRUE(provider->info("/v/a.jpg"));
    ASSERT_TRUE(provider->info("/v/a.jpg"));
    EXPECT_EQ(decoder->infoCalls, 1); // info is cached
    const int screens = decoder->count("/v/a.jpg", false);

    ready.clear();
    provider->invalidate("/v/a.jpg");
    EXPECT_EQ(provider->memoryUsage(), 0);
    // Still wanted by the client: reloaded.
    ASSERT_TRUE(waitReady("/v/a.jpg", Quality::Screen));
    EXPECT_EQ(decoder->count("/v/a.jpg", false), screens + 1);
    ASSERT_TRUE(provider->info("/v/a.jpg"));
    EXPECT_EQ(decoder->infoCalls, 2);
}

TEST_F(ImageProviderTest, InvalidateWhileDecodingDiscardsStaleResult) {
    decoder->closeGate();
    provider->request("/v/b.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitUntil([&] { return decoder->blocked() == 2; }, 5000)); // both stages running
    provider->invalidate("/v/b.jpg");
    decoder->openGate();
    ASSERT_TRUE(waitReady("/v/b.jpg", Quality::Screen));
    // The pre-invalidation decode was dropped and the file decoded again.
    EXPECT_EQ(decoder->count("/v/b.jpg", false), 2);
}

TEST_F(ImageProviderTest, FailureEmitsImageFailed) {
    decoder->failing.insert("/f/bad.jpg");
    provider->request("/f/bad.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitUntil([&] { return !failed.isEmpty(); }));
    EXPECT_EQ(failed[0].first, "/f/bad.jpg");
    EXPECT_TRUE(failed[0].second.contains("kaputt"));
    lf::test::spin(100);
    EXPECT_EQ(failed.size(), 1); // reported once, not retried endlessly
    // A later request reports the known failure again.
    provider->request("/f/bad.jpg", Quality::Screen, target, &clientB);
    ASSERT_TRUE(waitUntil([&] { return failed.size() == 2; }));
}

TEST_F(ImageProviderTest, FullOnlyOnExplicitRequest) {
    provider->request("/q/a.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitReady("/q/a.jpg", Quality::Screen));
    lf::test::spin(50);
    for (const auto& c : decoder->recorded())
        EXPECT_NE(c.maxPx, 0) << "unexpected full-resolution decode";
    provider->request("/q/a.jpg", Quality::Full, target, &clientA);
    ASSERT_TRUE(waitReady("/q/a.jpg", Quality::Full));
    EXPECT_EQ(ready.last().image->image.size(), QSize(4000, 3000));
}

// Review finding: a failed Full decode (e.g. a huge image) marked the whole file failed, so
// request() returned nullptr although Screen was cached and the view showed an error card.
TEST_F(ImageProviderTest, ReviewFailedFullKeepsScreen) {
    decoder->failingFull.insert("/s/huge.jpg");
    provider->request("/s/huge.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitReady("/s/huge.jpg", Quality::Screen));
    provider->request("/s/huge.jpg", Quality::Full, target, &clientA);
    ASSERT_TRUE(waitUntil([&] { return !stageFailures.isEmpty(); }));
    EXPECT_EQ(stageFailures[0].first, "/s/huge.jpg");
    EXPECT_EQ(stageFailures[0].second, Quality::Full);
    EXPECT_TRUE(failed.isEmpty());
    EXPECT_NE(provider->request("/s/huge.jpg", Quality::Screen, target, &clientA), nullptr);
    lf::test::spin(100);
    EXPECT_TRUE(failed.isEmpty());
    EXPECT_EQ(decoder->count("/s/huge.jpg", false), 2); // screen + one full attempt, no retries
}

// A failing embedded preview must not hide the screen stage that decodes fine.
TEST_F(ImageProviderTest, ReviewFailedPreviewStillShowsScreen) {
    decoder->failingPreview.insert("/s/nopreview.jpg");
    provider->request("/s/nopreview.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitReady("/s/nopreview.jpg", Quality::Screen));
    EXPECT_TRUE(failed.isEmpty());
    EXPECT_NE(provider->request("/s/nopreview.jpg", Quality::Screen, target, &clientA), nullptr);
}

// Review finding: a Screen result finishing after Full was still inserted and emitted, so
// the view downgraded from full resolution to the screen stage.
TEST_F(ImageProviderTest, ReviewLateScreenAfterFullIsDropped) {
    decoder->closeScreenGate();
    provider->request("/s/late.jpg", Quality::Screen, target, &clientA);
    ASSERT_TRUE(waitUntil([&] { return decoder->count("/s/late.jpg", false) == 1 && decoder->blocked() == 1; }));
    provider->request("/s/late.jpg", Quality::Full, target, &clientA);
    ASSERT_TRUE(waitReady("/s/late.jpg", Quality::Full));
    const int readyBefore = int(ready.size());
    decoder->openScreenGate();
    ASSERT_TRUE(waitUntil([&] { return decoder->blocked() == 0; }));
    lf::test::spin(100); // let the late screen result arrive on the GUI thread
    for (int i = readyBefore; i < ready.size(); ++i)
        EXPECT_NE(ready[i].quality, Quality::Screen) << "late screen stage emitted after full";
    DecodedImagePtr img = provider->request("/s/late.jpg", Quality::Screen, target, &clientA);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->image.size(), QSize(4000, 3000)); // still the full image
}

// Review finding: info cache, failure and generation tables grew without bound.
TEST_F(ImageProviderTest, ReviewBookkeepingStaysBounded) {
    decoder->previewMs = 0;
    decoder->screenMs = 0;
    for (int i = 0; i < 5000; ++i)
        ASSERT_TRUE(provider->info(QStringLiteral("/b/info%1.jpg").arg(i)));
    const int calls = decoder->infoCalls;
    ASSERT_TRUE(provider->info("/b/info0.jpg")); // oldest entry was evicted
    EXPECT_EQ(decoder->infoCalls, calls + 1);

    for (int i = 0; i < 700; ++i) {
        const QString p = QStringLiteral("/b/bad%1.jpg").arg(i);
        decoder->failing.insert(p);
        const int before = int(failed.size());
        provider->setWanted(&clientA, {p}, target, true);
        ASSERT_TRUE(waitUntil([&] { return failed.size() > before; }));
        provider->invalidate(QStringLiteral("/b/gone%1.jpg").arg(i)); // generation entries
    }
    provider->setWanted(&clientA, {"/b/ok.jpg"}, target, true);
    ASSERT_TRUE(waitReady("/b/ok.jpg", Quality::Screen));
    EXPECT_LE(provider->bookkeepingEntries(), 2048 + 3 * 512 + 8);
}
