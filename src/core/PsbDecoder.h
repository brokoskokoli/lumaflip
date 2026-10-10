// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Photoshop Large Document (PSB, "8BPS" version 2; SPEC 4.4 "Rest", M3): own decoder for
// the merged composite image ("image data" section). ImageIO handles PSD (version 1) and only
// some PSB files (as "PSD", in the app process). Portable C++ on QtCore/QtGui (+ zlib); runs in the decoder
// helper (SPEC 8.5).
//
// Supported: raw, RLE (PackBits with 4-byte row counts) and ZIP with/without prediction;
// 1/8/16/32 bits; RGB, Gray, Indexed, Bitmap, CMYK and Lab (Duotone/Multichannel as gray,
// marked approximate). The ICC profile from the image resources (0x040F) is attached; CMYK
// is converted with the embedded profile or the system's "Generic CMYK Profile" to Display
// P3 (F-COL-06), naively where neither exists. The merged colour data is matted with white
// where transparent; that is undone ("alpha unblend"). 32-bit documents are linear.
//
// Robustness: bounds-checked like the other own parsers (SPEC 8.1/8.5): sizes are checked
// before allocation (edges ≤ 300000, output ≤ kMaxPixels, subsampled for smaller targets),
// RLE rows must decode to exactly their length, ZIP streams to the expected size. decode()
// is the fuzzing entry point.

#include "Decoder.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

namespace lf {

namespace psb {

constexpr int kMaxEdge = 300000;                 // Photoshop's limit for PSB
constexpr qint64 kMaxPixels = qint64(1) << 28;   // decoded output (268 MP)
constexpr qint64 kMaxUnzipped = qint64(2) << 30; // ZIP-compressed image data

struct Probe {
    QSize size;
    int version = 2;
    int channels = 0;
    int bitsPerComponent = 8;
    int colorMode = 3;                          // Photoshop colour mode (0 bitmap … 9 Lab)
    QString colorModel = QStringLiteral("RGB"); // "RGB", "CMYK", "Gray"
};

struct Result {
    QImage image; // RGBA8888/RGBA64 premultiplied (RGBA32FPx4 for 32 bit), tagged
    QSize fullSize;
    int bitsPerComponent = 8;
    QString colorModel = QStringLiteral("RGB");
    bool hasAlpha = false;
    bool approximate = false;
    QStringList notes;
};

// "8BPS" with version 2 (`acceptPsd`: also version 1, for tests and fallbacks).
bool matchesSignature(const QByteArray& header, bool acceptPsd = false);
std::optional<Probe> probe(const QByteArray& data, QString* error);
// `maxEdge` > 0: subsampled while decoding to about that longer edge, then scaled.
std::optional<Result> decode(const QByteArray& data, int maxEdge, QString* error);

} // namespace psb

// Priority 101: above ImageIO (100), which recognises PSB as PSD and decodes some of them
// in the app process; large documents belong in the helper (SPEC 8.5), with ImageIO as the
// fallback when this decoder fails.
class PsbDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("Lumaflip PSB"); }
    int priority() const override { return 101; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;
};

} // namespace lf
