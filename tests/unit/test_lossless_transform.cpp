// SPDX-License-Identifier: GPL-3.0-or-later
// Saving the rotation (F-EDIT-02): orientation algebra, EXIF patch without touching the
// compressed data, DCT rotation (libjpeg-turbo), atomic replacement keeping extended
// attributes, refusal for RAW files.

#include "CorpusTestUtil.h"
#include "core/Decoder.h"
#include "core/LosslessTransform.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cmath>

#ifdef Q_OS_MACOS
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#endif

using namespace lf;
using namespace lf::test;

namespace {

constexpr int kM[9][4] = {{1, 0, 0, 1}, {1, 0, 0, 1},  {-1, 0, 0, 1},  {-1, 0, 0, -1}, {1, 0, 0, -1},
                          {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};

// Reference: stored pixels -> displayed pixels for EXIF orientation `o` (pixel exact).
QImage applyOrientation(const QImage& img, int o) {
    const int* m = kM[o];
    const int w = img.width(), h = img.height();
    const bool swap = m[0] == 0;
    QImage out(swap ? h : w, swap ? w : h, img.format());
    double minX = 0, minY = 0;
    for (auto [cx, cy] : {std::pair{0.0, 0.0}, {double(w), 0.0}, {0.0, double(h)}, {double(w), double(h)}}) {
        minX = std::min(minX, m[0] * cx + m[1] * cy);
        minY = std::min(minY, m[2] * cx + m[3] * cy);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double X = m[0] * (x + 0.5) + m[1] * (y + 0.5) - minX;
            const double Y = m[2] * (x + 0.5) + m[3] * (y + 0.5) - minY;
            out.setPixel(int(std::floor(X)), int(std::floor(Y)), img.pixel(x, y));
        }
    return out;
}

// An image whose orientation is unambiguous (no symmetry).
QImage pattern(QSize size) {
    QImage img(size, QImage::Format_RGB32);
    QPainter p(&img);
    p.fillRect(img.rect(), Qt::white);
    const int w = size.width(), h = size.height();
    p.fillRect(0, 0, w / 2, h / 2, Qt::red);
    p.fillRect(w / 2, 0, w - w / 2, h / 2, Qt::green);
    p.fillRect(0, h / 2, w / 2, h - h / 2, Qt::blue);
    p.fillRect(w / 2, h / 2, w / 4, h / 4, Qt::black);
    return img;
}

QByteArray jpegBytes(const QImage& img, int quality = 92) {
    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", quality);
    return data;
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// Pixels as stored (no EXIF orientation applied).
QImage storedPixels(const QString& path) {
    QImageReader r(path);
    r.setAutoTransform(false);
    return r.read().convertToFormat(QImage::Format_RGB32);
}

QImage displayed(const QString& path) {
    QString error;
    auto d = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error);
    return d ? d->image.convertToFormat(QImage::Format_RGB32) : QImage();
}

int maxDiff(const QImage& a, const QImage& b) {
    if (a.size() != b.size())
        return 1000;
    int m = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            m = std::max({m, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                          std::abs(qBlue(p) - qBlue(q))});
        }
    return m;
}

int orientationOf(const QString& path) {
    QString error;
    auto info = DecoderRegistry::instance().info(path, &error);
    return info ? info->exifOrientation : -1;
}

} // namespace

// The orientation table matches the EXIF definition (checked against ImageIO's display of
// the corpus files) and compose() equals applying the view operations one after another.
TEST(Orientation, F_EDIT_02_ComposeMatchesViewOperations) {
    const QImage base = pattern(QSize(6, 4));
    for (int o = 1; o <= 8; ++o)
        for (int q = 0; q < 4; ++q)
            for (int f = 0; f < 4; ++f) {
                const bool fh = f & 1, fv = f & 2;
                QImage view = applyOrientation(base, o);
                for (int i = 0; i < q; ++i)
                    view = applyOrientation(view, 6); // rotate clockwise
                if (fh)
                    view = applyOrientation(view, 2);
                if (fv)
                    view = applyOrientation(view, 4);
                const int c = orientation::compose(o, q, fh, fv);
                EXPECT_EQ(applyOrientation(base, c), view) << o << " " << q << " " << f;
            }
    for (int o = 1; o <= 8; ++o) {
        EXPECT_EQ(orientation::multiply(o, orientation::inverse(o)), 1);
        EXPECT_EQ(orientation::swapsAxes(o), o >= 5);
    }
    EXPECT_EQ(orientation::compose(1, 1, false, false), 6);
    EXPECT_EQ(orientation::compose(1, 3, false, false), 8);
    EXPECT_EQ(orientation::compose(6, 3, false, false), 1); // ⌘L undoes an orientation of 6
}

