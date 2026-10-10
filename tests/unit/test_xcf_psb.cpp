// SPDX-License-Identifier: GPL-3.0-or-later
// Own decoders for GIMP XCF and Photoshop PSB (SPEC 4.4 "Rest", M3). The files are built by
// the minimal writers in LayeredTestWriters.h so that every feature is covered exactly:
// XCF versions, compressions, precisions, layer modes (legacy and GIMP 2.10), opacity,
// visibility, offsets, masks, groups, gray/indexed images; PSB depths, colour modes,
// compressions and the ICC profile. Malformed input (truncation, byte flips) must fail
// cleanly. Requirement IDs: F-FMT-01 (formats), F-FMT-02 (damaged files give an error).

#include "CorpusTestUtil.h"
#include "LayeredTestWriters.h"
#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/PsbDecoder.h"
#include "core/XcfDecoder.h"

#include <QColorSpace>
#include <QFile>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtNumeric>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

using namespace lf;
using namespace lf::test;

namespace {

Rgba px(const QImage& img, int x, int y) {
    const QColor c = img.pixelColor(x, y);
    return {c.red(), c.green(), c.blue(), c.alpha()};
}

::testing::AssertionResult near(const Rgba& got, const Rgba& want, int tol = 2) {
    for (size_t i = 0; i < 4; ++i)
        if (std::abs(got[i] - want[i]) > tol)
            return ::testing::AssertionFailure()
                   << "got (" << got[0] << "," << got[1] << "," << got[2] << "," << got[3] << ") want ("
                   << want[0] << "," << want[1] << "," << want[2] << "," << want[3] << ")";
    return ::testing::AssertionSuccess();
}

xcf::Result decodeXcf(const XcfSpec& spec, int maxEdge = 0) {
    QString err;
    auto r = xcf::decode(writeXcf(spec), maxEdge, &err);
    EXPECT_TRUE(r.has_value()) << err.toStdString();
    return r ? *r : xcf::Result();
}

XcfLayerSpec solidLayer(int w, int h, int r, int g, int b, int a = 255) {
    XcfLayerSpec L;
    L.width = w;
    L.height = h;
    L.type = 1;
    L.pixels = solidRgba8(w, h, r, g, b, a);
    return L;
}

XcfSpec quadSpec(int version, int compression, int w = 64, int h = 48) {
    XcfSpec s;
    s.version = version;
    s.width = w;
    s.height = h;
    s.compression = compression;
    XcfLayerSpec L;
    L.width = w;
    L.height = h;
    L.type = 1;
    L.pixels = quadRgba8(w, h);
    s.layers << L;
    return s;
}

float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

QByteArray encode(double v, int bytes, bool isFloat) {
    QByteArray out;
    if (!isFloat) {
        const double maxv = bytes == 1 ? 255.0 : bytes == 2 ? 65535.0 : 4294967295.0;
        putBE(out, quint64(std::llround(v * maxv)), bytes);
    } else if (bytes == 2) {
        const qfloat16 h = qfloat16(float(v));
        quint16 u;
        std::memcpy(&u, &h, 2);
        putBE(out, u, 2);
    } else if (bytes == 4) {
        const float f = float(v);
        quint32 u;
        std::memcpy(&u, &f, 4);
        putBE(out, u, 4);
    } else {
        quint64 u;
        std::memcpy(&u, &v, 8);
        putBE(out, u, 8);
    }
    return out;
}

// Reference blend (perceptual, 0..1) for the layer-mode tests.
float refBlend(const QString& op, float cb, float cs) {
    auto c = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    if (op == u"multiply")
        return cb * cs;
    if (op == u"screen")
        return 1 - (1 - cb) * (1 - cs);
    if (op == u"overlay")
        return cb < 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    if (op == u"difference")
        return std::fabs(cb - cs);
    if (op == u"addition")
        return c(cb + cs);
    if (op == u"subtract")
        return c(cb - cs);
    if (op == u"darken")
        return std::min(cb, cs);
    if (op == u"lighten")
        return std::max(cb, cs);
    if (op == u"hardlight")
        return cs < 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);
    if (op == u"softlight")
        return c((1 - cb) * cb * cs + cb * (1 - (1 - cb) * (1 - cs)));
    return cs;
}

} // namespace

// ==== XCF =========================================================================================

