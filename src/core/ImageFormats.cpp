// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageFormats.h"

#include "Decoder.h"
#include "Platform.h"

#include <QSet>
#include <algorithm>

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

#ifdef Q_OS_DARWIN
// Rendered by some backend (Qt's PDF plugin) but documents, not images to flip through.
constexpr const char* kExcluded[] = {"pdf", "ps", "eps", "ai"};

void removeExcluded(QSet<QString>& set) {
    for (const char* s : kExcluded)
        set.remove(QString::fromLatin1(s));
}

// Suffixes of the platform backend alone (ImageIO, a few ms). Answers the common case
// without building the whole registry, whose Qt image plugins are slow to load (P-01).
const QSet<QString>& platformSuffixes() {
    static const QSet<QString> set = [] {
        DecoderRegistry platform;
        registerPlatformDecoders(platform);
        QSet<QString> out;
        for (const QString& e : platform.supportedExtensions())
            out.insert(e.toLower());
        removeExcluded(out);
        return out;
    }();
    return set;
}
#endif

// Everything a registered backend can decode.
const QSet<QString>& allSuffixes() {
    static const QSet<QString> set = [] {
#ifdef Q_OS_DARWIN
        // Only what a registered backend can decode (the registry's extension set is
        // built from the backends alone on macOS).
        QSet<QString> out;
        for (const QString& e : DecoderRegistry::instance().supportedExtensions())
            out.insert(e.toLower());
        removeExcluded(out);
        return out;
#else
        return staticTables().defaults;
#endif
    }();
    return set;
}

QStringList sorted(const QSet<QString>& set) {
    QStringList l(set.cbegin(), set.cend());
    std::sort(l.begin(), l.end());
    return l;
}

} // namespace

bool isImageSuffix(const QString& lowerSuffix) {
#ifdef Q_OS_DARWIN
    if (platformSuffixes().contains(lowerSuffix))
        return true;
#endif
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
