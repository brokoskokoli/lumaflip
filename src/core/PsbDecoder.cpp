// SPDX-License-Identifier: GPL-3.0-or-later
// Photoshop Large Document decoder (see PsbDecoder.h). Format reference: Adobe's
// "Photoshop File Formats Specification" (header, colour mode data, image resources, layer
// and mask information, image data).
#include "PsbDecoder.h"

#include "OwnDecoderUtil.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QVector>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lf {

namespace {

Q_LOGGING_CATEGORY(lcPsb, "lf.psb", QtInfoMsg)

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::PsbDecoder)
};

using decutil::Reader;

constexpr qint64 kMaxFileBytes = qint64(64) << 30;
constexpr quint16 kResourceIcc = 0x040F;
constexpr quint16 kResourceVersionInfo = 0x0421;
const char* const kGenericCmykProfile = "/System/Library/ColorSync/Profiles/Generic CMYK Profile.icc";

enum ColorMode {
    Bitmap = 0,
    Grayscale = 1,
    Indexed = 2,
    Rgb = 3,
    Cmyk = 4,
    Multichannel = 7,
    Duotone = 8,
    Lab = 9
};

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

QString damaged() {
    return Tr::tr("Die PSB-Datei ist beschädigt oder unvollständig.");
}

struct Header {
    int version = 2;
    int channels = 0;
    quint32 width = 0, height = 0;
    int depth = 8;
    int mode = Rgb;
};

int colorChannels(int mode) {
    switch (mode) {
    case Rgb:
    case Lab:
        return 3;
    case Cmyk:
        return 4;
    default:
        return 1;
    }
}

QString colorModelName(int mode) {
    switch (mode) {
    case Cmyk:
        return QStringLiteral("CMYK");
    case Grayscale:
    case Bitmap:
    case Duotone:
    case Multichannel:
        return QStringLiteral("Gray");
    default:
        return QStringLiteral("RGB");
    }
}

std::optional<Header> parseHeader(const QByteArray& data, bool acceptPsd, QString* error) {
    Reader r(data);
    if (r.bytes(4) != "8BPS") {
        setError(error, Tr::tr("Keine PSB-Datei."));
        return std::nullopt;
    }
    Header h;
    h.version = r.u16();
    r.skip(6);
    h.channels = r.u16();
    h.height = r.u32();
    h.width = r.u32();
    h.depth = r.u16();
    h.mode = r.u16();
    if (!r.ok() || (h.version != 2 && !(acceptPsd && h.version == 1))) {
        setError(error, r.ok() ? Tr::tr("Keine PSB-Datei.") : damaged());
        return std::nullopt;
    }
    const quint32 maxEdge = h.version == 2 ? quint32(psb::kMaxEdge) : 30000u;
    if (h.width == 0 || h.height == 0 || h.width > maxEdge || h.height > maxEdge) {
        setError(error,
                 Tr::tr("Die Bildgröße %1 × %2 ist ungültig oder zu groß.").arg(h.width).arg(h.height));
        return std::nullopt;
    }
    const bool depthOk = h.depth == 8 || h.depth == 16 || h.depth == 32 || (h.depth == 1 && h.mode == Bitmap);
    const bool modeOk = h.mode == Bitmap || h.mode == Grayscale || h.mode == Indexed || h.mode == Rgb ||
                        h.mode == Cmyk || h.mode == Multichannel || h.mode == Duotone || h.mode == Lab;
    if (!depthOk || !modeOk || h.channels < 1 || h.channels > 56 || h.channels < colorChannels(h.mode) ||
        (h.mode == Bitmap && h.depth != 1) || (h.mode == Indexed && h.depth != 8)) {
        setError(error, Tr::tr("Die Bittiefe oder der Farbmodus der PSB-Datei wird nicht unterstützt."));
        return std::nullopt;
    }
    return h;
}

