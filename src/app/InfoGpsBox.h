// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-INFO-04: the GPS part of the info panel - position in degrees/minutes/seconds,
// "Karte anzeigen" (mini map, loaded only after this click, SPEC 10.3) and
// "In Karten öffnen" (Apple Maps). Hidden for images without a position.

#include "core/GeoLocation.h"

#include <QWidget>

#include <memory>
#include <optional>

class QLabel;
class QPushButton;

namespace lf {

class InfoGpsBox : public QWidget {
    Q_OBJECT
public:
    // `provider` may be null (no map renderer: the map button reports that).
    explicit InfoGpsBox(std::unique_ptr<MapSnapshotProvider> provider, QWidget* parent = nullptr);
    ~InfoGpsBox() override;

    // The image's position (nullopt: box hidden); `label` names the pin in Maps.
    void setLocation(const std::optional<GeoPoint>& p, const QString& label = QString());

    MapPreviewController* controller() const { return m_controller; }
    QPushButton* showMapButton() const { return m_showMap; }
    QPushButton* openMapsButton() const { return m_openMaps; }
    QLabel* mapLabel() const { return m_map; }
    QLabel* coordinatesLabel() const { return m_coords; }
    // Tests: replaces opening the Maps app.
    void setUrlOpener(std::function<void(const QUrl&)> opener) { m_opener = std::move(opener); }

protected:
    void changeEvent(QEvent* event) override;

private:
    void updateState();
    void retranslateUi();

    std::unique_ptr<MapSnapshotProvider> m_provider;
    MapPreviewController* m_controller = nullptr;
    QLabel* m_coords = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_map = nullptr;
    QPushButton* m_showMap = nullptr;
    QPushButton* m_openMaps = nullptr;
    QString m_label;
    std::function<void(const QUrl&)> m_opener;
};

} // namespace lf