TEST(XcfDecoder, F_FMT_01_XcfSignatureAndProbe) {
    const QByteArray data = writeXcf(quadSpec(11, 1));
    EXPECT_TRUE(xcf::matchesSignature(data.left(64)));
    EXPECT_TRUE(xcf::matchesSignature(QByteArray("gimp xcf file", 14)));
    EXPECT_FALSE(xcf::matchesSignature(QByteArray("gimp xcf v1x1", 14)));
    EXPECT_FALSE(xcf::matchesSignature(QByteArray("\x89PNG\r\n\x1a\n")));
    QString err;
    const auto p = xcf::probe(data.left(64), &err);
    ASSERT_TRUE(p.has_value()) << err.toStdString();
    EXPECT_EQ(p->size, QSize(64, 48));
    EXPECT_EQ(p->version, 11);
    EXPECT_EQ(p->bitsPerComponent, 8);
    EXPECT_EQ(p->colorModel, QStringLiteral("RGB"));
}

TEST(XcfDecoder, F_FMT_01_XcfVersionsAndCompressions) {
    // v0 ("file"), v3 (no precision field), v7, v11/v12 (64-bit offsets) x none/RLE/zlib.
    for (int version : {0, 3, 7, 10, 11, 12}) {
        for (int compression : {0, 1, 2}) {
            SCOPED_TRACE(QStringLiteral("v%1 compression %2").arg(version).arg(compression).toStdString());
            XcfSpec s = quadSpec(version, compression, 150, 100); // several tiles, edge tiles
            const auto r = decodeXcf(s);
            ASSERT_EQ(r.image.size(), QSize(150, 100));
            EXPECT_EQ(r.version, version);
            EXPECT_EQ(r.image.format(), QImage::Format_RGBA8888_Premultiplied);
            EXPECT_FALSE(r.approximate) << r.notes.join(u';').toStdString();
            expectQuadrants(r.image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
        }
    }
}

TEST(XcfDecoder, F_FMT_01_XcfPrecisions) {
    struct Case {
        int version;
        quint32 code;
        int bytes;
        bool isFloat;
        bool linear;
        int bits;
    };
    const Case cases[] = {
        {4, 1, 2, false, false, 16},   {4, 2, 4, false, true, 32},    {4, 3, 2, true, true, 16},
        {4, 4, 4, true, true, 32},     {5, 100, 1, false, true, 8},   {5, 400, 2, true, true, 16},
        {6, 550, 4, true, false, 32},  {7, 250, 2, false, false, 16}, {7, 200, 2, false, true, 16},
        {7, 350, 4, false, false, 32}, {7, 500, 2, true, true, 16},   {7, 550, 2, true, false, 16},
        {7, 600, 4, true, true, 32},   {11, 650, 4, true, false, 32}, {12, 700, 8, true, true, 64},
        {12, 750, 8, true, false, 64},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(QStringLiteral("v%1 precision %2").arg(c.version).arg(c.code).toStdString());
        XcfSpec s;
        s.version = c.version;
        s.precision = c.code;
        s.width = 70;
        s.height = 3;
        s.compression = 1;
        XcfLayerSpec L;
        L.width = 70;
        L.height = 3;
        L.type = 1;
        const float gray = 0.5f;
        const float stored = c.linear ? srgbToLinear(gray) : gray;
        for (int i = 0; i < 70 * 3; ++i) {
            for (int k = 0; k < 3; ++k)
                L.pixels.append(encode(stored, c.bytes, c.isFloat));
            L.pixels.append(encode(1.0, c.bytes, c.isFloat));
        }
        s.layers << L;
        const auto r = decodeXcf(s);
        ASSERT_FALSE(r.image.isNull());
        EXPECT_EQ(r.bitsPerComponent, c.bits);
        EXPECT_EQ(r.image.format(),
                  c.bits > 8 ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied);
        EXPECT_TRUE(near(px(r.image, 35, 1), {128, 128, 128, 255}, 2));
    }
}

TEST(XcfDecoder, F_FMT_01_XcfLayerModes) {
    struct M {
        quint32 legacy, modern;
        const char* op;
    };
    const M modes[] = {{3, 30, "multiply"},   {4, 31, "screen"},    {5, 23, "overlay"}, {6, 32, "difference"},
                       {7, 33, "addition"},   {8, 34, "subtract"},  {9, 35, "darken"},  {10, 36, "lighten"},
                       {18, 44, "hardlight"}, {19, 45, "softlight"}};
    const int cb[3] = {153, 102, 51}, cs[3] = {128, 204, 77};
    for (const M& m : modes) {
        for (quint32 mode : {m.legacy, m.modern}) {
            SCOPED_TRACE(QStringLiteral("%1 (%2)").arg(QLatin1String(m.op)).arg(mode).toStdString());
            XcfSpec s;
            s.width = 8;
            s.height = 8;
            XcfLayerSpec top = solidLayer(8, 8, cs[0], cs[1], cs[2]);
            top.mode = mode;
            s.layers << top << solidLayer(8, 8, cb[0], cb[1], cb[2]);
            const auto r = decodeXcf(s);
            Rgba want{0, 0, 0, 255};
            for (size_t i = 0; i < 3; ++i)
                want[i] =
                    int(std::lround(refBlend(QLatin1String(m.op), cb[i] / 255.0f, cs[i] / 255.0f) * 255));
            EXPECT_TRUE(near(px(r.image, 4, 4), want, 2));
            EXPECT_FALSE(r.approximate);
        }
    }
}

TEST(XcfDecoder, F_FMT_01_XcfLegacyModesClipToBackdrop) {
    // Left half of the backdrop is transparent: a legacy multiply layer adds nothing there,
    // the GIMP 2.10 multiply (union) shows the layer itself.
    XcfSpec s;
    s.width = 16;
    s.height = 4;
    XcfLayerSpec back;
    back.width = 8;
    back.height = 4;
    back.x = 8;
    back.type = 1;
    back.pixels = solidRgba8(8, 4, 200, 200, 200);
    for (quint32 mode : {3u, 30u}) {
        XcfLayerSpec top = solidLayer(16, 4, 100, 50, 25);
        top.mode = mode;
        s.layers = {top, back};
        const auto r = decodeXcf(s);
        if (mode == 3)
            EXPECT_EQ(px(r.image, 2, 2)[3], 0);
        else
            EXPECT_TRUE(near(px(r.image, 2, 2), {100, 50, 25, 255}));
        const int m = int(std::lround(200 / 255.0 * 100 / 255.0 * 255));
        EXPECT_TRUE(near(px(r.image, 12, 2),
                         {m, int(std::lround(200 / 255.0 * 50)), int(std::lround(200 / 255.0 * 25)), 255}));
    }
}

TEST(XcfDecoder, F_FMT_01_XcfOpacityVisibilityBehind) {
    XcfSpec s;
    s.width = 8;
    s.height = 8;
    XcfLayerSpec hidden = solidLayer(8, 8, 0, 255, 0);
    hidden.visible = false;
    XcfLayerSpec half = solidLayer(8, 8, 255, 255, 255);
    half.opacity = 0.5f;
    half.floatOpacity = true;
    s.layers << hidden << half << solidLayer(8, 8, 0, 0, 0);
    auto r = decodeXcf(s);
    EXPECT_TRUE(near(px(r.image, 3, 3), {128, 128, 128, 255}, 2));
    EXPECT_EQ(r.layersComposited, 2);
    // "Behind" paints under the existing pixels only.
    XcfSpec b;
    b.width = 8;
    b.height = 8;
    XcfLayerSpec behind = solidLayer(8, 8, 0, 0, 255);
    behind.mode = 29;
    XcfLayerSpec part = solidLayer(4, 8, 255, 0, 0);
    b.layers << behind << part;
    r = decodeXcf(b);
    EXPECT_TRUE(near(px(r.image, 1, 1), {255, 0, 0, 255}));
    EXPECT_TRUE(near(px(r.image, 6, 1), {0, 0, 255, 255}));
}

TEST(XcfDecoder, F_FMT_01_XcfOffsetsAndCropping) {
    XcfSpec s;
    s.width = 100;
    s.height = 80;
    XcfLayerSpec small = solidLayer(10, 10, 255, 0, 0);
    small.x = 20;
    small.y = 30;
    XcfLayerSpec negative = solidLayer(30, 30, 0, 255, 0);
    negative.x = -20;
    negative.y = -25; // covers 0..9 x 0..4 of the canvas
    XcfLayerSpec big = solidLayer(140, 100, 0, 0, 255);
    big.x = 90;
    big.y = 70; // only 10 x 10 inside
    s.layers << small << negative << big;
    const auto r = decodeXcf(s);
    ASSERT_EQ(r.image.size(), QSize(100, 80));
    EXPECT_TRUE(near(px(r.image, 25, 35), {255, 0, 0, 255}));
    EXPECT_EQ(px(r.image, 19, 35)[3], 0);
    EXPECT_EQ(px(r.image, 30, 35)[3], 0);
    EXPECT_TRUE(near(px(r.image, 5, 2), {0, 255, 0, 255}));
    EXPECT_EQ(px(r.image, 5, 6)[3], 0);
    EXPECT_TRUE(near(px(r.image, 95, 75), {0, 0, 255, 255}));
    EXPECT_EQ(px(r.image, 85, 75)[3], 0);
}

TEST(XcfDecoder, F_FMT_01_XcfLayerMasks) {
    XcfSpec s;
    s.width = 20;
    s.height = 10;
    XcfLayerSpec top = solidLayer(20, 10, 255, 0, 0);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 20; ++x)
            top.mask.append(char(x < 10 ? 255 : 0));
    s.layers << top << solidLayer(20, 10, 0, 0, 255);
    auto r = decodeXcf(s);
    EXPECT_TRUE(near(px(r.image, 3, 5), {255, 0, 0, 255}));
    EXPECT_TRUE(near(px(r.image, 15, 5), {0, 0, 255, 255}));
    // A mask that is not applied has no effect.
    s.layers[0].applyMask = false;
    r = decodeXcf(s);
    EXPECT_TRUE(near(px(r.image, 15, 5), {255, 0, 0, 255}));
    // 16-bit masks.
    XcfSpec deep = s;
    deep.version = 7;
    deep.precision = 250;
    deep.layers[0].applyMask = true;
    for (XcfLayerSpec& L : deep.layers) {
        QByteArray p16;
        for (char c : L.pixels)
            p16.append(c).append(c);
        L.pixels = p16;
        QByteArray m16;
        for (char c : L.mask)
            m16.append(c).append(c);
        L.mask = m16;
    }
    r = decodeXcf(deep);
    EXPECT_TRUE(near(px(r.image, 3, 5), {255, 0, 0, 255}));
    EXPECT_TRUE(near(px(r.image, 15, 5), {0, 0, 255, 255}));
}

