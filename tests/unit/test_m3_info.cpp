// SPDX-License-Identifier: GPL-3.0-or-later
// M3 info panel logic: GPS position, "In Karten öffnen" and the mini map that loads only
// after a click (F-INFO-04, SPEC 10.3), the complete raw metadata tree with search and
// copy (F-INFO-05).

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/Decoder.h"
#include "core/GeoLocation.h"
#include "core/MetadataTree.h"

#include <QUrlQuery>

#include <gtest/gtest.h>

#include <cmath>

using namespace lf;
using namespace lf::test;

namespace {

// Stand-in for the MapKit snapshotter: counts requests (= network), answers on demand.
class FakeMapProvider : public MapSnapshotProvider {
public:
    struct Pending {
        GeoPoint p;
        Done done;
    };
    void request(const GeoPoint& p, QSize size, qreal dpr, QObject*, Done done) override {
        ++requests;
        lastSize = size;
        lastDpr = dpr;
        pending.append({p, std::move(done)});
    }
    void cancel() override { ++cancels; }
    void answer(int index, const QImage& img, const QString& error = {}) { pending[index].done(img, error); }

    int requests = 0;
    int cancels = 0;
    QSize lastSize;
    qreal lastDpr = 0;
    QList<Pending> pending;
};

QImage someMap() {
    QImage img(48, 32, QImage::Format_RGB32);
    img.fill(Qt::darkGreen);
    return img;
}

} // namespace

// ---- F-INFO-04 -----------------------------------------------------------------------------

TEST(GeoLocation, F_INFO_04_CoordinateTexts) {
    const GeoPoint p{48.1371, -122.4194};
    const QLocale de(QLocale::German, QLocale::Germany);
    const QLocale en(QLocale::English, QLocale::UnitedStates);
    EXPECT_EQ(geo::formatDecimal(p, de), QStringLiteral("48,1371° N, 122,4194° W"));
    EXPECT_EQ(geo::formatDecimal(p, en), QStringLiteral("48.1371° N, 122.4194° W"));
    EXPECT_EQ(geo::formatDms(p, de), QStringLiteral("48° 8′ 13,6″ N, 122° 25′ 9,8″ W"));
    EXPECT_EQ(geo::formatDms(GeoPoint{-33.8568, 151.2153}, en),
              QStringLiteral("33° 51′ 24.5″ S, 151° 12′ 55.1″ O"));
    // Rounding up to a full minute carries over.
    EXPECT_EQ(geo::formatDms(GeoPoint{10.99999, 0}, en), QStringLiteral("11° 0′ 0″ N, 0° 0′ 0″ O"));
    EXPECT_EQ(geo::machineText(p), QStringLiteral("48.137100,-122.419400"));
    EXPECT_TRUE(geo::isValid(p));
    EXPECT_FALSE(geo::isValid(GeoPoint{91, 0}));
    EXPECT_FALSE(geo::isValid(GeoPoint{0, 180.5}));
    EXPECT_FALSE(geo::isValid(GeoPoint{std::nan(""), 0}));
}

TEST(GeoLocation, F_INFO_04_OpenInMapsLink) {
    const QUrl url = geo::mapsUrl(GeoPoint{48.1371, -122.4194}, QStringLiteral("IMG 1.jpg"));
    EXPECT_EQ(url.scheme(), QStringLiteral("maps"));
    const QUrlQuery q(url);
    EXPECT_EQ(q.queryItemValue(QStringLiteral("ll")), QStringLiteral("48.137100,-122.419400"));
    EXPECT_EQ(q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("IMG 1.jpg"));
}

