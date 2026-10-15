// SPDX-License-Identifier: GPL-3.0-or-later
// Pure, bounds-checked decoders for QOI, PCX, PNM/PAM/PFM and SGI (see SimpleDecoders.h).
#include "SimpleDecoders.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lf::simplefmt {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::SimpleDecoders)
};

QString damaged() {
    return Tr::tr("Die Datei ist beschädigt oder unvollständig.");
}

template <typename T> std::optional<T> failWith(QString* error, const QString& msg) {
    if (error)
        *error = msg;
    return std::nullopt;
}

// PCX rows may be padded by a few bytes beyond the pixels (even byte counts, alignment).
constexpr qint64 kPcxLineSlack = 16;
// Upper bound of the compression ratio accepted for the run-length formats (PCX RLE: 63
// bytes from 2, SGI RLE: 127 values from 2 units); beyond it the input is damaged.
constexpr qint64 kMaxCompression = 64;

bool validSize(qint64 w, qint64 h) {
    return w > 0 && h > 0 && w <= kMaxEdge && h <= kMaxEdge && w * h <= kMaxPixels;
}

// Allocates the output; null (with an error) if the size is invalid or memory is short.
QImage allocate(qint64 w, qint64 h, QImage::Format f, QString* error) {
    if (!validSize(w, h)) {
        if (error)
            *error = Tr::tr("Die Bildgröße %1 × %2 ist ungültig oder zu groß.").arg(w).arg(h);
        return {};
    }
    QImage img(int(w), int(h), f);
    if (img.isNull() && error)
        *error = Tr::tr("Nicht genug Arbeitsspeicher für das Bild (%1 × %2 Pixel).").arg(w).arg(h);
    return img;
}

quint16 be16(const uchar* p) {
    return quint16((p[0] << 8) | p[1]);
}
quint32 be32(const uchar* p) {
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}
quint16 le16(const uchar* p) {
    return quint16(p[0] | (p[1] << 8));
}

const uchar* bytes(const QByteArray& a) {
    return reinterpret_cast<const uchar*>(a.constData());
}

// ---- QOI ----------------------------------------------------------------------------

std::optional<Probe> probeQoi(const QByteArray& data) {
    if (data.size() < 14 || !data.startsWith("qoif"))
        return std::nullopt;
    const uchar* d = bytes(data);
    const quint32 w = be32(d + 4), h = be32(d + 8);
    const int channels = d[12];
    if ((channels != 3 && channels != 4) || d[13] > 1 || !validSize(w, h))
        return std::nullopt;
    Probe p;
    p.size = QSize(int(w), int(h));
    p.hasAlpha = channels == 4;
    return p;
}

// ---- PCX ----------------------------------------------------------------------------

struct PcxHeader {
    int bpp = 0, planes = 0, bytesPerLine = 0, w = 0, h = 0, version = 0;
    bool rle = true;
};

std::optional<PcxHeader> pcxHeader(const QByteArray& data) {
    if (data.size() < 128)
        return std::nullopt;
    const uchar* d = bytes(data);
    if (d[0] != 0x0A || d[2] > 1)
        return std::nullopt;
    PcxHeader h;
    h.version = d[1];
    h.rle = d[2] == 1;
    h.bpp = d[3];
    const int xmin = le16(d + 4), ymin = le16(d + 6), xmax = le16(d + 8), ymax = le16(d + 10);
    h.planes = d[65];
    h.bytesPerLine = le16(d + 66);
    if (xmax < xmin || ymax < ymin)
        return std::nullopt;
    h.w = xmax - xmin + 1;
    h.h = ymax - ymin + 1;
    const bool combo = (h.bpp == 1 && h.planes >= 1 && h.planes <= 4) || (h.bpp == 2 && h.planes == 1) ||
                       (h.bpp == 4 && h.planes == 1) ||
                       (h.bpp == 8 && (h.planes == 1 || h.planes == 3 || h.planes == 4));
    if (!combo || h.version > 5 || !validSize(h.w, h.h))
        return std::nullopt;
    // bytesPerLine covers a row (rounded up to an even count by the format) plus at most a
    // little padding: a huge value would make the decoder loop over bytes no pixel uses.
    const qint64 needed = (qint64(h.w) * h.bpp + 7) / 8;
    if (h.bytesPerLine <= 0 || h.bytesPerLine < needed || h.bytesPerLine > needed + kPcxLineSlack)
        return std::nullopt;
    return h;
}

