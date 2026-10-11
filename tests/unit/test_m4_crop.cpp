// SPDX-License-Identifier: GPL-3.0-or-later
// Cropping (F-EDIT-03): crop geometry (aspect presets, handles, orientation, MCU grid),
// lossless JPEG crop (pixel exact against the decoded original), metadata and ICC kept,
// refusal for MPF/gain-map JPEGs, undo/redo, symbolic and hard link safety, "Sichern
// unter" never touching the source, exact re-encoding of lossless formats.

#include "ExifTestWriter.h"
#include "core/CropSave.h"
#include "core/Decoder.h"
#include "core/UndoBackupStore.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cmath>

#if LF_TEST_HAVE_TURBOJPEG
#include <turbojpeg.h>
#endif

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lf;
using namespace lf::test;

namespace {

QByteArray readBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

std::shared_ptr<UndoBackupStore> makeStore(const QString& root) {
    UndoBackupStore::Options o;
    o.root = root;
    o.maxBytes = qint64(1) << 30;
    o.freeFraction = 1.0;
    return std::make_shared<UndoBackupStore>(o);
}

// EXIF: orientation, pixel dimensions (LONG) in the Exif IFD and a thumbnail IFD1.
QByteArray exifWith(int orientation, QSize pixels, bool little = false) {
    TiffWriter t(little);
    QVector<qint64> pos0;
    const qint64 ifd0 =
        t.writeIfd({t.shortEntry(0x0112, {orientation}), t.longEntry(0x8769, 0)}, &pos0, 0); // patched below
    const qint64 exif = t.writeIfd(
        {t.longEntry(0xA002, quint32(pixels.width())), t.longEntry(0xA003, quint32(pixels.height()))});
    t.patch32(pos0[1], quint32(exif));
    const qint64 ifd1 = t.writeIfd({t.longEntry(0x0201, 0), t.longEntry(0x0202, 0)});
    t.patch32(ifd0 + 2 + 2 * 12, quint32(ifd1)); // IFD0's next-IFD field
    return t.bytes();
}

QByteArray iccApp2(const QByteArray& icc) {
    QByteArray payload("ICC_PROFILE\0", 12);
    payload.append(char(1)).append(char(1));
    payload.append(icc);
    return jpegSegment(0xE2, payload);
}

QByteArray xmpApp1(const QByteArray& xml) {
    return jpegSegment(0xE1, QByteArray("http://ns.adobe.com/xap/1.0/\0", 29) + xml);
}

// Payload of the first segment with `marker` whose payload starts with `id`.
QByteArray segmentPayload(const QByteArray& d, uchar marker, const QByteArray& id) {
    qsizetype pos = 2;
    while (pos + 4 <= d.size() && uchar(d[pos]) == 0xFF) {
        const uchar m = uchar(d[pos + 1]);
        if (m == 0xDA)
            break;
        const int len = (uchar(d[pos + 2]) << 8) | uchar(d[pos + 3]);
        const QByteArray payload = d.mid(pos + 4, len - 2);
        if (m == marker && payload.startsWith(id))
            return payload;
        pos += 2 + len;
    }
    return {};
}

// IFD1 offset of the EXIF block (0 = unlinked).
quint32 nextIfdOfIfd0(const QByteArray& d) {
    const QByteArray tiff = segmentPayload(d, 0xE1, QByteArray("Exif\0\0", 6)).mid(6);
    if (tiff.size() < 8)
        return 0;
    const bool little = tiff.startsWith("II");
    auto u16 = [&](int o) {
        return little ? quint16(uchar(tiff[o]) | (uchar(tiff[o + 1]) << 8))
                      : quint16((uchar(tiff[o]) << 8) | uchar(tiff[o + 1]));
    };
    auto u32 = [&](int o) {
        quint32 v = 0;
        for (int k = 0; k < 4; ++k)
            v |= quint32(uchar(tiff[o + k])) << (little ? 8 * k : 8 * (3 - k));
        return v;
    };
    const int ifd0 = int(u32(4));
    return u32(ifd0 + 2 + u16(ifd0) * 12);
}

// Displayed pixels for orientation o (reference, like the EXIF definition).
QImage oriented(const QImage& img, int o) {
    const QTransform t = crop::storedToDisplay(img.size(), o);
    QImage out(t.mapRect(QRectF(img.rect())).toAlignedRect().size(), img.format());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QPointF p = t.map(QPointF(x + 0.5, y + 0.5));
            out.setPixel(int(p.x()), int(p.y()), img.pixel(x, y));
        }
    return out;
}

} // namespace

