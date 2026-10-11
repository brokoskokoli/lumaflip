// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageFormats.h"

#include "Decoder.h"
#include "Platform.h"

#include <QSet>

#include <algorithm>
#include <utility>

namespace lf {
namespace {

// clang-format off
constexpr const char* kCommon[] = {
    "jpg", "jpeg", "jpe", "jfif", "png", "apng", "gif", "bmp", "dib", "tif", "tiff", "webp",
    "heic", "heif", "hif", "avif", "avifs", "jxl", "jp2", "j2k", "jpf", "jpx", "ico", "cur",
    "icns", "tga", "psd", "psb", "exr", "hdr", "pic", "dds", "ktx", "ktx2", "astc", "pvr",
    "svg", "svgz", "qoi", "pcx", "pbm", "pgm", "ppm", "pnm", "pam", "pfm", "xcf", "kra", "ora",
    "sgi", "rgb", "ras", "dcm",
};
constexpr const char* kRaw[] = {
    "crw", "cr2", "cr3", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "rwl", "dng",
    "pef", "ptx", "srw", "x3f", "3fr", "fff", "iiq", "mos", "mef", "mrw", "erf", "kdc", "dcr",
    "raw", "ari", "cap",
};
// clang-format on

// Static lists (no backend needed).
struct StaticTables {
    QSet<QString> defaults;
    QSet<QString> raw;
    StaticTables() {
        for (const char* s : kCommon)
            defaults.insert(QString::fromLatin1(s));
        for (const char* s : kRaw) {
            defaults.insert(QString::fromLatin1(s));
            raw.insert(QString::fromLatin1(s));
        }
    }
};

const StaticTables& staticTables() {
    static const StaticTables t; // thread-safe static initialisation
    return t;
}

// Rendered by some backend (Qt's PDF plugin) but documents, not images to flip through.
constexpr const char* kExcluded[] = {"pdf", "ps", "eps", "ai"};

// Other names of the same file types: a backend that reads the first one reads them as well
// (the decoders recognize the files by their content).
constexpr std::pair<const char*, const char*> kAliases[] = {
    {"jpg", "jpeg"}, {"jpg", "jpe"},  {"jpg", "jfif"}, {"jpeg", "jpg"},  {"png", "apng"},
    {"bmp", "dib"},  {"tif", "tiff"}, {"tiff", "tif"}, {"heic", "heif"}, {"heif", "heic"},
    {"jxr", "wdp"},  {"jxr", "hdp"},  {"wdp", "jxr"},  {"wdp", "hdp"}, // JPEG XR (Windows)
};

QSet<QString> finished(const QStringList& extensions) {
    QSet<QString> out;
    for (const QString& e : extensions)
        out.insert(e.toLower());
    for (const auto& [have, alias] : kAliases)
        if (out.contains(QLatin1String(have)))
            out.insert(QString::fromLatin1(alias));
    for (const char* s : kExcluded)
        out.remove(QString::fromLatin1(s));
    return out;
}

// Suffixes of the platform backend alone (ImageIO, WIC: a few ms). Answers the common case
// without building the whole registry, whose Qt image plugins are slow to load (P-01).
const QSet<QString>& platformSuffixes() {
    static const QSet<QString> set = [] {
        DecoderRegistry platform;
        registerPlatformDecoders(platform);
        return finished(platform.supportedExtensions());
    }();
    return set;
}

// Everything a registered backend can decode - on every platform derived from the backends
// that are really there (ImageIO/WIC with their installed codecs, Qt image plugins, the own
// decoders, libheif/libjxl/LibRaw/OpenEXR when built in), so suffixes nobody can decode
// (e.g. hdr, jp2 or dcm outside macOS) are not listed and produce no error cards.
const QSet<QString>& allSuffixes() {
    static const QSet<QString> set = finished(DecoderRegistry::instance().supportedExtensions());
    return set;
}

QStringList sorted(const QSet<QString>& set) {
    QStringList l(set.cbegin(), set.cend());
    std::sort(l.begin(), l.end());
    return l;
}

} // namespace

bool isImageSuffix(const QString& lowerSuffix) {
    if (platformSuffixes().contains(lowerSuffix))
        return true;
    return allSuffixes().contains(lowerSuffix);
}

bool isRawSuffix(const QString& lowerSuffix) {
    return staticTables().raw.contains(lowerSuffix);
}

bool hasImageSuffix(const QString& fileName) {
    const qsizetype dot = fileName.lastIndexOf(u'.');
    if (dot < 0 || dot == fileName.size() - 1)
        return false;
    if (fileName.lastIndexOf(u'/') > dot)
        return false;
    return isImageSuffix(fileName.mid(dot + 1).toLower());
}

QStringList imageSuffixes() {
    return sorted(allSuffixes());
}

QStringList defaultImageSuffixes() {
    static const QStringList l = [] {
        QSet<QString> s;
        for (const char* c : kCommon)
            s.insert(QString::fromLatin1(c));
        for (const char* c : kRaw)
            s.insert(QString::fromLatin1(c));
        return sorted(s);
    }();
    return l;
}

} // namespace lf
