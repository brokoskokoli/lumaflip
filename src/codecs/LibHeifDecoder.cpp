// SPDX-License-Identifier: GPL-3.0-or-later
// HEIF/HEIC and AVIF via libheif (core/LibraryDecoders.h). The decoder plugins (libde265
// for HEVC, dav1d/aom for AV1) come with the system's libheif; HEVC is optional because of
// the patent risk outside macOS (SPEC 11, docs/LINUX.md).

#include "CodecUtil.h"
#include "core/LibraryDecoders.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include <libheif/heif.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace lf {

namespace {

constexpr qint64 kMaxFileBytes = qint64(2) << 30;

} // namespace

void initLibHeif() {
    static std::once_flag once;
    std::call_once(once, [] {
        // A relocatable install (AppImage) ships its own decoder plugins next to the
        // binary; the system's plugins are built against the system's libheif.
        if (qEnvironmentVariableIsEmpty("LIBHEIF_PLUGIN_PATH") && QCoreApplication::instance()) {
            const QString dir = QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                                QStringLiteral("/../lib/libheif/plugins"));
            if (QFileInfo(dir).isDir())
                qputenv("LIBHEIF_PLUGIN_PATH", QFile::encodeName(dir));
        }
#if LIBHEIF_HAVE_VERSION(1, 13, 0)
        heif_init(nullptr);
#endif
    });
}

namespace {

struct ContextDeleter {
    void operator()(heif_context* c) const { heif_context_free(c); }
};
struct HandleDeleter {
    void operator()(heif_image_handle* h) const { heif_image_handle_release(h); }
};
struct ImageDeleter {
    void operator()(heif_image* i) const { heif_image_release(i); }
};
using Context = std::unique_ptr<heif_context, ContextDeleter>;
using Handle = std::unique_ptr<heif_image_handle, HandleDeleter>;
using Image = std::unique_ptr<heif_image, ImageDeleter>;

bool isHeifBrand(const QByteArray& b) {
    static const char* const brands[] = {"heic", "heix", "heim", "heis", "hevc", "hevx", "hevm",
                                         "hevs", "mif1", "mif2", "msf1", "avif", "avis"};
    for (const char* x : brands)
        if (b == x)
            return true;
    return false;
}

bool isAvif(const QByteArray& header) {
    return header.mid(8, 4) == "avif" || header.mid(8, 4) == "avis";
}

struct Opened {
    Context ctx;
    Handle handle;
    int pages = 1;
};

std::optional<Opened> open(const QString& path, int page, QString* error) {
    initLibHeif();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        codec::setError(error, DecoderRegistry::messageUnreadable());
        return std::nullopt;
    }
    if (f.size() <= 0 || f.size() > kMaxFileBytes) {
        codec::setError(error,
                        f.size() <= 0 ? DecoderRegistry::messageEmpty() : DecoderRegistry::messageTooLarge());
        return std::nullopt;
    }
    Opened o;
    o.ctx.reset(heif_context_alloc());
    if (!o.ctx) {
        codec::setError(error, DecoderRegistry::messageTooLarge());
        return std::nullopt;
    }
    const heif_error err =
        heif_context_read_from_file(o.ctx.get(), QFile::encodeName(path).constData(), nullptr);
    if (err.code != heif_error_Ok) {
        codec::setError(error, err.code == heif_error_Unsupported_filetype ||
                                       err.code == heif_error_Unsupported_feature
                                   ? DecoderRegistry::messageUnsupported()
                                   : DecoderRegistry::messageDamaged());
        return std::nullopt;
    }
    const int n = heif_context_get_number_of_top_level_images(o.ctx.get());
    if (n <= 0 || page < 0 || page >= n) {
        codec::setError(error, DecoderRegistry::messageDamaged());
        return std::nullopt;
    }
    o.pages = n;
    heif_image_handle* h = nullptr;
    heif_error e2{};
    if (page == 0) {
        e2 = heif_context_get_primary_image_handle(o.ctx.get(), &h);
    } else {
        std::vector<heif_item_id> ids(static_cast<size_t>(n));
        heif_context_get_list_of_top_level_image_IDs(o.ctx.get(), ids.data(), n);
        e2 = heif_context_get_image_handle(o.ctx.get(), ids[size_t(page)], &h);
    }
    if (e2.code != heif_error_Ok || !h) {
        codec::setError(error, DecoderRegistry::messageDamaged());
        return std::nullopt;
    }
    o.handle.reset(h);
    return o;
}

