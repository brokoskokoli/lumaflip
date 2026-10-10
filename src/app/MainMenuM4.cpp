// SPDX-License-Identifier: GPL-3.0-or-later
// M4 menu entries (MainMenu.h): F-MAC-07 Quick Look, F-FILE-09 wallpaper, F-FILE-10 custom
// actions (Ablage), F-MAC-06 Live Text and F-OPEN-10 right-to-left comics (Darstellung).

#include "Application.h"
#include "CustomActionsUi.h"
#include "MainMenu.h"
#include "MenuKeyEquivalents.h"
#include "ViewerWindow.h"
#include "WindowIntegration.h"
#include "core/Collection.h"
#include "core/LiveText.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"

#include <QAction>
#include <QMenu>

namespace lf {

void MainMenu::buildM4(QMenu* file, QMenu* view) {
    if (file) {
        QAction* quickLook = add(file, Action::QuickLook, tr("Quick Look")); // F-MAC-07
#ifdef LF_GENERIC_PLATFORM
        quickLook->setVisible(false); // macOS only (docs/LINUX.md)
#else
        Q_UNUSED(quickLook);
#endif
        add(file, Action::SetWallpaper, tr("Als Schreibtischhintergrund festlegen …")); // F-FILE-09
        m_customMenu = file->addMenu(tr("Eigene Aktionen"));                            // F-FILE-10
        m_customMenu->setObjectName(QStringLiteral("menu:customActions"));
        rebuildCustomActions();
        connect(&CustomActionsManager::instance(), &CustomActionsManager::actionsChanged, this,
                &MainMenu::rebuildCustomActions);
    }
    if (view) {
        view->addSeparator();
        QAction* liveText = add(view, Action::ToggleLiveText, tr("Live Text")); // F-MAC-06
        liveText->setCheckable(true);
#ifdef LF_GENERIC_PLATFORM
        liveText->setVisible(false); // VisionKit, macOS only (docs/LINUX.md)
#endif
        add(view, Action::ToggleRightToLeft, tr("Comics von rechts nach links lesen"))
            ->setCheckable(true); // F-OPEN-10
    }
}

void MainMenu::rebuildCustomActions() {
    if (!m_customMenu)
        return;
    for (QAction* a : std::as_const(m_customEntries)) {
        m_commands.remove(a->objectName().mid(int(qstrlen("command:"))));
        // deleteLater: the rebuild can run inside this action's own triggered() (the first
        // run saves the confirmation, F-FILE-10); deleting it there is a use after free.
        m_customMenu->removeAction(a);
        a->deleteLater();
    }
    m_customEntries.clear();
    m_customMenu->clear();
    const QList<CustomAction> actions = CustomActionsManager::instance().actions();
    for (const CustomAction& c : actions) {
        const QString id = c.id;
        QAction* act = addCommand(m_customMenu, QStringLiteral("customAction:") + id, c.name, c.keys);
        connect(act, &QAction::triggered, this, [this, id] {
            if (ViewerWindow* w = m_app.frontWindow())
                CustomActionsManager::instance().run(id, w);
        });
        m_customEntries << act;
    }
    if (actions.isEmpty()) {
        QAction* none = m_customMenu->addAction(tr("Keine – in den Einstellungen anlegen"));
        none->setEnabled(false);
        m_customEntries << none;
    }
    m_customMenu->addSeparator();
    add(m_customMenu, Action::ShowActionLog, tr("Protokoll …")); // deleted by the next clear()
    QAction* prefs = m_customMenu->addAction(tr("Aktionen bearbeiten …"));
    connect(prefs, &QAction::triggered, &m_app, &Application::showPreferences);
    m_customEntries << prefs;
    refreshShortcuts();
}

void MainMenu::refreshM4State() {
    ViewerWindow* w = m_app.frontWindow();
    WindowIntegration* integration = w ? WindowIntegration::of(w) : nullptr;
    for (Action a :
         {Action::QuickLook, Action::SetWallpaper, Action::ToggleLiveText, Action::ToggleRightToLeft})
        if (QAction* act = action(a))
            act->setEnabled(w != nullptr);
    if (QAction* act = action(Action::ToggleLiveText))
        act->setChecked(integration && integration->liveTextActive());
    if (QAction* act = action(Action::ToggleRightToLeft)) {
        act->setChecked(Settings::instance().value(key::ArchiveRightToLeft).toBool());
        act->setEnabled(w && w->collection()->isContainerCollection() &&
                        vpath::isComicArchive(w->collection()->containerPath()));
    }
    const bool member = w && vpath::isMember(w->currentPath());
    for (QAction* act : std::as_const(m_customEntries))
        if (act->objectName().startsWith(QLatin1String("command:customAction:")))
            act->setEnabled(w && !member);
}

} // namespace lf
