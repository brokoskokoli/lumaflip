// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder helper process (SPEC 8.5, M3): protocol, shared memory, crash recovery,
// timeouts, sandbox. Starts the real `lumaflip-decoder` (no windows).
// Requirements: F-FMT-02 (a crashing decoder gives the file an error card).

#include "CorpusTestUtil.h"
#include "core/DecoderHelper.h"
#include "core/DecoderIpc.h"
#include "core/QtImageDecoder.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <thread>
#include <vector>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace lf;
using namespace lf::test;

namespace {

Metadata sampleMetadata() {
    Metadata m;
    addMetadata(m, QStringLiteral("camera.model"), QStringLiteral("Testkamera"));
    addMetadata(m, QStringLiteral("shot.iso"), 400);
    m.captured = QDateTime(QDate(2024, 5, 6), QTime(7, 8, 9), QTimeZone::UTC);
    m.latitude = 48.1;
    m.rating = 3;
    return m;
}

} // namespace

TEST(DecoderIpc, ProtocolRoundTrip) {
    ipc::Request rq;
    rq.id = 42;
    rq.op = ipc::Op::Decode;
    rq.backend = QStringLiteral("QtSvg");
    rq.path = QStringLiteral("/tmp/ä ö/bild.svg");
    rq.decode.page = 2;
    rq.decode.maxPixelSize = 1234;
    rq.decode.maxBytes = 1ll << 33;
    rq.decode.hdr = true;
    const auto rq2 = ipc::decodeRequest(ipc::encode(rq));
    ASSERT_TRUE(rq2.has_value());
    EXPECT_EQ(rq2->id, 42u);
    EXPECT_EQ(rq2->op, ipc::Op::Decode);
    EXPECT_EQ(rq2->backend, rq.backend);
    EXPECT_EQ(rq2->path, rq.path);
    EXPECT_EQ(rq2->decode.page, 2);
    EXPECT_EQ(rq2->decode.maxPixelSize, 1234);
    EXPECT_EQ(rq2->decode.maxBytes, 1ll << 33);
    EXPECT_TRUE(rq2->decode.hdr);

    ipc::Response rs;
    rs.id = 42;
    rs.ok = true;
    rs.error = QStringLiteral("x");
    rs.sandboxed = true;
    rs.pid = 777;
    rs.info.pixelSize = QSize(10, 20);
    rs.info.formatName = QStringLiteral("SVG");
    rs.info.isHdr = true;
    rs.decoded.fullSize = QSize(100, 200);
    rs.decoded.headroom = 2.5f;
    rs.decoded.pageCount = 3;
    rs.pixels.width = 10;
    rs.pixels.height = 20;
    rs.pixels.format = int(QImage::Format_RGBA8888_Premultiplied);
    rs.pixels.bytesPerLine = 40;
    rs.pixels.shmSize = 800;
    rs.pixels.iccProfile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    rs.metadata = sampleMetadata();
    const auto rs2 = ipc::decodeResponse(ipc::encode(rs));
    ASSERT_TRUE(rs2.has_value());
    EXPECT_EQ(rs2->id, 42u);
    EXPECT_TRUE(rs2->ok);
    EXPECT_TRUE(rs2->sandboxed);
    EXPECT_EQ(rs2->pid, 777);
    EXPECT_EQ(rs2->info.pixelSize, QSize(10, 20));
    EXPECT_EQ(rs2->info.formatName, QStringLiteral("SVG"));
    EXPECT_TRUE(rs2->info.isHdr);
    EXPECT_EQ(rs2->decoded.fullSize, QSize(100, 200));
    EXPECT_FLOAT_EQ(rs2->decoded.headroom, 2.5f);
    EXPECT_EQ(rs2->decoded.pageCount, 3);
    EXPECT_EQ(rs2->pixels.bytesPerLine, 40);
    EXPECT_EQ(rs2->pixels.iccProfile, rs.pixels.iccProfile);
    EXPECT_EQ(rs2->metadata.entries.size(), 2);
    EXPECT_EQ(rs2->metadata.entries.at(0).value.toString(), QStringLiteral("Testkamera"));
    EXPECT_EQ(rs2->metadata.groups, rs.metadata.groups);
    EXPECT_EQ(rs2->metadata.captured, rs.metadata.captured);
    EXPECT_EQ(rs2->metadata.latitude, 48.1);
    EXPECT_FALSE(rs2->metadata.longitude.has_value());
    EXPECT_EQ(rs2->metadata.rating, 3);

    // Garbage, truncated data and a request parsed as response are rejected.
    EXPECT_FALSE(ipc::decodeRequest(QByteArray("garbage")).has_value());
    EXPECT_FALSE(ipc::decodeResponse(ipc::encode(rq)).has_value());
    EXPECT_FALSE(ipc::decodeResponse(ipc::encode(rs).left(30)).has_value());
}