QColorSpace nclxColorSpace(const heif_color_profile_nclx* nclx, bool* hdrTransfer) {
    using P = QColorSpace::Primaries;
    using T = QColorSpace::TransferFunction;
    *hdrTransfer = false;
    P primaries = P::SRgb;
    switch (nclx->color_primaries) {
    case heif_color_primaries_ITU_R_BT_709_5:
    case heif_color_primaries_unspecified:
        primaries = P::SRgb;
        break;
    case heif_color_primaries_SMPTE_EG_432_1:
        primaries = P::DciP3D65;
        break;
    case heif_color_primaries_ITU_R_BT_2020_2_and_2100_0:
        primaries = P::Bt2020;
        break;
    default:
        return {};
    }
    switch (nclx->transfer_characteristics) {
    case heif_transfer_characteristic_IEC_61966_2_1:
    case heif_transfer_characteristic_unspecified:
        return QColorSpace(primaries, T::SRgb);
    case heif_transfer_characteristic_linear:
        return QColorSpace(primaries, T::Linear);
    case heif_transfer_characteristic_ITU_R_BT_709_5:
    case heif_transfer_characteristic_ITU_R_BT_601_6:
    case heif_transfer_characteristic_ITU_R_BT_2020_2_10bit:
    case heif_transfer_characteristic_ITU_R_BT_2020_2_12bit:
        return QColorSpace(primaries, T::Bt2020);
    case heif_transfer_characteristic_ITU_R_BT_2100_0_PQ:
        *hdrTransfer = true;
        return QColorSpace(primaries, T::St2084);
    case heif_transfer_characteristic_ITU_R_BT_2100_0_HLG:
        *hdrTransfer = true;
        return QColorSpace(primaries, T::Hlg);
    default:
        return QColorSpace(primaries, T::SRgb);
    }
}

// The color space of the decoded pixels: an ICC profile or nclx (CICP) values.
QColorSpace colorSpaceOf(heif_image_handle* h, bool* hdrTransfer, QString* profileName) {
    *hdrTransfer = false;
    const heif_color_profile_type type = heif_image_handle_get_color_profile_type(h);
    if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC) {
        const size_t n = heif_image_handle_get_raw_color_profile_size(h);
        QByteArray icc(qsizetype(n), Qt::Uninitialized);
        if (n > 0 && heif_image_handle_get_raw_color_profile(h, icc.data()).code == heif_error_Ok) {
            QColorSpace cs = QColorSpace::fromIccProfile(icc);
            if (cs.isValid()) {
                *profileName = cs.description();
                return cs;
            }
        }
    }
    heif_color_profile_nclx* nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(h, &nclx).code == heif_error_Ok && nclx) {
        QColorSpace cs = nclxColorSpace(nclx, hdrTransfer);
        heif_nclx_color_profile_free(nclx);
        if (cs.isValid())
            return cs;
    }
    return QColorSpace(QColorSpace::SRgb);
}

class LibHeifDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("libheif"); }
    int priority() const override { return 60; }

    bool canDecode(const QString&, const QByteArray& header) const override {
        if (header.size() < 16 || header.mid(4, 4) != "ftyp")
            return false;
        if (isHeifBrand(header.mid(8, 4)))
            return true;
        // Compatible brands up to the end of the ftyp box (within the header).
        quint32 boxSize = (quint32(uchar(header[0])) << 24) | (quint32(uchar(header[1])) << 16) |
                          (quint32(uchar(header[2])) << 8) | quint32(uchar(header[3]));
        const qsizetype end = std::min<qsizetype>(header.size(), boxSize);
        for (qsizetype i = 16; i + 4 <= end; i += 4)
            if (isHeifBrand(header.mid(i, 4)))
                return true;
        return false;
    }

    std::optional<ImageInfo> info(const QString& path, QString* error) override {
        auto o = open(path, 0, error);
        if (!o)
            return std::nullopt;
        ImageInfo i;
        heif_image_handle* h = o->handle.get();
        i.displaySize = QSize(heif_image_handle_get_width(h), heif_image_handle_get_height(h));
        i.pixelSize = i.displaySize;
#if LIBHEIF_HAVE_VERSION(1, 4, 0)
        const int iw = heif_image_handle_get_ispe_width(h), ih = heif_image_handle_get_ispe_height(h);
        if (iw > 0 && ih > 0)
            i.pixelSize = QSize(iw, ih);
#endif
        i.hasAlpha = heif_image_handle_has_alpha_channel(h);
        i.bitsPerComponent = std::max(8, heif_image_handle_get_luma_bits_per_pixel(h));
        i.pageCount = o->pages;
        bool hdr = false;
        colorSpaceOf(h, &hdr, &i.profileName);
        i.isHdr = hdr;
        i.colorModel = QStringLiteral("RGB");
        QFile f(path);
        const QByteArray header = f.open(QIODevice::ReadOnly) ? f.read(64) : QByteArray();
        i.formatName = isAvif(header) ? QStringLiteral("AVIF") : QStringLiteral("HEIF");
        return i;
    }

    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override {
        if (req.frame > 0) {
            codec::setError(error, DecoderRegistry::messageUnsupported());
            return std::nullopt;
        }
        auto o = open(path, req.page, error);
        if (!o)
            return std::nullopt;
        heif_image_handle* h = o->handle.get();
        const QSize full(heif_image_handle_get_width(h), heif_image_handle_get_height(h));
        // A stored thumbnail is enough for the preview stage when it covers the target edge.
        Handle thumb;
        if (req.preferEmbeddedPreview && req.maxPixelSize > 0) {
            heif_item_id id = 0;
            if (heif_image_handle_get_list_of_thumbnail_IDs(h, &id, 1) == 1) {
                heif_image_handle* t = nullptr;
                if (heif_image_handle_get_thumbnail(h, id, &t).code == heif_error_Ok && t) {
                    thumb.reset(t);
                    if (std::max(heif_image_handle_get_width(t), heif_image_handle_get_height(t)) <
                        req.maxPixelSize)
                        thumb.reset();
                }
            }
        }
        heif_image_handle* src = thumb ? thumb.get() : h;
        const bool deep = heif_image_handle_get_luma_bits_per_pixel(h) > 8;
        const heif_chroma chroma = deep ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA;
        heif_image* raw = nullptr;
        const heif_error err = heif_decode_image(src, &raw, heif_colorspace_RGB, chroma, nullptr);
        Image img(raw);
        if (err.code != heif_error_Ok || !img) {
            codec::setError(error, err.code == heif_error_Unsupported_feature ||
                                           err.code == heif_error_Unsupported_filetype ||
                                           err.code == heif_error_Plugin_loading_error
                                       ? DecoderRegistry::messageUnsupported()
                                       : DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
        const int w = heif_image_get_width(img.get(), heif_channel_interleaved);
        const int hgt = heif_image_get_height(img.get(), heif_channel_interleaved);
        int stride = 0;
        const uint8_t* data = heif_image_get_plane_readonly(img.get(), heif_channel_interleaved, &stride);
        if (!data || w <= 0 || hgt <= 0) {
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
        QImage out(w, hgt, deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (out.isNull()) {
            codec::setError(error, DecoderRegistry::messageTooLarge());
            return std::nullopt;
        }
        if (deep) {
            const int bits =
                std::clamp(heif_image_get_bits_per_pixel_range(img.get(), heif_channel_interleaved), 9, 16);
            for (int y = 0; y < hgt; ++y) {
                const auto* s = reinterpret_cast<const uint8_t*>(data + qsizetype(y) * stride);
                auto* d = reinterpret_cast<quint16*>(out.scanLine(y));
                for (int x = 0; x < w * 4; ++x) {
                    const quint32 v = quint32(s[2 * x]) | (quint32(s[2 * x + 1]) << 8);
                    // Scale bits-wide values to 16 bits (replicate the top bits).
                    d[x] = quint16(std::min<quint32>(65535, (v << (16 - bits)) | (v >> (2 * bits - 16))));
                }
            }
        } else {
            for (int y = 0; y < hgt; ++y)
                std::memcpy(out.scanLine(y), data + qsizetype(y) * stride, size_t(w) * 4);
        }
        bool hdrTransfer = false;
        QString profile;
        out.setColorSpace(colorSpaceOf(h, &hdrTransfer, &profile));
        if (hdrTransfer) {
            // PQ/HLG: to linear float; finish() keeps the extended range when the request asks.
            out.convertTo(QImage::Format_RGBA32FPx4);
            out.convertToColorSpace(
                QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear));
        }
        auto res = codec::finish(std::move(out), full, req, error, bool(thumb));
        if (res) {
            res->page = req.page;
            res->pageCount = o->pages;
        }
        return res;
    }

    Metadata metadata(const QString& path) override {
        Metadata md;
        if (const auto i = info(path, nullptr))
            codec::addFileMetadata(md, *i);
        return md;
    }
};

} // namespace

QString libHeifVersion() {
    return QStringLiteral("libheif ") + QString::fromLatin1(heif_get_version());
}

std::shared_ptr<Decoder> createLibHeifDecoder() {
    return std::make_shared<LibHeifDecoder>();
}

} // namespace lf
