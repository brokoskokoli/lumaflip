// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Side panels of a viewer window (SPEC 5.1): info panel on the right (F-INFO-02/03/06)
// and filmstrip at the bottom (F-GRID-01), as dock areas of the QMainWindow. Each window
// has its own visibility; toggling remembers it for new windows (key::ShowInfoPanel,
// key::ShowFilmstrip). In fullscreen the panels hide with the rest of the chrome and
// come back when the mouse reaches an edge (F-VIEW-10). Panels are created on first use.

#include <QObject>
#include <QPoint>

class QDockWidget;

namespace lf {

class Filmstrip;
class ImageProvider;
class InfoPanel;
class ViewerWindow;

class WindowPanels : public QObject {
    Q_OBJECT
public:
    // The filmstrip takes its thumbnails from ThumbnailCache::instance().
    WindowPanels(ViewerWindow& window, ImageProvider& provider);
    ~WindowPanels() override;

    // When the window is shown the first time: panels enabled in the settings appear.
    void restore();
    void toggleInfoPanel();
    void toggleFilmstrip();
    void setInfoPanelEnabled(bool on);
    void setFilmstripEnabled(bool on);
    bool infoPanelEnabled() const { return m_infoOn; } // wanted by the user (menu check mark)
    bool filmstripEnabled() const { return m_filmOn; }
    bool infoPanelShown() const; // actually visible now
    bool filmstripShown() const;

    // Fullscreen chrome (F-VIEW-10): hidden panels stay enabled.
    void setChromeVisible(bool visible);
    bool underMouse() const;
    // Window coordinates near an edge where a panel lives (fullscreen reveal).
    bool isRevealEdge(QPoint windowPos, int edgePx) const;

    InfoPanel* infoPanel() const { return m_info; } // null until first shown
    Filmstrip* filmstrip() const { return m_film; }

private:
    void ensureInfoPanel();
    void ensureFilmstrip();
    void apply();
    void updateInfoFile();

    ViewerWindow& m_window;
    ImageProvider& m_provider;
    QDockWidget* m_infoDock = nullptr;
    QDockWidget* m_filmDock = nullptr;
    InfoPanel* m_info = nullptr;
    Filmstrip* m_film = nullptr;
    bool m_infoOn = false;
    bool m_filmOn = false;
    bool m_chrome = true;
    bool m_restored = false;
};

} // namespace lf