#ifdef Q_OS_UNIX

TEST(DecoderIpc, FramingOverPipes) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    const QByteArray big(300000, 'x'); // larger than the pipe buffer: written by a thread
    std::thread writer([&] {
        EXPECT_EQ(ipc::writeFrame(fds[1], QByteArray("hello")), ipc::IoStatus::Ok);
        EXPECT_EQ(ipc::writeFrame(fds[1], big), ipc::IoStatus::Ok);
    });
    QByteArray in;
    EXPECT_EQ(ipc::readFrame(fds[0], &in, 2000), ipc::IoStatus::Ok);
    EXPECT_EQ(in, QByteArray("hello"));
    EXPECT_EQ(ipc::readFrame(fds[0], &in, 2000), ipc::IoStatus::Ok);
    EXPECT_EQ(in, big);
    writer.join();
    // Nothing arrives: timeout.
    EXPECT_EQ(ipc::readFrame(fds[0], &in, 50), ipc::IoStatus::Timeout);
    // A length beyond the limit is refused without allocating it.
    const unsigned char huge[4] = {0xFF, 0xFF, 0xFF, 0x7F};
    ASSERT_EQ(write(fds[1], huge, 4), 4);
    EXPECT_EQ(ipc::readFrame(fds[0], &in, 1000), ipc::IoStatus::TooLarge);
    // Writer gone: closed.
    close(fds[1]);
    EXPECT_EQ(ipc::readFrame(fds[0], &in, 1000), ipc::IoStatus::Closed);
    close(fds[0]);
}

TEST(DecoderIpc, SharedMemoryRoundTripAndCleanup) {
    QImage img(33, 17, QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixelColor(x, y, QColor(x * 7, y * 13, 99, 255));
    img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const QByteArray name = ipc::shmName(getpid(), 900001);
    ipc::unlinkShared(name);
    ipc::PixelHeader h;
    QString error;
    ASSERT_TRUE(ipc::writeSharedImage(name, img, &h, &error)) << error.toStdString();
    EXPECT_TRUE(ipc::sharedExists(name));
    // Exclusive: a second object with the same name is refused.
    ipc::PixelHeader h2;
    EXPECT_FALSE(ipc::writeSharedImage(name, img, &h2, &error));
    QImage back = ipc::readSharedImage(name, h, &error);
    ASSERT_FALSE(back.isNull()) << error.toStdString();
    EXPECT_FALSE(ipc::sharedExists(name)) << "the name must be gone after mapping";
    EXPECT_EQ(back.colorSpace(), img.colorSpace());
    EXPECT_EQ(back.format(), img.format());
    for (int y = 0; y < img.height(); ++y)
        EXPECT_EQ(QByteArray(reinterpret_cast<const char*>(back.constScanLine(y)), img.width() * 4),
                  QByteArray(reinterpret_cast<const char*>(img.constScanLine(y)), img.width() * 4));
    back = QImage(); // unmaps

    // A header that does not match the object is rejected - and the name still removed.
    ASSERT_TRUE(ipc::writeSharedImage(name, img, &h, &error));
    ipc::PixelHeader bad = h;
    bad.height = 1000;
    bad.shmSize = bad.bytesPerLine * bad.height;
    EXPECT_TRUE(ipc::readSharedImage(name, bad, &error).isNull());
    EXPECT_FALSE(ipc::sharedExists(name));
}

#endif

#if defined(LF_DECODER_HELPER_BINARY)

namespace {

DecoderHelperPool::Options helperOptions(int decodeTimeoutMs = 30000) {
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.maxProcesses = 2;
    o.decodeTimeoutMs = decodeTimeoutMs;
    o.extraEnvironment = {QStringLiteral("LF_DECODER_HELPER_TEST_OPS=1"), QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    return o;
}

ipc::Request op(ipc::Op o, const QString& path = {}) {
    ipc::Request r;
    r.op = o;
    r.path = path;
    return r;
}

bool processAlive(qint64 pid) {
    return pid > 0 && ::kill(pid_t(pid), 0) == 0;
}

// Local stand-in with the helper's "Qt" backend name that claims every file.
class AnyFileDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("Qt"); }
    int priority() const override { return 10; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return std::nullopt; }
    std::optional<DecodedImage> decode(const QString&, const DecodeRequest&, QString*) override {
        return std::nullopt;
    }
    Metadata metadata(const QString&) override { return {}; }
};

} // namespace

