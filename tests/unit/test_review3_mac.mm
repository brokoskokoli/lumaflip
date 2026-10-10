// SPDX-License-Identifier: GPL-3.0-or-later
// macOS parts of the third review round: the default app registration reports a slow
// system answer as "still running" and delivers the final result (F-MAC-04), collects
// concurrent errors safely; huge deep images stay within the decode byte cap (P-08);
// the launch decode is adopted by the image provider (P-01).

#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/ImageProvider.h"
#include "platform/mac/ImageIODecoder.h"
#include "platform/mac/MacDefaultApp.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <gtest/gtest.h>

#import <Foundation/Foundation.h>
#include <atomic>
#include <dispatch/dispatch.h>

using namespace lf;
using lf::test::waitUntil;

namespace {

// ---- a huge 16-bit TIFF that costs a few KB: every strip points to the same data ----
// (ImageIO rejects compressed tiles of this kind, compressed strips are fine.)

void put16(QByteArray& b, quint16 v) {
    char c[2];
    qToLittleEndian(v, c);
    b.append(c, 2);
}
void put32(QByteArray& b, quint32 v) {
    char c[4];
    qToLittleEndian(v, c);
    b.append(c, 4);
}

// Writes an RGB, 16 bits per sample, deflate-compressed TIFF of w x h pixels (mid gray).
bool writeHugeTiff(const QString& path, quint32 w, quint32 h) {
    constexpr quint32 rowsPerStrip = 64;
    const quint32 tiles = (h + rowsPerStrip - 1) / rowsPerStrip; // strips
    const QByteArray raw(int(w * rowsPerStrip * 3 * 2), char(0x80));
    QByteArray zlib = qCompress(raw, 9).mid(4); // qCompress prefixes the length
    struct Entry {
        quint16 tag, type;
        quint32 count, value;
    };
    // Layout: header | bits-per-sample | strip data | offsets | byte counts | IFD
    QByteArray out;
    out.append("II*\0", 4);
    put32(out, 0); // IFD offset, patched below
    const quint32 bpsAt = quint32(out.size());
    put16(out, 16);
    put16(out, 16);
    put16(out, 16);
    const quint32 dataAt = quint32(out.size());
    out.append(zlib);
    if (out.size() % 2)
        out.append('\0'); // offsets are word aligned
    const quint32 offsetsAt = quint32(out.size());
    for (quint32 i = 0; i < tiles; ++i)
        put32(out, dataAt);
    const quint32 countsAt = quint32(out.size());
    for (quint32 i = 0; i < tiles; ++i)
        put32(out, quint32(zlib.size()));
    const quint32 ifdAt = quint32(out.size());
    const Entry entries[] = {
        {256, 4, 1, w},
        {257, 4, 1, h},
        {258, 3, 3, bpsAt},
        {259, 3, 1, 8},
        {262, 3, 1, 2},
        {273, 4, tiles, offsetsAt},
        {277, 3, 1, 3},
        {278, 4, 1, rowsPerStrip},
        {279, 4, tiles, countsAt},
        {284, 3, 1, 1},
    };
    put16(out, quint16(std::size(entries)));
    for (const Entry& e : entries) {
        put16(out, e.tag);
        put16(out, e.type);
        put32(out, e.count);
        if (e.type == 3 && e.count == 1) {
            put16(out, quint16(e.value));
            put16(out, 0);
        } else {
            put32(out, e.value);
        }
    }
    put32(out, 0); // no next IFD
    qToLittleEndian(ifdAt, out.data() + 4);
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
}

} // namespace

// ---- P-08 ----

TEST(Review3Mac, P_08_HugeDeepImageDecodedWithinByteCap) {
    test::ensureQtApp();
    QTemporaryDir dir;
    const QString path = dir.filePath("huge16.tif");
    ASSERT_TRUE(writeHugeTiff(path, 30000, 20000));
    ImageIODecoder decoder;
    QString error;
    const auto info = decoder.info(path, &error);
    ASSERT_TRUE(info) << error.toStdString();
    ASSERT_EQ(info->pixelSize, QSize(30000, 20000));
    EXPECT_EQ(info->bitsPerComponent, 16);
    // Full resolution would be 30000 x 20000 x 8 bytes = 4.8 GB. The cap is half the
    // memory budget, at most 1 GB (ImageProvider, P_08_DecodeByteCapFollowsBudget); a
    // smaller one keeps this test light.
    for (const qint64 cap : {qint64(256) << 20, qint64(48) << 20}) {
        DecodeRequest req;
        req.maxBytes = cap;
        const auto img = decoder.decode(path, req, &error);
        ASSERT_TRUE(img) << error.toStdString();
        EXPECT_LE(img->image.sizeInBytes(), cap);
        EXPECT_GE(img->image.sizeInBytes(), cap * 3 / 4); // downsampled to the cap, not further
        EXPECT_EQ(img->fullSize, QSize(30000, 20000));
        EXPECT_FALSE(img->isPreview); // the best this backend delivers
    }
}

TEST(Review3Mac, P_08_ProviderFullStageStaysWithinBudget) {
    test::ensureQtApp();
    QTemporaryDir dir;
    const QString path = QDir::cleanPath(dir.filePath("huge16.tif"));
    ASSERT_TRUE(writeHugeTiff(path, 30000, 20000));
    DecoderRegistry registry;
    registry.add(std::make_shared<ImageIODecoder>());
    ImageProvider provider(registry);
    const qint64 budget = qint64(512) << 20;
    provider.setMemoryBudget(budget);
    DecodedImagePtr full;
    QObject::connect(&provider, &ImageProvider::imageReady,
                     [&](const QString&, Quality q, const DecodedImagePtr& i) {
                         if (q == Quality::Full)
                             full = i;
                     });
    int client = 0;
    provider.request(path, Quality::Full, {}, &client);
    ASSERT_TRUE(waitUntil([&] { return full != nullptr; }, 60000));
    EXPECT_LE(full->image.sizeInBytes(), budget / 2);
    EXPECT_LE(provider.memoryUsage(), budget);
}

