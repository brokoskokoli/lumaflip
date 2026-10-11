// SPDX-License-Identifier: GPL-3.0-or-later
#include "FolderMap.h"

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace lf {

// ---- EXIF GPS (portable) -----------------------------------------------------------------------

namespace geotag {

namespace {

class Tiff {
public:
    Tiff(const uchar* d, qint64 n) : m_d(d), m_n(n) {}

    bool init() {
        if (m_n < 8)
            return false;
        if (m_d[0] == 'I' && m_d[1] == 'I')
            m_le = true;
        else if (m_d[0] == 'M' && m_d[1] == 'M')
            m_le = false;
        else
            return false;
        const quint16 magic = u16(2);
        // TIFF/DNG 42, Olympus ORF "RO"/"SR", Panasonic RW2 0x55.
        return magic == 42 || magic == 0x4F52 || magic == 0x5352 || magic == 0x55;
    }
    quint16 u16(qint64 off) const {
        if (off < 0 || off + 2 > m_n)
            return 0;
        return m_le ? quint16(m_d[off] | (m_d[off + 1] << 8)) : quint16((m_d[off] << 8) | m_d[off + 1]);
    }
    quint32 u32(qint64 off) const {
        if (off < 0 || off + 4 > m_n)
            return 0;
        const quint32 a = m_d[off], b = m_d[off + 1], c = m_d[off + 2], d = m_d[off + 3];
        return m_le ? (a | (b << 8) | (c << 16) | (d << 24)) : ((a << 24) | (b << 16) | (c << 8) | d);
    }
    // Offset of the 12-byte entry with `tag` in the IFD at `ifd`, or -1.
    qint64 find(qint64 ifd, quint16 tag) const {
        if (ifd <= 0 || ifd + 2 > m_n)
            return -1;
        const int count = u16(ifd);
        if (count > 1000 || ifd + 2 + qint64(count) * 12 > m_n)
            return -1;
        for (int i = 0; i < count; ++i) {
            const qint64 e = ifd + 2 + qint64(i) * 12;
            if (u16(e) == tag)
                return e;
        }
        return -1;
    }
    std::optional<double> degrees(qint64 entry) const {
        if (entry < 0 || u16(entry + 2) != 5 || u32(entry + 4) < 1) // RATIONAL
            return std::nullopt;
        const quint32 count = std::min<quint32>(u32(entry + 4), 3);
        const qint64 off = u32(entry + 8);
        if (off <= 0 || off + qint64(count) * 8 > m_n)
            return std::nullopt;
        double v = 0, scale = 1;
        for (quint32 i = 0; i < count; ++i) {
            const quint32 num = u32(off + i * 8), den = u32(off + i * 8 + 4);
            if (den == 0)
                return i == 0 ? std::nullopt : std::optional<double>(v);
            v += double(num) / double(den) / scale;
            scale *= 60;
        }
        return v;
    }
    char ref(qint64 entry) const {
        if (entry < 0 || u16(entry + 2) != 2) // ASCII, inline (count <= 4)
            return 0;
        return char(m_d[entry + 8]);
    }

private:
    const uchar* m_d;
    qint64 m_n;
    bool m_le = true;
};

std::optional<GeoPoint> parseTiff(const uchar* d, qint64 n) {
    Tiff t(d, n);
    if (!t.init())
        return std::nullopt;
    const qint64 gpsEntry = t.find(t.u32(4), 0x8825);
    if (gpsEntry < 0)
        return std::nullopt;
    const qint64 gps = t.u32(gpsEntry + 8);
    const auto lat = t.degrees(t.find(gps, 2));
    const auto lon = t.degrees(t.find(gps, 4));
    if (!lat || !lon)
        return std::nullopt;
    const char latRef = t.ref(t.find(gps, 1)), lonRef = t.ref(t.find(gps, 3));
    GeoPoint p{(latRef == 'S' || latRef == 's') ? -*lat : *lat,
               (lonRef == 'W' || lonRef == 'w') ? -*lon : *lon};
    // Cameras without a fix sometimes write 0/0: no position.
    if (!geo::isValid(p) || (p.latitude == 0.0 && p.longitude == 0.0))
        return std::nullopt;
    return p;
}

} // namespace

std::optional<GeoPoint> parseExifGps(const QByteArray& data) {
    const auto* d = reinterpret_cast<const uchar*>(data.constData());
    const qint64 n = data.size();
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) { // JPEG: look for APP1 "Exif"
        qint64 pos = 2;
        while (pos + 4 <= n) {
            if (d[pos] != 0xFF)
                return std::nullopt;
            const uchar marker = d[pos + 1];
            if (marker == 0xFF) { // fill byte
                ++pos;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA)
                return std::nullopt; // end of image / start of scan: no EXIF before the pixels
            const qint64 len = (qint64(d[pos + 2]) << 8) | d[pos + 3];
            if (len < 2 || pos + 2 + len > n)
                return std::nullopt;
            if (marker == 0xE1 && len >= 8 && std::memcmp(d + pos + 4, "Exif\0\0", 6) == 0)
                if (auto p = parseTiff(d + pos + 10, len - 8))
                    return p;
            pos += 2 + len;
        }
        return std::nullopt;
    }
    return parseTiff(d, n);
}

std::optional<GeoPoint> readExifGps(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parseExifGps(f.read(512 * 1024));
}

} // namespace geotag

