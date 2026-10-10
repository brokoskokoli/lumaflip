// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows/WicDecoder.h"

#include "core/LocaleFormat.h"
#include "platform/windows/WinCom.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QTimeZone>
#include <QTransform>
#include <QtCore/qfloat16.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <propvarutil.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>

namespace lf {

namespace {

using Microsoft::WRL::ComPtr;

// ---- COM and the factory --------------------------------------------------------------

// Worker threads stay in the MTA for their lifetime (entering/leaving it per call would
// tear the apartment down every time).
win::ComScope& threadCom() {
    thread_local win::ComScope scope;
    return scope;
}

ComPtr<IWICImagingFactory> factory() {
    threadCom();
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

// ---- installed codecs -------------------------------------------------------------------

struct Pattern {
    quint64 position = 0;
    QByteArray bytes;
    QByteArray mask;
};

struct Codec {
    GUID container{};
    QString name;
    QStringList extensions; // lower case, without dot
    QList<Pattern> patterns;
};

bool isGif(const GUID& g) {
    return IsEqualGUID(g, GUID_ContainerFormatGif) != 0;
}

QList<Codec> enumerateCodecs() {
    QList<Codec> out;
    ComPtr<IWICImagingFactory> f = factory();
    if (!f)
        return out;
    ComPtr<IEnumUnknown> e;
    if (FAILED(f->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault, &e)))
        return out;
    ComPtr<IUnknown> unknown;
    ULONG fetched = 0;
    while (e->Next(1, &unknown, &fetched) == S_OK && fetched == 1) {
        ComPtr<IWICBitmapDecoderInfo> info;
        if (FAILED(unknown.As(&info))) {
            unknown.Reset();
            continue;
        }
        unknown.Reset();
        Codec c;
        info->GetContainerFormat(&c.container);
        UINT len = 0;
        if (SUCCEEDED(info->GetFriendlyName(0, nullptr, &len)) && len > 0) {
            std::vector<wchar_t> buf(len);
            if (SUCCEEDED(info->GetFriendlyName(len, buf.data(), &len)))
                c.name = QString::fromWCharArray(buf.data()).trimmed();
        }
        len = 0;
        if (SUCCEEDED(info->GetFileExtensions(0, nullptr, &len)) && len > 0) {
            std::vector<wchar_t> buf(len);
            if (SUCCEEDED(info->GetFileExtensions(len, buf.data(), &len))) {
                const QStringList exts = QString::fromWCharArray(buf.data()).split(u',', Qt::SkipEmptyParts);
                for (QString x : exts) {
                    x = x.trimmed().toLower();
                    if (x.startsWith(u'.'))
                        x.remove(0, 1);
                    if (!x.isEmpty() && !c.extensions.contains(x))
                        c.extensions << x;
                }
            }
        }
        UINT count = 0, bytes = 0;
        if (SUCCEEDED(info->GetPatterns(0, nullptr, &count, &bytes)) && bytes > 0) {
            std::vector<BYTE> buf(bytes);
            auto* patterns = reinterpret_cast<WICBitmapPattern*>(buf.data());
            if (SUCCEEDED(info->GetPatterns(bytes, patterns, &count, &bytes))) {
                for (UINT i = 0; i < count; ++i) {
                    const WICBitmapPattern& p = patterns[i];
                    if (p.EndOfStream || !p.Pattern || !p.Mask || p.Length == 0)
                        continue;
                    c.patterns.append(
                        {quint64(p.Position.QuadPart),
                         QByteArray(reinterpret_cast<const char*>(p.Pattern), qsizetype(p.Length)),
                         QByteArray(reinterpret_cast<const char*>(p.Mask), qsizetype(p.Length))});
                }
            }
        }
        out << c;
    }
    return out;
}

const QList<Codec>& codecs() {
    static const QList<Codec> list = enumerateCodecs(); // thread-safe static init
    return list;
}

bool matches(const Pattern& p, const QByteArray& header) {
    if (p.position + quint64(p.bytes.size()) > quint64(header.size()))
        return false;
    for (qsizetype i = 0; i < p.bytes.size(); ++i) {
        const auto m = uchar(p.mask.at(i));
        if ((uchar(header.at(qsizetype(p.position) + i)) & m) != (uchar(p.bytes.at(i)) & m))
            return false;
    }
    return true;
}

// ---- errors -------------------------------------------------------------------------------

QString messageFor(HRESULT hr) {
    // HRESULT_FROM_WIN32 is a function in current SDKs: no switch over it.
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return DecoderRegistry::messageMissing();
    if (hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
        hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION))
        return DecoderRegistry::messageUnreadable();
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND || hr == WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT ||
        hr == WINCODEC_ERR_UNSUPPORTEDOPERATION || hr == WINCODEC_ERR_UNKNOWNIMAGEFORMAT)
        return DecoderRegistry::messageUnsupported();
    if (hr == E_OUTOFMEMORY || hr == WINCODEC_ERR_INSUFFICIENTBUFFER)
        return DecoderRegistry::messageTooLarge();
    return DecoderRegistry::messageDamaged();
}

