// SPDX-License-Identifier: GPL-3.0-or-later
// Own decoders of the M3 "Rest" format group (SPEC 4.4): QOI, PCX, PNM/PAM/PFM, SGI,
// OpenRaster, Krita; the ZIP reader they use; malformed input (fuzz-style truncation and
// byte flips) must fail cleanly. Requirement IDs: F-FMT-01 (formats by signature), F-FMT-02
// (damaged files give an error, never a crash).

#include "CorpusTestUtil.h"
#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/SimpleDecoders.h"
#include "core/ZipReader.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <zlib.h>

#ifdef Q_OS_DARWIN
#include <malloc/malloc.h>
#endif

using namespace lf;
using namespace lf::test;
using simplefmt::Format;

namespace {

// ---- tiny encoders for the tests -------------------------------------------------------

void put16be(QByteArray& a, int v) {
    a.append(char((v >> 8) & 255));
    a.append(char(v & 255));
}
void put32be(QByteArray& a, quint32 v) {
    for (int s = 24; s >= 0; s -= 8)
        a.append(char((v >> s) & 255));
}
void put16le(QByteArray& a, int v) {
    a.append(char(v & 255));
    a.append(char((v >> 8) & 255));
}
void put32le(QByteArray& a, quint32 v) {
    for (int s = 0; s <= 24; s += 8)
        a.append(char((v >> s) & 255));
}

QRgb quadColor(int x, int y, int w, int h) {
    const bool left = x < w / 2, top = y < h / 2;
    if (top && left)
        return qRgb(255, 0, 0);
    if (top)
        return qRgb(0, 255, 0);
    if (left)
        return qRgb(0, 0, 255);
    return qRgb(255, 255, 255);
}

void expectQuad(const QImage& img) {
    expectQuadrants(img, quadrantColors(QJsonValue(QStringLiteral("rgbw"))), 2);
}

// QOI with every op: RGB, RGBA, INDEX, DIFF, LUMA, RUN.
QByteArray qoiAllOps() {
    QByteArray a("qoif");
    put32be(a, 4);
    put32be(a, 2);
    a.append(char(4));
    a.append(char(0));
    a.append(char(0xFE)).append(char(10)).append(char(20)).append(char(30)); // (10,20,30,255)
    a.append(char(0x40 | (3 << 4) | (1 << 2) | 2));                          // diff +1,-1,0
    a.append(char(0x80 | (32 + 5))).append(char(((8 + 1) << 4) | (8 - 1)));  // luma g+5, r+6, b+4
    a.append(char(0xFF)).append(char(1)).append(char(2)).append(char(3)).append(char(4)); // rgba
    a.append(char(0xC0 | 2));                                                             // run 3
    const int idx = (10 * 3 + 20 * 5 + 30 * 7 + 255 * 11) % 64;
    a.append(char(idx)); // index -> (10,20,30,255)
    a.append(QByteArray(7, '\0')).append(char(1));
    return a;
}

QByteArray qoiQuad(int w, int h) {
    QByteArray a("qoif");
    put32be(a, quint32(w));
    put32be(a, quint32(h));
    a.append(char(3)).append(char(0));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const QRgb c = quadColor(x, y, w, h);
            a.append(char(0xFE)).append(char(qRed(c))).append(char(qGreen(c))).append(char(qBlue(c)));
        }
    a.append(QByteArray(7, '\0')).append(char(1));
    return a;
}

QByteArray pcxHeader(int w, int h, int bpp, int planes, int bytesPerLine, bool rle = true) {
    QByteArray a(128, '\0');
    a[0] = char(0x0A);
    a[1] = char(5);
    a[2] = char(rle ? 1 : 0);
    a[3] = char(bpp);
    a[8] = char((w - 1) & 255);
    a[9] = char((w - 1) >> 8);
    a[10] = char((h - 1) & 255);
    a[11] = char((h - 1) >> 8);
    a[65] = char(planes);
    a[66] = char(bytesPerLine & 255);
    a[67] = char(bytesPerLine >> 8);
    return a;
}

void pcxRle(QByteArray& out, const QByteArray& line) {
    for (int i = 0; i < line.size();) {
        int n = 1;
        while (i + n < line.size() && n < 63 && line[i + n] == line[i])
            ++n;
        if (n > 1 || uchar(line[i]) >= 0xC0)
            out.append(char(0xC0 | n)).append(line[i]);
        else
            out.append(line[i]);
        i += n;
    }
}

QByteArray pcxQuad24(int w, int h) {
    QByteArray a = pcxHeader(w, h, 8, 3, w);
    for (int y = 0; y < h; ++y)
        for (int c = 0; c < 3; ++c) {
            QByteArray line;
            for (int x = 0; x < w; ++x) {
                const QRgb q = quadColor(x, y, w, h);
                line.append(char(c == 0 ? qRed(q) : c == 1 ? qGreen(q) : qBlue(q)));
            }
            pcxRle(a, line);
        }
    return a;
}