TEST(GeoLocation, F_INFO_04_NoNetworkBeforeClick) {
    ensureQtApp();
    FakeMapProvider provider;
    MapPreviewController map(&provider);
    EXPECT_EQ(map.state(), MapPreviewController::State::NoLocation);

    map.setLocation(GeoPoint{48.1371, -122.4194});
    EXPECT_EQ(map.state(), MapPreviewController::State::Idle);
    map.setLocation(GeoPoint{1, 2}); // flipping through images with a position: still nothing
    map.setSize(QSize(200, 120), 2.0);
    EXPECT_EQ(provider.requests, 0) << "SPEC 10.3: no map tiles before the user asks";
    EXPECT_FALSE(map.hasConsent());

    map.showMap(); // the click
    EXPECT_EQ(provider.requests, 1);
    EXPECT_EQ(map.state(), MapPreviewController::State::Loading);
    EXPECT_EQ(provider.pending[0].p, (GeoPoint{1, 2}));
    EXPECT_EQ(provider.lastSize, QSize(200, 120));
    EXPECT_EQ(provider.lastDpr, 2.0);
    provider.answer(0, someMap());
    EXPECT_EQ(map.state(), MapPreviewController::State::Ready);
    EXPECT_FALSE(map.image().isNull());

    // The consent holds for this window: the next position loads without a click …
    map.setLocation(GeoPoint{3, 4});
    EXPECT_EQ(provider.requests, 2);
    // … and a late answer for an earlier position is dropped.
    map.setLocation(GeoPoint{5, 6});
    provider.answer(1, someMap());
    EXPECT_EQ(map.state(), MapPreviewController::State::Loading);
    provider.answer(2, QImage(), QStringLiteral("offline"));
    EXPECT_EQ(map.state(), MapPreviewController::State::Failed);
    EXPECT_EQ(map.error(), QStringLiteral("offline"));
    // An image without position: no map, no request.
    map.setLocation(std::nullopt);
    EXPECT_EQ(map.state(), MapPreviewController::State::NoLocation);
    EXPECT_EQ(provider.requests, 3);

    // A new controller (another window) needs its own click.
    MapPreviewController other(&provider);
    other.setLocation(GeoPoint{7, 8});
    EXPECT_EQ(provider.requests, 3);
    EXPECT_EQ(other.state(), MapPreviewController::State::Idle);
}

TEST(GeoLocation, F_INFO_04_WithoutProviderFailsGracefully) {
    ensureQtApp();
    MapPreviewController map(nullptr);
    map.setLocation(GeoPoint{1, 2});
    EXPECT_EQ(map.state(), MapPreviewController::State::Idle);
    map.showMap();
    EXPECT_EQ(map.state(), MapPreviewController::State::Failed);
    EXPECT_FALSE(map.error().isEmpty());
    map.setLocation(GeoPoint{std::nan(""), 0}); // invalid positions count as none
    EXPECT_EQ(map.state(), MapPreviewController::State::NoLocation);
}

TEST(GeoLocation, F_INFO_04_PositionFromCorpusFile) {
    LF_REQUIRE_CORPUS();
    const Metadata md = DecoderRegistry::instance().metadata(corpusFile(QStringLiteral("exif.jpg")));
    ASSERT_TRUE(md.latitude && md.longitude);
    EXPECT_NEAR(*md.latitude, 48.1371, 1e-4);
    EXPECT_NEAR(*md.longitude, -122.4194, 1e-4);
}

// ---- F-INFO-05 -----------------------------------------------------------------------------

