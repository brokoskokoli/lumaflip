// SPDX-License-Identifier: GPL-3.0-or-later
// M2 media in the core: animation frames through FrameReader (F-FMT-03) and pages of
// multi-page files, decoded and cached per page by the ImageProvider (F-FMT-04).

#include "../integration/GifTestUtil.h"
#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/ImageProvider.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace lf;
using namespace lf::test;

namespace {

QJsonObject expected(const QString& name) {
    return expectedCorpus().value(QStringLiteral("images")).toObject().value(name).toObject();
}

Rgba centerColor(const QImage& img) {
    const QColor c = img.pixelColor(img.width() / 2, img.height() / 2);
    return {c.red(), c.green(), c.blue(), c.alpha()};
}

void expectColor(const Rgba& got, const Rgba& want, const char* what) {
    for (size_t k = 0; k < 3; ++k)
        EXPECT_NEAR(got[k], want[k], 40) << what << " channel " << k;
}

} // namespace

// Frames, durations and pixels of every animated corpus file through one open reader.
TEST(AnimationFrames, F_FMT_03_FrameReaderTimingAndPixels) {
    LF_REQUIRE_CORPUS();
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "frame access needs the platform decoder";
#endif
    int checked = 0;
    for (const char* name : {"anim.gif", "anim.png", "anim.heics", "anim.webp"}) {
        SCOPED_TRACE(name);
        const QString path = corpusFile(QString::fromLatin1(name));
        const QJsonObject exp = expected(QString::fromLatin1(name));
        if (!QFileInfo::exists(path)) {
            ASSERT_TRUE(exp.value(QStringLiteral("optional")).toBool()) << "missing required " << name;
            continue;
        }
        auto reader = DecoderRegistry::instance().openFrames(path);
        ASSERT_TRUE(reader);
        ASSERT_EQ(reader->frameCount(), exp.value(QStringLiteral("frames")).toInt());
        const QJsonArray durations = exp.value(QStringLiteral("durations")).toArray();
        const QJsonArray colors = exp.value(QStringLiteral("frameColors")).toArray();
        for (int i = 0; i < reader->frameCount(); ++i) {
            QString error;
            auto frame = reader->readFrame(i, DecodeRequest{}, &error);
            ASSERT_TRUE(frame) << error.toStdString();
            EXPECT_EQ(frame->frameCount, reader->frameCount());
            EXPECT_EQ(frame->frame, i);
            EXPECT_EQ(frame->image.size(), jsonSize(exp.value(QStringLiteral("displaySize"))));
            EXPECT_EQ(frame->frameDurationMs, durations.at(i).toInt()) << "frame " << i;
            expectColor(centerColor(frame->image), namedColor(colors.at(i).toString().at(0)), "frame color");
        }
        // Random access backwards works too (stepping back with ',').
        QString error;
        auto first = reader->readFrame(0, DecodeRequest{}, &error);
        ASSERT_TRUE(first);
        expectColor(centerColor(first->image), namedColor(colors.at(0).toString().at(0)), "frame 0 again");
        ++checked;
    }
    EXPECT_GE(checked, 1);
}

// A plain decode reports the frame count, so the window knows the file is animated.
TEST(AnimationFrames, F_FMT_03_DecodeReportsFrameCount) {
    LF_REQUIRE_CORPUS();
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "frame counts come from the platform decoder";
#endif
    QString error;
    auto img = DecoderRegistry::instance().decode(corpusFile("anim.gif"), DecodeRequest{}, &error);
    ASSERT_TRUE(img);
    EXPECT_EQ(img->frameCount, 3);
    EXPECT_EQ(img->pageCount, 1);
    auto still = DecoderRegistry::instance().decode(corpusFile("orient_1.jpg"), DecodeRequest{}, &error);
    ASSERT_TRUE(still);
    EXPECT_EQ(still->frameCount, 1);
    EXPECT_EQ(still->pageCount, 1);
}

// B-2: the frame source of a generated GIF (every platform, no corpus needed): the frames
// differ, carry their delay, and the decode reports the file's loop count as the number of
// plays (NETSCAPE loop count n = n + 1 plays, no loop extension = once, 0 = endlessly).
TEST(AnimationFrames, F_FMT_03_GeneratedGifFramesAndLoopCount) {
    ensureQtApp();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const struct {
        int netscape;
        int plays;
    } cases[] = {{-1, 1}, {0, 0}, {1, 2}, {2, 3}};
    for (const auto& c : cases) {
        SCOPED_TRACE(c.netscape);
        const QString path = dir.filePath(QStringLiteral("loop%1.gif").arg(c.netscape + 1));
        ASSERT_TRUE(writeAnimatedGif(path, QSize(24, 16), 4, 5, c.netscape));
        QString error;
        auto img = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error);
        ASSERT_TRUE(img) << error.toStdString();
        EXPECT_EQ(img->frameCount, 4);
        EXPECT_EQ(img->loopCount, c.plays);
        EXPECT_EQ(img->frameDurationMs, 50);
    }
    auto reader = DecoderRegistry::instance().openFrames(dir.filePath(QStringLiteral("loop1.gif")));
    ASSERT_TRUE(reader);
    ASSERT_EQ(reader->frameCount(), 4);
    for (int i : {0, 1, 2, 3, 1}) { // in order, then back (stepping with ',')
        SCOPED_TRACE(i);
        QString error;
        auto frame = reader->readFrame(i, DecodeRequest{}, &error);
        ASSERT_TRUE(frame) << error.toStdString();
        EXPECT_EQ(frame->frame, i);
        EXPECT_EQ(frame->frameDurationMs, 50);
        EXPECT_EQ(frame->loopCount, 0);
        const QColor want = gifPaletteColor(i);
        expectColor(centerColor(frame->image), {want.red(), want.green(), want.blue(), 255}, "frame color");
    }
    // Downscaled frames (the size of a fitted first frame) are still the right frames.
    DecodeRequest small;
    small.maxPixelSize = 12;
    QString error;
    auto frame = reader->readFrame(2, small, &error);
    ASSERT_TRUE(frame) << error.toStdString();
    EXPECT_EQ(frame->image.size(), QSize(12, 8));
    const QColor want = gifPaletteColor(2);
    expectColor(centerColor(frame->image), {want.red(), want.green(), want.blue(), 255}, "small frame color");
}

