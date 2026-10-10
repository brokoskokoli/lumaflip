// SPDX-License-Identifier: GPL-3.0-or-later
#include "QtImageDecoder.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace lf {
namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::QtImageDecoder)
};

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

int orientationFromTransform(QImageIOHandler::Transformations t) {
    // Inverse of qt_imageTransform's mapping (EXIF orientation -> transformation).
    switch (int(t)) {
    case QImageIOHandler::TransformationMirror:
        return 2;
    case QImageIOHandler::TransformationRotate180:
        return 3;
    case QImageIOHandler::TransformationFlip:
        return 4;
    case QImageIOHandler::TransformationFlipAndRotate90:
        return 5;
    case QImageIOHandler::TransformationRotate90:
        return 6;
    case QImageIOHandler::TransformationMirrorAndRotate90:
        return 7;
    case QImageIOHandler::TransformationRotate270:
        return 8;
    default:
        return 1;
    }
}

bool isDeep(QImage::Format f) {
    switch (f) {
    case QImage::Format_RGBX64:
    case QImage::Format_RGBA64:
    case QImage::Format_RGBA64_Premultiplied:
    case QImage::Format_Grayscale16:
    case QImage::Format_BGR30:
    case QImage::Format_A2BGR30_Premultiplied:
    case QImage::Format_RGB30:
    case QImage::Format_A2RGB30_Premultiplied:
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
        return true;
    default:
        return false;
    }
}

// Brings a decoded image into the common output format (see header).
QImage normalize(QImage img) {
    const QImage::Format target =
        isDeep(img.format()) ? QImage::Format_RGBA64_Premultiplied : QImage::Format_RGBA8888_Premultiplied;
    const QColorSpace cs = img.colorSpace();
    if (!cs.isValid()) {
        img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    } else if (cs.colorModel() == QColorSpace::ColorModel::Cmyk) {
        return img.convertedToColorSpace(QColorSpace(QColorSpace::DisplayP3), target); // F-COL-06
    } else if (cs.colorModel() == QColorSpace::ColorModel::Gray) {
        return img.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), target);
    }
    if (img.format() != target)
        img.convertTo(target);
    return img;
}

bool isSvgFormat(const QByteArray& f) {
    return f == "svg" || f == "svgz";
}

// Skips UTF-8 BOM and whitespace.
QByteArray leadingText(const QByteArray& header) {
    QByteArray h = header;
    if (h.startsWith("\xEF\xBB\xBF"))
        h.remove(0, 3);
    return h.trimmed();
}

QSize svgSize(const QSvgRenderer& r) {
    QSize s = r.defaultSize();
    if (s.isEmpty())
        s = r.viewBoxF().size().toSize();
    if (s.isEmpty())
        s = QSize(SvgDecoder::kDefaultLongEdge, SvgDecoder::kDefaultLongEdge);
    return s;
}

QSize fitLongEdge(QSize s, int longEdge) {
    const double scale = double(longEdge) / std::max(s.width(), s.height());
    return QSize(std::max(1, int(std::lround(s.width() * scale))),
                 std::max(1, int(std::lround(s.height() * scale))));
}

} // namespace

// ---- QtImageDecoder --------------------------------------------------------------------

bool QtImageDecoder::canDecode(const QString& path, const QByteArray& header) const {
    Q_UNUSED(path);
    if (header.isEmpty())
        return false;
    QByteArray copy = header;
    QBuffer buf(&copy);
    if (!buf.open(QIODevice::ReadOnly))
        return false;
    const QByteArray fmt = QImageReader::imageFormat(&buf);
#ifdef Q_OS_MACOS
    // Qt's HEIF plugin (qmacheif) only wraps ImageIO, which decodes these files in the app
    // already. In the decoder helper's sandbox (no GPU/IOKit) it returns garbage pixels.
    if (fmt == "heic" || fmt == "heif")
        return false;
#endif
    return !fmt.isEmpty() && !isSvgFormat(fmt);
}