TEST(XcfDecoder, F_FMT_01_XcfLayerGroups) {
    // Isolated group (normal) at 50 % with two children, over a black background.
    XcfSpec s;
    s.width = 10;
    s.height = 10;
    XcfLayerSpec group;
    group.width = 10;
    group.height = 10;
    group.type = 1;
    group.group = true;
    group.opacity = 0.5f;
    XcfLayerSpec childTop = solidLayer(5, 10, 255, 255, 255);
    childTop.path = {0, 0};
    XcfLayerSpec childBottom = solidLayer(10, 10, 255, 0, 0);
    childBottom.path = {0, 1};
    XcfLayerSpec background = solidLayer(10, 10, 0, 0, 0);
    s.layers << group << childTop << childBottom << background;
    auto r = decodeXcf(s);
    EXPECT_TRUE(near(px(r.image, 2, 5), {128, 128, 128, 255}, 2));
    EXPECT_TRUE(near(px(r.image, 7, 5), {128, 0, 0, 255}, 2));
    EXPECT_EQ(r.layersComposited, 4);
    // Pass-through: a multiply child blends with the background below the group.
    s.layers[0].mode = 61;
    s.layers[0].opacity = 1.0f;
    s.layers[2].mode = 30; // multiply
    s.layers[3] = solidLayer(10, 10, 128, 128, 128);
    r = decodeXcf(s);
    EXPECT_TRUE(near(px(r.image, 7, 5), {128, 0, 0, 255}, 2));
    // Invisible group hides its children.
    s.layers[0].visible = false;
    r = decodeXcf(s);
    EXPECT_TRUE(near(px(r.image, 7, 5), {128, 128, 128, 255}, 2));
}

