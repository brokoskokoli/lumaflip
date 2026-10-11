// SPDX-License-Identifier: GPL-3.0-or-later
// Windows: no system map view (MapKit, F-INFO-08). The folder map panel shows a note and
// the number of located photos instead of a map; the model logic (reading positions,
// current photo) runs unchanged.
#include "FolderMapView.h"

#include <QCoreApplication>
#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QWidget>

namespace lf {

namespace {

class UnavailableMapView final : public MapViewBackend {
public:
    explicit UnavailableMapView(QWidget* host) : m_host(host) {}
    ~UnavailableMapView() override { stop(); }

    void start() override {
        if (m_label || !m_host || !m_host->layout())
            return;
        m_label = new QLabel(m_host);
        m_label->setObjectName(QStringLiteral("lfFolderMapContainer"));
        m_label->setWordWrap(true);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setMinimumSize(160, 120);
        m_host->layout()->addWidget(m_label);
        update();
    }
    void stop() override {
        if (m_label)
            delete m_label.data();
        m_label = nullptr;
    }
    bool isStarted() const override { return !m_label.isNull(); }
    void setMarkers(const QVector<MapMarker>& markers) override {
        m_count = int(markers.size());
        update();
    }
    void addMarkers(const QVector<MapMarker>& markers) override {
        m_count += int(markers.size());
        update();
    }
    void setCurrent(const QString&) override {}
    void showRegion(const GeoRegion&, bool) override {}
    std::optional<GeoRegion> visibleRegion() const override { return std::nullopt; }
    void setThumbnail(const QString&, const QImage&) override {}

private:
    void update() {
        if (!m_label)
            return;
        m_label->setText(
            QCoreApplication::translate(
                "lf::FolderMapView",
                "Karten sind unter Windows nicht verfügbar. Bilder mit Aufnahmeort in diesem Ordner: %1")
                .arg(m_count));
    }

    QPointer<QWidget> m_host;
    QPointer<QLabel> m_label;
    int m_count = 0;
};

} // namespace

std::unique_ptr<MapViewBackend> createMapViewBackend(QWidget* host) {
    return std::make_unique<UnavailableMapView>(host);
}

} // namespace lf
