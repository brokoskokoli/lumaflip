// SPDX-License-Identifier: GPL-3.0-or-later
// Portable part of exporting (F-EDIT-04, core/ImageExport.h): Qt's image writers. Metadata
// is not carried over here (Qt writes none), HDR is exported as SDR.

#include "core/ImageExport.h"

#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>

namespace lf::platform {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::ImageExport)
};

QByteArray qtFormat(const QString& id) {
    if (id == QLatin1String("jpeg"))
        return "jpeg";
    if (id == QLatin1String("png"))
        return "png";
    if (id == QLatin1String("tiff"))
        return "tiff";
    if (id == QLatin1String("heic"))
        return "heic";
    if (id == QLatin1String("avif"))
        return "avif";
    if (id == QLatin1String("jxl"))
        return "jxl";
    return {};
}

bool writeImage(QImage img, const QString& dst, const exporting::Options& o, QString* error) {
    if (o.maxLongEdge > 0 && std::max(img.width(), img.height()) > o.maxLongEdge)
        img = img.scaled(o.maxLongEdge, o.maxLongEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (o.color == exporting::ColorMode::SRgb && img.colorSpace().isValid())
        img.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    else if (o.color == exporting::ColorMode::DisplayP3 && img.colorSpace().isValid())
        img.convertToColorSpace(QColorSpace(QColorSpace::DisplayP3));
    const exporting::Format f = exporting::formatById(o.formatId);
    QImageWriter w(dst, qtFormat(o.formatId));
    if (f.lossy)
        w.setQuality(o.quality);
    if (!w.write(img)) {
        if (error)
            *error = Tr::tr("Das Bild konnte nicht geschrieben werden.");
        return false;
    }
    return true;
}

} // namespace

QStringList exportTypeIdentifiers() {
    QStringList out;
    const QList<QByteArray> formats = QImageWriter::supportedImageFormats();
    for (const exporting::Format& f : exporting::knownFormats())
        if (formats.contains(qtFormat(f.id)))
            out << f.typeId;
    return out;
}

bool exportImage(const QString& source, const QString& dst, const exporting::Options& o, QString* error,
                 QString* notice) {
    Q_UNUSED(notice);
    QImageReader r(source);
    r.setAutoTransform(true);
    const QImage img = r.read();
    if (img.isNull()) {
        if (error)
            *error = Tr::tr("Das Bild konnte nicht dekodiert werden.");
        return false;
    }
    return writeImage(img, dst, o, error);
}

bool exportDecoded(const QImage& image, const QString& dst, const exporting::Options& o, QString* error,
                   QString* notice) {
    Q_UNUSED(notice);
    return writeImage(image, dst, o, error);
}

} // namespace lf::platform
