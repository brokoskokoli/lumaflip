// SPDX-License-Identifier: GPL-3.0-or-later
// Ratings and culling (F-CULL-01..05), quick move/copy (F-FILE-08/11) and the
// photographer preset (F-IN-18). All file work happens in temporary folders; the trash is
// a fake that moves into a temporary folder.

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/FileBatch.h"
#include "core/FileOps.h"
#include "core/InputBindings.h"
#include "core/LosslessTransform.h"
#include "core/Platform.h"
#include "core/QuickMove.h"
#include "core/Rating.h"
#include "core/RatingStore.h"
#include "core/Settings.h"
#include "core/Xmp.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QUuid>

#include <gtest/gtest.h>

using namespace lf;
using lf::test::corpusFile;

namespace {

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

Rating stars(int n, PickFlag flag = PickFlag::None) {
    Rating r;
    r.stars = n;
    r.flag = flag;
    return r;
}

// Moves "trashed" files into a temporary folder (never the user's trash).
class FolderTrash : public TrashService {
public:
    FolderTrash() { dir.setAutoRemove(true); }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        const QString target =
            QDir(dir.path())
                .filePath(QUuid::createUuid().toString(QUuid::Id128) + "-" + QFileInfo(path).fileName());
        if (!QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("trash failed");
            return std::nullopt;
        }
        ++trashed;
        return target;
    }
    bool restoreFromTrash(const QString& trashedPath, const QString& original, QString* error) override {
        if (QFileInfo::exists(original) || !QFile::rename(trashedPath, original)) {
            if (error)
                *error = QStringLiteral("restore failed");
            return false;
        }
        return true;
    }
    QTemporaryDir dir;
    int trashed = 0;
};

const char* kLightroomSidecar = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Adobe XMP Core 7.0\">\n"
                                " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                                "  <rdf:Description rdf:about=\"\"\n"
                                "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
                                "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
                                "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
                                "    xmp:Rating=\"2\"\n"
                                "    xmp:Label=\"Rot\"\n"
                                "    crs:Exposure2012=\"+0.35\"\n"
                                "    crs:WhiteBalance=\"As Shot\">\n"
                                "   <dc:subject>\n"
                                "    <rdf:Bag>\n"
                                "     <rdf:li>Urlaub &amp; Meer</rdf:li>\n"
                                "    </rdf:Bag>\n"
                                "   </dc:subject>\n"
                                "   <!-- develop history -->\n"
                                "  </rdf:Description>\n"
                                " </rdf:RDF>\n"
                                "</x:xmpmeta>\n";

// Everything of `original` except the attribute xmp:Rating must still be there unchanged.
QString withoutRatingProps(QString s) {
    static const QRegularExpression re(
        QStringLiteral("\\s+(xmp|lumaflip|xmlns:lumaflip)(:Rating|:Pick|:Stars|)=\"[^\"]*\""));
    return s.remove(re);
}

class CullingTest : public ::testing::Test {
protected:
    void SetUp() override {
        lf::test::ensureQtApp();
        ASSERT_TRUE(dir.isValid());
        Settings::useTestingBackend(dir.filePath(QStringLiteral("settings.ini")));
    }
    QString path(const QString& name) const { return QDir(dir.path()).filePath(name); }
    QString makeJpeg(const QString& name, int w = 64, int h = 48) const {
        QImage img(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                img.setPixel(x, y, qRgb(x * 4, y * 5, (x + y) * 2));
        const QString p = path(name);
        return img.save(p, "JPEG", 90) ? p : QString();
    }
    QTemporaryDir dir;
};

} // namespace

// ---- F-CULL-01: rating model ----

TEST(CullingModel, F_CULL_01_ActionsSetStarsAndFlags) {
    Rating r;
    EXPECT_TRUE(r.isEmpty());
    r = rating::apply(r, Action::Rate3);
    EXPECT_EQ(r, stars(3));
    r = rating::apply(r, Action::RatePick);
    EXPECT_EQ(r, stars(3, PickFlag::Pick));
    r = rating::apply(r, Action::RateReject);
    EXPECT_EQ(r, stars(3, PickFlag::Reject)); // stars stay while rejected
    r = rating::apply(r, Action::RateClear);
    EXPECT_EQ(r, stars(3)); // U resets the flag only
    r = rating::apply(r, Action::Rate0);
    EXPECT_TRUE(r.isEmpty());
    r = rating::apply(r, Action::Rate5);
    EXPECT_EQ(r.stars, 5);
    EXPECT_EQ(rating::apply(r, Action::NextImage), r); // not a rating action
    for (Action a : {Action::Rate0, Action::Rate1, Action::Rate2, Action::Rate3, Action::Rate4, Action::Rate5,
                     Action::RatePick, Action::RateReject, Action::RateClear})
        EXPECT_TRUE(rating::isRatingAction(a));
    EXPECT_FALSE(rating::isRatingAction(Action::ZoomFit));
}

TEST(CullingModel, F_CULL_01_DescribeAndSortValue) {
    EXPECT_EQ(rating::describe(Rating()), QStringLiteral("–"));
    EXPECT_TRUE(rating::describe(stars(3)).startsWith(QStringLiteral("★★★☆☆")));
    EXPECT_TRUE(rating::describe(stars(0, PickFlag::Pick)).contains(QStringLiteral("Pick")));
    EXPECT_LT(rating::sortValue(stars(5, PickFlag::Reject)), rating::sortValue(Rating()));
    EXPECT_LT(rating::sortValue(stars(2)), rating::sortValue(stars(2, PickFlag::Pick)));
    EXPECT_LT(rating::sortValue(stars(2, PickFlag::Pick)), rating::sortValue(stars(3)));
}

TEST(CullingModel, F_CULL_01_RatingKeysAvailableNow) {
    for (Action a : {Action::Rate0, Action::Rate5, Action::RatePick, Action::RateReject, Action::RateClear,
                     Action::ToggleCullingMode, Action::QuickMove, Action::QuickCopy})
        EXPECT_TRUE(InputBindings::isActionAvailable(a)) << actionId(a).toStdString();
    EXPECT_TRUE(InputBindings::isChoiceAvailable(QStringLiteral("ratingKeys"), QStringLiteral("always")));
}

