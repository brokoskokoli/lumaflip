// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decoder abstraction (SPEC 8.3). All methods are called from worker threads and
// must be thread-safe (backends must not share mutable state without locking).

#include <QByteArray>
#include <QColorSpace>
#include <QDateTime>
#include <QSet>
#include <QStringList>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <memory>
#include <optional>

namespace lf {

struct ImageInfo {
    QSize pixelSize;            // as stored, before orientation
    QSize displaySize;          // after EXIF orientation
    int exifOrientation = 1;    // 1..8
    int frameCount = 1;         // animation frames
    int pageCount = 1;          // multi-page (TIFF, ICO, HEIF collections)
    bool hasAlpha = false;
    bool isHdr = false;         // gain map, PQ/HLG, float formats
    bool isAnimated = false;
    int bitsPerComponent = 8;
    QString colorModel;         // "RGB", "CMYK", "Gray"
    QString profileName;        // ICC description, empty if none
    QString formatName;         // "JPEG", "HEIC", "Canon CR3", ...
};

struct DecodeRequest {
    int page = 0;
    int frame = 0;
    // Max size of the longer edge after orientation; 0 = full resolution.
    // Backends should use decode-time downsampling (e.g. ImageIO thumbnails).
    int maxPixelSize = 0;
    bool preferEmbeddedPreview = false;  // fast path for RAW/JPEG (F-FMT-05)
    bool hdr = false;                    // decode to extended range float if possible (F-COL-03)
};

// Result of a decode. `image` is already oriented (EXIF applied) and carries its
// color space (QImage::colorSpace()). HDR results use QImage::Format_RGBA16FPx4
// (premultiplied variant) in extended linear Display P3 and set headroom > 1.
struct DecodedImage {
    QImage image;
    QSize fullSize;           // oriented full-resolution size of the source
    bool isPreview = false;   // true if this is a reduced/embedded preview
    float headroom = 1.0f;    // HDR peak relative to SDR white
    int frameDurationMs = 0;  // for animations
};

struct Metadata {
    // Grouped key/value pairs for the info panel (F-INFO-02), e.g.
    // {"Kamera": {"Modell": "...", ...}, "Aufnahme": {...}, "GPS": {...}}
    QVariantMap groups;
    std::optional<QDateTime> captured;
    std::optional<double> latitude, longitude;
    int rating = -1;  // xmp:Rating if present
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual QString name() const = 0;
    // Higher wins. ImageIO = 100, resvg = 90, Qt image plugins = 10.
    virtual int priority() const = 0;
    // `header` holds at least the first 64 bytes of the file.
    virtual bool canDecode(const QString& path, const QByteArray& header) const = 0;
    virtual std::optional<ImageInfo> info(const QString& path, QString* error) = 0;
    virtual std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                               QString* error) = 0;
    virtual Metadata metadata(const QString& path) = 0;
};

// Picks backends in priority order and falls back on failure (SPEC 8.3).
class DecoderRegistry {
public:
    void add(std::shared_ptr<Decoder> decoder);
    // Backends able to decode the file, best first.
    QList<std::shared_ptr<Decoder>> candidates(const QString& path) const;
    bool isSupported(const QString& path) const;
    // Fast check by extension only (used while enumerating folders).
    bool isSupportedExtension(const QString& suffixLower) const;
    void addSupportedExtensions(const QStringList& suffixesLower);
    // All registered extensions (lower case, sorted).
    QStringList supportedExtensions() const;

    std::optional<ImageInfo> info(const QString& path, QString* error) const;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) const;
    Metadata metadata(const QString& path) const;

    // Process-wide instance with platform + portable backends registered.
    static DecoderRegistry& instance();

private:
    QList<std::shared_ptr<Decoder>> m_decoders;
    QSet<QString> m_extensions;
};

} // namespace lf
