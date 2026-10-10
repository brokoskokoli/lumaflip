// SPDX-License-Identifier: GPL-3.0-or-later
// Camera RAW via LibRaw (core/LibraryDecoders.h; LibRaw under its CDDL-1.0 option, SPEC 11).
// The preview stages use the embedded JPEG preview when it is large enough (F-FMT-05);
// the full stage develops the sensor data (camera white balance, sRGB, 16 bit; half size
// when the target edge allows it).

#include "CodecUtil.h"
#include "core/ImageFormats.h"
#include "core/LibraryDecoders.h"
#include "core/LocaleFormat.h"

#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QImageReader>
#include <QTimeZone>

#include <libraw/libraw.h>

#include <cstring>
#include <memory>

namespace lf {

namespace {

// LibRaw's flip (sizes.flip) as an EXIF orientation.
int orientationOf(int flip) {
    switch (flip) {
    case 3:
        return 3;
    case 5:
        return 8;
    case 6:
        return 6;
    default:
        return 1;
    }
}

struct Raw {
    std::unique_ptr<LibRaw> raw = std::make_unique<LibRaw>(LIBRAW_OPTIONS_NONE);
};

bool openRaw(Raw& r, const QString& path, QString* error) {
    const int rc = r.raw->open_file(QFile::encodeName(path).constData());
    if (rc != LIBRAW_SUCCESS) {
        codec::setError(error, rc == LIBRAW_FILE_UNSUPPORTED ? DecoderRegistry::messageUnsupported()
                               : rc == LIBRAW_IO_ERROR       ? DecoderRegistry::messageUnreadable()
                                                             : DecoderRegistry::messageDamaged());
        return false;
    }
    return true;
}

// The embedded preview, oriented; null if there is none Qt can read.
QImage embeddedPreview(Raw& r) {
    if (r.raw->unpack_thumb() != LIBRAW_SUCCESS)
        return {};
    int rc = 0;
    libraw_processed_image_t* t = r.raw->dcraw_make_mem_thumb(&rc);
    if (!t)
        return {};
    QImage img;
    if (t->type == LIBRAW_IMAGE_JPEG) {
        QByteArray bytes =
            QByteArray::fromRawData(reinterpret_cast<const char*>(t->data), qsizetype(t->data_size));
        QBuffer buf(&bytes);
        QImageReader reader(&buf, "jpeg");
        reader.setAutoTransform(false);
        img = reader.read();
    } else if (t->type == LIBRAW_IMAGE_BITMAP && t->colors == 3 && t->bits == 8) {
        img = QImage(t->width, t->height, QImage::Format_RGB888);
        if (!img.isNull())
            for (int y = 0; y < t->height; ++y)
                std::memcpy(img.scanLine(y), t->data + qsizetype(y) * t->width * 3, size_t(t->width) * 3);
    }
    LibRaw::dcraw_clear_mem(t);
    if (img.isNull())
        return {};
    return codec::oriented(img, orientationOf(r.raw->imgdata.sizes.flip));
}

QString cameraName(const libraw_iparams_t& p) {
    return QStringLiteral("%1 %2").arg(QString::fromLatin1(p.make), QString::fromLatin1(p.model)).trimmed();
}

class LibRawDecoder final : public Decoder {
public:
    QString name() const override { return QStringLiteral("LibRaw"); }
    int priority() const override { return 50; }

    bool canDecode(const QString& path, const QByteArray& header) const override {
        // No common signature: RAW files are TIFF-like or vendor formats; LibRaw decides.
        if (!isRawSuffix(codec::suffixOf(path)))
            return false;
        return header.size() >= 8;
    }