std::optional<ImageInfo> QtImageDecoder::info(const QString& path, QString* error) {
    QImageReader r(path);
    r.setAutoTransform(false);
    if (!r.canRead()) {
        setError(error, QFileInfo(path).size() == 0
                            ? Tr::tr("Die Datei ist leer (0 Byte).")
                            : Tr::tr("Unbekanntes oder nicht unterstütztes Dateiformat."));
        return std::nullopt;
    }
    ImageInfo info;
    info.pixelSize = r.size();
    info.exifOrientation = orientationFromTransform(r.transformation());
    if (!info.pixelSize.isValid()) {
        // Some plugins only know the size after reading.
        const QImage img = r.read();
        if (img.isNull()) {
            setError(error, r.errorString());
            return std::nullopt;
        }
        info.pixelSize = img.size();
        info.hasAlpha = img.hasAlphaChannel();
        info.bitsPerComponent = isDeep(img.format()) ? 16 : 8;
    }
    info.displaySize = info.exifOrientation >= 5 ? info.pixelSize.transposed() : info.pixelSize;
    const int count = std::max(1, r.imageCount());
    info.isAnimated = r.supportsAnimation() && count > 1;
    if (info.isAnimated)
        info.frameCount = count;
    else
        info.pageCount = count;
    info.formatName = QString::fromLatin1(r.format()).toUpper();
    info.colorModel = QStringLiteral("RGB");
    return info;
}

std::optional<DecodedImage> QtImageDecoder::decode(const QString& path, const DecodeRequest& req,
                                                   QString* error) {
    QImageReader r(path);
    r.setAutoTransform(true);
    if (!r.canRead()) {
        setError(error, QFileInfo(path).size() == 0
                            ? Tr::tr("Die Datei ist leer (0 Byte).")
                            : Tr::tr("Unbekanntes oder nicht unterstütztes Dateiformat."));
        return std::nullopt;
    }
    const bool animated = r.supportsAnimation() && r.imageCount() > 1;
    const int index = animated ? req.frame : req.page;
    if (index < 0 || (index > 0 && r.imageCount() > 0 && index >= r.imageCount())) {
        setError(error, Tr::tr("Seite %1 existiert nicht.").arg(index + 1));
        return std::nullopt;
    }
    if (index > 0 && !r.jumpToImage(index)) {
        for (int i = 0; i < index; ++i) {
            if (r.read().isNull()) {
                setError(error, Tr::tr("Seite %1 existiert nicht.").arg(index + 1));
                return std::nullopt;
            }
        }
    }
    const QSize stored = r.size();
    const int orientation = orientationFromTransform(r.transformation());
    QSize fullSize = orientation >= 5 ? stored.transposed() : stored;
    int maxEdge = req.maxPixelSize;
    if (req.maxBytes > 0 && stored.isValid() && !stored.isEmpty()) {
        // P-08: byte limit of the decoded buffer (deep formats: 8 bytes per pixel).
        const qint64 bpp = isDeep(r.imageFormat()) ? 8 : 4;
        const double pixels = double(stored.width()) * double(stored.height());
        const double maxPixels = double(std::max<qint64>(1, req.maxBytes / bpp));
        if (pixels > maxPixels) {
            const int longE = std::max(stored.width(), stored.height());
            const int shortE = std::min(stored.width(), stored.height());
            int edge = std::max(1, int(std::floor(longE * std::sqrt(maxPixels / pixels))));
            while (edge > 1 && double(edge) * std::ceil(double(edge) * shortE / longE) > maxPixels)
                --edge; // the short edge is rounded up when scaling
            maxEdge = maxEdge > 0 ? std::min(maxEdge, edge) : edge;
        }
    }
    if (maxEdge > 0 && stored.isValid() && std::max(stored.width(), stored.height()) > maxEdge)
        r.setScaledSize(fitLongEdge(stored, maxEdge));
    QImage img = r.read();
    if (img.isNull()) {
        setError(error, Tr::tr("Das Bild konnte nicht dekodiert werden: %1").arg(r.errorString()));
        return std::nullopt;
    }
    DecodedImage out;
    out.fullSize = fullSize.isValid() ? fullSize : img.size();
    out.image = normalize(std::move(img));
    out.isPreview = out.image.width() < out.fullSize.width() || out.image.height() < out.fullSize.height();
    out.frameDurationMs = animated ? std::max(0, r.nextImageDelay()) : 0;
    return out;
}

