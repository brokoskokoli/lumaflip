// SPDX-License-Identifier: GPL-3.0-or-later
#include "MenuExtras.h"

#include "Application.h"
#include "ImageView.h"
#include "OpenWith.h"
#include "Share.h"
#include "ViewerWindow.h"
#include "WindowArranger.h"
#include "WindowTabbing.h"
#include "core/ArchiveCache.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"

#include <QAction>
#include <QMenu>
#include <QThreadPool>

namespace lf {

MenuExtras::MenuExtras(Application& app, RegisterCommand registerCommand, QObject* parent)
    : QObject(parent), m_app(app), m_register(std::move(registerCommand)) {}

QString MenuExtras::frontPath() const {
    ViewerWindow* w = m_app.frontWindow();
    return w ? w->currentPath() : QString();
}

QString MenuExtras::frontFile(QString* reason) const {
    reason->clear();
    QString p = frontPath();
    if (p.isEmpty() || !vpath::isMember(p))
        return p;
    // F-OPEN-10: other apps get the extracted copy of an image inside an archive (it carries
    // the member's file name); a PDF page has no file of its own.
    if (vpath::containerKind(vpath::containerOf(p)) == vpath::ContainerKind::Pdf) {
        *reason = tr("Einzelne PDF-Seiten können nicht an andere Apps übergeben werden.");
        return {};
    }
    QString file = ArchiveCache::instance().cachedFile(p);
    if (!file.isEmpty())
        return file;
    // Not extracted (any more): on a worker, ready when the menu opens again.
    QThreadPool::globalInstance()->start([p] {
        QString error;
        ArchiveCache::instance().materialize(p, &error);
    });
    *reason = tr("Das Bild wird aus dem Archiv entpackt – versuche es gleich noch einmal.");
    return {};
}

void MenuExtras::addOpenWith(QMenu* file) {
    // F-FILE-05: the list comes from a worker (cached per type); asking already when the
    // File menu opens makes it ready by the time the submenu is shown.
    m_openWith = file->addMenu(tr("Öffnen mit"));
    m_openWith->setObjectName(QStringLiteral("menu:openWith"));
    m_register(QStringLiteral("openWith"), m_openWith->menuAction());
    connect(file, &QMenu::aboutToShow, this, [this] {
        QString reason;
        const QString p = frontFile(&reason);
        if (!p.isEmpty())
            OpenWithService::instance().request(p);
    });
    connect(m_openWith, &QMenu::aboutToShow, this, [this] {
        QString reason;
        const QString p = frontFile(&reason);
        if (p.isEmpty() && !reason.isEmpty()) {
            m_openWith->clear();
            m_openWith->addAction(reason)->setEnabled(false);
            return;
        }
        OpenWithService::instance().populateMenu(m_openWith, p, m_app.shell(), m_app.frontWindow());
    });
}

void MenuExtras::addShare(QMenu* file) {
    // F-FILE-06: "Teilen …" (system picker) and the services directly.
    m_share = file->addMenu(tr("Teilen"));
    m_share->setObjectName(QStringLiteral("menu:share"));
    m_register(QStringLiteral("share"), m_share->menuAction());
#ifdef LF_GENERIC_PLATFORM
    m_share->menuAction()->setVisible(false); // no share sheet outside macOS (docs/LINUX.md)
#endif
    auto fill = [this] {
        QString reason;
        const QString p = frontFile(&reason);
        if (p.isEmpty() && !reason.isEmpty()) {
            m_share->clear();
            m_share->addAction(reason)->setEnabled(false);
            return;
        }
        populateShareMenu(m_share, p.isEmpty() ? QStringList() : QStringList{p},
                          [this] { showSharePickerForFrontWindow(); });
    };
    connect(m_share, &QMenu::aboutToShow, this, fill);
    // The services come from a worker (cached per type, never on the GUI thread); asking
    // when the File menu opens makes them ready by the time the submenu is shown.
    connect(file, &QMenu::aboutToShow, this, [this] {
        QString reason;
        const QString p = frontFile(&reason);
        if (!p.isEmpty())
            ShareServiceCache::instance().request(p);
    });
    // An entry from the start so the native menu shows the submenu arrow; the services are
    // looked up when it opens.
    populateShareMenu(m_share, {}, [this] { showSharePickerForFrontWindow(); });
}

bool MenuExtras::showSharePickerForFrontWindow() {
    ViewerWindow* w = m_app.frontWindow();
    if (!w || w->currentPath().isEmpty())
        return false;
    QString reason;
    const QString file = frontFile(&reason);
    if (file.isEmpty()) {
        if (!reason.isEmpty())
            w->showMessage(reason, 3500); // F-OPEN-10: a PDF page, a member not extracted yet
        return false;
    }
    // Anchored at the top middle of the image area.
    ImageView* view = w->view();
    const QPoint topMiddle = view->mapTo(w, QPoint(view->width() / 2, 0));
    return showSharePicker(w, QRect(topMiddle - QPoint(1, 0), QSize(2, 2)), {file});
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
    const QString tabLabels[] = {tr("Vorherigen Tab anzeigen"), tr("Nächsten Tab anzeigen"),
                                 tr("Tab in ein neues Fenster bewegen"), tr("Alle Fenster zusammenführen"),
                                 tr("Tableiste ein-/ausblenden")};
    i = 0;
    for (const auto& [id, command] : tabs) {
        QAction* act = window->addAction(tabLabels[i++]);
        const QString cid = QStringLiteral("tabs:") + QLatin1String(id);
        act->setObjectName(QStringLiteral("command:") + cid);
        m_register(cid, act);
        m_tabActions << act;
#ifdef LF_GENERIC_PLATFORM
        act->setVisible(false); // native window tabs are a macOS feature (docs/LINUX.md)
#endif
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
