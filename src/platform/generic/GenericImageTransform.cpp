// SPDX-License-Identifier: GPL-3.0-or-later
// Portable part of saving the rotation (F-EDIT-02, see core/LosslessTransform.h): JPEG
// files with an orientation tag (patched by the core) and lossless formats re-encoded with
// Qt. A lossless metadata copy for other formats is not available here.

#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/LosslessTransform.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTransform>

#include <filesystem>
#include <system_error>

namespace lf::platform {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::LosslessTransform", text);
}

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

QString formatOf(const QString& path) {
    QImageReader r(path);
    return QString::fromLatin1(r.format()).toLower();
}

} // namespace

QString imageTypeIdentifier(const QString& path) {
    const QString f = formatOf(path);
    if (f == QLatin1String("jpeg") || f == QLatin1String("jpg"))
        return QStringLiteral("public.jpeg");
    if (f == QLatin1String("png"))
        return QStringLiteral("public.png");
    if (f == QLatin1String("tiff") || f == QLatin1String("tif"))
        return QStringLiteral("public.tiff");
    if (f == QLatin1String("bmp"))
        return QStringLiteral("com.microsoft.bmp");
    if (isRawSuffix(QFileInfo(path).suffix().toLower()))
        return QStringLiteral("public.camera-raw-image");
    return f.isEmpty() ? QString() : QStringLiteral("generic.") + f;
}

bool isCameraRawType(const QString& typeId) {
    return typeId == QLatin1String("public.camera-raw-image");
}

int imageCount(const QString& path) {
    QImageReader r(path);
    return r.canRead() ? std::max(1, r.imageCount()) : 0;
}

bool supportsOrientationMetadata(const QString&) {
    return false; // only the in-place JPEG patch (core) is available
}

bool isLosslessCodec(const QString& typeId) {
    return typeId == QLatin1String("public.png") || typeId == QLatin1String("com.microsoft.bmp");
}

bool writeOrientationLossless(const QString&, const QString&, int, QString* error) {
    setError(error, tr("Für dieses Format kann die Ausrichtung nicht gespeichert werden."));
    return false;
}

bool reencodeTransformed(const QString& src, const QString& dst, int pixelOp, QString* error) {
    QImageReader r(src);
    r.setAutoTransform(false);
    const QByteArray format = r.format();
    QImage img = r.read();
    if (img.isNull()) {
        setError(error, tr("Das Bild konnte nicht dekodiert werden."));
        return false;
    }
    static const int m[9][4] = {{1, 0, 0, 1}, {1, 0, 0, 1},  {-1, 0, 0, 1},  {-1, 0, 0, -1}, {1, 0, 0, -1},
                                {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    const int* t = m[pixelOp >= 1 && pixelOp <= 8 ? pixelOp : 1];
    // QTransform maps (x, y) -> (m11 x + m21 y, m12 x + m22 y); QImage::transformed re-centers.
    img = img.transformed(QTransform(t[0], t[2], t[1], t[3], 0, 0));
    QImageWriter w(dst, format);
    w.setQuality(95);
    if (!w.write(img)) {
        setError(error, tr("Dieses Format kann nicht geschrieben werden."));
        return false;
    }
    return true;
}

bool replaceFile(const QString& replacement, const QString& target, bool keepModificationDate,
                 QString* error) {
    const QFileInfo orig(target);
    if (!orig.exists()) {
        setError(error, tr("„%1“ existiert nicht mehr.").arg(orig.fileName()));
        return false;
    }
    QFile::setPermissions(replacement, QFile::permissions(target));
    if (keepModificationDate) {
        QFile f(replacement);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(orig.lastModified(), QFileDevice::FileModificationTime);
    }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(replacement.toStdU16String()),
                            std::filesystem::path(target.toStdU16String()), ec);
    if (ec) {
        setError(error, tr("„%1“ konnte nicht ersetzt werden: %2")
                            .arg(orig.fileName(), QString::fromStdString(ec.message())));
        return false;
    }
    return true;
}

bool copyWithMetadata(const QString& path, const QString& copy, QString* error) {
    if (!QFile::copy(path, copy)) {
        setError(error, tr("Sicherungskopie für „Widerrufen“ konnte nicht angelegt werden."));
        return false;
    }
    return true;
}

} // namespace lf::platform
