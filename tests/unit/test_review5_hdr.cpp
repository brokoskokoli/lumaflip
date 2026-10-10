// SPDX-License-Identifier: GPL-3.0-or-later
// Review findings (HDR, SPEC 4.5/8.4): overlay patches without full-view copies, the loupe's
// CPU tone mapping, non-finite peaks, sanitized helper results, PFM with hdrFloat.
// Requirements: F-COL-03, F-COL-05, F-VIEW-11, F-FMT-02.

#include "core/Decoder.h"
#include "core/DecoderHelper.h"
#include "core/HdrMapping.h"
#include "core/ImagePatch.h"
#include "core/SimpleDecoders.h"

#include <QColorSpace>
#include <QColorTransform>
#include <QFile>
#include <QFloat16>
#include <QImage>
#include <QRgbaFloat>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

using namespace lf;

namespace {

QColorSpace linearP3() {
    return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear);
}

QImage hdrSolid(QSize size, float r, float g, float b) {
    QImage img(size, QImage::Format_RGBA16FPx4);
    for (int y = 0; y < img.height(); ++y) {
        auto* p = reinterpret_cast<qfloat16*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x, p += 4) {
            p[0] = qfloat16(r);
            p[1] = qfloat16(g);
            p[2] = qfloat16(b);
            p[3] = qfloat16(1.0f);
        }
    }
    img.setColorSpace(linearP3());
    return img;
}

// The widget pass on the CPU (same reference as tests/integration/tst_hdr.cpp).
QColor referenceSdr(float r, float g, float b, float contentPeak, float exposure) {
    const QColorTransform toLinearSrgb = linearP3().transformationToColorSpace(
        QColorSpace(QColorSpace::SRgb).withTransferFunction(QColorSpace::TransferFunction::Linear));
    const QRgbaFloat32 lin = toLinearSrgb.map(QRgbaFloat32{r, g, b, 1.0f});
    const hdr::ToneCurve curve = hdr::makeToneCurve(contentPeak * exposure, 1.0f);
    auto rgb = hdr::toneMapRgb(curve, {lin.r * exposure, lin.g * exposure, lin.b * exposure});
    auto enc = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    return QColor::fromRgbF(enc(rgb[0]), enc(rgb[1]), enc(rgb[2]));
}

bool close(QColor a, QColor b, int tol) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
           std::abs(a.blue() - b.blue()) <= tol;
}

void put32le(QByteArray& a, quint32 v) {
    for (int i = 0; i < 4; ++i)
        a.append(char((v >> (8 * i)) & 0xff));
}

// Little endian PFM, one color; `hot` (if finite or not) in the first pixel's red.
QByteArray pfm(int w, int h, float value, float hot) {
    QByteArray a = QStringLiteral("PF\n%1 %2\n-1.0\n").arg(w).arg(h).toLatin1();
    for (int i = 0; i < w * h; ++i)
        for (int c = 0; c < 3; ++c) {
            const float f = i == 0 && c == 0 ? hot : value;
            quint32 bits;
            std::memcpy(&bits, &f, 4);
            put32le(a, bits);
        }
    return a;
}

} // namespace

// ---- overlay patches (ImageView -> ImageCanvas, F-COL-03) ----

