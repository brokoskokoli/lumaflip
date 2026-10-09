// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/mac/ImageIODecoder.h"

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

QString tr(const char* text) {
    return QCoreApplication::translate("lf::ImageIODecoder", text);
}

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

// Long edge for a full-resolution decode of a w x h image, honouring the caps.
int cappedLongEdge(int w, int h) {
    const qint64 edge = std::max(w, h);
    qint64 cap = std::min<qint64>(edge, ImageIODecoder::kMaxFullEdge);
    const double pixels = double(w) * double(h);
    if (pixels > double(ImageIODecoder::kMaxFullPixels)) {
        const double scale = std::sqrt(double(ImageIODecoder::kMaxFullPixels) / pixels);
        cap = std::min<qint64>(cap, qint64(std::floor(double(edge) * scale)));
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

float peakOf(const QImage& img) {
    float peak = 0.f;
    for (int y = 0; y < img.height(); ++y) {
        const auto* p = reinterpret_cast<const qfloat16*>(img.constScanLine(y));
        for (int x = 0; x < img.width() * 4; x += 4) {
            peak = std::max({peak, float(p[x]), float(p[x + 1]), float(p[x + 2])});
        }
    }
    return std::isfinite(peak) ? peak : 1.f;
}

// Draws `img` into a QImage, applying EXIF orientation `o` (1 = none).
std::optional<Rendered> render(CGImageRef img, int o, bool hdr, QString* error) {
    const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (w == 0 || h == 0) {
        setError(error, tr("Das Bild hat keine Pixel."));
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
        setError(error, tr("Nicht genug Speicher für das Bild (%1 × %2 Pixel).").arg(dw).arg(dh));
        return std::nullopt;
    }
    CFRef<CGContextRef> ctx(CGBitmapContextCreate(
        out.bits(), size_t(dw), size_t(dh), bpc, size_t(out.bytesPerLine()), target.space.get(), bitmapInfo));
    if (!ctx) {
        setError(error, tr("Bildpuffer konnte nicht angelegt werden."));
        return std::nullopt;
    }
    float headroom = 1.0f;
    if (hdr) {
        headroom = CGImageGetContentHeadroom(img);
        // Do not tone-map: a target headroom above any realistic content keeps the full
        // range the decoder delivered (PQ peaks at 10000 / 203 nits ~ 49x SDR white).
        CGContextSetEDRTargetHeadroom(ctx.get(), std::max(headroom, 64.0f));
    }
    CGContextSetBlendMode(ctx.get(), kCGBlendModeCopy);
    CGContextSetInterpolationQuality(ctx.get(), kCGInterpolationNone);
    CGContextConcatCTM(ctx.get(), orientationTransform(o, CGFloat(w), CGFloat(h)));
    CGContextDrawImage(ctx.get(), CGRectMake(0, 0, CGFloat(w), CGFloat(h)), img);
    ctx.reset();
    out.setColorSpace(target.qspace);
    if (hdr)
        headroom = headroom > 1.0f ? headroom : std::max(1.0f, peakOf(out));
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
        setError(error, tr("Die Datei existiert nicht."));
        return {};
    }
    if (fi.size() == 0) {
        setError(error, tr("Die Datei ist leer (0 Byte)."));
        return {};
    }
    CFRef<CGImageSourceRef> src = openSource(path);
    if (!src) {
        setError(error, tr("Die Datei kann nicht gelesen werden."));
        return {};
    }
    if (typeOfFile(src.get()).isEmpty() || CGImageSourceGetStatus(src.get()) == kCGImageStatusUnknownType) {
        setError(error, tr("Unbekanntes oder nicht unterstütztes Dateiformat."));
        return {};
    }
    *layout = layoutOf(src.get());
    if (layout->count == 0) {
        setError(error, tr("Die Datei enthält keine lesbaren Bilddaten (beschädigt?)."));
        return {};
    }
    return src;
}

// ---- metadata formatting (German UI strings) ------------------------------------------

QString formatNumber(double v, int decimals) {
    static const QLocale de(QLocale::German, QLocale::Germany);
    QString s = de.toString(v, 'f', decimals);
    if (decimals > 0 && s.contains(QLatin1Char(','))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char(',')))
            s.chop(1);
    }
    return s;
}