// ---- regions ------------------------------------------------------------------------------------

namespace geo {

bool regionContains(const GeoRegion& r, const GeoPoint& p, double inset) {
    inset = std::clamp(inset, 0.0, 0.49);
    const double halfLat = r.latitudeDelta * (0.5 - inset);
    const double halfLon = r.longitudeDelta * (0.5 - inset);
    double dLon = p.longitude - r.center.longitude;
    if (dLon > 180)
        dLon -= 360;
    else if (dLon < -180)
        dLon += 360;
    return std::abs(p.latitude - r.center.latitude) <= halfLat && std::abs(dLon) <= halfLon;
}

GeoRegion boundingRegion(const QVector<GeoPoint>& points, double padding, double minSpan) {
    GeoRegion r;
    if (points.isEmpty())
        return r;
    double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    for (const GeoPoint& p : points) {
        minLat = std::min(minLat, p.latitude);
        maxLat = std::max(maxLat, p.latitude);
        minLon = std::min(minLon, p.longitude);
        maxLon = std::max(maxLon, p.longitude);
    }
    // Points on both sides of the antimeridian: use the shorter way round.
    double lonSpan = maxLon - minLon;
    double centerLon = (minLon + maxLon) / 2;
    if (lonSpan > 180) {
        double west = -180, east = 180; // largest gap decides
        QVector<double> lons;
        for (const GeoPoint& p : points)
            lons << p.longitude;
        std::sort(lons.begin(), lons.end());
        double gap = 360 - (lons.last() - lons.first());
        west = lons.first();
        east = lons.last();
        for (int i = 1; i < lons.size(); ++i)
            if (lons[i] - lons[i - 1] > gap) {
                gap = lons[i] - lons[i - 1];
                west = lons[i];
                east = lons[i - 1] + 360;
            }
        lonSpan = east - west;
        centerLon = (west + east) / 2;
        if (centerLon > 180)
            centerLon -= 360;
    }
    r.center = {(minLat + maxLat) / 2, centerLon};
    r.latitudeDelta = std::clamp((maxLat - minLat) * (1 + 2 * padding), minSpan, 180.0);
    r.longitudeDelta = std::clamp(lonSpan * (1 + 2 * padding), minSpan, 360.0);
    return r;
}

std::optional<GeoRegion> followRegion(const std::optional<GeoRegion>& visible, const GeoPoint& p,
                                      double inset, double fallbackSpan) {
    if (visible && visible->latitudeDelta > 0 && visible->longitudeDelta > 0) {
        if (regionContains(*visible, p, inset))
            return std::nullopt;
        return GeoRegion{p, visible->latitudeDelta, visible->longitudeDelta};
    }
    return GeoRegion{p, fallbackSpan, fallbackSpan};
}

} // namespace geo

// ---- GeoTagCache ----------------------------------------------------------------------------------

GeoTagCache::GeoTagCache(Reader reader) : m_reader(std::move(reader)) {}

GeoTagCache& GeoTagCache::instance() {
    static GeoTagCache* cache = new GeoTagCache; // process lifetime (workers may still read)
    return *cache;
}

GeoTagCache::Key GeoTagCache::keyOf(const QString& path, const QDateTime& modified, qint64 size) {
    return Key{path, modified.isValid() ? modified.toMSecsSinceEpoch() : 0, size};
}

std::optional<GeoPoint> GeoTagCache::location(const QString& path, const QDateTime& modified, qint64 size) {
    const Key key = keyOf(path, modified, size);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_map.constFind(key);
        if (it != m_map.constEnd())
            return *it;
    }
    ++m_reads;
    std::optional<GeoPoint> p = m_reader ? m_reader(path) : std::nullopt;
    if (p && !geo::isValid(*p))
        p.reset();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_map.insert(key, p);
    return p;
}

