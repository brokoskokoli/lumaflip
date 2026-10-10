// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder helper process (SPEC 8.5, M3): protocol, shared memory, crash recovery,
// timeouts, sandbox. Starts the real `lumaflip-decoder` (no windows).
// Requirements: F-FMT-02 (a crashing decoder gives the file an error card).

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/DecoderHelper.h"
#include "core/DecoderIpc.h"
#include "core/QtImageDecoder.h"
#include "core/SimpleDecoders.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <vector>

#ifdef Q_OS_UNIX
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef Q_OS_DARWIN
#include <crt_externs.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
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
    const ipc::PixelHeader h = ipc::describeImage(img);
    QString error;
    // The app creates the object (exclusive), the helper fills it and removes the name.
    int fd = ipc::createShared(name, h.shmSize, &error);
    ASSERT_GE(fd, 0) << error.toStdString();
    EXPECT_TRUE(ipc::sharedExists(name));
    EXPECT_LT(ipc::createShared(name, h.shmSize, &error), 0) << "exclusive";
    ASSERT_TRUE(ipc::fillSharedImage(name, img, &error)) << error.toStdString();
    EXPECT_FALSE(ipc::sharedExists(name)) << "the helper removes the name right after opening";
    QImage back = ipc::mapSharedImage(fd, h, &error);
    ASSERT_FALSE(back.isNull()) << error.toStdString();
    EXPECT_EQ(back.colorSpace(), img.colorSpace());
    EXPECT_EQ(back.format(), img.format());
    for (int y = 0; y < img.height(); ++y)
        EXPECT_EQ(QByteArray(reinterpret_cast<const char*>(back.constScanLine(y)), img.width() * 4),
                  QByteArray(reinterpret_cast<const char*>(img.constScanLine(y)), img.width() * 4));
    back = QImage(); // unmaps

    // A header that does not match the object is rejected.
    fd = ipc::createShared(name, h.shmSize, &error);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(ipc::fillSharedImage(name, img, &error));
    ipc::PixelHeader bad = h;
    bad.height = 1000;
    bad.shmSize = bad.bytesPerLine * bad.height;
    EXPECT_TRUE(ipc::mapSharedImage(fd, bad, &error).isNull());
    // An object smaller than the image is not written past its end.
    const QImage large(512, 512, QImage::Format_RGBA8888);
    fd = ipc::createShared(name, ipc::describeImage(large).shmSize / 2, &error);
    ASSERT_GE(fd, 0);
    EXPECT_FALSE(ipc::fillSharedImage(name, large, &error));
    EXPECT_FALSE(ipc::sharedExists(name));
    close(fd);
    // Only names with the helper's prefix are opened.
    EXPECT_FALSE(ipc::fillSharedImage("/other.name", img, &error));
}

// Metadata values cross the process boundary as plain types only: a hostile helper must
// not make the app construct arbitrary QVariant types (a QImage would be decoded by the
// app's own image plugins - the very thing the helper isolates). Found by fuzz_decoder_ipc.
TEST(DecoderIpc, MetadataValuesArePlainTypesOnly) {
    ipc::Response rs;
    rs.id = 1;
    rs.ok = true;
    QImage tiny(2, 2, QImage::Format_RGBA8888);
    tiny.fill(Qt::red);
    rs.metadata.entries.append({QStringLiteral("x.image"), QStringLiteral("Bild"), QVariant(tiny)});
    rs.metadata.entries.append({QStringLiteral("x.count"), QStringLiteral("Zahl"), QVariant(42)});
    QVariantMap inner{{QStringLiteral("when"), QDateTime(QDate(2024, 1, 2), QTime(3, 4), QTimeZone::UTC)},
                      {QStringLiteral("list"), QStringList{"a", "b"}},
                      {QStringLiteral("image"), QVariant(tiny)}};
    rs.metadata.groups.insert(QStringLiteral("Gruppe"), inner);
    const auto back = ipc::decodeResponse(ipc::encode(rs));
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->metadata.entries.size(), 2);
    EXPECT_NE(back->metadata.entries[0].value.typeId(), QMetaType::QImage);
    EXPECT_EQ(back->metadata.entries[1].value.toInt(), 42);
    const QVariantMap g = back->metadata.groups.value(QStringLiteral("Gruppe")).toMap();
    EXPECT_EQ(g.value(QStringLiteral("when")).toDateTime(), inner.value(QStringLiteral("when")).toDateTime());
    EXPECT_EQ(g.value(QStringLiteral("list")).toStringList(), (QStringList{"a", "b"}));
    EXPECT_NE(g.value(QStringLiteral("image")).typeId(), QMetaType::QImage);
}