// Smallest input that can plausibly describe `outputBytes` bytes: RLE packs at most 63 bytes
// into 2. A tiny file claiming a huge image is refused before anything is allocated (and a
// moderately truncated file still shows what is there).
bool plausibleInput(qint64 inputBytes, qint64 outputBytes) {
    return inputBytes > 0 && inputBytes >= outputBytes / kMaxCompression;
}

// ---- PNM ----------------------------------------------------------------------------

// Tokenizer for the text header (whitespace, '#' comments).
struct PnmCursor {
    const uchar* d = nullptr;
    qint64 n = 0;
    qint64 pos = 0;

    void skipSpace() {
        while (pos < n) {
            if (d[pos] == '#') {
                while (pos < n && d[pos] != '\n' && d[pos] != '\r')
                    ++pos;
            } else if (d[pos] == ' ' || d[pos] == '\t' || d[pos] == '\n' || d[pos] == '\r' ||
                       d[pos] == '\v' || d[pos] == '\f') {
                ++pos;
            } else {
                break;
            }
        }
    }
    // Non-negative decimal integer (header fields, ASCII samples).
    std::optional<qint64> number() {
        skipSpace();
        if (pos >= n || d[pos] < '0' || d[pos] > '9')
            return std::nullopt;
        qint64 v = 0;
        while (pos < n && d[pos] >= '0' && d[pos] <= '9') {
            v = v * 10 + (d[pos] - '0');
            if (v > 0x7fffffff)
                return std::nullopt;
            ++pos;
        }
        return v;
    }
    QByteArray word() {
        skipSpace();
        const qint64 start = pos;
        while (pos < n && d[pos] > ' ')
            ++pos;
        return QByteArray(reinterpret_cast<const char*>(d + start), int(pos - start));
    }
};

struct PnmHeader {
    char kind = 0; // '1'..'7', 'F' (PF colour), 'f' (Pf gray)
    qint64 w = 0, h = 0, maxval = 1, depth = 1;
    double scale = 1.0; // PFM: < 0 little endian
    qint64 dataOffset = 0;
};

std::optional<PnmHeader> pnmHeader(const QByteArray& data) {
    if (data.size() < 3 || data[0] != 'P')
        return std::nullopt;
    PnmHeader h;
    h.kind = data[1];
    PnmCursor c{bytes(data), data.size(), 2};
    if (h.kind >= '1' && h.kind <= '6') {
        const auto w = c.number(), hh = c.number();
        if (!w || !hh)
            return std::nullopt;
        h.w = *w;
        h.h = *hh;
        if (h.kind != '1' && h.kind != '4') {
            const auto m = c.number();
            if (!m || *m < 1 || *m > 65535)
                return std::nullopt;
            h.maxval = *m;
        }
        h.depth = (h.kind == '3' || h.kind == '6') ? 3 : 1;
        if (c.pos >= c.n && (h.kind == '4' || h.kind == '5' || h.kind == '6'))
            return std::nullopt;
        h.dataOffset = c.pos + 1; // exactly one whitespace character before binary data
    } else if (h.kind == '7') {
        bool end = false;
        for (int fields = 0; fields < 64 && !end; ++fields) {
            const QByteArray key = c.word();
            if (key.isEmpty())
                return std::nullopt;
            if (key == "ENDHDR") {
                end = true;
            } else if (key == "TUPLTYPE") {
                while (c.pos < c.n && c.d[c.pos] != '\n')
                    ++c.pos; // value until the end of the line
            } else {
                const auto v = c.number();
                if (!v)
                    return std::nullopt;
                if (key == "WIDTH")
                    h.w = *v;
                else if (key == "HEIGHT")
                    h.h = *v;
                else if (key == "DEPTH")
                    h.depth = *v;
                else if (key == "MAXVAL")
                    h.maxval = *v;
            }
        }
        if (!end || h.depth < 1 || h.depth > 4 || h.maxval < 1 || h.maxval > 65535)
            return std::nullopt;
        while (c.pos < c.n && c.d[c.pos] != '\n')
            ++c.pos;
        h.dataOffset = c.pos + 1;
    } else if (h.kind == 'F' || h.kind == 'f') {
        const auto w = c.number(), hh = c.number();
        if (!w || !hh)
            return std::nullopt;
        h.w = *w;
        h.h = *hh;
        const QByteArray s = c.word();
        bool ok = false;
        h.scale = s.toDouble(&ok);
        if (!ok || h.scale == 0.0 || !std::isfinite(h.scale))
            return std::nullopt;
        h.depth = h.kind == 'F' ? 3 : 1;
        h.dataOffset = c.pos + 1;
    } else {
        return std::nullopt;
    }
    if (!validSize(h.w, h.h))
        return std::nullopt;
    return h;
}

