// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/mac/ImageIODecoder.h"

#include "core/HdrMapping.h"
#include "core/ImageFormats.h"
#include "core/LocaleFormat.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QTimeZone>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <cmath>

namespace lf {
namespace {

// ---- small helpers -------------------------------------------------------------------

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

// RAII owner for CoreFoundation references (ARC does not manage CF types).
template <typename T> class CFRef {
public:
    CFRef() = default;
    explicit CFRef(T ref) : m_ref(ref) {}
    ~CFRef() { reset(); }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    CFRef(CFRef&& o) noexcept : m_ref(o.m_ref) { o.m_ref = nullptr; }
    CFRef& operator=(CFRef&& o) noexcept {
        if (this != &o) {
            reset();
            m_ref = o.m_ref;
            o.m_ref = nullptr;
        }
        return *this;
    }
    void reset(T ref = nullptr) {
        if (m_ref)
            CFRelease(m_ref);
        m_ref = ref;
    }
    T get() const { return m_ref; }
    explicit operator bool() const { return m_ref != nullptr; }

private:
    T m_ref = nullptr;
};

QString toQString(id value) {
    if ([value isKindOfClass:[NSString class]])
        return QString::fromNSString(static_cast<NSString*>(value));
    if ([value isKindOfClass:[NSNumber class]])
        return QString::fromNSString([static_cast<NSNumber*>(value) stringValue]);
    if ([value isKindOfClass:[NSArray class]]) {
        NSArray* arr = static_cast<NSArray*>(value);
        return arr.count > 0 ? toQString(arr[0]) : QString();
    }
    return {};
}

std::optional<double> toDouble(id value) {
    if ([value isKindOfClass:[NSNumber class]])
        return [static_cast<NSNumber*>(value) doubleValue];
    if ([value isKindOfClass:[NSString class]]) {
        bool ok = false;
        const double d = QString::fromNSString(static_cast<NSString*>(value)).toDouble(&ok);
        if (ok)
            return d;
    }
    if ([value isKindOfClass:[NSArray class]] && [static_cast<NSArray*>(value) count] > 0)
        return toDouble(static_cast<NSArray*>(value)[0]);
    return std::nullopt;
}

NSDictionary* dictValue(NSDictionary* dict, CFStringRef key) {
    id v = dict[(__bridge NSString*)key];
    return [v isKindOfClass:[NSDictionary class]] ? static_cast<NSDictionary*>(v) : nil;
}

id value(NSDictionary* dict, CFStringRef key) {
    return dict ? dict[(__bridge NSString*)key] : nil;
}

CFRef<CGImageSourceRef> openSource(const QString& path) {
    NSURL* url = [NSURL fileURLWithPath:path.toNSString()];
    NSDictionary* opts = @{(__bridge NSString*)kCGImageSourceShouldCache : @NO};
    return CFRef<CGImageSourceRef>(
        CGImageSourceCreateWithURL((__bridge CFURLRef)url, (__bridge CFDictionaryRef)opts));
}

const QSet<QString>& readableTypes() {
    static const QSet<QString> types = [] {
        QSet<QString> s;
        @autoreleasepool {
            NSArray* ids = CFBridgingRelease(CGImageSourceCopyTypeIdentifiers());
            for (NSString* t in ids)
                s.insert(QString::fromNSString(t));
        }
        return s;
    }();
    return types;
}

bool isAnimationType(const QString& t) {
    return t == u"com.compuserve.gif" || t == u"public.png" || t == u"org.webmproject.webp" ||
           t == u"public.heics" || t == u"public.avis" || t == u"public.jpeg-xl";
}

// Layout of the images inside a source: animation frames or pages.
struct Layout {
    QString type;
    size_t count = 0;
    size_t primary = 0;
    bool animated = false;
};

Layout layoutOf(CGImageSourceRef src) {
    Layout l;
    l.type = QString::fromNSString((__bridge NSString*)CGImageSourceGetType(src));
    l.count = CGImageSourceGetCount(src);
    l.primary = l.count > 0 ? std::min(CGImageSourceGetPrimaryImageIndex(src), l.count - 1) : 0;
    l.animated = l.count > 1 && isAnimationType(l.type);
    return l;
}

// Page p -> image index: page 0 is the primary image, the others follow in file order.
std::optional<size_t> imageIndex(const Layout& l, const DecodeRequest& req) {
    if (l.animated) {
        if (req.frame < 0 || size_t(req.frame) >= l.count)
            return std::nullopt;
        return size_t(req.frame);
    }
    if (req.page < 0 || size_t(req.page) >= l.count)
        return std::nullopt;
    const size_t p = size_t(req.page);
    if (p == 0)
        return l.primary;
    return p <= l.primary ? p - 1 : p;
}

// Offset of the first entropy-coded byte (just after the first SOS header), or -1 if the
// marker structure cannot be followed. Walking the segments by their lengths skips APPn
// payloads such as the EXIF thumbnail, which is a complete JPEG with its own EOI.
qint64 jpegEntropyStart(QFile& f, qint64 size) {
    uchar m[4];
    if (!f.seek(0) || f.read(reinterpret_cast<char*>(m), 2) != 2 || m[0] != 0xFF || m[1] != 0xD8)
        return -1;
    qint64 pos = 2;
    for (int guard = 0; guard < 4096 && pos + 4 <= size; ++guard) {
        if (!f.seek(pos) || f.read(reinterpret_cast<char*>(m), 4) != 4)
            return -1;
        if (m[0] != 0xFF)
            return -1;
        if (m[1] == 0xFF) { // fill byte
            ++pos;
            continue;
        }
        const int marker = m[1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2; // standalone markers
            continue;
        }
        if (marker == 0xD9)
            return -1; // EOI before any scan
        const qint64 len = (qint64(m[2]) << 8) | m[3];
        if (len < 2)
            return -1;
        if (marker == 0xDA) // SOS: entropy data follows its header
            return pos + 2 + len <= size ? pos + 2 + len : -1;
        pos += 2 + len;
    }
    return -1;
}

// ImageIO happily decodes a cut-off JPEG and fills the rest with gray, without any
// status flag. A complete JPEG ends with an EOI marker (FF D9); entropy-coded data
// never contains FF D9 (FF is byte-stuffed), so its absence after the start of the
// scan data means the file was truncated. Only the part after the first SOS is searched:
// an EXIF thumbnail in APP1 carries an EOI of its own. Some cameras append trailers,
// hence the generous window for large files.
bool jpegLooksTruncated(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const qint64 size = f.size();
    const qint64 entropy = jpegEntropyStart(f, size);
    if (entropy < 0)
        return false; // unusual structure: let ImageIO decide
    const qint64 from = std::max(entropy, size - 512 * 1024);
    if (!f.seek(from))
        return false;
    const QByteArray tail = f.read(size - from);
    return !tail.contains(QByteArrayLiteral("\xFF\xD9"));
}

// Long edge for a full-resolution decode of a w x h image, honouring the caps: the fixed
// edge/pixel limits and `maxPixels` (from DecodeRequest::maxBytes, 0 = none).
int cappedLongEdge(int w, int h, qint64 maxPixels = 0) {
    const qint64 edge = std::max(w, h);
    qint64 cap = std::min<qint64>(edge, ImageIODecoder::kMaxFullEdge);
    qint64 pixelCap = ImageIODecoder::kMaxFullPixels;
    if (maxPixels > 0)
        pixelCap = std::min(pixelCap, maxPixels);
    const double pixels = double(w) * double(h);
    if (pixels > double(pixelCap)) {
        const double scale = std::sqrt(double(pixelCap) / pixels);
        cap = std::min<qint64>(cap, qint64(std::floor(double(edge) * scale)));
        // The short edge is rounded up by the scaler: stay below the cap with it.
        const double shortEdge = std::min(w, h);
        while (cap > 1 && double(cap) * std::ceil(double(cap) * shortEdge / double(edge)) > double(pixelCap))
            --cap;
    }
    return int(std::max<qint64>(1, cap));
}

bool isHdrImage(CGImageSourceRef src, size_t index, NSDictionary* props) {
    if ([value(props, kCGImagePropertyIsFloat) boolValue])
        return true;
    if (CFRef<CFDictionaryRef> gm(
            CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, index, kCGImageAuxiliaryDataTypeISOGainMap));
        gm)
        return true;
    if (CFRef<CFDictionaryRef> gm(
            CGImageSourceCopyAuxiliaryDataInfoAtIndex(src, index, kCGImageAuxiliaryDataTypeHDRGainMap));
        gm)
        return true;
    // PQ/HLG transfer: only deep images can carry it; creating the CGImage is lazy (no decode).
    const int depth = [value(props, kCGImagePropertyDepth) intValue];
    if (depth > 8) {
        CFRef<CGImageRef> img(CGImageSourceCreateImageAtIndex(src, index, nullptr));
        if (img) {
            CGColorSpaceRef cs = CGImageGetColorSpace(img.get());
            if (cs && (CGColorSpaceUsesITUR_2100TF(cs) || CGColorSpaceUsesExtendedRange(cs)))
                return true;
        }
    }
    return false;
}

QString colorModelName(NSDictionary* props) {
    const QString m = toQString(value(props, kCGImagePropertyColorModel));
    return m;
}

// Fills everything except frame/page counts.
ImageInfo infoFromProperties(CGImageSourceRef src, const Layout& l, size_t index, NSDictionary* props) {
    ImageInfo info;
    const int w = [value(props, kCGImagePropertyPixelWidth) intValue];
    const int h = [value(props, kCGImagePropertyPixelHeight) intValue];
    int o = [value(props, kCGImagePropertyOrientation) intValue];
    if (o < 1 || o > 8)
        o = 1;
    info.pixelSize = QSize(w, h);
    info.exifOrientation = o;
    info.displaySize = o >= 5 ? QSize(h, w) : QSize(w, h);
    info.hasAlpha = [value(props, kCGImagePropertyHasAlpha) boolValue];
    const int depth = [value(props, kCGImagePropertyDepth) intValue];
    info.bitsPerComponent = depth > 0 ? depth : 8;
    info.colorModel = colorModelName(props);
    info.profileName = toQString(value(props, kCGImagePropertyProfileName));
    info.isHdr = isHdrImage(src, index, props);
    info.formatName = ImageIODecoder::formatNameForType(l.type);
    if (l.animated && l.type == u"public.png")
        info.formatName = QStringLiteral("APNG");
    if (l.animated) {
        info.isAnimated = true;
        info.frameCount = int(l.count);
    } else {
        info.pageCount = int(std::max<size_t>(1, l.count));
    }
    return info;
}

int frameDurationMs(NSDictionary* props) {
    struct Keys {
        CFStringRef dict, unclamped, clamped;
    };
    const Keys keys[] = {
        {kCGImagePropertyGIFDictionary, kCGImagePropertyGIFUnclampedDelayTime, kCGImagePropertyGIFDelayTime},
        {kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGUnclampedDelayTime,
         kCGImagePropertyAPNGDelayTime},
        {kCGImagePropertyWebPDictionary, kCGImagePropertyWebPUnclampedDelayTime,
         kCGImagePropertyWebPDelayTime},
        {kCGImagePropertyHEICSDictionary, kCGImagePropertyHEICSUnclampedDelayTime,
         kCGImagePropertyHEICSDelayTime},
    };
    for (const auto& k : keys) {
        NSDictionary* d = dictValue(props, k.dict);
        if (!d)
            continue;
        auto s = toDouble(value(d, k.unclamped));
        if (!s || *s <= 0)
            s = toDouble(value(d, k.clamped));
        if (!s)
            continue;
        const int ms = int(std::lround(*s * 1000.0));
        // Browser convention: delays <= 10 ms are played as 100 ms.
        return ms <= 10 ? 100 : ms;
    }
    return 0;
}

// Transform that draws a stored image (w x h) upright into the display-oriented
// context for EXIF orientation o. CG user space has its origin bottom-left.
CGAffineTransform orientationTransform(int o, CGFloat w, CGFloat h) {
    switch (o) {
    case 2:
        return CGAffineTransformMake(-1, 0, 0, 1, w, 0);
    case 3:
        return CGAffineTransformMake(-1, 0, 0, -1, w, h);
    case 4:
        return CGAffineTransformMake(1, 0, 0, -1, 0, h);
    case 5:
        return CGAffineTransformMake(0, -1, -1, 0, h, w);
    case 6:
        return CGAffineTransformMake(0, -1, 1, 0, 0, w);
    case 7:
        return CGAffineTransformMake(0, 1, 1, 0, 0, 0);
    case 8:
        return CGAffineTransformMake(0, 1, -1, 0, h, 0);
    default:
        return CGAffineTransformIdentity;
    }
}

struct Target {
    CFRef<CGColorSpaceRef> space;
    QColorSpace qspace;
    CFRef<CGImageRef> replacement; // image re-tagged (untagged -> sRGB)
};

CFRef<CGColorSpaceRef> namedSpace(CFStringRef name) {
    return CFRef<CGColorSpaceRef>(CGColorSpaceCreateWithName(name));
}

bool sameName(CGColorSpaceRef cs, CFStringRef name) {
    CFRef<CFStringRef> n(CGColorSpaceCopyName(cs));
    return n && CFStringCompare(n.get(), name, 0) == kCFCompareEqualTo;
}

// Picks the color space of the output bitmap for an SDR decode (see header comment).
Target sdrTarget(CGImageRef img) {
    Target t;
    CGColorSpaceRef cs = CGImageGetColorSpace(img);
    CGColorSpaceRef rgb = cs;
    CGColorSpaceModel model = cs ? CGColorSpaceGetModel(cs) : kCGColorSpaceModelUnknown;
    if (model == kCGColorSpaceModelIndexed) {
        rgb = CGColorSpaceGetBaseColorSpace(cs);
        model = rgb ? CGColorSpaceGetModel(rgb) : kCGColorSpaceModelUnknown;
    }
    auto useP3 = [&] {
        t.space = namedSpace(kCGColorSpaceDisplayP3);
        t.qspace = QColorSpace(QColorSpace::DisplayP3);
    };
    auto useSRGB = [&] {
        t.space = namedSpace(kCGColorSpaceSRGB);
        t.qspace = QColorSpace(QColorSpace::SRgb);
    };
    if (model == kCGColorSpaceModelMonochrome) {
        useSRGB();
        return t;
    }
    if (model != kCGColorSpaceModelRGB) {
        useP3(); // CMYK, Lab, DeviceN, ... -> ColorSync conversion (F-COL-06)
        return t;
    }
    if (CGColorSpaceUsesExtendedRange(rgb) || CGColorSpaceUsesITUR_2100TF(rgb)) {
        useP3(); // HDR content requested as SDR: ColorSync tone-maps into P3
        return t;
    }
    if (sameName(rgb, kCGColorSpaceSRGB)) {
        t.space = CFRef<CGColorSpaceRef>(CGColorSpaceRetain(rgb));
        t.qspace = QColorSpace(QColorSpace::SRgb);
        return t;
    }
    if (sameName(rgb, kCGColorSpaceDisplayP3)) {
        t.space = CFRef<CGColorSpaceRef>(CGColorSpaceRetain(rgb));
        t.qspace = QColorSpace(QColorSpace::DisplayP3);
        return t;
    }
    CFRef<CFDataRef> icc(CGColorSpaceCopyICCData(rgb));
    CFRef<CGColorSpaceRef> deviceRgb(CGColorSpaceCreateDeviceRGB());
    if (!icc || CFEqual(rgb, deviceRgb.get())) {
        // Untagged / device RGB: interpret the values as sRGB without converting them.
        // (Tagged spaces such as Generic RGB - gamma 1.8, other primaries - take the ICC
        // path below and keep their real profile.)
        useSRGB();
        if (cs == rgb)
            t.replacement.reset(CGImageCreateCopyWithColorSpace(img, t.space.get()));
        return t;
    }
    const QByteArray iccBytes(reinterpret_cast<const char*>(CFDataGetBytePtr(icc.get())),
                              CFDataGetLength(icc.get()));
    QColorSpace q = QColorSpace::fromIccProfile(iccBytes);
    if (q.isValid() && q.colorModel() == QColorSpace::ColorModel::Rgb) {
        t.space = CFRef<CGColorSpaceRef>(CGColorSpaceRetain(rgb));
        t.qspace = q;
    } else {
        useP3(); // profile Qt cannot represent (e.g. LUT based): convert, never flatten to sRGB
    }
    return t;
}

struct Rendered {
    QImage image;
    float headroom = 1.0f;
};

// Draws `img` into a QImage, applying EXIF orientation `o` (1 = none).
std::optional<Rendered> render(CGImageRef img, int o, bool hdr, QString* error) {
    const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (w == 0 || h == 0) {
        setError(error, QCoreApplication::translate("lf::ImageIODecoder", "Das Bild hat keine Pixel."));
        return std::nullopt;
    }
    const bool swap = o >= 5;
    const int dw = int(swap ? h : w), dh = int(swap ? w : h);

    Target target;
    QImage::Format fmt;
    size_t bpc;
    uint32_t bitmapInfo;
    if (hdr) {
        target.space = namedSpace(kCGColorSpaceExtendedLinearDisplayP3);
        target.qspace = QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear);
        fmt = QImage::Format_RGBA16FPx4_Premultiplied;
        bpc = 16;
        bitmapInfo = uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapFloatComponents) |
                     uint32_t(kCGBitmapByteOrder16Little);
    } else {
        target = sdrTarget(img);
        if (target.replacement)
            img = target.replacement.get();
        const bool deep = CGImageGetBitsPerComponent(img) > 8;
        fmt = deep ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied;
        bpc = deep ? 16 : 8;
        bitmapInfo = uint32_t(kCGImageAlphaPremultipliedLast) |
                     uint32_t(deep ? kCGBitmapByteOrder16Little : kCGBitmapByteOrder32Big);
    }

    QImage out(dw, dh, fmt);
    if (out.isNull()) {
        setError(error, QCoreApplication::translate("lf::ImageIODecoder",
                                                    "Nicht genug Speicher für das Bild (%1 × %2 Pixel).")
                            .arg(dw)
                            .arg(dh));
        return std::nullopt;
    }
    CFRef<CGContextRef> ctx(CGBitmapContextCreate(
        out.bits(), size_t(dw), size_t(dh), bpc, size_t(out.bytesPerLine()), target.space.get(), bitmapInfo));
    if (!ctx) {
        setError(error, QCoreApplication::translate("lf::ImageIODecoder",
                                                    "Bildpuffer konnte nicht angelegt werden."));
        return std::nullopt;
    }
    float headroom = 1.0f;
    if (hdr) {
        headroom = CGImageGetContentHeadroom(img);
        // Do not tone-map: a target headroom above any realistic content keeps the full
        // range the decoder delivered (PQ peaks at 10000 / 203 nits ~ 49x SDR white).
        CGContextSetEDRTargetHeadroom(ctx.get(), std::isfinite(headroom) ? std::max(headroom, 64.0f) : 64.0f);
    }
    CGContextSetBlendMode(ctx.get(), kCGBlendModeCopy);
    CGContextSetInterpolationQuality(ctx.get(), kCGInterpolationNone);
    CGContextConcatCTM(ctx.get(), orientationTransform(o, CGFloat(w), CGFloat(h)));
    CGContextDrawImage(ctx.get(), CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
    ctx.reset();
    out.setColorSpace(target.qspace);
    if (hdr)
        // Measured peak: non-finite samples (a single +inf) are skipped, never headroom 1.
        headroom = std::isfinite(headroom) && headroom > 1.0f ? headroom
                                                              : std::max(1.0f, lf::hdr::measuredPeak(out));
    return Rendered{std::move(out), headroom};
}