// ---- F-CULL-03 ----

TEST(CullingModel, F_CULL_03_AutoAdvanceAfterRating) {
    const rating::Step on = rating::cullingStep(Rating(), Action::Rate4, true);
    EXPECT_EQ(on.rating.stars, 4);
    EXPECT_TRUE(on.advance);
    EXPECT_FALSE(rating::cullingStep(Rating(), Action::Rate4, false).advance);
    EXPECT_TRUE(rating::cullingStep(Rating(), Action::RateReject, true).advance);
    EXPECT_FALSE(rating::cullingStep(Rating(), Action::NextImage, true).advance);
}

// ---- F-CULL-04: filter logic ----

TEST(CullingModel, F_CULL_04_FilterMatches) {
    using K = RatingFilter::Kind;
    const RatingFilter all;
    EXPECT_FALSE(all.isActive());
    EXPECT_TRUE(all.matches(stars(0, PickFlag::Reject)));
    const RatingFilter picks{K::Picks, 1};
    EXPECT_TRUE(picks.matches(stars(0, PickFlag::Pick)));
    EXPECT_FALSE(picks.matches(stars(5)));
    const RatingFilter min3{K::MinStars, 3};
    EXPECT_TRUE(min3.matches(stars(3)));
    EXPECT_TRUE(min3.matches(stars(5, PickFlag::Pick)));
    EXPECT_FALSE(min3.matches(stars(2)));
    EXPECT_FALSE(min3.matches(stars(4, PickFlag::Reject))); // rejects never count as "good"
    const RatingFilter unrated{K::Unrated, 1};
    EXPECT_TRUE(unrated.matches(Rating()));
    EXPECT_FALSE(unrated.matches(stars(1)));
    EXPECT_FALSE(unrated.matches(stars(0, PickFlag::Pick)));
    const RatingFilter rejects{K::Rejects, 1};
    EXPECT_TRUE(rejects.matches(stars(2, PickFlag::Reject)));
    EXPECT_FALSE(rejects.matches(Rating()));
    for (const RatingFilter& f : {all, picks, min3, unrated, rejects})
        EXPECT_EQ(RatingFilter::fromId(f.id()), f) << f.id().toStdString();
    EXPECT_EQ(min3.describe(), QStringLiteral("≥ 3 ★"));
    EXPECT_FALSE(RatingFilter::fromId(QStringLiteral("min9")).isActive());
}

// ---- F-CULL-02 (a): XMP sidecars ----

TEST(CullingXmp, F_CULL_02_SidecarNamingVariants) {
    EXPECT_EQ(rating::sidecarPath("/p/IMG_1.CR3"), QStringLiteral("/p/IMG_1.xmp"));
    EXPECT_EQ(rating::sidecarPath("/p/a.b.jpg"), QStringLiteral("/p/a.b.xmp"));
    EXPECT_EQ(rating::sidecarPathWithExtension("/p/IMG_1.CR3"), QStringLiteral("/p/IMG_1.CR3.xmp"));
    const QStringList c = rating::sidecarCandidates("/p/IMG_1.CR3");
    ASSERT_GE(c.size(), 2);
    EXPECT_EQ(c[0], QStringLiteral("/p/IMG_1.CR3.xmp"));
    EXPECT_EQ(c[1], QStringLiteral("/p/IMG_1.xmp"));
}

TEST(CullingXmp, F_CULL_02_ReadWritePreservesForeignXmp) {
    const QString original = QString::fromUtf8(kLightroomSidecar);
    EXPECT_EQ(xmp::readRating(original), stars(2));
    const auto out = xmp::writeRating(original, stars(4, PickFlag::Pick));
    ASSERT_TRUE(out);
    EXPECT_EQ(xmp::readRating(*out), stars(4, PickFlag::Pick));
    EXPECT_TRUE(xmp::isWellFormed(*out));
    // Foreign content byte for byte: develop settings, label, keywords, comment, layout.
    for (const char* kept : {"crs:Exposure2012=\"+0.35\"", "xmp:Label=\"Rot\"", "Urlaub &amp; Meer",
                             "<!-- develop history -->", "x:xmptk=\"Adobe XMP Core 7.0\""})
        EXPECT_TRUE(out->contains(QString::fromUtf8(kept))) << kept;
    EXPECT_TRUE(out->contains(QStringLiteral("\n    xmp:Rating=\"4\"")));
    EXPECT_EQ(withoutRatingProps(*out), withoutRatingProps(original));
    // Back to the original rating: only our attributes differ.
    const auto back = xmp::writeRating(*out, stars(2));
    ASSERT_TRUE(back);
    EXPECT_EQ(*back, original);
}

TEST(CullingXmp, F_CULL_02_ElementFormOtherPrefixAndRejectInterop) {
    const QString elementForm = QStringLiteral(
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
        "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description rdf:about=\"\" xmlns:xap=\"http://ns.adobe.com/xap/1.0/\" "
        "xmlns:darktable=\"http://darktable.sf.net/\" darktable:xmp_version=\"5\">"
        "<xap:Rating>3</xap:Rating></rdf:Description></rdf:RDF></x:xmpmeta>\n<?xpacket end=\"w\"?>");
    EXPECT_EQ(xmp::readRating(elementForm), stars(3));
    // Reject: xmp:Rating = -1 (Bridge/darktable), stars kept in lumaflip:Stars.
    const auto rejected = xmp::writeRating(elementForm, stars(3, PickFlag::Reject));
    ASSERT_TRUE(rejected);
    EXPECT_TRUE(rejected->contains(QStringLiteral("<xap:Rating>-1</xap:Rating>")));
    EXPECT_TRUE(rejected->contains(QStringLiteral("darktable:xmp_version=\"5\"")));
    EXPECT_TRUE(rejected->contains(QStringLiteral("xmlns:lumaflip=\"http://ns.lumaflip.app/xmp/1.0/\"")));
    EXPECT_EQ(xmp::readRating(*rejected), stars(3, PickFlag::Reject));
    // Another program only knows xmp:Rating = -1: a reject without stars.
    const QString foreignReject = QString(elementForm).replace(QStringLiteral(">3<"), QStringLiteral(">-1<"));
    EXPECT_EQ(xmp::readRating(foreignReject), stars(0, PickFlag::Reject));
    // Another program changed the rating after our reject: xmp:Rating wins.
    const QString changed = QString(*rejected).replace(QStringLiteral(">-1<"), QStringLiteral(">5<"));
    EXPECT_EQ(xmp::readRating(changed), stars(5));
    // Clearing removes our properties, keeps the packet wrapper.
    const auto cleared = xmp::writeRating(*rejected, Rating());
    ASSERT_TRUE(cleared);
    EXPECT_FALSE(xmp::readRating(*cleared).has_value());
    EXPECT_TRUE(cleared->startsWith(QStringLiteral("<?xpacket begin")));
    EXPECT_TRUE(cleared->contains(QStringLiteral("darktable:xmp_version=\"5\"")));
}

