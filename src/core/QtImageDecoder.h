// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Portable decoder backends (SPEC 8.3):
//   - QtImageDecoder: QImageReader + installed Qt image plugins (priority 10), fallback
//     for everything the platform decoder cannot read.
//   - SvgDecoder: SVG/SVGZ via QtSvg (priority 90; ImageIO has no SVG support).
//
// Output pixel formats match the ImageIO backend: Format_RGBA8888_Premultiplied for
// 8-bit sources, Format_RGBA64_Premultiplied for deeper ones; images carry their
// QColorSpace (untagged = sRGB). Both classes are stateless and thread-safe.

#include "Decoder.h"

namespace lf {

class QtImageDecoder final : public Decoder {
  public:
    QString name() const override { return QStringLiteral("Qt"); }
    int priority() const override { return 10; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;

    static QStringList supportedExtensions();
};

class SvgDecoder final : public Decoder {
  public:
    // Longer edge used when the full resolution is requested (maxPixelSize == 0).
    static constexpr int kDefaultLongEdge = 2048;

    QString name() const override { return QStringLiteral("QtSvg"); }
    int priority() const override { return 90; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    // pixelSize/displaySize = intrinsic SVG size (width/height or viewBox).
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    // Renders at maxPixelSize (longer edge, may upscale) or kDefaultLongEdge. fullSize is
    // the size of the kDefaultLongEdge rendering, so isPreview is false for that one.
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;
};

// Registers the portable backends (Qt image plugins + SVG) and their suffixes.
// Called by the owner of the registry in addition to registerPlatformDecoders().
void registerPortableDecoders(DecoderRegistry& registry);

} // namespace lf
