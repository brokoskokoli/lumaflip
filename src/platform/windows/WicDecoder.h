// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decoder backend on the Windows Imaging Component (SPEC 8.3, priority 100 - the
// counterpart of ImageIO on macOS).
//
// Formats: what the installed WIC codecs read - built in: JPEG, PNG, TIFF, BMP, ICO, JPEG XR,
// DDS (and GIF, which is left to Qt for the animation); with the codec extensions from the
// Microsoft Store: HEIF/HEIC, AVIF (AV1), WebP, JPEG XL, camera RAW. The codec list is read
// once (IWICImagingFactory::CreateComponentEnumerator); canDecode() matches the codecs'
// signature patterns against the file header without COM, the extension only for codecs
// without patterns (RAW).
//
// Output pixel formats as with ImageIO (ImageIODecoder.h):
//   - 8-bit sources            -> QImage::Format_RGBA8888_Premultiplied
//   - > 8-bit integer sources  -> QImage::Format_RGBA64_Premultiplied
//   - float sources with req.hdr / req.hdrFloat (JPEG XR, DDS, half/float TIFF)
//                              -> QImage::Format_RGBA16FPx4_Premultiplied, linear sRGB
//                                 primaries (scRGB), headroom = brightest component
// Color (F-COL-01/06): RGB images keep their pixels and get their embedded ICC profile (or
// the EXIF color space: sRGB, Adobe RGB) as QColorSpace; untagged = sRGB. CMYK images and
// RGB profiles Qt cannot represent are converted to sRGB by WIC color management
// (IWICColorTransform); gray images are expanded to neutral sRGB.
// Orientation: System.Photo.Orientation (EXIF/XMP, all containers), applied after decoding.
// Size: decode-time downscaling with IWICBitmapScaler (which uses the codec's own fast
// scaling, e.g. JPEG DCT scaling); full decodes are capped like ImageIO (kMaxFullEdge,
// kMaxFullPixels) and by DecodeRequest::maxBytes.
//
// Thread-safe: every call uses its own decoder objects; COM is entered per thread (MTA).

#include "core/Decoder.h"

namespace lf {

class WicDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("WIC"); }
    int priority() const override { return 100; }

    static constexpr int kMaxFullEdge = 32768;
    static constexpr qint64 kMaxFullPixels = 500'000'000;

    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req, QString* error) override;
    Metadata metadata(const QString& path) override;

    // Lower-case suffixes of all installed WIC decoders (GIF excluded, see above).
    static QStringList supportedExtensions();
    // Names of the installed decoders ("WIC JPEG Decoder", "HEIF Decoder" ...), for diagnostics.
    static QStringList installedCodecs();
};

} // namespace lf