TEST(CullingXmp, F_CULL_02_MalformedSidecarIsNeverReplaced) {
    EXPECT_FALSE(xmp::writeRating(QStringLiteral("<x:xmpmeta><rdf:RDF>"), stars(1)).has_value());
    EXPECT_FALSE(xmp::writeRating(QStringLiteral("<root/>"), stars(1)).has_value()); // no rdf:RDF
    EXPECT_FALSE(xmp::readRating(QStringLiteral("not xml")).has_value());
    const QString fresh = xmp::newSidecar(stars(2, PickFlag::Pick));
    EXPECT_EQ(xmp::readRating(fresh), stars(2, PickFlag::Pick));
    EXPECT_TRUE(xmp::isWellFormed(fresh));
}

TEST_F(CullingTest, F_CULL_02_SidecarFilesBothNamingVariants) {
    const QString raw = path("IMG_1.CR3");
    ASSERT_TRUE(writeFile(raw, "raw"));
    // Adobe naming: created as IMG_1.xmp, foreign content kept.
    ASSERT_TRUE(writeFile(path("IMG_1.xmp"), kLightroomSidecar));
    EXPECT_EQ(ratingio::readSidecar(raw), stars(2));
    EXPECT_TRUE(ratingio::writeSidecar(raw, stars(5)).isEmpty());
    const QString text = QString::fromUtf8(readFile(path("IMG_1.xmp")));
    EXPECT_TRUE(text.contains(QStringLiteral("crs:Exposure2012=\"+0.35\"")));
    EXPECT_EQ(ratingio::readSidecar(raw), stars(5));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.CR3.xmp")));

    // darktable naming "IMG_2.CR3.xmp" is read and updated in place.
    const QString raw2 = path("IMG_2.CR3");
    ASSERT_TRUE(writeFile(raw2, "raw"));
    ASSERT_TRUE(writeFile(path("IMG_2.CR3.xmp"), xmp::newSidecar(stars(1)).toUtf8()));
    EXPECT_EQ(ratingio::readSidecar(raw2), stars(1));
    EXPECT_TRUE(ratingio::writeSidecar(raw2, stars(0, PickFlag::Reject)).isEmpty());
    EXPECT_EQ(ratingio::readSidecar(raw2), stars(0, PickFlag::Reject));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_2.xmp")));

    // New file: "<base>.xmp"; a JPEG next to a RAW of the same name gets its own sidecar.
    const QString jpg = path("IMG_1.JPG");
    ASSERT_TRUE(writeFile(jpg, "jpeg"));
    QFile::remove(path("IMG_1.xmp"));
    EXPECT_TRUE(ratingio::writeSidecar(jpg, stars(3)).isEmpty());
    EXPECT_TRUE(QFileInfo::exists(path("IMG_1.JPG.xmp")));
    EXPECT_FALSE(QFileInfo::exists(path("IMG_1.xmp")));
    // Unrated without a sidecar: nothing is created.
    const QString lone = path("lone.png");
    ASSERT_TRUE(writeFile(lone, "png"));
    EXPECT_TRUE(ratingio::writeSidecar(lone, Rating()).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(path("lone.xmp")));
    // A broken sidecar is left alone (error, file unchanged).
    ASSERT_TRUE(writeFile(path("lone.xmp"), "<x:xmpmeta><broken>"));
    EXPECT_FALSE(ratingio::writeSidecar(lone, stars(1)).isEmpty());
    EXPECT_EQ(readFile(path("lone.xmp")), QByteArray("<x:xmpmeta><broken>"));
}

// ---- F-CULL-02 (b): embedded XMP ----

TEST_F(CullingTest, F_CULL_02_EmbeddedJpegKeepsScanDataByteIdentical) {
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "embedded writes need the platform's lossless metadata copy";
#else
    const QString jpg = makeJpeg(QStringLiteral("embedded.jpg"));
    ASSERT_FALSE(jpg.isEmpty());
    const QByteArray before = readFile(jpg);
    QString reason;
    ASSERT_TRUE(ratingio::canEmbed(jpg, &reason)) << reason.toStdString();
    bool refused = true;
    ASSERT_TRUE(ratingio::writeEmbedded(jpg, stars(4, PickFlag::Pick), true, &refused).isEmpty());
    EXPECT_FALSE(refused);
    const QByteArray after = readFile(jpg);
    EXPECT_NE(after, before);
    EXPECT_FALSE(jpeg::scanData(before).isEmpty());
    EXPECT_EQ(jpeg::scanData(after), jpeg::scanData(before)); // no re-encoding
    EXPECT_EQ(ratingio::readEmbedded(jpg), stars(4, PickFlag::Pick));
    // Change and clear the flag (removal through the merge).
    ASSERT_TRUE(ratingio::writeEmbedded(jpg, stars(2), true).isEmpty());
    EXPECT_EQ(ratingio::readEmbedded(jpg), stars(2));
    ASSERT_TRUE(ratingio::writeEmbedded(jpg, stars(1, PickFlag::Reject), true).isEmpty());
    EXPECT_EQ(ratingio::readEmbedded(jpg), stars(1, PickFlag::Reject));
    EXPECT_EQ(jpeg::scanData(readFile(jpg)), jpeg::scanData(before));
    EXPECT_FALSE(QFileInfo::exists(path("embedded.xmp"))); // no sidecar needed
    // No temporary files left behind.
    EXPECT_EQ(
        QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).filter(QStringLiteral("lumaflip")).size(), 0);
