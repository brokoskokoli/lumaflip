// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-INFO-08 folder map: every image of the window's collection with a GPS position as a
// marker on a map. This is the platform-neutral part (unit-tested):
//
// - readImageLocation(): the position from the file's metadata without decoding pixels
//   (macOS: ImageIO properties, platform/mac/MacGeoTag.mm; elsewhere the EXIF GPS parser
//   geotag::parseExifGps()).
// - GeoTagCache: positions per path + modification time + size, thread-safe, shared by all
//   windows, so filtering, re-sorting or a second window do not read files again.
// - FolderMapModel: the markers of a collection snapshot (FileEntry list: filters and
//   RAW+JPEG pairs are already applied by the Collection; a pair is one entry and uses the
//   position of the other file if its own has none), read progressively on worker threads
//   (current image outwards), cancelled when the entries change or the map closes; counts
//   ("312 von 412 Bildern mit Ort"); the current image (highlight, recentre when it leaves
//   the visible region); marker clicks become jump requests.
// - MapViewBackend: what the model needs from the map view (macOS: MKMapView with
//   clustering, app/FolderMapView_mac.mm; tests: a fake).
//
// Privacy (SPEC 10.3): nothing is read and no map is created before open() - the user
// opening the map view is the consent; close() stops the map (no more tile loading) and the
// reading. The consent is not stored.

#include "FileEntry.h"
#include "GeoLocation.h"

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QThreadPool;

namespace lf {

// The GPS position stored in the image file (EXIF/XMP via the platform), nullopt if there
// is none. Blocking file access: worker threads only. Implemented per platform.
std::optional<GeoPoint> readImageLocation(const QString& path);

namespace geotag {
// EXIF GPS (GPSLatitude/-Ref, GPSLongitude/-Ref) of a JPEG (APP1 "Exif") or TIFF-based file
// (TIFF, DNG and most RAW formats). Bounds-checked; nullopt if absent or invalid.
std::optional<GeoPoint> parseExifGps(const QByteArray& data);
// parseExifGps() on the first 512 KB of the file.
std::optional<GeoPoint> readExifGps(const QString& path);
} // namespace geotag

// A rectangle on the map in degrees (MapKit's MKCoordinateRegion).
struct GeoRegion {
    GeoPoint center;
    double latitudeDelta = 0;  // full height in degrees
    double longitudeDelta = 0; // full width in degrees
    friend bool operator==(const GeoRegion&, const GeoRegion&) = default;
};

namespace geo {
// `p` lies inside `r` shrunk by `inset` (fraction of the span on each side, 0..0.5).
bool regionContains(const GeoRegion& r, const GeoPoint& p, double inset = 0.0);
// The region showing all `points` with `padding` (fraction of the span added on each side)
// and at least `minSpan` degrees in each direction (a single photo: about 1 km).
GeoRegion boundingRegion(const QVector<GeoPoint>& points, double padding = 0.15, double minSpan = 0.01);
// F-INFO-08 "follows ←/→": the region to move to so that `p` is visible again (same span,
// centred on p), or nullopt if `p` is still inside `visible` (shrunk by `inset`) - then the
// map stays put. Without a known visible region: centred on p with `fallbackSpan`.
std::optional<GeoRegion> followRegion(const std::optional<GeoRegion>& visible, const GeoPoint& p,
                                      double inset = 0.08, double fallbackSpan = 0.05);
} // namespace geo

class GeoTagCache {
public:
    using Reader = std::function<std::optional<GeoPoint>(const QString& path)>;
    explicit GeoTagCache(Reader reader = &readImageLocation);
    static GeoTagCache& instance();

    // The position of `path` (read on first use; blocking, worker threads). `modified`/`size`
    // are part of the key: a changed file is read again.
    std::optional<GeoPoint> location(const QString& path, const QDateTime& modified, qint64 size);
    // Cached answer without reading: outer nullopt = not known yet.
    std::optional<std::optional<GeoPoint>> cached(const QString& path, const QDateTime& modified,
                                                  qint64 size) const;
    int readCount() const { return m_reads.load(); } // files actually read (tests)
    void clear();

private:
    struct Key {
        QString path;
        qint64 modifiedMs = 0;
        qint64 size = -1;
        friend bool operator==(const Key&, const Key&) = default;
        friend size_t qHash(const Key& k, size_t seed = 0) {
            return qHashMulti(seed, k.path, k.modifiedMs, k.size);
        }
    };
    static Key keyOf(const QString& path, const QDateTime& modified, qint64 size);