TEST(XcfDecoder, F_FMT_01_XcfGrayAndIndexed) {
    XcfSpec g;
    g.version = 3;
    g.baseType = 1;
    g.width = 4;
    g.height = 4;
    XcfLayerSpec gl;
    gl.width = 4;
    gl.height = 4;
    gl.type = 3; // gray + alpha
    for (int i = 0; i < 16; ++i)
        gl.pixels.append(char(100)).append(char(255));
    g.layers << gl;
    auto r = decodeXcf(g);
    EXPECT_EQ(r.colorModel, QStringLiteral("Gray"));
    EXPECT_TRUE(near(px(r.image, 1, 1), {100, 100, 100, 255}));

    XcfSpec ix;
    ix.baseType = 2;
    ix.width = 4;
    ix.height = 2;
    ix.colormap = QByteArray("\xff\x00\x00\x00\x00\xff", 6);
    XcfLayerSpec il;
    il.width = 4;
    il.height = 2;
    il.type = 4;
    il.pixels = QByteArray("\x00\x00\x01\x01\x00\x00\x01\x01", 8);
    ix.layers << il;
    r = decodeXcf(ix);
    EXPECT_TRUE(near(px(r.image, 0, 0), {255, 0, 0, 255}));
    EXPECT_TRUE(near(px(r.image, 3, 1), {0, 0, 255, 255}));
}