QString typeOfFile(CGImageSourceRef src) {
    CFStringRef t = CGImageSourceGetType(src);
    return t ? QString::fromNSString((__bridge NSString*)t) : QString();
}

// Opens and validates a source. Returns null + error on failure.
CFRef<CGImageSourceRef> openValidated(const QString& path, Layout* layout, QString* error) {
    const QFileInfo fi(path);
    if (!fi.exists()) {
        setError(error, QCoreApplication::translate("lf::ImageIODecoder", "Die Datei existiert nicht."));
        return {};
    }
    if (fi.size() == 0) {
        setError(error, QCoreApplication::translate("lf::ImageIODecoder", "Die Datei ist leer (0 Byte)."));
        return {};
    }
    CFRef<CGImageSourceRef> src = openSource(path);
    if (!src) {
        setError(error,
                 QCoreApplication::translate("lf::ImageIODecoder", "Die Datei kann nicht gelesen werden."));
        return {};
    }
    if (typeOfFile(src.get()).isEmpty() || CGImageSourceGetStatus(src.get()) == kCGImageStatusUnknownType) {
        setError(error, QCoreApplication::translate("lf::ImageIODecoder",
                                                    "Unbekanntes oder nicht unterstütztes Dateiformat."));
        return {};
    }
    *layout = layoutOf(src.get());
    if (layout->count == 0) {
        setError(error,
                 QCoreApplication::translate("lf::ImageIODecoder",
                                             "Die Datei enthält keine lesbaren Bilddaten (beschädigt?)."));
        return {};
    }
    return src;
}

