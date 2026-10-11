// SPDX-License-Identifier: GPL-3.0-or-later
// UI description of sort specs (F-SORT-05) and the supported suffix list (F-OPEN-06).

#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/ImageFormats.h"
#include "core/LibraryDecoders.h"
#include "core/SortSpec.h"

#include <gtest/gtest.h>

using namespace lf;

namespace {
// These tests check the real lists (the other unit tests use the static one where the
// platform's backends differ, test_format_environment.cpp).
struct RealFormatList {
    RealFormatList() { setStaticImageSuffixesForTesting(false); }
    ~RealFormatList() {
#if !defined(Q_OS_DARWIN) || defined(LF_GENERIC_PLATFORM)
        setStaticImageSuffixesForTesting(true); // as the environment set it
#endif
    }
};
} // namespace

TEST(SortSpec, F_SORT_05_DescribeFinderSources) {
    EXPECT_EQ(describe({SortKey::DateAdded, false, SortSource::FinderDsStore}),
              QString::fromUtf8("Wie Finder: Hinzugefügt am ↓"));
    EXPECT_EQ(describe({SortKey::Name, true, SortSource::FinderGlobal}),
              QString::fromUtf8("Wie Finder: Name ↑"));
    EXPECT_EQ(describe({SortKey::FinderIconPosition, true, SortSource::FinderDsStore}),
              QString::fromUtf8("Wie Finder: Symbolposition"));
}

// F-SORT-04: the order came from the front Finder window (opt-in live query).
TEST(SortSpec, F_SORT_04_DescribeLiveSource) {
    EXPECT_EQ(describe({SortKey::Size, false, SortSource::FinderLive}),
              QString::fromUtf8("Wie Finder (live): Größe ↓"));
    EXPECT_EQ(describe({SortKey::FinderIconPosition, true, SortSource::FinderLive}),
              QString::fromUtf8("Wie Finder (live): Symbolposition"));
}

TEST(SortSpec, F_SORT_05_DescribeFallbackAndManual) {
    EXPECT_EQ(describe({SortKey::Name, true, SortSource::Fallback}),
              QString::fromUtf8("Name ↑ (Finder-Info nicht gefunden)"));
    EXPECT_EQ(describe({SortKey::DateModified, false, SortSource::Manual}),
              QString::fromUtf8("Änderungsdatum ↓"));
}

TEST(SortSpec, F_SORT_02_EveryKeyHasALabel) {
    for (SortKey k : {SortKey::Name, SortKey::DateModified, SortKey::DateCreated, SortKey::DateAdded,
                      SortKey::DateCaptured, SortKey::Size, SortKey::Kind, SortKey::PixelCount,
                      SortKey::Rating, SortKey::FinderIconPosition})
        EXPECT_FALSE(sortKeyLabel(k).isEmpty());
    EXPECT_EQ(sortKeyLabel(SortKey::DateCaptured), QString::fromUtf8("Aufnahmedatum"));
}