TEST(XcfDecoder, F_FMT_01_XcfIccProfileAttached) {
    XcfSpec s = quadSpec(11, 2, 8, 8);
    s.icc = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    const auto r = decodeXcf(s);
    EXPECT_EQ(r.image.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
}

TEST(XcfDecoder, F_FMT_01_XcfMaxEdgeSubsamples) {
    const auto r = decodeXcf(quadSpec(11, 1, 300, 200), 100);
    EXPECT_EQ(r.fullSize, QSize(300, 200));
    EXPECT_EQ(r.image.size(), QSize(100, 67));
    expectQuadrants(r.image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 3);
}

TEST(XcfDecoder, F_FMT_01_XcfUnsupportedFeaturesApproximate) {
    XcfSpec s;
    s.width = 4;
    s.height = 4;
    XcfLayerSpec hue = solidLayer(4, 4, 10, 200, 30);
    hue.mode = 11; // HSV hue (legacy): rendered as normal
    s.layers << hue << solidLayer(4, 4, 0, 0, 0);
    auto r = decodeXcf(s);
    EXPECT_TRUE(r.approximate);
    EXPECT_FALSE(r.notes.isEmpty());
    EXPECT_TRUE(near(px(r.image, 1, 1), {10, 200, 30, 255}));
    EXPECT_FALSE(xcf::isSupportedMode(11));
    EXPECT_TRUE(xcf::isSupportedMode(28));
    // Newer versions and effect properties render what is known, marked approximate.
    XcfSpec v = quadSpec(14, 1, 8, 8);
    v.extraLayerProps << qMakePair(quint32(45), QByteArray(4, '\0'));
    r = decodeXcf(v);
    EXPECT_TRUE(r.approximate);
    expectQuadrants(r.image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
}

TEST(XcfDecoder, F_FMT_01_XcfMalformed) {
    QString err;
    EXPECT_FALSE(xcf::decode(QByteArray(), 0, &err).has_value());
    EXPECT_FALSE(xcf::decode(QByteArray("gimp xcf v011", 14), 0, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    // Unknown precision, compression, base type; zero / huge sizes.
    XcfSpec bad = quadSpec(7, 1, 8, 8);
    bad.precision = 123;
    EXPECT_FALSE(xcf::decode(writeXcf(bad), 0, &err).has_value());
    bad = quadSpec(7, 3, 8, 8);
    EXPECT_FALSE(xcf::decode(writeXcf(bad), 0, &err).has_value());
    QByteArray data = writeXcf(quadSpec(11, 1, 8, 8));
    QByteArray huge = data;
    patchBE(huge, 14, 0x7FFFFFFF, 4);
    EXPECT_FALSE(xcf::decode(huge, 0, &err).has_value());
    QByteArray zero = data;
    patchBE(zero, 18, 0, 4);
    EXPECT_FALSE(xcf::decode(zero, 0, &err).has_value());
    QByteArray type = data;
    patchBE(type, 22, 7, 4);
    EXPECT_FALSE(xcf::decode(type, 0, &err).has_value());
}

TEST(XcfDecoder, F_FMT_01_XcfFuzzTruncationAndByteFlips) {
    XcfSpec s;
    s.width = 70;
    s.height = 40;
    XcfLayerSpec masked = solidLayer(70, 40, 10, 20, 30);
    masked.mask = QByteArray(70 * 40, char(200));
    masked.x = 3;
    masked.mode = 3;
    XcfLayerSpec group;
    group.width = 70;
    group.height = 40;
    group.group = true;
    XcfLayerSpec child = solidLayer(70, 40, 200, 100, 0);
    child.path = {1, 0};
    s.layers << masked << group << child;
    for (int compression : {0, 1, 2}) {
        s.compression = compression;
        const QByteArray good = writeXcf(s);
        QString err;
        ASSERT_TRUE(xcf::decode(good, 0, &err).has_value()) << err.toStdString();
        for (qsizetype n = 0; n < good.size(); n += std::max<qsizetype>(1, good.size() / 400)) {
            const auto r = xcf::decode(good.left(n), 64, &err);
            if (!r)
                EXPECT_FALSE(err.isEmpty());
        }
        QRandomGenerator rng(4711 + compression);
        for (int i = 0; i < 1500; ++i) {
            QByteArray m = good;
            const int flips = 1 + int(rng.bounded(4));
            for (int f = 0; f < flips; ++f)
                m[qsizetype(rng.bounded(quint32(m.size())))] = char(rng.bounded(256));
            err.clear();
            const auto r = xcf::decode(m, 64, &err);
            if (r)
                EXPECT_FALSE(r->image.isNull());
            else
                EXPECT_FALSE(err.isEmpty());
        }
    }
}

TEST(XcfDecoder, F_FMT_01_XcfBackendAndRegistry) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("picture.xcf"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(writeXcf(quadSpec(11, 2, 64, 48)));
    f.close();
    EXPECT_TRUE(DecoderRegistry::instance().isSupportedExtension(QStringLiteral("xcf")));
    EXPECT_TRUE(isImageSuffix(QStringLiteral("xcf")));
    QString err;
    const auto info = DecoderRegistry::instance().info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->displaySize, QSize(64, 48));
    EXPECT_EQ(info->formatName, QStringLiteral("XCF"));
    const auto img = DecoderRegistry::instance().decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    expectQuadrants(img->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
    DecodeRequest small;
    small.maxPixelSize = 32;
    const auto preview = DecoderRegistry::instance().decode(path, small, &err);
    ASSERT_TRUE(preview.has_value());
    EXPECT_EQ(preview->image.size(), QSize(32, 24));
    EXPECT_TRUE(preview->isPreview);
    EXPECT_EQ(preview->fullSize, QSize(64, 48));
    const Metadata md = DecoderRegistry::instance().metadata(path);
    EXPECT_FALSE(md.entries.isEmpty());
}

// ==== PSB =========================================================================================

namespace {

// Planar channel data of a quadrant image: `values(x, y)` gives the channel samples.
template <typename Fn> QVector<QByteArray> planar(int w, int h, int channels, int depth, Fn&& values) {
    QVector<QByteArray> out(channels);
    for (int c = 0; c < channels; ++c)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double v = values(x, y, c);
                if (depth == 8)
                    out[c].append(char(std::lround(v * 255)));
                else if (depth == 16)
                    putBE(out[c], quint64(std::lround(v * 65535)), 2);
                else
                    out[c].append(encode(v, 4, true));
            }
    return out;
}

double quadValue(int x, int y, int c, int w, int h) {
    const bool top = y < h / 2, left = x < w / 2;
    const double rgb[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
    const int q = (top ? 0 : 2) + (left ? 0 : 1);
    return c < 3 ? rgb[q][c] : 1.0;
}

std::optional<psb::Result> decodePsb(const PsbSpec& s, int maxEdge = 0) {
    QString err;
    auto r = psb::decode(writePsb(s), maxEdge, &err);
    EXPECT_TRUE(r.has_value()) << err.toStdString();
    return r;
}

PsbSpec quadPsb(int depth, int compression, int w = 64, int h = 48, int channels = 3) {
    PsbSpec s;
    s.width = w;
    s.height = h;
    s.depth = depth;
    s.compression = compression;
    s.channels = planar(w, h, channels, depth, [&](int x, int y, int c) { return quadValue(x, y, c, w, h); });
    return s;
}

} // namespace

TEST(PsbDecoder, F_FMT_01_PsbSignatureAndProbe) {
    const QByteArray data = writePsb(quadPsb(8, 1));
    EXPECT_TRUE(psb::matchesSignature(data.left(64)));
    PsbSpec v1 = quadPsb(8, 1);
    v1.version = 1;
    const QByteArray psd = writePsb(v1);
    EXPECT_FALSE(psb::matchesSignature(psd.left(64))); // PSD is ImageIO's
    EXPECT_TRUE(psb::matchesSignature(psd.left(64), true));
    QString err;
    const auto p = psb::probe(data, &err);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->size, QSize(64, 48));
    EXPECT_EQ(p->channels, 3);
    EXPECT_EQ(p->colorModel, QStringLiteral("RGB"));
}

TEST(PsbDecoder, F_FMT_01_PsbDepthsAndCompressions) {
    for (int depth : {8, 16, 32}) {
        for (int compression : {0, 1, 2, 3}) {
            SCOPED_TRACE(QStringLiteral("depth %1 compression %2").arg(depth).arg(compression).toStdString());
            const auto r = decodePsb(quadPsb(depth, compression, 70, 50));
            ASSERT_TRUE(r.has_value());
            EXPECT_EQ(r->image.size(), QSize(70, 50));
            EXPECT_EQ(r->bitsPerComponent, depth);
            EXPECT_EQ(r->image.format(), depth > 8 ? QImage::Format_RGBA64_Premultiplied
                                                   : QImage::Format_RGBA8888_Premultiplied);
            QImage srgb = r->image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb));
            expectQuadrants(srgb, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 3);
        }
    }
    // PSD (version 1, 2-byte RLE counts) through the same parser.
    PsbSpec v1 = quadPsb(8, 1);
    v1.version = 1;
    QString err;
    const auto r = psb::decode(writePsb(v1), 0, &err);
    ASSERT_TRUE(r.has_value()) << err.toStdString();
    expectQuadrants(r->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
}

TEST(PsbDecoder, F_FMT_01_PsbAlphaUnblendGrayIndexedBitmapLab) {
    // RGBA: half transparent red, stored matted with white -> (255, 127, 127), alpha 128.
    PsbSpec rgba;
    rgba.width = 4;
    rgba.height = 4;
    rgba.channels = planar(4, 4, 4, 8, [](int, int, int c) {
        return c == 0 ? 1.0 : c == 3 ? 128 / 255.0 : 127 / 255.0;
    });
    auto r = decodePsb(rgba);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->hasAlpha);
    EXPECT_TRUE(near(px(r->image, 1, 1), {255, 0, 0, 128}, 3));

    PsbSpec gray;
    gray.mode = 1;
    gray.width = 4;
    gray.height = 4;
    gray.channels = planar(4, 4, 1, 16, [](int x, int, int) { return x < 2 ? 0.25 : 0.75; });
    gray.depth = 16;
    r = decodePsb(gray);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->colorModel, QStringLiteral("Gray"));
    EXPECT_TRUE(near(px(r->image, 0, 0), {64, 64, 64, 255}, 2));
    EXPECT_TRUE(near(px(r->image, 3, 0), {191, 191, 191, 255}, 2));

    PsbSpec indexed;
    indexed.mode = 2;
    indexed.width = 2;
    indexed.height = 1;
    indexed.colorModeData = QByteArray(768, '\0');
    indexed.colorModeData[1] = char(255);       // index 1: red
    indexed.colorModeData[512 + 0] = char(255); // index 0: blue
    indexed.channels = {QByteArray("\x00\x01", 2)};
    r = decodePsb(indexed);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(near(px(r->image, 0, 0), {0, 0, 255, 255}));
    EXPECT_TRUE(near(px(r->image, 1, 0), {255, 0, 0, 255}));

    PsbSpec bitmap;
    bitmap.mode = 0;
    bitmap.depth = 1;
    bitmap.width = 10;
    bitmap.height = 1;
    bitmap.channels = {QByteArray("\xF0\x00", 2)}; // first four black
    r = decodePsb(bitmap);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(near(px(r->image, 0, 0), {0, 0, 0, 255}));
    EXPECT_TRUE(near(px(r->image, 8, 0), {255, 255, 255, 255}));

    PsbSpec lab; // L=50, a=b=0 -> mid gray (sRGB ~119)
    lab.mode = 9;
    lab.width = 2;
    lab.height = 2;
    lab.channels = planar(2, 2, 3, 8, [](int, int, int c) { return c == 0 ? 0.5 : 128 / 255.0; });
    r = decodePsb(lab);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(near(px(r->image, 0, 0), {119, 119, 119, 255}, 3));
}