QByteArray pcxQuadPalette(int w, int h) {
    QByteArray a = pcxHeader(w, h, 8, 1, w);
    for (int y = 0; y < h; ++y) {
        QByteArray line;
        for (int x = 0; x < w; ++x) {
            const QRgb q = quadColor(x, y, w, h);
            line.append(char(q == qRgb(255, 0, 0)   ? 1
                             : q == qRgb(0, 255, 0) ? 2
                             : q == qRgb(0, 0, 255) ? 3
                                                    : 200));
        }
        pcxRle(a, line);
    }
    a.append(char(0x0C));
    QByteArray pal(768, '\0');
    pal[3] = char(255);   // 1 = red
    pal[7] = char(255);   // 2 = green
    pal[11] = char(255);  // 3 = blue
    pal[600] = char(255); // 200 = white
    pal[601] = char(255);
    pal[602] = char(255);
    a.append(pal);
    return a;
}

QByteArray ppm(int w, int h, int maxval, bool ascii) {
    QByteArray a = QStringLiteral("%1\n# comment\n%2 %3\n%4\n")
                       .arg(ascii ? QStringLiteral("P3") : QStringLiteral("P6"))
                       .arg(w)
                       .arg(h)
                       .arg(maxval)
                       .toLatin1();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const QRgb q = quadColor(x, y, w, h);
            for (int v : {qRed(q), qGreen(q), qBlue(q)}) {
                const int s = v * maxval / 255;
                if (ascii)
                    a.append(QByteArray::number(s)).append(' ');
                else if (maxval > 255)
                    put16be(a, s);
                else
                    a.append(char(s));
            }
        }
    return a;
}

QByteArray pfm(int w, int h, bool littleEndian, float whiteValue) {
    QByteArray a =
        QStringLiteral("PF\n%1 %2\n%3\n").arg(w).arg(h).arg(littleEndian ? "-1.0" : "1.0").toLatin1();
    for (int row = h - 1; row >= 0; --row)
        for (int x = 0; x < w; ++x) {
            const QRgb q = quadColor(x, row, w, h);
            const bool white = q == qRgb(255, 255, 255);
            for (int v : {qRed(q), qGreen(q), qBlue(q)}) {
                const float f = white ? whiteValue : v / 255.0f;
                quint32 bits;
                std::memcpy(&bits, &f, 4);
                littleEndian ? put32le(a, bits) : put32be(a, bits);
            }
        }
    return a;
}

QByteArray sgi(int w, int h, int z, int bpc, bool rle) {
    QByteArray a(512, '\0');
    a[0] = char(0x01);
    a[1] = char(0xDA);
    a[2] = char(rle ? 1 : 0);
    a[3] = char(bpc);
    QByteArray dims;
    put16be(dims, 3);
    put16be(dims, w);
    put16be(dims, h);
    put16be(dims, z);
    a.replace(4, 8, dims);
    auto sample = [&](int x, int yBottomUp, int c) {
        const QRgb q = quadColor(x, h - 1 - yBottomUp, w, h);
        const int v = c == 0 ? qRed(q) : c == 1 ? qGreen(q) : c == 2 ? qBlue(q) : 255;
        return bpc == 2 ? v * 257 : v;
    };
    if (!rle) {
        for (int c = 0; c < z; ++c)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    bpc == 2 ? put16be(a, sample(x, y, c)) : void(a.append(char(sample(x, y, c))));
        return a;
    }
    QList<QByteArray> rows;
    for (int c = 0; c < z; ++c)
        for (int y = 0; y < h; ++y) {
            QByteArray row;
            auto unit = [&](int v) { bpc == 2 ? put16be(row, v) : void(row.append(char(v))); };
            // repeat packets for runs, literal packets otherwise
            for (int x = 0; x < w;) {
                int n = 1;
                while (x + n < w && n < 127 && sample(x + n, y, c) == sample(x, y, c))
                    ++n;
                if (n > 1) {
                    unit(n);
                    unit(sample(x, y, c));
                } else {
                    unit(0x80 | 1);
                    unit(sample(x, y, c));
                }
                x += n;
            }
            unit(0);
            rows << row;
        }
    quint32 offset = quint32(512 + rows.size() * 8);
    QByteArray starts, lengths;
    for (const QByteArray& r : rows) {
        put32be(starts, offset);
        put32be(lengths, quint32(r.size()));
        offset += quint32(r.size());
    }
    a.append(starts).append(lengths);
    for (const QByteArray& r : rows)
        a.append(r);
    return a;
}

QByteArray pngQuad(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, quadColor(x, y, w, h));
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