// ---- Lab (D50) -> sRGB ------------------------------------------------------------------------

void labToSrgb(float L, float a, float b, float* rgb) {
    auto finv = [](float t) {
        constexpr float d = 6.0f / 29.0f;
        return t > d ? t * t * t : 3.0f * d * d * (t - 4.0f / 29.0f);
    };
    const float fy = (L + 16.0f) / 116.0f;
    const float X = 0.9642f * finv(fy + a / 500.0f);
    const float Y = finv(fy);
    const float Z = 0.8249f * finv(fy - b / 200.0f);
    // XYZ (D50) -> linear sRGB (Bradford-adapted).
    const float r = 3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z;
    const float g = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z;
    const float bl = 0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;
    rgb[0] = decutil::linearToSrgb(r);
    rgb[1] = decutil::linearToSrgb(g);
    rgb[2] = decutil::linearToSrgb(bl);
}

// ---- image data access ------------------------------------------------------------------------

class Planes {
public:
    Planes(const QByteArray& data, const Header& h) : m_data(data), m_h(h) {
        m_rowBytes = h.depth == 1 ? (qint64(h.width) + 7) / 8 : qint64(h.width) * (h.depth / 8);
    }
    qint64 rowBytes() const { return m_rowBytes; }

    bool open(qint64 offset, QString* error) {
        Reader r(m_data);
        r.seek(offset);
        m_compression = r.u16();
        if (!r.ok())
            return fail(error, "image data header");
        const qint64 rows = qint64(m_h.channels) * m_h.height;
        switch (m_compression) {
        case 0:
            m_base = r.pos();
            if (rows * m_rowBytes > r.remaining())
                return fail(error, "raw image data truncated");
            return true;
        case 1: {
            const int countBytes = m_h.version == 2 ? 4 : 2;
            if (rows > r.remaining() / countBytes)
                return fail(error, "RLE row table truncated");
            m_rowOffsets.resize(rows + 1);
            qint64 pos = r.pos() + rows * countBytes;
            for (qint64 i = 0; i < rows; ++i) {
                m_rowOffsets[i] = pos;
                pos += countBytes == 4 ? qint64(r.u32()) : qint64(r.u16());
            }
            m_rowOffsets[rows] = pos;
            if (!r.ok() || pos > r.size())
                return fail(error, "RLE data truncated");
            return true;
        }
        case 2:
        case 3: {
            const qint64 total = rows * m_rowBytes;
            if (total > psb::kMaxUnzipped)
                return fail(error, "ZIP image data too large");
            m_unzipped.resize(total);
            uLongf destLen = uLongf(total);
            uLong srcLen = uLong(r.remaining());
            const int rc =
                uncompress2(reinterpret_cast<Bytef*>(m_unzipped.data()), &destLen, r.here(), &srcLen);
            if ((rc != Z_OK && rc != Z_BUF_ERROR) || qint64(destLen) != total)
                return fail(error, "ZIP image data damaged");
            if (m_compression == 3)
                unpredict();
            return true;
        }
        default:
            setError(error, Tr::tr("Die Komprimierung der PSB-Datei wird nicht unterstützt."));
            return false;
        }
    }