#endif
}

// Same safety as saving the rotation: symbolic links stay links, the modification date is
// kept on request, files with a gain map are never damaged (written unchanged or refused).
TEST_F(CullingTest, F_CULL_02_EmbeddedWriteSafety) {
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "embedded writes need the platform's lossless metadata copy";
#else
    const QString jpg = makeJpeg(QStringLiteral("real.jpg"));
    ASSERT_FALSE(jpg.isEmpty());
    const QDateTime old = QDateTime::currentDateTimeUtc().addDays(-3);
    {
        QFile f(jpg);
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_TRUE(f.setFileTime(old, QFileDevice::FileModificationTime));
    }
    const QString link = path("link.jpg");
    ASSERT_TRUE(QFile::link(jpg, link));
    ASSERT_TRUE(ratingio::writeEmbedded(link, stars(2), true).isEmpty());
    EXPECT_TRUE(QFileInfo(link).isSymLink());
    EXPECT_EQ(ratingio::readEmbedded(jpg), stars(2));
    EXPECT_EQ(QFileInfo(jpg).lastModified().toSecsSinceEpoch(), old.toSecsSinceEpoch());
    ASSERT_TRUE(ratingio::writeEmbedded(jpg, stars(3), false).isEmpty());
    EXPECT_GT(QFileInfo(jpg).lastModified().toSecsSinceEpoch(), old.toSecsSinceEpoch());

    if (QFileInfo::exists(corpusFile(QStringLiteral("gainmap.jpg")))) {
        const QString gm = path("gainmap.jpg");
        ASSERT_TRUE(QFile::copy(corpusFile(QStringLiteral("gainmap.jpg")), gm));
        QFile(gm).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QByteArray before = readFile(gm);
        bool refused = false;
        const QString e = ratingio::writeEmbedded(gm, stars(4), false, &refused);
        if (e.isEmpty()) // everything behind the primary image's SOS (the gain map) unchanged
            EXPECT_EQ(jpeg::scanData(readFile(gm)), jpeg::scanData(before));
        else
            EXPECT_EQ(readFile(gm), before) << e.toStdString();
    }
#endif
}

TEST_F(CullingTest, F_CULL_02_EmbeddedHeicPngTiff) {
#ifdef LF_GENERIC_PLATFORM
    GTEST_SKIP() << "embedded writes need the platform's lossless metadata copy";
#else
    QStringList files;
    {
        QImage img(32, 24, QImage::Format_ARGB32);
        img.fill(qRgba(10, 200, 30, 255));
        ASSERT_TRUE(img.save(path("still.png"), "PNG"));
        files << path("still.png");
    }
    for (const char* name : {"still.heic", "rgb16.tiff"}) {
        if (QFileInfo::exists(corpusFile(QString::fromLatin1(name)))) {
            ASSERT_TRUE(QFile::copy(corpusFile(QString::fromLatin1(name)), path(QString::fromLatin1(name))));
            QFile(path(QString::fromLatin1(name)))
                .setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            files << path(QString::fromLatin1(name));
        }
    }
    for (const QString& f : std::as_const(files)) {
        SCOPED_TRACE(f.toStdString());
        const QImage pixelsBefore(f);
        QString reason;
        ASSERT_TRUE(ratingio::canEmbed(f, &reason)) << reason.toStdString();
        bool refused = false;
        const QString e = ratingio::writeEmbedded(f, stars(5, PickFlag::Pick), false, &refused);
        ASSERT_TRUE(e.isEmpty()) << e.toStdString();
        EXPECT_EQ(ratingio::readEmbedded(f), stars(5, PickFlag::Pick));
        if (!pixelsBefore.isNull())
            EXPECT_EQ(QImage(f).convertToFormat(QImage::Format_ARGB32),
                      pixelsBefore.convertToFormat(QImage::Format_ARGB32));
        const QString e2 = ratingio::writeEmbedded(f, Rating(), false);
        ASSERT_TRUE(e2.isEmpty()) << e2.toStdString();
        const auto cleared = ratingio::readEmbedded(f);
        EXPECT_TRUE(!cleared || cleared->isEmpty());
    }
#endif
}

TEST_F(CullingTest, F_CULL_02_RawRefusedFallsBackToSidecar) {
    const QString raw = path("DSC_0001.NEF");
    ASSERT_TRUE(writeFile(raw, "not really a raw file"));
    const QByteArray before = readFile(raw);
    QString reason;
    EXPECT_FALSE(ratingio::canEmbed(raw, &reason));
    EXPECT_FALSE(reason.isEmpty());
    RatingOptions o;
    o.sidecar = false;
    o.embedded = true;
    const RatingWriteResult res = ratingio::write(raw, stars(3), o);
    EXPECT_TRUE(res.ok()) << res.error.toStdString();
    EXPECT_FALSE(res.notice.isEmpty()); // the fallback is shown to the user
    EXPECT_EQ(readFile(raw), before);   // RAW untouched
    EXPECT_TRUE(QFileInfo::exists(path("DSC_0001.xmp")));
    EXPECT_EQ(ratingio::read(raw, o), stars(3)); // read back through the fallback
}

// ---- F-CULL-02 (c): Finder tags ----

