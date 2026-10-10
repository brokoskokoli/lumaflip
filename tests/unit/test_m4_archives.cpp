// SPDX-License-Identifier: GPL-3.0-or-later
// F-OPEN-10 (M4): archives (ZIP/CBZ, RAR/CBR, 7z/CB7) browsed like folders - member paths,
// listing order, extraction on demand, zip-bomb and malformed-archive safety, the helper's
// archive operations and the container collection. Archives are generated here (libarchive's
// writer, the own ZIP writer below and /usr/bin/zip); no windows.

#include "QtTestApp.h"
#include "core/Archive.h"
#include "core/ArchiveCache.h"
#include "core/Collection.h"
#include "core/ContainerPolicy.h"
#include "core/Decoder.h"
#include "core/DecoderHelper.h"
#include "core/DecoderIpc.h"
#include "core/LibArchive.h"
#include "core/Platform.h"
#include "core/VirtualPath.h"

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>

#include <zlib.h>

using namespace lf;

namespace {

QByteArray png(int w, int h, QColor color = Qt::red) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(color);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

void put16(QByteArray& b, quint32 v) {
    b.append(char(v & 0xff)).append(char((v >> 8) & 0xff));
}
void put32(QByteArray& b, quint32 v) {
    put16(b, v & 0xffff);
    put16(b, v >> 16);
}

QByteArray rawDeflate(const QByteArray& in) {
    z_stream zs{};
    deflateInit2(&zs, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(int(deflateBound(&zs, uLong(in.size()))), '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uInt(out.size());
    deflate(&zs, Z_FINISH);
    out.truncate(int(zs.total_out));
    deflateEnd(&zs);
    return out;
}

struct ZipMember {
    QString name;
    QByteArray data;
    qint64 claimedSize = -1; // -1: the real size (zip bombs lie here)
};

// Minimal ZIP writer (deflate), so a test can state any size in the directory.
QByteArray makeZip(const QList<ZipMember>& members) {
    QByteArray zip, central;
    for (const ZipMember& m : members) {
        const QByteArray stored = rawDeflate(m.data);
        const quint32 crc =
            quint32(crc32(0, reinterpret_cast<const Bytef*>(m.data.constData()), uInt(m.data.size())));
        const quint32 size = quint32(m.claimedSize >= 0 ? m.claimedSize : m.data.size());
        const QByteArray n = m.name.toUtf8();
        const quint32 offset = quint32(zip.size());
        put32(zip, 0x04034b50);
        put16(zip, 20);
        put16(zip, 0x800);
        put16(zip, 8);
        put32(zip, 0);
        put32(zip, crc);
        put32(zip, quint32(stored.size()));
        put32(zip, size);
        put16(zip, quint32(n.size()));
        put16(zip, 0);
        zip.append(n).append(stored);
        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x800);
        put16(central, 8);
        put32(central, 0);
        put32(central, crc);
        put32(central, quint32(stored.size()));
        put32(central, size);
        put16(central, quint32(n.size()));
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central.append(n);
    }
    const quint32 cdOffset = quint32(zip.size());
    zip.append(central);
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, quint32(members.size()));
    put16(zip, quint32(members.size()));
    put32(zip, quint32(central.size()));
    put32(zip, cdOffset);
    put16(zip, 0);
    return zip;
}

QString writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
    return path;
}

QStringList memberNames(const QVector<FileEntry>& entries) {
    QStringList out;
    for (const FileEntry& e : entries)
        out << vpath::split(e.path)->member;
    return out;
}

class ArchiveTest : public ::testing::Test {
protected:
    void SetUp() override {
        test::ensureQtApp();
        ASSERT_TRUE(dir.isValid());
        collator = createNameCollator();
    }
    QString path(const QString& name) const { return QDir::cleanPath(dir.path()) + u'/' + name; }

    QTemporaryDir dir;
    std::unique_ptr<NameCollator> collator;
};

