// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-08 folder map, platform-neutral part (core/FolderMap.h): GPS positions from the
// metadata without decoding (EXIF parser, ImageIO on macOS), the cache, regions and the
// "follow the current image" decision, and the model with a fake map view: counts,
// progressive markers, cancellation, filters/pairs, marker clicks, and SPEC 10.3 (no file
// reading and no map before the view is opened; closing stops the map).

#include "CorpusTestUtil.h"
#include "QtTestApp.h"
#include "core/Collection.h"
#include "core/FolderMap.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>

using namespace lf;
using namespace lf::test;

namespace {

void be16(QByteArray& a, quint16 v) {
    a.append(char(v >> 8)).append(char(v & 0xFF));
}
void be32(QByteArray& a, quint32 v) {
    for (int s = 24; s >= 0; s -= 8)
        a.append(char((v >> s) & 0xFF));
}

// Big-endian TIFF block with an IFD0 that points to a GPS IFD (version, lat/lon with refs).
QByteArray exifTiff(double lat, double lon) {
    auto dms = [](double v, quint32 out[6]) {
        v = std::abs(v);
        const int d = int(v);
        const int m = int((v - d) * 60);
        const double s = ((v - d) * 60 - m) * 60;
        out[0] = quint32(d), out[1] = 1, out[2] = quint32(m), out[3] = 1;
        out[4] = quint32(std::llround(s * 10000)), out[5] = 10000;
    };
    QByteArray t("MM\0*", 4);
    be32(t, 8);
    // IFD0 at 8: one entry (GPS pointer) -> GPS IFD at 26.
    be16(t, 1);
    be16(t, 0x8825), be16(t, 4), be32(t, 1), be32(t, 26);
    be32(t, 0);
    // GPS IFD at 26: 5 entries (2 + 5*12 + 4 = 66 bytes) -> rationals at 92 and 116.
    be16(t, 5);
    be16(t, 0), be16(t, 1), be32(t, 4), t.append(QByteArray("\x02\x02\x00\x00", 4));
    be16(t, 1), be16(t, 2), be32(t, 2), t.append(QByteArray(lat < 0 ? "S\0\0\0" : "N\0\0\0", 4));
    be16(t, 2), be16(t, 5), be32(t, 3), be32(t, 92);
    be16(t, 3), be16(t, 2), be32(t, 2), t.append(QByteArray(lon < 0 ? "W\0\0\0" : "E\0\0\0", 4));
    be16(t, 4), be16(t, 5), be32(t, 3), be32(t, 116);
    be32(t, 0);
    quint32 a[6], b[6];
    dms(lat, a);
    dms(lon, b);
    for (quint32 v : a)
        be32(t, v);
    for (quint32 v : b)
        be32(t, v);
    return t;
}

// A small JPEG (Qt's writer) with the EXIF block inserted after SOI.
QByteArray gpsJpeg(std::optional<GeoPoint> p, QColor color = Qt::darkGreen) {
    QImage img(16, 12, QImage::Format_RGB32);
    img.fill(color);
    QByteArray jpg;
    QBuffer buf(&jpg);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 90);
    if (!p)
        return jpg;
    const QByteArray tiff = exifTiff(p->latitude, p->longitude);
    QByteArray app1("\xFF\xE1", 2);
    be16(app1, quint16(2 + 6 + tiff.size()));
    app1.append(QByteArray("Exif\0\0", 6)).append(tiff);
    return jpg.left(2) + app1 + jpg.mid(2);
}

void writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(data);
}

bool near(const std::optional<GeoPoint>& p, double lat, double lon, double tol = 1e-5) {
    return p && std::abs(p->latitude - lat) < tol && std::abs(p->longitude - lon) < tol;
}

FileEntry entry(const QString& path, const QString& paired = QString()) {
    FileEntry e;
    e.path = path;
    e.name = QFileInfo(path).fileName();
    e.modified = QDateTime::fromMSecsSinceEpoch(1700000000000);
    e.size = 1000;
    e.pairedPath = paired;
    return e;
}

class FakeMap final : public MapViewBackend {
public:
    void start() override {
        ++starts;
        started = true;
    }
    void stop() override {
        ++stops;
        started = false;
        markers.clear();
    }
    bool isStarted() const override { return started; }
    void setMarkers(const QVector<MapMarker>& m) override {
        ++calls;
        markers = m;
    }
    void addMarkers(const QVector<MapMarker>& m) override {
        ++calls;
        ++addBatches;
        markers += m;
    }
    void setCurrent(const QString& path) override {
        ++calls;
        current = path;
    }
    void showRegion(const GeoRegion& r, bool animated) override {
        ++calls;
        regions << r;
        lastAnimated = animated;
        visible = r;
    }
    std::optional<GeoRegion> visibleRegion() const override { return visible; }
    void setThumbnail(const QString& path, const QImage&) override { thumbs << path; }

