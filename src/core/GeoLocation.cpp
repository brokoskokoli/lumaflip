// SPDX-License-Identifier: GPL-3.0-or-later
#include "GeoLocation.h"

#include <QCoreApplication>
#include <QUrlQuery>

#include <cmath>

namespace lf {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::GeoLocation)
};

QString dms(double degrees, const QString& hemisphere, const QLocale& locale) {
    double v = std::abs(degrees);
    int d = int(v);
    double minutesF = (v - d) * 60.0;
    int m = int(minutesF);
    double s = std::round((minutesF - m) * 60.0 * 10.0) / 10.0;
    if (s >= 60.0) { // rounding carried over
        s = 0;
        if (++m == 60) {
            m = 0;
            ++d;
        }
    }
    //: GPS coordinate in degrees, minutes, seconds, e.g. "48° 8′ 13,6″ N"
    return Tr::tr("%1° %2′ %3″ %4").arg(d).arg(m).arg(formatDecimal(s, 1, locale), hemisphere);
}

} // namespace

namespace geo {

bool isValid(const GeoPoint& p) {
    return std::isfinite(p.latitude) && std::isfinite(p.longitude) && std::abs(p.latitude) <= 90.0 &&
           std::abs(p.longitude) <= 180.0;
}

QString formatDecimal(const GeoPoint& p, const QLocale& locale) {
    //: GPS position: latitude, longitude
    return Tr::tr("%1, %2").arg(formatLatitude(p.latitude, locale), formatLongitude(p.longitude, locale));
}

QString formatDms(const GeoPoint& p, const QLocale& locale) {
    const QString lat = dms(p.latitude,
                            p.latitude < 0 ? QCoreApplication::translate("lf::Format", "S", "south")
                                           : QCoreApplication::translate("lf::Format", "N", "north"),
                            locale);
    const QString lon = dms(p.longitude,
                            p.longitude < 0 ? QCoreApplication::translate("lf::Format", "W", "west")
                                            : QCoreApplication::translate("lf::Format", "O", "east"),
                            locale);
    return Tr::tr("%1, %2").arg(lat, lon);
}

QString machineText(const GeoPoint& p) {
    return QStringLiteral("%1,%2").arg(p.latitude, 0, 'f', 6).arg(p.longitude, 0, 'f', 6);
}

QUrl mapsUrl(const GeoPoint& p, const QString& label) {
    QUrl url(QStringLiteral("maps://"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("ll"), machineText(p));
    if (!label.isEmpty())
        q.addQueryItem(QStringLiteral("q"), label);
    url.setQuery(q);
    return url;
}

} // namespace geo

// ---- MapPreviewController -----------------------------------------------------------------

MapPreviewController::MapPreviewController(MapSnapshotProvider* provider, QObject* parent)
    : QObject(parent), m_provider(provider) {}

void MapPreviewController::setLocation(const std::optional<GeoPoint>& p) {
    const std::optional<GeoPoint> valid = p && geo::isValid(*p) ? p : std::nullopt;
    if (valid == m_location && m_state != State::NoLocation)
        return;
    m_location = valid;
    ++m_token; // a pending snapshot of the previous position is dropped
    if (m_provider)
        m_provider->cancel();
    m_image = QImage();
    m_error.clear();
    if (!m_location) {
        setState(State::NoLocation);
        return;
    }
    if (m_consent)
        request();
    else
        setState(State::Idle); // SPEC 10.3: no network before the click
}

void MapPreviewController::showMap() {
    m_consent = true;
    if (m_location && (m_state == State::Idle || m_state == State::Failed))
        request();
}

void MapPreviewController::setSize(QSize pointSize, qreal devicePixelRatio) {
    if (pointSize.isEmpty())
        return;
    m_size = pointSize;
    m_dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
}

void MapPreviewController::request() {
    if (!m_provider) {
        m_error = tr("Karten sind auf diesem System nicht verfügbar.");
        setState(State::Failed);
        return;
    }
    const quint64 token = ++m_token;
    ++m_requests;
    setState(State::Loading);
    m_provider->request(
        *m_location, m_size, m_dpr, this, [this, token](const QImage& img, const QString& error) {
            if (token != m_token)
                return; // another position meanwhile
            m_image = img;
            m_error = img.isNull() ? (error.isEmpty() ? tr("Die Karte konnte nicht geladen werden.") : error)
                                   : QString();
            setState(img.isNull() ? State::Failed : State::Ready);
        });
}

void MapPreviewController::setState(State s) {
    m_state = s;
    emit stateChanged(s);
}

} // namespace lf
