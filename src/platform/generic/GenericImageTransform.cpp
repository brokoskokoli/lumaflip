// SPDX-License-Identifier: GPL-3.0-or-later
// Portable part of saving the rotation (F-EDIT-02, see core/LosslessTransform.h): JPEG
// files with an orientation tag (patched by the core) and lossless formats re-encoded with
// Qt. A lossless metadata copy for other formats is not available here.

#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/LosslessTransform.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QTransform>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lf::platform {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::LosslessTransform)
};

void setError(QString* error, const QString& msg) {
    if (error)
        *error = msg;
}

// fsync() of a file or folder (POSIX); elsewhere a no-op that reports success.
bool syncFile(const QString& path) {
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#else
    Q_UNUSED(path);
    return true;
#endif
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
    setError(error, Tr::tr("Für dieses Format kann die Ausrichtung nicht gespeichert werden."));
    return false;
}

bool reencodeTransformed(const QString& src, const QString& dst, int pixelOp, bool requireExact,
                         QString* error, bool* inexact) {
    if (inexact)
        *inexact = false;
    QImageReader r(src);
    r.setAutoTransform(false);
    const QByteArray format = r.format();
    QImage img = r.read();
    if (img.isNull()) {
        setError(error, Tr::tr("Das Bild konnte nicht dekodiert werden."));
        return false;
    }
    static const int m[9][4] = {{1, 0, 0, 1}, {1, 0, 0, 1},  {-1, 0, 0, 1},  {-1, 0, 0, -1}, {1, 0, 0, -1},
                                {0, 1, 1, 0}, {0, -1, 1, 0}, {0, -1, -1, 0}, {0, 1, -1, 0}};
    const int* t = m[pixelOp >= 1 && pixelOp <= 8 ? pixelOp : 1];
    if (img.depth() % 8 == 0) {
        // Byte-aligned pixels are moved as they are: same format, color table, alpha
        // (QImage::transformed may convert or extend the color table).
        const int w = img.width(), h = img.height(), B = img.depth() / 8;
        const bool swap = t[0] == 0;
        QImage out(swap ? h : w, swap ? w : h, img.format());
        if (out.isNull()) {
            setError(error, Tr::tr("Nicht genug Speicher zum Drehen."));
            return false;
        }
        out.setColorTable(img.colorTable());
        out.setColorSpace(img.colorSpace());
        out.setDotsPerMeterX(swap ? img.dotsPerMeterY() : img.dotsPerMeterX());
        out.setDotsPerMeterY(swap ? img.dotsPerMeterX() : img.dotsPerMeterY());
        const int minX = std::min(0, t[0] * (w - 1)) + std::min(0, t[1] * (h - 1));
        const int minY = std::min(0, t[2] * (w - 1)) + std::min(0, t[3] * (h - 1));
        for (int y = 0; y < h; ++y) {
            const uchar* row = img.constScanLine(y);
            for (int x = 0; x < w; ++x) {
                const int X = t[0] * x + t[1] * y - minX, Y = t[2] * x + t[3] * y - minY;
                std::memcpy(out.scanLine(Y) + qsizetype(X) * B, row + qsizetype(x) * B, size_t(B));
            }
        }
        img = out;
    } else {
        // QTransform maps (x, y) -> (m11 x + m21 y, m12 x + m22 y); QImage::transformed re-centers.
        img = img.transformed(QTransform(t[0], t[2], t[1], t[3], 0, 0));
    }
    QImageWriter w(dst, format);
    if (!requireExact)
        w.setQuality(95);
    if (!w.write(img)) {
        setError(error, Tr::tr("Dieses Format kann nicht geschrieben werden."));
        return false;
    }
    if (requireExact) {
        // Read back: same format (color model, depth, palette) and the same pixels.
        QImageReader back(dst);
        back.setAutoTransform(false);
        if (back.read() != img) {
            QFile::remove(dst);
            if (inexact)
                *inexact = true;
            setError(error, Tr::tr("Das Bild lässt sich in diesem Format nicht exakt neu speichern."));
            return false;
        }
    }
    return true;
}

bool replaceFile(const QString& replacement, const QString& target, bool keepModificationDate,
                 QString* error) {
    const QFileInfo orig(target);
    if (!orig.exists()) {
        setError(error, Tr::tr("„%1“ existiert nicht mehr.").arg(orig.fileName()));
        return false;
    }
    if (orig.isSymLink() || !orig.isFile()) {
        setError(error, Tr::tr("„%1“ ist keine normale Datei.").arg(orig.fileName()));
        return false;
    }
    std::error_code ec;
    const auto links = std::filesystem::hard_link_count(std::filesystem::path(target.toStdU16String()), ec);
    if (!ec && links > 1) {
        // Hard links: the contents go into the existing file (see LosslessTransform.h).
        if (!syncFile(replacement)) {
            setError(error, Tr::tr("Die neue Datei konnte nicht gesichert werden."));
            return false;
        }
        reportDurabilityStep(DurabilityStep::FullSync, replacement);
        const QDateTime mtime = keepModificationDate ? orig.lastModified() : QDateTime::currentDateTimeUtc();
        QFile in(replacement), out(target);
        if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::ReadWrite)) {
            setError(error, Tr::tr("Die Datei kann nicht geschrieben werden."));
            return false;
        }
        const QByteArray data = in.readAll();
        const bool ok = out.write(data) == data.size() && out.resize(data.size()) && out.flush() &&
                        out.setFileTime(mtime, QFileDevice::FileModificationTime);
        out.close();
        if (!ok || !syncFile(target)) {
            setError(error, Tr::tr("Die Datei konnte nicht vollständig geschrieben werden."));
            return false;
        }
        reportDurabilityStep(DurabilityStep::InPlaceCopy, target);
        QFile::remove(replacement);
        return true;
    }
    QFile::setPermissions(replacement, QFile::permissions(target));
    {
        // Kept on request; otherwise "now" (the replacement may carry the original's date).
        QFile f(replacement);
        if (f.open(QIODevice::ReadWrite))
            f.setFileTime(keepModificationDate ? orig.lastModified() : QDateTime::currentDateTimeUtc(),
                          QFileDevice::FileModificationTime);
    }
    if (!syncFile(replacement)) {
        setError(error, Tr::tr("Die neue Datei konnte nicht gesichert werden."));
        return false;
    }
    reportDurabilityStep(DurabilityStep::FullSync, replacement);
    std::filesystem::rename(std::filesystem::path(replacement.toStdU16String()),
                            std::filesystem::path(target.toStdU16String()), ec);
    if (ec) {
        setError(error, Tr::tr("„%1“ konnte nicht ersetzt werden: %2")
                            .arg(orig.fileName(), QString::fromStdString(ec.message())));
        return false;
    }
    reportDurabilityStep(DurabilityStep::Rename, target);
    if (syncFile(orig.absolutePath()))
        reportDurabilityStep(DurabilityStep::FolderSync, orig.absolutePath());
    return true;
}

bool copyWithMetadata(const QString& path, const QString& copy, QString* error, bool* cloned) {
    if (cloned)
        *cloned = false;
    if (!QFile::copy(path, copy)) {
        setError(error, Tr::tr("Sicherungskopie für „Widerrufen“ konnte nicht angelegt werden."));
        return false;
    }
    QFile f(copy);
    if (f.open(QIODevice::ReadWrite))
        f.setFileTime(QFileInfo(path).lastModified(), QFileDevice::FileModificationTime);
    return true;
}

} // namespace lf::platform
