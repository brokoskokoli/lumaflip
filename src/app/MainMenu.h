// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The native menu bar shared by all windows (F-MAC-01), the Window menu list and
// the Dock menu (F-WIN-10). One parentless QMenuBar: on macOS it is the global
// menu bar, also when no window is open. Commands act on the front window.
// Shortcuts come from InputBindings and follow the settings (F-SET-02).

#include "core/InputBindings.h"

#include <QHash>
#include <QObject>
#include <QStringList>

class QAction;
class QActionGroup;
class QMenu;
class QMenuBar;

namespace lf {

class Application;
class MenuExtras;

class MainMenu : public QObject {
    Q_OBJECT
public:
    explicit MainMenu(Application& app);
    ~MainMenu() override;

    QMenuBar* menuBar() const { return m_bar; }
    QMenu* dockMenu() const { return m_dock; }
    QList<QMenu*> menus() const;     // top level menus in order
    QAction* action(Action a) const; // nullptr if the action is not in the menu
    // Commands without an Action: "open", "openFolder", "makeDefault", "about", "quit",
    // "statusBar", "minimize", "zoomWindow", "bringAllToFront", "zoom:<modeId>",
    // "background:<id>", "background:custom", "sort:finder", "sort:<key>", "sort:asc", "sort:desc".
    QAction* command(const QString& id) const;
    MenuExtras* extras() const { return m_extras; } // M2 submenus (open with, share, arrange, tabs)
    QStringList windowMenuTitles() const;           // window entries of the Window menu
    QStringList dockMenuTitles() const;

    void refreshShortcuts();  // after the bindings changed
    void refreshState();      // check marks / enabled state for the front window
    void refreshWindowList(); // Window menu + Dock menu

private:
    QAction* add(QMenu* menu, Action a, const QString& text = {});
    QAction* addCommand(QMenu* menu, const QString& id, const QString& text,
                        const QKeySequence& shortcut = {});
    void build();
    void rebuildRecent(bool withIcons = true);
    // M2 media entries (MainMenuMedia.cpp): slideshow, pages, animation, saving the rotation.
    void buildMedia(QMenu* view, QMenu* nav, QMenu* image);
    void refreshMediaState();

    Application& m_app;
    QMenuBar* m_bar = nullptr;
    QMenu* m_dock = nullptr;
    QMenu* m_windowMenu = nullptr;
    QMenu* m_recentMenu = nullptr;
    QAction* m_windowListStart = nullptr; // separator before the window entries
    QList<QAction*> m_windowEntries;
    QList<QAction*> m_dockEntries;
    QHash<int, QAction*> m_actions;
    QHash<QString, QAction*> m_commands;
    MenuExtras* m_extras = nullptr;
    QAction* m_undo = nullptr;
    QAction* m_redo = nullptr;
};

} // namespace lf