// ---- geometry ----

TEST(Crop, F_EDIT_03_McuSnappingKeepsRightBottomEdges) {
    const QSize image(100, 80);
    EXPECT_EQ(crop::snapToMcu(QRect(17, 9, 40, 30), QSize(16, 16), image),
              QRect(QPoint(16, 0), QPoint(56, 38)));
    EXPECT_EQ(crop::snapToMcu(QRect(16, 8, 40, 30), QSize(8, 8), image), QRect(16, 8, 40, 30)); // on the grid
    EXPECT_EQ(crop::snapToMcu(QRect(7, 7, 10, 10), QSize(8, 16), image), QRect(QPoint(0, 0), QPoint(16, 16)));
    // Clipped to the image; an invalid block size leaves the rectangle as it is.
    EXPECT_EQ(crop::snapToMcu(QRect(90, 70, 40, 40), QSize(16, 16), image),
              QRect(QPoint(80, 64), QPoint(99, 79)));
    EXPECT_EQ(crop::snapToMcu(QRect(3, 5, 10, 10), QSize(), image), QRect(3, 5, 10, 10));
}

TEST(Crop, F_EDIT_03_OrientationMappingMatchesPixels) {
    const QImage stored = noisyPattern(QSize(13, 7));
    const QRect displayRect(2, 3, 4, 3);
    for (int o = 1; o <= 8; ++o) {
        const QImage shown = oriented(stored, o);
        const QRect s = crop::displayToStoredRect(displayRect, stored.size(), o);
        // Cropping the stored pixels and orienting them = cropping what is shown.
        EXPECT_EQ(oriented(stored.copy(s), o), shown.copy(displayRect)) << "orientation " << o;
        EXPECT_EQ(crop::storedToDisplayRect(s, stored.size(), o), displayRect) << o;
    }
}

TEST(Crop, F_EDIT_03_AspectPresetsAndHandles) {
    using crop::Aspect;
    EXPECT_DOUBLE_EQ(crop::aspectRatio(Aspect::Free, QSizeF(400, 300), false), 0.0);
    EXPECT_DOUBLE_EQ(crop::aspectRatio(Aspect::Original, QSizeF(400, 300), false), 4.0 / 3.0);
    EXPECT_DOUBLE_EQ(crop::aspectRatio(Aspect::R3x2, QSizeF(400, 300), true), 2.0 / 3.0); // portrait
    EXPECT_DOUBLE_EQ(crop::aspectRatio(Aspect::R16x9, QSizeF(), false), 16.0 / 9.0);
    EXPECT_DOUBLE_EQ(crop::aspectRatio(Aspect::Square, QSizeF(1, 2), true), 1.0);
    for (Aspect a :
         {Aspect::Free, Aspect::Original, Aspect::Square, Aspect::R4x3, Aspect::R3x2, Aspect::R16x9})
        EXPECT_EQ(crop::aspectFromId(crop::aspectId(a)), a);

    const QRectF bounds(0, 0, 400, 300);
    const QRectF fit = crop::fitRatio(bounds, 1.0);
    EXPECT_EQ(fit, QRectF(50, 0, 300, 300));
    // Corner drag with a ratio: ratio kept, opposite corner fixed, inside the bounds.
    const QRectF start(100, 100, 100, 100);
    const QRectF r = crop::dragRect(start, crop::Handle::BottomRight, QPointF(150, 20), bounds, 1.0);
    EXPECT_EQ(r.topLeft(), start.topLeft());
    EXPECT_NEAR(r.width() / r.height(), 1.0, 1e-9);
    EXPECT_TRUE(bounds.contains(r));
    // Free edge drag beyond the bounds stops at them; a move stays inside.
    EXPECT_EQ(crop::dragRect(start, crop::Handle::Right, QPointF(1000, 0), bounds, 0.0).right(), 400.0);
    EXPECT_EQ(crop::dragRect(start, crop::Handle::Move, QPointF(-500, 500), bounds, 0.0),
              QRectF(0, 200, 100, 100));
    // Edge drag with a ratio keeps it (height follows, centered).
    const QRectF e = crop::dragRect(start, crop::Handle::Left, QPointF(-50, 0), bounds, 3.0 / 2.0);
    EXPECT_NEAR(e.width() / e.height(), 1.5, 1e-9);
    EXPECT_EQ(e.right(), start.right());
    // A new rectangle from the anchor, never smaller than the minimum.
    const QRectF n =
        crop::dragRect({}, crop::Handle::New, QPointF(-30, -2), bounds, 0.0, 8.0, QPointF(200, 150));
    EXPECT_EQ(n, QRectF(170, 142, 30, 8));
    const auto th = crop::thirds(QRectF(0, 0, 300, 90));
    EXPECT_DOUBLE_EQ(th[0], 100.0);
    EXPECT_DOUBLE_EQ(th[3], 60.0);
}