void setError(QString* error, HRESULT hr) {
    if (error)
        *error = messageFor(hr) + QStringLiteral(" (WIC 0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// ---- reading -----------------------------------------------------------------------------

// The open file: shared for reading, writing and deleting, so a shown image can still be
// renamed, trashed or rewritten by other programs (WIC's own CreateDecoderFromFilename only
// shares reading). Declared first in Source: closed after the decoder is released.
struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
};

struct Source {
    FileHandle file;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    GUID container{};
    UINT frameCount = 1;
};

HRESULT open(const QString& path, Source* s) {
    s->factory = factory();
    if (!s->factory)
        return REGDB_E_CLASSNOTREG;
    const QString native = QDir::toNativeSeparators(path);
    s->file.h =
        CreateFileW(win::wstr(native), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s->file.h == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = s->factory->CreateDecoderFromFileHandle(reinterpret_cast<ULONG_PTR>(s->file.h), nullptr,
                                                         WICDecodeMetadataCacheOnDemand, &s->decoder);
    if (FAILED(hr))
        return hr;
    s->decoder->GetContainerFormat(&s->container);
    if (FAILED(s->decoder->GetFrameCount(&s->frameCount)) || s->frameCount == 0)
        s->frameCount = 1;
    return S_OK;
}

QString containerName(const GUID& g) {
    struct Name {
        const GUID* guid;
        const char* name;
    };
    static const Name names[] = {
        {&GUID_ContainerFormatJpeg, "JPEG"}, {&GUID_ContainerFormatPng, "PNG"},
        {&GUID_ContainerFormatTiff, "TIFF"}, {&GUID_ContainerFormatBmp, "BMP"},
        {&GUID_ContainerFormatIco, "ICO"},   {&GUID_ContainerFormatWmp, "JPEG XR"},
        {&GUID_ContainerFormatDds, "DDS"},   {&GUID_ContainerFormatGif, "GIF"},
        {&GUID_ContainerFormatHeif, "HEIF"}, {&GUID_ContainerFormatWebp, "WebP"},
        {&GUID_ContainerFormatAdng, "DNG"},  {&GUID_ContainerFormatRaw, "RAW"},
    };
    for (const Name& n : names)
        if (IsEqualGUID(g, *n.guid))
            return QString::fromLatin1(n.name);
    for (const Codec& c : codecs())
        if (IsEqualGUID(g, c.container))
            return c.name;
    return QStringLiteral("WIC");
}

struct PixelInfo {
    UINT bitsPerPixel = 32;
    UINT channels = 4;
    bool alpha = false;
    bool isFloat = false;
    bool indexed = false;
    bool cmyk = false;
};

PixelInfo pixelInfo(IWICImagingFactory* f, const WICPixelFormatGUID& pf) {
    PixelInfo p;
    ComPtr<IWICComponentInfo> ci;
    ComPtr<IWICPixelFormatInfo2> info;
    if (SUCCEEDED(f->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci.As(&info))) {
        info->GetBitsPerPixel(&p.bitsPerPixel);
        info->GetChannelCount(&p.channels);
        BOOL alpha = FALSE;
        info->SupportsTransparency(&alpha);
        p.alpha = alpha != 0;
        WICPixelFormatNumericRepresentation rep = WICPixelFormatNumericRepresentationUnspecified;
        info->GetNumericRepresentation(&rep);
        p.isFloat = rep == WICPixelFormatNumericRepresentationFloat ||
                    rep == WICPixelFormatNumericRepresentationFixed;
        p.indexed = rep == WICPixelFormatNumericRepresentationIndexed;
    }
    p.cmyk = IsEqualGUID(pf, GUID_WICPixelFormat32bppCMYK) || IsEqualGUID(pf, GUID_WICPixelFormat64bppCMYK) ||
             IsEqualGUID(pf, GUID_WICPixelFormat40bppCMYKAlpha) ||
             IsEqualGUID(pf, GUID_WICPixelFormat80bppCMYKAlpha);
    return p;
}

int bitsPerComponent(const PixelInfo& p) {
    if (p.indexed || p.channels == 0)
        return 8;
    return int(p.bitsPerPixel / p.channels);
}

// The embedded color profile (ICC bytes) and the EXIF color space (1 sRGB, 2 Adobe RGB).
struct ColorInfo {
    QByteArray icc;
    UINT exifSpace = 0;
    ComPtr<IWICColorContext> iccContext;
};

ColorInfo colorInfo(IWICImagingFactory* f, IWICBitmapFrameDecode* frame) {
    ColorInfo out;
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0)
        return out;
    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count, nullptr);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(f->CreateColorContext(&contexts[i])))
            return out;
        raw[i] = contexts[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count)))
        return out;
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type = WICColorContextUninitialized;
        contexts[i]->GetType(&type);
        if (type == WICColorContextProfile && out.icc.isEmpty()) {
            UINT size = 0;
            if (SUCCEEDED(contexts[i]->GetProfileBytes(0, nullptr, &size)) && size > 0 &&
                size < 64 * 1024 * 1024) {
                QByteArray bytes(qsizetype(size), Qt::Uninitialized);
                if (SUCCEEDED(
                        contexts[i]->GetProfileBytes(size, reinterpret_cast<BYTE*>(bytes.data()), &size))) {
                    out.icc = bytes;
                    out.iccContext = contexts[i];
                }
            }
        } else if (type == WICColorContextExifColorSpace) {
            contexts[i]->GetExifColorSpace(&out.exifSpace);
        }
    }
    return out;
}