// Decodes image `index` of an open source (shared by decode() and the frame reader).
std::optional<DecodedImage> decodeAt(CGImageSourceRef src, const Layout& l, size_t index,
                                     const DecodeRequest& req, QString* error) {
    NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src, index, nullptr));
    int o = [value(props, kCGImagePropertyOrientation) intValue];
    if (o < 1 || o > 8)
        o = 1;
    const int pw = [value(props, kCGImagePropertyPixelWidth) intValue];
    const int ph = [value(props, kCGImagePropertyPixelHeight) intValue];
    const QSize fullSize = o >= 5 ? QSize(ph, pw) : QSize(pw, ph);
    const bool floatSource = [value(props, kCGImagePropertyIsFloat) boolValue];
    const bool hdr = (req.hdr || (req.hdrFloat && floatSource)) && isHdrImage(src, index, props);

    NSMutableDictionary* opts = [NSMutableDictionary dictionary];
    if (hdr)
        opts[(__bridge NSString*)kCGImageSourceDecodeRequest] = (__bridge NSString*)kCGImageSourceDecodeToHDR;

    // Full decodes beyond the cap are downsampled to the cap (see header). P-08: the
    // byte limit counts the output format (16-bit and float sources: 8 bytes/pixel).
    const bool deepSource = hdr || [value(props, kCGImagePropertyIsFloat) boolValue] ||
                            [value(props, kCGImagePropertyDepth) intValue] > 8;
    const qint64 maxPixels = req.maxBytes > 0 ? std::max<qint64>(1, req.maxBytes / (deepSource ? 8 : 4)) : 0;
    int maxPixelSize = req.maxPixelSize;
    bool capped = false;
    if (pw > 0 && ph > 0) {
        const int cap = cappedLongEdge(pw, ph, maxPixels);
        if (maxPixelSize <= 0 ? cap < std::max(pw, ph) : maxPixelSize > cap) {
            maxPixelSize = cap;
            capped = !req.preferEmbeddedPreview;
        }
    }

    CFRef<CGImageRef> img;
    int drawOrientation = o;
    if (maxPixelSize > 0 || req.preferEmbeddedPreview) {
        NSMutableDictionary* t = [opts mutableCopy];
        // A thumbnail is created as a bitmap anyway; decoding it right here keeps the
        // work on this worker thread.
        t[(__bridge NSString*)kCGImageSourceShouldCacheImmediately] = @YES;
        t[(__bridge NSString*)(req.preferEmbeddedPreview ? kCGImageSourceCreateThumbnailFromImageIfAbsent
                                                         : kCGImageSourceCreateThumbnailFromImageAlways)] =
            @YES;
        t[(__bridge NSString*)kCGImageSourceCreateThumbnailWithTransform] = @YES;
        if (maxPixelSize > 0)
            t[(__bridge NSString*)kCGImageSourceThumbnailMaxPixelSize] = @(maxPixelSize);
        img.reset(CGImageSourceCreateThumbnailAtIndex(src, index, (__bridge CFDictionaryRef)t));
        if (img)
            drawOrientation = 1; // already oriented by ImageIO
    }
    if (!img && !capped) {
        // Draw-once full decode: no ImageIO cache. With kCGImageSourceShouldCacheImmediately
        // ImageIO would keep its own full-size bitmap while we draw into a second one.
        opts[(__bridge NSString*)kCGImageSourceShouldCache] = @NO;
        img.reset(CGImageSourceCreateImageAtIndex(src, index, (__bridge CFDictionaryRef)opts));
        drawOrientation = o;
    }
    if (!img) {
        setError(error,
                 QCoreApplication::translate("lf::ImageIODecoder",
                                             "Das Bild konnte nicht dekodiert werden (Datei beschädigt?)."));
        return std::nullopt;
    }
    // HDR only if the decoder actually delivered extended range data.
    bool hdrOut = false;
    if (hdr) {
        CGColorSpaceRef cs = CGImageGetColorSpace(img.get());
        hdrOut = cs && (CGColorSpaceUsesITUR_2100TF(cs) || CGColorSpaceUsesExtendedRange(cs) ||
                        CGImageGetContentHeadroom(img.get()) > 1.0f);
    }
    auto r = render(img.get(), drawOrientation, hdrOut, error);
    if (!r)
        return std::nullopt;

    DecodedImage out;
    out.fullSize = fullSize.isValid() && !fullSize.isEmpty() ? fullSize : r->image.size();
    // A capped full decode is the best this backend delivers: not a preview.
    out.isPreview =
        !capped && (r->image.width() < out.fullSize.width() || r->image.height() < out.fullSize.height());
    out.headroom = r->headroom;
    out.frameDurationMs = l.animated ? frameDurationMs(props) : 0;
    out.image = std::move(r->image);
    // F-FMT-03/04: which image of the file this is.
    out.frameCount = l.animated ? int(l.count) : 1;
    out.pageCount = l.animated ? 1 : int(std::max<size_t>(1, l.count));
    out.frame = l.animated ? int(index) : 0;
    out.page = l.animated ? 0 : req.page;
    return out;
}