TEST(PsbDecoder, F_FMT_01_PsbCmykAndIcc) {
    // CMYK, stored inverted (255 = no ink): cyan, magenta, yellow, no ink.
    PsbSpec cmyk;
    cmyk.mode = 4;
    cmyk.width = 8;
    cmyk.height = 8;
    cmyk.channels = planar(8, 8, 4, 8, [](int x, int y, int c) {
        const int q = (y < 4 ? 0 : 2) + (x < 4 ? 0 : 1);
        const bool ink = c < 3 && c == q;
        return ink ? 0.0 : 1.0;
    });
    auto r = decodePsb(cmyk);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->colorModel, QStringLiteral("CMYK"));
    const QImage img = r->image.convertedToColorSpace(QColorSpace(QColorSpace::SRgb));
    const Rgba c = px(img, 1, 1), m = px(img, 6, 1), y = px(img, 1, 6), w = px(img, 6, 6);
    EXPECT_LT(c[0], 80);
    EXPECT_GT(c[2], 180); // cyan
    EXPECT_GT(m[0], 180);
    EXPECT_LT(m[1], 80); // magenta
    EXPECT_GT(y[1], 180);
    EXPECT_LT(y[2], 80); // yellow
    EXPECT_TRUE(near(w, {255, 255, 255, 255}, 12));

    PsbSpec tagged = quadPsb(8, 1, 8, 8);
    tagged.icc = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    r = decodePsb(tagged);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->image.colorSpace(), QColorSpace(QColorSpace::DisplayP3));

    PsbSpec notMerged = quadPsb(8, 1, 8, 8);
    notMerged.versionInfo = true;
    notMerged.realMergedData = false;
    r = decodePsb(notMerged);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->approximate);
}