    int starts = 0, stops = 0, calls = 0, addBatches = 0;
    bool started = false, lastAnimated = false;
    QVector<MapMarker> markers;
    QString current;
    QVector<GeoRegion> regions;
    std::optional<GeoRegion> visible;
    QStringList thumbs;
};

// Positions by path, optionally slow, counting reads.
struct FakeReader {
    QHash<QString, GeoPoint> points;
    int delayMs = 0;
    std::shared_ptr<std::atomic<int>> reads = std::make_shared<std::atomic<int>>(0);
    GeoTagCache::Reader reader() const {
        auto pts = points;
        auto r = reads;
        const int delay = delayMs;
        return [pts, r, delay](const QString& path) -> std::optional<GeoPoint> {
            ++*r;
            if (delay > 0)
                QThread::msleep(ulong(delay));
            const auto it = pts.constFind(path);
            return it == pts.constEnd() ? std::nullopt : std::optional<GeoPoint>(*it);
        };
    }
};

} // namespace

TEST(FolderMap, F_INFO_08_ExifGpsParserJpegAndTiff) {
    const QByteArray jpg = gpsJpeg(GeoPoint{48.1371, 11.5754});
    EXPECT_TRUE(near(geotag::parseExifGps(jpg), 48.1371, 11.5754));
    EXPECT_TRUE(near(geotag::parseExifGps(gpsJpeg(GeoPoint{-33.8568, -151.2153})), -33.8568, -151.2153));
    EXPECT_TRUE(near(geotag::parseExifGps(exifTiff(64.1466, -21.9426)), 64.1466, -21.9426)); // TIFF/DNG/RAW
    EXPECT_FALSE(geotag::parseExifGps(gpsJpeg(std::nullopt)).has_value());
    EXPECT_FALSE(geotag::parseExifGps(gpsJpeg(GeoPoint{0, 0})).has_value()); // no fix
    EXPECT_FALSE(geotag::parseExifGps(QByteArray()).has_value());
    // Truncations and byte flips never crash.
    for (qsizetype n = 0; n < 200; ++n)
        (void)geotag::parseExifGps(jpg.left(n));
    QByteArray m = jpg;
    for (int i = 0; i < 2000; ++i) {
        m[qsizetype((i * 7919) % 160)] = char(i * 31);
        (void)geotag::parseExifGps(m);
    }
}

TEST(FolderMap, F_INFO_08_ReadsLocationWithoutDecoding) {
    QTemporaryDir dir;
    const QString with = dir.filePath(QStringLiteral("with.jpg")),
                  without = dir.filePath(QStringLiteral("without.jpg"));
    writeFile(with, gpsJpeg(GeoPoint{52.5163, 13.3777}));
    writeFile(without, gpsJpeg(std::nullopt));
    EXPECT_TRUE(near(readImageLocation(with), 52.5163, 13.3777, 1e-4));
    EXPECT_FALSE(readImageLocation(without).has_value());
    EXPECT_FALSE(readImageLocation(dir.filePath(QStringLiteral("missing.jpg"))).has_value());
    // The corpus' EXIF JPEG (tests/corpus/tools/gen_corpus.swift): 48.1371 N, 122.4194 W.
    const QString exif = corpusFile(QStringLiteral("exif.jpg"));
    if (QFileInfo::exists(exif))
        EXPECT_TRUE(near(readImageLocation(exif), 48.1371, -122.4194, 1e-3));
}

TEST(FolderMap, F_INFO_08_GeoTagCacheByPathAndModification) {
    FakeReader fr;
    fr.points.insert(QStringLiteral("/a.jpg"), GeoPoint{1, 2});
    GeoTagCache cache(fr.reader());
    const QDateTime t1 = QDateTime::fromMSecsSinceEpoch(1000), t2 = QDateTime::fromMSecsSinceEpoch(2000);
    EXPECT_FALSE(cache.cached(QStringLiteral("/a.jpg"), t1, 10).has_value());
    EXPECT_TRUE(near(cache.location(QStringLiteral("/a.jpg"), t1, 10), 1, 2));
    EXPECT_TRUE(near(cache.location(QStringLiteral("/a.jpg"), t1, 10), 1, 2));
    EXPECT_EQ(cache.readCount(), 1);
    ASSERT_TRUE(cache.cached(QStringLiteral("/a.jpg"), t1, 10).has_value());
    EXPECT_FALSE(cache.location(QStringLiteral("/b.jpg"), t1, 10).has_value());
    EXPECT_TRUE(cache.cached(QStringLiteral("/b.jpg"), t1, 10).has_value()); // "no position" is cached too
    (void)cache.location(QStringLiteral("/a.jpg"), t2, 10);                  // modified: read again
    (void)cache.location(QStringLiteral("/a.jpg"), t2, 11);
    EXPECT_EQ(cache.readCount(), 4);
}