// ---- lossless JPEG crop ----

TEST(Crop, F_EDIT_03_LosslessJpegCropPixelExact) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    for (int o : {1, 6, 3}) {
        const QString path = dir.filePath(QStringLiteral("photo%1.jpg").arg(o));
        const QImage img = noisyPattern(QSize(203, 157), quint32(o));
        const QByteArray original = withSegments(encodeJpeg(img), exifApp1(exifWith(o, img.size())));
        ASSERT_TRUE(writeBytes(path, original));
        const QSize mcu = jpeg::mcuSize(original);
        ASSERT_TRUE(mcu.isValid());

        CropRequest r;
        r.path = path;
        r.rect = QRect(21, 13, 77, 59); // displayed pixels, off the block grid
        const CropPlan plan = planCropSave(r);
        ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
        EXPECT_EQ(plan.method, CropMethod::JpegLossless);
        EXPECT_TRUE(plan.snapped);
        EXPECT_FALSE(plan.notice.isEmpty());
        EXPECT_TRUE(plan.displayRect.contains(r.rect)); // only grows
        EXPECT_EQ(plan.storedRect.x() % mcu.width(), 0);
        EXPECT_EQ(plan.storedRect.y() % mcu.height(), 0);
        EXPECT_EQ(plan.storedRect, crop::displayToStoredRect(plan.displayRect, img.size(), o));

        ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
        const QByteArray after = readBytes(path);
        // Pixel exact: the cropped file decodes to the same pixels as the cropped original
        // (nearest chroma upsampling, so block borders compare exactly).
        const QImage expected = jpeg::decodePixels(original, true).copy(plan.storedRect);
        EXPECT_EQ(jpeg::decodePixels(after, true), expected) << "orientation " << o;
        EXPECT_EQ(jpeg::frameSize(after), plan.storedRect.size());
        EXPECT_EQ(jpeg::exifOrientation(after), o);
    }
}

TEST(Crop, F_EDIT_03_MetadataIccAndXmpPreserved) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    const QString path = dir.filePath("meta.jpg");
    const QImage img = noisyPattern(QSize(160, 120));
    const QByteArray icc = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    ASSERT_FALSE(icc.isEmpty());
    const QByteArray xmp =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/"
        "22-rdf-syntax-ns#\"><rdf:Description xmp:Rating=\"4\" xmlns:xmp=\"http://ns.adobe.com/"
        "xap/1.0/\"/></rdf:RDF></x:xmpmeta>";
    const QByteArray original =
        withSegments(encodeJpeg(img), exifApp1(exifWith(6, img.size(), true)) + iccApp2(icc) + xmpApp1(xmp));
    ASSERT_TRUE(writeBytes(path, original));
    ASSERT_NE(nextIfdOfIfd0(original), 0u);

    CropRequest r;
    r.path = path;
    r.rect = QRect(16, 32, 48, 64); // displayed (orientation 6: stored is 160 x 120)
    const CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
    const QByteArray after = readBytes(path);
    EXPECT_EQ(jpeg::exifOrientation(after), 6);                          // orientation unchanged
    EXPECT_EQ(jpeg::exifPixelDimensions(after), plan.storedRect.size()); // dimensions updated
    EXPECT_EQ(nextIfdOfIfd0(after), 0u);                                 // old thumbnail unlinked
    EXPECT_EQ(segmentPayload(after, 0xE2, "ICC_PROFILE").mid(14), icc);  // ICC byte-identical
    EXPECT_TRUE(segmentPayload(after, 0xE1, "http://ns.adobe.com/xap/1.0/").contains("xmp:Rating=\"4\""));
    // The decoder sees the new size with the orientation applied.
    QString error;
    const auto info = DecoderRegistry::instance().info(path, &error);
    ASSERT_TRUE(info) << error.toStdString();
    EXPECT_EQ(info->displaySize, plan.displayRect.size());
}

