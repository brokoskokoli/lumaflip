// SPDX-License-Identifier: GPL-3.0-or-later
// MainMenu, M3 view entries: grid (F-GRID-02), compare (F-GRID-03), go to and shuffle
// (F-NAV-06), name filter (F-NAV-08); in the Fenster menu always on top (F-VIEW-12),
// minimal window (F-VIEW-13) and coupled windows (F-WIN-07).

#include "Application.h"
#include "CollectionExtras.h"
#include "CouplingManager.h"
#include "MainMenu.h"
#include "NavigationExtras.h"
#include "OverviewController.h"
#include "ViewerWindow.h"
#include "WindowExtras.h"
#include "core/Collection.h"

#include <QAction>
#include <QMenu>

namespace lf {

void MainMenu::buildViews(QMenu* view, QMenu* nav) {
    auto front = [this] { return m_app.frontWindow(); };

    // ---- Darstellung: grid and compare ----
    view->addSeparator();
    add(view, Action::ToggleGrid, tr("Raster"))->setCheckable(true);    // F-GRID-02
    add(view, Action::CompareWithNext, tr("Mit nächstem vergleichen")); // F-GRID-03
    connect(addCommand(view, QStringLiteral("compare:selection"), tr("Auswahl vergleichen")),
            &QAction::triggered, this, [front] {
                if (ViewerWindow* w = front(); w && w->extras())
                    w->extras()->overview()->compare(w->extras()->overview()->subjectPaths());
            });

    // ---- Darstellung: subfolders (F-OPEN-08) ----
    view->addSeparator();
    add(view, Action::ToggleSubfolders, tr("Unterordner einbeziehen"))->setCheckable(true);

    // ---- Darstellung: folder map (F-INFO-08) ----
    view->addSeparator();
    add(view, Action::ToggleFolderMap, tr("Karte"))->setCheckable(true);

    // ---- Navigation: sibling folders (F-OPEN-09) ----
    nav->addSeparator();
    add(nav, Action::NextFolder, tr("Nächster Ordner"));
    add(nav, Action::PreviousFolder, tr("Vorheriger Ordner"));

    // ---- Navigation: go to, shuffle, name filter ----
    nav->addSeparator();
    add(nav, Action::GoToImage, tr("Gehe zu Bild …"));                                // F-NAV-06
    add(nav, Action::ToggleShuffle, tr("Zufällige Reihenfolge"))->setCheckable(true); // F-NAV-06
    add(nav, Action::FilterByName, tr("Nach Dateiname filtern …"));                   // F-NAV-08
    connect(addCommand(nav, QStringLiteral("nameFilter:clear"), tr("Namensfilter aufheben")),
            &QAction::triggered, this, [front] {
                if (ViewerWindow* w = front())
                    w->collection()->setNameFilter(QString());
            });
}

void MainMenu::buildWindowModes(QMenu* window) {
    add(window, Action::ToggleAlwaysOnTop, tr("Immer im Vordergrund"))->setCheckable(true);     // F-VIEW-12
    add(window, Action::ToggleMinimalWindow, tr("Minimaler Fenstermodus"))->setCheckable(true); // F-VIEW-13
    window->addSeparator();
    add(window, Action::CoupleWindows, tr("Fenster koppeln …")); // F-WIN-07
    connect(addCommand(window, QStringLiteral("uncouple"), tr("Entkoppeln")), &QAction::triggered, this,
            [this] { CouplingManager::of(m_app).uncouple(); });
}

void MainMenu::refreshViewsState() {
    ViewerWindow* w = m_app.frontWindow();
    WindowExtras* x = w ? w->extras() : nullptr;
    for (Action a : {Action::ToggleGrid, Action::CompareWithNext, Action::GoToImage, Action::ToggleShuffle,
                     Action::FilterByName, Action::ToggleAlwaysOnTop, Action::ToggleMinimalWindow,
                     Action::ToggleSubfolders, Action::NextFolder, Action::PreviousFolder,
                     Action::ToggleRawJpeg, Action::ToggleFolderMap})
        if (QAction* act = action(a))
            act->setEnabled(x != nullptr);
    if (QAction* act = action(Action::ToggleSubfolders)) // F-OPEN-08
        act->setChecked(x && x->collectionExtras()->includesSubfolders());
    if (QAction* act = action(Action::ToggleRawJpeg)) { // F-FMT-06
        act->setEnabled(x && x->collectionExtras()->currentIsPair());
        act->setChecked(x && x->collectionExtras()->currentShowsRaw());
    }
    if (QAction* act = action(Action::ToggleGrid))
        act->setChecked(x && x->overview()->isGridShown());
    if (QAction* act = action(Action::ToggleFolderMap)) // F-INFO-08
        act->setChecked(x && x->folderMapShown());
    if (QAction* act = action(Action::ToggleShuffle))
        act->setChecked(x && x->navigation()->shuffle());
    if (QAction* act = action(Action::ToggleAlwaysOnTop))
        act->setChecked(x && x->alwaysOnTop());
    if (QAction* act = action(Action::ToggleMinimalWindow))
        act->setChecked(x && x->minimalWindow());
    command(QStringLiteral("compare:selection"))->setEnabled(x && x->overview()->canCompareSelection());
    command(QStringLiteral("nameFilter:clear"))->setEnabled(w && !w->collection()->nameFilter().isEmpty());
    CouplingManager& coupling = CouplingManager::of(m_app);
    if (QAction* act = action(Action::CoupleWindows))
        act->setEnabled(m_app.windows().size() >= 2 || !coupling.group().isEmpty());
    command(QStringLiteral("uncouple"))->setEnabled(!coupling.group().isEmpty());
}

} // namespace lf