// ---- SGI ----------------------------------------------------------------------------

struct SgiHeader {
    bool rle = false;
    int bpc = 1;
    int w = 0, h = 0, z = 1;
};

std::optional<SgiHeader> sgiHeader(const QByteArray& data) {
    if (data.size() < 12)
        return std::nullopt;
    const uchar* d = bytes(data);
    if (be16(d) != 474 || d[2] > 1 || (d[3] != 1 && d[3] != 2))
        return std::nullopt;
    SgiHeader h;
    h.rle = d[2] == 1;
    h.bpc = d[3];
    const int dim = be16(d + 4);
    if (dim < 1 || dim > 3)
        return std::nullopt;
    h.w = be16(d + 6);
    h.h = dim >= 2 ? be16(d + 8) : 1;
    h.z = dim == 3 ? be16(d + 10) : 1;
    if (h.z < 1 || !validSize(h.w, h.h))
        return std::nullopt;
    return h;
}

} // namespace

// ---------------------------------------------------------------------------------------

bool matchesSignature(Format f, const QByteArray& header, const QString& lowerSuffix) {
    switch (f) {
    case Format::Qoi:
        return header.startsWith("qoif");
    case Format::Pcx:
        // PCX has a weak signature: the header must be valid and the suffix must fit.
        return lowerSuffix == u"pcx" && header.size() >= 4 && uchar(header[0]) == 0x0A;
    case Format::Pnm:
        return header.size() >= 2 && header[0] == 'P' &&
               ((header[1] >= '1' && header[1] <= '7') || header[1] == 'F' || header[1] == 'f') &&
               (header.size() < 3 || header[2] == ' ' || header[2] == '\n' || header[2] == '\r' ||
                header[2] == '\t' || header[2] == '#');
    case Format::Sgi:
        return header.size() >= 4 && uchar(header[0]) == 0x01 && uchar(header[1]) == 0xDA &&
               uchar(header[2]) <= 1 && (header[3] == 1 || header[3] == 2);
    case Format::Ora:
    case Format::Kra: {
        if (!header.startsWith("PK\x03\x04"))
            return false;
        const QByteArray mime =
            f == Format::Ora ? QByteArray("image/openraster") : QByteArray("application/x-krita");
        // The first member is the uncompressed "mimetype" (both specifications).
        if (header.size() >= 38 + mime.size() && header.mid(30, 8) == "mimetype")
            return header.mid(38, mime.size()) == mime;
        return lowerSuffix == (f == Format::Ora ? u"ora" : u"kra");
    }
    }
    return false;
}