std::optional<std::optional<GeoPoint>> GeoTagCache::cached(const QString& path, const QDateTime& modified,
                                                           qint64 size) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_map.constFind(keyOf(path, modified, size));
    if (it == m_map.constEnd())
        return std::nullopt;
    return *it;
}

void GeoTagCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_map.clear();
}

// ---- FolderMapModel -------------------------------------------------------------------------------

namespace {
constexpr int kBatch = 16;

struct ScanItem {
    int index;
    QString path;
    QString paired;
    QDateTime modified;
    qint64 size;
};
} // namespace

FolderMapModel::FolderMapModel(GeoTagCache* cache, QObject* parent) : QObject(parent), m_cache(cache) {
    m_pool = new QThreadPool(this);
    m_pool->setMaxThreadCount(2);
    m_pool->setThreadPriority(QThread::LowPriority);
}

FolderMapModel::~FolderMapModel() {
    cancelScan();
    m_pool->clear();
    m_pool->waitForDone();
    if (m_backend) {
        m_backend->markerActivated = nullptr;
        m_backend->markerHovered = nullptr;
        m_backend->userMovedMap = nullptr;
    }
}

void FolderMapModel::setWorkerThreads(int n) {
    m_pool->setMaxThreadCount(std::max(1, n));
}

void FolderMapModel::setBackend(MapViewBackend* backend) {
    if (m_backend == backend)
        return;
    if (m_backend) {
        m_backend->markerActivated = nullptr;
        m_backend->markerHovered = nullptr;
        m_backend->userMovedMap = nullptr;
        if (m_backend->isStarted())
            m_backend->stop();
    }
    m_backend = backend;
    connectBackend();
    if (m_open && m_backend) {
        m_backend->start();
        m_backend->setMarkers(m_markers);
        m_backend->setCurrent(m_current);
        m_fitted = false;
        fitAll(false);
        followCurrent(false);
    }
}

void FolderMapModel::connectBackend() {
    if (!m_backend)
        return;
    m_backend->markerActivated = [this](const QString& path) {
        if (m_open && m_markerIndex.contains(path))
            emit jumpRequested(path);
    };
    m_backend->markerHovered = [this](const QString& path) {
        if (m_open && !path.isEmpty())
            emit thumbnailWanted(path);
    };
    m_backend->userMovedMap = [this] { m_userMoved = true; };
}

void FolderMapModel::setEntries(const QVector<FileEntry>& entries) {
    cancelScan();
    ++m_generation;
    m_entries = entries;
    m_known = QVector<bool>(entries.size(), false);
    m_markers.clear();
    m_markerIndex.clear();
    m_scanned = 0;
    m_fitted = false;
    if (m_open) {
        // Known positions at once (no file access on the GUI thread).
        QVector<MapMarker> known;
        for (int i = 0; i < m_entries.size(); ++i) {
            const FileEntry& e = m_entries[i];
            const auto c = m_cache->cached(e.path, e.modified, e.size);
            if (!c || (!*c && !e.pairedPath.isEmpty()))
                continue; // unknown, or a pair whose other file may have the position
            m_known[i] = true;
            ++m_scanned;
            if (*c)
                known << MapMarker{e.path, e.name, **c};
        }
        if (m_backend)
            m_backend->setMarkers({});
        addLocated(known);
        if (m_backend)
            m_backend->setCurrent(m_current);
        startScan();
    }
    emit countsChanged();
    if (m_open && m_scanned == totalCount())
        emit scanFinished();
}

void FolderMapModel::setCurrent(const QString& path) {
    if (m_current == path)
        return;
    m_current = path;
    if (m_open && m_backend) {
        m_backend->setCurrent(path);
        followCurrent(true);
    }
}

void FolderMapModel::open() {
    if (m_open)
        return;
    m_open = true;
    m_userMoved = false;
    if (m_backend)
        m_backend->start();
    setEntries(QVector<FileEntry>(m_entries)); // known positions now, the rest on workers
}

void FolderMapModel::close() {
    if (!m_open)
        return;
    m_open = false;
    cancelScan();
    ++m_generation;
    m_markers.clear();
    m_markerIndex.clear();
    m_known.fill(false);
    m_scanned = 0;
    if (m_backend && m_backend->isStarted())
        m_backend->stop();
    emit countsChanged();
}

std::optional<GeoPoint> FolderMapModel::locationOf(const QString& path) const {
    const auto it = m_markerIndex.constFind(path);
    if (it == m_markerIndex.constEnd())
        return std::nullopt;
    return m_markers[*it].point;
}

