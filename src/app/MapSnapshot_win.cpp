// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: no system map service (MapKit); the info panel offers the position as a link to
// a web map instead of a map image (InfoGpsBox shows the error text in place of the map).
#include "MapSnapshot.h"

#include <QCoreApplication>
#include <QPointer>
#include <QTimer>

namespace lf {

namespace {

class UnavailableMapSnapshotProvider final : public MapSnapshotProvider {
public:
    void request(const GeoPoint&, QSize, qreal, QObject* context, Done done) override {
        QPointer<QObject> guard(context);
        QTimer::singleShot(0, context, [guard, done = std::move(done)] {
            if (guard && done)
                done(QImage(), QCoreApplication::translate("lf::MapSnapshot",
                                                           "Karten sind unter Windows nicht verfügbar."));
        });
    }
    void cancel() override {}
};

} // namespace

std::unique_ptr<MapSnapshotProvider> createMapSnapshotProvider() {
    return std::make_unique<UnavailableMapSnapshotProvider>();
}

} // namespace lf