std::optional<Probe> probe(Format f, const QByteArray& data) {
    switch (f) {
    case Format::Qoi:
        return probeQoi(data);
    case Format::Pcx: {
        const auto h = pcxHeader(data);
        if (!h)
            return std::nullopt;
        Probe p;
        p.size = QSize(h->w, h->h);
        p.hasAlpha = h->bpp == 8 && h->planes == 4;
        if (h->bpp * h->planes <= 8 && !(h->bpp == 8 && h->planes == 1))
            p.bitsPerComponent = h->bpp * h->planes;
        return p;
    }
    case Format::Pnm: {
        const auto h = pnmHeader(data);
        if (!h)
            return std::nullopt;
        Probe p;
        p.size = QSize(int(h->w), int(h->h));
        p.isFloat = h->kind == 'F' || h->kind == 'f';
        p.bitsPerComponent = p.isFloat ? 32 : (h->maxval > 255 ? 16 : (h->maxval == 1 ? 1 : 8));
        p.hasAlpha = h->depth == 2 || h->depth == 4;
        p.colorModel = h->depth <= 2 ? QStringLiteral("Gray") : QStringLiteral("RGB");
        return p;
    }
    case Format::Sgi: {
        const auto h = sgiHeader(data);
        if (!h)
            return std::nullopt;
        Probe p;
        p.size = QSize(h->w, h->h);
        p.bitsPerComponent = h->bpc * 8;
        p.hasAlpha = h->z == 2 || h->z >= 4;
        p.colorModel = h->z <= 2 ? QStringLiteral("Gray") : QStringLiteral("RGB");
        return p;
    }
    case Format::Ora:
    case Format::Kra:
        return std::nullopt; // archives: see SimpleDecoderBackend.cpp
    }
    return std::nullopt;
}

// ---- QOI ------------------------------------------------------------------------------

std::optional<Raster> decodeQoi(const QByteArray& data, QString* error) {
    const auto p = probeQoi(data);
    if (!p)
        return failWith<Raster>(error, damaged());
    QImage img = allocate(p->size.width(), p->size.height(), QImage::Format_RGBA8888, error);
    if (img.isNull())
        return std::nullopt;
    const uchar* d = bytes(data);
    const qint64 n = data.size();
    qint64 pos = 14;
    uchar index[64][4] = {};
    uchar px[4] = {0, 0, 0, 255};
    int run = 0;
    const int w = img.width();
    for (int y = 0; y < img.height(); ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            if (run > 0) {
                --run;
            } else {
                if (pos >= n)
                    return failWith<Raster>(error, damaged());
                const uchar b = d[pos++];
                if (b == 0xFE) {
                    if (pos + 3 > n)
                        return failWith<Raster>(error, damaged());
                    std::memcpy(px, d + pos, 3);
                    pos += 3;
                } else if (b == 0xFF) {
                    if (pos + 4 > n)
                        return failWith<Raster>(error, damaged());
                    std::memcpy(px, d + pos, 4);
                    pos += 4;
                } else {
                    switch (b >> 6) {
                    case 0:
                        std::memcpy(px, index[b & 0x3F], 4);
                        break;
                    case 1:
                        px[0] = uchar(px[0] + ((b >> 4) & 3) - 2);
                        px[1] = uchar(px[1] + ((b >> 2) & 3) - 2);
                        px[2] = uchar(px[2] + (b & 3) - 2);
                        break;
                    case 2: {
                        if (pos >= n)
                            return failWith<Raster>(error, damaged());
                        const uchar b2 = d[pos++];
                        const int vg = (b & 0x3F) - 32;
                        px[0] = uchar(px[0] + vg - 8 + ((b2 >> 4) & 0x0F));
                        px[1] = uchar(px[1] + vg);
                        px[2] = uchar(px[2] + vg - 8 + (b2 & 0x0F));
                        break;
                    }
                    default:
                        run = b & 0x3F; // this pixel plus `run` more repeat the previous one
                        break;
                    }
                }
                const int h = (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64;
                std::memcpy(index[h], px, 4);
            }
            std::memcpy(line + x * 4, px, 4);
        }
    }
    Raster r;
    r.hasAlpha = p->hasAlpha;
    if (!p->hasAlpha)
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < w; ++x)
                line[x * 4 + 3] = 255;
        }
    img.setColorSpace(QColorSpace(data[13] == 1 ? QColorSpace::SRgbLinear : QColorSpace::SRgb));
    r.image = std::move(img);
    return r;
}