QString FolderMapModel::statusText() const {
    const int total = totalCount();
    if (total == 0)
        return tr("Keine Bilder");
    if (isScanning())
        //: folder map (F-INFO-08) while the GPS positions are read: done / all images
        return tr("Aufnahmeorte werden gelesen … %1 von %2").arg(scannedCount()).arg(total);
    //: folder map (F-INFO-08): "312 von 412 Bildern mit Aufnahmeort" (%1 images with a position, %n all)
    return tr("%1 von %n Bildern mit Aufnahmeort", nullptr, total).arg(locatedCount());
}

void FolderMapModel::cancelScan() {
    if (m_cancel)
        m_cancel->store(true);
    m_cancel.reset();
    m_pool->clear(); // queued batches go; running ones stop at their next file
}

void FolderMapModel::startScan() {
    QVector<ScanItem> pending;
    for (int i = 0; i < m_entries.size(); ++i)
        if (!m_known[i]) {
            const FileEntry& e = m_entries[i];
            pending << ScanItem{i, e.path, e.pairedPath, e.modified, e.size};
        }
    if (pending.isEmpty())
        return;
    // The current image and its neighbours first (what the user looks at).
    int cur = -1;
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].path == m_current) {
            cur = i;
            break;
        }
    if (cur >= 0)
        std::stable_sort(pending.begin(), pending.end(), [cur](const ScanItem& a, const ScanItem& b) {
            return std::abs(a.index - cur) < std::abs(b.index - cur);
        });
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = m_generation;
    for (int start = 0; start < pending.size(); start += kBatch) {
        const QVector<ScanItem> batch = pending.mid(start, kBatch);
        auto cancel = m_cancel;
        GeoTagCache* cache = m_cache;
        m_pool->start([this, batch, cancel, cache, generation] {
            QVector<QPair<int, std::optional<GeoPoint>>> results;
            for (const ScanItem& item : batch) {
                if (cancel->load())
                    return;
                std::optional<GeoPoint> p = cache->location(item.path, item.modified, item.size);
                if (!p && !item.paired.isEmpty()) { // RAW+JPEG: the other file of the pair
                    const QFileInfo fi(item.paired);
                    p = cache->location(item.paired, fi.lastModified(), fi.size());
                }
                results.append({item.index, p});
            }
            if (cancel->load())
                return;
            // The model waits for its workers before it goes, so `this` is alive here; the
            // queued call is dropped if the model is deleted before it runs.
            QMetaObject::invokeMethod(
                this, [this, generation, results] { onResults(generation, results); }, Qt::QueuedConnection);
        });
    }
}

void FolderMapModel::onResults(quint64 generation,
                               const QVector<QPair<int, std::optional<GeoPoint>>>& results) {
    if (generation != m_generation || !m_open)
        return; // stale (entries changed, map closed)
    QVector<MapMarker> added;
    for (const auto& [index, p] : results) {
        if (index < 0 || index >= m_entries.size() || m_known[index])
            continue;
        m_known[index] = true;
        ++m_scanned;
        if (p)
            added << MapMarker{m_entries[index].path, m_entries[index].name, *p};
    }
    addLocated(added);
    emit countsChanged();
    if (m_scanned == totalCount()) {
        if (!m_userMoved)
            fitAll(true);
        followCurrent(true);
        emit scanFinished();
    }
}

void FolderMapModel::addLocated(const QVector<MapMarker>& added) {
    if (added.isEmpty())
        return;
    for (const MapMarker& m : added) {
        m_markerIndex.insert(m.path, int(m_markers.size()));
        m_markers << m;
    }
    if (!m_backend || !m_open)
        return;
    m_backend->addMarkers(added);
    if (!m_fitted && !m_userMoved)
        fitAll(false); // first positions: show them
    if (std::any_of(added.cbegin(), added.cend(),
                    [this](const MapMarker& m) { return m.path == m_current; })) {
        m_backend->setCurrent(m_current);
        followCurrent(true);
    }
}

void FolderMapModel::followCurrent(bool animated) {
    if (!m_backend || !m_open)
        return;
    const auto p = locationOf(m_current);
    if (!p)
        return;
    if (const auto r = geo::followRegion(m_backend->visibleRegion(), *p))
        m_backend->showRegion(*r, animated);
}

void FolderMapModel::fitAll(bool animated) {
    if (!m_backend || !m_open || m_markers.isEmpty())
        return;
    QVector<GeoPoint> points;
    points.reserve(m_markers.size());
    for (const MapMarker& m : m_markers)
        points << m.point;
    m_backend->showRegion(geo::boundingRegion(points), animated);
    m_fitted = true;
}

} // namespace lf

#include "moc_FolderMap.cpp"
