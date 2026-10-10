// SPDX-License-Identifier: GPL-3.0-or-later
// M4 part 2 menu entries (MainMenu.h): F-EDIT-03 crop (Bild), F-EDIT-04 export (Ablage),
// F-INFO-07 photo overlays and F-GRID-04 compare blending (Darstellung).

#include "Application.h"
#include "CompareView.h"
#include "MainMenu.h"
#include "OverviewController.h"
#include "ViewerWindow.h"
#include "WindowEditing.h"
#include "WindowExtras.h"
#include "core/CompareBlend.h"
#include "core/Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>

namespace lf {

namespace {

CompareView* frontCompare(Application& app) {
    ViewerWindow* w = app.frontWindow();
    OverviewController* o = w && w->extras() ? w->extras()->overview() : nullptr;
    return o && o->isCompareShown() ? o->compareView() : nullptr;
}

} // namespace

void MainMenu::buildEdit(QMenu* file, QMenu* view, QMenu* image) {
    if (file) {
        QAction* exportAct = add(file, Action::ExportImage, tr("Exportieren …")); // F-EDIT-04
        // Next to the other file commands, before "Als Standard-App festlegen …".
        if (QAction* before = command(QStringLiteral("makeDefault"))) {
            file->removeAction(exportAct);
            file->insertAction(before, exportAct);
            file->insertSeparator(before);
        }
    }
    if (image) {
        image->addSeparator();
        add(image, Action::CropImage, tr("Zuschneiden …"))->setCheckable(true); // F-EDIT-03
    }
    if (view) {
        view->addSeparator();
        QMenu* overlays = view->addMenu(tr("Overlays")); // F-INFO-07
        overlays->setObjectName(QStringLiteral("menu:overlays"));
        add(overlays, Action::ToggleClipping, tr("Über-/Unterbelichtung"))->setCheckable(true);
        add(overlays, Action::ToggleFocusPeaking, tr("Focus Peaking"))->setCheckable(true);
        add(overlays, Action::ToggleAfPoints, tr("Fokuspunkte"))->setCheckable(true);
        connect(overlays, &QMenu::aboutToShow, this, &MainMenu::refreshState);

        QMenu* compareMenu = view->addMenu(tr("Vergleich überlagern")); // F-GRID-04
        compareMenu->setObjectName(QStringLiteral("menu:compareBlend"));
        auto* group = new QActionGroup(this);
        const QList<QPair<compare::BlendMode, QString>> modes = {
            {compare::BlendMode::Off, tr("Nebeneinander")},
            {compare::BlendMode::Difference, tr("Differenz")},
            {compare::BlendMode::Blend, tr("Überblenden")},
            {compare::BlendMode::Wipe, tr("Wischen (geteilte Ansicht)")}};
        for (const auto& [mode, text] : modes) {
            QAction* act =
                addCommand(compareMenu, QStringLiteral("compare:") + compare::blendModeId(mode), text);
            act->setCheckable(true);
            group->addAction(act);
            const compare::BlendMode m = mode;
            connect(act, &QAction::triggered, this, [this, m] {
                if (CompareView* c = frontCompare(m_app))
                    c->setBlendMode(m);
                refreshState();
            });
        }
        connect(compareMenu, &QMenu::aboutToShow, this, &MainMenu::refreshState);
    }
}

void MainMenu::refreshEditState() {
    ViewerWindow* w = m_app.frontWindow();
    for (Action a : {Action::CropImage, Action::ExportImage, Action::ToggleClipping,
                     Action::ToggleFocusPeaking, Action::ToggleAfPoints})
        if (QAction* act = action(a))
            act->setEnabled(w != nullptr);
    Settings& st = Settings::instance();
    if (QAction* act = action(Action::ToggleClipping))
        act->setChecked(st.value(key::OverlayClipping).toBool());
    if (QAction* act = action(Action::ToggleFocusPeaking))
        act->setChecked(st.value(key::OverlayPeaking).toBool());
    if (QAction* act = action(Action::ToggleAfPoints))
        act->setChecked(st.value(key::OverlayAfPoints).toBool());
    if (QAction* act = action(Action::CropImage)) {
        WindowEditing* editing = w ? WindowEditing::of(w) : nullptr;
        act->setChecked(editing && editing->cropActive());
    }
    CompareView* c = frontCompare(m_app);
    for (compare::BlendMode m : {compare::BlendMode::Off, compare::BlendMode::Difference,
                                 compare::BlendMode::Blend, compare::BlendMode::Wipe}) {
        if (QAction* act = command(QStringLiteral("compare:") + compare::blendModeId(m))) {
            act->setEnabled(c && c->paneCount() >= 2);
            act->setChecked(c ? c->blendMode() == m : m == compare::BlendMode::Off);
        }
    }
}

} // namespace lf