QByteArray rawDeflate(const QByteArray& in) {
    z_stream zs{};
    deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
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

// ZIP writer: members in order, `deflated` = method 8 (except "mimetype").
QByteArray makeZip(const QList<QPair<QString, QByteArray>>& members, bool deflated) {
    QByteArray zip, central;
    int count = 0;
    for (const auto& [name, data] : members) {
        const bool def = deflated && name != u"mimetype";
        const QByteArray stored = def ? rawDeflate(data) : data;
        const quint32 crc =
            quint32(crc32(0, reinterpret_cast<const Bytef*>(data.constData()), uInt(data.size())));
        const QByteArray n = name.toUtf8();
        const quint32 offset = quint32(zip.size());
        put32le(zip, 0x04034b50);
        put16le(zip, 20);
        put16le(zip, 0x800);
        put16le(zip, def ? 8 : 0);
        put32le(zip, 0); // time/date
        put32le(zip, crc);
        put32le(zip, quint32(stored.size()));
        put32le(zip, quint32(data.size()));
        put16le(zip, int(n.size()));
        put16le(zip, 0);
        zip.append(n).append(stored);
        put32le(central, 0x02014b50);
        put16le(central, 20);
        put16le(central, 20);
        put16le(central, 0x800);
        put16le(central, def ? 8 : 0);
        put32le(central, 0);
        put32le(central, crc);
        put32le(central, quint32(stored.size()));
        put32le(central, quint32(data.size()));
        put16le(central, int(n.size()));
        put16le(central, 0);
        put16le(central, 0);
        put16le(central, 0);
        put16le(central, 0);
        put32le(central, 0);
        put32le(central, offset);
        central.append(n);
        ++count;
    }
    const quint32 cdOffset = quint32(zip.size());
    zip.append(central);
    put32le(zip, 0x06054b50);
    put16le(zip, 0);
    put16le(zip, 0);
    put16le(zip, count);
    put16le(zip, count);
    put32le(zip, quint32(central.size()));
    put32le(zip, cdOffset);
    put16le(zip, 0);
    return zip;
}

QByteArray oraArchive(bool withMerged, bool deflated) {
    QList<QPair<QString, QByteArray>> m{
        {QStringLiteral("mimetype"), QByteArray("image/openraster")},
        {QStringLiteral("stack.xml"), QByteArray("<image w=\"8\" h=\"6\"/>")}};
    if (withMerged)
        m.append({QStringLiteral("mergedimage.png"), pngQuad(8, 6)});
    m.append({QStringLiteral("Thumbnails/thumbnail.png"), pngQuad(4, 3)});
    return makeZip(m, deflated);
}

std::optional<simplefmt::Raster> decodeAny(Format f, const QByteArray& data, QString* error) {
    switch (f) {
    case Format::Qoi:
        return simplefmt::decodeQoi(data, error);
    case Format::Pcx:
        return simplefmt::decodePcx(data, error);
    case Format::Pnm:
        return simplefmt::decodePnm(data, error);
    case Format::Sgi:
        return simplefmt::decodeSgi(data, error);
    case Format::Ora:
    case Format::Kra:
        return simplefmt::decodeLayeredData(f, data, 0, error);
    }
    return std::nullopt;
}

// Fuzz-style: every truncation and many random byte changes decode or fail - never crash,
// never return an image of the wrong size.
void fuzz(Format f, const QByteArray& valid, int flips = 400) {
    QString err;
    const auto ref = decodeAny(f, valid, &err);
    ASSERT_TRUE(ref.has_value()) << err.toStdString();
    const int step = std::max(1, int(valid.size() / 300));
    for (int len = 0; len < valid.size(); len += step) {
        QString e;
        const auto r = decodeAny(f, valid.left(len), &e);
        if (!r)
            EXPECT_FALSE(e.isEmpty()) << "truncated at " << len << " without error";
    }
    QRandomGenerator rng(1234);
    for (int i = 0; i < flips; ++i) {
        QByteArray d = valid;
        const int changes = 1 + int(rng.bounded(4));
        for (int k = 0; k < changes; ++k)
            d[int(rng.bounded(quint32(d.size())))] = char(rng.bounded(256));
        QString e;
        const auto r = decodeAny(f, d, &e);
        if (r) {
            EXPECT_FALSE(r->image.isNull());
            EXPECT_LE(qint64(r->image.width()) * r->image.height(), simplefmt::kMaxPixels);
        } else {
            EXPECT_FALSE(e.isEmpty());
        }
    }
}

QString writeTemp(QTemporaryDir& dir, const QString& name, const QByteArray& data) {
    const QString p = dir.filePath(name);
    QFile f(p);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
    return p;
}

} // namespace

// ---- QOI --------------------------------------------------------------------------------

