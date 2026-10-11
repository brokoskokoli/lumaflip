// SPDX-License-Identifier: GPL-3.0-or-later
// JPEG XL via libjxl (core/LibraryDecoders.h). Works with libjxl 0.7 (Ubuntu 24.04) and
// newer (0.9 dropped the pixel format argument of the color profile calls).

#include "CodecUtil.h"
#include "core/LibraryDecoders.h"

#include <QFile>

#include <jxl/decode.h>
#if __has_include(<jxl/resizable_parallel_runner.h>) && LF_HAVE_LIBJXL_THREADS
#include <jxl/resizable_parallel_runner.h>
#define LF_JXL_THREADS 1
#else
#define LF_JXL_THREADS 0
#endif
#if __has_include(<jxl/version.h>)
#include <jxl/version.h>
#endif

#include <cstring>
#include <memory>
#include <vector>

#if defined(JPEGXL_MAJOR_VERSION) && (JPEGXL_MAJOR_VERSION > 0 || JPEGXL_MINOR_VERSION >= 9)
#define LF_JXL_PROFILE_FMT(fmt) /* no pixel format argument since 0.9 */
#else
#define LF_JXL_PROFILE_FMT(fmt) fmt,
#endif

namespace lf {

namespace {

constexpr qint64 kMaxFileBytes = qint64(2) << 30;

struct DecoderDeleter {
    void operator()(JxlDecoder* d) const { JxlDecoderDestroy(d); }
};
#if LF_JXL_THREADS
struct RunnerDeleter {
    void operator()(void* r) const { JxlResizableParallelRunnerDestroy(r); }
};
#endif

QColorSpace fromEncoding(const JxlColorEncoding& e, bool* hdr) {
    using P = QColorSpace::Primaries;
    using T = QColorSpace::TransferFunction;
    *hdr = false;
    if (e.color_space != JXL_COLOR_SPACE_RGB && e.color_space != JXL_COLOR_SPACE_GRAY)
        return {};
    if (e.white_point != JXL_WHITE_POINT_D65)
        return {};
    P p = P::SRgb;
    switch (e.primaries) {
    case JXL_PRIMARIES_SRGB:
        p = P::SRgb;
        break;
    case JXL_PRIMARIES_P3:
        p = P::DciP3D65;
        break;
    case JXL_PRIMARIES_2100:
        p = P::Bt2020;
        break;
    default:
        return {};
    }
    switch (e.transfer_function) {
    case JXL_TRANSFER_FUNCTION_SRGB:
        return QColorSpace(p, T::SRgb);
    case JXL_TRANSFER_FUNCTION_LINEAR:
        return QColorSpace(p, T::Linear);
    case JXL_TRANSFER_FUNCTION_709:
        return QColorSpace(p, T::Bt2020);
    case JXL_TRANSFER_FUNCTION_PQ:
        *hdr = true;
        return QColorSpace(p, T::St2084);
    case JXL_TRANSFER_FUNCTION_HLG:
        *hdr = true;
        return QColorSpace(p, T::Hlg);
    case JXL_TRANSFER_FUNCTION_GAMMA:
        return QColorSpace(p, T::Gamma, float(1.0 / e.gamma));
    default:
        return {};
    }
}

struct Result {
    JxlBasicInfo basic{};
    QImage image;
    // From JXL_DEC_COLOR_ENCODING, which comes before the pixel buffer exists: applied to the
    // image once it is allocated (a null QImage keeps no color space).
    QColorSpace colorSpace;
    QString profileName;
    bool hdrTransfer = false;
};

// Decodes the basic info (and with `pixels` the first frame) of an in-memory file.
std::optional<Result> run(const QByteArray& data, bool pixels, QString* error) {
    std::unique_ptr<JxlDecoder, DecoderDeleter> dec(JxlDecoderCreate(nullptr));
    if (!dec) {
        codec::setError(error, DecoderRegistry::messageTooLarge());
        return std::nullopt;
    }
#if LF_JXL_THREADS
    std::unique_ptr<void, RunnerDeleter> runner(JxlResizableParallelRunnerCreate(nullptr));
    if (pixels && runner)
        JxlDecoderSetParallelRunner(dec.get(), JxlResizableParallelRunner, runner.get());
#endif
    int events = JXL_DEC_BASIC_INFO;
    if (pixels)
        events |= JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE;
    if (JxlDecoderSubscribeEvents(dec.get(), events) != JXL_DEC_SUCCESS) {
        codec::setError(error, DecoderRegistry::messageDamaged());
        return std::nullopt;
    }
    JxlDecoderSetInput(dec.get(), reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()));
    JxlDecoderCloseInput(dec.get());
    Result r;
    JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    for (;;) {
        const JxlDecoderStatus s = JxlDecoderProcessInput(dec.get());
        switch (s) {
        case JXL_DEC_BASIC_INFO: {
            if (JxlDecoderGetBasicInfo(dec.get(), &r.basic) != JXL_DEC_SUCCESS) {
                codec::setError(error, DecoderRegistry::messageDamaged());
                return std::nullopt;
            }
            // The largest buffer below: 8 bytes per pixel (16-bit or half float RGBA).
            if (!codec::fitsLimits(r.basic.xsize, r.basic.ysize, 8)) {
                codec::setError(error, r.basic.xsize == 0 || r.basic.ysize == 0
                                           ? DecoderRegistry::messageDamaged()
                                           : DecoderRegistry::messageTooLarge());
                return std::nullopt;
            }
            if (!pixels)
                return r;
#if LF_JXL_THREADS
            if (runner)
                JxlResizableParallelRunnerSetThreads(
                    runner.get(), JxlResizableParallelRunnerSuggestThreads(r.basic.xsize, r.basic.ysize));
#endif
            if (r.basic.exponent_bits_per_sample > 0)
                format.data_type = JXL_TYPE_FLOAT16; // half the memory of float, enough range
            else if (r.basic.bits_per_sample > 8)
                format.data_type = JXL_TYPE_UINT16;
            break;
        }
        case JXL_DEC_COLOR_ENCODING: {
            JxlColorEncoding enc{};
            QColorSpace cs;
            if (JxlDecoderGetColorAsEncodedProfile(dec.get(),
                                                   LF_JXL_PROFILE_FMT(&format) JXL_COLOR_PROFILE_TARGET_DATA,
                                                   &enc) == JXL_DEC_SUCCESS)
                cs = fromEncoding(enc, &r.hdrTransfer);
            if (!cs.isValid()) {
                size_t n = 0;
                if (JxlDecoderGetICCProfileSize(dec.get(),
                                                LF_JXL_PROFILE_FMT(&format) JXL_COLOR_PROFILE_TARGET_DATA,
                                                &n) == JXL_DEC_SUCCESS &&
                    n > 0 && n < (64u << 20)) {
                    QByteArray icc(qsizetype(n), Qt::Uninitialized);
                    if (JxlDecoderGetColorAsICCProfile(
                            dec.get(), LF_JXL_PROFILE_FMT(&format) JXL_COLOR_PROFILE_TARGET_DATA,
                            reinterpret_cast<uint8_t*>(icc.data()), n) == JXL_DEC_SUCCESS) {
                        cs = QColorSpace::fromIccProfile(icc);
                        if (cs.isValid())
                            r.profileName = cs.description();
                    }
                }
            }
            r.colorSpace = cs.isValid() ? cs : QColorSpace(QColorSpace::SRgb);
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            const QImage::Format qf = format.data_type == JXL_TYPE_FLOAT16  ? QImage::Format_RGBA16FPx4
                                      : format.data_type == JXL_TYPE_UINT16 ? QImage::Format_RGBA64
                                                                            : QImage::Format_RGBA8888;
            size_t need = 0;
            if (JxlDecoderImageOutBufferSize(dec.get(), &format, &need) != JXL_DEC_SUCCESS) {
                codec::setError(error, DecoderRegistry::messageDamaged());
                return std::nullopt;
            }
            // The output is oriented: the buffer has the oriented size.
            const bool swap = r.basic.orientation >= JXL_ORIENT_TRANSPOSE;
            const int w = int(swap ? r.basic.ysize : r.basic.xsize),
                      h = int(swap ? r.basic.xsize : r.basic.ysize);
            r.image = QImage(w, h, qf);
            if (r.image.isNull() || size_t(r.image.sizeInBytes()) < need ||
                size_t(r.image.bytesPerLine()) * size_t(h) != need) {
                codec::setError(error, DecoderRegistry::messageTooLarge());
                return std::nullopt;
            }
            r.image.setColorSpace(r.colorSpace.isValid() ? r.colorSpace : QColorSpace(QColorSpace::SRgb));
            if (JxlDecoderSetImageOutBuffer(dec.get(), &format, r.image.bits(), need) != JXL_DEC_SUCCESS) {
                codec::setError(error, DecoderRegistry::messageDamaged());
                return std::nullopt;
            }
            break;
        }
        case JXL_DEC_FULL_IMAGE:
            return r; // the first frame is enough (animations: still image)
        case JXL_DEC_SUCCESS:
            if (!pixels)
                return r;
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        case JXL_DEC_NEED_MORE_INPUT:
        case JXL_DEC_ERROR:
        default:
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
    }
}

bool readAll(const QString& path, QFile& file, QByteArray& data, QString* error) {
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly)) {
        codec::setError(error, DecoderRegistry::messageUnreadable());
        return false;
    }
    const qint64 size = file.size();
    if (size <= 0 || size > kMaxFileBytes) {
        codec::setError(error,
                        size <= 0 ? DecoderRegistry::messageEmpty() : DecoderRegistry::messageTooLarge());
        return false;
    }
    if (uchar* m = file.map(0, size)) {
        data = QByteArray::fromRawData(reinterpret_cast<const char*>(m), size);
        return true;
    }
    data = file.readAll();
    if (data.size() != size) {
        codec::setError(error, DecoderRegistry::messageUnreadable());
        return false;
    }
    return true;
}

class LibJxlDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("libjxl"); }
    int priority() const override { return 60; }

    bool canDecode(const QString&, const QByteArray& header) const override {
        const JxlSignature s =
            JxlSignatureCheck(reinterpret_cast<const uint8_t*>(header.constData()), size_t(header.size()));
        return s == JXL_SIG_CODESTREAM || s == JXL_SIG_CONTAINER;
    }

    std::optional<ImageInfo> info(const QString& path, QString* error) override {
        QFile f;
        QByteArray data;
        if (!readAll(path, f, data, error))
            return std::nullopt;
        const auto r = run(data, false, error);
        if (!r)
            return std::nullopt;
        ImageInfo i;
        i.pixelSize = QSize(int(r->basic.xsize), int(r->basic.ysize));
        i.exifOrientation = int(r->basic.orientation);
        i.displaySize = codec::orientedSize(i.pixelSize, i.exifOrientation);
        i.hasAlpha = r->basic.alpha_bits > 0;
        i.bitsPerComponent = int(r->basic.bits_per_sample);
        i.isHdr = r->basic.exponent_bits_per_sample > 0 || r->basic.intensity_target > 255.f;
        i.isAnimated = r->basic.have_animation;
        i.colorModel = r->basic.num_color_channels == 1 ? QStringLiteral("Gray") : QStringLiteral("RGB");
        i.formatName = QStringLiteral("JPEG XL");
        return i;
    }

    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override {
        if (req.page > 0 || req.frame > 0) {
            codec::setError(error, DecoderRegistry::messageUnsupported());
            return std::nullopt;
        }
        QFile f;
        QByteArray data;
        if (!readAll(path, f, data, error))
            return std::nullopt;
        auto r = run(data, true, error);
        if (!r)
            return std::nullopt;
        QImage img = std::move(r->image);
        r->image = QImage();
        const QSize full = img.size();
        if (r->hdrTransfer) {
            // PQ/HLG: to linear float (smaller first, finish() keeps the extended range).
            codec::prescale(img, req, 8);
            if (!codec::fitsLimits(img.width(), img.height(), 16)) {
                codec::setError(error, DecoderRegistry::messageTooLarge());
                return std::nullopt;
            }
            img.convertTo(QImage::Format_RGBA32FPx4);
            img.convertToColorSpace(
                QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear));
        }
        return codec::finish(std::move(img), full, req, error);
    }

    Metadata metadata(const QString& path) override {
        Metadata md;
        if (const auto i = info(path, nullptr))
            codec::addFileMetadata(md, *i);
        return md;
    }
};

} // namespace

QString libJxlVersion() {
    const uint32_t v = JxlDecoderVersion();
    return QStringLiteral("libjxl %1.%2.%3").arg(v / 1000000).arg((v / 1000) % 1000).arg(v % 1000);
}

std::shared_ptr<Decoder> createLibJxlDecoder() {
    return std::make_shared<LibJxlDecoder>();
}

} // namespace lf
