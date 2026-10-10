// SPDX-License-Identifier: GPL-3.0-or-later
// Photo overlays (F-INFO-07): clipping math for SDR and HDR, focus peaking kernel on
// synthetic edges, pixel identity with the overlays off, the shader's uniform layout, AF
// point parsing (Canon, Nikon, Sony, Fujifilm, EXIF SubjectArea) and orientation.
// Compare blending (F-GRID-04): difference, blend and wipe math.

#include "ExifTestWriter.h"
#include "core/AfPoints.h"
#include "core/CompareBlend.h"
#include "core/PhotoOverlays.h"

#include <QFile>
#include <QRegularExpression>
#include <gtest/gtest.h>

#include <cstring>

using namespace lf;
using namespace lf::test;

namespace {

// 3x3 luma neighbourhoods sampled from a synthetic image at (x, y).
std::array<float, 9> neighbourhood(const QImage& img, int x, int y) {
    std::array<float, 9> l{};
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const QColor c = img.pixelColor(std::clamp(x + dx, 0, img.width() - 1),
                                            std::clamp(y + dy, 0, img.height() - 1));
            l[size_t((dy + 1) * 3 + dx + 1)] =
                overlays::luma({float(c.redF()), float(c.greenF()), float(c.blueF())});
        }
    return l;
}

QImage stepEdge(int contrast) {
    QImage img(16, 16, QImage::Format_RGB32);
    img.fill(QColor(100, 100, 100));
    for (int y = 0; y < 16; ++y)
        for (int x = 8; x < 16; ++x)
            img.setPixelColor(x, y, QColor(100 + contrast, 100 + contrast, 100 + contrast));
    return img;
}

QByteArray jpegWithExif(const QByteArray& tiff) {
    return withSegments(encodeJpeg(noisyPattern(QSize(64, 48))), exifApp1(tiff));
}

// TIFF with Make, an Exif IFD (pixel dimensions) and a maker note blob placed by `place`.
QByteArray
tiffWithMakerNote(bool little, const QByteArray& make, QSize pixels,
                  const std::function<QByteArray(TiffWriter&, qint64 makerNoteOffset)>& makerNote) {
    TiffWriter t(little);
    QVector<qint64> pos0;
    t.writeIfd({t.asciiEntry(0x010F, make), t.longEntry(0x8769, 0)}, &pos0);
    // The maker note goes after the Exif IFD; its offset is known once the IFD is written.
    QVector<qint64> posExif;
    const qint64 exif = t.writeIfd({t.longEntry(0xA002, quint32(pixels.width())),
                                    t.longEntry(0xA003, quint32(pixels.height())),
                                    t.undefinedEntry(0x927C, QByteArray(8, '\0'))},
                                   &posExif);
    t.patch32(pos0[1], quint32(exif));
    const qint64 mnOffset = (t.size() + 1) / 2 * 2;
    const QByteArray mn = makerNote(t, mnOffset);
    const qint64 placed = t.append(mn);
    EXPECT_EQ(placed, mnOffset);
    // MakerNote entry: count + offset.
    t.patch32(posExif[2] - 4, quint32(mn.size()));
    t.patch32(posExif[2], quint32(mnOffset));
    return t.bytes();
}

} // namespace

// ---- clipping ----

TEST(PhotoOverlays, F_INFO_07_ClippingThresholdsSdr) {
    overlays::Params p;
    p.clipping = true;
    p.clipHigh = 0.98f;
    p.clipLow = 0.02f;
    EXPECT_EQ(overlays::classify({1.0f, 0.2f, 0.2f}, p, 1.0f),
              overlays::Clip::Highlight); // one channel blown
    EXPECT_EQ(overlays::classify({0.5f, 0.5f, 0.5f}, p, 1.0f), overlays::Clip::None);
    EXPECT_EQ(overlays::classify({0.0f, 0.0f, 0.001f}, p, 1.0f), overlays::Clip::Shadow);
    EXPECT_EQ(overlays::classify({0.0f, 0.01f, 0.0f}, p, 1.0f),
              overlays::Clip::None); // 0.01 linear ~ 10 % sRGB
    // The threshold is an sRGB-encoded fraction: 98 % encoded = 0.955 linear.
    EXPECT_NEAR(overlays::highThreshold(p, 1.0f), overlays::srgbToLinear(0.98f), 1e-6);
    EXPECT_EQ(overlays::classify({0.96f, 0.0f, 0.0f}, p, 1.0f), overlays::Clip::Highlight);
    EXPECT_EQ(overlays::classify({0.95f, 0.0f, 0.0f}, p, 1.0f), overlays::Clip::None);
    p.clipping = false;
    EXPECT_EQ(overlays::classify({1.0f, 1.0f, 1.0f}, p, 1.0f), overlays::Clip::None);
    for (float v : {0.0f, 0.01f, 0.2f, 0.5f, 0.9f, 1.0f})
        EXPECT_NEAR(overlays::linearToSrgb(overlays::srgbToLinear(v)), v, 1e-5);
}