// PROPVARIANT owner.
struct Prop {
    PROPVARIANT v;
    Prop() { PropVariantInit(&v); }
    ~Prop() { PropVariantClear(&v); }
    Prop(const Prop&) = delete;
    Prop& operator=(const Prop&) = delete;
};

bool query(IWICMetadataQueryReader* q, const wchar_t* name, Prop* out) {
    return q && SUCCEEDED(q->GetMetadataByName(name, &out->v)) && out->v.vt != VT_EMPTY;
}

std::optional<double> queryDouble(IWICMetadataQueryReader* q, const wchar_t* name) {
    Prop p;
    if (!query(q, name, &p))
        return std::nullopt;
    if (p.v.vt == VT_UI8) { // a raw EXIF rational: numerator low, denominator high
        const quint32 num = quint32(p.v.uhVal.QuadPart & 0xFFFFFFFFu);
        const quint32 den = quint32(p.v.uhVal.QuadPart >> 32);
        return den ? std::optional<double>(double(num) / den) : std::nullopt;
    }
    if (p.v.vt == VT_I8) {
        const qint32 num = qint32(p.v.hVal.QuadPart & 0xFFFFFFFF);
        const qint32 den = qint32(p.v.hVal.QuadPart >> 32);
        return den ? std::optional<double>(double(num) / den) : std::nullopt;
    }
    double d = 0;
    if (SUCCEEDED(PropVariantToDouble(p.v, &d)))
        return d;
    return std::nullopt;
}

