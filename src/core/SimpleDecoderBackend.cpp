// SPDX-License-Identifier: GPL-3.0-or-later
// SimpleFormatDecoder (Decoder backend for the own small decoders), OpenRaster/Krita
// archives and the registration (see SimpleDecoders.h).
#include "SimpleDecoders.h"

#include "ZipReader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <cmath>

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::SimpleDecoders)
};

using simplefmt::Format;

constexpr qint64 kProbeBytes = 64 * 1024;
constexpr qint64 kMaxFileBytes = qint64(4) << 30;

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

QSize fitLongEdge(QSize s, int longEdge) {
    const double scale = double(longEdge) / std::max(s.width(), s.height());
    return QSize(std::max(1, int(std::lround(s.width() * scale))),
                 std::max(1, int(std::lround(s.height() * scale))));
}

// Longer edge the result may have (0 = full size): maxPixelSize and the byte budget (P-08).
int targetEdge(QSize full, int bytesPerPixel, const DecodeRequest& req) {
    int edge = req.maxPixelSize;
    if (req.maxBytes > 0 && !full.isEmpty()) {
        const double pixels = double(full.width()) * full.height();
        const double maxPixels = double(std::max<qint64>(1, req.maxBytes / bytesPerPixel));
        if (pixels > maxPixels) {
            const int longE = std::max(full.width(), full.height());
            const int shortE = std::min(full.width(), full.height());
            int e = std::max(1, int(std::floor(longE * std::sqrt(maxPixels / pixels))));
            while (e > 1 && double(e) * std::ceil(double(e) * shortE / longE) > maxPixels)
                --e;
            edge = edge > 0 ? std::min(edge, e) : e;
        }
    }
    if (edge > 0 && std::max(full.width(), full.height()) <= edge)
        return 0;
    return edge;
}

// The merged image inside an OpenRaster/Krita archive, and a smaller fallback.
QStringList mergedCandidates(Format f) {
    if (f == Format::Ora)
        return {QStringLiteral("mergedimage.png"), QStringLiteral("Thumbnails/thumbnail.png")};
    return {QStringLiteral("mergedimage.png"), QStringLiteral("preview.png")};
}

std::optional<QSize> pngSize(const QByteArray& head) {
    if (head.size() < 24 || !head.startsWith("\x89PNG\r\n\x1a\n") || head.mid(12, 4) != "IHDR")
        return std::nullopt;
    const auto* d = reinterpret_cast<const uchar*>(head.constData());
    const quint32 w = (quint32(d[16]) << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
    const quint32 h = (quint32(d[20]) << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
    if (w == 0 || h == 0 || w > quint32(simplefmt::kMaxEdge) || h > quint32(simplefmt::kMaxEdge))
        return std::nullopt;
    return QSize(int(w), int(h));
}

struct LayeredProbe {
    QSize size;
    bool fromFallback = false; // only the small preview exists
    QString member;
};

std::optional<LayeredProbe> probeLayered(Format f, ZipReader& zip) {
    if (!zip.isValid())
        return std::nullopt;
    const QStringList candidates = mergedCandidates(f);
    for (int i = 0; i < candidates.size(); ++i) {
        const auto head = zip.read(candidates[i], 64);
        if (!head)
            continue;
        if (const auto s = pngSize(*head))
            return LayeredProbe{*s, i > 0, candidates[i]};
    }
    return std::nullopt;
}

std::optional<simplefmt::Raster> decodeLayeredZip(Format f, ZipReader& zip, int maxEdge, QString* error,
                                                  bool* fromFallback = nullptr) {
    const auto p = probeLayered(f, zip);
    if (!p) {
        setError(error, zip.isValid() ? Tr::tr("Das Archiv enthält kein zusammengesetztes Bild.")
                                      : zip.errorString());
        return std::nullopt;
    }
    if (qint64(p->size.width()) * p->size.height() > simplefmt::kMaxPixels) {
        setError(error, Tr::tr("Die Bildgröße %1 × %2 ist ungültig oder zu groß.")
                            .arg(p->size.width())
                            .arg(p->size.height()));
        return std::nullopt;
    }
    const auto png = zip.read(p->member);
    if (!png) {
        setError(error, zip.errorString());
        return std::nullopt;
    }
    QByteArray bytes = *png;
    QBuffer buf(&bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "png");
    if (maxEdge > 0 && std::max(p->size.width(), p->size.height()) > maxEdge)
        reader.setScaledSize(fitLongEdge(p->size, maxEdge));
    QImage img = reader.read();
    if (img.isNull()) {
        setError(error, Tr::tr("Das Bild im Archiv ist beschädigt: %1").arg(reader.errorString()));
        return std::nullopt;
    }
    simplefmt::Raster r;
    r.hasAlpha = img.hasAlphaChannel();
    r.bitsPerComponent = img.depth() > 32 ? 16 : 8;
    if (!img.colorSpace().isValid())
        img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    r.image = std::move(img);
    if (fromFallback)
        *fromFallback = p->fromFallback;
    return r;
}

// Reads a whole file (memory mapped where possible) for the in-memory parsers.
struct FileBytes {
    QFile file;
    QByteArray data;
};

bool readFile(const QString& path, FileBytes& fb, QString* error) {
    fb.file.setFileName(path);
    if (!fb.file.open(QIODevice::ReadOnly)) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return false;
    }
    const qint64 size = fb.file.size();
    if (size <= 0 || size > kMaxFileBytes) {
        setError(error,
                 size <= 0 ? Tr::tr("Die Datei ist leer (0 Byte).") : Tr::tr("Die Datei ist zu groß."));
        return false;
    }
    if (uchar* m = fb.file.map(0, size)) {
        fb.data = QByteArray::fromRawData(reinterpret_cast<const char*>(m), size);
        return true;
    }
    fb.data = fb.file.readAll();
    if (fb.data.size() != size) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return false;
    }
    return true;
}

