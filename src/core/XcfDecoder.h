// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GIMP XCF (SPEC 4.4 "Rest", M3): own decoder that composites the visible layers, since
// XCF files carry no merged image. Portable C++ on QtCore/QtGui (+ zlib), registered like
// the other own decoders and run in the decoder helper process (SPEC 8.5).
//
// Supported: XCF v0–v12 (64-bit offsets from v11), RGB/Gray/Indexed images, tiles stored
// uncompressed, RLE or zlib, precisions 8/16/32-bit integer and half/float/double, each
// linear or perceptual ("gamma"). Visible layers are composited bottom-up with opacity,
// offsets and applied layer masks; layer groups (pass-through and isolated) are honoured.
// Layer modes: normal, dissolve, behind, multiply, screen, overlay, difference, addition,
// subtract, darken/lighten only, divide, dodge, burn, hard/soft light, grain extract/merge,
// exclusion, linear burn, vivid/pin/linear light, hard mix - the legacy (GIMP ≤ 2.8,
// clipped to the backdrop) and the GIMP 2.10 variants (union). Blending happens in the
// perceptual space. Other modes (HSV/LCH, colour erase …), unknown versions and layer
// effects are rendered as "normal" / ignored and the result is marked `approximate` (logged
// to "lf.xcf"). Channels, paths, guides and parasites other than the ICC profile are ignored.
//
// Robustness (SPEC 8.1/8.5): every offset and size is bounds-checked, tile data must decode
// to exactly the expected byte count, at most kMaxLayers layers, kMaxEdge per edge; the
// working canvas is capped at kMaxCanvasPixels (larger images are composited subsampled).
// Damaged input yields an error, never a crash. decode() is the fuzzing entry point.
//
// Output: Format_RGBA8888_Premultiplied for 8-bit images, Format_RGBA64_Premultiplied for
// deeper ones, sRGB (or the embedded ICC profile of perceptual 8/16-bit images).

#include "Decoder.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace lf {

namespace xcf {

constexpr int kMaxEdge = 1 << 18;                     // GIMP's own limit is 524288
constexpr qint64 kMaxCanvasPixels = qint64(40) << 20; // float working canvas (640 MB)
constexpr int kMaxLayers = 4096;
constexpr int kMaxGroupDepth = 32;

struct Probe {
    QSize size;
    int version = 0;
    int bitsPerComponent = 8;
    bool isFloat = false;
    QString colorModel = QStringLiteral("RGB"); // "RGB", "Gray", "Indexed"
};

struct Result {
    QImage image;   // composited, possibly subsampled (see fullSize)
    QSize fullSize; // canvas size of the file
    int version = 0;
    int bitsPerComponent = 8;
    QString colorModel = QStringLiteral("RGB");
    int layersComposited = 0;
    // Something was not rendered exactly (unsupported mode, unknown version, effects …);
    // `notes` says what (English, for logs).
    bool approximate = false;
    QStringList notes;
};

bool matchesSignature(const QByteArray& header);
// Header only (the first 64 bytes suffice).
std::optional<Probe> probe(const QByteArray& data, QString* error);
// `maxEdge` > 0: the longer edge of the result is at most about maxEdge (composited
// subsampled, then scaled smoothly); 0 = full size (still capped at kMaxCanvasPixels).
std::optional<Result> decode(const QByteArray& data, int maxEdge, QString* error);

// Layer modes (GimpLayerMode values stored in PROP_MODE) the compositor knows.
bool isSupportedMode(quint32 mode);

} // namespace xcf

// Priority 20 like the other own decoders (ImageIO cannot read XCF).
class XcfDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("Lumaflip XCF"); }
    int priority() const override { return 20; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;
};

} // namespace lf