// ---- PCX ------------------------------------------------------------------------------

std::optional<Raster> decodePcx(const QByteArray& data, QString* error) {
    const auto h = pcxHeader(data);
    if (!h)
        return failWith<Raster>(error, damaged());
    const qint64 n = data.size();
    if (!plausibleInput(n - 128, qint64(h->bytesPerLine) * h->planes * h->h))
        return failWith<Raster>(error, damaged());
    QImage img = allocate(h->w, h->h, QImage::Format_RGBA8888, error);
    if (img.isNull())
        return std::nullopt;
    img.fill(Qt::black); // rows after the end of a truncated file
    const uchar* d = bytes(data);
    // 256-colour palette at the end (VGA): 0x0C + 768 bytes.
    uchar palette[256][3];
    std::memset(palette, 0, sizeof palette);
    for (int i = 0; i < 16; ++i)
        std::memcpy(palette[i], d + 16 + i * 3, 3);
    if (h->bpp == 8 && h->planes == 1) {
        if (n >= 128 + 769 && d[n - 769] == 0x0C) {
            std::memcpy(palette, d + n - 768, 768);
        } else {
            for (int i = 0; i < 256; ++i)
                palette[i][0] = palette[i][1] = palette[i][2] = uchar(i); // no palette: gray
        }
    } else if (h->bpp == 1 && h->planes == 1) {
        palette[0][0] = palette[0][1] = palette[0][2] = 0; // monochrome: black/white
        palette[1][0] = palette[1][1] = palette[1][2] = 255;
    }
    const qint64 lineBytes = qint64(h->bytesPerLine) * h->planes;
    QByteArray lineBuf(lineBytes, '\0');
    auto* line = reinterpret_cast<uchar*>(lineBuf.data());
    qint64 pos = 128;
    int runLeft = 0;
    uchar runValue = 0;
    const qint64 dataEnd =
        (h->bpp == 8 && h->planes == 1 && n >= 128 + 769 && d[n - 769] == 0x0C) ? n - 769 : n;
    for (int y = 0; y < h->h; ++y) {
        if (pos >= dataEnd && runLeft == 0) {
            if (y == 0)
                return failWith<Raster>(error, damaged());
            break; // input exhausted: the remaining rows stay black
        }
        for (qint64 i = 0; i < lineBytes; ++i) {
            if (!h->rle) {
                line[i] = pos < dataEnd ? d[pos++] : 0;
                continue;
            }
            if (runLeft == 0) {
                if (pos >= dataEnd) {
                    if (y == 0 && i == 0)
                        return failWith<Raster>(error, damaged());
                    runValue = 0; // truncated: the rest stays black
                    runLeft = int(std::min<qint64>(lineBytes, 0x3FFFFFFF));
                } else {
                    const uchar b = d[pos++];
                    if ((b & 0xC0) == 0xC0) {
                        runLeft = b & 0x3F;
                        runValue = pos < dataEnd ? d[pos++] : 0;
                        if (runLeft == 0)
                            runLeft = 1; // a zero count is invalid; treat it as one
                    } else {
                        runLeft = 1;
                        runValue = b;
                    }
                }
            }
            line[i] = runValue;
            --runLeft;
        }
        uchar* out = img.scanLine(y);
        for (int x = 0; x < h->w; ++x) {
            uchar* o = out + x * 4;
            o[3] = 255;
            if (h->bpp == 8 && h->planes >= 3) {
                o[0] = line[x];
                o[1] = line[h->bytesPerLine + x];
                o[2] = line[2 * h->bytesPerLine + x];
                if (h->planes == 4)
                    o[3] = line[3 * h->bytesPerLine + x];
                continue;
            }
            int idx = 0;
            if (h->bpp == 8) {
                idx = line[x];
            } else if (h->bpp == 4) {
                idx = (line[x / 2] >> ((1 - x % 2) * 4)) & 0x0F;
            } else if (h->bpp == 2) {
                idx = (line[x / 4] >> ((3 - x % 4) * 2)) & 0x03;
            } else { // 1 bit, 1..4 planes
                for (int p = 0; p < h->planes; ++p)
                    idx |= ((line[p * h->bytesPerLine + x / 8] >> (7 - x % 8)) & 1) << p;
            }
            std::memcpy(o, palette[idx], 3);
        }
    }
    Raster r;
    r.hasAlpha = h->bpp == 8 && h->planes == 4;
    img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    r.image = std::move(img);
    return r;
}