TEST(ImageFormats, F_OPEN_06_KnownSuffixes) {
    const RealFormatList real;
    test::ensureQtApp(); // the list comes from the backends (Qt image plugins)
    for (const char* s : {"jpg", "jpeg", "png", "webp", "tiff", "tif", "gif", "svg", "qoi", "xcf", "kra"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
#if defined(Q_OS_DARWIN) && !defined(LF_GENERIC_PLATFORM)
    for (const char* s :
         {"heic", "avif", "jxl", "psd", "exr", "cr3", "nef", "arw", "dng", "raf", "orf", "rw2", "dcm"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
#endif
    for (const char* s : {"txt", "pdf", "mov", "mp4", "zip", "", "app", "JPG"})
        EXPECT_FALSE(isImageSuffix(QString::fromLatin1(s))) << s;
    EXPECT_TRUE(hasImageSuffix("Foto.JPG"));
    EXPECT_TRUE(hasImageSuffix("/a/b.c/IMG_1.png"));
    EXPECT_FALSE(hasImageSuffix("/a/b.jpg/readme"));
    EXPECT_FALSE(hasImageSuffix("noext"));
    EXPECT_FALSE(hasImageSuffix("trailingdot."));
    EXPECT_TRUE(isRawSuffix("cr3"));
    EXPECT_FALSE(isRawSuffix("jpg"));
    EXPECT_GE(imageSuffixes().size(), 20);
    EXPECT_GE(defaultImageSuffixes().size(), 80);
    for (const char* s : {"qoi", "xcf", "kra", "cr3", "x3f"})
        EXPECT_TRUE(defaultImageSuffixes().contains(QString::fromLatin1(s))) << s;
}

// Review findings (F-OPEN-06): only suffixes with a registered backend are claimed; formats
// nobody can decode (GIMP before M3, Sigma X3F, ...) were listed and produced error cards.
// M3: Krita, OpenRaster, PCX, QOI … have own decoders now (SimpleDecoders.h), XCF and PSB too.
TEST(ImageFormats, F_OPEN_06_OnlySuffixesWithBackend) {
    const RealFormatList real;
    test::ensureQtApp();
#if defined(Q_OS_DARWIN) && !defined(LF_GENERIC_PLATFORM) // ImageIO decides the suffixes
    for (const char* s : {"jpg", "jpeg", "jfif", "heic", "heif", "cr3", "jxl", "avif", "dng", "nef", "png",
                          "svg", "svgz", "tif", "webp"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
    for (const char* s : {"x3f", "ari", "cap", "pdf", "txt"})
        EXPECT_FALSE(isImageSuffix(QString::fromLatin1(s))) << s;
    EXPECT_FALSE(hasImageSuffix("x.x3f"));
    EXPECT_TRUE(hasImageSuffix("IMG_0001.HEIC"));
#endif
    for (const char* s : {"kra", "ora", "qoi", "pcx", "pfm", "sgi", "xcf", "psb"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
    for (const QString& s : imageSuffixes())
        EXPECT_EQ(s, s.toLower());
}

// Review (ports): the list of each platform is the list of its backends - every suffix is
// declared by a registered backend (or is another name of one), and suffixes without a
// backend there are not offered. Windows: the WIC codecs (JPEG XR as jxr/wdp/hdp); Linux:
// the library decoders that are built in.
TEST(ImageFormats, F_OPEN_06_FormatListFollowsTheBackendsOfThisPlatform) {
    const RealFormatList real;
    test::ensureQtApp();
    const QStringList declared = DecoderRegistry::instance().supportedExtensions();
    static const QStringList aliases = {"jpe",  "jfif", "jpeg", "jpg", "apng", "dib", "tif",
                                        "tiff", "heic", "heif", "jxr", "wdp",  "hdp"};
    for (const QString& s : imageSuffixes())
        EXPECT_TRUE(declared.contains(s) || aliases.contains(s)) << s.toStdString() << " has no backend";
#ifdef Q_OS_WIN
    for (const char* s : {"jxr", "wdp", "hdp", "jpg", "png", "tif", "bmp", "ico", "dds"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
    // (x3f etc. come with the Raw Image Extension, which a runner may have installed)
    for (const char* s : {"hdr", "pic", "jp2", "ktx", "dcm", "exr"})
        EXPECT_FALSE(isImageSuffix(QString::fromLatin1(s))) << s;
#elif !defined(Q_OS_DARWIN)
    for (const char* s : {"hdr", "pic", "jp2", "ktx", "dcm", "jxr"})
        EXPECT_FALSE(isImageSuffix(QString::fromLatin1(s))) << s;
    // The library decoders: exactly when built in (heic/avif/jxl/RAW/exr).
    const QStringList library = libraryDecoderSuffixes();
    for (const char* s : {"heic", "avif", "jxl", "cr3", "nef", "raw", "exr"})
        EXPECT_EQ(isImageSuffix(QString::fromLatin1(s)), library.contains(QString::fromLatin1(s))) << s;
#endif
}