TEST(Crop, F_EDIT_03_MpfJpegRefusesLosslessCrop) {
    QTemporaryDir dir;
    const QString path = dir.filePath("ultrahdr.jpg");
    const QByteArray primary = encodeJpeg(noisyPattern(QSize(96, 64)));
    const QByteArray mpf = jpegSegment(0xE2, QByteArray("MPF\0", 4) + QByteArray("MM\0*\0\0\0\x08\0\0", 10));
    const QByteArray original = withSegments(primary, mpf) + encodeJpeg(noisyPattern(QSize(24, 16), 3));
    ASSERT_TRUE(jpeg::hasAdditionalImages(original));
    ASSERT_TRUE(writeBytes(path, original));
    CropRequest r;
    r.path = path;
    r.rect = QRect(8, 8, 40, 30);
    const CropPlan plan = planCropSave(r);
    EXPECT_EQ(plan.status, CropPlan::NeedsReencode); // asked first, never silently
    EXPECT_EQ(plan.method, CropMethod::ReencodeLossy);
    EXPECT_TRUE(plan.message.contains(QStringLiteral("Gain-Map")));
    // The lossless path refuses even when forced; the file stays untouched.
    CropPlan forced = plan;
    forced.storedRect = QRect(8, 8, 40, 30);
    EXPECT_FALSE(executeCropSave(r, forced, CropMethod::JpegLossless).isEmpty());
    EXPECT_EQ(readBytes(path), original);
}

TEST(Crop, F_EDIT_03_UndoRedoRestoresOriginal) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    const QString path = dir.filePath("undo.jpg");
    const QByteArray original = encodeJpeg(noisyPattern(QSize(128, 96)));
    ASSERT_TRUE(writeBytes(path, original));
    CropRequest r;
    r.path = path;
    r.rect = QRect(16, 16, 64, 48);
    const CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready);
    auto edit = makeCropEdit(r, plan, plan.method, makeStore(dir.filePath("undo-store")));
    ASSERT_EQ(edit->apply(), QString());
    const QByteArray cropped = readBytes(path);
    EXPECT_EQ(jpeg::frameSize(cropped), QSize(64, 48));
    ASSERT_EQ(edit->revert(), QString());
    EXPECT_EQ(readBytes(path), original);
    ASSERT_EQ(edit->apply(), QString());
    EXPECT_EQ(readBytes(path), cropped);
    // An outside change blocks undo (the change is kept).
    ASSERT_TRUE(writeBytes(path, cropped + "x"));
    EXPECT_EQ(edit->revert(), FileEdit::messageChangedUndo());
    EXPECT_TRUE(readBytes(path).endsWith("x"));
}

#ifdef Q_OS_UNIX
TEST(Crop, F_EDIT_03_SymlinkAndHardlinkSafety) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    const QByteArray original = encodeJpeg(noisyPattern(QSize(96, 96)));
    // Symbolic link: the real file is cropped, the link stays a link.
    const QString real = dir.filePath("real.jpg"), link = dir.filePath("link.jpg");
    ASSERT_TRUE(writeBytes(real, original));
    ASSERT_TRUE(QFile::link(real, link));
    CropRequest r;
    r.path = link;
    r.rect = QRect(0, 0, 48, 48);
    CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.target, QFileInfo(real).canonicalFilePath());
    auto edit = makeCropEdit(r, plan, plan.method, makeStore(dir.filePath("store")));
    ASSERT_EQ(edit->apply(), QString());
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(jpeg::frameSize(readBytes(real)), QSize(48, 48));
    ASSERT_EQ(edit->revert(), QString());
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(readBytes(real), original);

    // Hard links: same inode, every name sees the crop.
    const QString a = dir.filePath("a.jpg"), b = dir.filePath("b.jpg");
    ASSERT_TRUE(writeBytes(a, original));
    ASSERT_EQ(::link(QFile::encodeName(a).constData(), QFile::encodeName(b).constData()), 0);
    struct stat before{};
    ::stat(QFile::encodeName(a).constData(), &before);
    r.path = a;
    plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready);
    ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
    struct stat after{};
    ::stat(QFile::encodeName(b).constData(), &after);
    EXPECT_EQ(after.st_ino, before.st_ino);
    EXPECT_EQ(after.st_nlink, 2);
    EXPECT_EQ(jpeg::frameSize(readBytes(b)), QSize(48, 48));
}
#endif