// ---- PNM / PAM / PFM ------------------------------------------------------------------

std::optional<Raster> decodePnm(const QByteArray& data, QString* error) {
    const auto h = pnmHeader(data);
    if (!h)
        return failWith<Raster>(error, damaged());
    const bool isFloat = h->kind == 'F' || h->kind == 'f';
    const bool deep = !isFloat && h->maxval > 255;
    // The data must be there before the image is allocated (a tiny file claiming 16k × 16k).
    const qint64 samples = h->w * h->h * h->depth; // <= kMaxPixels * 4
    const qint64 available = data.size() - std::min<qint64>(h->dataOffset, data.size());
    qint64 need = 0;
    switch (h->kind) {
    case '1':
        need = samples; // "0"/"1", whitespace optional
        break;
    case '2':
    case '3':
        need = samples * 2 - 1; // digits separated by whitespace
        break;
    case '4':
        need = (h->w + 7) / 8 * h->h;
        break;
    case 'F':
    case 'f':
        need = samples * 4;
        break;
    default: // P5, P6, P7
        need = samples * (h->maxval > 255 ? 2 : 1);
        break;
    }
    if (h->dataOffset > data.size() || available < need)
        return failWith<Raster>(error, damaged());
    const QImage::Format fmt =
        isFloat ? QImage::Format_RGBA32FPx4 : (deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    QImage img = allocate(h->w, h->h, fmt, error);
    if (img.isNull())
        return std::nullopt;
    const uchar* d = bytes(data);
    const qint64 n = data.size();
    const int w = int(h->w), hh = int(h->h), depth = int(h->depth);
    const qint64 maxval = h->maxval;
    const bool alpha = depth == 2 || depth == 4;
    // Writes one pixel from `depth` samples scaled to 0..1 range of the output depth.
    auto put = [&](int x, int y, const quint32* s) {
        auto scale = [&](quint32 v) -> quint32 {
            v = std::min<quint32>(v, quint32(maxval));
            return deep ? quint32((qint64(v) * 65535 + maxval / 2) / maxval)
                        : quint32((qint64(v) * 255 + maxval / 2) / maxval);
        };
        quint32 r, g, b, a = deep ? 65535 : 255;
        if (depth <= 2) {
            r = g = b = scale(s[0]);
            if (depth == 2)
                a = scale(s[1]);
        } else {
            r = scale(s[0]);
            g = scale(s[1]);
            b = scale(s[2]);
            if (depth == 4)
                a = scale(s[3]);
        }
        if (deep) {
            auto* o = reinterpret_cast<quint16*>(img.scanLine(y)) + x * 4;
            o[0] = quint16(r), o[1] = quint16(g), o[2] = quint16(b), o[3] = quint16(a);
        } else {
            uchar* o = img.scanLine(y) + x * 4;
            o[0] = uchar(r), o[1] = uchar(g), o[2] = uchar(b), o[3] = uchar(a);
        }
    };
    if (isFloat) {
        const qint64 need = qint64(w) * hh * depth * 4;
        if (h->dataOffset > n || n - h->dataOffset < need)
            return failWith<Raster>(error, damaged());
        const bool little = h->scale < 0;
        const uchar* p = d + h->dataOffset;
        for (int row = 0; row < hh; ++row) {
            auto* o = reinterpret_cast<float*>(img.scanLine(hh - 1 - row)); // stored bottom-up
            for (int x = 0; x < w; ++x) {
                float v[3];
                for (int c = 0; c < depth; ++c) {
                    quint32 bits = little ? (quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) |
                                             (quint32(p[3]) << 24))
                                          : be32(p);
                    float f;
                    std::memcpy(&f, &bits, 4);
                    if (!std::isfinite(f) || f < 0)
                        f = 0;
                    v[c] = f; // |scale| is a unit hint only (relative values)
                    p += 4;
                }
                o[x * 4 + 0] = v[0];
                o[x * 4 + 1] = depth == 3 ? v[1] : v[0];
                o[x * 4 + 2] = depth == 3 ? v[2] : v[0];
                o[x * 4 + 3] = 1.0f;
            }
        }
        img.setColorSpace(QColorSpace(QColorSpace::SRgbLinear));
        Raster r;
        r.image = std::move(img);
        r.isFloat = true;
        r.bitsPerComponent = 32;
        r.colorModel = depth == 1 ? QStringLiteral("Gray") : QStringLiteral("RGB");
        return r;
    }
    quint32 s[4] = {};
    if (h->kind == '1' || h->kind == '2' || h->kind == '3') {
        PnmCursor c{d, n, h->dataOffset - 1};
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < w; ++x) {
                for (int k = 0; k < depth; ++k) {
                    if (h->kind == '1') {
                        c.skipSpace();
                        if (c.pos >= n || (d[c.pos] != '0' && d[c.pos] != '1'))
                            return failWith<Raster>(error, damaged());
                        s[k] = d[c.pos++] == '1' ? 0 : 1; // PBM: 1 = black
                    } else {
                        const auto v = c.number();
                        if (!v)
                            return failWith<Raster>(error, damaged());
                        s[k] = quint32(*v);
                    }
                }
                put(x, y, s);
            }
    } else if (h->kind == '4') {
        const qint64 rowBytes = (qint64(w) + 7) / 8;
        if (h->dataOffset > n || n - h->dataOffset < rowBytes * hh)
            return failWith<Raster>(error, damaged());
        for (int y = 0; y < hh; ++y) {
            const uchar* row = d + h->dataOffset + rowBytes * y;
            for (int x = 0; x < w; ++x) {
                s[0] = ((row[x / 8] >> (7 - x % 8)) & 1) ? 0 : 1;
                put(x, y, s);
            }
        }
    } else { // P5, P6, P7
        const int sampleBytes = maxval > 255 ? 2 : 1;
        const qint64 need = qint64(w) * hh * depth * sampleBytes;
        if (h->dataOffset > n || n - h->dataOffset < need)
            return failWith<Raster>(error, damaged());
        const uchar* p = d + h->dataOffset;
        for (int y = 0; y < hh; ++y)
            for (int x = 0; x < w; ++x) {
                for (int k = 0; k < depth; ++k) {
                    s[k] = sampleBytes == 2 ? be16(p) : *p;
                    p += sampleBytes;
                }
                put(x, y, s);
            }
    }
    Raster r;
    r.hasAlpha = alpha;
    r.bitsPerComponent = deep ? 16 : (maxval == 1 ? 1 : 8);
    r.colorModel = depth <= 2 ? QStringLiteral("Gray") : QStringLiteral("RGB");
    img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    r.image = std::move(img);
    return r;
}