    Reader m_reader;
    mutable std::mutex m_mutex;
    QHash<Key, std::optional<GeoPoint>> m_map;
    std::atomic<int> m_reads{0};
};

struct MapMarker {
    QString path; // the collection entry (a RAW+JPEG pair: the shown file)
    QString name; // file name (accessibility, callouts)
    GeoPoint point;
    friend bool operator==(const MapMarker&, const MapMarker&) = default;
};

// The map view as the model sees it. The MapKit implementation lives in the app; tests use
// a fake. Callbacks are invoked on the GUI thread.
class MapViewBackend {
public:
    virtual ~MapViewBackend() = default;
    // Creates the map: from here on map data may be loaded from the network.
    virtual void start() = 0;
    // Removes the map: no more loading. start() may follow again.
    virtual void stop() = 0;
    virtual bool isStarted() const = 0;
    virtual void setMarkers(const QVector<MapMarker>& markers) = 0; // replaces all
    virtual void addMarkers(const QVector<MapMarker>& markers) = 0;
    virtual void setCurrent(const QString& path) = 0; // highlighted marker; empty = none
    virtual void showRegion(const GeoRegion& region, bool animated) = 0;
    virtual std::optional<GeoRegion> visibleRegion() const = 0;
    // The hover preview of `path` (answer to markerHovered).
    virtual void setThumbnail(const QString& path, const QImage& image) = 0;

    std::function<void(const QString& path)> markerActivated; // click on a marker
    std::function<void(const QString& path)> markerHovered;   // pointer over a marker; empty = left
    std::function<void()> userMovedMap;                       // pan/zoom by the user
};

class FolderMapModel : public QObject {
    Q_OBJECT
public:
    explicit FolderMapModel(GeoTagCache* cache = &GeoTagCache::instance(), QObject* parent = nullptr);
    ~FolderMapModel() override; // cancels and waits for the reading

    // The view (not owned; may be null). Must outlive the model or be reset first.
    void setBackend(MapViewBackend* backend);
    MapViewBackend* backend() const { return m_backend; }

    // The collection's current entries (in order). Cancels a running scan; positions known
    // to the cache appear at once, the others are read on workers while the map is open.
    void setEntries(const QVector<FileEntry>& entries);
    void setCurrent(const QString& path);
    QString current() const { return m_current; }

    // The user opened / closed the map view (SPEC 10.3, see above).
    void open();
    void close();
    bool isOpen() const { return m_open; }

    int totalCount() const { return int(m_entries.size()); }
    int locatedCount() const { return int(m_markers.size()); }
    int scannedCount() const { return m_scanned; }
    bool isScanning() const { return m_open && m_scanned < totalCount(); }
    // "312 von 412 Bildern mit Ort" (while reading: "Orte werden gelesen … 120 von 412").
    QString statusText() const;
    const QVector<MapMarker>& markers() const { return m_markers; }
    std::optional<GeoPoint> locationOf(const QString& path) const;

    void setWorkerThreads(int n); // default 2

signals:
    void countsChanged();
    void scanFinished();
    // A marker was clicked: the window shows that image.
    void jumpRequested(const QString& path);
    // The pointer is over a marker: the view wants its thumbnail (setThumbnail on the backend).
    void thumbnailWanted(const QString& path);

private:
    struct Job;
    void startScan();
    void cancelScan();
    void onResults(quint64 generation, const QVector<QPair<int, std::optional<GeoPoint>>>& results);
    void addLocated(const QVector<MapMarker>& added);
    void followCurrent(bool animated);
    void fitAll(bool animated);
    void connectBackend();

    GeoTagCache* m_cache = nullptr;
    MapViewBackend* m_backend = nullptr;
    QThreadPool* m_pool = nullptr;
    QVector<FileEntry> m_entries;
    QVector<bool> m_known; // per entry: position known (with or without a result)
    QVector<MapMarker> m_markers;
    QHash<QString, int> m_markerIndex; // path -> index in m_markers
    QString m_current;
    int m_scanned = 0;
    bool m_open = false;
    bool m_userMoved = false;
    bool m_fitted = false;
    quint64 m_generation = 0;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

} // namespace lf