TEST(Crop, F_EDIT_03_SaveAsNeverTouchesSource) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    const QString src = dir.filePath("source.jpg");
    const QByteArray original = encodeJpeg(noisyPattern(QSize(80, 64)));
    ASSERT_TRUE(writeBytes(src, original));
    CropRequest r;
    r.path = src;
    r.rect = QRect(8, 8, 32, 32);
    r.saveAs = dir.filePath("copy.jpg");
    const CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
    EXPECT_EQ(readBytes(src), original);
    EXPECT_EQ(jpeg::frameSize(readBytes(r.saveAs)), plan.storedRect.size());
    EXPECT_TRUE(plan.storedRect.contains(QRect(8, 8, 32, 32)));
    // The source as "save as" target - directly or through a link - is refused.
    r.saveAs = src;
    EXPECT_EQ(planCropSave(r).status, CropPlan::Refused);
#ifdef Q_OS_UNIX
    const QString link = dir.filePath("alias.jpg");
    ASSERT_TRUE(QFile::link(src, link));
    r.saveAs = link;
    EXPECT_EQ(planCropSave(r).status, CropPlan::Refused);
#endif
    EXPECT_EQ(readBytes(src), original);
}

TEST(Crop, F_EDIT_03_ChangeBetweenPlanAndSaveIsNotOverwritten) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    const QString path = dir.filePath("p.jpg");
    ASSERT_TRUE(writeBytes(path, encodeJpeg(noisyPattern(QSize(64, 64)))));
    CropRequest r;
    r.path = path;
    r.rect = QRect(0, 0, 32, 32);
    const CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready);
    const QByteArray changed = encodeJpeg(noisyPattern(QSize(64, 64), 99));
    ASSERT_TRUE(writeBytes(path, changed));
    EXPECT_FALSE(executeCropSave(r, plan, plan.method).isEmpty());
    EXPECT_EQ(readBytes(path), changed);
    // Nothing left behind next to the file.
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{"p.jpg"});
}

TEST(Crop, F_EDIT_03_PngReencodedExactlyWithProfile) {
    QTemporaryDir dir;
    const QString path = dir.filePath("alpha.png");
    QImage img(57, 43, QImage::Format_RGBA64);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            img.setPixelColor(x, y,
                              QColor::fromRgba64(quint16(x * 1100 + 7), quint16(y * 1500 + 3),
                                                 quint16((x * y * 37) & 0xFFFF), quint16(40000 + x * 300)));
    img.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    ASSERT_TRUE(img.save(path, "PNG"));
    CropRequest r;
    r.path = path;
    r.rect = QRect(5, 7, 30, 20);
    const CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    EXPECT_EQ(plan.method, CropMethod::ReencodeLossless);
    EXPECT_FALSE(plan.snapped);
    EXPECT_EQ(plan.storedRect, r.rect);
    ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
    QImageReader back(path);
    const QImage out = back.read();
    ASSERT_EQ(out.size(), QSize(30, 20));
    EXPECT_EQ(out.convertToFormat(QImage::Format_RGBA64),
              img.copy(r.rect).convertToFormat(QImage::Format_RGBA64));
    EXPECT_EQ(out.colorSpace().primaries(), QColorSpace::Primaries::DciP3D65);
}

TEST(Crop, F_EDIT_03_RefusedForMembersMissingAndWholeImage) {
    QTemporaryDir dir;
    CropRequest r;
    r.path = dir.filePath("comics.cbz#/001.jpg");
    r.rect = QRect(0, 0, 10, 10);
    EXPECT_EQ(planCropSave(r).status, CropPlan::Refused);
    r.path = dir.filePath("missing.jpg");
    EXPECT_EQ(planCropSave(r).status, CropPlan::Refused);
    const QString path = dir.filePath("whole.jpg");
    ASSERT_TRUE(writeBytes(path, encodeJpeg(noisyPattern(QSize(40, 30)))));
    r.path = path;
    r.rect = QRect(-5, -5, 100, 100); // clipped to the whole image: nothing to crop
    const CropPlan whole = planCropSave(r);
    EXPECT_EQ(whole.status, CropPlan::Refused);
    EXPECT_FALSE(whole.message.isEmpty());
}

// ---- review M4: metadata of the cut-away part, more JPEG layouts, orientation pixels ----

