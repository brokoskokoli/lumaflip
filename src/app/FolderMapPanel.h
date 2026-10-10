// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-INFO-08: the folder map of a viewer window - a side panel (dock on the right, owned by
// WindowExtras) with a header ("312 von 412 Bildern mit Ort", close button) and the map.
// It feeds the window's collection (filters and RAW+JPEG pairs applied) and current image
// into a FolderMapModel, turns marker clicks into "show this image", and answers hover
// requests with thumbnails from the ThumbnailCache.
//
// SPEC 10.3: the map view is created only by openMap() (the user opened the panel: menu
// Darstellung → Karte, ⇧⌘M, or "Ordnerkarte" in the info panel) and removed by closeMap().

#include <QWidget>

#include <functional>
#include <memory>

class QLabel;
class QToolButton;

namespace lf {

class FolderMapModel;
class MapViewBackend;
class ViewerWindow;

class FolderMapPanel : public QWidget {
    Q_OBJECT
public:
    explicit FolderMapPanel(ViewerWindow& window, QWidget* parent = nullptr);
    ~FolderMapPanel() override;

    void openMap();
    void closeMap();
    bool isMapOpen() const;

    FolderMapModel* model() const { return m_model; }
    MapViewBackend* backend() const { return m_backend.get(); }
    QLabel* statusLabel() const { return m_status; }
    QToolButton* closeButton() const { return m_close; }

    // Tests: replaces the MapKit view (no network); empty = MapKit again.
    using BackendFactory = std::function<std::unique_ptr<MapViewBackend>(QWidget* host)>;
    static void setBackendFactory(BackendFactory factory);

signals:
    void closeRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void syncEntries();
    void updateStatus();
    void requestThumbnail(const QString& path);
    void retranslateUi();

    ViewerWindow& m_window;
    FolderMapModel* m_model = nullptr;
    QWidget* m_mapHost = nullptr;
    std::unique_ptr<MapViewBackend> m_backend;
    QLabel* m_status = nullptr;
    QLabel* m_empty = nullptr;
    QToolButton* m_close = nullptr;
    QString m_thumbPath;
};

} // namespace lf