// Code review M3 #5: a hostile helper's header must not overflow the size check
// (bytesPerLine * height wrapping to a small shmSize), announce an empty buffer or an
// indexed/mono format (colour table not transferred).
TEST(DecoderIpc, PixelHeaderValidation) {
    ipc::PixelHeader ok;
    ok.width = 3;
    ok.height = 4;
    ok.format = int(QImage::Format_RGBA8888);
    ok.bytesPerLine = 12;
    ok.shmSize = 48;
    QString error;
    EXPECT_TRUE(ipc::validatePixelHeader(ok, &error));
    ipc::PixelHeader h = ok;
    h.width = 1;
    h.bytesPerLine = (qint64(1) << 62) + 1;
    h.shmSize = 4; // (2^62 + 1) * 4 wraps to 4
    EXPECT_FALSE(ipc::validatePixelHeader(h, &error));
    EXPECT_FALSE(error.isEmpty());
    h = ok;
    h.shmSize = 0;
    h.bytesPerLine = 0;
    EXPECT_FALSE(ipc::validatePixelHeader(h, &error));
    h = ok;
    h.bytesPerLine = 11; // shorter than a row
    h.shmSize = 44;
    EXPECT_FALSE(ipc::validatePixelHeader(h, &error));
    for (QImage::Format f : {QImage::Format_Indexed8, QImage::Format_Mono, QImage::Format_MonoLSB,
                             QImage::Format_Invalid, QImage::NImageFormats}) {
        h = ok;
        h.format = int(f);
        EXPECT_FALSE(ipc::validatePixelHeader(h, &error)) << int(f);
    }
    h = ok;
    h.height = -4;
    h.shmSize = -48;
    EXPECT_FALSE(ipc::validatePixelHeader(h, &error));

    // Mapping refuses the same header (and closes the descriptor).
    const QByteArray name = ipc::shmName(getpid(), 900002);
    ipc::unlinkShared(name);
    const int fd = ipc::createShared(name, 4096, &error);
    ASSERT_GE(fd, 0);
    ipc::unlinkShared(name);
    h = ok;
    h.width = 1;
    h.bytesPerLine = (qint64(1) << 62) + 1;
    h.shmSize = 4;
    EXPECT_TRUE(ipc::mapSharedImage(fd, h, &error).isNull());
    EXPECT_LT(ipc::createShared(name, 0, &error), 0) << "empty objects are refused";
}