namespace {

QByteArray irbBlock(quint16 id, const QByteArray& data) {
    QByteArray b("8BIM");
    b.append(char(id >> 8)).append(char(id & 0xFF));
    b.append(char(0)).append(char(0)); // empty Pascal name, padded to an even length
    const quint32 n = quint32(data.size());
    for (int k = 3; k >= 0; --k)
        b.append(char((n >> (8 * k)) & 0xFF));
    b.append(data);
    if (data.size() % 2)
        b.append(char(0));
    return b;
}

// EXIF with an IFD1 thumbnail that holds `thumbnail`.
QByteArray exifWithThumbnail(QSize pixels, const QByteArray& thumbnail) {
    TiffWriter t(false);
    QVector<qint64> pos0;
    const qint64 ifd0 = t.writeIfd({t.shortEntry(0x0112, {1}), t.longEntry(0x8769, 0)}, &pos0, 0);
    const qint64 exif = t.writeIfd(
        {t.longEntry(0xA002, quint32(pixels.width())), t.longEntry(0xA003, quint32(pixels.height()))});
    t.patch32(pos0[1], quint32(exif));
    const qint64 thumb = t.append(thumbnail);
    const qint64 ifd1 =
        t.writeIfd({t.longEntry(0x0201, quint32(thumb)), t.longEntry(0x0202, quint32(thumbnail.size()))});
    t.patch32(ifd0 + 2 + 2 * 12, quint32(ifd1));
    return t.bytes();
}

double meanDifference(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull())
        return 1e9;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    for (int j = 0; j < x.height(); ++j)
        for (int i = 0; i < x.width(); ++i) {
            const QRgb p = x.pixel(i, j), q = y.pixel(i, j);
            sum +=
                std::abs(qRed(p) - qRed(q)) + std::abs(qGreen(p) - qGreen(q)) + std::abs(qBlue(p) - qBlue(q));
        }
    return sum / (3.0 * x.width() * x.height());
}

} // namespace

// Privacy: the thumbnails would still show what was cut away - their bytes are gone after
// the lossless crop (EXIF IFD1 and Photoshop IRB), other IRB resources stay, and the XMP
// pixel dimensions follow the crop like the EXIF ones.
TEST(Crop, F_EDIT_03_ThumbnailsRemovedAndXmpDimensionsUpdated) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    const QString path = dir.filePath("thumbs.jpg");
    const QImage img = noisyPattern(QSize(160, 120));
    const QByteArray exifThumb = "\xFF\xD8 EXIF-THUMBNAIL-SECRET \xFF\xD9";
    const QByteArray irb = QByteArray("Photoshop 3.0\0", 14) + irbBlock(0x0404, "IPTC-KEEP-ME") +
                           irbBlock(0x040C, "IRB-THUMBNAIL-SECRET") + irbBlock(0x0409, "OLD-IRB-THUMB");
    const QByteArray xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><rdf:Description "
                           "exif:PixelXDimension=\"160\" exif:PixelYDimension='120'>"
                           "<tiff:ImageWidth>160</tiff:ImageWidth><tiff:ImageLength> 120 </tiff:ImageLength>"
                           "</rdf:Description></rdf:RDF></x:xmpmeta>";
    const QByteArray original =
        withSegments(encodeJpeg(img), exifApp1(exifWithThumbnail(img.size(), exifThumb)) +
                                          jpegSegment(0xED, irb) + xmpApp1(xmp));
    ASSERT_TRUE(writeBytes(path, original));
    ASSERT_TRUE(original.contains("EXIF-THUMBNAIL-SECRET"));

    CropRequest r;
    r.path = path;
    r.rect = QRect(32, 16, 64, 48);
    const CropPlan plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    ASSERT_EQ(plan.method, CropMethod::JpegLossless);
    ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
    const QByteArray after = readBytes(path);
    EXPECT_FALSE(after.contains("EXIF-THUMBNAIL-SECRET"));
    EXPECT_FALSE(after.contains("IRB-THUMBNAIL-SECRET"));
    EXPECT_FALSE(after.contains("OLD-IRB-THUMB"));
    EXPECT_TRUE(after.contains("IPTC-KEEP-ME"));
    EXPECT_EQ(nextIfdOfIfd0(after), 0u);
    EXPECT_EQ(jpeg::exifPixelDimensions(after), QSize(64, 48));
    const QByteArray x = segmentPayload(after, 0xE1, "http://ns.adobe.com/xap/1.0/");
    EXPECT_TRUE(x.contains("exif:PixelXDimension=\"64\"")) << x.toStdString();
    EXPECT_TRUE(x.contains("exif:PixelYDimension='48'")) << x.toStdString();
    EXPECT_TRUE(x.contains("<tiff:ImageWidth>64</tiff:ImageWidth>")) << x.toStdString();
    EXPECT_TRUE(x.contains("<tiff:ImageLength> 48 </tiff:ImageLength>")) << x.toStdString();
    EXPECT_EQ(jpeg::decodePixels(after, true), jpeg::decodePixels(original, true).copy(plan.storedRect));

    // The helpers on their own: nothing to remove is reported as such.
    QByteArray plain = encodeJpeg(img);
    EXPECT_FALSE(jpeg::removeExifThumbnail(plain));
    EXPECT_EQ(jpeg::removeIrbThumbnails(plain), 0);
    EXPECT_EQ(plain, encodeJpeg(img));
}