QString queryString(IWICMetadataQueryReader* q, const wchar_t* name) {
    Prop p;
    if (!query(q, name, &p))
        return {};
    if (p.v.vt == VT_LPSTR && p.v.pszVal)
        return QString::fromUtf8(p.v.pszVal).trimmed();
    PWSTR s = nullptr;
    if (FAILED(PropVariantToStringAlloc(p.v, &s)) || !s)
        return {};
    const QString out = QString::fromWCharArray(s).trimmed();
    CoTaskMemFree(s);
    return out;
}

// GPS coordinates: degrees, minutes, seconds.
std::optional<double> queryDms(IWICMetadataQueryReader* q, const wchar_t* name) {
    Prop p;
    if (!query(q, name, &p))
        return std::nullopt;
    const ULONG n = PropVariantGetElementCount(p.v);
    double parts[3] = {0, 0, 0};
    for (ULONG i = 0; i < n && i < 3; ++i)
        if (FAILED(PropVariantGetDoubleElem(p.v, i, &parts[i])))
            return std::nullopt;
    if (n == 0)
        return std::nullopt;
    return parts[0] + parts[1] / 60.0 + parts[2] / 3600.0;
}

int orientationOf(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q)))
        return 1;
    for (const wchar_t* name :
         {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        Prop p;
        if (!query(q.Get(), name, &p))
            continue;
        ULONG o = 0;
        if (SUCCEEDED(PropVariantToUInt32(p.v, &o)) && o >= 1 && o <= 8)
            return int(o);
    }
    return 1;
}

QSize orientedSize(QSize s, int o) {
    return o >= 5 ? s.transposed() : s;
}

QImage applyOrientation(QImage img, int o) {
    switch (o) {
    case 2:
        return img.flipped(Qt::Horizontal);
    case 3:
        return img.flipped(Qt::Horizontal | Qt::Vertical);
    case 4:
        return img.flipped(Qt::Vertical);
    case 5: // transpose
        return img.transformed(QTransform().rotate(90)).flipped(Qt::Horizontal);
    case 6:
        return img.transformed(QTransform().rotate(90));
    case 7: // transverse
        return img.transformed(QTransform().rotate(270)).flipped(Qt::Horizontal);
    case 8:
        return img.transformed(QTransform().rotate(270));
    default:
        return img;
    }
}

struct FrameData {
    ComPtr<IWICBitmapFrameDecode> frame;
    QSize size;
    WICPixelFormatGUID format{};
    PixelInfo pixel;
    int orientation = 1;
};

HRESULT readFrame(Source& s, UINT index, FrameData* out) {
    HRESULT hr = s.decoder->GetFrame(std::min(index, s.frameCount - 1), &out->frame);
    if (FAILED(hr))
        return hr;
    UINT w = 0, h = 0;
    hr = out->frame->GetSize(&w, &h);
    if (FAILED(hr))
        return hr;
    if (w == 0 || h == 0)
        return WINCODEC_ERR_BADIMAGE;
    out->size = QSize(int(std::min<UINT>(w, INT_MAX)), int(std::min<UINT>(h, INT_MAX)));
    out->frame->GetPixelFormat(&out->format);
    out->pixel = pixelInfo(s.factory.Get(), out->format);
    out->orientation = orientationOf(out->frame.Get());
    return S_OK;
}

ImageInfo makeInfo(const Source& s, const FrameData& f) {
    ImageInfo info;
    info.pixelSize = f.size;
    info.exifOrientation = f.orientation;
    info.displaySize = orientedSize(f.size, f.orientation);
    info.pageCount = int(s.frameCount);
    info.frameCount = 1;
    info.hasAlpha = f.pixel.alpha;
    info.isHdr = f.pixel.isFloat;
    info.bitsPerComponent = bitsPerComponent(f.pixel);
    info.colorModel = f.pixel.cmyk ? QStringLiteral("CMYK")
                                   : ((f.pixel.channels == 1 || (f.pixel.channels == 2 && f.pixel.alpha))
                                          ? QStringLiteral("Gray")
                                          : QStringLiteral("RGB"));
    info.formatName = containerName(s.container);
    return info;
}

