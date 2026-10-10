// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SVG/SVGZ via resvg (SPEC 4.4 "Vektor", M3): full SVG 1.1 with CSS, filters, masks and
// text, through resvg's C API (Homebrew `libresvg`, Apache-2.0/MIT, dynamically linked,
// SPEC 11). Priority 95: above QtSvg (90), which stays the fallback when resvg fails or is
// not built in (CMake LF_ENABLE_RESVG, LF_HAVE_RESVG). Runs in the decoder helper process
// like the other library decoders (SPEC 8.5, registered in PortableDecoders.cpp).
//
// - info(): the intrinsic size (width/height, else the viewBox), like SvgDecoder.
// - decode(): renders at maxPixelSize (longer edge, may upscale) or kDefaultLongEdge,
//   bounded by DecodeRequest::maxBytes and kMaxEdge; sRGB, Format_RGBA8888_Premultiplied.
// - Fonts: resvg's font database is filled with the system fonts once per process, and only
//   when a document contains text (loading them takes a moment); documents without text use
//   an options object without fonts. Font files are read-only accesses, which the helper's
//   sandbox allows.
//
// Thread-safe: parsing is serialised (shared options), rendering runs in parallel.

#include "Decoder.h"

#include <memory>

namespace lf {

class ResvgDecoder final : public Decoder {
public:
    static constexpr int kDefaultLongEdge = 2048; // same as SvgDecoder
    static constexpr int kMaxEdge = 16384;

    QString name() const override { return QStringLiteral("resvg"); }
    int priority() const override { return 95; }
    bool canDecode(const QString& path, const QByteArray& header) const override;
    std::optional<ImageInfo> info(const QString& path, QString* error) override;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override;
    Metadata metadata(const QString& path) override;

    // Pure entry points on a buffer (tests, fuzzing). `baseDir` resolves relative image
    // references (may be empty). `size` is the output size in pixels.
    static std::optional<QSize> intrinsicSize(const QByteArray& data, QString* error);
    static QImage render(const QByteArray& data, const QString& baseDir, QSize size, QString* error);
    // Whether the system fonts have been loaded in this process (tests).
    static bool systemFontsLoaded();
};

// True when the build links resvg (LF_HAVE_RESVG).
bool resvgAvailable();
// A ResvgDecoder, or nullptr without resvg.
std::shared_ptr<Decoder> createResvgDecoder();

} // namespace lf