TEST(DecoderHelper, PingAndSandbox) {
    DecoderHelperPool pool(helperOptions());
    DecoderHelperPool::Failure f;
    QString error;
    const auto pong = pool.call(op(ipc::Op::Ping), 10000, &f, &error);
    ASSERT_TRUE(pong.has_value()) << error.toStdString();
    EXPECT_TRUE(pong->ok);
    EXPECT_GT(pong->pid, 0);
    EXPECT_NE(pong->pid, qint64(getpid()));
#ifdef Q_OS_MACOS
    EXPECT_TRUE(pong->sandboxed);
    // No file writes from inside the sandbox.
    QTemporaryDir dir;
    const QString target = dir.filePath(QStringLiteral("written-by-helper.txt"));
    const auto w = pool.call(op(ipc::Op::TestWriteFile, target), 10000);
    ASSERT_TRUE(w.has_value());
    EXPECT_FALSE(w->ok);
    EXPECT_FALSE(QFile::exists(target));
#endif
}

TEST(DecoderHelper, DecodesLikeInProcess) {
    LF_REQUIRE_CORPUS();
    auto pool = std::make_shared<DecoderHelperPool>(helperOptions());
    struct Case {
        std::shared_ptr<Decoder> local;
        const char* file;
    };
    const std::vector<Case> cases = {{std::make_shared<SvgDecoder>(), "vector.svg"},
                                     {std::make_shared<QtImageDecoder>(), "rgba8.png"},
                                     {std::make_shared<QtImageDecoder>(), "p3.png"}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        const QString path = corpusFile(QString::fromLatin1(c.file));
        RemoteDecoder remote(c.local, pool);
        DecodeRequest req;
        req.maxPixelSize = 300;
        QString e1, e2;
        const auto local = c.local->decode(path, req, &e1);
        const auto viaHelper = remote.decode(path, req, &e2);
        ASSERT_TRUE(local.has_value()) << e1.toStdString();
        ASSERT_TRUE(viaHelper.has_value()) << e2.toStdString();
        EXPECT_EQ(viaHelper->image.size(), local->image.size());
        EXPECT_EQ(viaHelper->image.format(), local->image.format());
        EXPECT_EQ(viaHelper->image.colorSpace(), local->image.colorSpace());
        EXPECT_EQ(viaHelper->fullSize, local->fullSize);
        EXPECT_EQ(viaHelper->isPreview, local->isPreview);
        EXPECT_EQ(viaHelper->image, local->image);
        const auto i1 = c.local->info(path, &e1);
        const auto i2 = remote.info(path, &e2);
        ASSERT_TRUE(i1 && i2);
        EXPECT_EQ(i2->pixelSize, i1->pixelSize);
        EXPECT_EQ(i2->formatName, i1->formatName);
        EXPECT_EQ(remote.metadata(path).entries.size(), c.local->metadata(path).entries.size());
    }
    // Errors of the backend arrive as such (no crash, no restart).
    RemoteDecoder remote(std::make_shared<QtImageDecoder>(), pool);
    QString error;
    EXPECT_FALSE(remote.decode(corpusFile(QStringLiteral("garbage.jpg")), DecodeRequest{}, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(pool->startedProcesses(), 1);
}

TEST(DecoderHelper, SharedMemoryIsRemovedInEveryCase) {
    LF_REQUIRE_CORPUS();
    DecoderHelperPool pool(helperOptions());
    ipc::Request r = op(ipc::Op::Decode, corpusFile(QStringLiteral("rgba8.png")));
    r.backend = QStringLiteral("Qt");
    // 1. Mapped by the app: gone right away.
    qint64 pid = 0;
    auto resp = pool.call(r, 10000, nullptr, nullptr, &pid);
    ASSERT_TRUE(resp && resp->ok);
    const QByteArray first = ipc::shmName(pid, resp->id);
    EXPECT_TRUE(ipc::sharedExists(first));
    QString error;
    EXPECT_FALSE(ipc::readSharedImage(first, resp->pixels, &error).isNull());
    EXPECT_FALSE(ipc::sharedExists(first));
    // 2. Never picked up: the helper removes it with the next request.
    resp = pool.call(r, 10000, nullptr, nullptr, &pid);
    ASSERT_TRUE(resp && resp->ok);
    const QByteArray second = ipc::shmName(pid, resp->id);
    EXPECT_TRUE(ipc::sharedExists(second));
    ASSERT_TRUE(pool.call(op(ipc::Op::Ping), 10000).has_value());
    EXPECT_FALSE(ipc::sharedExists(second));
    // 3. Helper crashes after writing the pixels: the app removes the name it computed.
    r.path = QDir::temp().filePath(QStringLiteral("x.lfcrash"));
    resp = pool.call(r, 10000, nullptr, nullptr, &pid);
    EXPECT_FALSE(resp.has_value());
    for (quint32 id = 1; id < 10; ++id)
        EXPECT_FALSE(ipc::sharedExists(ipc::shmName(pid, id)));
}

TEST(DecoderHelper, F_FMT_02_CrashGivesErrorCardAndRestarts) {
    auto pool = std::make_shared<DecoderHelperPool>(helperOptions());
    DecoderHelperPool::Failure f = DecoderHelperPool::Failure::None;
    QString error;
    qint64 firstPid = 0;
    ASSERT_TRUE(pool->call(op(ipc::Op::Ping), 10000, &f, &error, &firstPid).has_value());
    EXPECT_FALSE(pool->call(op(ipc::Op::TestCrash), 10000, &f, &error).has_value());
    EXPECT_EQ(f, DecoderHelperPool::Failure::Crashed);
    EXPECT_EQ(error, messageDecoderCrashed());
    EXPECT_FALSE(processAlive(firstPid));
    // The next request gets a fresh helper.
    qint64 secondPid = 0;
    const auto pong = pool->call(op(ipc::Op::Ping), 10000, &f, &error, &secondPid);
    ASSERT_TRUE(pong.has_value());
    EXPECT_NE(secondPid, firstPid);
    EXPECT_EQ(pool->startedProcesses(), 2);

    // Through the registry: the file gets the crash message as its error (card), the app
    // lives on and the next file decodes.
    QTemporaryDir dir;
    const QString crashing = dir.filePath(QStringLiteral("evil.lfcrash"));
    QFile file(crashing);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not an image at all, but large enough for the header check");
    file.close();
    DecoderRegistry reg;
    reg.add(std::make_shared<RemoteDecoder>(std::make_shared<AnyFileDecoder>(), pool));
    QString regError;
    EXPECT_FALSE(reg.decode(crashing, DecodeRequest{}, &regError).has_value());
    EXPECT_EQ(regError, messageDecoderCrashed());
    LF_REQUIRE_CORPUS();
    const auto ok = reg.decode(corpusFile(QStringLiteral("rgba8.png")), DecodeRequest{}, &regError);
    ASSERT_TRUE(ok.has_value()) << regError.toStdString();
    EXPECT_FALSE(ok->image.isNull());
}

TEST(DecoderHelper, TimeoutKillsAndRestarts) {
    DecoderHelperPool pool(helperOptions());
    DecoderHelperPool::Failure f = DecoderHelperPool::Failure::None;
    QString error;
    qint64 pid = 0;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(pool.call(op(ipc::Op::TestHang), 300, &f, &error, &pid).has_value());
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(f, DecoderHelperPool::Failure::Timeout);
    EXPECT_EQ(error, messageDecoderTimeout());
    EXPECT_LT(ms, 5000);
    EXPECT_FALSE(processAlive(pid));
    qint64 next = 0;
    ASSERT_TRUE(pool.call(op(ipc::Op::Ping), 10000, &f, &error, &next).has_value());
    EXPECT_NE(next, pid);
}

TEST(DecoderHelper, ConcurrentCallsShareThePool) {
    LF_REQUIRE_CORPUS();
    auto pool = std::make_shared<DecoderHelperPool>(helperOptions());
    RemoteDecoder remote(std::make_shared<QtImageDecoder>(), pool);
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 5; ++i) {
                QString e;
                if (auto img = remote.decode(corpusFile(QStringLiteral("rgba8.png")), DecodeRequest{}, &e);
                    img && !img->image.isNull())
                    ++ok;
            }
        });
    }
    for (auto& t : threads)
        t.join();
    EXPECT_EQ(ok.load(), 30);
    EXPECT_LE(pool->startedProcesses(), 2);
}

TEST(DecoderHelper, MissingExecutableFailsCleanly) {
    DecoderHelperPool::Options o = helperOptions();
    o.executable = QStringLiteral("/nonexistent/lumaflip-decoder");
    DecoderHelperPool pool(o);
    DecoderHelperPool::Failure f = DecoderHelperPool::Failure::None;
    QString error;
    EXPECT_FALSE(pool.call(op(ipc::Op::Ping), 1000, &f, &error).has_value());
    EXPECT_EQ(f, DecoderHelperPool::Failure::StartFailed);
    EXPECT_FALSE(error.isEmpty());
}

#endif
