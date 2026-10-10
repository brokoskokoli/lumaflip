// SPDX-License-Identifier: GPL-3.0-or-later
// libraryDecoders() (LibraryDecoders.h): collects the backends of src/codecs that this
// build has (LF_HAVE_LIBHEIF, LF_HAVE_LIBJXL, LF_HAVE_LIBRAW, LF_HAVE_OPENEXR).

#include "LibraryDecoders.h"

#include "ImageFormats.h"

namespace lf {

#if !LF_HAVE_LIBHEIF
std::shared_ptr<Decoder> createLibHeifDecoder() {
    return nullptr;
}
#endif
#if !LF_HAVE_LIBJXL
std::shared_ptr<Decoder> createLibJxlDecoder() {
    return nullptr;
}
#endif
#if !LF_HAVE_LIBRAW
std::shared_ptr<Decoder> createLibRawDecoder() {
    return nullptr;
}
#endif
#if !LF_HAVE_OPENEXR
std::shared_ptr<Decoder> createOpenExrDecoder() {
    return nullptr;
}
#endif

// Version strings, defined next to each backend.
QString libHeifVersion();
QString libJxlVersion();
QString libRawVersion();
QString openExrVersion();
void initLibHeif();

void prepareLibraryDecoders() {
#if LF_HAVE_LIBHEIF
    initLibHeif(); // loads libheif's decoder plugins
#endif
}

QList<std::shared_ptr<Decoder>> libraryDecoders() {
    QList<std::shared_ptr<Decoder>> out;
    for (auto d :
         {createLibHeifDecoder(), createLibJxlDecoder(), createLibRawDecoder(), createOpenExrDecoder()})
        if (d)
            out << d;
    return out;
}

QStringList libraryDecoderSuffixes() {
    QStringList out;
    if (LF_HAVE_LIBHEIF)
        out << QStringLiteral("heic") << QStringLiteral("heif") << QStringLiteral("hif")
            << QStringLiteral("avif") << QStringLiteral("avifs") << QStringLiteral("heics")
            << QStringLiteral("heifs");
    if (LF_HAVE_LIBJXL)
        out << QStringLiteral("jxl");
    if (LF_HAVE_LIBRAW)
        for (const QString& s : defaultImageSuffixes())
            if (isRawSuffix(s))
                out << s;
    if (LF_HAVE_OPENEXR)
        out << QStringLiteral("exr");
    return out;
}

QStringList libraryDecoderVersions() {
    QStringList out;
#if LF_HAVE_LIBHEIF
    out << libHeifVersion();
#endif
#if LF_HAVE_LIBJXL
    out << libJxlVersion();
#endif
#if LF_HAVE_LIBRAW
    out << libRawVersion();
#endif
#if LF_HAVE_OPENEXR
    out << openExrVersion();
#endif
    return out;
}

} // namespace lf
