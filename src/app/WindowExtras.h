// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M3 view features of a viewer window, kept out of ViewerWindow.cpp (which only calls the
// hooks below): grid and compare (OverviewController, F-GRID-02/03), magnifier
// (LoupeController, F-VIEW-11), "Immer im Vordergrund" (F-VIEW-12), minimal window mode
// with the fading title strip (TitleStripController, F-VIEW-13/14), go to / shuffle
// (NavigationExtras, F-NAV-06), type-to-filter (QuickFilterBar, F-NAV-08) and the status
// of coupled windows (CouplingManager, F-WIN-07).

#include "core/InputBindings.h"

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QStringList>

class QDockWidget;
class QLabel;
class QStatusBar;
class QWidget;

namespace lf {

class Application;
class CollectionExtras;
class FolderMapPanel;
class LoupeController;
class NavigationExtras;
class OverviewController;
class QuickFilterBar;
class TitleStripController;
class ViewerWindow;

class WindowExtras : public QObject {
    Q_OBJECT
public:
    WindowExtras(ViewerWindow& window, Application& app);
    ~WindowExtras() override;

    OverviewController* overview() const { return m_overview; }
    LoupeController* loupe() const { return m_loupe; }
    NavigationExtras* navigation() const { return m_nav; }
    QuickFilterBar* quickFilter() const { return m_filter; }
    CollectionExtras* collectionExtras() const { return m_collection; } // F-OPEN-08, F-FMT-06

    // ---- F-INFO-08 folder map (dock on the right, created on first use) ----
    FolderMapPanel* folderMap() const { return m_map; }
    bool folderMapShown() const;
    // Shows the panel and opens the map (the consent for this window, SPEC 10.3) or closes it.
    void setFolderMapShown(bool on);

    // ---- hooks called by ViewerWindow ----
    bool perform(Action action, QPointF pos); // true: handled here
    bool ratingKeysActive() const;            // grid / compare: digits rate (SPEC 5.2)
    void attachStatusBar(QStatusBar* bar);
    void updateStatus();
    QStringList statusParts() const; // "Zufällig", "Gekoppelt", "Name: „…“"
    void windowStateChanged();       // shown, full screen left …: native modes again
    QString emptyStateText() const;  // the name filter matches nothing; else empty
    QWidget* contentWidget() const;  // image view, grid or compare view
    void focusContent();

    // ---- F-VIEW-12 / F-VIEW-13/14 (per window) ----
    bool alwaysOnTop() const { return m_onTop; }
    void setAlwaysOnTop(bool on);
    // F-VIEW-13 "Minimaler Fenstermodus" = the borderless window with the title strip that
    // fades in (F-VIEW-14); new windows follow the setting key::TitleBar.
    bool minimalWindow() const;
    void setMinimalWindow(bool on);
    TitleStripController* titleStrip() const { return m_titleStrip; }
    // Windows: the menu bar is collapsed into the title strip (ViewerWindow keeps it so).
    bool menuInTitleStrip() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void watchKeys(QWidget* widget);

    ViewerWindow& m_window;
    Application& m_app;
    OverviewController* m_overview = nullptr;
    LoupeController* m_loupe = nullptr;
    NavigationExtras* m_nav = nullptr;
    QuickFilterBar* m_filter = nullptr;
    CollectionExtras* m_collection = nullptr;
    TitleStripController* m_titleStrip = nullptr;
    FolderMapPanel* m_map = nullptr;
    QDockWidget* m_mapDock = nullptr;
    // Remembered once: the window's own centralWidget() must not be queried from the
    // event filter while the window is being destroyed (its layout is already gone).
    QPointer<QWidget> m_central;
    QLabel* m_shuffleLabel = nullptr;
    QLabel* m_coupledLabel = nullptr;
    QLabel* m_nameLabel = nullptr;
    bool m_onTop = false;
};

} // namespace lf