TEST(ImagePatch, F_COL_03_CopiesClipsAndNeverDetaches) {
    QImage dst(64, 32, QImage::Format_ARGB32_Premultiplied);
    dst.fill(Qt::transparent);
    const uchar* before = dst.constBits();
    QImage patch(10, 6, QImage::Format_ARGB32_Premultiplied);
    patch.fill(qRgba(10, 20, 30, 255));
    EXPECT_EQ(copyImagePatch(dst, patch, {5, 4}), QRect(5, 4, 10, 6));
    EXPECT_EQ(dst.constBits(), before); // written in place: no copy of the whole buffer
    EXPECT_TRUE(dst.isDetached());
    EXPECT_EQ(dst.pixel(5, 4), qRgba(10, 20, 30, 255));
    EXPECT_EQ(dst.pixel(14, 9), qRgba(10, 20, 30, 255));
    EXPECT_EQ(dst.pixel(4, 4), 0u);
    EXPECT_EQ(dst.pixel(15, 9), 0u);
    // Clipped at the edges (also with a negative position), other formats are converted.
    QImage rgb(8, 8, QImage::Format_RGB32);
    rgb.fill(qRgb(200, 0, 0));
    EXPECT_EQ(copyImagePatch(dst, rgb, {60, -3}), QRect(60, 0, 4, 5));
    EXPECT_EQ(dst.pixel(63, 0), qRgba(200, 0, 0, 255));
    EXPECT_EQ(dst.pixel(59, 0), 0u);
    EXPECT_TRUE(copyImagePatch(dst, rgb, {100, 100}).isEmpty());
    EXPECT_TRUE(copyImagePatch(dst, QImage(), {0, 0}).isEmpty());
    // A copy of the changed part (what ImageView hands over) leaves the painting buffer
    // unshared, so the next paint does not detach it.
    const QImage handedOver = dst.copy(QRect(5, 4, 10, 6));
    EXPECT_TRUE(dst.isDetached());
    EXPECT_EQ(handedOver.pixel(0, 0), qRgba(10, 20, 30, 255));
}

// ---- loupe (F-VIEW-11) with exposure and tone mapping (F-COL-03/05) ----

TEST(HdrMappingReview5, F_VIEW_11_LoupeToneMapsLikeTheWidgetPass) {
    struct Case {
        float r, g, b, peak, exposure;
    };
    for (const Case& c : {Case{4.0f, 2.0f, 1.0f, 4.0f, 1.0f}, Case{0.5f, 0.25f, 0.1f, 4.0f, 1.0f},
                          Case{8.0f, 8.0f, 8.0f, 8.0f, 1.0f}, Case{0.2f, 0.1f, 0.05f, 2.0f, 2.0f},
                          Case{3.0f, 1.0f, 0.5f, 4.0f, 0.5f}}) {
        SCOPED_TRACE(testing::Message()
                     << c.r << "," << c.g << "," << c.b << " peak " << c.peak << " exp " << c.exposure);
        const QImage out = hdr::toneMappedSdr(hdrSolid({4, 4}, c.r, c.g, c.b), c.peak, c.exposure);
        ASSERT_FALSE(out.isNull());
        EXPECT_EQ(out.format(), QImage::Format_ARGB32_Premultiplied);
        EXPECT_EQ(out.colorSpace(), QColorSpace(QColorSpace::SRgb));
        const QColor got = out.pixelColor(2, 2);
        const QColor want = referenceSdr(c.r, c.g, c.b, c.peak, c.exposure);
        EXPECT_TRUE(close(got, want, 2)) << got.name().toStdString() << " vs " << want.name().toStdString();
    }
    // Highlights are compressed, not clipped: brighter stays brighter.
    const QColor mid = hdr::toneMappedSdr(hdrSolid({2, 2}, 2.0f, 2.0f, 2.0f), 8.0f, 1.0f).pixelColor(0, 0);
    const QColor high = hdr::toneMappedSdr(hdrSolid({2, 2}, 6.0f, 6.0f, 6.0f), 8.0f, 1.0f).pixelColor(0, 0);
    EXPECT_GT(high.red(), mid.red());
    // SDR content at exposure 1: unchanged (8-bit sRGB in, the same values out).
    QImage sdr(3, 1, QImage::Format_RGB32);
    sdr.setPixel(0, 0, qRgb(0, 0, 0));
    sdr.setPixel(1, 0, qRgb(128, 64, 32));
    sdr.setPixel(2, 0, qRgb(255, 255, 255));
    const QImage same = hdr::toneMappedSdr(sdr, 1.0f, 1.0f);
    for (int x = 0; x < 3; ++x)
        EXPECT_TRUE(close(same.pixelColor(x, 0), sdr.pixelColor(x, 0), 1)) << x;
    // Exposure darkens SDR content of a float file as well (the shader multiplies first).
    const QImage darker = hdr::toneMappedSdr(sdr, 1.0f, 0.25f);
    EXPECT_LT(darker.pixelColor(2, 0).red(), 200);
}

