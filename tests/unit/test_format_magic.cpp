// SPDX-License-Identifier: GPL-3.0-or-later
// Formats ImageIO reads but does not recognise from the first bytes (DDS), and the other
// GPU texture containers / Radiance: recognised by their magic number (SPEC 4.4). Stable
// metadata keys of the ImageIO backend (F-INFO-02).

#include "CorpusTestUtil.h"
#include "core/Decoder.h"
#include "platform/mac/ImageIODecoder.h"

#include <QFile>
#include <gtest/gtest.h>

using namespace lf;
using namespace lf::test;

TEST(ImageIODecoder, F_FMT_01_TextureContainersRecognisedByMagic) {
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray("DDS \x7c\0\0\0", 8) + QByteArray(56, '\0')),
              QStringLiteral("com.microsoft.dds"));
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray("\xabKTX 11\xbb\r\n\x1a\n", 12)),
              QStringLiteral("org.khronos.ktx"));
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray("\xabKTX 20\xbb\r\n\x1a\n", 12)),
              QStringLiteral("org.khronos.ktx2"));
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray("\x13\xab\xa1\x5c", 4) + QByteArray(12, '\1')),
              QStringLiteral("org.khronos.astc"));
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray("PVR\x03", 4)), QStringLiteral("public.pvr"));
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray(44, '\0') + "PVR!"), QStringLiteral("public.pvr"));
    EXPECT_EQ(ImageIODecoder::typeByMagic("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n"),
              QStringLiteral("public.radiance"));
    EXPECT_EQ(ImageIODecoder::typeByMagic("#?RGBE\n"), QStringLiteral("public.radiance"));
    EXPECT_EQ(ImageIODecoder::typeByMagic("DDS"), QString()); // too short
    EXPECT_EQ(ImageIODecoder::typeByMagic(QByteArray("\xff\xd8\xff\xe0", 4)), QString());
}

// The corpus files are found by the registry from their first 64 bytes (what the folder
// scan and the decoder selection see) and decode; DDS failed before (ImageIO alone does
// not recognise it from 64 bytes).
TEST(ImageIODecoder, F_FMT_01_TextureContainersDecodeThroughRegistry) {
    LF_REQUIRE_CORPUS();
    for (const char* name : {"quad.dds", "quad.ktx", "quad.ktx2", "quad.hdr", "quad16.psd"}) {
        SCOPED_TRACE(name);
        const QString path = corpusFile(QString::fromLatin1(name));
        if (!QFile::exists(path))
            continue; // optional encoder missing
        EXPECT_TRUE(ImageIODecoder().canDecode(path, fileHeader(path, 64)));
        EXPECT_TRUE(DecoderRegistry::instance().isSupported(path));
        QString error;
        const auto img = DecoderRegistry::instance().decode(path, DecodeRequest{}, &error);
        ASSERT_TRUE(img.has_value()) << error.toStdString();
        EXPECT_EQ(img->fullSize, QSize(64, 48));
    }
}

// F-INFO-02: metadata rows carry stable keys; labels and groups come from one table (the
// German labels of `groups` stay as they were).
TEST(ImageIODecoder, F_INFO_02_MetadataStableKeysAndLabels) {
    LF_REQUIRE_CORPUS();
    ImageIODecoder dec;
    const Metadata md = dec.metadata(corpusFile(QStringLiteral("exif.jpg")));
    auto entry = [&](const QString& key) -> const MetadataEntry* {
        for (const MetadataEntry& e : md.entries)
            if (e.key == key)
                return &e;
        return nullptr;
    };
    const MetadataEntry* model = entry(QStringLiteral("camera.model"));
    ASSERT_TRUE(model);
    EXPECT_EQ(model->label, QStringLiteral("Modell"));
    EXPECT_EQ(model->value.toString(), QStringLiteral("Synthetic Camera X1"));
    ASSERT_TRUE(entry(QStringLiteral("shot.exposureTime")));
    EXPECT_EQ(entry(QStringLiteral("shot.exposureTime"))->value.toString(), QStringLiteral("1/250 s"));
    ASSERT_TRUE(entry(QStringLiteral("file.rating")));
    EXPECT_EQ(entry(QStringLiteral("file.rating"))->value.toInt(), 4);
    ASSERT_TRUE(entry(QStringLiteral("gps.altitude")));
    // Every row is also in `groups` under the labels of its group and key.
    const QStringList keys = metadataKeys();
    for (const MetadataEntry& e : md.entries) {
        SCOPED_TRACE(e.key.toStdString());
        EXPECT_TRUE(keys.contains(e.key));
        const QString group = metadataGroupLabel(e.key.section(QLatin1Char('.'), 0, 0));
        EXPECT_EQ(md.groups.value(group).toMap().value(e.label), e.value);
    }
    EXPECT_TRUE(md.entries.first().key.startsWith(QStringLiteral("file.")));
    for (const QString& k : keys)
        EXPECT_FALSE(metadataLabel(k).isEmpty()) << k.toStdString();
    EXPECT_EQ(metadataGroupLabel(QStringLiteral("camera")), QStringLiteral("Kamera"));
    EXPECT_EQ(metadataLabel(QStringLiteral("no.such")), QString());

    Metadata m;
    addMetadata(m, QStringLiteral("shot.iso"), QStringLiteral("100"));
    addMetadata(m, QStringLiteral("shot.iso"), QStringLiteral("200"));
    ASSERT_EQ(m.entries.size(), 1);
    EXPECT_EQ(m.entries.first().value.toString(), QStringLiteral("200"));
    EXPECT_EQ(m.groups.value(QStringLiteral("Aufnahme")).toMap().value(QStringLiteral("ISO")).toString(),
              QStringLiteral("200"));
}