// ---- metadata ---------------------------------------------------------------------------
// Values are formatted for the UI language (core/LocaleFormat.h, F-I18N-04).

std::optional<QDateTime> parseExifDate(const QString& s, const QString& offset, const QString& subsec) {
    QDateTime dt = QDateTime::fromString(s.trimmed(), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    if (!dt.isValid())
        return std::nullopt;
    if (!subsec.isEmpty()) {
        bool ok = false;
        const int ms = (subsec.trimmed() + QStringLiteral("00")).left(3).toInt(&ok);
        if (ok)
            dt = dt.addMSecs(ms);
    }
    const QString off = offset.trimmed();
    if (off.size() >= 6 && (off[0] == u'+' || off[0] == u'-')) {
        const int sign = off[0] == u'-' ? -1 : 1;
        const int secs = sign * (off.mid(1, 2).toInt() * 3600 + off.mid(4, 2).toInt() * 60);
        dt = QDateTime(dt.date(), dt.time(), QTimeZone::fromSecondsAheadOfUtc(secs));
    }
    return dt;
}

QString flashText(int flash) {
    if (flash & 0x1)
        return QCoreApplication::translate("lf::ImageIODecoder", "Ausgelöst");
    return QCoreApplication::translate("lf::ImageIODecoder", "Nicht ausgelöst");
}

} // namespace