TEST(CullingTags, F_CULL_02_FinderTagsMapping) {
    RatingTagNames names;
    const QStringList existing{QStringLiteral("Urlaub"), QStringLiteral("Rot\n6"), QStringLiteral("★★")};
    bool found = false;
    EXPECT_EQ(rating::fromTags(existing, names, &found), stars(2));
    EXPECT_TRUE(found);
    const QStringList next = rating::applyToTags(existing, stars(4, PickFlag::Pick), names);
    EXPECT_EQ(next, (QStringList{QStringLiteral("Urlaub"), QStringLiteral("Rot\n6"), QStringLiteral("★★★★"),
                                 QStringLiteral("Pick\n2")}));
    EXPECT_EQ(rating::fromTags(next, names), stars(4, PickFlag::Pick));
    const QStringList rejected = rating::applyToTags(next, stars(0, PickFlag::Reject), names);
    EXPECT_EQ(rejected,
              (QStringList{QStringLiteral("Urlaub"), QStringLiteral("Rot\n6"), QStringLiteral("Reject\n6")}));
    EXPECT_EQ(rating::applyToTags(rejected, Rating(), names),
              (QStringList{QStringLiteral("Urlaub"), QStringLiteral("Rot\n6")}));
    rating::fromTags({QStringLiteral("Urlaub")}, names, &found);
    EXPECT_FALSE(found);
    // Configurable names and colors.
    RatingTagNames custom;
    custom.starSymbol = QStringLiteral("*");
    custom.pick = QStringLiteral("Behalten");
    custom.pickColor = 4;
    EXPECT_EQ(rating::applyToTags({}, stars(3, PickFlag::Pick), custom),
              (QStringList{QStringLiteral("***"), QStringLiteral("Behalten\n4")}));
    EXPECT_EQ(rating::tagName(QStringLiteral("Behalten\n4")), QStringLiteral("Behalten"));
    EXPECT_EQ(rating::tagColor(QStringLiteral("Behalten\n4")), 4);
}

TEST_F(CullingTest, F_CULL_02_FinderTagsOnTempFile) {
    if (!platform::tagsSupported())
        GTEST_SKIP() << "no Finder tags on this platform";
    const QString f = path("tagged.jpg");
    ASSERT_TRUE(writeFile(f, "x"));
    QString error;
    ASSERT_TRUE(platform::writeRawTags(f, {QStringLiteral("Urlaub")}, &error)) << error.toStdString();
    RatingTagNames names;
    EXPECT_TRUE(ratingio::writeTags(f, stars(3, PickFlag::Pick), names).isEmpty());
    EXPECT_EQ(platform::readRawTags(f, &error).value_or(QStringList()),
              (QStringList{QStringLiteral("Urlaub"), QStringLiteral("★★★"), QStringLiteral("Pick\n2")}));
    EXPECT_EQ(ratingio::readTags(f, names), stars(3, PickFlag::Pick));
    // The names are visible to the regular tag API (F-INFO-06).
    auto service = createTagService();
    EXPECT_EQ(service->tags(f, &error).value_or(QStringList()),
              (QStringList{QStringLiteral("Urlaub"), QStringLiteral("★★★"), QStringLiteral("Pick")}));
    EXPECT_TRUE(ratingio::writeTags(f, Rating(), names).isEmpty());
    EXPECT_EQ(platform::readRawTags(f, &error).value_or(QStringList()),
              QStringList{QStringLiteral("Urlaub")});
}

// ---- F-CULL-02: conflict rule ----

TEST(CullingStores, F_CULL_02_ConflictResolutionOrder) {
    RatingOptions all;
    all.sidecar = all.embedded = all.finderTags = true;
    const auto side = std::optional<Rating>(stars(1));
    const auto emb = std::optional<Rating>(stars(2));
    const auto tag = std::optional<Rating>(stars(3));
    EXPECT_EQ(ratingio::resolve(all, side, emb, tag), stars(1));
    EXPECT_EQ(ratingio::resolve(all, std::nullopt, emb, tag), stars(2)); // first with a value
    EXPECT_EQ(ratingio::resolve(all, std::nullopt, std::nullopt, tag), stars(3));
    RatingOptions noSidecar = all;
    noSidecar.sidecar = false;
    EXPECT_EQ(ratingio::resolve(noSidecar, side, emb, tag), stars(2)); // disabled stores are ignored
    RatingOptions tagsOnly;
    tagsOnly.sidecar = false;
    tagsOnly.finderTags = true;
    EXPECT_EQ(ratingio::resolve(tagsOnly, side, emb, tag), stars(3));
    EXPECT_FALSE(ratingio::resolve(tagsOnly, side, emb, std::nullopt).has_value());
}

TEST_F(CullingTest, F_CULL_02_ConflictResolutionOnFiles) {
    const QString f = path("both.jpg");
    ASSERT_TRUE(writeFile(f, "x"));
    ASSERT_TRUE(writeFile(path("both.xmp"), xmp::newSidecar(stars(2)).toUtf8()));
    RatingOptions o;
    o.sidecar = true;
    o.finderTags = platform::tagsSupported();
    if (o.finderTags) {
        ASSERT_TRUE(ratingio::writeTags(f, stars(5), o.tags).isEmpty());
        EXPECT_EQ(ratingio::read(f, o), stars(2)); // sidecar first
        o.sidecar = false;
        EXPECT_EQ(ratingio::read(f, o), stars(5));
        o.sidecar = true;
    }
    // Writing goes to every enabled store.
    ASSERT_TRUE(ratingio::write(f, stars(4), o).ok());
    EXPECT_EQ(ratingio::readSidecar(f), stars(4));
    if (o.finderTags)
        EXPECT_EQ(ratingio::readTags(f, o.tags), stars(4));
    // readMany lists the folder's sidecars once and gives the same result.
    const QHash<QString, Rating> many = ratingio::readMany({f, path("nothing.jpg")}, o);
    EXPECT_EQ(many.value(f), stars(4));
    EXPECT_TRUE(many.value(path("nothing.jpg")).isEmpty());
}