TEST(FolderMap, F_INFO_08_RegionsAndFollowDecision) {
    const GeoRegion r{{50, 10}, 2, 4};
    EXPECT_TRUE(geo::regionContains(r, {50.9, 11.9}));
    EXPECT_FALSE(geo::regionContains(r, {50.9, 11.9}, 0.1)); // in the margin
    EXPECT_FALSE(geo::regionContains(r, {52, 10}));
    // Bounding region: padding and a minimum span for a single photo.
    const GeoRegion one = geo::boundingRegion({{48, 11}});
    EXPECT_DOUBLE_EQ(one.center.latitude, 48);
    EXPECT_GE(one.latitudeDelta, 0.01);
    const GeoRegion two = geo::boundingRegion({{48, 11}, {50, 15}}, 0.1);
    EXPECT_DOUBLE_EQ(two.center.latitude, 49);
    EXPECT_DOUBLE_EQ(two.center.longitude, 13);
    EXPECT_NEAR(two.latitudeDelta, 2.4, 1e-9);
    EXPECT_NEAR(two.longitudeDelta, 4.8, 1e-9);
    // Across the antimeridian the short way round is used.
    const GeoRegion pacific = geo::boundingRegion({{0, 179}, {0, -179}}, 0);
    EXPECT_NEAR(pacific.longitudeDelta, 2, 1e-9);
    EXPECT_NEAR(std::abs(pacific.center.longitude), 180, 1e-9);
    EXPECT_TRUE(geo::regionContains(pacific, {0, -179.5}));
    // Follow: inside -> stay; outside -> same span centred on the point; unknown -> fallback.
    EXPECT_FALSE(geo::followRegion(r, {50.1, 10.1}).has_value());
    const auto moved = geo::followRegion(r, {55, 20});
    ASSERT_TRUE(moved.has_value());
    EXPECT_EQ(*moved, (GeoRegion{{55, 20}, 2, 4}));
    const auto fallback = geo::followRegion(std::nullopt, {1, 1}, 0.08, 0.5);
    ASSERT_TRUE(fallback.has_value());
    EXPECT_EQ(fallback->latitudeDelta, 0.5);
}

TEST(FolderMap, F_INFO_08_NothingReadOrLoadedBeforeOpen) {
    ensureQtApp();
    FakeReader fr;
    QVector<FileEntry> entries;
    for (int i = 0; i < 10; ++i) {
        const QString p = QStringLiteral("/photos/%1.jpg").arg(i);
        entries << entry(p);
        fr.points.insert(p, GeoPoint{45.0 + i * 0.01, 7.0});
    }
    GeoTagCache cache(fr.reader());
    FakeMap map;
    FolderMapModel model(&cache);
    model.setBackend(&map);
    model.setEntries(entries);
    model.setCurrent(entries[3].path);
    spin(50);
    // SPEC 10.3: no map (no network) and no file access before the user opens the view.
    EXPECT_EQ(map.starts, 0);
    EXPECT_EQ(map.calls, 0);
    EXPECT_EQ(fr.reads->load(), 0);
    EXPECT_EQ(model.locatedCount(), 0);
    model.open();
    EXPECT_EQ(map.starts, 1);
    ASSERT_TRUE(waitUntil([&] { return !model.isScanning(); }, 5000));
    EXPECT_EQ(model.locatedCount(), 10);
    EXPECT_EQ(map.markers.size(), 10);
    EXPECT_EQ(map.current, entries[3].path);
    // Closing removes the map (no more loading) and stops the reading.
    model.close();
    EXPECT_EQ(map.stops, 1);
    EXPECT_FALSE(map.isStarted());
    const int calls = map.calls, reads = fr.reads->load();
    model.setEntries(entries.mid(0, 5));
    model.setCurrent(entries[1].path);
    spin(50);
    EXPECT_EQ(map.calls, calls);
    EXPECT_EQ(fr.reads->load(), reads);
    // Opening again: positions come from the cache (no new reads).
    model.open();
    EXPECT_EQ(map.starts, 2);
    EXPECT_EQ(model.locatedCount(), 5);
    EXPECT_EQ(fr.reads->load(), reads);
}

