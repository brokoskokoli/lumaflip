// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayText.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>

namespace lf {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::DisplayText", text);
}

const QLocale& german() {
    static const QLocale loc(QLocale::German, QLocale::Germany);
    return loc;
}

// Extension -> German type name (images Lumaflip opens; the rest uses the system text).
const QHash<QString, QString>& suffixNames() {
    static const QHash<QString, QString> names = [] {
        QHash<QString, QString> h;
        auto add = [&h](std::initializer_list<const char*> suffixes, const QString& name) {
            for (const char* s : suffixes)
                h.insert(QString::fromLatin1(s), name);
        };
        add({"jpg", "jpeg", "jpe", "jfif"}, tr("JPEG-Bild"));
        add({"png"}, tr("PNG-Bild"));
        add({"apng"}, tr("APNG-Bild"));
        add({"gif"}, tr("GIF-Bild"));
        add({"tif", "tiff"}, tr("TIFF-Bild"));
        add({"bmp", "dib"}, tr("BMP-Bild"));
        add({"webp"}, tr("WebP-Bild"));
        add({"heic", "heics"}, tr("HEIC-Bild"));
        add({"heif", "hif"}, tr("HEIF-Bild"));
        add({"avif", "avifs"}, tr("AVIF-Bild"));
        add({"jxl"}, tr("JPEG-XL-Bild"));
        add({"jp2", "j2k", "jpf", "jpx", "j2c"}, tr("JPEG-2000-Bild"));
        add({"psd"}, tr("Photoshop-Dokument"));
        add({"psb"}, tr("Photoshop-Dokument (groß)"));
        add({"exr"}, tr("OpenEXR-Bild"));
        add({"hdr", "pic"}, tr("Radiance-HDR-Bild"));
        add({"tga", "targa"}, tr("TGA-Bild"));
        add({"dds"}, tr("DDS-Textur"));
        add({"ktx", "ktx2"}, tr("KTX-Textur"));
        add({"ico"}, tr("Windows-Symbol (ICO)"));
        add({"cur"}, tr("Windows-Mauszeiger (CUR)"));
        add({"icns"}, tr("Apple-Symbol (ICNS)"));
        add({"svg", "svgz"}, tr("SVG-Vektorgrafik"));
        add({"pdf"}, tr("PDF-Dokument"));
        // Camera RAW: maker and format.
        add({"dng"}, tr("Kamera-RAW (Adobe DNG)"));
        add({"cr2"}, tr("Kamera-RAW (Canon CR2)"));
        add({"cr3"}, tr("Kamera-RAW (Canon CR3)"));
        add({"crw"}, tr("Kamera-RAW (Canon CRW)"));
        add({"nef"}, tr("Kamera-RAW (Nikon NEF)"));
        add({"nrw"}, tr("Kamera-RAW (Nikon NRW)"));
        add({"arw"}, tr("Kamera-RAW (Sony ARW)"));
        add({"sr2"}, tr("Kamera-RAW (Sony SR2)"));
        add({"srf"}, tr("Kamera-RAW (Sony SRF)"));
        add({"raf"}, tr("Kamera-RAW (Fujifilm RAF)"));
        add({"orf"}, tr("Kamera-RAW (Olympus ORF)"));
        add({"rw2"}, tr("Kamera-RAW (Panasonic RW2)"));
        add({"raw"}, tr("Kamera-RAW"));
        add({"pef"}, tr("Kamera-RAW (Pentax PEF)"));
        add({"iiq"}, tr("Kamera-RAW (Phase One IIQ)"));
        add({"3fr"}, tr("Kamera-RAW (Hasselblad 3FR)"));
        add({"fff"}, tr("Kamera-RAW (Hasselblad FFF)"));
        add({"erf"}, tr("Kamera-RAW (Epson ERF)"));
        add({"mrw"}, tr("Kamera-RAW (Minolta MRW)"));
        add({"mos"}, tr("Kamera-RAW (Leaf MOS)"));
        add({"rwl"}, tr("Kamera-RAW (Leica RWL)"));
        add({"srw"}, tr("Kamera-RAW (Samsung SRW)"));
        add({"x3f"}, tr("Kamera-RAW (Sigma X3F)"));
        return h;
    }();
    return names;
}