// ---- non-finite peaks ----

TEST(HdrMappingReview5, F_COL_03_InfinitePeakIsNotIdentity) {
    const float inf = std::numeric_limits<float>::infinity();
    const hdr::ToneCurve c = hdr::makeToneCurve(inf, 1.0f);
    EXPECT_TRUE(c.active);
    EXPECT_LE(hdr::toneMap(c, 1000.0f), 1.0f);
    EXPECT_FALSE(hdr::makeToneCurve(std::numeric_limits<float>::quiet_NaN(), 1.0f).active);
    // One +inf component does not hide the real peak (ImageIO decoder uses measuredPeak).
    QImage img = hdrSolid({4, 4}, 3.0f, 1.0f, 0.5f);
    reinterpret_cast<qfloat16*>(img.scanLine(1))[4] = qfloat16(inf);
    EXPECT_FLOAT_EQ(hdr::measuredPeak(img), 3.0f);
}

// ---- helper results (SPEC 8.5) ----

TEST(DecoderHelperReview5, F_FMT_02_HelperHeadroomAndFullSizeAreSanitized) {
    auto make = [](QSize img, QSize full, float headroom) {
        DecodedImage d;
        d.image = QImage(img, QImage::Format_RGBA8888);
        d.fullSize = full;
        d.headroom = headroom;
        sanitizeHelperResult(d);
        return d;
    };
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(make({40, 30}, {40, 30}, 4.0f).headroom, 4.0f);
    EXPECT_FLOAT_EQ(make({40, 30}, {40, 30}, 64.0f).headroom, 64.0f);
    for (float bad : {inf, -inf, nan, 0.5f, 0.0f, -2.0f, 65.0f, 1e30f})
        EXPECT_FLOAT_EQ(make({40, 30}, {40, 30}, bad).headroom, 1.0f) << bad;
    // fullSize: a downsampled decode (rounded edges) keeps it ...
    EXPECT_EQ(make({400, 300}, {4000, 3000}, 1.0f).fullSize, QSize(4000, 3000));
    EXPECT_EQ(make({401, 300}, {4000, 3000}, 1.0f).fullSize, QSize(4000, 3000));
    EXPECT_EQ(make({1333, 1000}, {4000, 3001}, 1.0f).fullSize, QSize(4000, 3001));
    EXPECT_EQ(make({1, 1}, {3, 2}, 1.0f).fullSize, QSize(3, 2));
    // ... anything else becomes the image size.
    const QSize img(400, 300);
    for (QSize bad :
         {QSize(), QSize(0, 300), QSize(-4000, -3000), QSize(399, 300), QSize(400, 299),
          QSize((1 << 20) + 4, 786435), QSize(4000, 6000), QSize(4000, 2000), QSize(1 << 20, 1 << 20)})
        EXPECT_EQ(make(img, bad, 1.0f).fullSize, img) << bad.width() << "x" << bad.height();
    // Largest allowed edge with the right aspect ratio.
    EXPECT_EQ(make({1024, 768}, {1 << 20, 786432}, 1.0f).fullSize, QSize(1 << 20, 786432));
}

// ---- PFM: hdrFloat keeps the range (F-COL-05) ----

TEST(SimpleDecodersReview5, F_COL_05_PfmKeepsRangeForHdrFloat) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("a.pfm"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(pfm(4, 4, 3.0f, std::numeric_limits<float>::infinity()));
    }
    SimpleFormatDecoder dec(simplefmt::Format::Pnm);
    QString err;
    DecodeRequest req;
    req.hdrFloat = true; // SDR preview of a float source: range kept for the exposure
    const auto d = dec.decode(path, req, &err);
    ASSERT_TRUE(d.has_value()) << err.toStdString();
    EXPECT_EQ(d->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
    // The +inf sample does not hide the real peak.
    EXPECT_FLOAT_EQ(d->headroom, 3.0f);
    const auto sdr = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(sdr.has_value());
    EXPECT_FLOAT_EQ(sdr->headroom, 1.0f);
}