// Longer edge of the decoded image for `req` (before orientation the same as after).
int targetLongEdge(QSize stored, const DecodeRequest& req, int bytesPerPixel) {
    const int longEdge = std::max(stored.width(), stored.height());
    int target = req.maxPixelSize > 0 ? std::min(req.maxPixelSize, longEdge) : longEdge;
    target = std::min(target, WicDecoder::kMaxFullEdge);
    const double pixels = double(stored.width()) * stored.height();
    double maxPixels = double(WicDecoder::kMaxFullPixels);
    if (req.maxBytes > 0)
        maxPixels = std::min(maxPixels, double(req.maxBytes) / bytesPerPixel);
    const double scaledPixels = pixels * (double(target) / longEdge) * (double(target) / longEdge);
    if (scaledPixels > maxPixels)
        target = std::max(1, int(std::floor(longEdge * std::sqrt(maxPixels / pixels))));
    return std::max(1, target);
}

} // namespace

// ---- Decoder interface ---------------------------------------------------------------------

bool WicDecoder::canDecode(const QString& path, const QByteArray& header) const {
    const QString suffix = QFileInfo(path).suffix().toLower();
    for (const Codec& c : codecs()) {
        if (isGif(c.container))
            continue; // animations: Qt's GIF plugin (frames composed for us)
        if (c.patterns.isEmpty()) {
            if (!suffix.isEmpty() && c.extensions.contains(suffix))
                return true; // codecs without signature (camera RAW): by extension
            continue;
        }
        for (const Pattern& p : c.patterns)
            if (matches(p, header))
                return true;
    }
    return false;
}

std::optional<ImageInfo> WicDecoder::info(const QString& path, QString* error) {
    Source s;
    HRESULT hr = open(path, &s);
    FrameData f;
    if (SUCCEEDED(hr))
        hr = readFrame(s, 0, &f);
    if (FAILED(hr)) {
        setError(error, hr);
        return std::nullopt;
    }
    ImageInfo info = makeInfo(s, f);
    const ColorInfo color = colorInfo(s.factory.Get(), f.frame.Get());
    if (!color.icc.isEmpty())
        info.profileName = QColorSpace::fromIccProfile(color.icc).description();
    else if (color.exifSpace == 2)
        info.profileName = QStringLiteral("Adobe RGB (1998)");
    return info;
}