const QList<QPair<QString, QByteArray>>& comicPages() {
    static const QList<QPair<QString, QByteArray>> pages = {
        {QStringLiteral("10.png"), png(4, 3)},
        {QStringLiteral("2.png"), png(4, 3)},
        {QStringLiteral("1.png"), png(4, 3)},
        {QStringLiteral("Kapitel 10/01.png"), png(4, 3)},
        {QStringLiteral("Kapitel 2/01.png"), png(4, 3)},
        {QStringLiteral("__MACOSX/._1.png"), QByteArray("resource fork")},
        {QStringLiteral(".hidden.png"), png(4, 3)},
        {QStringLiteral("notes.txt"), QByteArray("text")},
    };
    return pages;
}

} // namespace

// ---- member paths -----------------------------------------------------------------------

TEST(VirtualPath, F_OPEN_10_MemberPathsRoundTrip) {
    const QString c = QStringLiteral("/Comics/Band 1.cbz");
    const QString m = vpath::join(c, QStringLiteral("Kapitel 1/012.jpg"));
    EXPECT_EQ(m, QStringLiteral("/Comics/Band 1.cbz#/Kapitel 1/012.jpg"));
    ASSERT_TRUE(vpath::isMember(m));
    EXPECT_EQ(vpath::split(m)->container, c);
    EXPECT_EQ(vpath::split(m)->member, QStringLiteral("Kapitel 1/012.jpg"));
    EXPECT_EQ(vpath::containerOf(m), c);
    EXPECT_EQ(vpath::memberFileName(m), QStringLiteral("012.jpg"));
    EXPECT_EQ(vpath::displayName(m), QStringLiteral("Band 1.cbz › Kapitel 1/012.jpg"));
    // Ordinary paths are never members, also with a "#/" after something else.
    EXPECT_FALSE(vpath::isMember(QStringLiteral("/Fotos/a#/b.jpg")));
    EXPECT_FALSE(vpath::isMember(QStringLiteral("/Fotos/x.cbz")));
    EXPECT_FALSE(vpath::isMember(QStringLiteral("/Fotos/x.cbz#/")));
    EXPECT_EQ(vpath::displayName(QStringLiteral("/Fotos/a.jpg")), QStringLiteral("a.jpg"));
    // Every container type by suffix (case-insensitive); PDF pages.
    for (const char* s : {"zip", "CBZ", "rar", "cbr", "7z", "cb7", "pdf"})
        EXPECT_TRUE(vpath::isContainerPath(QStringLiteral("/a/b.") + QLatin1String(s))) << s;
    EXPECT_FALSE(vpath::isContainerPath(QStringLiteral("/a/b.jpg")));
    EXPECT_TRUE(vpath::isComicArchive(QStringLiteral("/a/b.cbr")));
    EXPECT_FALSE(vpath::isComicArchive(QStringLiteral("/a/b.zip")));
    EXPECT_EQ(vpath::pdfPageMember(2), QStringLiteral("0003"));
    EXPECT_EQ(vpath::pdfPageIndex(QStringLiteral("0003")), 2);
    EXPECT_EQ(vpath::pdfPageIndex(QStringLiteral("x")), -1);
    EXPECT_EQ(vpath::displayName(vpath::join(QStringLiteral("/d/Plan.pdf"), QStringLiteral("0003"))),
              QStringLiteral("Plan.pdf › Seite 3"));
}

TEST(VirtualPath, F_OPEN_10_ImageMemberFilter) {
    EXPECT_TRUE(vpath::isImageMemberName(QStringLiteral("a/b/001.JPG")));
    EXPECT_FALSE(vpath::isImageMemberName(QStringLiteral("__MACOSX/._001.jpg")));
    EXPECT_FALSE(vpath::isImageMemberName(QStringLiteral(".thumb.jpg")));
    EXPECT_FALSE(vpath::isImageMemberName(QStringLiteral("dir/.hidden/x.jpg")));
    EXPECT_FALSE(vpath::isImageMemberName(QStringLiteral("../evil.jpg")));
    EXPECT_FALSE(vpath::isImageMemberName(QStringLiteral("folder/")));
    EXPECT_FALSE(vpath::isImageMemberName(QStringLiteral("readme.txt")));
}