// The corpus animations loop endlessly (written with loop count 0).
TEST(AnimationFrames, F_FMT_03_CorpusLoopCount) {
    LF_REQUIRE_CORPUS();
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "animated WebP/APNG need the platform decoder";
#endif
    for (const char* name : {"anim.gif", "anim.png", "anim.webp"}) {
        SCOPED_TRACE(name);
        const QString path = corpusFile(QString::fromLatin1(name));
        if (!QFileInfo::exists(path))
            continue; // optional (WebP needs img2webp)
        QString error;
        auto img = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error);
        ASSERT_TRUE(img) << error.toStdString();
        EXPECT_EQ(img->frameCount, 3);
        EXPECT_EQ(img->loopCount, 0);
    }
}

TEST(MultiPage, F_FMT_04_DecodePagesTiffAndIco) {
    LF_REQUIRE_CORPUS();
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "pages come from the platform decoder";
#endif
    const QJsonArray sizes = expected("multipage.tiff").value(QStringLiteral("pageSizes")).toArray();
    ASSERT_EQ(sizes.size(), 3);
    for (int p = 0; p < 3; ++p) {
        DecodeRequest req;
        req.page = p;
        QString error;
        auto img = DecoderRegistry::instance().decode(corpusFile("multipage.tiff"), req, &error);
        ASSERT_TRUE(img) << error.toStdString();
        EXPECT_EQ(img->pageCount, 3);
        EXPECT_EQ(img->page, p);
        EXPECT_EQ(img->image.size(), jsonSize(sizes.at(p))) << "page " << p;
    }
    DecodeRequest beyond;
    beyond.page = 3;
    QString error;
    EXPECT_FALSE(DecoderRegistry::instance().decode(corpusFile("multipage.tiff"), beyond, &error));
    EXPECT_FALSE(error.isEmpty());

    auto ico = DecoderRegistry::instance().decode(corpusFile("icon.ico"), DecodeRequest{}, &error);
    ASSERT_TRUE(ico);
    EXPECT_EQ(ico->pageCount, expected("icon.ico").value(QStringLiteral("pages")).toInt());
}

// The provider caches pages separately (cache key includes the page) and delivers each
// page tagged with its number; invalidate() drops every page of the file.
TEST(MultiPage, F_FMT_04_ProviderCachesPerPage) {
    LF_REQUIRE_CORPUS();
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "pages come from the platform decoder";
#endif
    ensureQtApp();
    ImageProvider provider(DecoderRegistry::instance());
    const QString path = corpusFile("multipage.tiff");
    int client = 0;
    QList<int> pagesSeen;
    QObject::connect(&provider, &ImageProvider::imageReady, &provider,
                     [&](const QString& p, Quality q, const DecodedImagePtr& img) {
                         Q_UNUSED(q); // a complete preview stage may already satisfy Screen
                         if (p == path)
                             pagesSeen << img->page;
                     });
    const QSize target(1024, 768);
    provider.request(path, Quality::Screen, target, &client, 0, 1);
    ASSERT_TRUE(waitUntil([&] { return pagesSeen.contains(1); }, 5000));
    provider.request(path, Quality::Screen, target, &client, 0, 2);
    ASSERT_TRUE(waitUntil([&] { return pagesSeen.contains(2); }, 5000));
    provider.request(path, Quality::Screen, target, &client, 0, 0);
    ASSERT_TRUE(waitUntil([&] { return pagesSeen.contains(0); }, 5000));

    auto p0 = provider.cached(path, Quality::Screen, target, 0);
    auto p1 = provider.cached(path, Quality::Screen, target, 1);
    auto p2 = provider.cached(path, Quality::Screen, target, 2);
    ASSERT_TRUE(p0 && p1 && p2);
    EXPECT_EQ(p0->image.size(), QSize(64, 48));
    EXPECT_EQ(p1->image.size(), QSize(32, 24));
    EXPECT_EQ(p2->image.size(), QSize(16, 12));
    EXPECT_EQ(p1->page, 1);
    EXPECT_NE(p0.get(), p1.get());
    // Asking for page 1 again is a cache hit, no decode.
    const int before = int(pagesSeen.size());
    EXPECT_EQ(provider.request(path, Quality::Screen, target, &client, 0, 1).get(), p1.get());
    spin(50);
    EXPECT_EQ(int(pagesSeen.size()), before);

    provider.removeClient(&client);
    provider.invalidate(path);
    EXPECT_FALSE(provider.cached(path, Quality::Preview, {}, 0));
    EXPECT_FALSE(provider.cached(path, Quality::Preview, {}, 1));
    EXPECT_FALSE(provider.cached(path, Quality::Preview, {}, 2));
}