TEST(SimpleDecoders, F_FMT_01_QoiAllOps) {
    QString err;
    const auto r = simplefmt::decodeQoi(qoiAllOps(), &err);
    ASSERT_TRUE(r.has_value()) << err.toStdString();
    ASSERT_EQ(r->image.size(), QSize(4, 2));
    EXPECT_TRUE(r->hasAlpha);
    const QImage& img = r->image;
    EXPECT_EQ(img.pixelColor(0, 0), QColor(10, 20, 30, 255));
    EXPECT_EQ(img.pixelColor(1, 0), QColor(11, 19, 30, 255));
    EXPECT_EQ(img.pixelColor(2, 0), QColor(11 + 6, 19 + 5, 30 + 4, 255));
    EXPECT_EQ(img.pixelColor(3, 0), QColor(1, 2, 3, 4));
    for (int x = 0; x < 3; ++x)
        EXPECT_EQ(img.pixelColor(x, 1), QColor(1, 2, 3, 4)) << x; // run of 3
    EXPECT_EQ(img.pixelColor(3, 1), QColor(10, 20, 30, 255));     // index
}

TEST(SimpleDecoders, F_FMT_01_QoiQuadrantsAndSignature) {
    const QByteArray q = qoiQuad(16, 12);
    QString err;
    const auto r = simplefmt::decodeQoi(q, &err);
    ASSERT_TRUE(r.has_value()) << err.toStdString();
    EXPECT_FALSE(r->hasAlpha);
    expectQuad(r->image);
    EXPECT_TRUE(simplefmt::matchesSignature(Format::Qoi, q.left(64), QStringLiteral("bin")));
    EXPECT_FALSE(simplefmt::matchesSignature(Format::Qoi, QByteArray("\x89PNG"), QStringLiteral("qoi")));
}