TEST(PsbDecoder, F_FMT_01_PsbMaxEdgeSubsamples) {
    const auto r = decodePsb(quadPsb(8, 1, 400, 300), 100);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->fullSize, QSize(400, 300));
    EXPECT_EQ(r->image.size(), QSize(100, 75));
    expectQuadrants(r->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 3);
}

TEST(PsbDecoder, F_FMT_01_PsbMalformed) {
    QString err;
    EXPECT_FALSE(psb::decode(QByteArray("8BPS"), 0, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    PsbSpec bad = quadPsb(8, 1, 8, 8);
    bad.depth = 12;
    EXPECT_FALSE(psb::decode(writePsb(bad), 0, &err).has_value());
    bad = quadPsb(8, 1, 8, 8);
    bad.compression = 7;
    EXPECT_FALSE(psb::decode(writePsb(bad), 0, &err).has_value());
    QByteArray huge = writePsb(quadPsb(8, 1, 8, 8));
    patchBE(huge, 14, 400000, 4);
    EXPECT_FALSE(psb::decode(huge, 0, &err).has_value());
    for (int compression : {0, 1, 2, 3}) {
        const QByteArray good = writePsb(quadPsb(16, compression, 40, 30, 4));
        ASSERT_TRUE(psb::decode(good, 0, &err).has_value());
        for (qsizetype n = 0; n < good.size(); n += std::max<qsizetype>(1, good.size() / 300)) {
            const auto r = psb::decode(good.left(n), 32, &err);
            if (!r)
                EXPECT_FALSE(err.isEmpty());
        }
        QRandomGenerator rng(99 + compression);
        for (int i = 0; i < 1500; ++i) {
            QByteArray m = good;
            for (int f = 0; f < 1 + int(rng.bounded(4)); ++f)
                m[qsizetype(rng.bounded(quint32(m.size())))] = char(rng.bounded(256));
            err.clear();
            const auto r = psb::decode(m, 32, &err);
            if (!r)
                EXPECT_FALSE(err.isEmpty());
        }
    }
}

TEST(PsbDecoder, F_FMT_01_PsbBackendAndRegistry) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("large.psb"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(writePsb(quadPsb(16, 1)));
    f.close();
    EXPECT_TRUE(DecoderRegistry::instance().isSupportedExtension(QStringLiteral("psb")));
    QString err;
    const auto info = DecoderRegistry::instance().info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->formatName, QStringLiteral("PSB"));
    EXPECT_EQ(info->bitsPerComponent, 16);
    const auto img = DecoderRegistry::instance().decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(img.has_value()) << err.toStdString();
    expectQuadrants(img->image, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
}

// ==== corpus (tests/corpus/make_corpus.sh) =======================================================

TEST(LayeredDecoders, F_FMT_01_XcfPsbCorpus) {
    LF_REQUIRE_CORPUS();
    const QJsonObject group = expectedCorpus().value(QStringLiteral("layered")).toObject();
    ASSERT_FALSE(group.isEmpty());
    int checked = 0;
    for (auto it = group.begin(); it != group.end(); ++it) {
        const QString name = it.key();
        const QJsonObject e = it.value().toObject();
        SCOPED_TRACE(name.toStdString());
        const QString path = corpusFile(name);
        if (!QFileInfo::exists(path)) {
            EXPECT_TRUE(e.value(QStringLiteral("optional")).toBool()) << "required corpus file missing";
            continue;
        }
        ++checked;
        QString err;
        const auto info = DecoderRegistry::instance().info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_EQ(info->displaySize, jsonSize(e.value(QStringLiteral("displaySize"))));
        EXPECT_EQ(info->formatName, e.value(QStringLiteral("format")).toString());
        if (e.contains(QStringLiteral("bits")))
            EXPECT_EQ(info->bitsPerComponent, e.value(QStringLiteral("bits")).toInt());
        if (e.contains(QStringLiteral("colorModel")))
            EXPECT_EQ(info->colorModel, e.value(QStringLiteral("colorModel")).toString());
        const auto img = DecoderRegistry::instance().decode(path, DecodeRequest{}, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        QImage shown = img->image;
        if (e.value(QStringLiteral("colorSpace")).toString() == QLatin1String("DisplayP3"))
            EXPECT_EQ(shown.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
        expectQuadrants(shown, quadrantColors(e.value(QStringLiteral("quadrants"))),
                        e.value(QStringLiteral("tolerance")).toInt(3));
    }
    EXPECT_GE(checked, 4);
}