// ---- listing order -----------------------------------------------------------------------

TEST_F(ArchiveTest, F_OPEN_10_ZipListingInFinderOrder) {
    if (!archive::libArchiveAvailable()) // the test archives are written with libarchive
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString zip = path(QStringLiteral("Band.cbz"));
    ASSERT_TRUE(libarchive::writeArchive(zip, QStringLiteral("zip"), comicPages(), &error))
        << error.toStdString();
    const archive::Listing l = archive::listLocal(zip);
    ASSERT_TRUE(l.ok) << l.error.toStdString();
    const QVector<FileEntry> entries = archive::imageEntries(zip, l, *collator);
    // Natural numbers like Finder; a folder's images before its subfolders; folders in name
    // order; no resource forks, hidden files or non-images.
    EXPECT_EQ(memberNames(entries),
              (QStringList{"1.png", "2.png", "10.png", "Kapitel 2/01.png", "Kapitel 10/01.png"}));
    EXPECT_EQ(entries.first().name, QStringLiteral("1.png"));
    EXPECT_EQ(entries.first().size, comicPages()[2].second.size());
    EXPECT_TRUE(entries.first().modified.isValid());
}

TEST_F(ArchiveTest, F_OPEN_10_ZipFromCommandLineToolIsListed) {
    // An archive made by /usr/bin/zip (data descriptors, extra fields): the own reader.
    const QString src = path(QStringLiteral("src"));
    writeFile(src + QStringLiteral("/b 2.png"), png(3, 3));
    writeFile(src + QStringLiteral("/b 11.png"), png(3, 3));
    writeFile(src + QStringLiteral("/b 1.png"), png(3, 3));
    const QString zipTool = QStandardPaths::findExecutable(QStringLiteral("zip"));
    if (zipTool.isEmpty())
        GTEST_SKIP() << "no zip tool";
    QProcess p;
    p.setWorkingDirectory(src);
    p.start(zipTool,
            {QStringLiteral("-q"), QStringLiteral("-r"), path(QStringLiteral("t.zip")), QStringLiteral(".")});
    ASSERT_TRUE(p.waitForFinished(10000));
    ASSERT_EQ(p.exitCode(), 0);
    const archive::Listing l = archive::listLocal(path(QStringLiteral("t.zip")));
    ASSERT_TRUE(l.ok) << l.error.toStdString();
    EXPECT_EQ(memberNames(archive::imageEntries(path(QStringLiteral("t.zip")), l, *collator)),
              (QStringList{"b 1.png", "b 2.png", "b 11.png"}));
    QString error;
    const auto bytes = archive::readLocal(path(QStringLiteral("t.zip")), QStringLiteral("b 11.png"), &error);
    ASSERT_TRUE(bytes.has_value()) << error.toStdString();
    EXPECT_EQ(*bytes, png(3, 3));
}

TEST_F(ArchiveTest, F_OPEN_10_SevenZipAndOtherFormatsViaLibArchive) {
    if (!archive::libArchiveAvailable())
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString seven = path(QStringLiteral("Band.cb7"));
    ASSERT_TRUE(libarchive::writeArchive(seven, QStringLiteral("7zip"), comicPages(), &error))
        << error.toStdString();
    const archive::Listing l = archive::listLocal(seven);
    ASSERT_TRUE(l.ok) << l.error.toStdString();
    EXPECT_EQ(memberNames(archive::imageEntries(seven, l, *collator)),
              (QStringList{"1.png", "2.png", "10.png", "Kapitel 2/01.png", "Kapitel 10/01.png"}));
    const auto bytes = archive::readLocal(seven, QStringLiteral("Kapitel 10/01.png"), &error);
    ASSERT_TRUE(bytes.has_value()) << error.toStdString();
    EXPECT_EQ(*bytes, comicPages()[3].second);
    // A RAR cannot be written here; a file with the suffix but other contents fails cleanly.
    const QString rar = writeFile(path(QStringLiteral("x.cbr")), QByteArray("Rar!\x1a\x07\x00garbage", 14));
    const archive::Listing bad = archive::listLocal(rar);
    EXPECT_FALSE(bad.ok);
    EXPECT_FALSE(bad.error.isEmpty());
}

