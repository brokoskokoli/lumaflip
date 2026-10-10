// SPDX-License-Identifier: GPL-3.0-or-later
#include "MenuExtras.h"

#include "Application.h"
#include "ImageView.h"
#include "OpenWith.h"
#include "Share.h"
#include "ViewerWindow.h"
#include "WindowArranger.h"
#include "WindowTabbing.h"
#include "core/Settings.h"

#include <QAction>
#include <QMenu>

namespace lf {

MenuExtras::MenuExtras(Application& app, RegisterCommand registerCommand, QObject* parent)
    : QObject(parent), m_app(app), m_register(std::move(registerCommand)) {}

QString MenuExtras::frontPath() const {
    ViewerWindow* w = m_app.frontWindow();
    return w ? w->currentPath() : QString();
}

void MenuExtras::addOpenWith(QMenu* file) {
    // F-FILE-05: the list comes from a worker (cached per type); asking already when the
    // File menu opens makes it ready by the time the submenu is shown.
    m_openWith = file->addMenu(tr("Öffnen mit"));
    m_openWith->setObjectName(QStringLiteral("menu:openWith"));
    m_register(QStringLiteral("openWith"), m_openWith->menuAction());
    connect(file, &QMenu::aboutToShow, this, [this] {
        const QString p = frontPath();
        if (!p.isEmpty())
            OpenWithService::instance().request(p);
    });
    connect(m_openWith, &QMenu::aboutToShow, this, [this] {
        OpenWithService::instance().populateMenu(m_openWith, frontPath(), m_app.shell(), m_app.frontWindow());
    });
}

void MenuExtras::addShare(QMenu* file) {
    // F-FILE-06: "Teilen …" (system picker) and the services directly.
    m_share = file->addMenu(tr("Teilen"));
    m_share->setObjectName(QStringLiteral("menu:share"));
    m_register(QStringLiteral("share"), m_share->menuAction());
    auto fill = [this] {
        const QString p = frontPath();
        populateShareMenu(m_share, p.isEmpty() ? QStringList() : QStringList{p},
                          [this] { showSharePickerForFrontWindow(); });
    };
    connect(m_share, &QMenu::aboutToShow, this, fill);
    // An entry from the start so the native menu shows the submenu arrow; the services are
    // looked up when it opens.
    populateShareMenu(m_share, {}, [this] { showSharePickerForFrontWindow(); });
}

bool MenuExtras::showSharePickerForFrontWindow() {
    ViewerWindow* w = m_app.frontWindow();
    if (!w || w->currentPath().isEmpty())
        return false;
    // Anchored at the top middle of the image area.
    ImageView* view = w->view();
    const QPoint topMiddle = view->mapTo(w, QPoint(view->width() / 2, 0));
    return showSharePicker(w, QRect(topMiddle - QPoint(1, 0), QSize(2, 2)), {w->currentPath()});
}

void MenuExtras::addWindowCommands(QMenu* window) {
    // F-WIN-04
    m_arrange = window->addMenu(tr("Anordnen"));
    m_arrange->setObjectName(QStringLiteral("menu:arrange"));
    const std::pair<const char*, Arrangement> items[] = {{"sideBySide", Arrangement::SideBySide},
                                                         {"stacked", Arrangement::Stacked},
                                                         {"grid", Arrangement::Grid},
                                                         {"cascade", Arrangement::Cascade}};
    const QString labels[] = {tr("Nebeneinander"), tr("Untereinander"), tr("Raster"), tr("Kaskadieren")};
    int i = 0;
    for (const auto& [id, arrangement] : items) {
        QAction* act = m_arrange->addAction(labels[i++]);
        const QString cid = QStringLiteral("arrange:") + QLatin1String(id);
        act->setObjectName(QStringLiteral("command:") + cid);
        m_register(cid, act);
        const Arrangement a = arrangement;
        connect(act, &QAction::triggered, this, [this, a] { arrangeViewerWindows(m_app, a); });
    }
    // F-WIN-05: tab commands (active when tabs are allowed in the preferences).
    window->addSeparator();
    const std::pair<const char*, TabCommand> tabs[] = {{"previous", TabCommand::SelectPreviousTab},
                                                       {"next", TabCommand::SelectNextTab},
                                                       {"moveToNewWindow", TabCommand::MoveTabToNewWindow},
                                                       {"merge", TabCommand::MergeAllWindows},
                                                       {"tabBar", TabCommand::ToggleTabBar}};
    const QString tabLabels[] = {tr("Vorherigen Tab zeigen"), tr("Nächsten Tab zeigen"),
                                 tr("Tab in neues Fenster bewegen"), tr("Alle Fenster zusammenführen"),
                                 tr("Tableiste ein-/ausblenden")};
    i = 0;
    for (const auto& [id, command] : tabs) {
        QAction* act = window->addAction(tabLabels[i++]);
        const QString cid = QStringLiteral("tabs:") + QLatin1String(id);
        act->setObjectName(QStringLiteral("command:") + cid);
        m_register(cid, act);
        m_tabActions << act;
        const TabCommand c = command;
        connect(act, &QAction::triggered, this, [this, c] {
            if (ViewerWindow* w = m_app.frontWindow())
                performTabCommand(w, c);
        });
    }
}

void MenuExtras::refreshState() {
    const bool hasWindow = m_app.frontWindow() != nullptr;
    const bool hasImage = !frontPath().isEmpty();
    if (m_openWith)
        m_openWith->menuAction()->setEnabled(hasImage);
    if (m_share)
        m_share->menuAction()->setEnabled(hasImage);
    if (m_arrange)
        m_arrange->menuAction()->setEnabled(hasWindow);
    const bool tabs = hasWindow && Settings::instance().value(key::AllowTabs).toBool();
    for (QAction* a : std::as_const(m_tabActions)) {
        a->setEnabled(tabs);
        a->setToolTip(tabs ? QString() : tr("Tabs in den Einstellungen erlauben (Fenster)"));
    }
}

} // namespace lf