TEST(FolderMap, F_INFO_08_CountsAndProgressiveMarkers) {
    ensureQtApp();
    FakeReader fr;
    fr.delayMs = 3;
    QVector<FileEntry> entries;
    for (int i = 0; i < 60; ++i) {
        const QString p = QStringLiteral("/trip/IMG_%1.jpg").arg(i);
        entries << entry(p);
        if (i % 3 != 0) // 40 of 60 with a position
            fr.points.insert(p, GeoPoint{60.0 + i * 0.001, 25.0});
    }
    GeoTagCache cache(fr.reader());
    FakeMap map;
    FolderMapModel model(&cache);
    model.setBackend(&map);
    model.setEntries(entries);
    int counts = 0, finished = 0;
    QObject::connect(&model, &FolderMapModel::countsChanged, [&] { ++counts; });
    QObject::connect(&model, &FolderMapModel::scanFinished, [&] { ++finished; });
    model.open();
    EXPECT_TRUE(model.isScanning());
    EXPECT_TRUE(model.statusText().contains(QStringLiteral("60")));
    ASSERT_TRUE(waitUntil([&] { return finished > 0; }, 10000));
    EXPECT_EQ(model.totalCount(), 60);
    EXPECT_EQ(model.locatedCount(), 40);
    EXPECT_EQ(model.scannedCount(), 60);
    EXPECT_GT(counts, 2);         // filled progressively
    EXPECT_GT(map.addBatches, 1); // markers arrive in batches
    EXPECT_EQ(map.markers.size(), 40);
    const QString status = model.statusText();
    EXPECT_TRUE(status.contains(QStringLiteral("40")) && status.contains(QStringLiteral("60")))
        << status.toStdString();
    // The map shows all markers once reading is done.
    ASSERT_FALSE(map.regions.isEmpty());
    for (const MapMarker& m : map.markers)
        EXPECT_TRUE(geo::regionContains(map.regions.last(), m.point));
}

TEST(FolderMap, F_INFO_08_FollowsCurrentAndMarkerClickJumps) {
    ensureQtApp();
    FakeReader fr;
    QVector<FileEntry> entries;
    const GeoPoint pts[] = {{48.0, 11.0}, {48.001, 11.001}, {41.9, 12.5}, {40.4, -3.7}};
    for (int i = 0; i < 4; ++i) {
        entries << entry(QStringLiteral("/t/%1.jpg").arg(i));
        fr.points.insert(entries[i].path, pts[i]);
    }
    GeoTagCache cache(fr.reader());
    FakeMap map;
    FolderMapModel model(&cache);
    model.setBackend(&map);
    model.setEntries(entries);
    model.setCurrent(entries[0].path);
    model.open();
    ASSERT_TRUE(waitUntil([&] { return !model.isScanning(); }, 5000));
    // The user zooms in around Munich.
    map.userMovedMap();
    map.visible = GeoRegion{{48.0005, 11.0005}, 0.05, 0.05};
    const int before = int(map.regions.size());
    model.setCurrent(entries[1].path); // still visible: the map stays
    EXPECT_EQ(map.current, entries[1].path);
    EXPECT_EQ(int(map.regions.size()), before);
    model.setCurrent(entries[2].path); // Rome: recentred, same zoom, animated
    ASSERT_EQ(int(map.regions.size()), before + 1);
    EXPECT_EQ(map.regions.last(), (GeoRegion{pts[2], 0.05, 0.05}));
    EXPECT_TRUE(map.lastAnimated);
    // Click on a marker -> jump; hover -> thumbnail wanted.
    QStringList jump, thumb;
    QObject::connect(&model, &FolderMapModel::jumpRequested, [&](const QString& p) { jump << p; });
    QObject::connect(&model, &FolderMapModel::thumbnailWanted, [&](const QString& p) { thumb << p; });
    map.markerActivated(entries[3].path);
    ASSERT_EQ(jump.count(), 1);
    EXPECT_EQ(jump.first(), entries[3].path);
    map.markerActivated(QStringLiteral("/not/in/collection.jpg"));
    EXPECT_EQ(jump.count(), 1);
    map.markerHovered(entries[2].path);
    map.markerHovered(QString());
    ASSERT_EQ(thumb.count(), 1);
    EXPECT_EQ(thumb.first(), entries[2].path);
}