Metadata QtImageDecoder::metadata(const QString& path) {
    Metadata md;
    QString err;
    const auto info = this->info(path, &err);
    if (!info)
        return md;
    addMetadata(md, QStringLiteral("file.format"), info->formatName);
    addMetadata(md, QStringLiteral("file.dimensions"),
                QStringLiteral("%1 × %2").arg(info->displaySize.width()).arg(info->displaySize.height()));
    return md;
}

QStringList QtImageDecoder::supportedExtensions() {
    QSet<QString> s;
    for (const QByteArray& f : QImageReader::supportedImageFormats())
        if (!isSvgFormat(f))
            s.insert(QString::fromLatin1(f).toLower());
    QStringList l(s.begin(), s.end());
    l.sort();
    return l;
}

// ---- SvgDecoder ----------------------------------------------------------------------

bool SvgDecoder::canDecode(const QString& path, const QByteArray& header) const {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == u"svgz")
        return header.startsWith("\x1F\x8B");
    const QByteArray h = leadingText(header);
    if (h.startsWith("<svg") || h.contains("<svg"))
        return true;
    // XML prolog / comments / doctype before the root element: trust the suffix.
    return suffix == u"svg" && (h.startsWith("<?xml") || h.startsWith("<!"));
}

std::optional<ImageInfo> SvgDecoder::info(const QString& path, QString* error) {
    QSvgRenderer r(path);
    if (!r.isValid()) {
        setError(error, Tr::tr("Die SVG-Datei ist ungültig oder beschädigt."));
        return std::nullopt;
    }
    ImageInfo info;
    info.pixelSize = info.displaySize = svgSize(r);
    info.hasAlpha = true;
    info.colorModel = QStringLiteral("RGB");
    info.formatName = QStringLiteral("SVG");
    return info;
}

std::optional<DecodedImage> SvgDecoder::decode(const QString& path, const DecodeRequest& req,
                                               QString* error) {
    QSvgRenderer r(path);
    if (!r.isValid()) {
        setError(error, Tr::tr("Die SVG-Datei ist ungültig oder beschädigt."));
        return std::nullopt;
    }
    const QSize intrinsic = svgSize(r);
    const QSize full = fitLongEdge(intrinsic, kDefaultLongEdge);
    const QSize size = req.maxPixelSize > 0 ? fitLongEdge(intrinsic, req.maxPixelSize) : full;
    QImage img(size, QImage::Format_RGBA8888_Premultiplied);
    if (img.isNull()) {
        setError(error, Tr::tr("Nicht genug Speicher für das Bild."));
        return std::nullopt;
    }
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        r.render(&p, QRectF(QPointF(0, 0), QSizeF(size)));
    }
    img.setColorSpace(QColorSpace(QColorSpace::SRgb));
    DecodedImage out;
    out.fullSize = full;
    out.isPreview = size.width() < full.width() || size.height() < full.height();
    out.image = std::move(img);
    return out;
}

Metadata SvgDecoder::metadata(const QString& path) {
    Metadata md;
    if (const auto info = this->info(path, nullptr)) {
        addMetadata(md, QStringLiteral("file.format"), info->formatName);
        addMetadata(md, QStringLiteral("file.dimensions"),
                    QStringLiteral("%1 × %2").arg(info->displaySize.width()).arg(info->displaySize.height()));
    }
    return md;
}

} // namespace lf