// ---- SGI ------------------------------------------------------------------------------

std::optional<Raster> decodeSgi(const QByteArray& data, QString* error) {
    const auto h = sgiHeader(data);
    if (!h || data.size() < 512)
        return failWith<Raster>(error, damaged());
    const bool deep = h->bpc == 2;
    const qint64 n = data.size();
    // Sizes first, then the allocation: a 512-byte file must not cost a 4096² buffer.
    const qint64 imageBytes = qint64(h->w) * h->bpc * h->h * h->z;
    if (h->rle) {
        const qint64 tableEntries = qint64(h->h) * h->z;
        if (tableEntries * 8 > n - 512 || !plausibleInput(n - 512, imageBytes))
            return failWith<Raster>(error, damaged());
    } else if (n - 512 < imageBytes) {
        return failWith<Raster>(error, damaged());
    }
    QImage img = allocate(h->w, h->h, deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888, error);
    if (img.isNull())
        return std::nullopt;
    img.fill(Qt::black); // opaque black; missing channels keep these values
    const uchar* d = bytes(data);
    const int channels = std::min(h->z, 4);
    // Output channel for an SGI channel: gray (+alpha) or RGB(A).
    auto target = [&](int z) -> int {
        if (h->z <= 2)
            return z == 0 ? -1 : 3; // -1 = gray into R, G and B
        return z;
    };
    QByteArray rowBuf(qint64(h->w) * h->bpc, '\0');
    auto* row = reinterpret_cast<uchar*>(rowBuf.data());
    const qint64 rowBytes = qint64(h->w) * h->bpc;
    if (h->rle) {
        const qint64 tableEntries = qint64(h->h) * h->z;
        for (int z = 0; z < channels; ++z)
            for (int y = 0; y < h->h; ++y) {
                const qint64 t = qint64(z) * h->h + y;
                const qint64 start = be32(d + 512 + t * 4);
                const qint64 len = be32(d + 512 + tableEntries * 4 + t * 4);
                if (start > n || len > n - start)
                    return failWith<Raster>(error, damaged());
                std::memset(row, 0, size_t(rowBytes));
                qint64 in = start, end = start + len, out = 0;
                const int unit = h->bpc;
                while (in + unit <= end && out < h->w) {
                    const quint32 v = unit == 2 ? be16(d + in) : d[in];
                    in += unit;
                    const int count = int(v & 0x7F);
                    if (count == 0)
                        break;
                    if (v & 0x80) {
                        for (int k = 0; k < count && out < h->w && in + unit <= end; ++k, ++out, in += unit)
                            std::memcpy(row + out * unit, d + in, size_t(unit));
                    } else {
                        if (in + unit > end)
                            break;
                        for (int k = 0; k < count && out < h->w; ++k, ++out)
                            std::memcpy(row + out * unit, d + in, size_t(unit));
                        in += unit;
                    }
                }
                const int ty = h->h - 1 - y; // stored bottom-up
                const int tc = target(z);
                for (int x = 0; x < h->w; ++x) {
                    const quint16 v = deep ? be16(row + x * 2) : row[x];
                    for (int c = (tc < 0 ? 0 : tc); c <= (tc < 0 ? 2 : tc); ++c) {
                        if (deep)
                            reinterpret_cast<quint16*>(img.scanLine(ty))[x * 4 + c] = v;
                        else
                            img.scanLine(ty)[x * 4 + c] = uchar(v);
                    }
                }
            }
    } else {
        for (int z = 0; z < channels; ++z)
            for (int y = 0; y < h->h; ++y) {
                const uchar* src = d + 512 + (qint64(z) * h->h + y) * rowBytes;
                const int ty = h->h - 1 - y;
                const int tc = target(z);
                for (int x = 0; x < h->w; ++x) {
                    const quint16 v = deep ? be16(src + x * 2) : src[x];
                    for (int c = (tc < 0 ? 0 : tc); c <= (tc < 0 ? 2 : tc); ++c) {
                        if (deep)
                            reinterpret_cast<quint16*>(img.scanLine(ty))[x * 4 + c] = v;
                        else
                            img.scanLine(ty)[x * 4 + c] = uchar(v);
                    }
                }
            }
    }
    Raster r;
    r.hasAlpha = h->z == 2 || h->z >= 4;
    r.bitsPerComponent = deep ? 16 : 8;
    r.colorModel = h->z <= 2 ? QStringLiteral("Gray") : QStringLiteral("RGB");
    img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    r.image = std::move(img);
    return r;
}

} // namespace lf::simplefmt