// ---- extraction on demand ----------------------------------------------------------------

TEST_F(ArchiveTest, F_OPEN_10_MemberIsExtractedPrivatelyAndDecoded) {
    if (!archive::libArchiveAvailable()) // the test archives are written with libarchive
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString zip = path(QStringLiteral("Band.cbz"));
    ASSERT_TRUE(libarchive::writeArchive(zip, QStringLiteral("zip"),
                                         {{QStringLiteral("p/01.png"), png(40, 30)}}, &error));
    const QString member = vpath::join(zip, QStringLiteral("p/01.png"));
    ArchiveCache cache;
    const QString file = cache.materialize(member, &error);
    ASSERT_FALSE(file.isEmpty()) << error.toStdString();
    EXPECT_TRUE(file.endsWith(QStringLiteral(".png")));
    EXPECT_FALSE(file.contains(QStringLiteral("p/01"))); // hashed name, never the member path
    EXPECT_EQ(QFileInfo(QFileInfo(file).absolutePath()).permissions() &
                  (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteOther),
              QFileDevice::Permissions());
    EXPECT_TRUE(cache.contains(member));
    EXPECT_EQ(cache.materialize(member, &error), file); // extracted once
    // The registry decodes member paths through the shared cache.
    DecodeRequest req;
    req.maxPixelSize = 20;
    const auto img = DecoderRegistry::instance().decode(member, req, &error);
    ASSERT_TRUE(img.has_value()) << error.toStdString();
    EXPECT_EQ(std::max(img->image.width(), img->image.height()), 20);
    EXPECT_EQ(img->fullSize, QSize(40, 30));
    // A missing member is an error, not a crash.
    EXPECT_FALSE(
        DecoderRegistry::instance().decode(vpath::join(zip, QStringLiteral("nope.png")), req, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(ArchiveTest, F_OPEN_10_CacheStaysWithinBudget) {
    int reads = 0;
    ArchiveCache cache(
        [&](const QString&, const QString& member, QString*) -> std::optional<QByteArray> {
            ++reads;
            return QByteArray(1000, member.at(0).toLatin1());
        },
        2500);
    QString error;
    const QString c = path(QStringLiteral("a.zip"));
    writeFile(c, "x");
    const QString a = cache.materialize(vpath::join(c, QStringLiteral("a.png")), &error);
    const QString b = cache.materialize(vpath::join(c, QStringLiteral("b.png")), &error);
    const QString d = cache.materialize(vpath::join(c, QStringLiteral("d.png")), &error);
    EXPECT_FALSE(a.isEmpty());
    EXPECT_LE(cache.cachedBytes(), 2500);
    EXPECT_FALSE(QFile::exists(a)); // the oldest went
    EXPECT_TRUE(QFile::exists(b));
    EXPECT_TRUE(QFile::exists(d));
    EXPECT_EQ(reads, 3);
}

// ---- safety ------------------------------------------------------------------------------

TEST_F(ArchiveTest, F_OPEN_10_ZipBombIsRefusedWithoutAllocating) {
    // 61 MiB of zeros compress to a few KiB: more than kMaxMemberBytes is refused.
    const QByteArray zeros(61 * 1024 * 1024, '\0');
    const QString bomb =
        writeFile(path(QStringLiteral("bomb.cbz")),
                  makeZip({{QStringLiteral("big.png"), zeros}, {QStringLiteral("ok.png"), png(2, 2)}}));
    QString error;
    EXPECT_FALSE(archive::readLocal(bomb, QStringLiteral("big.png"), &error).has_value());
    EXPECT_EQ(error, archive::messageMemberTooLarge());
    EXPECT_TRUE(archive::readLocal(bomb, QStringLiteral("ok.png"), &error).has_value());
    // A member that claims to be small but inflates to more: never more than claimed.
    const QString liar =
        writeFile(path(QStringLiteral("liar.zip")),
                  makeZip({{QStringLiteral("x.png"), QByteArray(5 * 1024 * 1024, 'a'), 100}}));
    const auto small = archive::readLocal(liar, QStringLiteral("x.png"), &error);
    if (small)
        EXPECT_LE(small->size(), 100);
    // A directory that claims 4 GiB for a tiny member.
    const QString huge = writeFile(path(QStringLiteral("huge.zip")),
                                   makeZip({{QStringLiteral("x.png"), QByteArray("abc"), 0xFFFFFFF0ll}}));
    EXPECT_FALSE(archive::readLocal(huge, QStringLiteral("x.png"), &error).has_value());
}

TEST_F(ArchiveTest, F_OPEN_10_SevenZipBombIsBounded) {
    if (!archive::libArchiveAvailable())
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString seven = path(QStringLiteral("bomb.7z"));
    ASSERT_TRUE(libarchive::writeArchive(seven, QStringLiteral("7zip"),
                                         {{QStringLiteral("big.png"), QByteArray(61 * 1024 * 1024, '\0')}},
                                         &error));
    EXPECT_LT(QFileInfo(seven).size(), 1024 * 1024);
    EXPECT_FALSE(
        libarchive::read(seven, QStringLiteral("big.png"), archive::kMaxMemberBytes, &error).has_value());
    EXPECT_EQ(error, archive::messageMemberTooLarge());
}

TEST_F(ArchiveTest, F_OPEN_10_MalformedArchivesFailCleanly) {
    if (!archive::libArchiveAvailable()) // the test archives are written with libarchive
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString good = path(QStringLiteral("good.zip"));
    ASSERT_TRUE(libarchive::writeArchive(good, QStringLiteral("zip"), comicPages(), &error));
    QFile f(good);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    // Empty, truncated, garbage and randomly damaged archives of every kind: an error each
    // time, never a crash or a hang.
    writeFile(path(QStringLiteral("empty.zip")), QByteArray());
    EXPECT_FALSE(archive::listLocal(path(QStringLiteral("empty.zip"))).ok);
    writeFile(path(QStringLiteral("cut.zip")), bytes.left(bytes.size() / 2));
    archive::listLocal(path(QStringLiteral("cut.zip")));
    writeFile(path(QStringLiteral("garbage.7z")), QByteArray(4096, '\x37'));
    EXPECT_FALSE(archive::listLocal(path(QStringLiteral("garbage.7z"))).ok);
    EXPECT_FALSE(archive::listLocal(path(QStringLiteral("missing.cbz"))).ok);
    QRandomGenerator rng(42);
    for (int round = 0; round < 150; ++round) {
        QByteArray damaged = bytes;
        for (int k = 0; k < 8; ++k)
            damaged[int(rng.bounded(quint32(damaged.size())))] = char(rng.bounded(256));
        const QString p = writeFile(path(QStringLiteral("fuzz.zip")), damaged);
        const archive::Listing l = archive::listLocal(p);
        for (const archive::Member& m : l.members)
            archive::readLocal(p, m.name, &error);
    }
    SUCCEED();
}

TEST(ArchiveIpc, F_OPEN_10_ArchiveOperationsRoundTripAndAreBounded) {
    ipc::Request rq;
    rq.op = ipc::Op::ArchiveRead;
    rq.path = QStringLiteral("/a/Band.cbz");
    rq.member = QStringLiteral("Kapitel 1/01.jpg");
    const auto rq2 = ipc::decodeRequest(ipc::encode(rq));
    ASSERT_TRUE(rq2);
    EXPECT_EQ(rq2->op, ipc::Op::ArchiveRead);
    EXPECT_EQ(rq2->member, rq.member);
    ipc::Response r;
    r.ok = true;
    r.members = {{QStringLiteral("a.jpg"), 12, QDateTime::fromSecsSinceEpoch(1700000000, QTimeZone::UTC)}};
    r.data = QByteArray("bytes");
    const auto r2 = ipc::decodeResponse(ipc::encode(r));
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->members, r.members);
    EXPECT_EQ(r2->data, r.data);
    // A helper cannot hand over more than one member's limit.
    r.data = QByteArray(int(archive::kMaxMemberBytes + 1), 'x');
    EXPECT_FALSE(ipc::decodeResponse(ipc::encode(r)).has_value());
}

#ifdef LF_DECODER_HELPER_BINARY
TEST_F(ArchiveTest, F_OPEN_10_SandboxedHelperListsAndExtracts) {
    if (!archive::libArchiveAvailable()) // the test archives are written with libarchive
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString zip = path(QStringLiteral("Band.cbz"));
    ASSERT_TRUE(libarchive::writeArchive(zip, QStringLiteral("zip"), comicPages(), &error));
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.maxProcesses = 1;
    o.extraEnvironment = {QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    DecoderHelperPool pool(o);
    ipc::Request list;
    list.op = ipc::Op::ArchiveList;
    list.path = zip;
    const auto listed = pool.call(list, 20000, nullptr, &error);
    ASSERT_TRUE(listed.has_value()) << error.toStdString();
    ASSERT_TRUE(listed->ok) << listed->error.toStdString();
    EXPECT_EQ(listed->members, archive::listLocal(zip).members);
    ipc::Request read;
    read.op = ipc::Op::ArchiveRead;
    read.path = zip;
    read.member = QStringLiteral("Kapitel 2/01.png");
    const auto got = pool.call(read, 20000, nullptr, &error);
    ASSERT_TRUE(got.has_value()) << error.toStdString();
    ASSERT_TRUE(got->ok) << got->error.toStdString();
    EXPECT_EQ(got->data, comicPages()[4].second);
    if (archive::libArchiveAvailable()) {
        const QString seven = path(QStringLiteral("Band.7z"));
        ASSERT_TRUE(libarchive::writeArchive(seven, QStringLiteral("7zip"), comicPages(), &error));
        read.path = seven;
        const auto fromSeven = pool.call(read, 20000, nullptr, &error);
        ASSERT_TRUE(fromSeven.has_value()) << error.toStdString();
        EXPECT_TRUE(fromSeven->ok) << fromSeven->error.toStdString();
        EXPECT_EQ(fromSeven->data, comicPages()[4].second);
    }
}
#endif

// ---- the collection ----------------------------------------------------------------------

TEST_F(ArchiveTest, F_OPEN_10_CollectionBrowsesArchiveLikeAFolder) {
    if (!archive::libArchiveAvailable()) // the test archives are written with libarchive
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString zip = path(QStringLiteral("Band.cbz"));
    ASSERT_TRUE(libarchive::writeArchive(zip, QStringLiteral("zip"), comicPages(), &error));
    Collection c;
    int finished = 0;
    QObject::connect(&c, &Collection::loadingFinished, [&] { ++finished; });
    c.openContainer(zip);
    ASSERT_TRUE(test::waitUntil([&] { return finished > 0 && !c.isLoading(); }, 10000));
    EXPECT_TRUE(c.isContainerCollection());
    EXPECT_FALSE(c.isFolderCollection());
    EXPECT_TRUE(c.folders().isEmpty()); // never watched
    EXPECT_EQ(c.containerPath(), zip);
    EXPECT_EQ(memberNames(c.entries()),
              (QStringList{"1.png", "2.png", "10.png", "Kapitel 2/01.png", "Kapitel 10/01.png"}));
    EXPECT_EQ(c.currentIndex(), 0);
    EXPECT_TRUE(c.next(WrapMode::Stop));
    EXPECT_EQ(c.currentPath(), vpath::join(zip, QStringLiteral("2.png")));
    // Reopened with a member: that one is current at once and after the listing.
    c.openContainer(zip, QStringLiteral("Kapitel 2/01.png"));
    EXPECT_EQ(c.currentPath(), vpath::join(zip, QStringLiteral("Kapitel 2/01.png")));
    ASSERT_TRUE(test::waitUntil([&] { return finished > 1 && !c.isLoading(); }, 10000));
    EXPECT_EQ(c.currentIndex(), 3);
    // A manual sort (descending by name) applies to the member paths.
    c.setSortSpec(SortSpec{SortKey::Name, false, SortSource::Manual});
    ASSERT_TRUE(test::waitUntil([&] { return !c.isLoading(); }, 10000));
    EXPECT_EQ(memberNames(c.entries()).first(), QStringLiteral("Kapitel 10/01.png"));
    // Inserting files of a folder does not apply.
    c.insertPath(path(QStringLiteral("other.png")));
    EXPECT_FALSE(c.hasPendingInserts());
}

TEST_F(ArchiveTest, F_OPEN_10_EmptyOrBrokenArchiveReportsWhy) {
    if (!archive::libArchiveAvailable()) // the test archives are written with libarchive
        GTEST_SKIP() << "libarchive not available";
    QString error;
    const QString zip = path(QStringLiteral("text.zip"));
    ASSERT_TRUE(libarchive::writeArchive(zip, QStringLiteral("zip"),
                                         {{QStringLiteral("a.txt"), QByteArray("x")}}, &error));
    Collection c;
    QString failed;
    QObject::connect(&c, &Collection::containerFailed, [&](const QString&, const QString& e) { failed = e; });
    c.openContainer(zip);
    ASSERT_TRUE(test::waitUntil([&] { return !failed.isEmpty(); }, 10000));
    EXPECT_EQ(failed, archive::messageNoImages());
    EXPECT_EQ(c.count(), 0);
    failed.clear();
    c.openContainer(writeFile(path(QStringLiteral("broken.cbz")), QByteArray("PK\x03\x04 nope")));
    ASSERT_TRUE(test::waitUntil([&] { return !failed.isEmpty(); }, 10000));
    EXPECT_EQ(c.count(), 0);
}

// ---- what a window does with members ----------------------------------------------------

TEST(ContainerPolicy, F_OPEN_10_FileOperationsAreRefusedInArchives) {
    for (Action a :
         {Action::Trash, Action::Rename, Action::QuickMove, Action::QuickCopy, Action::SaveRotation,
          Action::Rate3, Action::RatePick, Action::OpenInEditor, Action::DragOut, Action::SetWallpaper})
        EXPECT_TRUE(containers::isBlocked(a)) << actionId(a).toStdString();
    for (Action a :
         {Action::NextImage, Action::ZoomIn, Action::CopyImage, Action::ToggleInfoPanel, Action::RotateLeft})
        EXPECT_FALSE(containers::isBlocked(a)) << actionId(a).toStdString();
    EXPECT_FALSE(containers::blockedMessage().isEmpty());
}

TEST(ContainerPolicy, F_OPEN_10_RightToLeftSwapsArrowsAndSwipesOnly) {
    InputEvent left;
    left.kind = InputKind::Key;
    left.key = Qt::Key_Left;
    InputEvent space;
    space.kind = InputKind::Key;
    space.key = Qt::Key_Space;
    InputEvent swipe;
    swipe.kind = InputKind::Swipe;
    InputEvent wheel;
    wheel.kind = InputKind::Wheel;
    EXPECT_EQ(containers::readingDirection(Action::PreviousImage, left, true), Action::NextImage);
    EXPECT_EQ(containers::readingDirection(Action::PreviousImage, left, false), Action::PreviousImage);
    EXPECT_EQ(containers::readingDirection(Action::NextImage, space, true), Action::NextImage);
    EXPECT_EQ(containers::readingDirection(Action::NextImage, swipe, true), Action::PreviousImage);
    EXPECT_EQ(containers::readingDirection(Action::NextImage, wheel, true), Action::NextImage);
    EXPECT_EQ(containers::readingDirection(Action::ZoomIn, left, true), Action::ZoomIn);
}