TEST(Orientation, F_EDIT_02_TableMatchesExifDefinition) {
    LF_REQUIRE_CORPUS();
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "reference display from the platform decoder";
#endif
    for (int o = 1; o <= 8; ++o) {
        const QString path = corpusFile(QStringLiteral("orient_%1.jpg").arg(o));
        const QImage mine = applyOrientation(storedPixels(path), o);
        const QImage ref = displayed(path);
        ASSERT_EQ(mine.size(), ref.size()) << "orientation " << o;
        // Quadrant centers (JPEG edges differ between decoders).
        for (const QPoint& pt :
             {QPoint(ref.width() / 4, ref.height() / 4), QPoint(3 * ref.width() / 4, ref.height() / 4),
              QPoint(ref.width() / 4, 3 * ref.height() / 4),
              QPoint(3 * ref.width() / 4, 3 * ref.height() / 4)}) {
            const QRgb a = mine.pixel(pt), b = ref.pixel(pt);
            EXPECT_LE(std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) +
                          std::abs(qBlue(a) - qBlue(b)),
                      60)
                << "orientation " << o << " at " << pt.x() << "," << pt.y();
        }
        EXPECT_EQ(jpeg::exifOrientation(readFile(path)), o);
    }
}

// EXIF strategy on a JPEG with an orientation tag: two bytes change, the compressed
// image data stays byte-identical.
TEST(JpegLossless, F_EDIT_02_ExifPatchKeepsScanDataByteIdentical) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString path = dir.filePath("photo.jpg");
    const QByteArray original = readFile(corpusFile("orient_1.jpg"));
    ASSERT_TRUE(writeFile(path, original));

    RotationRequest r;
    r.path = path;
    r.quarterTurnsCW = 1;
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.method, RotationMethod::PatchExif);
    EXPECT_EQ(plan.newOrientation, 6);
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());

    const QByteArray after = readFile(path);
    ASSERT_EQ(after.size(), original.size());
    EXPECT_EQ(jpeg::scanData(after), jpeg::scanData(original));
    EXPECT_FALSE(jpeg::scanData(after).isEmpty());
    int differing = 0;
    for (qsizetype i = 0; i < after.size(); ++i)
        differing += after[i] != original[i];
    EXPECT_LE(differing, 2);
    EXPECT_EQ(jpeg::exifOrientation(after), 6);
    EXPECT_EQ(storedPixels(path), storedPixels(corpusFile("orient_1.jpg"))); // pixels untouched
}

