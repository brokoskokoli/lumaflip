// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decoder backend on Apple ImageIO (SPEC 8.3, priority 100).
//
// Output pixel formats (chosen so the renderer can upload without conversion;
// all are RGBA byte/component order, premultiplied alpha, opaque images have A = max):
//   - 8-bit sources            -> QImage::Format_RGBA8888_Premultiplied  (RGBA8 texture)
//   - >8-bit / float SDR       -> QImage::Format_RGBA64_Premultiplied    (RGBA16 texture)
//   - HDR (req.hdr && isHdr)   -> QImage::Format_RGBA16FPx4_Premultiplied (RGBA16F texture)
//                                 in extended linear Display P3, DecodedImage::headroom > 1
//
// Color (F-COL-01, F-COL-06): RGB images keep their source color space (pixels are not
// converted; the ICC profile is attached as QColorSpace). Indexed images keep the RGB
// base space of their palette. Untagged images are treated as sRGB. CMYK, Lab, other
// models and RGB profiles Qt cannot represent are converted to Display P3 by ColorSync.
// Gray images are expanded to neutral sRGB.
//
// Size cap (memory safety): a full-resolution request (maxPixelSize == 0) for an image whose
// longer edge exceeds kMaxFullEdge or whose pixel count exceeds kMaxFullPixels is decoded
// downsampled to fit both limits (ImageIO thumbnail API). The result has the correct
// DecodedImage::fullSize but isPreview == false: it is the best this backend delivers.
// Full decodes are drawn once into the output bitmap without asking ImageIO to keep its
// own cached copy (kCGImageSourceShouldCache = NO). ImageIO still needs one temporary
// decode buffer during the draw, so the peak is ~2x the output and 1x afterwards.
//
// All methods are thread-safe (no shared mutable state) and wrap their work in an
// autorelease pool.

#include "core/Decoder.h"

namespace lf {

class ImageIODecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("ImageIO"); }
    int priority() const override { return 100; }

    static constexpr int kMaxFullEdge = 32768;
    static constexpr qint64 kMaxFullPixels = 500'000'000;
    // Signature based (CGImageSourceGetType on the header); the suffix is only used
    // for formats without a reliable magic number (RAW, TGA).
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;
    // F-FMT-03: keeps the CGImageSource open for all frames.
    std::unique_ptr<FrameReader> openFrames(const QString& path) override;

    // Lower-case file suffixes of all types ImageIO can read (for the registry).
    static QStringList supportedExtensions();
    // Human readable format name for a UTType identifier, e.g. "Canon CR3".
    static QString formatNameForType(const QString& typeIdentifier);
};

} // namespace lf
