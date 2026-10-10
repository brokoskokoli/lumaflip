// SPDX-License-Identifier: GPL-3.0-or-later
// MainMenu, M3 submenu "Darstellung → HDR": HDR toggle (F-COL-04) and exposure (F-COL-05).

#include "Application.h"
#include "HdrController.h"
#include "ImageView.h"
#include "MainMenu.h"
#include "ViewerWindow.h"
#include "core/Settings.h"

#include <QAction>
#include <QMenu>

namespace lf {

void MainMenu::buildHdr(QMenu* view) {
    view->addSeparator();
    QMenu* menu = view->addMenu(tr("HDR"));
    menu->setObjectName(QStringLiteral("menu:hdr"));
    add(menu, Action::ToggleHdr, tr("HDR anzeigen"))->setCheckable(true);
    menu->addSeparator();
    add(menu, Action::ExposureIncrease, tr("Belichtung erhöhen"));
    add(menu, Action::ExposureDecrease, tr("Belichtung verringern"));
    add(menu, Action::ExposureReset, tr("Belichtung zurücksetzen"));
    QAction* lock =
        addCommand(menu, QStringLiteral("hdr:lockExposure"), tr("Belichtung beim Bildwechsel beibehalten"));
    lock->setCheckable(true);
    connect(lock, &QAction::triggered, this, [this](bool on) {
        if (ViewerWindow* w = m_app.frontWindow())
            w->view()->setExposureLocked(on);
    });
    add(menu, Action::ToggleExposureHud, tr("Belichtungsregler einblenden"))->setCheckable(true);
    connect(menu, &QMenu::aboutToShow, this, &MainMenu::refreshState);
}

void MainMenu::refreshHdrState() {
    ViewerWindow* w = m_app.frontWindow();
    const bool hasWindow = w != nullptr;
    QAction* toggle = action(Action::ToggleHdr);
    toggle->setChecked(Settings::instance().value(key::HdrEnabled).toBool());
    for (Action a : {Action::ExposureIncrease, Action::ExposureDecrease, Action::ExposureReset})
        action(a)->setEnabled(hasWindow); // (does nothing for SDR images, with a hint)
    QAction* lock = command(QStringLiteral("hdr:lockExposure"));
    lock->setEnabled(hasWindow);
    lock->setChecked(w && w->view()->exposureLocked());
    QAction* hud = action(Action::ToggleExposureHud);
    hud->setEnabled(hasWindow);
    hud->setChecked(w && w->hdr() && w->hdr()->hudRequested());
}

} // namespace lf