// EXIF strategy on files without a tag: the platform copies the image data unchanged and
// adds the orientation (JPEG without EXIF, PNG, HEIC).
TEST(JpegLossless, F_EDIT_02_MetadataCopyKeepsImageData) {
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "lossless metadata copy is a platform feature";
#endif
    QTemporaryDir dir;
    const QImage img = pattern(QSize(64, 48));
    const QString jpg = dir.filePath("plain.jpg");
    ASSERT_TRUE(writeFile(jpg, jpegBytes(img)));
    const QByteArray before = readFile(jpg);
    ASSERT_EQ(jpeg::exifOrientation(before), 0);

    RotationRequest r;
    r.path = jpg;
    r.quarterTurnsCW = 3; // ⌘L
    RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready);
    EXPECT_EQ(plan.method, RotationMethod::MetadataCopy);
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
    EXPECT_EQ(jpeg::scanData(readFile(jpg)), jpeg::scanData(before)); // not re-encoded
    EXPECT_EQ(orientationOf(jpg), 8);
    EXPECT_EQ(displayed(jpg).size(), QSize(48, 64));

    // PNG: orientation in eXIf, pixels identical.
    const QString png = dir.filePath("plain.png");
    ASSERT_TRUE(img.save(png));
    r.path = png;
    r.quarterTurnsCW = 1;
    plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready);
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
    EXPECT_EQ(orientationOf(png), 6);
    const QImage pngPixels = storedPixels(png);
    EXPECT_EQ(pngPixels.size(), img.size());
    EXPECT_EQ(maxDiff(pngPixels, img), 0);
    EXPECT_LE(maxDiff(displayed(png), applyOrientation(img, 6)), 1);

    // HEIC (lossy): only the metadata changes, the decoded pixels stay the same.
    if (corpusAvailable()) {
        const QString heic = dir.filePath("still.heic");
        ASSERT_TRUE(QFile::copy(corpusFile("still.heic"), heic));
        const QImage beforeHeic = displayed(heic);
        r.path = heic;
        r.quarterTurnsCW = 2;
        plan = planRotationSave(r);
        ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
        EXPECT_EQ(plan.method, RotationMethod::MetadataCopy);
        ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
        EXPECT_EQ(orientationOf(heic), 3);
        EXPECT_EQ(maxDiff(displayed(heic), applyOrientation(beforeHeic, 3)), 0);
    }
}

// Strategy (2): DCT transform. Rotating there and back gives exactly the original
// coefficients (pixel-identical decode); the rotated image equals the rotated pixels.
TEST(JpegLossless, F_EDIT_02_DctRotationPixelExact) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "built without libjpeg-turbo";
    const QImage img = pattern(QSize(64, 48)); // multiples of the 16x16 MCU
    const QByteArray original = jpegBytes(img);
    QTemporaryDir dir;
    const QString origPath = dir.filePath("orig.jpg");
    ASSERT_TRUE(writeFile(origPath, original));
    const QImage decoded = storedPixels(origPath);

    for (int op = 2; op <= 8; ++op) {
        SCOPED_TRACE(op);
        QByteArray rotated, back;
        QString error;
        ASSERT_EQ(jpeg::transformDct(original, op, false, &rotated, &error), jpeg::DctResult::Ok)
            << error.toStdString();
        const QString rotPath = dir.filePath(QStringLiteral("rot%1.jpg").arg(op));
        ASSERT_TRUE(writeFile(rotPath, rotated));
        // Same pixels, moved: identical to the reference transform of the decoded original.
        EXPECT_EQ(maxDiff(storedPixels(rotPath), applyOrientation(decoded, op)), 0);
        ASSERT_EQ(jpeg::transformDct(rotated, orientation::inverse(op), false, &back, &error),
                  jpeg::DctResult::Ok);
        const QString backPath = dir.filePath(QStringLiteral("back%1.jpg").arg(op));
        ASSERT_TRUE(writeFile(backPath, back));
        EXPECT_EQ(storedPixels(backPath), decoded); // coefficients round-trip exactly
    }

    // Through the strategy: tag reset to 1, view matches.
    RotationRequest r;
    r.path = origPath;
    r.quarterTurnsCW = 1;
    r.strategy = RotationStrategy::LosslessJpeg;
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready);
    EXPECT_EQ(plan.method, RotationMethod::JpegDct);
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
    EXPECT_LE(jpeg::exifOrientation(readFile(origPath)), 1);
    EXPECT_EQ(maxDiff(storedPixels(origPath), applyOrientation(decoded, 6)), 0);
}