TEST(MetadataTree, F_INFO_05_FromVariantSearchCopy) {
    QVariantMap exif{{"FNumber", 2.8}, {"ISOSpeedRatings", QVariantList{400}}, {"LensModel", "Lens 35"}};
    QVariantMap root{{"Exif", exif},
                     {"MakerApple", QVariantMap{{"1", QVariantList{QVariantMap{{"a", 1}}, 2}}}},
                     {"Blob", QByteArray("\x01\x02", 2)}};
    const MetadataNode tree = metadatatree::fromVariant(QStringLiteral("x.jpg"), root);
    ASSERT_NE(metadatatree::find(tree, QStringLiteral("Exif/FNumber")), nullptr);
    EXPECT_EQ(metadatatree::find(tree, QStringLiteral("Exif/FNumber"))->value, QStringLiteral("2.8"));
    EXPECT_EQ(metadatatree::find(tree, QStringLiteral("Exif/ISOSpeedRatings"))->value, QStringLiteral("400"));
    EXPECT_EQ(metadatatree::find(tree, QStringLiteral("Blob"))->value, QStringLiteral("<2 Bytes: 01 02>"));
    EXPECT_NE(metadatatree::find(tree, QStringLiteral("MakerApple/1/[0]/a")), nullptr); // nested list
    EXPECT_EQ(metadatatree::leafCount(tree), 6);

    // Search: case-insensitive in keys and values; groups stay as context.
    const MetadataNode lens = metadatatree::filtered(tree, QStringLiteral("lens 35"));
    EXPECT_EQ(metadatatree::leafCount(lens), 1);
    EXPECT_NE(metadatatree::find(lens, QStringLiteral("Exif/LensModel")), nullptr);
    const MetadataNode group = metadatatree::filtered(tree, QStringLiteral("exif"));
    EXPECT_EQ(metadatatree::leafCount(group), 3); // a matching group keeps its rows
    EXPECT_EQ(metadatatree::filtered(tree, QString()), tree);
    EXPECT_EQ(metadatatree::leafCount(metadatatree::filtered(tree, QStringLiteral("nothing-like-this"))), 1);

    // Copy: "key: value" lines, groups as headers, indented.
    const MetadataNode* exifNode = metadatatree::find(tree, QStringLiteral("Exif"));
    EXPECT_EQ(metadatatree::toText(*exifNode),
              QStringLiteral("Exif:\n  FNumber: 2.8\n  ISOSpeedRatings: 400\n  LensModel: Lens 35"));
    EXPECT_EQ(metadatatree::toText(*metadatatree::find(tree, QStringLiteral("Exif/FNumber"))),
              QStringLiteral("FNumber: 2.8"));
}

TEST(MetadataTree, F_INFO_05_RawMetadataOfCorpusFile) {
    LF_REQUIRE_CORPUS();
    const MetadataNode tree = readRawMetadata(corpusFile(QStringLiteral("exif.jpg")));
    EXPECT_EQ(tree.key, QStringLiteral("exif.jpg"));
#if defined(Q_OS_DARWIN) && !defined(LF_GENERIC_PLATFORM)
    auto value = [&](const char* keyPath) {
        const MetadataNode* n = metadatatree::find(tree, QString::fromUtf8(keyPath));
        return n ? n->value : QStringLiteral("<missing %1>").arg(QString::fromUtf8(keyPath));
    };
    EXPECT_EQ(value("TIFF/Model"), QStringLiteral("Synthetic Camera X1"));
    EXPECT_EQ(value("Exif/FNumber"), QStringLiteral("2.8"));
    EXPECT_EQ(value("Exif/LensModel"), QStringLiteral("Synthetic 35mm F1.4"));
    EXPECT_EQ(value("Exif/ISOSpeedRatings"), QStringLiteral("400"));
    EXPECT_EQ(value("GPS/LatitudeRef"), QStringLiteral("N"));
    EXPECT_EQ(value("XMP/xmp/xmp:Rating"), QStringLiteral("4"));
    EXPECT_GE(metadatatree::leafCount(tree), 20);
    // Searching the real tree finds the camera model in its group.
    const MetadataNode found = metadatatree::filtered(tree, QStringLiteral("synthetic camera"));
    EXPECT_NE(metadatatree::find(found, QStringLiteral("TIFF/Model")), nullptr);
    EXPECT_TRUE(metadatatree::toText(found, false).contains(QStringLiteral("Model: Synthetic Camera X1")));
#endif
    // Unreadable files give an empty tree (the view shows a note), never a crash.
    EXPECT_TRUE(readRawMetadata(corpusFile(QStringLiteral("garbage.jpg"))).children.isEmpty());
    EXPECT_TRUE(readRawMetadata(QStringLiteral("/nonexistent/file.jpg")).children.isEmpty());
}
