// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-OPEN-10 (M4): PDF pages as images. macOS renders with CoreGraphics (CGPDFDocument into
// an offscreen bitmap, no GPU, no window server - runs in the decoder helper's sandbox);
// other platforms have no renderer yet (available() = false).
//
// A page has no pixel size of its own: its "full resolution" is the crop box at kFullDpi
// (at most kMaxSide pixels per edge). Thread-safe (every call opens the document).

#include "Decoder.h"

#include <QSizeF>
#include <QString>

#include <optional>

namespace lf::pdf {

inline constexpr double kFullDpi = 300.0;
inline constexpr int kMaxSide = 12000;

bool available();
// Number of pages, -1 + error if the file is no readable PDF (encrypted ones included).
int pageCount(const QString& path, QString* error);
// Size of page `index` (0-based) in points after its /Rotate, invalid on error.
QSizeF pageSize(const QString& path, int index, QString* error);
// Pixel size of the full resolution rendering for a page of `points` (kFullDpi, kMaxSide).
QSize fullPixelSize(const QSizeF& points);
// Pixel size for a request: the full size scaled to req.maxPixelSize (longer edge, never
// above the full size) and to req.maxBytes (4 bytes per pixel).
QSize targetPixelSize(const QSizeF& points, const DecodeRequest& req);
// Renders page `index` on white (sRGB, ARGB32 premultiplied) at targetPixelSize().
std::optional<DecodedImage> renderPage(const QString& path, int index, const DecodeRequest& req,
                                       QString* error);

// The decoder backend for PDF page member paths (vpath, "Plan.pdf#/0003"). Runs in the
// decoder helper when there is one (DecoderHelper.h isolatedDecoder()).
class PdfPageDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("PDF"); }
    int priority() const override { return 100; }
    bool handlesVirtualPaths() const override { return true; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;
};

} // namespace lf::pdf
