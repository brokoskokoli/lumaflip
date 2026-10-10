// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M2 menu entries that need more than an Action (MainMenu adds them through this class):
// Ablage → Öffnen mit ▸ (F-FILE-05), Ablage → Teilen ▸ (F-FILE-06),
// Fenster → Anordnen ▸ (F-WIN-04) and the tab commands (F-WIN-05).
// Command ids (MainMenu::command): "openWith", "share" (submenu actions),
// "arrange:sideBySide|stacked|grid|cascade", "tabs:merge|moveToNewWindow|next|previous|tabBar".

#include <QObject>
#include <QString>

#include <functional>

class QAction;
class QMenu;

namespace lf {

class Application;

class MenuExtras : public QObject {
    Q_OBJECT
public:
    using RegisterCommand = std::function<void(const QString& id, QAction* action)>;
    MenuExtras(Application& app, RegisterCommand registerCommand, QObject* parent);

    void addOpenWith(QMenu* file);
    void addShare(QMenu* file);
    void addWindowCommands(QMenu* window);
    void refreshState();

    QMenu* openWithMenu() const { return m_openWith; }
    QMenu* shareMenu() const { return m_share; }
    QMenu* arrangeMenu() const { return m_arrange; }
    // "Teilen …": picker next to the front window's image (F-FILE-06).
    bool showSharePickerForFrontWindow();

private:
    QString frontPath() const;

    Application& m_app;
    RegisterCommand m_register;
    QMenu* m_openWith = nullptr;
    QMenu* m_share = nullptr;
    QMenu* m_arrange = nullptr;
    QList<QAction*> m_tabActions;
};

} // namespace lf
