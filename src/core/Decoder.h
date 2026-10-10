// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Decoder abstraction (SPEC 8.3). All methods are called from worker threads and
// must be thread-safe (backends must not share mutable state without locking).

#include <QByteArray>
#include <QColorSpace>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <optional>

namespace lf {

struct ImageInfo {
    QSize pixelSize;         // as stored, before orientation
    QSize displaySize;       // after EXIF orientation
    int exifOrientation = 1; // 1..8
    int frameCount = 1;      // animation frames
    int pageCount = 1;       // multi-page (TIFF, ICO, HEIF collections)
    bool hasAlpha = false;
    bool isHdr = false; // gain map, PQ/HLG, float formats
    bool isAnimated = false;
    int bitsPerComponent = 8;
    QString colorModel;  // "RGB", "CMYK", "Gray"
    QString profileName; // ICC description, empty if none
    QString formatName;  // "JPEG", "HEIC", "Canon CR3", ...
};

// Stage of the image pipeline (SPEC 7.1). Declared here (not in ImageProvider.h, which
// re-exports it) so DecodedImage can say which stage it was delivered as.
enum class Quality {
    Preview, // embedded preview / small thumbnail, fast (stage 1)
    Screen,  // downsampled to the target size (stage 2)
    Full,    // full resolution (stage 3, when zooming beyond fit)
};

struct DecodeRequest {
    int page = 0;
    int frame = 0;
    // Max size of the longer edge after orientation; 0 = full resolution.
    // Backends should use decode-time downsampling (e.g. ImageIO thumbnails).
    int maxPixelSize = 0;
    // P-08: upper bound of the decoded pixel buffer in bytes (image.sizeInBytes()), taking
    // the output format into account (16-bit/float = 8 bytes per pixel); larger images are
    // downsampled while decoding. 0 = no limit beyond the backend's own caps.
    qint64 maxBytes = 0;
    bool preferEmbeddedPreview = false; // fast path for RAW/JPEG (F-FMT-05)
    bool hdr = false;                   // decode to extended range float if possible (F-COL-03)
};

// Result of a decode. `image` is already oriented (EXIF applied) and carries its
// color space (QImage::colorSpace()). HDR results use QImage::Format_RGBA16FPx4
// (premultiplied variant) in extended linear Display P3 and set headroom > 1.
struct DecodedImage {
    QImage image;
    QSize fullSize; // oriented full-resolution size of the source
    // Decoder view: the image is smaller than the source (embedded preview or decode-time
    // downsampling - so also true for the Screen stage of a large file). Not a quality
    // gate: whether an image is good enough for a purpose is `quality` below.
    bool isPreview = false;
    float headroom = 1.0f;   // HDR peak relative to SDR white
    int frameDurationMs = 0; // for animations
    // F-FMT-03/04 (additive): what the file holds besides this image. Backends that do not
    // know leave the defaults (a single still image).
    int page = 0;       // page this image belongs to (DecodeRequest::page)
    int frame = 0;      // animation frame (DecodeRequest::frame)
    int pageCount = 1;  // pages of the file (multi-page TIFF, ICO, HEIF collections)
    int frameCount = 1; // animation frames of the file (> 1: animated)
    // Pipeline stage this image is good for. Set by ImageProvider (and the animation
    // player); decoders leave the default. Use it - not isPreview - to decide whether an
    // image is good enough (histogram, animation start): a downscaled Screen stage is
    // Quality::Screen, an embedded/small preview Quality::Preview, and any image that
    // holds the full resolution (also the preview stage of a small file) Quality::Full.
    Quality quality = Quality::Full;
};

// F-FMT-03: sequential access to the frames of an animation. Keeps the file open, so
// decoders that composite frames on top of the previous ones (GIF, APNG) do not start
// over for every frame. Not thread-safe: one reader is used by one thread at a time.
class FrameReader {
public:
    virtual ~FrameReader() = default;
    virtual int frameCount() const = 0;
    // Frame `index` (DecodeRequest::frame is ignored, maxPixelSize/maxBytes apply).
    virtual std::optional<DecodedImage> readFrame(int index, const DecodeRequest& req, QString* error) = 0;
};

struct Metadata {
    // Grouped key/value pairs for the info panel (F-INFO-02), e.g.
    // {"Kamera": {"Modell": "...", ...}, "Aufnahme": {...}, "GPS": {...}}
    QVariantMap groups;
    std::optional<QDateTime> captured;
    std::optional<double> latitude, longitude;
    int rating = -1; // xmp:Rating if present
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
    // F-FMT-03: an open reader for the frames of an animated file; nullptr = not supported
    // by this backend (the registry then falls back to decode() with DecodeRequest::frame).
    virtual std::unique_ptr<FrameReader> openFrames(const QString& path) {
        Q_UNUSED(path);
        return nullptr;
    }
};

// Picks backends in priority order and falls back on failure (SPEC 8.3).
class DecoderRegistry {
public:
    void add(std::shared_ptr<Decoder> decoder);
    // Backends able to decode the file, best first.
    QList<std::shared_ptr<Decoder>> candidates(const QString& path) const;
    QList<std::shared_ptr<Decoder>> candidates(const QString& path, const QByteArray& header) const;
    bool isSupported(const QString& path) const;
    // Fast check by extension only (used while enumerating folders).
    bool isSupportedExtension(const QString& suffixLower) const;
    void addSupportedExtensions(const QStringList& suffixesLower);
    // All registered extensions (lower case, sorted).
    QStringList supportedExtensions() const;

    // `error` is user facing (German, no backend names, F-FMT-02): one of the message*()
    // texts below. `detail` (optional) gets the backend errors for logs/diagnostics; they
    // are also logged to the "lf.decode" category.
    std::optional<ImageInfo> info(const QString& path, QString* error, QString* detail = nullptr) const;
    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req, QString* error,
                                       QString* detail = nullptr) const;

    static QString messageMissing();     // "Die Datei existiert nicht."
    static QString messageEmpty();       // "Die Datei ist leer."
    static QString messageUnreadable();  // "Die Datei kann nicht gelesen werden."
    static QString messageDamaged();     // "Die Datei ist beschädigt oder unvollständig."
    static QString messageUnsupported(); // "Das Dateiformat wird nicht unterstützt."
    static QString messageTooLarge();    // out of memory while decoding
    Metadata metadata(const QString& path) const;
    // F-FMT-03: frame reader of the best backend, or a generic one built on decode().
    // nullptr if no backend can decode the file.
    std::unique_ptr<FrameReader> openFrames(const QString& path) const;

    // Process-wide instance with platform + portable backends registered.
    static DecoderRegistry& instance();

private:
    QList<std::shared_ptr<Decoder>> m_decoders;
    QSet<QString> m_extensions;
};

} // namespace lf