TEST(FolderMap, F_INFO_08_RawJpegPairUsesOneEntry) {
    ensureQtApp();
    FakeReader fr;
    // The JPEG has no position, its RAW has: one marker under the shown JPEG.
    QTemporaryDir dir;
    const QString jpg = dir.filePath(QStringLiteral("IMG_1.JPG")),
                  raw = dir.filePath(QStringLiteral("IMG_1.CR3"));
    writeFile(raw, "raw");
    fr.points.insert(raw, GeoPoint{35.68, 139.76});
    GeoTagCache cache(fr.reader());
    FakeMap map;
    FolderMapModel model(&cache);
    model.setBackend(&map);
    model.setEntries({entry(jpg, raw), entry(dir.filePath(QStringLiteral("other.jpg")))});
    model.open();
    ASSERT_TRUE(waitUntil([&] { return !model.isScanning(); }, 5000));
    EXPECT_EQ(model.totalCount(), 2);
    ASSERT_EQ(model.locatedCount(), 1);
    EXPECT_EQ(model.markers().first().path, jpg);
    EXPECT_TRUE(near(model.locationOf(jpg), 35.68, 139.76));
}

TEST(FolderMap, F_INFO_08_RespectsCollectionFilters) {
    ensureQtApp();
    QTemporaryDir dir;
    const QString root = QDir::cleanPath(dir.path());
    writeFile(root + QStringLiteral("/berlin_1.jpg"), gpsJpeg(GeoPoint{52.52, 13.40}));
    writeFile(root + QStringLiteral("/berlin_2.jpg"), gpsJpeg(GeoPoint{52.51, 13.38}));
    writeFile(root + QStringLiteral("/paris_1.jpg"), gpsJpeg(GeoPoint{48.85, 2.35}));
    writeFile(root + QStringLiteral("/scan.jpg"), gpsJpeg(std::nullopt));
    Collection c;
    c.openFolder(root);
    ASSERT_TRUE(waitUntil([&] { return !c.isLoading() && c.count() == 4; }, 5000));
    GeoTagCache cache; // the platform reader (ImageIO / EXIF parser)
    FakeMap map;
    FolderMapModel model(&cache);
    model.setBackend(&map);
    QObject::connect(&c, &Collection::entriesChanged, &model, [&] { model.setEntries(c.entries()); });
    model.setEntries(c.entries());
    model.open();
    ASSERT_TRUE(waitUntil([&] { return !model.isScanning(); }, 5000));
    EXPECT_EQ(model.totalCount(), 4);
    EXPECT_EQ(model.locatedCount(), 3);
    // F-NAV-08 name filter: only the matching images are on the map and counted.
    c.setNameFilter(QStringLiteral("berlin"));
    ASSERT_TRUE(waitUntil([&] { return c.count() == 2; }, 5000));
    ASSERT_TRUE(waitUntil([&] { return !model.isScanning() && model.totalCount() == 2; }, 5000));
    EXPECT_EQ(model.locatedCount(), 2);
    EXPECT_EQ(map.markers.size(), 2);
    for (const MapMarker& m : map.markers)
        EXPECT_TRUE(m.name.startsWith(QStringLiteral("berlin")));
}

TEST(FolderMap, F_INFO_08_CancelsWhenEntriesChange) {
    ensureQtApp();
    FakeReader fr;
    fr.delayMs = 5;
    QVector<FileEntry> many, few;
    for (int i = 0; i < 300; ++i) {
        many << entry(QStringLiteral("/big/%1.jpg").arg(i));
        fr.points.insert(many.last().path, GeoPoint{10, 10 + i * 0.001});
    }
    for (int i = 0; i < 3; ++i) {
        few << entry(QStringLiteral("/small/%1.jpg").arg(i));
        fr.points.insert(few.last().path, GeoPoint{-10, -10 - i * 0.001});
    }
    GeoTagCache cache(fr.reader());
    FakeMap map;
    {
        FolderMapModel model(&cache);
        model.setBackend(&map);
        model.setEntries(many);
        model.open();
        ASSERT_TRUE(waitUntil([&] { return model.scannedCount() > 0; }, 5000));
        model.setEntries(few); // another folder: the old reading stops
        ASSERT_TRUE(waitUntil([&] { return !model.isScanning(); }, 5000));
        spin(100); // late results of the old folder must not appear
        EXPECT_EQ(model.locatedCount(), 3);
        ASSERT_EQ(map.markers.size(), 3);
        for (const MapMarker& m : map.markers)
            EXPECT_TRUE(m.path.startsWith(QStringLiteral("/small/")));
        EXPECT_LT(fr.reads->load(), 150);
        // Deleting the model while it reads waits for the workers (no crash).
        model.setEntries(many);
    }
    spin(50);
}