QString formatExposure(double t) {
    if (t <= 0)
        return {};
    if (t < 1.0) {
        const double inv = 1.0 / t;
        return QStringLiteral("1/%1 s").arg(formatNumber(inv >= 10 ? std::round(inv) : inv, 1));
    }
    return QStringLiteral("%1 s").arg(formatNumber(t, 1));
}

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
        return tr("Ausgelöst");
    return tr("Nicht ausgelöst");
}

} // namespace

// ---- Decoder interface -------------------------------------------------------------------

bool ImageIODecoder::canDecode(const QString& path, const QByteArray& header) const {
    @autoreleasepool {
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
            setError(error, tr("Die Bildinformationen sind beschädigt."));
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
            setError(error, l.animated ? tr("Einzelbild %1 existiert nicht.").arg(req.frame + 1)
                                       : tr("Seite %1 existiert nicht.").arg(req.page + 1));
            return std::nullopt;
        }
        if (l.type == u"public.jpeg" && jpegLooksTruncated(path)) {
            setError(error, tr("Die Datei ist unvollständig (abgeschnitten)."));
            return std::nullopt;
        }
        NSDictionary* props =
            CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(src.get(), *index, nullptr));
        int o = [value(props, kCGImagePropertyOrientation) intValue];
        if (o < 1 || o > 8)
            o = 1;
        const int pw = [value(props, kCGImagePropertyPixelWidth) intValue];
        const int ph = [value(props, kCGImagePropertyPixelHeight) intValue];
        const QSize fullSize = o >= 5 ? QSize(ph, pw) : QSize(pw, ph);
        const bool hdr = req.hdr && isHdrImage(src.get(), *index, props);

        NSMutableDictionary* opts = [NSMutableDictionary dictionary];
        if (hdr)
            opts[(__bridge NSString*)kCGImageSourceDecodeRequest] =
                (__bridge NSString*)kCGImageSourceDecodeToHDR;

        // Full decodes beyond the cap are downsampled to the cap (see header).
        int maxPixelSize = req.maxPixelSize;
        bool capped = false;
        if (maxPixelSize <= 0 && !req.preferEmbeddedPreview && pw > 0 && ph > 0) {
            const int cap = cappedLongEdge(pw, ph);
            if (cap < std::max(pw, ph)) {
                maxPixelSize = cap;
                capped = true;
            }
        }

        CFRef<CGImageRef> img;
        int drawOrientation = o;
        if (maxPixelSize > 0 || req.preferEmbeddedPreview) {
            NSMutableDictionary* t = [opts mutableCopy];
            // A thumbnail is created as a bitmap anyway; decoding it right here keeps the
            // work on this worker thread.
            t[(__bridge NSString*)kCGImageSourceShouldCacheImmediately] = @YES;
            t[(__bridge NSString*)(req.preferEmbeddedPreview
                                       ? kCGImageSourceCreateThumbnailFromImageIfAbsent
                                       : kCGImageSourceCreateThumbnailFromImageAlways)] = @YES;
            t[(__bridge NSString*)kCGImageSourceCreateThumbnailWithTransform] = @YES;
            if (maxPixelSize > 0)
                t[(__bridge NSString*)kCGImageSourceThumbnailMaxPixelSize] = @(maxPixelSize);
            img.reset(CGImageSourceCreateThumbnailAtIndex(src.get(), *index, (__bridge CFDictionaryRef)t));
            if (img)
                drawOrientation = 1; // already oriented by ImageIO
        }
        if (!img && !capped) {
            // Draw-once full decode: no ImageIO cache. With kCGImageSourceShouldCacheImmediately
            // ImageIO would keep its own full-size bitmap while we draw into a second one.
            opts[(__bridge NSString*)kCGImageSourceShouldCache] = @NO;
            img.reset(CGImageSourceCreateImageAtIndex(src.get(), *index, (__bridge CFDictionaryRef)opts));
            drawOrientation = o;
        }
        if (!img) {
            setError(error, tr("Das Bild konnte nicht dekodiert werden (Datei beschädigt?)."));
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
        out.isPreview = !capped && (r->image.width() < out.fullSize.width() ||
                                    r->image.height() < out.fullSize.height());
        out.headroom = r->headroom;
        out.frameDurationMs = l.animated ? frameDurationMs(props) : 0;
        out.image = std::move(r->image);
        return out;
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

        QVariantMap file;
        file.insert(QStringLiteral("Format"), info.formatName);
        file.insert(QStringLiteral("Pixelmaße"),
                    QStringLiteral("%1 × %2").arg(info.displaySize.width()).arg(info.displaySize.height()));
        if (!info.colorModel.isEmpty())
            file.insert(QStringLiteral("Farbraum"), info.colorModel);
        if (!info.profileName.isEmpty())
            file.insert(QStringLiteral("Profil"), info.profileName);
        file.insert(QStringLiteral("Bittiefe"), tr("%1 Bit").arg(info.bitsPerComponent));
        file.insert(QStringLiteral("HDR"), info.isHdr ? tr("Ja") : tr("Nein"));
        if (info.hasAlpha)
            file.insert(QStringLiteral("Transparenz"), tr("Ja"));
        if (info.isAnimated)
            file.insert(QStringLiteral("Einzelbilder"), info.frameCount);
        if (info.pageCount > 1)
            file.insert(QStringLiteral("Seiten"), info.pageCount);
        if (info.exifOrientation != 1)
            file.insert(QStringLiteral("Ausrichtung"), info.exifOrientation);
        md.groups.insert(QStringLiteral("Datei"), file);

        NSDictionary* tiff = dictValue(props, kCGImagePropertyTIFFDictionary);
        NSDictionary* exif = dictValue(props, kCGImagePropertyExifDictionary);
        NSDictionary* exifAux = dictValue(props, kCGImagePropertyExifAuxDictionary);
        NSDictionary* gps = dictValue(props, kCGImagePropertyGPSDictionary);

        QVariantMap camera;
        auto putStr = [](QVariantMap& m, const QString& key, id v) {
            const QString s = toQString(v).trimmed();
            if (!s.isEmpty())
                m.insert(key, s);
        };
        putStr(camera, QStringLiteral("Hersteller"), value(tiff, kCGImagePropertyTIFFMake));
        putStr(camera, QStringLiteral("Modell"), value(tiff, kCGImagePropertyTIFFModel));
        id lens = value(exif, kCGImagePropertyExifLensModel);
        if (!lens)
            lens = value(exifAux, kCGImagePropertyExifAuxLensModel);
        putStr(camera, QStringLiteral("Objektiv"), lens);
        if (!camera.isEmpty())
            md.groups.insert(QStringLiteral("Kamera"), camera);

        QVariantMap shot;
        if (auto f = toDouble(value(exif, kCGImagePropertyExifFocalLength)); f && *f > 0) {
            QString s = QStringLiteral("%1 mm").arg(formatNumber(*f, 1));
            if (auto f35 = toDouble(value(exif, kCGImagePropertyExifFocalLenIn35mmFilm));
                f35 && *f35 > 0 && *f35 != *f)
                s += QStringLiteral(" (KB: %1 mm)").arg(formatNumber(*f35, 0));
            shot.insert(QStringLiteral("Brennweite"), s);
        }
        if (auto n = toDouble(value(exif, kCGImagePropertyExifFNumber)); n && *n > 0)
            shot.insert(QStringLiteral("Blende"), QStringLiteral("f/%1").arg(formatNumber(*n, 1)));
        if (auto t = toDouble(value(exif, kCGImagePropertyExifExposureTime)); t && *t > 0)
            shot.insert(QStringLiteral("Belichtungszeit"), formatExposure(*t));
        if (auto iso = toDouble(value(exif, kCGImagePropertyExifISOSpeedRatings)); iso && *iso > 0)
            shot.insert(QStringLiteral("ISO"), QString::number(qRound(*iso)));
        if (auto ev = toDouble(value(exif, kCGImagePropertyExifExposureBiasValue)); ev) {
            const QString num = formatNumber(*ev, 1);
            shot.insert(QStringLiteral("Belichtungskorrektur"),
                        QStringLiteral("%1%2 EV").arg(*ev > 0 ? QStringLiteral("+") : QString(), num));
        }
        if (id flash = value(exif, kCGImagePropertyExifFlash))
            shot.insert(QStringLiteral("Blitz"), flashText([flash intValue]));

        const QString subsec = toQString(value(exif, kCGImagePropertyExifSubsecTimeOriginal));
        auto captured = parseExifDate(toQString(value(exif, kCGImagePropertyExifDateTimeOriginal)),
                                      toQString(value(exif, kCGImagePropertyExifOffsetTimeOriginal)), subsec);
        if (!captured)
            captured =
                parseExifDate(toQString(value(exif, kCGImagePropertyExifDateTimeDigitized)),
                              toQString(value(exif, kCGImagePropertyExifOffsetTimeDigitized)), QString());
        if (captured) {
            md.captured = captured;
            QString s = captured->toString(QStringLiteral("dd.MM.yyyy HH:mm:ss"));
            if (captured->timeSpec() == Qt::OffsetFromUTC) {
                const int off = captured->offsetFromUtc();
                s += QStringLiteral(" (UTC%1%2:%3)")
                         .arg(off < 0 ? QLatin1Char('-') : QLatin1Char('+'))
                         .arg(std::abs(off) / 3600, 2, 10, QLatin1Char('0'))
                         .arg((std::abs(off) % 3600) / 60, 2, 10, QLatin1Char('0'));
            }
            shot.insert(QStringLiteral("Aufnahmedatum"), s);
        }
        if (!shot.isEmpty())
            md.groups.insert(QStringLiteral("Aufnahme"), shot);

        if (gps) {
            QVariantMap g;
            auto lat = toDouble(value(gps, kCGImagePropertyGPSLatitude));
            auto lon = toDouble(value(gps, kCGImagePropertyGPSLongitude));
            if (lat && lon) {
                const QString latRef = toQString(value(gps, kCGImagePropertyGPSLatitudeRef)).toUpper();
                const QString lonRef = toQString(value(gps, kCGImagePropertyGPSLongitudeRef)).toUpper();
                const double la = latRef == u"S" ? -std::abs(*lat) : std::abs(*lat);
                const double lo = lonRef == u"W" ? -std::abs(*lon) : std::abs(*lon);
                md.latitude = la;
                md.longitude = lo;
                g.insert(QStringLiteral("Breite"),
                         QStringLiteral("%1° %2").arg(formatNumber(std::abs(la), 6),
                                                      la < 0 ? QStringLiteral("S") : QStringLiteral("N")));
                g.insert(QStringLiteral("Länge"),
                         QStringLiteral("%1° %2").arg(formatNumber(std::abs(lo), 6),
                                                      lo < 0 ? QStringLiteral("W") : QStringLiteral("O")));
            }
            if (auto alt = toDouble(value(gps, kCGImagePropertyGPSAltitude))) {
                const bool below = [value(gps, kCGImagePropertyGPSAltitudeRef) intValue] == 1;
                g.insert(QStringLiteral("Höhe"),
                         QStringLiteral("%1 m").arg(formatNumber(below ? -*alt : *alt, 1)));
            }
            if (!g.isEmpty())
                md.groups.insert(QStringLiteral("GPS"), g);
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
        if (md.rating >= 0) {
            QVariantMap fileGroup = md.groups.value(QStringLiteral("Datei")).toMap();
            fileGroup.insert(QStringLiteral("Bewertung"), md.rating);
            md.groups.insert(QStringLiteral("Datei"), fileGroup);
        }
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
        {"public.heics", "HEIC-Sequenz"},
        {"public.heif", "HEIF"},
        {"public.avci", "AVCI"},
        {"public.avif", "AVIF"},
        {"public.avis", "AVIF-Sequenz"},
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
            return QString::fromLatin1(e.name);
    @autoreleasepool {
        UTType* type = [UTType typeWithIdentifier:t.toNSString()];
        if (type.preferredFilenameExtension)
            return QString::fromNSString(type.preferredFilenameExtension).toUpper();
    }
    return t;
}

} // namespace lf