    // Row `y` of channel `c` (rowBytes() bytes) into `out`.
    bool row(int c, quint32 y, uchar* out) const {
        const qint64 index = qint64(c) * m_h.height + y;
        switch (m_compression) {
        case 0:
            std::memcpy(out, m_data.constData() + m_base + index * m_rowBytes, size_t(m_rowBytes));
            return true;
        case 1:
            return unpackBits(reinterpret_cast<const uchar*>(m_data.constData()) + m_rowOffsets[index],
                              m_rowOffsets[index + 1] - m_rowOffsets[index], out);
        default:
            std::memcpy(out, m_unzipped.constData() + index * m_rowBytes, size_t(m_rowBytes));
            return true;
        }
    }

private:
    bool fail(QString* error, const char* why) const {
        qCDebug(lcPsb) << "damaged:" << why;
        setError(error, damaged());
        return false;
    }
    bool unpackBits(const uchar* src, qint64 len, uchar* out) const {
        qint64 s = 0, d = 0;
        while (d < m_rowBytes) {
            if (s >= len)
                return false;
            const int n = src[s] >= 0x80 ? int(src[s]) - 0x100 : int(src[s]); // signed run length
            ++s;
            if (n >= 0) {
                const qint64 count = n + 1;
                if (s + count > len || d + count > m_rowBytes)
                    return false;
                std::memcpy(out + d, src + s, size_t(count));
                s += count;
                d += count;
            } else if (n != -128) {
                const qint64 count = 1 - n;
                if (s >= len || d + count > m_rowBytes)
                    return false;
                std::memset(out + d, src[s++], size_t(count));
                d += count;
            }
        }
        return true;
    }
    void unpredict() {
        const qint64 rows = qint64(m_h.channels) * m_h.height;
        auto* base = reinterpret_cast<uchar*>(m_unzipped.data());
        const qint64 w = m_h.width;
        QByteArray tmp;
        for (qint64 i = 0; i < rows; ++i) {
            uchar* row = base + i * m_rowBytes;
            if (m_h.depth == 8) {
                for (qint64 x = 1; x < w; ++x)
                    row[x] = uchar(row[x] + row[x - 1]);
            } else if (m_h.depth == 16) {
                quint16 prev = quint16((row[0] << 8) | row[1]);
                for (qint64 x = 1; x < w; ++x) {
                    const quint16 v = quint16(((row[2 * x] << 8) | row[2 * x + 1]) + prev);
                    row[2 * x] = uchar(v >> 8);
                    row[2 * x + 1] = uchar(v);
                    prev = v;
                }
            } else if (m_h.depth == 32) {
                for (qint64 x = 1; x < 4 * w; ++x)
                    row[x] = uchar(row[x] + row[x - 1]);
                tmp.resize(4 * w); // bytes are stored as planes (all byte 0, all byte 1 …)
                auto* t = reinterpret_cast<uchar*>(tmp.data());
                for (qint64 x = 0; x < w; ++x)
                    for (int b = 0; b < 4; ++b)
                        t[4 * x + b] = row[b * w + x];
                std::memcpy(row, t, size_t(4 * w));
            }
        }
    }

    const QByteArray& m_data;
    Header m_h;
    qint64 m_rowBytes = 0;
    int m_compression = 0;
    qint64 m_base = 0;
    QVector<qint64> m_rowOffsets;
    QByteArray m_unzipped;
};