float peakOf(const QImage& img) {
    float peak = 0.f;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const float*>(img.constScanLine(y));
        for (int x = 0; x < img.width() * 4; x += 4)
            for (int c = 0; c < 3; ++c)
                if (std::isfinite(p[x + c])) // one +inf/NaN sample must not hide the real peak
                    peak = std::max(peak, p[x + c]);
    }
    return peak;
}

QString suffixOf(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

} // namespace

// ---- simplefmt helpers ------------------------------------------------------------------

namespace simplefmt {

std::optional<Raster> decodeLayered(Format f, const QString& path, int maxEdge, QString* error) {
    ZipReader zip(path);
    return decodeLayeredZip(f, zip, maxEdge, error);
}

std::optional<Raster> decodeLayeredData(Format f, const QByteArray& data, int maxEdge, QString* error) {
    ZipReader zip(data);
    return decodeLayeredZip(f, zip, maxEdge, error);
}

QString formatName(Format f) {
    switch (f) {
    case Format::Qoi:
        return QStringLiteral("QOI");
    case Format::Pcx:
        return QStringLiteral("PCX");
    case Format::Pnm:
        return QStringLiteral("PNM");
    case Format::Sgi:
        return QStringLiteral("SGI");
    case Format::Ora:
        return QStringLiteral("OpenRaster");
    case Format::Kra:
        return QStringLiteral("Krita");
    }
    return {};
}

QStringList suffixes(Format f) {
    switch (f) {
    case Format::Qoi:
        return {QStringLiteral("qoi")};
    case Format::Pcx:
        return {QStringLiteral("pcx")};
    case Format::Pnm:
        return {QStringLiteral("pbm"), QStringLiteral("pgm"), QStringLiteral("ppm"),
                QStringLiteral("pnm"), QStringLiteral("pam"), QStringLiteral("pfm")};
    case Format::Sgi:
        return {QStringLiteral("sgi"), QStringLiteral("rgb"), QStringLiteral("rgba"), QStringLiteral("bw")};
    case Format::Ora:
        return {QStringLiteral("ora")};
    case Format::Kra:
        return {QStringLiteral("kra")};
    }
    return {};
}

} // namespace simplefmt

// ---- SimpleFormatDecoder ------------------------------------------------------------------

QString SimpleFormatDecoder::name() const {
    return QStringLiteral("Lumaflip ") + simplefmt::formatName(m_format);
}

bool SimpleFormatDecoder::canDecode(const QString& path, const QByteArray& header) const {
    return simplefmt::matchesSignature(m_format, header, suffixOf(path));
}