// ---- F-MAC-04 ----

TEST(Review3Mac, F_MAC_04_SlowRegistrationReportsPendingThenResult) {
    test::ensureQtApp();
    QObject context;
    QStringList events;
    bool ok = false;
    auto setter = [](const QString&, std::function<void(const QString&)> completion) {
        // The system answers after the user confirmed its dialog (here: 400 ms).
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * int64_t(NSEC_PER_MSEC)),
                       dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
                         completion(QString());
                       });
    };
    mac::registerDefaultApp(
        {"public.jpeg", "public.png"}, [](const QString&) { return true; }, setter, 50, &context,
        [&](bool success, const QString&) {
            ok = success;
            events << "done";
        },
        [&](const QString& message) {
            EXPECT_EQ(message, mac::defaultAppPendingMessage());
            events << "pending";
        });
    ASSERT_TRUE(waitUntil([&] { return events.contains("done"); }, 5000));
    EXPECT_EQ(events, (QStringList{"pending", "done"})); // a note first, never a failure
    EXPECT_TRUE(ok);
}

TEST(Review3Mac, F_MAC_04_FastRegistrationHasNoPendingNote) {
    test::ensureQtApp();
    QObject context;
    QStringList events;
    auto setter = [](const QString&, std::function<void(const QString&)> completion) {
        completion(QString());
    };
    mac::registerDefaultApp(
        {"public.jpeg"}, [](const QString&) { return true; }, setter, 300, &context,
        [&](bool, const QString&) { events << "done"; }, [&](const QString&) { events << "pending"; });
    ASSERT_TRUE(waitUntil([&] { return events.contains("done"); }, 5000));
    lf::test::spin(500);
    EXPECT_EQ(events, QStringList{"done"});
}

TEST(Review3Mac, F_MAC_04_ConcurrentCompletionsCollectAllErrors) {
    test::ensureQtApp();
    QObject context;
    QStringList types;
    for (int i = 0; i < 200; ++i)
        types << QStringLiteral("type.%1").arg(i);
    bool finished = false, ok = true;
    QString errors;
    // Every second type fails; completions come from many threads at once; every fifth
    // type is unknown (reported without calling the system).
    auto setter = [](const QString& t, std::function<void(const QString&)> completion) {
        const int n = t.section('.', 1).toInt();
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
          completion(n % 2 ? QStringLiteral("abgelehnt") : QString());
        });
    };
    auto known = [](const QString& t) { return t.section('.', 1).toInt() % 5 != 0; };
    mac::registerDefaultApp(types, known, setter, 10000, &context,
                            [&](bool success, const QString& e) {
                                finished = true;
                                ok = success;
                                errors = e;
                            },
                            {});
    ASSERT_TRUE(waitUntil([&] { return finished; }, 5000));
    EXPECT_FALSE(ok);
    int unknown = 0, rejected = 0;
    for (int n = 0; n < 200; ++n) {
        if (n % 5 == 0)
            ++unknown;
        else if (n % 2)
            ++rejected;
    }
    EXPECT_EQ(errors.count(QStringLiteral("unbekannter Typ")), unknown);
    EXPECT_EQ(errors.count(QStringLiteral("abgelehnt")), rejected);
}

// ---- P-01: the decode started at launch is adopted ----

namespace {
class CountingDecoder : public Decoder {
public:
    QString name() const override { return QStringLiteral("count"); }
    int priority() const override { return 100; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString&, const DecodeRequest& req, QString*) override {
        if (!req.preferEmbeddedPreview)
            ++screenOrFull;
        DecodedImage d;
        d.fullSize = QSize(3000, 2000);
        d.image = QImage(QSize(100, 67), QImage::Format_RGB32);
        d.image.fill(Qt::black);
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }
    std::atomic<int> screenOrFull{0};
};
} // namespace

TEST(Review3Mac, P_01_PrestartedDecodeIsAdopted) {
    test::ensureQtApp();
    QTemporaryDir dir;
    const QString path = QDir::cleanPath(dir.filePath("start.jpg"));
    QImage img(QSize(3000, 2000), QImage::Format_RGB32);
    img.fill(Qt::darkCyan);
    ASSERT_TRUE(img.save(path, "JPEG"));
    const QSize target(1200, 800);
    ImageProvider::prestart(path, target); // as main() does before the application exists
    auto* dec = new CountingDecoder;
    DecoderRegistry registry;
    registry.add(std::shared_ptr<Decoder>(dec));
    ImageProvider provider(registry);
    int client = 0;
    DecodedImagePtr shown = provider.request(path, Quality::Screen, target, &client);
    if (!shown) { // still decoding: it arrives like any other stage
        QObject::connect(&provider, &ImageProvider::imageReady,
                         [&](const QString& p, Quality q, const DecodedImagePtr& i) {
                             if (p == path && q == Quality::Screen)
                                 shown = i;
                         });
        ASSERT_TRUE(waitUntil([&] { return shown != nullptr; }, 5000));
    }
    EXPECT_EQ(shown->fullSize, QSize(3000, 2000));
    EXPECT_LE(std::max(shown->image.width(), shown->image.height()), 1280);
    EXPECT_EQ(shown->image.pixelColor(10, 10).rgb() & 0xff000000, 0xff000000u);
    lf::test::spin(100);
    EXPECT_EQ(dec->screenOrFull.load(), 0); // not decoded a second time
}