float sampleAt(const uchar* row, quint32 x, int depth) {
    switch (depth) {
    case 1:
        return (row[x >> 3] >> (7 - (x & 7))) & 1 ? 0.0f : 1.0f; // 1 = black
    case 8:
        return row[x] / 255.0f;
    case 16:
        return float((row[2 * x] << 8) | row[2 * x + 1]) / 65535.0f;
    default: {
        const uchar* p = row + 4 * x;
        const quint32 u =
            (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
        float f;
        std::memcpy(&f, &u, sizeof f);
        return std::isfinite(f) ? f : 0.0f;
    }
    }
}

struct Sections {
    QByteArray colorModeData;
    QByteArray icc;
    bool realMergedData = true;
    qint64 imageData = 0;
};

std::optional<Sections> readSections(const QByteArray& data, const Header& h, QString* error) {
    Reader r(data);
    r.seek(26);
    Sections s;
    const quint32 cmLen = r.u32();
    s.colorModeData = r.bytes(cmLen);
    const quint32 resLen = r.u32();
    const qint64 resEnd = r.pos() + qint64(resLen);
    if (!r.ok() || resEnd > r.size()) {
        setError(error, damaged());
        return std::nullopt;
    }
    while (r.ok() && r.pos() + 12 <= resEnd) {
        const QByteArray sig = r.bytes(4);
        const quint16 id = r.u16();
        const quint8 nameLen = r.u8();
        r.skip(nameLen + ((nameLen + 1) % 2)); // pascal string padded to even
        const quint32 size = r.u32();
        if (!r.ok() || r.pos() + qint64(size) > resEnd)
            break;
        if (sig == "8BIM" && id == kResourceIcc)
            s.icc = QByteArray(reinterpret_cast<const char*>(r.here()), size);
        else if (sig == "8BIM" && id == kResourceVersionInfo && size >= 5)
            s.realMergedData = r.here()[4] != 0;
        r.skip(qint64(size) + (size % 2));
    }
    if (!r.seek(resEnd)) {
        setError(error, damaged());
        return std::nullopt;
    }
    const qint64 layerLen = h.version == 2 ? qint64(r.u64()) : qint64(r.u32());
    if (!r.ok() || layerLen < 0 || layerLen > r.remaining()) {
        setError(error, damaged());
        return std::nullopt;
    }
    r.skip(layerLen);
    s.imageData = r.pos();
    return s;
}

QColorSpace cmykSpace(const QByteArray& icc) {
    if (!icc.isEmpty()) {
        const QColorSpace cs = QColorSpace::fromIccProfile(icc);
        if (cs.isValid() && cs.colorModel() == QColorSpace::ColorModel::Cmyk)
            return cs;
    }
    static const QColorSpace generic = [] {
        QFile f(QString::fromUtf8(kGenericCmykProfile));
        if (!f.open(QIODevice::ReadOnly))
            return QColorSpace();
        return QColorSpace::fromIccProfile(f.readAll());
    }();
    return generic;
}

} // namespace

// ---- psb ------------------------------------------------------------------------------------------