TEST(SimpleDecoders, F_FMT_01_QoiMalformed) {
    fuzz(Format::Qoi, qoiQuad(16, 12));
    // Huge declared size with no data: rejected before allocating.
    QByteArray huge("qoif");
    put32be(huge, 100000);
    put32be(huge, 100000);
    huge.append(char(4)).append(char(0));
    QString err;
    EXPECT_FALSE(simplefmt::decodeQoi(huge, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(simplefmt::probe(Format::Qoi, huge).has_value());
}

// ---- PCX --------------------------------------------------------------------------------

TEST(SimpleDecoders, F_FMT_01_PcxTrueColorAndPalette) {
    QString err;
    const auto rgb = simplefmt::decodePcx(pcxQuad24(16, 12), &err);
    ASSERT_TRUE(rgb.has_value()) << err.toStdString();
    expectQuad(rgb->image);
    const auto pal = simplefmt::decodePcx(pcxQuadPalette(16, 12), &err);
    ASSERT_TRUE(pal.has_value()) << err.toStdString();
    expectQuad(pal->image);
}

TEST(SimpleDecoders, F_FMT_01_PcxMonochromeAndSixteenColors) {
    // 1 bit: left half black, right half white.
    QByteArray mono = pcxHeader(16, 2, 1, 1, 2);
    for (int y = 0; y < 2; ++y)
        pcxRle(mono, QByteArray("\x00\xFF", 2));
    QString err;
    const auto m = simplefmt::decodePcx(mono, &err);
    ASSERT_TRUE(m.has_value()) << err.toStdString();
    EXPECT_EQ(m->image.pixelColor(0, 0), QColor(Qt::black));
    EXPECT_EQ(m->image.pixelColor(15, 1), QColor(Qt::white));
    // 4 bit, 1 plane: header palette entries 1 and 2.
    QByteArray four = pcxHeader(4, 1, 4, 1, 2, false);
    four[16 + 3] = char(255); // palette 1 = red
    four[16 + 7] = char(255); // palette 2 = green
    four.append(char(0x12)).append(char(0x21));
    const auto f = simplefmt::decodePcx(four, &err);
    ASSERT_TRUE(f.has_value()) << err.toStdString();
    EXPECT_EQ(f->image.pixelColor(0, 0), QColor(255, 0, 0));
    EXPECT_EQ(f->image.pixelColor(1, 0), QColor(0, 255, 0));
    EXPECT_EQ(f->image.pixelColor(2, 0), QColor(0, 255, 0));
    EXPECT_EQ(f->image.pixelColor(3, 0), QColor(255, 0, 0));
    // 1 bit, 4 planes (EGA): index bits from the planes.
    QByteArray ega = pcxHeader(8, 1, 1, 4, 1, false);
    ega[16 + 15 * 3] = char(255);                  // palette 15 = (255,0,0)
    ega.append(QByteArray("\x80\x80\x80\x80", 4)); // pixel 0: all planes set -> 15
    const auto e = simplefmt::decodePcx(ega, &err);
    ASSERT_TRUE(e.has_value()) << err.toStdString();
    EXPECT_EQ(e->image.pixelColor(0, 0), QColor(255, 0, 0));
    EXPECT_EQ(e->image.pixelColor(1, 0), QColor(0, 0, 0));
}

TEST(SimpleDecoders, F_FMT_01_PcxMalformed) {
    fuzz(Format::Pcx, pcxQuad24(16, 12));
    fuzz(Format::Pcx, pcxQuadPalette(16, 12), 200);
    QByteArray bad = pcxHeader(16, 12, 8, 3, 4); // bytes per line too small
    QString err;
    EXPECT_FALSE(simplefmt::decodePcx(bad, &err).has_value());
    EXPECT_FALSE(simplefmt::decodePcx(QByteArray(10, '\x0A'), &err).has_value());
}

// ---- PNM / PAM / PFM ---------------------------------------------------------------------

TEST(SimpleDecoders, F_FMT_01_PnmVariants) {
    QString err;
    for (bool ascii : {false, true}) {
        SCOPED_TRACE(ascii ? "P3" : "P6");
        const auto r = simplefmt::decodePnm(ppm(16, 12, 255, ascii), &err);
        ASSERT_TRUE(r.has_value()) << err.toStdString();
        EXPECT_EQ(r->bitsPerComponent, 8);
        expectQuad(r->image);
    }
    const auto deep = simplefmt::decodePnm(ppm(16, 12, 65535, false), &err);
    ASSERT_TRUE(deep.has_value()) << err.toStdString();
    EXPECT_EQ(deep->bitsPerComponent, 16);
    EXPECT_EQ(deep->image.format(), QImage::Format_RGBA64);
    expectQuad(deep->image);
    // maxval 1023 (10 bit) scales to 16 bit.
    const auto ten = simplefmt::decodePnm(ppm(4, 2, 1023, false), &err);
    ASSERT_TRUE(ten.has_value()) << err.toStdString();
    EXPECT_EQ(ten->image.pixelColor(0, 0).rgba64().red(), 65535);

    // P1 (digits without separators), P4 (packed), P2 and P5 gray.
    const auto p1 = simplefmt::decodePnm(QByteArray("P1\n3 2\n010\n1 0 1\n"), &err);
    ASSERT_TRUE(p1.has_value()) << err.toStdString();
    EXPECT_EQ(p1->image.pixelColor(0, 0), QColor(Qt::white));
    EXPECT_EQ(p1->image.pixelColor(1, 0), QColor(Qt::black));
    EXPECT_EQ(p1->image.pixelColor(0, 1), QColor(Qt::black));
    const auto p4 = simplefmt::decodePnm(QByteArray("P4 9 1\n\x80\x80", 9), &err);
    ASSERT_TRUE(p4.has_value()) << err.toStdString();
    EXPECT_EQ(p4->image.pixelColor(0, 0), QColor(Qt::black));
    EXPECT_EQ(p4->image.pixelColor(1, 0), QColor(Qt::white));
    EXPECT_EQ(p4->image.pixelColor(8, 0), QColor(Qt::black));
    const auto p2 = simplefmt::decodePnm(QByteArray("P2 2 1 10\n0 10\n"), &err);
    ASSERT_TRUE(p2.has_value()) << err.toStdString();
    EXPECT_EQ(p2->colorModel, QStringLiteral("Gray"));
    EXPECT_EQ(p2->image.pixelColor(1, 0), QColor(Qt::white));
    const auto p5 = simplefmt::decodePnm(QByteArray("P5 2 1 255\n\x00\x80", 13), &err);
    ASSERT_TRUE(p5.has_value()) << err.toStdString();
    EXPECT_EQ(p5->image.pixelColor(1, 0), QColor(128, 128, 128));
    // PAM with alpha.
    const QByteArray pam =
        QByteArray("P7\nWIDTH 2\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n") +
        QByteArray("\xFF\x00\x00\xFF\x00\x00\xFF\x00", 8);
    const auto p7 = simplefmt::decodePnm(pam, &err);
    ASSERT_TRUE(p7.has_value()) << err.toStdString();
    EXPECT_TRUE(p7->hasAlpha);
    EXPECT_EQ(p7->image.pixelColor(0, 0), QColor(255, 0, 0, 255));
    EXPECT_EQ(p7->image.pixelColor(1, 0).alpha(), 0);
}

TEST(SimpleDecoders, F_FMT_01_PfmFloatBottomUpBothEndians) {
    for (bool little : {true, false}) {
        SCOPED_TRACE(little ? "little endian" : "big endian");
        QString err;
        const auto r = simplefmt::decodePnm(pfm(8, 6, little, 4.0f), &err);
        ASSERT_TRUE(r.has_value()) << err.toStdString();
        EXPECT_TRUE(r->isFloat);
        EXPECT_EQ(r->image.format(), QImage::Format_RGBA32FPx4);
        const auto* tl = reinterpret_cast<const float*>(r->image.constScanLine(0));
        EXPECT_FLOAT_EQ(tl[0], 1.0f); // red at the top: rows are stored bottom-up
        EXPECT_FLOAT_EQ(tl[1], 0.0f);
        const auto* br = reinterpret_cast<const float*>(r->image.constScanLine(5)) + 7 * 4;
        EXPECT_FLOAT_EQ(br[0], 4.0f);
    }
    // The backend: HDR on request (extended range), clipped otherwise.
    QTemporaryDir dir;
    const QString path = writeTemp(dir, QStringLiteral("hdr.pfm"), pfm(8, 6, true, 4.0f));
    SimpleFormatDecoder dec(Format::Pnm);
    QString err;
    const auto info = dec.info(path, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_TRUE(info->isHdr);
    EXPECT_EQ(info->formatName, QStringLiteral("PFM"));
    DecodeRequest hdr;
    hdr.hdr = true;
    const auto h = dec.decode(path, hdr, &err);
    ASSERT_TRUE(h.has_value()) << err.toStdString();
    EXPECT_EQ(h->image.format(), QImage::Format_RGBA16FPx4_Premultiplied);
    EXPECT_GT(h->headroom, 3.9f);
    const auto sdr = dec.decode(path, DecodeRequest{}, &err);
    ASSERT_TRUE(sdr.has_value()) << err.toStdString();
    EXPECT_EQ(sdr->image.format(), QImage::Format_RGBA64_Premultiplied);
    EXPECT_FLOAT_EQ(sdr->headroom, 1.0f);
}

TEST(SimpleDecoders, F_FMT_01_PnmMalformed) {
    fuzz(Format::Pnm, ppm(16, 12, 255, false));
    fuzz(Format::Pnm, ppm(8, 6, 255, true), 200);
    fuzz(Format::Pnm, pfm(8, 6, true, 2.0f), 200);
    QString err;
    for (const char* bad : {"P6 0 5 255\n", "P6 99999999 2 255\n", "P5 2 2 70000\n",
                            "P7\nWIDTH 2\nHEIGHT 2\n", "PF\n2 2\nnan\n", "P9 1 1 1\n", "P6 2 2 255\n\x01"}) {
        SCOPED_TRACE(bad);
        EXPECT_FALSE(simplefmt::decodePnm(QByteArray(bad), &err).has_value());
        EXPECT_FALSE(err.isEmpty());
    }
}

// ---- SGI --------------------------------------------------------------------------------

TEST(SimpleDecoders, F_FMT_01_SgiVerbatimRleDeep) {
    QString err;
    for (bool rle : {false, true})
        for (int bpc : {1, 2})
            for (int z : {3, 4}) {
                SCOPED_TRACE(QStringLiteral("rle %1 bpc %2 z %3").arg(rle).arg(bpc).arg(z).toStdString());
                const auto r = simplefmt::decodeSgi(sgi(16, 12, z, bpc, rle), &err);
                ASSERT_TRUE(r.has_value()) << err.toStdString();
                EXPECT_EQ(r->bitsPerComponent, bpc * 8);
                EXPECT_EQ(r->hasAlpha, z == 4);
                expectQuad(r->image);
            }
    // Gray (z = 1).
    const auto g = simplefmt::decodeSgi(sgi(4, 2, 1, 1, false), &err);
    ASSERT_TRUE(g.has_value()) << err.toStdString();
    EXPECT_EQ(g->colorModel, QStringLiteral("Gray"));
    EXPECT_EQ(g->image.pixelColor(0, 0), QColor(255, 255, 255)); // red channel used as gray
}

TEST(SimpleDecoders, F_FMT_01_SgiMalformed) {
    fuzz(Format::Sgi, sgi(16, 12, 4, 1, true));
    fuzz(Format::Sgi, sgi(8, 6, 3, 2, false), 200);
    QByteArray bad = sgi(16, 12, 3, 1, true);
    bad[512] = char(0x7F); // first row offset far outside the file
    QString err;
    EXPECT_FALSE(simplefmt::decodeSgi(bad, &err).has_value());
}

// Code review M3 #3/#4: tiny crafted files must fail fast and without allocating the
// claimed image (CPU/memory DoS inside the decoder helper). Before: PCX looped over a
// 65535-byte line for every row, SGI/PNM/PFM allocated (and SGI filled) the image first.
TEST(SimpleDecoders, F_FMT_02_TinyFilesClaimingHugeImagesFailFast) {
    struct Case {
        const char* name;
        QByteArray data;
        std::optional<simplefmt::Raster> (*decode)(const QByteArray&, QString*);
    };
    QList<Case> cases;
    // PCX: 1 × 4096 pixels, 4 planes of 65535 bytes per line, two bytes of data.
    QByteArray pcxLine = pcxHeader(1, 4096, 8, 4, 65535);
    pcxLine.append("\x01\x01", 2);
    cases.append({"pcx bytesPerLine", pcxLine, &simplefmt::decodePcx});
    // PCX: 16384² with a plausible line length but no data to speak of.
    QByteArray pcxHuge = pcxHeader(16384, 16384, 8, 1, 16384);
    pcxHuge.append("\xC1\x00", 2);
    cases.append({"pcx huge", pcxHuge, &simplefmt::decodePcx});
    // SGI: 8192² × 16 bit, header only.
    for (bool rle : {false, true}) {
        QByteArray sgiHuge = sgi(1, 1, 3, 2, false).left(512);
        sgiHuge[2] = char(rle ? 1 : 0);
        sgiHuge[6] = char(0x20); // width 8192
        sgiHuge[7] = 0;
        sgiHuge[8] = char(0x20); // height 8192
        sgiHuge[9] = 0;
        cases.append({rle ? "sgi rle" : "sgi verbatim", sgiHuge, &simplefmt::decodeSgi});
    }
    cases.append({"ppm", QByteArray("P6 16384 16384 65535\n\x01\x02", 24), &simplefmt::decodePnm});
    cases.append({"pgm ascii", QByteArray("P2 16384 16384 65535\n1 2 3"), &simplefmt::decodePnm});
    // 2 MB of samples: before, a 1 GB image was allocated and filled until the data ran out.
    cases.append({"ppm ascii, some data",
                  QByteArray("P3 16384 16384 255\n") + QByteArray("1 ").repeated(1 << 20),
                  &simplefmt::decodePnm});
    cases.append({"pbm ascii", QByteArray("P1 16384 16384\n0101"), &simplefmt::decodePnm});
    cases.append({"pam", QByteArray("P7\nWIDTH 16384\nHEIGHT 16384\nDEPTH 4\nMAXVAL 255\nENDHDR\nxx"),
                  &simplefmt::decodePnm});
    cases.append({"pfm", QByteArray("PF\n16384 16384\n-1.0\n\x00\x00\x80\x3F", 24), &simplefmt::decodePnm});
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
#ifdef Q_OS_DARWIN
        // Peak of the heap while decoding (malloc keeps no high-water mark).
        auto inUse = [] {
            malloc_statistics_t st{};
            malloc_zone_statistics(nullptr, &st);
            return st.size_in_use;
        };
        const size_t baseline = inUse();
        std::atomic<bool> stop{false};
        std::atomic<size_t> peak{baseline};
        std::thread sampler([&] {
            while (!stop) {
                peak = std::max(peak.load(), inUse());
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
#endif
        QElapsedTimer t;
        t.start();
        QString err;
        EXPECT_FALSE(c.decode(c.data, &err).has_value());
        EXPECT_LT(t.elapsed(), 50);
        EXPECT_FALSE(err.isEmpty());
#ifdef Q_OS_DARWIN
        stop = true;
        sampler.join();
        EXPECT_LT(peak.load(), baseline + (64u << 20)) << "allocated the claimed image";
#endif
    }
}

// ---- ZIP, OpenRaster, Krita ---------------------------------------------------------------

TEST(SimpleDecoders, F_FMT_01_ZipStoredAndDeflated) {
    for (bool deflated : {false, true}) {
        SCOPED_TRACE(deflated ? "deflated" : "stored");
        const QByteArray payload = QByteArray("hello zip ").repeated(500);
        ZipReader zip(makeZip(
            {{QStringLiteral("mimetype"), QByteArray("x")}, {QStringLiteral("a/b.txt"), payload}}, deflated));
        ASSERT_TRUE(zip.isValid()) << zip.errorString().toStdString();
        EXPECT_EQ(zip.names(), QStringList({QStringLiteral("mimetype"), QStringLiteral("a/b.txt")}));
        EXPECT_EQ(zip.read(QStringLiteral("a/b.txt")).value_or(QByteArray()), payload);
        EXPECT_EQ(zip.read(QStringLiteral("a/b.txt"), 10).value_or(QByteArray()), payload.left(10));
        EXPECT_FALSE(zip.read(QStringLiteral("missing")).has_value());
    }
    EXPECT_FALSE(ZipReader(QByteArray("PK\x05\x06", 4)).isValid());
    EXPECT_FALSE(ZipReader(QByteArray(100, 'x')).isValid());
}

TEST(SimpleDecoders, F_FMT_01_OpenRasterAndKrita) {
    QString err;
    for (bool deflated : {false, true}) {
        const auto r = simplefmt::decodeLayeredData(Format::Ora, oraArchive(true, deflated), 0, &err);
        ASSERT_TRUE(r.has_value()) << err.toStdString();
        EXPECT_EQ(r->image.size(), QSize(8, 6));
        expectQuad(r->image);
    }
    // Only the thumbnail: shown as a preview (smaller) - better than an error card.
    QTemporaryDir dir;
    const QString thumbOnly = writeTemp(dir, QStringLiteral("t.ora"), oraArchive(false, true));
    SimpleFormatDecoder ora(Format::Ora);
    EXPECT_TRUE(ora.canDecode(thumbOnly, oraArchive(false, true).left(64)));
    const auto t = ora.decode(thumbOnly, DecodeRequest{}, &err);
    ASSERT_TRUE(t.has_value()) << err.toStdString();
    EXPECT_TRUE(t->isPreview);
    // Krita: mimetype decides, merged image decoded; scaled on request.
    const QByteArray kra = makeZip({{QStringLiteral("mimetype"), QByteArray("application/x-krita")},
                                    {QStringLiteral("maindoc.xml"), QByteArray("<DOC/>")},
                                    {QStringLiteral("mergedimage.png"), pngQuad(40, 30)}},
                                   true);
    const QString kraPath = writeTemp(dir, QStringLiteral("k.kra"), kra);
    SimpleFormatDecoder krita(Format::Kra);
    EXPECT_TRUE(krita.canDecode(QStringLiteral("/x/no-suffix"), kra.left(64)));
    EXPECT_FALSE(ora.canDecode(QStringLiteral("/x/no-suffix"), kra.left(64)));
    const auto info = krita.info(kraPath, &err);
    ASSERT_TRUE(info.has_value()) << err.toStdString();
    EXPECT_EQ(info->displaySize, QSize(40, 30));
    DecodeRequest small;
    small.maxPixelSize = 20;
    const auto k = krita.decode(kraPath, small, &err);
    ASSERT_TRUE(k.has_value()) << err.toStdString();
    EXPECT_EQ(k->image.size(), QSize(20, 15));
    EXPECT_EQ(k->fullSize, QSize(40, 30));
    EXPECT_TRUE(k->isPreview);
}

TEST(SimpleDecoders, F_FMT_01_ArchiveMalformed) {
    fuzz(Format::Ora, oraArchive(true, true), 300);
    fuzz(Format::Ora, oraArchive(true, false), 150);
    QString err;
    const QByteArray noImage = makeZip({{QStringLiteral("mimetype"), QByteArray("image/openraster")}}, false);
    EXPECT_FALSE(simplefmt::decodeLayeredData(Format::Ora, noImage, 0, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
}

// ---- registration and the corpus -----------------------------------------------------------

TEST(SimpleDecoders, F_FMT_01_SuffixesRegisteredForCollections) {
    for (const char* s :
         {"qoi", "pcx", "pbm", "pgm", "ppm", "pnm", "pam", "pfm", "sgi", "rgb", "ora", "kra"}) {
        SCOPED_TRACE(s);
        EXPECT_TRUE(DecoderRegistry::instance().isSupportedExtension(QString::fromLatin1(s)));
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s)));
    }
    EXPECT_TRUE(hasImageSuffix(QStringLiteral("/a/b/Picture.QOI")));
}

TEST(SimpleDecoders, F_FMT_01_RestCorpus) {
    LF_REQUIRE_CORPUS();
    const QJsonObject rest = expectedCorpus().value(QStringLiteral("rest")).toObject();
    ASSERT_FALSE(rest.isEmpty());
    const int tol = expectedCorpus().value(QStringLiteral("tolerance")).toInt(40);
    int checked = 0;
    for (auto it = rest.begin(); it != rest.end(); ++it) {
        const QString name = it.key();
        const QJsonObject e = it.value().toObject();
        SCOPED_TRACE(name.toStdString());
        const QString path = corpusFile(name);
        if (!QFileInfo::exists(path)) {
            EXPECT_TRUE(e.value(QStringLiteral("optional")).toBool()) << "required corpus file missing";
            continue;
        }
        ++checked;
        // Our backend for the file (the registry may prefer ImageIO for some, e.g. PPM).
        std::shared_ptr<Decoder> own;
        for (const auto& d : DecoderRegistry::instance().candidates(path))
            if (dynamic_cast<SimpleFormatDecoder*>(d.get()))
                own = d;
        ASSERT_TRUE(own) << "no own decoder claims the file";
        QString err;
        const auto info = own->info(path, &err);
        ASSERT_TRUE(info.has_value()) << err.toStdString();
        EXPECT_EQ(info->displaySize, jsonSize(e.value(QStringLiteral("displaySize"))));
        EXPECT_EQ(info->formatName, e.value(QStringLiteral("format")).toString());
        EXPECT_EQ(info->isHdr, e.value(QStringLiteral("isHdr")).toBool());
        if (e.contains(QStringLiteral("bits")))
            EXPECT_EQ(info->bitsPerComponent, e.value(QStringLiteral("bits")).toInt());
        const auto img = own->decode(path, DecodeRequest{}, &err);
        ASSERT_TRUE(img.has_value()) << err.toStdString();
        EXPECT_EQ(img->image.size(), info->displaySize);
        expectQuadrants(img->image, quadrantColors(e.value(QStringLiteral("quadrants"))), tol);
        // The registry decodes it too (whichever backend wins).
        EXPECT_TRUE(DecoderRegistry::instance().decode(path, DecodeRequest{}, &err).has_value())
            << err.toStdString();
    }
    EXPECT_GE(checked, 6);
}