std::optional<ImageInfo> SimpleFormatDecoder::info(const QString& path, QString* error) {
    ImageInfo info;
    info.formatName = simplefmt::formatName(m_format);
    if (m_format == Format::Ora || m_format == Format::Kra) {
        ZipReader zip(path);
        const auto p = probeLayered(m_format, zip);
        if (!p) {
            setError(error, zip.isValid() ? Tr::tr("Das Archiv enthält kein zusammengesetztes Bild.")
                                          : zip.errorString());
            return std::nullopt;
        }
        info.pixelSize = info.displaySize = p->size;
        info.hasAlpha = true;
        info.colorModel = QStringLiteral("RGB");
        return info;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setError(error, Tr::tr("Die Datei kann nicht gelesen werden."));
        return std::nullopt;
    }
    const auto p = simplefmt::probe(m_format, f.read(kProbeBytes));
    if (!p) {
        setError(error, Tr::tr("Die Datei ist beschädigt oder unvollständig."));
        return std::nullopt;
    }
    info.pixelSize = info.displaySize = p->size;
    info.hasAlpha = p->hasAlpha;
    info.bitsPerComponent = p->bitsPerComponent;
    info.isHdr = p->isFloat;
    info.colorModel = p->colorModel;
    if (m_format == Format::Pnm && p->isFloat)
        info.formatName = QStringLiteral("PFM");
    return info;
}

std::optional<DecodedImage> SimpleFormatDecoder::decode(const QString& path, const DecodeRequest& req,
                                                        QString* error) {
    if (req.page > 0 || req.frame > 0) {
        setError(error, Tr::tr("Seite %1 existiert nicht.").arg(std::max(req.page, req.frame) + 1));
        return std::nullopt;
    }
    std::optional<simplefmt::Raster> raster;
    bool fromFallback = false;
    QSize full;
    if (m_format == Format::Ora || m_format == Format::Kra) {
        ZipReader zip(path);
        const auto p = probeLayered(m_format, zip);
        const int edge = p ? targetEdge(p->size, 8, req) : 0;
        raster = decodeLayeredZip(m_format, zip, edge, error, &fromFallback);
        if (p)
            full = p->size;
    } else {
        FileBytes fb;
        if (!readFile(path, fb, error))
            return std::nullopt;
        switch (m_format) {
        case Format::Qoi:
            raster = simplefmt::decodeQoi(fb.data, error);
            break;
        case Format::Pcx:
            raster = simplefmt::decodePcx(fb.data, error);
            break;
        case Format::Pnm:
            raster = simplefmt::decodePnm(fb.data, error);
            break;
        case Format::Sgi:
            raster = simplefmt::decodeSgi(fb.data, error);
            break;
        default:
            break;
        }
        if (raster)
            full = raster->image.size();
    }
    if (!raster)
        return std::nullopt;
    QImage img = std::move(raster->image);
    DecodedImage out;
    if (raster->isFloat) {
        const float peak = peakOf(img);
        // Float sources keep their range also for the SDR preview's exposure (hdrFloat).
        if ((req.hdr || req.hdrFloat) && peak > 1.0f) {
            // F-COL-03: extended range, like the ImageIO backend (linear Display P3).
            img.convertToColorSpace(
                QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear));
            img.convertTo(QImage::Format_RGBA16FPx4_Premultiplied);
            out.headroom = peak;
        } else {
            img.convertTo(QImage::Format_RGBA64_Premultiplied); // clipped to 0..1 (SDR)
        }
    } else {
        img.convertTo(raster->bitsPerComponent > 8 ? QImage::Format_RGBA64_Premultiplied
                                                   : QImage::Format_RGBA8888_Premultiplied);
    }
    if (img.isNull()) {
        setError(error, Tr::tr("Nicht genug Arbeitsspeicher für das Bild."));
        return std::nullopt;
    }
    const int bpp = img.depth() / 8;
    if (const int edge = targetEdge(img.size(), bpp, req); edge > 0) {
        const QColorSpace cs = img.colorSpace();
        img = img.scaled(fitLongEdge(img.size(), edge), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        img.setColorSpace(cs);
    }
    out.fullSize = full.isValid() ? full : img.size();
    out.isPreview =
        fromFallback || img.width() < out.fullSize.width() || img.height() < out.fullSize.height();
    out.image = std::move(img);
    return out;
}

Metadata SimpleFormatDecoder::metadata(const QString& path) {
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

QList<std::shared_ptr<Decoder>> simpleDecoders() {
    QList<std::shared_ptr<Decoder>> out;
    for (Format f : {Format::Qoi, Format::Pcx, Format::Pnm, Format::Sgi, Format::Ora, Format::Kra})
        out.append(std::make_shared<SimpleFormatDecoder>(f));
    return out;
}

QStringList simpleDecoderSuffixes() {
    QStringList out;
    for (Format f : {Format::Qoi, Format::Pcx, Format::Pnm, Format::Sgi, Format::Ora, Format::Kra})
        out << simplefmt::suffixes(f);
    return out;
}

} // namespace lf