namespace psb {

bool matchesSignature(const QByteArray& header, bool acceptPsd) {
    if (header.size() < 6 || !header.startsWith("8BPS"))
        return false;
    const int version = (uchar(header.at(4)) << 8) | uchar(header.at(5));
    return version == 2 || (acceptPsd && version == 1);
}

std::optional<Probe> probe(const QByteArray& data, QString* error) {
    const auto h = parseHeader(data, true, error);
    if (!h)
        return std::nullopt;
    Probe p;
    p.size = QSize(int(h->width), int(h->height));
    p.version = h->version;
    p.channels = h->channels;
    p.bitsPerComponent = h->depth;
    p.colorMode = h->mode;
    p.colorModel = colorModelName(h->mode);
    return p;
}

std::optional<Result> decode(const QByteArray& data, int maxEdge, QString* error) {
    const auto h = parseHeader(data, true, error);
    if (!h)
        return std::nullopt;
    const auto sec = readSections(data, *h, error);
    if (!sec)
        return std::nullopt;
    Result res;
    res.fullSize = QSize(int(h->width), int(h->height));
    res.bitsPerComponent = h->depth;
    res.colorModel = colorModelName(h->mode);
    if (!sec->realMergedData) {
        res.approximate = true;
        res.notes << QStringLiteral("file saved without a real merged image (maximize compatibility off)");
    }
    if (h->mode == Duotone || h->mode == Multichannel) {
        res.approximate = true;
        res.notes << QStringLiteral("duotone/multichannel shown as gray");
    }
    Planes planes(data, *h);
    if (!planes.open(sec->imageData, error))
        return std::nullopt;

    const int k = decutil::subsampleFactor(res.fullSize, maxEdge, kMaxPixels);
    const int ow = int((h->width + quint32(k) - 1) / quint32(k)),
              oh = int((h->height + quint32(k) - 1) / quint32(k));
    const int colors = colorChannels(h->mode);
    const bool alpha = h->channels > colors && h->mode != Multichannel && h->mode != Bitmap;
    res.hasAlpha = alpha;
    const int used = colors + (alpha ? 1 : 0);
    const bool isFloat = h->depth == 32;
    const bool cmyk = h->mode == Cmyk;

    // Intermediate: straight RGBA (or CMYK ink + alpha) as floats per output pixel.
    QImage out(ow, oh, QImage::Format_RGBA32FPx4);
    if (out.isNull()) {
        setError(error, Tr::tr("Nicht genug Arbeitsspeicher für das Bild."));
        return std::nullopt;
    }
    QVector<QByteArray> rows(used);
    for (QByteArray& b : rows)
        b.resize(planes.rowBytes());
    const QByteArray& palette = sec->colorModeData;
    for (int oy = 0; oy < oh; ++oy) {
        const quint32 y = quint32(oy) * quint32(k);
        for (int c = 0; c < used; ++c) {
            if (!planes.row(c, y, reinterpret_cast<uchar*>(rows[c].data()))) {
                setError(error, damaged());
                return std::nullopt;
            }
        }
        auto* d = reinterpret_cast<float*>(out.scanLine(oy));
        for (int ox = 0; ox < ow; ++ox) {
            const quint32 x = quint32(ox) * quint32(k);
            float v[5] = {0, 0, 0, 0, 0};
            for (int c = 0; c < used; ++c)
                v[c] = sampleAt(reinterpret_cast<const uchar*>(rows[c].constData()), x, h->depth);
            const float a = alpha ? std::clamp(v[colors], 0.0f, 1.0f) : 1.0f;
            float* p = d + ox * 4;
            switch (h->mode) {
            case Rgb:
                p[0] = v[0], p[1] = v[1], p[2] = v[2];
                break;
            case Indexed: {
                const int idx = int(std::lround(v[0] * 255.0f));
                if (palette.size() >= 768) {
                    p[0] = uchar(palette.at(idx)) / 255.0f;
                    p[1] = uchar(palette.at(256 + idx)) / 255.0f;
                    p[2] = uchar(palette.at(512 + idx)) / 255.0f;
                } else {
                    p[0] = p[1] = p[2] = v[0];
                }
                break;
            }
            case Cmyk: // stored inverted (1 = no ink); kept as ink amounts C, M, Y in p, K below
                p[0] = 1.0f - v[0], p[1] = 1.0f - v[1], p[2] = 1.0f - v[2];
                break;
            case Lab:
                if (h->depth == 8)
                    labToSrgb(v[0] * 100.0f, v[1] * 255.0f - 128.0f, v[2] * 255.0f - 128.0f, p);
                else
                    labToSrgb(v[0] * 100.0f, v[1] * 256.0f - 128.0f, v[2] * 256.0f - 128.0f, p);
                break;
            case Multichannel:
                p[0] = p[1] = p[2] = 1.0f - v[0];
                break;
            default: // gray, bitmap, duotone
                p[0] = p[1] = p[2] = v[0];
                break;
            }
            if (cmyk) {
                // Undo the white matte in ink space (white = no ink): ink = ink_matted / a.
                const float k4 = 1.0f - v[3];
                const float inv = a > 0.0f ? 1.0f / a : 0.0f;
                p[0] = std::clamp(p[0] * inv, 0.0f, 1.0f);
                p[1] = std::clamp(p[1] * inv, 0.0f, 1.0f);
                p[2] = std::clamp(p[2] * inv, 0.0f, 1.0f);
                // Pack K into the alpha slot for now; alpha is applied after the conversion.
                p[3] = std::clamp(k4 * inv, 0.0f, 1.0f);
            } else {
                if (alpha && a > 0.0f && a < 1.0f) // undo the white matte: c = (c - (1 - a)) / a
                    for (int i = 0; i < 3; ++i)
                        p[i] = (p[i] - (1.0f - a)) / a;
                for (int i = 0; i < 3; ++i)
                    p[i] = isFloat ? std::max(0.0f, p[i]) : std::clamp(p[i], 0.0f, 1.0f);
                p[3] = a;
            }
        }
    }

    if (cmyk) {
        // Ink -> Format_CMYK8888, converted with the profile; alpha re-applied afterwards.
        QImage ink(ow, oh, QImage::Format_CMYK8888);
        QImage alphaPlane(alpha ? ow : 0, alpha ? oh : 0, QImage::Format_Alpha8);
        if (ink.isNull() || (alpha && alphaPlane.isNull())) {
            setError(error, Tr::tr("Nicht genug Arbeitsspeicher für das Bild."));
            return std::nullopt;
        }
        // The alpha channel is read once more per row (the float buffer holds K in its place).
        for (int oy = 0; oy < oh; ++oy) {
            const auto* s = reinterpret_cast<const float*>(out.constScanLine(oy));
            uchar* d = ink.scanLine(oy);
            for (int ox = 0; ox < ow; ++ox)
                for (int i = 0; i < 4; ++i)
                    d[ox * 4 + i] = uchar(std::lround(s[ox * 4 + i] * 255.0f));
            if (alpha) {
                QByteArray arow(planes.rowBytes(), '\0');
                if (!planes.row(colors, quint32(oy) * quint32(k), reinterpret_cast<uchar*>(arow.data()))) {
                    setError(error, damaged());
                    return std::nullopt;
                }
                uchar* ad = alphaPlane.scanLine(oy);
                for (int ox = 0; ox < ow; ++ox)
                    ad[ox] = uchar(
                        std::lround(std::clamp(sampleAt(reinterpret_cast<const uchar*>(arow.constData()),
                                                        quint32(ox) * quint32(k), h->depth),
                                               0.0f, 1.0f) *
                                    255.0f));
            }
        }
        QImage rgb;
        const QColorSpace cs = cmykSpace(sec->icc);
        if (cs.isValid()) {
            ink.setColorSpace(cs);
            rgb = ink.convertedToColorSpace(QColorSpace(QColorSpace::DisplayP3), QImage::Format_RGBA8888);
        }
        if (rgb.isNull()) { // naive conversion (no CMYK profile available)
            res.approximate = true;
            res.notes << QStringLiteral("CMYK converted without a profile");
            rgb = QImage(ow, oh, QImage::Format_RGBA8888);
            for (int oy = 0; oy < oh; ++oy) {
                const uchar* s = ink.constScanLine(oy);
                uchar* d = rgb.scanLine(oy);
                for (int ox = 0; ox < ow; ++ox) {
                    const int kk = 255 - s[ox * 4 + 3];
                    for (int i = 0; i < 3; ++i)
                        d[ox * 4 + i] = uchar((255 - s[ox * 4 + i]) * kk / 255);
                    d[ox * 4 + 3] = 255;
                }
            }
            rgb.setColorSpace(QColorSpace(QColorSpace::SRgb));
        }
        if (alpha) {
            for (int oy = 0; oy < oh; ++oy) {
                uchar* d = rgb.scanLine(oy);
                const uchar* a = alphaPlane.constScanLine(oy);
                for (int ox = 0; ox < ow; ++ox)
                    d[ox * 4 + 3] = a[ox];
            }
        }
        rgb.convertTo(QImage::Format_RGBA8888_Premultiplied);
        out = std::move(rgb);
    } else {
        QColorSpace cs;
        if (!sec->icc.isEmpty() && h->mode != Lab) {
            cs = QColorSpace::fromIccProfile(sec->icc);
            if (cs.isValid() && cs.colorModel() != QColorSpace::ColorModel::Rgb)
                cs = QColorSpace(); // gray profiles: the data is expanded to RGB
        }
        if (isFloat)
            cs =
                cs.isValid() ? cs.withTransferFunction(QColorSpace::TransferFunction::Linear) : QColorSpace();
        if (!cs.isValid())
            cs = isFloat ? QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Linear)
                         : QColorSpace(QColorSpace::SRgb);
        out.setColorSpace(cs);
        out.convertTo(h->depth > 8 ? QImage::Format_RGBA64_Premultiplied
                                   : QImage::Format_RGBA8888_Premultiplied);
    }
    if (out.isNull()) {
        setError(error, Tr::tr("Nicht genug Arbeitsspeicher für das Bild."));
        return std::nullopt;
    }
    if (maxEdge > 0 && std::max(out.width(), out.height()) > maxEdge) {
        const QColorSpace cs = out.colorSpace();
        out = out.scaled(decutil::fitLongEdge(res.fullSize, maxEdge), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
        out.setColorSpace(cs);
    }
    res.image = std::move(out);
    if (res.approximate)
        qCInfo(lcPsb).noquote() << "approximate rendering:" << res.notes.join(QStringLiteral("; "));
    return res;
}

} // namespace psb

