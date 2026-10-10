// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Shared steps of the library decoder backends (src/codecs, core/LibraryDecoders.h): the
// output conventions of the other backends (QtImageDecoder.h, SimpleDecoderBackend.cpp):
// Format_RGBA8888_Premultiplied for 8-bit sources, Format_RGBA64_Premultiplied for deeper
// ones, float sources as extended linear Display P3 (Format_RGBA16FPx4_Premultiplied, headroom
// = peak) when the request allows it; downsampling to the request's target edge.

#include "core/Decoder.h"
#include "core/OwnDecoderUtil.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QImage>

#include <algorithm>
#include <cmath>

namespace lf::codec {

inline void setError(QString* error, const QString& message) {
    if (error && error->isEmpty())
        *error = message;
}

inline QString suffixOf(const QString& path) {
    return QFileInfo(path).suffix().toLower();
}

// Largest finite RGB value of a Format_RGBA32FPx4 image.
inline float peakOf(const QImage& img) {
    float peak = 0.f;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const float*>(img.constScanLine(y));
        for (int x = 0; x < img.width() * 4; x += 4)
            for (int c = 0; c < 3; ++c)
                if (std::isfinite(p[x + c]))
                    peak = std::max(peak, p[x + c]);
    }
    return peak;
}

// Turns a decoded image (any format, with its color space; untagged = sRGB) into the result
// of a request. `full` is the oriented full-resolution size of the source.
inline std::optional<DecodedImage> finish(QImage img, QSize full, const DecodeRequest& req, QString* error,
                                          bool isPreview = false) {
    if (img.isNull()) {
        setError(error, DecoderRegistry::messageTooLarge());
        return std::nullopt;
    }
    if (!img.colorSpace().isValid())
        img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    DecodedImage out;
    const QImage::Format f = img.format();
    const bool isFloat = f == QImage::Format_RGBA32FPx4 || f == QImage::Format_RGBA32FPx4_Premultiplied ||
                         f == QImage::Format_RGBA16FPx4 || f == QImage::Format_RGBA16FPx4_Premultiplied ||
                         f == QImage::Format_RGBX32FPx4 || f == QImage::Format_RGBX16FPx4;
    if (isFloat) {
        img.convertTo(QImage::Format_RGBA32FPx4);
        const float peak = peakOf(img);
        if ((req.hdr || req.hdrFloat) && peak > 1.0f) {
            // F-COL-03: extended range like the other backends (linear Display P3).
            img.convertToColorSpace(
                QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear));
            img.convertTo(QImage::Format_RGBA16FPx4_Premultiplied);
            out.headroom = peak;
        } else {
            img.convertTo(QImage::Format_RGBA64_Premultiplied); // clipped to 0..1 (SDR)
        }
    } else if (img.depth() > 32) {
        img.convertTo(QImage::Format_RGBA64_Premultiplied);
    } else {
        img.convertTo(QImage::Format_RGBA8888_Premultiplied);
    }
    if (img.isNull()) {
        setError(error, DecoderRegistry::messageTooLarge());
        return std::nullopt;
    }
    const int bpp = img.depth() / 8;
    if (const int edge = decutil::targetEdge(img.size(), bpp, req); edge > 0) {
        const QColorSpace cs = img.colorSpace();
        img = img.scaled(decutil::fitLongEdge(img.size(), edge), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
        img.setColorSpace(cs);
    }
    out.fullSize = full.isValid() ? full : img.size();
    out.isPreview = isPreview || img.width() < out.fullSize.width() || img.height() < out.fullSize.height();
    out.image = std::move(img);
    return out;
}

// Applies an EXIF orientation (1..8) to `img`.
inline QImage oriented(const QImage& img, int orientation) {
    QTransform t;
    switch (orientation) {
    case 2:
        return img.flipped(Qt::Horizontal);
    case 3:
        t.rotate(180);
        break;
    case 4:
        return img.flipped(Qt::Vertical);
    case 5:
        t.rotate(90);
        return img.transformed(t).flipped(Qt::Horizontal);
    case 6:
        t.rotate(90);
        break;
    case 7:
        t.rotate(-90);
        return img.transformed(t).flipped(Qt::Horizontal);
    case 8:
        t.rotate(-90);
        break;
    default:
        return img;
    }
    return img.transformed(t);
}

inline QSize orientedSize(QSize s, int orientation) {
    return orientation >= 5 && orientation <= 8 ? s.transposed() : s;
}

// Basic metadata rows from an ImageInfo (all backends add the same "file.*" rows).
inline void addFileMetadata(Metadata& md, const ImageInfo& i) {
    addMetadata(md, QStringLiteral("file.format"), i.formatName);
    addMetadata(md, QStringLiteral("file.dimensions"),
                QStringLiteral("%1 × %2").arg(i.displaySize.width()).arg(i.displaySize.height()));
    if (!i.colorModel.isEmpty())
        addMetadata(md, QStringLiteral("file.colorModel"), i.colorModel);
    if (!i.profileName.isEmpty())
        addMetadata(md, QStringLiteral("file.profile"), i.profileName);
    addMetadata(md, QStringLiteral("file.bitDepth"), i.bitsPerComponent);
    if (i.pageCount > 1)
        addMetadata(md, QStringLiteral("file.pages"), i.pageCount);
}

} // namespace lf::codec