TEST_F(CullingTest, F_CULL_02_AsyncWriteIsUndoable) {
    FolderTrash trash;
    FileOps ops(trash);
    RatingStore store;
    RatingOptions o;
    store.setOptions(o);
    const QString f = path("undo.jpg");
    ASSERT_TRUE(writeFile(f, "x"));
    QList<Rating> changes;
    QObject::connect(&store, &RatingStore::ratingChanged,
                     [&](const QString&, const Rating& r) { changes << r; });
    bool done = false, ok = false;
    store.setRating(ops, f, stars(3, PickFlag::Pick), [&](bool success, const QString&, const QString&) {
        done = true;
        ok = success;
    });
    ASSERT_TRUE(lf::test::waitUntil([&] { return done; }));
    EXPECT_TRUE(ok);
    EXPECT_EQ(ratingio::readSidecar(f), stars(3, PickFlag::Pick));
    EXPECT_EQ(store.cached(f), stars(3, PickFlag::Pick));
    ASSERT_EQ(ops.undoStack()->count(), 1);
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 0; }));
    const auto afterUndo = ratingio::readSidecar(f);
    EXPECT_TRUE(!afterUndo || afterUndo->isEmpty());
    EXPECT_EQ(store.cached(f), Rating());
    ops.redo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 1; }));
    EXPECT_EQ(ratingio::readSidecar(f), stars(3, PickFlag::Pick));
    ASSERT_EQ(changes.size(), 3);
    EXPECT_EQ(changes.last(), stars(3, PickFlag::Pick));
}

// ---- F-CULL-05: bulk actions ----

TEST_F(CullingTest, F_CULL_05_RejectsToTrashAsOneUndoStep) {
    FolderTrash trash;
    FileOps ops(trash);
    QStringList rejects;
    for (const char* n : {"a.jpg", "b.jpg", "c.jpg"}) {
        ASSERT_TRUE(writeFile(path(QString::fromLatin1(n)), n));
        rejects << path(QString::fromLatin1(n));
    }
    ASSERT_TRUE(writeFile(path("a.xmp"), xmp::newSidecar(stars(0, PickFlag::Reject)).toUtf8()));
    ASSERT_TRUE(writeFile(path("keep.jpg"), "keep"));
    QVector<TransferItem> items;
    for (const QString& p : rejects)
        items.append({p, {}, ConflictChoice::Skip});
    auto batch = std::make_shared<FileBatch>(trash, TransferKind::Trash, items);
    QStringList removed;
    QObject::connect(&ops, &FileOps::fileRemoved, [&](const QString& p) { removed << p; });
    bool done = false;
    runBatchAsync(ops, QStringLiteral("Verworfene in den Papierkorb"), batch, [&](bool ok, const QString& e) {
        EXPECT_TRUE(ok) << e.toStdString();
        done = true;
    });
    ASSERT_TRUE(lf::test::waitUntil([&] { return done; }));
    for (const QString& p : rejects)
        EXPECT_FALSE(QFileInfo::exists(p));
    EXPECT_FALSE(QFileInfo::exists(path("a.xmp"))); // the sidecar went with its image
    EXPECT_TRUE(QFileInfo::exists(path("keep.jpg")));
    EXPECT_EQ(removed.size(), 3);
    ASSERT_EQ(ops.undoStack()->count(), 1); // one step
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 0; }));
    for (const QString& p : rejects)
        EXPECT_TRUE(QFileInfo::exists(p)) << p.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path("a.xmp")));
    EXPECT_EQ(readFile(path("b.jpg")), QByteArray("b.jpg"));
}

TEST_F(CullingTest, F_CULL_05_RejectsToRejectedFolderAndUndo) {
    FolderTrash trash;
    FileOps ops(trash);
    ASSERT_TRUE(writeFile(path("r1.jpg"), "1"));
    ASSERT_TRUE(writeFile(path("r1.jpg.xmp"), xmp::newSidecar(stars(0, PickFlag::Reject)).toUtf8()));
    ASSERT_TRUE(writeFile(path("r2.CR3"), "2"));
    ASSERT_TRUE(writeFile(path("r2.xmp"), xmp::newSidecar(stars(0, PickFlag::Reject)).toUtf8()));
    ASSERT_TRUE(writeFile(path("r2.JPG"), "pair")); // never reads "r2.xmp" (the RAW's, Sidecars.h)
    const QString rejected = path("_Rejected");
    QVector<TransferItem> items{{path("r1.jpg"), rejected, ConflictChoice::KeepBoth},
                                {path("r2.CR3"), rejected, ConflictChoice::KeepBoth}};
    auto batch = std::make_shared<FileBatch>(trash, TransferKind::Move, items);
    bool done = false;
    runBatchAsync(ops, QStringLiteral("Verworfene verschieben"), batch, [&](bool ok, const QString&) {
        EXPECT_TRUE(ok);
        done = true;
    });
    ASSERT_TRUE(lf::test::waitUntil([&] { return done; }));
    EXPECT_TRUE(QFileInfo::exists(rejected + "/r1.jpg"));
    EXPECT_TRUE(QFileInfo::exists(rejected + "/r1.jpg.xmp"));
    EXPECT_TRUE(QFileInfo::exists(rejected + "/r2.CR3"));
    EXPECT_TRUE(QFileInfo::exists(rejected + "/r2.xmp")); // the RAW's Adobe sidecar goes along
    EXPECT_TRUE(QFileInfo::exists(path("r2.JPG")));
    EXPECT_EQ(trash.trashed, 0);
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 0; }));
    EXPECT_TRUE(QFileInfo::exists(path("r1.jpg")));
    EXPECT_TRUE(QFileInfo::exists(path("r1.jpg.xmp")));
    EXPECT_TRUE(QFileInfo::exists(path("r2.CR3")));
    EXPECT_FALSE(QFileInfo::exists(rejected)); // created by the batch, empty again: gone
    ops.redo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 1; }));
    EXPECT_TRUE(QFileInfo::exists(rejected + "/r1.jpg"));
}

// ---- F-FILE-08 / F-FILE-11: quick move/copy and conflicts ----