// ---- PsbDecoder -----------------------------------------------------------------------------------

bool PsbDecoder::canDecode(const QString& path, const QByteArray& header) const {
    Q_UNUSED(path);
    return psb::matchesSignature(header);
}

std::optional<ImageInfo> PsbDecoder::info(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return std::nullopt;
    }
    const QByteArray head = f.read(64);
    if (!psb::matchesSignature(head)) {
        setError(error, Tr::tr("Keine PSB-Datei."));
        return std::nullopt;
    }
    const auto p = psb::probe(head, error);
    if (!p)
        return std::nullopt;
    ImageInfo info;
    info.pixelSize = info.displaySize = p->size;
    info.hasAlpha =
        p->channels > colorChannels(p->colorMode) && p->colorMode != Multichannel && p->colorMode != Bitmap;
    info.bitsPerComponent = p->bitsPerComponent;
    info.isHdr = p->bitsPerComponent == 32;
    info.colorModel = p->colorModel;
    info.formatName = QStringLiteral("PSB");
    return info;
}

std::optional<DecodedImage> PsbDecoder::decode(const QString& path, const DecodeRequest& req,
                                               QString* error) {
    if (req.page > 0 || req.frame > 0) {
        setError(error, Tr::tr("Seite %1 existiert nicht.").arg(std::max(req.page, req.frame) + 1));
        return std::nullopt;
    }
    decutil::FileBytes fb;
    if (!decutil::readFile(path, fb, kMaxFileBytes, error, Tr::tr("Die Datei kann nicht gelesen werden."),
                           Tr::tr("Die Datei ist leer (0 Byte)."), Tr::tr("Die Datei ist zu groß.")))
        return std::nullopt;
    if (!psb::matchesSignature(fb.data)) {
        setError(error, Tr::tr("Keine PSB-Datei."));
        return std::nullopt;
    }
    const auto p = psb::probe(fb.data, error);
    if (!p)
        return std::nullopt;
    const int edge = decutil::targetEdge(p->size, p->bitsPerComponent > 8 ? 8 : 4, req);
    auto r = psb::decode(fb.data, edge, error);
    if (!r)
        return std::nullopt;
    DecodedImage out;
    out.fullSize = r->fullSize;
    out.isPreview = r->image.width() < r->fullSize.width() || r->image.height() < r->fullSize.height();
    out.image = std::move(r->image);
    return out;
}

Metadata PsbDecoder::metadata(const QString& path) {
    Metadata md;
    const auto i = info(path, nullptr);
    if (!i)
        return md;
    addMetadata(md, QStringLiteral("file.format"), i->formatName);
    addMetadata(md, QStringLiteral("file.dimensions"),
                QStringLiteral("%1 × %2").arg(i->displaySize.width()).arg(i->displaySize.height()));
    addMetadata(md, QStringLiteral("file.bitDepth"), i->bitsPerComponent);
    return md;
}

} // namespace lf