TEST(PhotoOverlays, F_INFO_07_ClippingHdrRelativeToHeadroomOrSdrWhite) {
    overlays::Params p;
    p.clipping = true;
    p.clipHigh = 1.0f;
    // HDR content with 4x headroom: SDR white is far from clipping relative to the headroom …
    p.hdrRelativeToHeadroom = true;
    EXPECT_EQ(overlays::classify({1.0f, 1.0f, 1.0f}, p, 4.0f), overlays::Clip::None);
    EXPECT_EQ(overlays::classify({4.0f, 1.0f, 1.0f}, p, 4.0f), overlays::Clip::Highlight);
    EXPECT_NEAR(overlays::highThreshold(p, 4.0f), 4.0f, 1e-6);
    // … but counts when measured against SDR white.
    p.hdrRelativeToHeadroom = false;
    EXPECT_EQ(overlays::classify({1.0f, 0.5f, 0.5f}, p, 4.0f), overlays::Clip::Highlight);
    EXPECT_NEAR(overlays::highThreshold(p, 4.0f), 1.0f, 1e-6);
    // SDR content ignores the reference choice; shadows are always relative to SDR white.
    p.hdrRelativeToHeadroom = true;
    EXPECT_NEAR(overlays::highThreshold(p, 1.0f), 1.0f, 1e-6);
    EXPECT_NEAR(overlays::lowThreshold(p), overlays::srgbToLinear(p.clipLow), 1e-7);
}

// ---- focus peaking ----