// UTType -> German name for the types the app registers for (F-MAC-04).
QString typeName(const QString& id) {
    static const QHash<QString, QString> names = {
        {QStringLiteral("public.jpeg"), tr("JPEG-Bild")},
        {QStringLiteral("public.png"), tr("PNG-Bild")},
        {QStringLiteral("com.compuserve.gif"), tr("GIF-Bild")},
        {QStringLiteral("public.tiff"), tr("TIFF-Bild")},
        {QStringLiteral("com.microsoft.bmp"), tr("BMP-Bild")},
        {QStringLiteral("org.webmproject.webp"), tr("WebP-Bild")},
        {QStringLiteral("public.heic"), tr("HEIC-Bild")},
        {QStringLiteral("public.heif"), tr("HEIF-Bild")},
        {QStringLiteral("public.avif"), tr("AVIF-Bild")},
        {QStringLiteral("public.jpeg-xl"), tr("JPEG-XL-Bild")},
        {QStringLiteral("public.camera-raw-image"), tr("Kamera-RAW")},
        {QStringLiteral("com.adobe.raw-image"), tr("Kamera-RAW (Adobe DNG)")},
        {QStringLiteral("com.adobe.photoshop-image"), tr("Photoshop-Dokument")},
        {QStringLiteral("com.ilm.openexr-image"), tr("OpenEXR-Bild")},
        {QStringLiteral("public.radiance"), tr("Radiance-HDR-Bild")},
        {QStringLiteral("public.jpeg-2000"), tr("JPEG-2000-Bild")},
        {QStringLiteral("com.truevision.tga-image"), tr("TGA-Bild")},
        {QStringLiteral("com.microsoft.ico"), tr("Windows-Symbol (ICO)")},
        {QStringLiteral("com.apple.icns"), tr("Apple-Symbol (ICNS)")},
        {QStringLiteral("public.svg-image"), tr("SVG-Vektorgrafik")},
    };
    return names.value(id);
}

// Abstract types without own extensions.
QStringList typeExtensions(const QString& id) {
    if (id == QLatin1String("public.camera-raw-image"))
        return {QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"), QStringLiteral("arw"),
                QStringLiteral("raf"), QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("pef"),
                QStringLiteral("…")};
    return {};
}

} // namespace

QString formatByteSize(qint64 bytes) {
    const QLocale& de = german();
    if (bytes < 1000)
        return tr("%1 Byte").arg(de.toString(bytes));
    static const char* const units[] = {"KB", "MB", "GB", "TB", "PB"};
    double v = double(bytes);
    int u = -1;
    while (v >= 999.95 && u < 4) {
        v /= 1000.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(de.toString(v, 'f', 1), QString::fromLatin1(units[u]));
}

QString formatByteSizeExact(qint64 bytes) {
    if (bytes < 1000)
        return formatByteSize(bytes);
    return tr("%1 (%2 Byte)").arg(formatByteSize(bytes), german().toString(bytes));
}

QString kindNameForSuffix(const QString& suffix) {
    return suffixNames().value(suffix.toLower());
}

QString displayKind(const QString& fileName, const QString& systemKind) {
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    const QString name = dot >= 0 ? kindNameForSuffix(fileName.mid(dot + 1)) : QString();
    return name.isEmpty() ? systemKind : name;
}

ImageTypeDescription describeImageType(const QString& identifier) {
    ImageTypeDescription d;
    d.identifier = identifier;
    d.name = typeName(identifier);
    if (d.name.isEmpty())
        d.name = systemTypeDescription(identifier);
    if (d.name.isEmpty())
        d.name = identifier;
    d.extensions = systemTypeExtensions(identifier);
    if (d.extensions.isEmpty())
        d.extensions = typeExtensions(identifier);
    return d;
}

} // namespace lf