#if LF_TEST_HAVE_TURBOJPEG
namespace {
QByteArray encodeTurbo(const QImage& img, int subsamp, bool progressive, int restartRows) {
    tjhandle h = tj3Init(TJINIT_COMPRESS);
    if (!h)
        return {};
    tj3Set(h, TJPARAM_SUBSAMP, subsamp);
    tj3Set(h, TJPARAM_QUALITY, 90);
    tj3Set(h, TJPARAM_PROGRESSIVE, progressive ? 1 : 0);
    if (restartRows > 0)
        tj3Set(h, TJPARAM_RESTARTROWS, restartRows);
    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    unsigned char* buf = nullptr;
    size_t size = 0;
    QByteArray out;
    if (tj3Compress8(h, rgb.constBits(), rgb.width(), int(rgb.bytesPerLine()), rgb.height(), TJPF_RGB, &buf,
                     &size) == 0)
        out = QByteArray(reinterpret_cast<const char*>(buf), qsizetype(size));
    tj3Free(buf);
    tj3Destroy(h);
    return out;
}
} // namespace

// The lossless crop beyond 4:2:0 baseline: 4:4:4, 4:2:2, progressive scans, restart markers,
// and selections that end at a right/bottom edge which is not a multiple of the block size.
TEST(Crop, F_EDIT_03_LosslessCropSubsamplingProgressiveRestartAndEdges) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    struct Variant {
        const char* name;
        int subsamp;
        bool progressive;
        int restartRows;
        QSize mcu;
    };
    const Variant variants[] = {{"444", TJSAMP_444, false, 0, QSize(8, 8)},
                                {"422", TJSAMP_422, false, 0, QSize(16, 8)},
                                {"420-progressive", TJSAMP_420, true, 0, QSize(16, 16)},
                                {"420-restart", TJSAMP_420, false, 1, QSize(16, 16)},
                                {"422-progressive-restart", TJSAMP_422, true, 2, QSize(16, 8)}};
    const QSize size(203, 157); // neither a multiple of 8 nor of 16
    const QRect rects[] = {QRect(21, 13, 77, 59), QRect(150, 100, 53, 57), // up to the right and bottom edge
                           QRect(0, 141, 203, 16)};                        // the partial bottom block row
    QTemporaryDir dir;
    int n = 0;
    for (const Variant& v : variants) {
        const QImage img = noisyPattern(size, quint32(++n));
        const QByteArray original = encodeTurbo(img, v.subsamp, v.progressive, v.restartRows);
        ASSERT_FALSE(original.isEmpty()) << v.name;
        ASSERT_EQ(jpeg::mcuSize(original), v.mcu) << v.name;
        for (const QRect& rect : rects) {
            const QString path =
                dir.filePath(QStringLiteral("%1-%2.jpg").arg(QLatin1String(v.name)).arg(n++));
            ASSERT_TRUE(writeBytes(path, original));
            CropRequest r;
            r.path = path;
            r.rect = rect;
            const CropPlan plan = planCropSave(r);
            ASSERT_EQ(plan.status, CropPlan::Ready) << v.name << " " << plan.message.toStdString();
            ASSERT_EQ(plan.method, CropMethod::JpegLossless) << v.name;
            EXPECT_EQ(plan.storedRect.right(), rect.right()) << v.name; // right/bottom edges stay
            EXPECT_EQ(plan.storedRect.bottom(), rect.bottom()) << v.name;
            ASSERT_EQ(executeCropSave(r, plan, plan.method), QString()) << v.name;
            const QByteArray after = readBytes(path);
            EXPECT_EQ(jpeg::frameSize(after), plan.storedRect.size()) << v.name;
            EXPECT_EQ(jpeg::decodePixels(after, true),
                      jpeg::decodePixels(original, true).copy(plan.storedRect))
                << v.name << " " << rect.x() << "," << rect.y();
        }
    }
}
#endif