TEST(QuickMove, F_FILE_08_ChordMoveCopyAndRepeatLast) {
    QuickChord::setLastSlot(0);
    QuickChord c;
    EXPECT_EQ(c.feed(Qt::Key_3, Qt::NoModifier).kind, QuickChord::Kind::NotHandled); // nothing pending
    // M M without a previous target.
    c.start(QuickOp::Move);
    QuickChord::Result r = c.feed(Qt::Key_M, Qt::NoModifier);
    EXPECT_EQ(r.kind, QuickChord::Kind::Cancelled);
    EXPECT_TRUE(r.noLastTarget);
    // M 3 moves into target 3.
    EXPECT_EQ(c.start(QuickOp::Move).kind, QuickChord::Kind::Pending);
    EXPECT_EQ(c.feed(Qt::Key_Shift, Qt::ShiftModifier).kind, QuickChord::Kind::Pending); // modifier alone
    r = c.feed(Qt::Key_3, Qt::NoModifier);
    EXPECT_EQ(r.kind, QuickChord::Kind::Execute);
    EXPECT_EQ(r.op, QuickOp::Move);
    EXPECT_EQ(r.slot, 3);
    EXPECT_FALSE(c.isPending());
    // M M repeats the last target.
    c.start(QuickOp::Move);
    r = c.feed(Qt::Key_M, Qt::NoModifier);
    EXPECT_EQ(r.kind, QuickChord::Kind::Execute);
    EXPECT_EQ(r.slot, 3);
    EXPECT_TRUE(r.repeat);
    // C 7 copies; C C repeats (copy) the last target.
    c.start(QuickOp::Copy);
    r = c.feed(Qt::Key_7, Qt::NoModifier);
    EXPECT_EQ(r.op, QuickOp::Copy);
    EXPECT_EQ(r.slot, 7);
    c.start(QuickOp::Copy);
    r = c.feed(Qt::Key_C, Qt::NoModifier);
    EXPECT_EQ(r.kind, QuickChord::Kind::Execute);
    EXPECT_EQ(r.op, QuickOp::Copy);
    EXPECT_EQ(r.slot, 7);
    // Other keys cancel; Esc is swallowed, anything else is handled normally.
    c.start(QuickOp::Move);
    r = c.feed(Qt::Key_Escape, Qt::NoModifier);
    EXPECT_EQ(r.kind, QuickChord::Kind::Cancelled);
    EXPECT_FALSE(r.passThrough);
    c.start(QuickOp::Move);
    r = c.feed(Qt::Key_Right, Qt::NoModifier);
    EXPECT_TRUE(r.passThrough);
    c.start(QuickOp::Move);
    EXPECT_EQ(c.feed(Qt::Key_0, Qt::NoModifier).kind, QuickChord::Kind::Cancelled); // only 1..9
    c.start(QuickOp::Move);
    EXPECT_EQ(c.feed(Qt::Key_2, Qt::ControlModifier).kind, QuickChord::Kind::Cancelled);
}

TEST_F(CullingTest, F_FILE_08_TargetsFromSettings) {
    EXPECT_EQ(quickmove::targets().size(), 9);
    EXPECT_TRUE(quickmove::target(1).isEmpty());
    quickmove::setTarget(2, path("Auswahl"));
    EXPECT_EQ(quickmove::target(2), path("Auswahl"));
    EXPECT_EQ(quickmove::targets().at(1), path("Auswahl"));
    EXPECT_EQ(quickmove::label(2, path("Auswahl")), QStringLiteral("2  Auswahl"));
    EXPECT_TRUE(quickmove::label(4, QString()).contains(QStringLiteral("nicht festgelegt")));
    quickmove::setTarget(10, path("x")); // out of range: ignored
    EXPECT_TRUE(quickmove::target(10).isEmpty());
}

TEST_F(CullingTest, F_FILE_08_MoveAndCopyIntoTargetWithUndo) {
    FolderTrash trash;
    FileOps ops(trash);
    const QString target = path("Ziel");
    ASSERT_TRUE(QDir().mkpath(target));
    ASSERT_TRUE(writeFile(path("m.jpg"), "move"));
    ASSERT_TRUE(writeFile(path("c.jpg"), "copy"));
    auto run = [&](TransferKind kind, const QString& src) {
        bool done = false;
        auto b = std::make_shared<FileBatch>(trash, kind,
                                             QVector<TransferItem>{{src, target, ConflictChoice::Skip}});
        runBatchAsync(ops, QStringLiteral("x"), b, [&](bool ok, const QString&) {
            EXPECT_TRUE(ok);
            done = true;
        });
        EXPECT_TRUE(lf::test::waitUntil([&] { return done; }));
        return b;
    };
    run(TransferKind::Move, path("m.jpg"));
    EXPECT_FALSE(QFileInfo::exists(path("m.jpg")));
    EXPECT_EQ(readFile(target + "/m.jpg"), QByteArray("move"));
    run(TransferKind::Copy, path("c.jpg"));
    EXPECT_TRUE(QFileInfo::exists(path("c.jpg")));
    EXPECT_EQ(readFile(target + "/c.jpg"), QByteArray("copy"));
    ASSERT_EQ(ops.undoStack()->count(), 2);
    ops.undo();
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 0; }));
    EXPECT_TRUE(QFileInfo::exists(path("m.jpg")));
    EXPECT_FALSE(QFileInfo::exists(target + "/m.jpg"));
    EXPECT_FALSE(QFileInfo::exists(target + "/c.jpg"));
    EXPECT_TRUE(QFileInfo::exists(path("c.jpg")));
}

TEST(QuickMove, F_FILE_11_KeepBothNaming) {
    QSet<QString> taken{QStringLiteral("/t/IMG.jpg"), QStringLiteral("/t/IMG 2.jpg"),
                        QStringLiteral("/t/noext")};
    auto exists = [&](const QString& p) { return taken.contains(p); };
    EXPECT_EQ(filebatch::keepBothName("/t", "IMG.jpg", exists), QStringLiteral("IMG 3.jpg"));
    EXPECT_EQ(filebatch::keepBothName("/t", "free.jpg", exists), QStringLiteral("free.jpg"));
    EXPECT_EQ(filebatch::keepBothName("/t", "noext", exists), QStringLiteral("noext 2"));
    taken.insert(QStringLiteral("/t/a.b.png"));
    EXPECT_EQ(filebatch::keepBothName("/t", "a.b.png", exists), QStringLiteral("a.b 2.png"));
}

