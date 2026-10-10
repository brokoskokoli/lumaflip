// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GPS position of an image (F-INFO-04): display texts, the "In Karten öffnen" link and the
// mini map. SPEC 10.3: map tiles are network traffic and are only loaded after the user
// asked for them (click on "Karte anzeigen"), only while the info panel is visible and only
// for the image it shows; "Karte ausblenden" takes the consent back. MapPreviewController
// enforces that: it never asks its MapSnapshotProvider before showMap(), after hideMap() or
// while it is inactive (setActive(false): the panel is hidden).

#include "LocaleFormat.h"

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

namespace lf {

struct GeoPoint {
    double latitude = 0;  // degrees, north positive
    double longitude = 0; // degrees, east positive
    friend bool operator==(const GeoPoint&, const GeoPoint&) = default;
};

namespace geo {
// Finite and within ±90° / ±180°.
bool isValid(const GeoPoint& p);
// "48,1371° N, 122,4194° W" (UI language, LocaleFormat.h).
QString formatDecimal(const GeoPoint& p, const QLocale& locale = formatLocale());
// "48° 8′ 13,6″ N, 122° 25′ 9,8″ W".
QString formatDms(const GeoPoint& p, const QLocale& locale = formatLocale());
// "48.137100,-122.419400" (always with '.', for the clipboard and other apps).
QString machineText(const GeoPoint& p);
// Apple Maps link that opens the Maps app at the position with a pin ("maps://?ll=…&q=…").
QUrl mapsUrl(const GeoPoint& p, const QString& label = QString());
} // namespace geo

// Renders a map image around a position (macOS: MapKit snapshot, app/MapSnapshot.h). Network access is done
// by the implementation; `done` runs on the thread of `context` (or not at all if it is
// gone). One request at a time per provider; a new request replaces the previous one.
class MapSnapshotProvider {
public:
    virtual ~MapSnapshotProvider() = default;
    using Done = std::function<void(const QImage& image, const QString& error)>;
    virtual void request(const GeoPoint& p, QSize pointSize, qreal devicePixelRatio, QObject* context,
                         Done done) = 0;
    virtual void cancel() = 0;
};

// State of the mini map in the info panel (no widgets: unit-testable).
class MapPreviewController : public QObject {
    Q_OBJECT
public:
    enum class State {
        NoLocation, // the image has no GPS position
        Idle,       // position known, map not requested (no network)
        Loading,    // after the click: the provider renders
        Ready,      // image() holds the map
        Failed,     // error() says why (offline, no provider …)
    };
    Q_ENUM(State)

    explicit MapPreviewController(MapSnapshotProvider* provider, QObject* parent = nullptr);

    // The shown image changed. Without consent (see showMap) nothing is requested.
    void setLocation(const std::optional<GeoPoint>& p);
    std::optional<GeoPoint> location() const { return m_location; }
    // The user clicked "Karte anzeigen": loads the map. The consent holds for this
    // controller (one window) from now on, so the next images with a position load their
    // map without another click; it is not stored.
    void showMap();
    // "Karte ausblenden": the consent is taken back, a pending request is dropped and the
    // map goes; the next image needs a click again.
    void hideMap();
    bool hasConsent() const { return m_consent; }
    // Whether the map may be requested at all (the info panel is visible). Inactive: no
    // request, a pending one is dropped; becoming active again (with consent) loads the map
    // of the current position. Default: active.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    void setSize(QSize pointSize, qreal devicePixelRatio);

    State state() const { return m_state; }
    QImage image() const { return m_image; }
    QString error() const { return m_error; }
    int requestCount() const { return m_requests; } // tests

signals:
    void stateChanged(lf::MapPreviewController::State state);

private:
    void request();
    void setState(State s);

    MapSnapshotProvider* m_provider = nullptr;
    std::optional<GeoPoint> m_location;
    bool m_consent = false;
    bool m_active = true;
    State m_state = State::NoLocation;
    QImage m_image;
    QString m_error;
    QSize m_size{240, 160};
    qreal m_dpr = 2.0;
    quint64 m_token = 0;
    int m_requests = 0;
};

} // namespace lf