// ---- Decoder interface -------------------------------------------------------------------

QString ImageIODecoder::typeByMagic(const QByteArray& header) {
    // Formats ImageIO reads but does not recognise from the first bytes alone (DDS), and
    // the other GPU texture containers and Radiance for robustness.
    struct Magic {
        qsizetype offset;
        QByteArrayView bytes;
        const char* type;
    };
    static const Magic magics[] = {
        {0, QByteArrayView("DDS \x7c\0\0\0", 8), "com.microsoft.dds"}, // magic + header size 124
        {0, QByteArrayView("\xabKTX 11\xbb\r\n\x1a\n", 12), "org.khronos.ktx"},
        {0, QByteArrayView("\xabKTX 20\xbb\r\n\x1a\n", 12), "org.khronos.ktx2"},
        {0, QByteArrayView("\x13\xab\xa1\x5c", 4), "org.khronos.astc"},
        {0, QByteArrayView("PVR\x03", 4), "public.pvr"},
        {0, QByteArrayView("\x03RVP", 4), "public.pvr"}, // PVR v3, other byte order
        {44, QByteArrayView("PVR!", 4), "public.pvr"},   // PVR v2
        {0, QByteArrayView("#?RADIANCE", 10), "public.radiance"},
        {0, QByteArrayView("#?RGBE", 6), "public.radiance"},
    };
    for (const Magic& m : magics)
        if (header.size() >= m.offset + m.bytes.size() &&
            QByteArrayView(header).sliced(m.offset, m.bytes.size()) == m.bytes)
            return QString::fromLatin1(m.type);
    return {};
}