// Code review M3 #6: the timeout bounds the whole frame, not each wait for a byte.
TEST(DecoderIpc, ReadFrameHasAbsoluteDeadline) {
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    std::atomic<bool> stop{false};
    std::thread trickle([&] {
        const unsigned char len[4] = {100, 0, 0, 0};
        for (int i = 0; i < 4 && !stop; ++i) {
            (void)write(fds[1], len + i, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        for (int i = 0; i < 100 && !stop; ++i) {
            (void)write(fds[1], "x", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    });
    QElapsedTimer t;
    t.start();
    QByteArray in;
    EXPECT_EQ(ipc::readFrame(fds[0], &in, 300), ipc::IoStatus::Timeout);
    EXPECT_LT(t.elapsed(), 1500);
    stop = true;
    trickle.join();
    close(fds[0]);
    close(fds[1]);

    // The in-memory parser used by the fuzz target agrees with the framing.
    QByteArray frame("\x03\x00\x00\x00"
                     "abc",
                     7);
    QByteArray payload;
    EXPECT_EQ(ipc::parseFrame(frame, &payload), 7);
    EXPECT_EQ(payload, QByteArray("abc"));
    EXPECT_EQ(ipc::parseFrame(frame.left(5), &payload), 0);
    EXPECT_EQ(ipc::parseFrame(QByteArray("\xFF\xFF\xFF\x7F", 4), &payload), -1);
}

#endif

#if defined(LF_DECODER_HELPER_BINARY)

namespace {

DecoderHelperPool::Options helperOptions(int decodeTimeoutMs = 30000) {
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.maxProcesses = 2;
    o.decodeTimeoutMs = decodeTimeoutMs;
    o.extraEnvironment = {QStringLiteral("LF_DECODER_HELPER_TEST_OPS=1"),
                          QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    return o;
}

ipc::Request op(ipc::Op o, const QString& path = {}) {
    ipc::Request r;
    r.op = o;
    r.path = path;
    return r;
}

int maxChannelDifference(const QImage& a, const QImage& b) {
    if (a.size() != b.size())
        return 256;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888),
                 y = b.convertToFormat(QImage::Format_RGBA8888);
    int worst = 0;
    for (int row = 0; row < x.height(); ++row) {
        const uchar* p = x.constScanLine(row);
        const uchar* q = y.constScanLine(row);
        for (int i = 0; i < x.width() * 4; ++i)
            worst = std::max(worst, std::abs(int(p[i]) - int(q[i])));
    }
    return worst;
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
    EXPECT_FALSE(
        remote.decode(corpusFile(QStringLiteral("garbage.jpg")), DecodeRequest{}, &error).has_value());
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(pool->startedProcesses(), 1);
}

TEST(DecoderHelper, SharedMemoryIsRemovedInEveryCase) {
    LF_REQUIRE_CORPUS();
    DecoderHelperPool pool(helperOptions());
    ipc::Request r = op(ipc::Op::Decode, corpusFile(QStringLiteral("rgba8.png")));
    r.backend = QStringLiteral("Qt");
    const qint64 app = qint64(getpid());
    // 1. Mapped by the pool while it holds the helper: no name left, the image valid.
    auto resp = pool.call(r, 10000);
    ASSERT_TRUE(resp && resp->ok);
    EXPECT_FALSE(resp->image.isNull());
    EXPECT_EQ(resp->image.size(), QSize(64, 48));
    EXPECT_FALSE(ipc::sharedExists(ipc::shmName(app, resp->id)));
    // 2. Helper crashes: the app removes the names it created.
    r.path = QDir::temp().filePath(QStringLiteral("x.lfcrash"));
    resp = pool.call(r, 10000);
    EXPECT_FALSE(resp.has_value());
    for (quint32 id = 1; id < 12; ++id)
        EXPECT_FALSE(ipc::sharedExists(ipc::shmName(app, id)));
}

// The helper's request loop (in process, over pipes): Decode keeps the image and announces
// its size, Deliver fills the app's object and removes its name; a Deliver for another
// request, or after any other request, is refused. The helper never creates an object.
TEST(DecoderHelper, HelperDeliversIntoAppOwnedSharedMemory) {
    LF_REQUIRE_CORPUS();
    int toHelper[2], fromHelper[2];
    ASSERT_EQ(pipe(toHelper), 0);
    ASSERT_EQ(pipe(fromHelper), 0);
    const QList<std::shared_ptr<Decoder>> backends = {std::make_shared<QtImageDecoder>()};
    int exitCode = -1;
    std::thread loop(
        [&] { exitCode = runDecoderHelper(toHelper[0], fromHelper[1], backends, false, false); });
    ipc::Request r = op(ipc::Op::Decode, corpusFile(QStringLiteral("rgba8.png")));
    r.backend = QStringLiteral("Qt");
    auto roundTrip = [&](ipc::Request req) {
        EXPECT_EQ(ipc::writeFrame(toHelper[1], ipc::encode(req)), ipc::IoStatus::Ok);
        QByteArray payload;
        EXPECT_EQ(ipc::readFrame(fromHelper[0], &payload, 10000), ipc::IoStatus::Ok);
        return ipc::decodeResponse(payload);
    };
    r.id = 4711;
    auto resp = roundTrip(r);
    ASSERT_TRUE(resp && resp->ok);
    EXPECT_FALSE(ipc::sharedExists(ipc::shmName(getpid(), 4711))) << "the helper creates nothing";
    ASSERT_TRUE(ipc::validatePixelHeader(resp->pixels, nullptr));
    const QByteArray name = ipc::shmName(getpid(), 4711);
    ipc::unlinkShared(name);
    QString error;
    const int fd = ipc::createShared(name, resp->pixels.shmSize, &error);
    ASSERT_GE(fd, 0);
    ipc::Request deliver = op(ipc::Op::Deliver);
    deliver.id = 4711;
    deliver.shm = name;
    auto delivered = roundTrip(deliver);
    ASSERT_TRUE(delivered && delivered->ok) << (delivered ? delivered->error.toStdString() : "");
    EXPECT_FALSE(ipc::sharedExists(name));
    const QImage img = ipc::mapSharedImage(fd, resp->pixels, &error);
    EXPECT_EQ(img.size(), QSize(64, 48));
    // Deliver again (image already handed over) or for an unknown id: refused.
    const int fd2 = ipc::createShared(name, resp->pixels.shmSize, &error);
    ASSERT_GE(fd2, 0);
    delivered = roundTrip(deliver);
    ASSERT_TRUE(delivered.has_value());
    EXPECT_FALSE(delivered->ok);
    r.id = 4712;
    ASSERT_TRUE(roundTrip(r)->ok);
    ipc::Request ping = op(ipc::Op::Ping);
    ping.id = 4713;
    ASSERT_TRUE(roundTrip(ping).has_value()); // drops the pending image of 4712
    deliver.id = 4712;
    delivered = roundTrip(deliver);
    ASSERT_TRUE(delivered.has_value());
    EXPECT_FALSE(delivered->ok);
    ipc::unlinkShared(name);
    close(fd2);
    close(toHelper[1]); // app gone
    loop.join();
    EXPECT_EQ(exitCode, 0);
    close(toHelper[0]);
    close(fromHelper[0]);
    close(fromHelper[1]);
}

#ifdef Q_OS_MACOS
// Code review M3 #1: the app quits or crashes while the helper decodes. Before, the helper
// had created "/lfd.<helper pid>.<id>" and died of SIGPIPE writing the answer - the segment
// stayed until reboot. Now the helper ignores SIGPIPE, exits cleanly and owns no segment.
TEST(DecoderHelper, NoSegmentLeftWhenAppGoesAwayMidDecode) {
    QTemporaryDir dir;
    const QString img = dir.filePath(QStringLiteral("big.pgm"));
    {
        QFile f(img);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("P5\n3000 3000\n255\n");
        f.write(QByteArray(3000 * 3000, char(0x80)));
    }
    int in[2], out[2];
    ASSERT_EQ(pipe(in), 0);
    ASSERT_EQ(pipe(out), 0);
    posix_spawn_file_actions_t a;
    posix_spawn_file_actions_init(&a);
    posix_spawn_file_actions_adddup2(&a, in[0], 0);
    posix_spawn_file_actions_adddup2(&a, out[1], 1);
    posix_spawn_file_actions_addclose(&a, in[1]);
    posix_spawn_file_actions_addclose(&a, out[0]);
    const QByteArray exe(LF_DECODER_HELPER_BINARY);
    char* argv[] = {const_cast<char*>(exe.constData()), const_cast<char*>("--decoder-helper"), nullptr};
    std::vector<QByteArray> envStore;
    for (char** e = *_NSGetEnviron(); e && *e; ++e)
        envStore.emplace_back(*e);
    envStore.emplace_back("QT_QPA_PLATFORM=offscreen");
    std::vector<char*> envp;
    for (QByteArray& e : envStore)
        envp.push_back(e.data());
    envp.push_back(nullptr);
    pid_t pid = 0;
    ASSERT_EQ(posix_spawn(&pid, exe.constData(), &a, nullptr, argv, envp.data()), 0);
    posix_spawn_file_actions_destroy(&a);
    close(in[0]);
    close(out[1]);
    ipc::Request r = op(ipc::Op::Decode, img);
    r.id = 7;
    r.backend = QStringLiteral("Lumaflip PNM");
    ASSERT_EQ(fcntl(in[1], F_SETNOSIGPIPE, 1), 0);
    ASSERT_EQ(ipc::writeFrame(in[1], ipc::encode(r)), ipc::IoStatus::Ok);
    close(out[0]); // the app is gone while the helper decodes; its stdin stays open
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(in[1]);
    EXPECT_FALSE(WIFSIGNALED(status)) << "helper killed by signal " << WTERMSIG(status);
    const QByteArray helperName = ipc::shmName(pid, 7), appName = ipc::shmName(getpid(), 7);
    const bool leaked = ipc::sharedExists(helperName) || ipc::sharedExists(appName);
    ipc::unlinkShared(helperName);
    EXPECT_FALSE(leaked);
}

#endif

// Code review M3 #6: waiting for an idle helper counts against the call's deadline.
TEST(DecoderHelper, WaitingForAnIdleHelperHasADeadline) {
    DecoderHelperPool::Options o = helperOptions();
    o.maxProcesses = 1;
    DecoderHelperPool pool(o);
    ASSERT_TRUE(pool.call(op(ipc::Op::Ping), 10000).has_value()); // started and verified
    std::thread hang([&] { pool.call(op(ipc::Op::TestHang), 4000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // the hang holds the helper
    DecoderHelperPool::Failure f = DecoderHelperPool::Failure::None;
    QElapsedTimer t;
    t.start();
    EXPECT_FALSE(pool.call(op(ipc::Op::Ping), 300, &f).has_value());
    EXPECT_LT(t.elapsed(), 2000);
    EXPECT_EQ(f, DecoderHelperPool::Failure::Timeout);
    hang.join();
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
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
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

#ifdef Q_OS_MACOS

// Code review M3 #2: deny-by-default sandbox. The helper can neither talk to the network,
// write files, start programs, create shared memory nor reach services like LaunchServices
// or the pasteboard - while the same operations work outside the sandbox (in this test).
TEST(DecoderHelper, SandboxDeniesEverythingButReading) {
    DecoderHelperPool pool(helperOptions());
    auto probe = [&](ipc::Op o, const QString& arg) {
        const auto r = pool.call(op(o, arg), 10000);
        EXPECT_TRUE(r.has_value());
        return r && r->ok;
    };
    // Network: a listening socket on the loopback interface.
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    ASSERT_EQ(listen(listener, 4), 0);
    socklen_t len = sizeof addr;
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const QString port = QString::number(ntohs(addr.sin_port));
    {
        const int c = socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_EQ(::connect(c, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0) << "control";
        close(c);
    }
    EXPECT_FALSE(probe(ipc::Op::TestSocket, port));
    close(listener);

    QTemporaryDir dir;
    const QString target = dir.filePath(QStringLiteral("written-by-helper.txt"));
    EXPECT_FALSE(probe(ipc::Op::TestWriteFile, target));
    EXPECT_FALSE(QFile::exists(target));
    EXPECT_FALSE(probe(ipc::Op::TestExec, QStringLiteral("/usr/bin/true")));
    const QString shm = QString::fromLatin1(ipc::shmName(getpid(), 999999));
    EXPECT_FALSE(probe(ipc::Op::TestCreateShm, shm));
    EXPECT_FALSE(ipc::sharedExists(shm.toLatin1()));
    for (const char* service : {"com.apple.coreservices.launchservicesd", "com.apple.pasteboard.1",
                                "com.apple.windowserver.active", "com.apple.coreservices.appleevents"}) {
        SCOPED_TRACE(service);
        mach_port_t port = MACH_PORT_NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const bool reachable = bootstrap_look_up(bootstrap_port, service, &port) == KERN_SUCCESS;
#pragma clang diagnostic pop
        if (reachable)
            mach_port_deallocate(mach_task_self(), port);
        EXPECT_FALSE(probe(ipc::Op::TestMachLookup, QString::fromLatin1(service)))
            << "reachable outside the sandbox: " << reachable;
    }
    // Not even the font daemon: fonts are read as files (SVG text still renders, see
    // SandboxedHelperDecodesEveryServedCorpusFormat).
    EXPECT_FALSE(probe(ipc::Op::TestMachLookup, QStringLiteral("com.apple.fonts")));
}

// The app refuses a helper that reports running without sandbox (error card text).
TEST(DecoderHelper, RefusesHelperWithoutSandbox) {
    DecoderHelperPool::Options o = helperOptions();
    o.extraEnvironment << QStringLiteral("LF_DECODER_HELPER_TEST_NO_SANDBOX=1");
    auto pool = std::make_shared<DecoderHelperPool>(o);
    DecoderHelperPool::Failure f = DecoderHelperPool::Failure::None;
    QString error;
    qint64 pid = 0;
    EXPECT_FALSE(pool->call(op(ipc::Op::Ping), 10000, &f, &error, &pid).has_value());
    EXPECT_EQ(f, DecoderHelperPool::Failure::NotSandboxed);
    EXPECT_EQ(error, messageDecoderNotSandboxed());
    EXPECT_FALSE(processAlive(pid));
    LF_REQUIRE_CORPUS();
    RemoteDecoder remote(std::make_shared<QtImageDecoder>(), pool);
    EXPECT_FALSE(remote.decode(corpusFile(QStringLiteral("rgba8.png")), DecodeRequest{}, &error).has_value());
    EXPECT_EQ(error, messageDecoderNotSandboxed());
}

#endif

// Every corpus file a helper backend claims decodes in the sandboxed helper exactly when it
// decodes in process (the sandbox profile does not break a format, e.g. SVG text needing
// the font services).
TEST(DecoderHelper, SandboxedHelperDecodesEveryServedCorpusFormat) {
    LF_REQUIRE_CORPUS();
    ensureQtApp(); // fonts for the in-process SVG rendering
    auto pool = std::make_shared<DecoderHelperPool>(helperOptions());
    QList<std::shared_ptr<Decoder>> backends = {std::make_shared<SvgDecoder>(),
                                                std::make_shared<QtImageDecoder>()};
    backends << simpleDecoders();
    // SVG with text in a system font (CoreText inside the sandbox).
    QTemporaryDir dir;
    const QString textSvg = dir.filePath(QStringLiteral("text.svg"));
    {
        QFile f(textSvg);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"60\">"
                "<text x=\"10\" y=\"40\" font-family=\"Helvetica\" font-size=\"30\">Lumaflip</text></svg>");
    }
    QStringList files = {textSvg};
    QDirIterator it(QStringLiteral(LF_CORPUS_DIR "/generated"), QDir::Files);
    while (it.hasNext())
        files << it.next();
    int compared = 0;
    for (const QString& path : files) {
        const QByteArray header = fileHeader(path);
        for (const auto& local : backends) {
            if (!local->canDecode(path, header))
                continue;
            SCOPED_TRACE((QFileInfo(path).fileName() + QLatin1String(" / ") + local->name()).toStdString());
            RemoteDecoder remote(local, pool);
            DecodeRequest req;
            req.maxPixelSize = 256;
            QString e1, e2;
            const auto a = local->decode(path, req, &e1);
            const auto b = remote.decode(path, req, &e2);
            EXPECT_EQ(a.has_value(), b.has_value()) << e1.toStdString() << " | " << e2.toStdString();
            if (a && b) {
                // ImageIO-backed Qt plugins (HEIF) use the software HEVC decoder in the
                // sandbox (no com.apple.coremedia.videodecoder): rounding may differ by a step.
                EXPECT_EQ(a->image.size(), b->image.size());
                EXPECT_EQ(a->image.format(), b->image.format());
                EXPECT_LE(maxChannelDifference(a->image, b->image), 4);
                ++compared;
            }
            break; // the first backend that claims the file (registry order)
        }
    }
    EXPECT_GT(compared, 10);
    // The text really rendered (glyphs, not an empty canvas).
    RemoteDecoder svg(std::make_shared<SvgDecoder>(), pool);
    QString error;
    const auto text = svg.decode(textSvg, DecodeRequest{}, &error);
    ASSERT_TRUE(text.has_value()) << error.toStdString();
    int inked = 0;
    const QImage rgba = text->image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < rgba.height(); ++y)
        for (int x = 0; x < rgba.width(); ++x)
            inked += rgba.pixelColor(x, y).alpha() > 128 ? 1 : 0;
    EXPECT_GT(inked, 200);
    EXPECT_EQ(pool->startedProcesses(), 1) << "no helper crashed in its sandbox";
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