TEST_F(CullingTest, F_FILE_11_ReplaceKeepBothSkipWithUndo) {
    FolderTrash trash;
    FileOps ops(trash);
    const QString target = path("Ziel");
    ASSERT_TRUE(QDir().mkpath(target));
    for (const char* n : {"r.jpg", "k.jpg", "s.jpg"}) {
        ASSERT_TRUE(writeFile(path(QString::fromLatin1(n)), "new"));
        ASSERT_TRUE(writeFile(target + "/" + QString::fromLatin1(n), "old"));
    }
    QVector<TransferItem> items{{path("r.jpg"), target, ConflictChoice::Replace},
                                {path("k.jpg"), target, ConflictChoice::KeepBoth},
                                {path("s.jpg"), target, ConflictChoice::Skip}};
    EXPECT_EQ(filebatch::conflicts(items, TransferKind::Move).size(), 3);
    auto batch = std::make_shared<FileBatch>(trash, TransferKind::Move, items);
    bool done = false;
    runBatchAsync(ops, QStringLiteral("x"), batch, [&](bool ok, const QString&) {
        EXPECT_TRUE(ok);
        done = true;
    });
    ASSERT_TRUE(lf::test::waitUntil([&] { return done; }));
    EXPECT_EQ(readFile(target + "/r.jpg"), QByteArray("new")); // replaced (old one in the trash)
    EXPECT_EQ(readFile(target + "/k.jpg"), QByteArray("old"));
    EXPECT_EQ(readFile(target + "/k 2.jpg"), QByteArray("new")); // kept both
    EXPECT_EQ(readFile(target + "/s.jpg"), QByteArray("old"));   // skipped
    EXPECT_TRUE(QFileInfo::exists(path("s.jpg")));
    EXPECT_EQ(batch->skipped(), QStringList{path("s.jpg")});
    EXPECT_EQ(trash.trashed, 1);
    ops.undo();
    ASSERT_TRUE(lf::test::waitUntil([&] { return !ops.isBusy() && ops.undoStack()->index() == 0; }));
    EXPECT_EQ(readFile(target + "/r.jpg"), QByteArray("old")); // the replaced file is back
    EXPECT_EQ(readFile(path("r.jpg")), QByteArray("new"));
    EXPECT_FALSE(QFileInfo::exists(target + "/k 2.jpg"));
    EXPECT_EQ(readFile(path("k.jpg")), QByteArray("new"));
}

TEST_F(CullingTest, F_FILE_11_FailureRollsBackWholeBatch) {
    FolderTrash trash;
    FileOps ops(trash);
    const QString target = path("Ziel");
    ASSERT_TRUE(QDir().mkpath(target));
    ASSERT_TRUE(writeFile(path("one.jpg"), "1"));
    ASSERT_TRUE(writeFile(path("two.jpg"), "2"));
    // The second target folder cannot be created (a file is in the way).
    ASSERT_TRUE(writeFile(path("blocked"), "file"));
    QVector<TransferItem> items{{path("one.jpg"), target, ConflictChoice::Skip},
                                {path("two.jpg"), path("blocked/sub"), ConflictChoice::Skip}};
    bool done = false, ok = true;
    runBatchAsync(ops, QStringLiteral("x"), std::make_shared<FileBatch>(trash, TransferKind::Move, items),
                  [&](bool success, const QString&) {
                      ok = success;
                      done = true;
                  });
    ASSERT_TRUE(lf::test::waitUntil([&] { return done; }));
    EXPECT_FALSE(ok);
    EXPECT_TRUE(QFileInfo::exists(path("one.jpg"))); // rolled back
    EXPECT_FALSE(QFileInfo::exists(target + "/one.jpg"));
    EXPECT_EQ(ops.undoStack()->count(), 0);
}

// ---- F-IN-18 ----

TEST_F(CullingTest, F_IN_18_PhotographerPresetRatings) {
    InputBindings::applyPreset(QStringLiteral("photographer"));
    InputBindings b;
    EXPECT_EQ(b.choice(QStringLiteral("ratingKeys")), QStringLiteral("always"));
    InputEvent e;
    e.kind = InputKind::Key;
    e.key = Qt::Key_3;
    EXPECT_EQ(b.resolve(e), Action::Rate3); // digits = stars outside the culling mode
    e.key = Qt::Key_P;
    EXPECT_EQ(b.resolve(e), Action::RatePick);
    e.key = Qt::Key_X;
    EXPECT_EQ(b.resolve(e), Action::RateReject);
    e.key = Qt::Key_U;
    EXPECT_EQ(b.resolve(e), Action::RateClear);
    e.key = Qt::Key_0;
    e.modifiers = Qt::ControlModifier;
    EXPECT_EQ(b.resolve(e), Action::ZoomActual); // ⌘0 still zooms
    InputBindings::applyPreset(QStringLiteral("mac"));
    InputBindings mac;
    EXPECT_EQ(mac.choice(QStringLiteral("ratingKeys")), QStringLiteral("cullingOnly"));
    e.modifiers = Qt::NoModifier;
    e.key = Qt::Key_3;
    EXPECT_EQ(mac.resolve(e), Action::ZoomActual);
    e.inCullingMode = true;
    EXPECT_EQ(mac.resolve(e), Action::Rate3);
    // Culling mode and quick move keys.
    e.inCullingMode = false;
    e.key = Qt::Key_M;
    EXPECT_EQ(mac.resolve(e), Action::QuickMove);
    e.key = Qt::Key_C;
    EXPECT_EQ(mac.resolve(e), Action::QuickCopy);
    e.key = Qt::Key_K;
    e.modifiers = Qt::ControlModifier;
    EXPECT_EQ(mac.resolve(e), Action::ToggleCullingMode);
}