// With an EXIF orientation the saved crop shows exactly the selected displayed pixels (the
// decoder applies the orientation; compared by content, not only by size).
TEST(Crop, F_EDIT_03_OrientedCropShowsTheSelectedPixels) {
    if (!jpeg::dctTransformAvailable())
        GTEST_SKIP() << "libjpeg-turbo not linked";
    QTemporaryDir dir;
    for (int o : {3, 5, 6, 8}) {
        const QString path = dir.filePath(QStringLiteral("o%1.jpg").arg(o));
        const QImage img = noisyPattern(QSize(128, 96), quint32(40 + o));
        const QByteArray original = withSegments(encodeJpeg(img, 95), exifApp1(exifWith(o, img.size())));
        ASSERT_TRUE(writeBytes(path, original));
        QString error;
        const auto before = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error);
        ASSERT_TRUE(before) << error.toStdString();
        CropRequest r;
        r.path = path;
        r.rect = QRect(16, 32, 48, 40); // displayed pixels (snapped where needed: plan.displayRect)
        const CropPlan plan = planCropSave(r);
        ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
        ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
        const auto after = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error);
        ASSERT_TRUE(after) << error.toStdString();
        const QImage expected = before->image.copy(plan.displayRect);
        ASSERT_EQ(after->image.size(), expected.size()) << "orientation " << o;
        // Same pixels up to chroma upsampling at the new edges; a mirrored selection is far off.
        EXPECT_LT(meanDifference(after->image, expected), 4.0) << "orientation " << o;
        EXPECT_GT(meanDifference(after->image, expected.flipped(Qt::Horizontal)), 20.0)
            << "orientation " << o;
    }
}

// "Sichern unter" replaces an existing file only with the user's consent; a file that
// appears after planning is never replaced.
TEST(Crop, F_EDIT_03_SaveAsReplacesOnlyWhenAgreed) {
    QTemporaryDir dir;
    const QString src = dir.filePath("src.png");
    ASSERT_TRUE(noisyPattern(QSize(60, 40)).save(src, "PNG"));
    CropRequest r;
    r.path = src;
    r.rect = QRect(5, 5, 20, 10);
    r.saveAs = dir.filePath("taken.png");
    ASSERT_TRUE(writeBytes(r.saveAs, "old"));
    CropPlan plan = planCropSave(r);
    EXPECT_EQ(plan.status, CropPlan::Refused);
    EXPECT_TRUE(plan.message.contains(QStringLiteral("taken.png")));
    EXPECT_EQ(readBytes(r.saveAs), QByteArray("old"));
    r.replaceExisting = true;
    plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    ASSERT_EQ(executeCropSave(r, plan, plan.method), QString());
    EXPECT_EQ(QImage(r.saveAs).size(), QSize(20, 10));

    r.replaceExisting = false;
    r.saveAs = dir.filePath("late.png");
    plan = planCropSave(r);
    ASSERT_EQ(plan.status, CropPlan::Ready) << plan.message.toStdString();
    ASSERT_TRUE(writeBytes(r.saveAs, "late"));
    EXPECT_FALSE(executeCropSave(r, plan, plan.method).isEmpty());
    EXPECT_EQ(readBytes(r.saveAs), QByteArray("late"));
}

// What cannot be cropped in place is offered as a cropped export (and says so), never as a
// question that would end in an error.
TEST(Crop, F_EDIT_03_RefusalsOfferTheCroppedExport) {
    QTemporaryDir dir;
    CropRequest r;
    r.rect = QRect(0, 0, 10, 10);
    r.path = dir.filePath("comics.cbz#/001.jpg");
    CropPlan plan = planCropSave(r);
    EXPECT_EQ(plan.status, CropPlan::Refused);
    EXPECT_TRUE(plan.exportInstead);
    const QString raw = dir.filePath("camera.cr2");
    ASSERT_TRUE(writeBytes(raw, encodeJpeg(noisyPattern(QSize(40, 30)))));
    r.path = raw;
    plan = planCropSave(r);
    EXPECT_EQ(plan.status, CropPlan::Refused);
    EXPECT_TRUE(plan.exportInstead) << plan.message.toStdString();
    r.path = dir.filePath("missing.jpg");
    EXPECT_FALSE(planCropSave(r).exportInstead);
    EXPECT_TRUE(platform::canEncodeType(QStringLiteral("public.png")));
    EXPECT_FALSE(platform::canEncodeType(QStringLiteral("generic.no-such-format")));
}