// Partial MCUs at the edges: not perfect -> the caller must ask; with trimming the edge
// blocks are dropped.
TEST(JpegLossless, F_EDIT_02_DctNeedsTrimForPartialBlocks) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "built without libjpeg-turbo";
    QTemporaryDir dir;
    const QString path = dir.filePath("odd.jpg");
    ASSERT_TRUE(writeFile(path, jpegBytes(pattern(QSize(70, 50)))));
    RotationRequest r;
    r.path = path;
    r.quarterTurnsCW = 1;
    r.strategy = RotationStrategy::LosslessJpeg;
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::NeedsTrim);
    EXPECT_FALSE(plan.message.isEmpty());
    const QByteArray before = readFile(path);
    ASSERT_EQ(executeRotationSave(r, plan, RotationMethod::JpegDctTrim), QString());
    const QImage after = storedPixels(path);
    EXPECT_EQ(after.size(), QSize(48, 70)); // 50 -> 48 (bottom partial row of blocks dropped)
    EXPECT_NE(readFile(path), before);
}

#if defined(Q_OS_MACOS) && !defined(LF_GENERIC_PLATFORM) // attributes are kept by the macOS layer
// The file is replaced atomically (new inode via rename) and keeps permissions, extended
// attributes incl. Finder tags and - on request - the modification date.
TEST(RotationSave, F_EDIT_02_AtomicWritePreservesXattrsAndTags) {
    LF_REQUIRE_CORPUS();
    QTemporaryDir dir;
    const QString path = dir.filePath("tagged.jpg");
    ASSERT_TRUE(QFile::copy(corpusFile("orient_1.jpg"), path));
    const QByteArray p = QFile::encodeName(path);
    ASSERT_EQ(chmod(p.constData(), 0640), 0);
    // Finder tag "Rot" (binary plist array of strings) and a custom attribute.
    const QByteArray tags =
        QByteArray::fromHex("62706c6973743030a10155526f740a36080a000000000000010100000000000000"
                            "0200000000000000000000000000000010");
    ASSERT_EQ(setxattr(p.constData(), "com.apple.metadata:_kMDItemUserTags", tags.constData(),
                       size_t(tags.size()), 0, 0),
              0);
    ASSERT_EQ(setxattr(p.constData(), "org.lumaflip.test", "hello", 5, 0, 0), 0);
    struct timespec old[2] = {{1'600'000'000, 0}, {1'600'000'000, 0}};
    ASSERT_EQ(utimensat(AT_FDCWD, p.constData(), old, 0), 0);
    struct stat before{};
    ASSERT_EQ(stat(p.constData(), &before), 0);

    RotationRequest r;
    r.path = path;
    r.quarterTurnsCW = 1;
    r.keepModificationDate = true;
    const RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready);
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());

    struct stat after{};
    ASSERT_EQ(stat(p.constData(), &after), 0);
    EXPECT_NE(after.st_ino, before.st_ino); // replaced, not written in place
    EXPECT_EQ(after.st_mode & 0777, mode_t(0640));
    EXPECT_EQ(after.st_mtimespec.tv_sec, before.st_mtimespec.tv_sec);
    char buf[256];
    const ssize_t n = getxattr(p.constData(), "com.apple.metadata:_kMDItemUserTags", buf, sizeof(buf), 0, 0);
    ASSERT_EQ(n, tags.size());
    EXPECT_EQ(QByteArray(buf, int(n)), tags);
    const ssize_t m = getxattr(p.constData(), "org.lumaflip.test", buf, sizeof(buf), 0, 0);
    EXPECT_EQ(QByteArray(buf, int(std::max<ssize_t>(0, m))), QByteArray("hello"));
    EXPECT_EQ(orientationOf(path), 6);
    // No temporary file is left behind.
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"tagged.jpg"});
}
#endif

