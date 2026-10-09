// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageFormats.h"

#include "Decoder.h"

#include <QSet>
#include <algorithm>

namespace lf {
namespace {

// clang-format off
constexpr const char* kCommon[] = {
    "jpg", "jpeg", "jpe", "jfif", "png", "apng", "gif", "bmp", "dib", "tif", "tiff", "webp",
    "heic", "heif", "hif", "avif", "avifs", "jxl", "jp2", "j2k", "jpf", "jpx", "ico", "cur",
    "icns", "tga", "psd", "psb", "exr", "hdr", "pic", "dds", "ktx", "ktx2", "astc", "pvr",
    "svg", "svgz", "qoi", "pcx", "pbm", "pgm", "ppm", "pnm", "pfm", "xcf", "kra", "ora",
    "sgi", "rgb", "ras", "dcm",
};
constexpr const char* kRaw[] = {
    "crw", "cr2", "cr3", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "rwl", "dng",
    "pef", "ptx", "srw", "x3f", "3fr", "fff", "iiq", "mos", "mef", "mrw", "erf", "kdc", "dcr",
    "raw", "ari", "cap",
};
// clang-format on

// Rendered by some backend (Qt's PDF plugin) but documents, not images to flip through.
constexpr const char* kExcluded[] = {"pdf", "ps", "eps", "ai"};

struct Tables {
    QSet<QString> defaults;
    QSet<QString> all;
    QSet<QString> raw;
    Tables() {
        for (const char* s : kCommon)
            defaults.insert(QString::fromLatin1(s));
        for (const char* s : kRaw) {
            defaults.insert(QString::fromLatin1(s));
            raw.insert(QString::fromLatin1(s));
        }
#ifdef Q_OS_DARWIN
        // Only what a registered backend can decode (the registry's extension set is
        // built from the backends alone on macOS).
        for (const QString& e : DecoderRegistry::instance().supportedExtensions())
            all.insert(e.toLower());
        for (const char* s : kExcluded)
            all.remove(QString::fromLatin1(s));
#else
        all = defaults;
#endif
    }
};

const Tables& tables() {
    static const Tables t; // thread-safe static initialisation
    return t;
}

QStringList sorted(const QSet<QString>& set) {
    QStringList l(set.cbegin(), set.cend());
    std::sort(l.begin(), l.end());
    return l;
}

} // namespace

bool isImageSuffix(const QString& lowerSuffix) {
    return tables().all.contains(lowerSuffix);
}

bool isRawSuffix(const QString& lowerSuffix) {
    return tables().raw.contains(lowerSuffix);
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
    return sorted(tables().all);
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