bool ImageIODecoder::canDecode(const QString& path, const QByteArray& header) const {
    @autoreleasepool {
        if (const QString t = typeByMagic(header); !t.isEmpty())
            return readableTypes().contains(t);
        if (!header.isEmpty()) {
            CFRef<CFDataRef> data(
                CFDataCreateWithBytesNoCopy(nullptr, reinterpret_cast<const UInt8*>(header.constData()),
                                            header.size(), kCFAllocatorNull));
            CFRef<CGImageSourceRef> src(CGImageSourceCreateWithData(data.get(), nullptr));
            if (src) {
                const QString t = typeOfFile(src.get());
                if (!t.isEmpty())
                    return readableTypes().contains(t);
            }
        }
        // No signature match: only trust the suffix for formats without a reliable magic
        // number in the first bytes (TGA, some RAW variants).
        const QString suffix = QFileInfo(path).suffix();
        if (suffix.isEmpty() || header.isEmpty())
            return false;
        UTType* type = [UTType typeWithFilenameExtension:suffix.toNSString()];
        if (!type || !readableTypes().contains(QString::fromNSString(type.identifier)))
            return false;
        UTType* raw = [UTType typeWithIdentifier:@"public.camera-raw-image"];
        return [type conformsToType:raw] || [type.identifier isEqualToString:@"com.truevision.tga-image"];
    }
}

std::optional<ImageInfo> ImageIODecoder::info(const QString& path, QString* error) {
    @autoreleasepool {
        Layout l;
        CFRef<CGImageSourceRef> src = openValidated(path, &l, error);
        if (!src)
            return std::nullopt;
        NSDictionary* props =
            CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src.get(), l.primary, nullptr));
        if (!props || !value(props, kCGImagePropertyPixelWidth)) {
            // explicit context: lupdate loses track of the class after the Objective-C above
            setError(error, QCoreApplication::translate("lf::ImageIODecoder",
                                                        "Die Bildinformationen sind beschädigt."));
            return std::nullopt;
        }
        return infoFromProperties(src.get(), l, l.primary, props);
    }
}

std::optional<DecodedImage> ImageIODecoder::decode(const QString& path, const DecodeRequest& req,
                                                   QString* error) {
    @autoreleasepool {
        Layout l;
        CFRef<CGImageSourceRef> src = openValidated(path, &l, error);
        if (!src)
            return std::nullopt;
        const auto index = imageIndex(l, req);
        if (!index) {
            setError(error,
                     l.animated
                         ? QCoreApplication::translate("lf::ImageIODecoder", "Einzelbild %1 existiert nicht.")
                               .arg(req.frame + 1)
                         : QCoreApplication::translate("lf::ImageIODecoder", "Seite %1 existiert nicht.")
                               .arg(req.page + 1));
            return std::nullopt;
        }
        if (l.type == u"public.jpeg" && jpegLooksTruncated(path)) {
            setError(error, QCoreApplication::translate("lf::ImageIODecoder",
                                                        "Die Datei ist unvollständig (abgeschnitten)."));
            return std::nullopt;
        }
        return decodeAt(src.get(), l, *index, req, error);
    }
}

namespace {

// F-FMT-03: one source kept open for all frames of an animation.
class ImageIOFrameReader final : public FrameReader {
public:
    ImageIOFrameReader(CFRef<CGImageSourceRef> src, Layout layout)
        : m_src(std::move(src)), m_layout(std::move(layout)) {}
    int frameCount() const override { return m_layout.animated ? int(m_layout.count) : 1; }
    std::optional<DecodedImage> readFrame(int index, const DecodeRequest& req, QString* error) override {
        @autoreleasepool {
            if (index < 0 || index >= frameCount()) {
                setError(error,
                         QCoreApplication::translate("lf::ImageIODecoder", "Einzelbild %1 existiert nicht.")
                             .arg(index + 1));
                return std::nullopt;
            }
            DecodeRequest r = req;
            r.frame = index;
            r.page = 0;
            const size_t at = m_layout.animated ? size_t(index) : m_layout.primary;
            return decodeAt(m_src.get(), m_layout, at, r, error);
        }
    }

private:
    CFRef<CGImageSourceRef> m_src;
    Layout m_layout;
};

} // namespace

std::unique_ptr<FrameReader> ImageIODecoder::openFrames(const QString& path) {
    @autoreleasepool {
        Layout l;
        QString error;
        CFRef<CGImageSourceRef> src = openValidated(path, &l, &error);
        if (!src)
            return nullptr;
        return std::make_unique<ImageIOFrameReader>(std::move(src), l);
    }
}

