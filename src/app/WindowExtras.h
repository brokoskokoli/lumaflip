// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M3 view features of a viewer window, kept out of ViewerWindow.cpp (which only calls the
// hooks below): grid and compare (OverviewController, F-GRID-02/03), magnifier
// (LoupeController, F-VIEW-11), "Immer im Vordergrund" (F-VIEW-12), minimal window mode
// (F-VIEW-13), go to / shuffle (NavigationExtras, F-NAV-06), type-to-filter
// (QuickFilterBar, F-NAV-08) and the status of coupled windows (CouplingManager, F-WIN-07).

#include "core/InputBindings.h"

#include <QObject>
#include <QPointF>
#include <QStringList>

class QLabel;
class QStatusBar;
class QWidget;

namespace lf {

class Application;
class CollectionExtras;
class LoupeController;
class NavigationExtras;
class OverviewController;
class QuickFilterBar;
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

    // ---- F-VIEW-12 / F-VIEW-13 (per window) ----
    bool alwaysOnTop() const { return m_onTop; }
    void setAlwaysOnTop(bool on);
    bool minimalWindow() const { return m_minimal; }
    void setMinimalWindow(bool on);
    QWidget* moveHandle() const { return m_handle; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void watchKeys(QWidget* widget);
    void placeHandle();

    ViewerWindow& m_window;
    Application& m_app;
    OverviewController* m_overview = nullptr;
    LoupeController* m_loupe = nullptr;
    NavigationExtras* m_nav = nullptr;
    QuickFilterBar* m_filter = nullptr;
    CollectionExtras* m_collection = nullptr;
    QWidget* m_handle = nullptr;
    QLabel* m_shuffleLabel = nullptr;
    QLabel* m_coupledLabel = nullptr;
    QLabel* m_nameLabel = nullptr;
    bool m_onTop = false;
    bool m_minimal = false;
};

} // namespace lf
