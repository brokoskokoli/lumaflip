// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImageFormats.h"

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

struct Tables {
    QSet<QString> all;
    QSet<QString> raw;
    Tables() {
        for (const char* s : kCommon)
            all.insert(QString::fromLatin1(s));
        for (const char* s : kRaw) {
            all.insert(QString::fromLatin1(s));
            raw.insert(QString::fromLatin1(s));
        }
    }
};

const Tables& tables() {
    static const Tables t; // thread-safe static initialisation
    return t;
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
    QStringList l(tables().all.cbegin(), tables().all.cend());
    std::sort(l.begin(), l.end());
    return l;
}

} // namespace lf