// Minimal DNG (TIFF with a DNGVersion tag, 8x8 RGB) - a stand-in for camera RAW files.
QByteArray syntheticDng() {
    QByteArray out("II*\0", 4);
    auto u16 = [&](quint16 v) { out.append(char(v & 0xFF)).append(char(v >> 8)); };
    auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            out.append(char((v >> (8 * i)) & 0xFF));
    };
    const int n = 11, w = 8, h = 8;
    const quint32 ifdSize = 2 + n * 12 + 4, bps = 8 + ifdSize, data = bps + 6;
    u32(8);
    u16(n);
    auto entry = [&](quint16 tag, quint16 type, quint32 count, quint32 value) {
        u16(tag);
        u16(type);
        u32(count);
        u32(value);
    };
    entry(254, 4, 1, 0);
    entry(256, 3, 1, w);
    entry(257, 3, 1, h);
    entry(258, 3, 3, bps);
    entry(259, 3, 1, 1);
    entry(262, 3, 1, 2);
    entry(273, 4, 1, data);
    entry(277, 3, 1, 3);
    entry(278, 3, 1, h);
    entry(279, 4, 1, w * h * 3);
    entry(50706, 1, 4, 0x00000401); // DNGVersion 1.4.0.0
    u32(0);
    u16(8);
    u16(8);
    u16(8);
    for (int i = 0; i < w * h; ++i)
        out.append(char(200)).append(char(50)).append(char(50));
    return out;
}

// Camera RAW files are never written: the plan refuses with a message.
TEST(RotationSave, F_EDIT_02_RawRefusedWithMessage) {
#ifdef LF_GENERIC_PLATFORM
    EXPECT_TRUE(platform::isCameraRawType(QStringLiteral("public.camera-raw-image")));
#else
    EXPECT_TRUE(platform::isCameraRawType(QStringLiteral("com.adobe.raw-image")));
    EXPECT_TRUE(platform::isCameraRawType(QStringLiteral("com.canon.cr3-raw-image")));
    EXPECT_FALSE(platform::isCameraRawType(QStringLiteral("public.jpeg")));
#endif
    QTemporaryDir dir;
    QStringList files{dir.filePath("synthetic.dng")};
    ASSERT_TRUE(writeFile(files.first(), syntheticDng()));
    // Real RAW files too if the optional samples were downloaded.
    const QDir raw(corpusFile("raw"));
    for (const QString& f : raw.entryList(QDir::Files)) {
        const QString copy = dir.filePath(f);
        if (QFile::copy(raw.filePath(f), copy))
            files << copy;
    }
    for (const QString& path : files) {
        SCOPED_TRACE(path.toStdString());
        const QByteArray before = readFile(path);
        for (RotationStrategy s : {RotationStrategy::ExifOrientation, RotationStrategy::LosslessJpeg}) {
            RotationRequest r;
            r.path = path;
            r.quarterTurnsCW = 1;
            r.strategy = s;
            const RotationPlan plan = planRotationSave(r);
            EXPECT_EQ(plan.status, RotationPlan::Refused);
            EXPECT_TRUE(plan.message.contains(QStringLiteral("RAW")));
        }
        EXPECT_EQ(readFile(path), before);
    }
}

// Formats without an orientation tag: lossless codecs are rewritten with rotated pixels,
// lossy ones need the user's consent.
TEST(RotationSave, F_EDIT_02_ReencodeOnlyLosslessWithoutAsking) {
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "type detection by the platform";
#endif
    QTemporaryDir dir;
    const QImage img = pattern(QSize(20, 10));
    const QString bmp = dir.filePath("p.bmp");
    ASSERT_TRUE(img.save(bmp));
    RotationRequest r;
    r.path = bmp;
    r.quarterTurnsCW = 1;
    RotationPlan plan = planRotationSave(r);
    ASSERT_EQ(plan.status, RotationPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.method, RotationMethod::ReencodeLossless);
    ASSERT_EQ(executeRotationSave(r, plan, plan.method), QString());
    EXPECT_EQ(storedPixels(bmp), applyOrientation(img, 6));

    if (corpusAvailable() && QFileInfo::exists(corpusFile("lossy.webp"))) {
        const QString webp = dir.filePath("p.webp");
        ASSERT_TRUE(QFile::copy(corpusFile("lossy.webp"), webp));
        r.path = webp;
        plan = planRotationSave(r);
        EXPECT_TRUE(plan.status == RotationPlan::NeedsReencode || plan.status == RotationPlan::Refused);
    }
}
