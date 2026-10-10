// SPDX-License-Identifier: GPL-3.0-or-later
#include "DisplayText.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>

namespace lf {

namespace {

// tr() for the code in this file outside QObject classes (lupdate takes the context from it).
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::DisplayText)
};

// Extension -> type name (German source text; images Lumaflip opens, the rest uses the
// system text). Translated when looked up, so a language switch applies at once.
const QHash<QString, const char*>& suffixNames() {
    static const QHash<QString, const char*> names = [] {
        QHash<QString, const char*> h;
        auto add = [&h](std::initializer_list<const char*> suffixes, const char* name) {
            for (const char* s : suffixes)
                h.insert(QString::fromLatin1(s), name);
        };
        add({"jpg", "jpeg", "jpe", "jfif"}, QT_TRANSLATE_NOOP("lf::DisplayText", "JPEG-Bild"));
        add({"png"}, QT_TRANSLATE_NOOP("lf::DisplayText", "PNG-Bild"));
        add({"apng"}, QT_TRANSLATE_NOOP("lf::DisplayText", "APNG-Bild"));
        add({"gif"}, QT_TRANSLATE_NOOP("lf::DisplayText", "GIF-Bild"));
        add({"tif", "tiff"}, QT_TRANSLATE_NOOP("lf::DisplayText", "TIFF-Bild"));
        add({"bmp", "dib"}, QT_TRANSLATE_NOOP("lf::DisplayText", "BMP-Bild"));
        add({"webp"}, QT_TRANSLATE_NOOP("lf::DisplayText", "WebP-Bild"));
        add({"heic", "heics"}, QT_TRANSLATE_NOOP("lf::DisplayText", "HEIC-Bild"));
        add({"heif", "hif"}, QT_TRANSLATE_NOOP("lf::DisplayText", "HEIF-Bild"));
        add({"avif", "avifs"}, QT_TRANSLATE_NOOP("lf::DisplayText", "AVIF-Bild"));
        add({"jxl"}, QT_TRANSLATE_NOOP("lf::DisplayText", "JPEG-XL-Bild"));
        add({"jp2", "j2k", "jpf", "jpx", "j2c"}, QT_TRANSLATE_NOOP("lf::DisplayText", "JPEG-2000-Bild"));
        add({"psd"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Photoshop-Dokument"));
        add({"psb"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Photoshop-Dokument (groß)"));
        add({"exr"}, QT_TRANSLATE_NOOP("lf::DisplayText", "OpenEXR-Bild"));
        add({"hdr", "pic"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Radiance-HDR-Bild"));
        add({"tga", "targa"}, QT_TRANSLATE_NOOP("lf::DisplayText", "TGA-Bild"));
        add({"dds"}, QT_TRANSLATE_NOOP("lf::DisplayText", "DDS-Textur"));
        add({"ktx", "ktx2"}, QT_TRANSLATE_NOOP("lf::DisplayText", "KTX-Textur"));
        add({"ico"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Windows-Symbol (ICO)"));
        add({"cur"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Windows-Mauszeiger (CUR)"));
        add({"icns"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Apple-Symbol (ICNS)"));
        add({"svg", "svgz"}, QT_TRANSLATE_NOOP("lf::DisplayText", "SVG-Vektorgrafik"));
        add({"pdf"}, QT_TRANSLATE_NOOP("lf::DisplayText", "PDF-Dokument"));
        // Camera RAW: maker and format.
        add({"dng"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Adobe DNG)"));
        add({"cr2"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Canon CR2)"));
        add({"cr3"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Canon CR3)"));
        add({"crw"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Canon CRW)"));
        add({"nef"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Nikon NEF)"));
        add({"nrw"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Nikon NRW)"));
        add({"arw"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Sony ARW)"));
        add({"sr2"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Sony SR2)"));
        add({"srf"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Sony SRF)"));
        add({"raf"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Fujifilm RAF)"));
        add({"orf"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Olympus ORF)"));
        add({"rw2"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Panasonic RW2)"));
        add({"raw"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW"));
        add({"pef"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Pentax PEF)"));
        add({"iiq"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Phase One IIQ)"));
        add({"3fr"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Hasselblad 3FR)"));
        add({"fff"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Hasselblad FFF)"));
        add({"erf"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Epson ERF)"));
        add({"mrw"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Minolta MRW)"));
        add({"mos"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Leaf MOS)"));
        add({"rwl"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Leica RWL)"));
        add({"srw"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Samsung SRW)"));
        add({"x3f"}, QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Sigma X3F)"));
        return h;
    }();
    return names;
}

// UTType -> name for the types the app registers for (F-MAC-04).
QString typeName(const QString& id) {
    static const QHash<QString, const char*> names = {
        {QStringLiteral("public.jpeg"), QT_TRANSLATE_NOOP("lf::DisplayText", "JPEG-Bild")},
        {QStringLiteral("public.png"), QT_TRANSLATE_NOOP("lf::DisplayText", "PNG-Bild")},
        {QStringLiteral("com.compuserve.gif"), QT_TRANSLATE_NOOP("lf::DisplayText", "GIF-Bild")},
        {QStringLiteral("public.tiff"), QT_TRANSLATE_NOOP("lf::DisplayText", "TIFF-Bild")},
        {QStringLiteral("com.microsoft.bmp"), QT_TRANSLATE_NOOP("lf::DisplayText", "BMP-Bild")},
        {QStringLiteral("org.webmproject.webp"), QT_TRANSLATE_NOOP("lf::DisplayText", "WebP-Bild")},
        {QStringLiteral("public.heic"), QT_TRANSLATE_NOOP("lf::DisplayText", "HEIC-Bild")},
        {QStringLiteral("public.heif"), QT_TRANSLATE_NOOP("lf::DisplayText", "HEIF-Bild")},
        {QStringLiteral("public.avif"), QT_TRANSLATE_NOOP("lf::DisplayText", "AVIF-Bild")},
        {QStringLiteral("public.jpeg-xl"), QT_TRANSLATE_NOOP("lf::DisplayText", "JPEG-XL-Bild")},
        {QStringLiteral("public.camera-raw-image"), QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW")},
        {QStringLiteral("com.adobe.raw-image"),
         QT_TRANSLATE_NOOP("lf::DisplayText", "Kamera-RAW (Adobe DNG)")},
        {QStringLiteral("com.adobe.photoshop-image"),
         QT_TRANSLATE_NOOP("lf::DisplayText", "Photoshop-Dokument")},
        {QStringLiteral("com.ilm.openexr-image"), QT_TRANSLATE_NOOP("lf::DisplayText", "OpenEXR-Bild")},
        {QStringLiteral("public.radiance"), QT_TRANSLATE_NOOP("lf::DisplayText", "Radiance-HDR-Bild")},
        {QStringLiteral("public.jpeg-2000"), QT_TRANSLATE_NOOP("lf::DisplayText", "JPEG-2000-Bild")},
        {QStringLiteral("com.truevision.tga-image"), QT_TRANSLATE_NOOP("lf::DisplayText", "TGA-Bild")},
        {QStringLiteral("com.microsoft.ico"), QT_TRANSLATE_NOOP("lf::DisplayText", "Windows-Symbol (ICO)")},
        {QStringLiteral("com.apple.icns"), QT_TRANSLATE_NOOP("lf::DisplayText", "Apple-Symbol (ICNS)")},
        {QStringLiteral("public.svg-image"), QT_TRANSLATE_NOOP("lf::DisplayText", "SVG-Vektorgrafik")},
    };
    const char* name = names.value(id);
    return name ? Tr::tr(name) : QString();
}

// Abstract types without own extensions.
QStringList typeExtensions(const QString& id) {
    if (id == QLatin1String("public.camera-raw-image"))
        return {QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"),
                QStringLiteral("arw"), QStringLiteral("raf"), QStringLiteral("orf"),
                QStringLiteral("rw2"), QStringLiteral("pef"), QStringLiteral("…")};
    return {};
}

} // namespace

QString kindNameForSuffix(const QString& suffix) {
    const char* name = suffixNames().value(suffix.toLower());
    return name ? Tr::tr(name) : QString();
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