Metadata ImageIODecoder::metadata(const QString& path) {
    Metadata md;
    @autoreleasepool {
        Layout l;
        CFRef<CGImageSourceRef> src = openValidated(path, &l, nullptr);
        if (!src)
            return md;
        NSDictionary* props =
            CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src.get(), l.primary, nullptr));
        if (!props)
            return md;
        const ImageInfo info = infoFromProperties(src.get(), l, l.primary, props);

        // Rows with stable keys; groups and labels come from the table in MetadataKeys.cpp.
        addMetadata(md, QStringLiteral("file.format"), info.formatName);
        addMetadata(md, QStringLiteral("file.dimensions"),
                    QStringLiteral("%1 × %2").arg(info.displaySize.width()).arg(info.displaySize.height()));
        if (!info.colorModel.isEmpty())
            addMetadata(md, QStringLiteral("file.colorModel"), info.colorModel);
        if (!info.profileName.isEmpty())
            addMetadata(md, QStringLiteral("file.profile"), info.profileName);
        addMetadata(md, QStringLiteral("file.bitDepth"),
                    QCoreApplication::translate("lf::ImageIODecoder", "%1 Bit").arg(info.bitsPerComponent));
        addMetadata(md, QStringLiteral("file.hdr"),
                    info.isHdr ? QCoreApplication::translate("lf::ImageIODecoder", "Ja")
                               : QCoreApplication::translate("lf::ImageIODecoder", "Nein"));
        if (info.hasAlpha)
            addMetadata(md, QStringLiteral("file.alpha"),
                        QCoreApplication::translate("lf::ImageIODecoder", "Ja"));
        if (info.isAnimated)
            addMetadata(md, QStringLiteral("file.frames"), info.frameCount);
        if (info.pageCount > 1)
            addMetadata(md, QStringLiteral("file.pages"), info.pageCount);
        if (info.exifOrientation != 1)
            addMetadata(md, QStringLiteral("file.orientation"), info.exifOrientation);

        NSDictionary* tiff = dictValue(props, kCGImagePropertyTIFFDictionary);
        NSDictionary* exif = dictValue(props, kCGImagePropertyExifDictionary);
        NSDictionary* exifAux = dictValue(props, kCGImagePropertyExifAuxDictionary);
        NSDictionary* gps = dictValue(props, kCGImagePropertyGPSDictionary);

        auto putStr = [&md](const char* key, id v) {
            const QString s = toQString(v).trimmed();
            if (!s.isEmpty())
                addMetadata(md, QString::fromLatin1(key), s);
        };
        putStr("camera.make", value(tiff, kCGImagePropertyTIFFMake));
        putStr("camera.model", value(tiff, kCGImagePropertyTIFFModel));
        id lens = value(exif, kCGImagePropertyExifLensModel);
        if (!lens)
            lens = value(exifAux, kCGImagePropertyExifAuxLensModel);
        putStr("camera.lens", lens);

        if (auto f = toDouble(value(exif, kCGImagePropertyExifFocalLength)); f && *f > 0) {
            const auto f35 = toDouble(value(exif, kCGImagePropertyExifFocalLenIn35mmFilm));
            addMetadata(md, QStringLiteral("shot.focalLength"), formatFocalLength(*f, f35.value_or(0)));
        }
        if (auto n = toDouble(value(exif, kCGImagePropertyExifFNumber)); n && *n > 0)
            addMetadata(md, QStringLiteral("shot.aperture"), formatAperture(*n));
        if (auto t = toDouble(value(exif, kCGImagePropertyExifExposureTime)); t && *t > 0)
            addMetadata(md, QStringLiteral("shot.exposureTime"), formatExposureTime(*t));
        if (auto iso = toDouble(value(exif, kCGImagePropertyExifISOSpeedRatings)); iso && *iso > 0)
            addMetadata(md, QStringLiteral("shot.iso"), QString::number(qRound(*iso)));
        if (auto ev = toDouble(value(exif, kCGImagePropertyExifExposureBiasValue)); ev) {
            addMetadata(md, QStringLiteral("shot.exposureBias"), formatExposureBias(*ev));
        }
        if (id flash = value(exif, kCGImagePropertyExifFlash))
            addMetadata(md, QStringLiteral("shot.flash"), flashText([flash intValue]));

        const QString subsec = toQString(value(exif, kCGImagePropertyExifSubsecTimeOriginal));
        auto captured = parseExifDate(toQString(value(exif, kCGImagePropertyExifDateTimeOriginal)),
                                      toQString(value(exif, kCGImagePropertyExifOffsetTimeOriginal)), subsec);
        if (!captured)
            captured =
                parseExifDate(toQString(value(exif, kCGImagePropertyExifDateTimeDigitized)),
                              toQString(value(exif, kCGImagePropertyExifOffsetTimeDigitized)), QString());
        if (captured) {
            md.captured = captured;
            QString s = formatDateTime(*captured);
            if (captured->timeSpec() == Qt::OffsetFromUTC) {
                const int off = captured->offsetFromUtc();
                s += QStringLiteral(" (UTC%1%2:%3)")
                         .arg(off < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                         .arg(std::abs(off) / 3600, 2, 10, QLatin1Char('0'))
                         .arg((std::abs(off) % 3600) / 60, 2, 10, QLatin1Char('0'));
            }
            addMetadata(md, QStringLiteral("shot.captured"), s);
        }

        if (gps) {
            auto lat = toDouble(value(gps, kCGImagePropertyGPSLatitude));
            auto lon = toDouble(value(gps, kCGImagePropertyGPSLongitude));
            if (lat && lon) {
                const QString latRef = toQString(value(gps, kCGImagePropertyGPSLatitudeRef)).toUpper();
                const QString lonRef = toQString(value(gps, kCGImagePropertyGPSLongitudeRef)).toUpper();
                const double la = latRef == u"S" ? -std::abs(*lat) : std::abs(*lat);
                const double lo = lonRef == u"W" ? -std::abs(*lon) : std::abs(*lon);
                md.latitude = la;
                md.longitude = lo;
                addMetadata(md, QStringLiteral("gps.latitude"), formatLatitude(la));
                addMetadata(md, QStringLiteral("gps.longitude"), formatLongitude(lo));
            }
            if (auto alt = toDouble(value(gps, kCGImagePropertyGPSAltitude))) {
                const bool below = [value(gps, kCGImagePropertyGPSAltitudeRef) intValue] == 1;
                addMetadata(md, QStringLiteral("gps.altitude"), formatAltitude(below ? -*alt : *alt));
            }
        }

        // Rating: XMP first, then IPTC star rating.
        CFRef<CGImageMetadataRef> xmp(CGImageSourceCopyMetadataAtIndex(src.get(), l.primary, nullptr));
        if (xmp) {
            CFRef<CGImageMetadataTagRef> tag(
                CGImageMetadataCopyTagWithPath(xmp.get(), nullptr, CFSTR("xmp:Rating")));
            if (tag) {
                id v = CFBridgingRelease(CGImageMetadataTagCopyValue(tag.get()));
                if (auto r = toDouble(v))
                    md.rating = int(std::lround(*r));
            }
        }
        if (md.rating < 0) {
            NSDictionary* iptc = dictValue(props, kCGImagePropertyIPTCDictionary);
            if (auto r = toDouble(value(iptc, kCGImagePropertyIPTCStarRating)))
                md.rating = int(std::lround(*r));
        }
        if (md.rating >= 0)
            addMetadata(md, QStringLiteral("file.rating"), md.rating);
    }
    return md;
}