TEST(PhotoOverlays, F_INFO_07_FocusPeakingKernelOnSyntheticEdges) {
    const QImage flat = stepEdge(0);
    EXPECT_FLOAT_EQ(overlays::sobel(neighbourhood(flat, 8, 8)), 0.0f);
    const QImage hard = stepEdge(150), soft = stepEdge(12);
    // On the edge: a strong edge peaks at every sensitivity, a faint one only at high ones.
    EXPECT_TRUE(overlays::isPeak(neighbourhood(hard, 8, 8), 0.0f));
    EXPECT_TRUE(overlays::isPeak(neighbourhood(hard, 7, 8), 0.5f));
    EXPECT_FALSE(overlays::isPeak(neighbourhood(soft, 8, 8), 0.0f));
    EXPECT_TRUE(overlays::isPeak(neighbourhood(soft, 8, 8), 1.0f));
    // Away from the edge nothing peaks.
    EXPECT_FALSE(overlays::isPeak(neighbourhood(hard, 2, 8), 1.0f));
    // The threshold falls with the sensitivity.
    EXPECT_GT(overlays::peakingThreshold(0.0f), overlays::peakingThreshold(0.5f));
    EXPECT_GT(overlays::peakingThreshold(0.5f), overlays::peakingThreshold(1.0f));
    // Direction independent: a horizontal edge gives the same magnitude.
    const QImage rotated = hard.transformed(QTransform().rotate(90));
    EXPECT_NEAR(overlays::sobel(neighbourhood(hard, 8, 8)), overlays::sobel(neighbourhood(rotated, 7, 8)),
                1e-5);
    // The peaking color replaces the pixel; clipping wins over peaking.
    overlays::Params p;
    p.peaking = true;
    const auto n = neighbourhood(hard, 8, 8);
    const auto out = overlays::apply({0.4f, 0.4f, 0.4f}, {0.1f, 0.1f, 0.1f}, &n, p, 1.0f);
    EXPECT_FLOAT_EQ(out[0], float(p.peakingColor.redF()));
    p.clipping = true;
    const auto clipped = overlays::apply({1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, &n, p, 1.0f);
    EXPECT_FLOAT_EQ(clipped[2], float(p.highlightColor.blueF()));
}

TEST(PhotoOverlays, F_INFO_07_OverlaysOffKeepPixelsIdentical) {
    const overlays::Params off;
    EXPECT_FALSE(off.active());
    const std::array<float, 9> edge{0, 0, 1, 0, 0, 1, 0, 0, 1};
    for (int i = 0; i <= 255; ++i) {
        const float v = float(i) / 255.0f;
        const std::array<float, 3> shown{v, 1.0f - v, v * 0.5f};
        const std::array<float, 3> linear{2.0f * v, 0.0f, 0.0f};
        const auto out = overlays::apply(shown, linear, &edge, off, 4.0f);
        EXPECT_EQ(std::memcmp(out.data(), shown.data(), sizeof(shown)), 0) << i; // bit-identical
    }
}

// The shader keeps the CPU reference's uniform layout in both stages (image.vert/.frag share
// one block) and only enters the overlay code when an overlay is on.
TEST(PhotoOverlays, F_INFO_07_ShaderUniformBlocksMatch) {
    auto block = [](const QString& file) {
        QFile f(QStringLiteral(LF_SRC_DIR "/src/app/shaders/") + file);
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        const QString s = QString::fromUtf8(f.readAll());
        const qsizetype a = s.indexOf(QStringLiteral("uniform buf {"));
        const qsizetype b = s.indexOf(QStringLiteral("} ubuf;"), a);
        QString members = s.mid(a, b - a);
        members.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        return members.simplified();
    };
    const QString vert = block(QStringLiteral("image.vert")), frag = block(QStringLiteral("image.frag"));
    ASSERT_FALSE(vert.isEmpty());
    EXPECT_EQ(vert, frag);
    EXPECT_TRUE(frag.contains(QStringLiteral("vec4 overlay;")));
    EXPECT_TRUE(frag.contains(QStringLiteral("vec4 overlayColor;")));
    QFile f(QStringLiteral(LF_SRC_DIR "/src/app/shaders/image.frag"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(f.readAll());
    EXPECT_TRUE(src.contains(QStringLiteral("if (ubuf.overlay.x > 0.5 || ubuf.overlay.y > 0.5)")));
}

// ---- AF points ----

TEST(AfPoints, F_INFO_07_CanonAfInfo2) {
    for (bool little : {true, false}) {
        const QByteArray tiff =
            tiffWithMakerNote(little, "Canon", QSize(6000, 4000), [&](TiffWriter& t, qint64 at) {
                // Two AF areas on a 6000 x 4000 AF image: the first (in focus) 300 px left of and
                // 200 px above the center, the second at the center.
                const int n = 2;
                QVector<int> v{0, 1,   n, n,    6000, 4000, 6000, 4000, 100, 120, 80, 80, quint16(-300),
                               0, 200, 0, 0b01, 0b10};
                v[0] = int(v.size() * 2);
                // A maker note IFD with one entry whose data lies behind it (offsets are relative
                // to the TIFF header, so build it in place).
                QByteArray ifd;
                auto put16 = [&](QByteArray& a, int x) {
                    if (little)
                        a.append(char(x & 0xFF)).append(char((x >> 8) & 0xFF));
                    else
                        a.append(char((x >> 8) & 0xFF)).append(char(x & 0xFF));
                };
                auto put32 = [&](QByteArray& a, quint32 x) {
                    for (int k = 0; k < 4; ++k)
                        a.append(char((x >> (little ? 8 * k : 8 * (3 - k))) & 0xFF));
                };
                put16(ifd, 1);
                put16(ifd, 0x0026);
                put16(ifd, 3);
                put32(ifd, quint32(v.size()));
                put32(ifd, quint32(at + 2 + 12 + 4));
                put32(ifd, 0);
                ifd.append(t.shorts(v));
                return ifd;
            });
        const afpoints::Result r = afpoints::parse(jpegWithExif(tiff));
        ASSERT_TRUE(r.valid()) << little;
        EXPECT_EQ(r.source, QStringLiteral("Canon"));
        ASSERT_EQ(r.points.size(), 2);
        // Center (3000 - 300, 2000 - 200) (positive Y is up), 100 x 80.
        EXPECT_NEAR(r.points[0].rect.center().x(), 2700.0 / 6000.0, 1e-9);
        EXPECT_NEAR(r.points[0].rect.center().y(), 1800.0 / 4000.0, 1e-9);
        EXPECT_NEAR(r.points[0].rect.width(), 100.0 / 6000.0, 1e-9);
        EXPECT_TRUE(r.points[0].inFocus);
        EXPECT_FALSE(r.points[1].inFocus);
        EXPECT_TRUE(r.points[1].selected);
        EXPECT_NEAR(r.points[1].rect.center().x(), 0.5, 1e-9);
    }
}

TEST(AfPoints, F_INFO_07_SonyFujiNikonAndSubjectArea) {
    // Sony: "SONY DSC" header, FocusLocation = width, height, x, y.
    const QByteArray sony = tiffWithMakerNote(true, "SONY", QSize(4000, 3000), [](TiffWriter& t, qint64 at) {
        QByteArray mn("SONY DSC \0\0\0", 12);
        QByteArray entries;
        entries.append(t.u16(1));
        entries.append(t.u16(0x2027)).append(t.u16(3)).append(t.u32(4));
        entries.append(t.u32(quint32(at + 12 + 2 + 12 + 4)));
        entries.append(t.u32(0));
        entries.append(t.shorts({4000, 3000, 1000, 750}));
        return mn + entries;
    });
    afpoints::Result r = afpoints::parse(jpegWithExif(sony));
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(r.source, QStringLiteral("Sony"));
    EXPECT_NEAR(r.points[0].rect.center().x(), 0.25, 1e-9);
    EXPECT_NEAR(r.points[0].rect.center().y(), 0.25, 1e-9);

    // Fujifilm: "FUJIFILM" + offset (little endian, relative to the maker note).
    const QByteArray fuji = tiffWithMakerNote(false, "FUJIFILM", QSize(6000, 4000), [](TiffWriter&, qint64) {
        TiffWriter le(true);
        QByteArray mn("FUJIFILM", 8);
        mn.append(le.u32(12));
        mn.append(le.u16(1));
        mn.append(le.u16(0x1023)).append(le.u16(3)).append(le.u32(2));
        mn.append(le.shorts({4500, 1000}));
        mn.append(le.u32(0));
        return mn;
    });
    r = afpoints::parse(jpegWithExif(fuji));
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(r.source, QStringLiteral("Fujifilm"));
    EXPECT_NEAR(r.points[0].rect.center().x(), 0.75, 1e-9);
    EXPECT_NEAR(r.points[0].rect.center().y(), 0.25, 1e-9);

    // Nikon AFInfo2 0100, contrast-detect area: "Nikon\0" + version + embedded TIFF.
    const QByteArray nikon =
        tiffWithMakerNote(true, "NIKON CORPORATION", QSize(6000, 4000), [](TiffWriter&, qint64) {
            TiffWriter inner(false); // Nikon maker notes are big endian
            QByteArray af("0100", 4);
            af.append(char(1)).append(char(0)).append(char(0)).append(char(0)); // contrast AF on
            af.append(QByteArray(8, '\0'));
            af.append(inner.shorts({600, 400, 150, 100, 60, 40})); // image, x, y, w, h
            af.append(char(1)).append(char(0));                    // in focus
            inner.writeIfd({inner.undefinedEntry(0x00B7, af)});
            QByteArray tiff = inner.bytes();
            tiff.replace(4, 4, inner.u32(8)); // IFD at 8
            QByteArray mn("Nikon\0\x02\x10\0\0", 10);
            return mn + tiff;
        });
    r = afpoints::parse(jpegWithExif(nikon));
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(r.source, QStringLiteral("Nikon"));
    EXPECT_NEAR(r.points[0].rect.center().x(), 0.25, 1e-9);
    EXPECT_NEAR(r.points[0].rect.width(), 0.1, 1e-9);
    EXPECT_TRUE(r.points[0].inFocus);

    // EXIF SubjectArea (rectangle: center x, y, width, height), e.g. iPhone.
    TiffWriter t(false);
    QVector<qint64> pos;
    t.writeIfd({t.asciiEntry(0x010F, "Apple"), t.longEntry(0x8769, 0)}, &pos);
    const qint64 exif = t.writeIfd(
        {t.shortEntry(0x9214, {2000, 1500, 400, 300}), t.longEntry(0xA002, 4000), t.longEntry(0xA003, 3000)});
    t.patch32(pos[1], quint32(exif));
    r = afpoints::parse(jpegWithExif(t.bytes()));
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(r.source, QStringLiteral("EXIF"));
    EXPECT_EQ(r.points[0].rect, QRectF(0.45, 0.45, 0.1, 0.1));
    // The same structure as a bare TIFF (RAW files are TIFF based).
    EXPECT_TRUE(afpoints::parse(t.bytes()).valid());
    // Nothing for files without these tags, and for garbage.
    EXPECT_FALSE(afpoints::parse(encodeJpeg(noisyPattern(QSize(8, 8)))).valid());
    EXPECT_FALSE(afpoints::parse(QByteArray(100, '\x55')).valid());
}

TEST(AfPoints, F_INFO_07_OrientationOfAfPoints) {
    const QVector<afpoints::Point> stored{{QRectF(0.1, 0.2, 0.3, 0.1), true, true}};
    EXPECT_EQ(afpoints::toDisplay(stored, 1)[0].rect, stored[0].rect);
    // Orientation 6 (rotate 90° clockwise): x' = 1 - (y + h), y' = x.
    const QRectF r6 = afpoints::toDisplay(stored, 6)[0].rect;
    EXPECT_NEAR(r6.x(), 0.7, 1e-9);
    EXPECT_NEAR(r6.y(), 0.1, 1e-9);
    EXPECT_NEAR(r6.width(), 0.1, 1e-9);
    EXPECT_NEAR(r6.height(), 0.3, 1e-9);
    EXPECT_TRUE(afpoints::toDisplay(stored, 3)[0].inFocus);
}

// ---- compare blending ----

TEST(CompareBlend, F_GRID_04_DifferenceBlendAndWipeMath) {
    EXPECT_EQ(compare::differencePixel(qRgb(100, 50, 0), qRgb(90, 60, 0), 1.0), qRgb(10, 10, 0));
    EXPECT_EQ(compare::differencePixel(qRgb(100, 50, 0), qRgb(90, 60, 0), 4.0), qRgb(40, 40, 0)); // amplified
    EXPECT_EQ(compare::differencePixel(qRgb(255, 0, 0), qRgb(0, 0, 0), 4.0), qRgb(255, 0, 0));    // clipped
    EXPECT_EQ(compare::differencePixel(qRgb(7, 7, 7), qRgb(7, 7, 7), 8.0), qRgb(0, 0, 0));
    EXPECT_EQ(compare::blendPixel(qRgb(0, 100, 200), qRgb(100, 200, 0), 0.0), qRgb(0, 100, 200));
    EXPECT_EQ(compare::blendPixel(qRgb(0, 100, 200), qRgb(100, 200, 0), 1.0), qRgb(100, 200, 0));
    EXPECT_EQ(compare::blendPixel(qRgb(0, 100, 200), qRgb(100, 200, 0), 0.5), qRgb(50, 150, 100));

    QImage a(10, 2, QImage::Format_RGB32), b(10, 2, QImage::Format_RGB32);
    a.fill(qRgb(200, 0, 0));
    b.fill(qRgb(0, 0, 200));
    const QImage wipe = compare::composite(a, b, compare::BlendMode::Wipe, 0.3);
    EXPECT_EQ(wipe.pixel(2, 0), qRgb(200, 0, 0));
    EXPECT_EQ(wipe.pixel(3, 1), qRgb(0, 0, 200));
    const QImage diff = compare::composite(a, b, compare::BlendMode::Difference, 1.0);
    EXPECT_EQ(diff.pixel(5, 1), qRgb(200, 0, 200));
    EXPECT_EQ(compare::composite(a, a, compare::BlendMode::Difference, 16.0).pixel(0, 0), qRgb(0, 0, 0));
    const QImage blend = compare::composite(a, b, compare::BlendMode::Blend, 0.25);
    EXPECT_EQ(blend.pixel(9, 0), qRgb(150, 0, 50));
    // Off or mismatched sizes: the first image.
    EXPECT_EQ(compare::composite(a, b, compare::BlendMode::Off, 0.5).pixel(0, 0), a.pixel(0, 0));
    EXPECT_EQ(
        compare::composite(a, QImage(3, 3, QImage::Format_RGB32), compare::BlendMode::Blend, 0.5).pixel(0, 0),
        a.pixel(0, 0));

    QImage c = a;
    c.setPixel(4, 1, qRgb(190, 0, 0));
    const compare::DiffStats s = compare::differenceStats(a, c);
    EXPECT_EQ(s.maxDelta, 10);
    EXPECT_DOUBLE_EQ(s.changedFraction, 1.0 / 20.0);
    for (auto m : {compare::BlendMode::Off, compare::BlendMode::Difference, compare::BlendMode::Blend,
                   compare::BlendMode::Wipe})
        EXPECT_EQ(compare::blendModeFromId(compare::blendModeId(m)), m);
}