std::optional<DecodedImage> WicDecoder::decode(const QString& path, const DecodeRequest& req,
                                               QString* error) {
    Source s;
    HRESULT hr = open(path, &s);
    FrameData f;
    const UINT index = UINT(std::max(0, req.page > 0 ? req.page : req.frame));
    if (SUCCEEDED(hr))
        hr = readFrame(s, index, &f);
    if (FAILED(hr)) {
        setError(error, hr);
        return std::nullopt;
    }
    IWICImagingFactory* fac = s.factory.Get();
    const bool hdrOut = f.pixel.isFloat && (req.hdr || req.hdrFloat);
    const bool deep = !hdrOut && (f.pixel.isFloat || bitsPerComponent(f.pixel) > 8);
    const int bytesPerPixel = hdrOut || deep ? 8 : 4;

    // Color: keep RGB pixels with their profile; convert CMYK and profiles Qt cannot use.
    const ColorInfo color = colorInfo(fac, f.frame.Get());
    QColorSpace space(QColorSpace::SRgb);
    bool transform = false;
    if (!color.icc.isEmpty()) {
        const QColorSpace embedded = QColorSpace::fromIccProfile(color.icc);
        const bool rgb = !f.pixel.cmyk && f.pixel.channels >= 3;
        if (rgb && embedded.isValid() && embedded.colorModel() == QColorSpace::ColorModel::Rgb)
            space = embedded;
        else if (!f.pixel.isFloat)
            transform = true; // CMYK / unusable profile -> sRGB by WIC
    } else if (color.exifSpace == 2) {
        space = QColorSpace(QColorSpace::AdobeRgb);
    }
    if (hdrOut)
        space = QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Linear);

    ComPtr<IWICBitmapSource> src = f.frame;
    if (transform && color.iccContext) {
        ComPtr<IWICColorContext> srgb;
        ComPtr<IWICColorTransform> ct;
        if (SUCCEEDED(fac->CreateColorContext(&srgb)) && SUCCEEDED(srgb->InitializeFromExifColorSpace(1)) &&
            SUCCEEDED(fac->CreateColorTransformer(&ct)) &&
            SUCCEEDED(ct->Initialize(src.Get(), color.iccContext.Get(), srgb.Get(),
                                     deep ? GUID_WICPixelFormat64bppRGBA : GUID_WICPixelFormat32bppBGRA)))
            src = ct;
    }

    // Size: decode-time downscaling.
    const QSize full = orientedSize(f.size, f.orientation);
    const int target = targetLongEdge(f.size, req, bytesPerPixel);
    const int longEdge = std::max(f.size.width(), f.size.height());
    QSize out = f.size;
    if (target < longEdge) {
        const double k = double(target) / longEdge;
        out = QSize(std::max(1, int(std::lround(f.size.width() * k))),
                    std::max(1, int(std::lround(f.size.height() * k))));
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(fac->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(src.Get(), UINT(out.width()), UINT(out.height()),
                                         WICBitmapInterpolationModeHighQualityCubic)))
            src = scaler;
        else
            out = f.size;
    }

    // Pixel format.
    struct Target {
        const GUID* wic;
        QImage::Format qt;
    };
    std::vector<Target> targets;
    if (hdrOut) {
        targets = {{&GUID_WICPixelFormat64bppPRGBAHalf, QImage::Format_RGBA16FPx4_Premultiplied},
                   {&GUID_WICPixelFormat128bppPRGBAFloat, QImage::Format_RGBA32FPx4_Premultiplied}};
    } else if (deep) {
        targets = {{&GUID_WICPixelFormat64bppPRGBA, QImage::Format_RGBA64_Premultiplied}};
    } else {
        targets = {{&GUID_WICPixelFormat32bppPRGBA, QImage::Format_RGBA8888_Premultiplied},
                   {&GUID_WICPixelFormat32bppPBGRA, QImage::Format_ARGB32_Premultiplied}};
    }
    QImage img;
    hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    for (const Target& t : targets) {
        ComPtr<IWICFormatConverter> conv;
        BOOL can = FALSE;
        WICPixelFormatGUID current{};
        src->GetPixelFormat(&current);
        if (FAILED(fac->CreateFormatConverter(&conv)) || FAILED(conv->CanConvert(current, *t.wic, &can)) ||
            !can)
            continue;
        hr = conv->Initialize(src.Get(), *t.wic, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
            continue;
        img = QImage(out, t.qt);
        if (img.isNull()) {
            hr = E_OUTOFMEMORY;
            break;
        }
        hr = conv->CopyPixels(nullptr, UINT(img.bytesPerLine()), UINT(img.sizeInBytes()), img.bits());
        if (SUCCEEDED(hr))
            break;
        img = QImage();
    }
    if (FAILED(hr) || img.isNull()) {
        setError(error, FAILED(hr) ? hr : E_FAIL);
        return std::nullopt;
    }
    if (img.format() == QImage::Format_ARGB32_Premultiplied)
        img = std::move(img).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (img.format() == QImage::Format_RGBA32FPx4_Premultiplied)
        img = std::move(img).convertToFormat(QImage::Format_RGBA16FPx4_Premultiplied);

    DecodedImage result;
    if (hdrOut) {
        float peak = 1.0f;
        for (int y = 0; y < img.height(); ++y) {
            const auto* px = reinterpret_cast<const qfloat16*>(img.constScanLine(y));
            for (int x = 0; x < img.width() * 4; x += 4)
                peak = std::max({peak, float(px[x]), float(px[x + 1]), float(px[x + 2])});
        }
        result.headroom = std::isfinite(peak) ? std::min(peak, 64.0f) : 1.0f;
    }
    img.setColorSpace(space);
    result.image = applyOrientation(std::move(img), f.orientation);
    result.fullSize = full;
    result.isPreview = result.image.width() < full.width() || result.image.height() < full.height();
    result.page = int(std::min(index, s.frameCount - 1));
    result.pageCount = int(s.frameCount);
    return result;
}