QStringList ImageIODecoder::supportedExtensions() {
    static const QStringList exts = [] {
        QSet<QString> s;
        @autoreleasepool {
            for (const QString& id : readableTypes()) {
                UTType* t = [UTType typeWithIdentifier:id.toNSString()];
                NSArray<NSString*>* list = t.tags[UTTagClassFilenameExtension];
                for (NSString* e in list)
                    s.insert(QString::fromNSString(e).toLower());
            }
            // Suffixes whose files ImageIO recognises by signature although no UTType maps
            // them to the reader (APNG/AVIF sequences/PNM share the PNG/AVIF/PBM decoders).
            const std::pair<const char*, const char*> bySignature[] = {
                {"apng", "public.png"}, {"avifs", "public.avis"}, {"pnm", "public.pbm"}};
            for (const auto& [suffix, type] : bySignature)
                if (readableTypes().contains(QLatin1StringView(type)))
                    s.insert(QString::fromLatin1(suffix));
            // Aliases the readable types do not list themselves (jfif, cur, ...): a known
            // image suffix counts if its UTType is, or conforms to, a readable type.
            for (const QString& suffix : defaultImageSuffixes()) {
                if (s.contains(suffix))
                    continue;
                UTType* t = [UTType typeWithFilenameExtension:suffix.toNSString()];
                if (!t || t.dynamic)
                    continue;
                for (const QString& id : readableTypes()) {
                    UTType* readable = [UTType typeWithIdentifier:id.toNSString()];
                    if (readable && [t conformsToType:readable]) {
                        s.insert(suffix);
                        break;
                    }
                }
            }
        }
        QStringList l(s.begin(), s.end());
        l.sort();
        return l;
    }();
    return exts;
}

QString ImageIODecoder::formatNameForType(const QString& t) {
    struct Entry {
        const char* uti;
        const char* name;
    };
    static const Entry table[] = {
        {"public.jpeg", "JPEG"},
        {"public.png", "PNG"},
        {"com.compuserve.gif", "GIF"},
        {"public.tiff", "TIFF"},
        {"com.microsoft.bmp", "BMP"},
        {"com.microsoft.ico", "ICO"},
        {"com.microsoft.cur", "CUR"},
        {"com.apple.icns", "ICNS"},
        {"org.webmproject.webp", "WebP"},
        {"public.heic", "HEIC"},
        {"public.heics", QT_TRANSLATE_NOOP("lf::ImageIODecoder", "HEIC-Sequenz")},
        {"public.heif", "HEIF"},
        {"public.avci", "AVCI"},
        {"public.avif", "AVIF"},
        {"public.avis", QT_TRANSLATE_NOOP("lf::ImageIODecoder", "AVIF-Sequenz")},
        {"public.jpeg-xl", "JPEG XL"},
        {"public.jpeg-2000", "JPEG 2000"},
        {"com.adobe.photoshop-image", "Photoshop (PSD)"},
        {"com.ilm.openexr-image", "OpenEXR"},
        {"public.radiance", "Radiance HDR"},
        {"com.truevision.tga-image", "TGA"},
        {"com.microsoft.dds", "DDS"},
        {"org.khronos.ktx", "KTX"},
        {"org.khronos.ktx2", "KTX2"},
        {"org.khronos.astc", "ASTC"},
        {"public.pbm", "PBM"},
        {"public.pvr", "PVR"},
        {"com.sgi.sgi-image", "SGI"},
        {"com.apple.pict", "PICT"},
        {"public.mpo-image", "MPO"},
        {"org.nema.dicom", "DICOM"},
        {"com.apple.atx", "ATX"},
        {"com.adobe.pdf", "PDF"},
        // RAW
        {"com.adobe.raw-image", "Adobe DNG"},
        {"com.canon.cr2-raw-image", "Canon CR2"},
        {"com.canon.cr3-raw-image", "Canon CR3"},
        {"com.canon.crw-raw-image", "Canon CRW"},
        {"com.canon.tif-raw-image", "Canon TIF-RAW"},
        {"com.nikon.raw-image", "Nikon NEF"},
        {"com.nikon.nrw-raw-image", "Nikon NRW"},
        {"com.nikon.nefx-raw-image", "Nikon NEF (HE)"},
        {"com.sony.arw-raw-image", "Sony ARW"},
        {"com.sony.raw-image", "Sony SRF"},
        {"com.sony.sr2-raw-image", "Sony SR2"},
        {"com.sony.axr-raw-image", "Sony AXR"},
        {"com.fuji.raw-image", "Fujifilm RAF"},
        {"com.olympus.raw-image", "Olympus ORF"},
        {"com.olympus.or-raw-image", "OM System ORF"},
        {"com.olympus.sr-raw-image", "Olympus SR"},
        {"com.panasonic.raw-image", "Panasonic RAW"},
        {"com.panasonic.rw2-raw-image", "Panasonic RW2"},
        {"com.pentax.raw-image", "Pentax PEF"},
        {"com.leica.raw-image", "Leica RAW"},
        {"com.leica.rwl-raw-image", "Leica RWL"},
        {"com.samsung.raw-image", "Samsung SRW"},
        {"com.hasselblad.3fr-raw-image", "Hasselblad 3FR"},
        {"com.hasselblad.fff-raw-image", "Hasselblad FFF"},
        {"com.phaseone.raw-image", "Phase One IIQ"},
        {"com.leafamerica.raw-image", "Leaf MOS"},
        {"com.konicaminolta.raw-image", "Minolta MRW"},
        {"com.kodak.raw-image", "Kodak RAW"},
        {"com.epson.raw-image", "Epson ERF"},
        {"com.dxo.raw-image", "DxO RAW"},
    };
    for (const auto& e : table)
        if (t == QLatin1StringView(e.uti))
            return QCoreApplication::translate("lf::ImageIODecoder",
                                               e.name); // brand names stay, "HEIC-Sequenz" is translated
    @autoreleasepool {
        UTType* type = [UTType typeWithIdentifier:t.toNSString()];
        if (type.preferredFilenameExtension)
            return QString::fromNSString(type.preferredFilenameExtension).toUpper();
    }
    return t;
}

} // namespace lf