    std::optional<ImageInfo> info(const QString& path, QString* error) override {
        Raw r;
        if (!openRaw(r, path, error))
            return std::nullopt;
        const auto& s = r.raw->imgdata.sizes;
        ImageInfo i;
        i.pixelSize = QSize(s.width, s.height);
        i.exifOrientation = orientationOf(s.flip);
        i.displaySize = codec::orientedSize(i.pixelSize, i.exifOrientation);
        i.bitsPerComponent = 16;
        i.colorModel = QStringLiteral("RGB");
        const QString camera = cameraName(r.raw->imgdata.idata);
        i.formatName = camera.isEmpty() ? QStringLiteral("RAW") : QStringLiteral("RAW (%1)").arg(camera);
        if (i.pixelSize.isEmpty()) {
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
        return i;
    }

    std::optional<DecodedImage> decode(const QString& path, const DecodeRequest& req,
                                       QString* error) override {
        if (req.page > 0 || req.frame > 0) {
            codec::setError(error, DecoderRegistry::messageUnsupported());
            return std::nullopt;
        }
        Raw r;
        if (!openRaw(r, path, error))
            return std::nullopt;
        const auto& s = r.raw->imgdata.sizes;
        const QSize full = codec::orientedSize(QSize(s.width, s.height), orientationOf(s.flip));
        if (req.preferEmbeddedPreview || req.maxPixelSize > 0) {
            QImage preview = embeddedPreview(r);
            const int edge = req.maxPixelSize > 0 ? req.maxPixelSize : 1024;
            if (!preview.isNull() &&
                (req.preferEmbeddedPreview || std::max(preview.width(), preview.height()) >= edge)) {
                preview.setColorSpace(QColorSpace(QColorSpace::SRgb));
                return codec::finish(std::move(preview), full, req, error, true);
            }
        }
        auto& p = r.raw->imgdata.params;
        p.use_camera_wb = 1;
        p.output_color = 1; // sRGB
        p.output_bps = 16;
        p.gamm[0] = 1.0 / 2.4; // sRGB curve
        p.gamm[1] = 12.92;
        p.no_auto_bright = 0;
        const int target = decutil::targetEdge(full, 8, req);
        p.half_size = target > 0 && target * 2 <= std::max(full.width(), full.height()) ? 1 : 0;
        if (r.raw->unpack() != LIBRAW_SUCCESS || r.raw->dcraw_process() != LIBRAW_SUCCESS) {
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
        int rc = 0;
        libraw_processed_image_t* img = r.raw->dcraw_make_mem_image(&rc);
        if (!img || img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3 ||
            (img->bits != 16 && img->bits != 8)) {
            if (img)
                LibRaw::dcraw_clear_mem(img);
            codec::setError(error, DecoderRegistry::messageDamaged());
            return std::nullopt;
        }
        QImage out(img->width, img->height,
                   img->bits == 16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (!out.isNull()) {
            for (int y = 0; y < img->height; ++y) {
                if (img->bits == 16) {
                    const auto* src =
                        reinterpret_cast<const quint16*>(img->data) + qsizetype(y) * img->width * 3;
                    auto* dst = reinterpret_cast<quint16*>(out.scanLine(y));
                    for (int x = 0; x < img->width; ++x) {
                        dst[4 * x] = src[3 * x];
                        dst[4 * x + 1] = src[3 * x + 1];
                        dst[4 * x + 2] = src[3 * x + 2];
                        dst[4 * x + 3] = 0xFFFF;
                    }
                } else {
                    const uchar* src = img->data + qsizetype(y) * img->width * 3;
                    uchar* dst = out.scanLine(y);
                    for (int x = 0; x < img->width; ++x) {
                        dst[4 * x] = src[3 * x];
                        dst[4 * x + 1] = src[3 * x + 1];
                        dst[4 * x + 2] = src[3 * x + 2];
                        dst[4 * x + 3] = 0xFF;
                    }
                }
            }
        }
        LibRaw::dcraw_clear_mem(img);
        out.setColorSpace(QColorSpace(QColorSpace::SRgb));
        return codec::finish(std::move(out), full, req, error, p.half_size != 0);
    }

    Metadata metadata(const QString& path) override {
        Metadata md;
        Raw r;
        if (!openRaw(r, path, nullptr))
            return md;
        if (const auto i = info(path, nullptr))
            codec::addFileMetadata(md, *i);
        const auto& id = r.raw->imgdata.idata;
        const auto& o = r.raw->imgdata.other;
        if (id.make[0])
            addMetadata(md, QStringLiteral("camera.make"), QString::fromLatin1(id.make));
        if (id.model[0])
            addMetadata(md, QStringLiteral("camera.model"), QString::fromLatin1(id.model));
        if (r.raw->imgdata.lens.Lens[0])
            addMetadata(md, QStringLiteral("camera.lens"), QString::fromLatin1(r.raw->imgdata.lens.Lens));
        if (o.focal_len > 0)
            addMetadata(md, QStringLiteral("shot.focalLength"), formatFocalLength(o.focal_len, 0));
        if (o.aperture > 0)
            addMetadata(md, QStringLiteral("shot.aperture"), formatAperture(o.aperture));
        if (o.shutter > 0)
            addMetadata(md, QStringLiteral("shot.exposureTime"), formatExposureTime(o.shutter));
        if (o.iso_speed > 0)
            addMetadata(md, QStringLiteral("shot.iso"), QString::number(qRound(o.iso_speed)));
        if (o.timestamp > 0) {
            // LibRaw converts the EXIF local time with mktime(): back to the wall clock time.
            const QDateTime t = QDateTime::fromSecsSinceEpoch(qint64(o.timestamp));
            md.captured = t;
            addMetadata(md, QStringLiteral("shot.captured"), formatDateTime(t));
        }
        if (o.parsed_gps.gpsparsed) {
            auto deg = [](const float v[3]) {
                return double(v[0]) + double(v[1]) / 60.0 + double(v[2]) / 3600.0;
            };
            double la = deg(o.parsed_gps.latitude), lo = deg(o.parsed_gps.longitude);
            if (o.parsed_gps.latref == 'S')
                la = -la;
            if (o.parsed_gps.longref == 'W')
                lo = -lo;
            if (la != 0.0 || lo != 0.0) {
                md.latitude = la;
                md.longitude = lo;
            }
        }
        return md;
    }
};

} // namespace

QString libRawVersion() {
    return QStringLiteral("LibRaw ") + QString::fromLatin1(LibRaw::version());
}

std::shared_ptr<Decoder> createLibRawDecoder() {
    return std::make_shared<LibRawDecoder>();
}

} // namespace lf