Metadata WicDecoder::metadata(const QString& path) {
    Metadata md;
    Source s;
    FrameData f;
    if (FAILED(open(path, &s)) || FAILED(readFrame(s, 0, &f)))
        return md;
    const ImageInfo info = makeInfo(s, f);
    addMetadata(md, QStringLiteral("file.format"), info.formatName);
    addMetadata(md, QStringLiteral("file.dimensions"),
                QStringLiteral("%1 × %2").arg(info.displaySize.width()).arg(info.displaySize.height()));
    addMetadata(md, QStringLiteral("file.colorModel"), info.colorModel);
    const ColorInfo color = colorInfo(s.factory.Get(), f.frame.Get());
    if (!color.icc.isEmpty()) {
        const QString name = QColorSpace::fromIccProfile(color.icc).description();
        if (!name.isEmpty())
            addMetadata(md, QStringLiteral("file.profile"), name);
    }
    addMetadata(md, QStringLiteral("file.bitDepth"),
                QCoreApplication::translate("lf::ImageIODecoder", "%1 Bit").arg(info.bitsPerComponent));
    addMetadata(md, QStringLiteral("file.hdr"),
                info.isHdr ? QCoreApplication::translate("lf::ImageIODecoder", "Ja")
                           : QCoreApplication::translate("lf::ImageIODecoder", "Nein"));
    if (info.hasAlpha)
        addMetadata(md, QStringLiteral("file.alpha"),
                    QCoreApplication::translate("lf::ImageIODecoder", "Ja"));
    if (info.pageCount > 1)
        addMetadata(md, QStringLiteral("file.pages"), info.pageCount);
    if (info.exifOrientation != 1)
        addMetadata(md, QStringLiteral("file.orientation"), info.exifOrientation);

    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(f.frame->GetMetadataQueryReader(&q)))
        return md;
    IWICMetadataQueryReader* r = q.Get();
    auto putStr = [&](const char* key, const wchar_t* name) {
        const QString v = queryString(r, name);
        if (!v.isEmpty())
            addMetadata(md, QString::fromLatin1(key), v);
    };
    putStr("camera.make", L"System.Photo.CameraManufacturer");
    putStr("camera.model", L"System.Photo.CameraModel");
    putStr("camera.lens", L"System.Photo.LensModel");
    if (auto fl = queryDouble(r, L"System.Photo.FocalLength"); fl && *fl > 0)
        addMetadata(md, QStringLiteral("shot.focalLength"),
                    formatFocalLength(*fl, queryDouble(r, L"System.Photo.FocalLengthInFilm").value_or(0)));
    if (auto n = queryDouble(r, L"System.Photo.FNumber"); n && *n > 0)
        addMetadata(md, QStringLiteral("shot.aperture"), formatAperture(*n));
    if (auto t = queryDouble(r, L"System.Photo.ExposureTime"); t && *t > 0)
        addMetadata(md, QStringLiteral("shot.exposureTime"), formatExposureTime(*t));
    if (auto iso = queryDouble(r, L"System.Photo.ISOSpeed"); iso && *iso > 0)
        addMetadata(md, QStringLiteral("shot.iso"), QString::number(qRound(*iso)));
    if (auto ev = queryDouble(r, L"System.Photo.ExposureBias"))
        addMetadata(md, QStringLiteral("shot.exposureBias"), formatExposureBias(*ev));
    if (auto flash = queryDouble(r, L"System.Photo.Flash"))
        addMetadata(md, QStringLiteral("shot.flash"),
                    (int(*flash) & 0x1)
                        ? QCoreApplication::translate("lf::ImageIODecoder", "Ausgelöst")
                        : QCoreApplication::translate("lf::ImageIODecoder", "Nicht ausgelöst"));

    // Capture date: the EXIF text (local time, optional offset); else Windows' DateTaken.
    std::optional<QDateTime> captured;
    for (const wchar_t* prefix : {L"/app1/ifd/exif", L"/ifd/exif"}) {
        const std::wstring p(prefix);
        const QString date = queryString(r, (p + L"/{ushort=36867}").c_str());
        if (date.isEmpty())
            continue;
        QDateTime dt = QDateTime::fromString(date, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        if (!dt.isValid())
            continue;
        const QString offset = queryString(r, (p + L"/{ushort=36881}").c_str());
        if (offset.size() >= 6 && (offset[0] == u'+' || offset[0] == u'-')) {
            const int sign = offset[0] == u'-' ? -1 : 1;
            const int secs = sign * (offset.mid(1, 2).toInt() * 3600 + offset.mid(4, 2).toInt() * 60);
            dt = QDateTime(dt.date(), dt.time(), QTimeZone::fromSecondsAheadOfUtc(secs));
        }
        captured = dt;
        break;
    }
    if (!captured) {
        Prop p;
        FILETIME ft{};
        if (query(r, L"System.Photo.DateTaken", &p) && SUCCEEDED(PropVariantToFileTime(p.v, PSTF_UTC, &ft))) {
            const qint64 ticks = (qint64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            const qint64 ms = ticks / 10000 - 11644473600000LL;
            const QDateTime local = QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toLocalTime();
            captured = QDateTime(local.date(), local.time());
        }
    }
    if (captured) {
        md.captured = captured;
        addMetadata(md, QStringLiteral("shot.captured"), formatDateTime(*captured));
    }

    const auto lat = queryDms(r, L"System.GPS.Latitude");
    const auto lon = queryDms(r, L"System.GPS.Longitude");
    if (lat && lon) {
        const double la =
            queryString(r, L"System.GPS.LatitudeRef").toUpper() == u"S" ? -std::abs(*lat) : std::abs(*lat);
        const double lo =
            queryString(r, L"System.GPS.LongitudeRef").toUpper() == u"W" ? -std::abs(*lon) : std::abs(*lon);
        md.latitude = la;
        md.longitude = lo;
        addMetadata(md, QStringLiteral("gps.latitude"), formatLatitude(la));
        addMetadata(md, QStringLiteral("gps.longitude"), formatLongitude(lo));
    }
    if (auto alt = queryDouble(r, L"System.GPS.Altitude")) {
        const bool below = queryDouble(r, L"System.GPS.AltitudeRef").value_or(0) == 1;
        addMetadata(md, QStringLiteral("gps.altitude"), formatAltitude(below ? -*alt : *alt));
    }
    if (auto rating = queryDouble(r, L"System.SimpleRating"); rating && *rating > 0) {
        md.rating = int(std::lround(*rating));
        addMetadata(md, QStringLiteral("file.rating"), md.rating);
    }
    return md;
}

QStringList WicDecoder::supportedExtensions() {
    QStringList out;
    for (const Codec& c : codecs()) {
        if (isGif(c.container))
            continue;
        for (const QString& e : c.extensions)
            if (!out.contains(e))
                out << e;
    }
    out.sort();
    return out;
}

QStringList WicDecoder::installedCodecs() {
    QStringList out;
    for (const Codec& c : codecs())
        out << QStringLiteral("%1 (%2)").arg(c.name, c.extensions.join(u' '));
    return out;
}

} // namespace lf
