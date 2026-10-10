// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Own small decoders for the "Rest" format group (SPEC 4.4, M3): QOI, PCX, PNM/PAM/PFM
// (incl. 16 bit and float), SGI, OpenRaster (.ora) and Krita (.kra, merged image).
// Portable C++ on QtCore/QtGui only: the backends are registered like QtImageDecoder
// (registerPortableDecoders(), so they can run in the decoder helper process, SPEC 8.5).
//
// Robustness (SPEC 8.1/8.5): every parser works on a byte buffer with explicit bounds
// checks; sizes from the file are validated before anything is allocated (at most
// kMaxPixels, kMaxEdge per edge); damaged input yields an error, never a crash. The
// decode*() functions are the fuzzing entry points.
//
// Output (like the other backends): Format_RGBA8888_Premultiplied for 8-bit sources,
// Format_RGBA64_Premultiplied for 16-bit ones, sRGB (QOI with linear colour space flag:
// linear sRGB). PFM is linear float: with DecodeRequest::hdr and values above 1 the result
// is Format_RGBA16FPx4_Premultiplied in extended linear Display P3 with headroom > 1,
// otherwise it is clipped to 16 bit linear sRGB.

#include "Decoder.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace lf {

namespace simplefmt {

constexpr qint64 kMaxPixels = qint64(1) << 28; // 268 MP
constexpr int kMaxEdge = 1 << 16;

enum class Format { Qoi, Pcx, Pnm, Sgi, Ora, Kra };

// Result of a pure decode: the image in its native depth plus what info() reports.
struct Raster {
    QImage image; // RGBA8888(_Premultiplied), RGBA64(_Premultiplied) or RGBA32FPx4
    int bitsPerComponent = 8;
    bool hasAlpha = false;
    bool isFloat = false; // PFM
    QString colorModel = QStringLiteral("RGB");
};

// Header-only probes: size (and depth) without decoding the pixels; nullopt if the header
// is not valid for the format. `data` may be just the first bytes of the file.
struct Probe {
    QSize size;
    int bitsPerComponent = 8;
    bool hasAlpha = false;
    bool isFloat = false;
    QString colorModel = QStringLiteral("RGB");
};

bool matchesSignature(Format f, const QByteArray& header, const QString& lowerSuffix);
std::optional<Probe> probe(Format f, const QByteArray& data);

std::optional<Raster> decodeQoi(const QByteArray& data, QString* error);
std::optional<Raster> decodePcx(const QByteArray& data, QString* error);
std::optional<Raster> decodePnm(const QByteArray& data, QString* error); // P1–P7, PF, Pf
std::optional<Raster> decodeSgi(const QByteArray& data, QString* error);
// ORA/KRA: the merged image of the archive (PNG), `maxEdge` > 0 scales while decoding.
std::optional<Raster> decodeLayered(Format f, const QString& path, int maxEdge, QString* error);
std::optional<Raster> decodeLayeredData(Format f, const QByteArray& zip, int maxEdge, QString* error);

QString formatName(Format f);
QStringList suffixes(Format f);

} // namespace simplefmt

// One backend per format (priority 20: after ImageIO and SVG, before the Qt plugins).
class SimpleFormatDecoder final : public Decoder {
public:
    explicit SimpleFormatDecoder(simplefmt::Format format) : m_format(format) {}
    QString name() const override;
    int priority() const override { return 20; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;
    simplefmt::Format format() const { return m_format; }

private:
    simplefmt::Format m_format;
};

// Registers all of the above and their suffixes (called by registerPortableDecoders()).
void registerSimpleDecoders(DecoderRegistry& registry);
// Suffixes of the own decoders (qoi, pcx, pbm, …, kra).
QStringList simpleDecoderSuffixes();

} // namespace lf
