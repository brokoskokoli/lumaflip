// SPDX-License-Identifier: GPL-3.0-or-later
// UI description of sort specs (F-SORT-05) and the supported suffix list (F-OPEN-06).

#include "core/ImageFormats.h"
#include "core/SortSpec.h"

#include <gtest/gtest.h>

using namespace lf;

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
    for (const char* s : {"jpg", "jpeg", "png", "heic", "avif", "jxl", "webp", "tiff", "gif", "psd",
                          "exr", "svg",  "cr3", "nef",  "arw",  "dng", "raf",  "orf",  "rw2", "dcm"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
    for (const char* s : {"txt", "pdf", "mov", "mp4", "zip", "", "app", "JPG"})
        EXPECT_FALSE(isImageSuffix(QString::fromLatin1(s))) << s;
    EXPECT_TRUE(hasImageSuffix("Foto.JPG"));
    EXPECT_TRUE(hasImageSuffix("/a/b.c/IMG_1.cr2"));
    EXPECT_FALSE(hasImageSuffix("/a/b.jpg/readme"));
    EXPECT_FALSE(hasImageSuffix("noext"));
    EXPECT_FALSE(hasImageSuffix("trailingdot."));
    EXPECT_TRUE(isRawSuffix("cr3"));
    EXPECT_FALSE(isRawSuffix("jpg"));
    EXPECT_GE(imageSuffixes().size(), 50);
    EXPECT_GE(defaultImageSuffixes().size(), 80);
    for (const char* s : {"qoi", "xcf", "kra", "cr3", "x3f"})
        EXPECT_TRUE(defaultImageSuffixes().contains(QString::fromLatin1(s))) << s;
}

// Review finding (F-OPEN-06): on macOS only suffixes with a registered backend are claimed;
// formats nobody can decode (GIMP, Sigma X3F, ...) were listed and produced error cards.
// M3: Krita, OpenRaster, PCX, QOI … have own decoders now (SimpleDecoders.h).
TEST(ImageFormats, F_OPEN_06_OnlySuffixesWithBackend) {
#if defined(Q_OS_DARWIN) && !defined(LF_GENERIC_PLATFORM) // ImageIO decides the suffixes
    for (const char* s : {"jpg", "jpeg", "jfif", "heic", "heif", "cr3", "jxl", "avif", "dng", "nef", "png",
                          "svg", "svgz", "tif", "webp"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
    for (const char* s : {"xcf", "x3f", "ari", "cap", "pdf", "txt"})
        EXPECT_FALSE(isImageSuffix(QString::fromLatin1(s))) << s;
    for (const char* s : {"kra", "ora", "qoi", "pcx", "pfm", "sgi"})
        EXPECT_TRUE(isImageSuffix(QString::fromLatin1(s))) << s;
    EXPECT_FALSE(hasImageSuffix("x.xcf"));
    EXPECT_TRUE(hasImageSuffix("IMG_0001.HEIC"));
    for (const QString& s : imageSuffixes())
        EXPECT_EQ(s, s.toLower());
#elif defined(Q_OS_DARWIN)
    // LF_FORCE_GENERIC_PLATFORM on macOS: ImageFormats still adds every suffix the
    // registry knows (incl. all Qt image plugins), so only check the defaults are in.
    const QStringList all = imageSuffixes();
    for (const QString& s : defaultImageSuffixes())
        EXPECT_TRUE(all.contains(s)) << s.toStdString();
#else
    EXPECT_EQ(imageSuffixes(), defaultImageSuffixes());
#endif
}
