// SPDX-License-Identifier: GPL-3.0-or-later
// OpenEXR (core/LibraryDecoders.h): the RGBA interface (luminance/chroma files are
// converted by the library), half floats copied as they are into Format_RGBA16FPx4; scene
// linear with the file's chromaticities (default Rec. 709 = linear sRGB). Float images keep
// their extended range like PFM (CodecUtil.h finish()).

#include "CodecUtil.h"
#include "core/LibraryDecoders.h"

#include <QFile>

#include <ImfArray.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfHeader.h>
#include <ImfRgbaFile.h>
#include <ImfStandardAttributes.h>
#include <ImfVersion.h>
#include <OpenEXRConfig.h>

#include <cstring>
#include <exception>

namespace lf {

namespace {

constexpr qint64 kMaxPixels = qint64(1) << 28;

QColorSpace colorSpaceOf(const Imf::Header& h) {
    if (!Imf::hasChromaticities(h))
        return QColorSpace(QColorSpace::SRgbLinear);
    const Imf::Chromaticities& c = Imf::chromaticities(h);
    QColorSpace cs(QPointF(c.white.x, c.white.y), QPointF(c.red.x, c.red.y), QPointF(c.green.x, c.green.y),
                   QPointF(c.blue.x, c.blue.y), QColorSpace::TransferFunction::Linear);
    return cs.isValid() ? cs : QColorSpace(QColorSpace::SRgbLinear);
}

class OpenExrDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("OpenEXR"); }
    int priority() const override { return 60; }

    bool canDecode(const QString&, const QByteArray& header) const override {
        return header.size() >= 4 && Imf::isImfMagic(header.constData());
    }

    std::optional<ImageInfo> info(const QString& path, QString* error) override {
        try {
            Imf::RgbaInputFile file(QFile::encodeName(path).constData());
            const auto dw = file.dataWindow();
            ImageInfo i;
            i.pixelSize = i.displaySize = QSize(dw.max.x - dw.min.x + 1, dw.max.y - dw.min.y + 1);
            i.hasAlpha = (file.channels() & Imf::WRITE_A) != 0;
            i.bitsPerComponent = 16;
            i.isHdr = true;
            i.colorModel = (file.channels() & Imf::WRITE_C) || (file.channels() & Imf::WRITE_RGB)
                               ? QStringLiteral("RGB")
                               : QStringLiteral("Gray");
            i.formatName = QStringLiteral("OpenEXR");
            if (i.pixelSize.isEmpty()) {
                codec::setError(error, DecoderRegistry::messageDamaged());
                return std::nullopt;
            }
            return i;
        } catch (const std::exception&) {
            codec::setError(error, QFile::exists(path) ? DecoderRegistry::messageDamaged()
                                                       : DecoderRegistry::messageMissing());
            return std::nullopt;
        }
    }

    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override {
        if (req.page > 0 || req.frame > 0) {
            codec::setError(error, DecoderRegistry::messageUnsupported());
            return std::nullopt;
        }
        try {
            Imf::RgbaInputFile file(QFile::encodeName(path).constData());
            const auto dw = file.dataWindow();
            const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
            if (w <= 0 || h <= 0 || qint64(w) * h > kMaxPixels) {
                codec::setError(error, w <= 0 || h <= 0 ? DecoderRegistry::messageDamaged()
                                                        : DecoderRegistry::messageTooLarge());
                return std::nullopt;
            }
            Imf::Array2D<Imf::Rgba> pixels(h, w);
            file.setFrameBuffer(&pixels[0][0] - dw.min.x - qsizetype(dw.min.y) * w, 1, size_t(w));
            file.readPixels(dw.min.y, dw.max.y);
            // Imf::Rgba is four IEEE halves (r, g, b, a) = Format_RGBA16FPx4 (qfloat16).
            static_assert(sizeof(Imf::Rgba) == 8);
            QImage img(w, h, QImage::Format_RGBA16FPx4);
            if (img.isNull()) {
                codec::setError(error, DecoderRegistry::messageTooLarge());
                return std::nullopt;
            }
            for (int y = 0; y < h; ++y)
                std::memcpy(img.scanLine(y), &pixels[y][0], size_t(w) * sizeof(Imf::Rgba));
            img.setColorSpace(colorSpaceOf(file.header()));
            return codec::finish(std::move(img), QSize(w, h), req, error);
        } catch (const std::exception&) {
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
    }

    Metadata metadata(const QString& path) override {
        Metadata md;
        if (const auto i = info(path, nullptr))
            codec::addFileMetadata(md, *i);
        return md;
    }
};

} // namespace

QString openExrVersion() {
    return QStringLiteral("OpenEXR ") + QStringLiteral(OPENEXR_VERSION_STRING);
}

std::shared_ptr<Decoder> createOpenExrDecoder() {
    return std::make_shared<OpenExrDecoder>();
}

} // namespace lf
