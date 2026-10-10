// SPDX-License-Identifier: GPL-3.0-or-later
#include "CouplingManager.h"

#include "Application.h"
#include "ImageView.h"
#include "ViewSyncBinding.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace lf {

CouplingManager& CouplingManager::of(Application& app) {
    if (auto* m = app.findChild<CouplingManager*>(QStringLiteral("lfCouplingManager"), Qt::FindDirectChildrenOnly))
        return *m;
    return *new CouplingManager(app);
}

CouplingManager::CouplingManager(Application& app) : QObject(&app), m_app(app) {
    setObjectName(QStringLiteral("lfCouplingManager"));
    m_views = new ViewSyncBinding(this);
    connect(&app, &Application::windowsChanged, this, &CouplingManager::prune);
}

CouplingManager::~CouplingManager() = default;

CoupleBy CouplingManager::coupleBy() const {
    return coupling::fromId(Settings::instance().value(key::CoupleBy).toString());
}

QList<ViewerWindow*> CouplingManager::group() const {
    QList<ViewerWindow*> out;
    for (const QPointer<ViewerWindow>& w : m_group)
        if (w)
            out << w.data();
    return out;
}

bool CouplingManager::isCoupled(const ViewerWindow* window) const {
    for (const QPointer<ViewerWindow>& w : m_group)
        if (w == window)
            return true;
    return false;
}

bool CouplingManager::couple(const QList<ViewerWindow*>& windows) {
    QList<ViewerWindow*> unique;
    for (ViewerWindow* w : windows)
        if (w && !unique.contains(w))
            unique << w;
    if (unique.size() < 2)
        return false;
    uncouple();
    QList<ImageView*> views;
    for (ViewerWindow* w : unique) {
        m_group << QPointer<ViewerWindow>(w);
        connect(w->collection(), &Collection::currentChanged, this, [this, w] { onCurrentChanged(w); });
        views << w->view();
    }
    m_views->setViews(views);
    notifyWindows();
    for (ViewerWindow* w : unique)
        w->showMessage(tr("%n Fenster gekoppelt", nullptr, int(unique.size())), 1500);
    // The others follow the first window right away.
    onCurrentChanged(unique.first());
    emit changed();
    return true;
}

void CouplingManager::uncouple() {
    if (m_group.isEmpty())
        return;
    const QList<ViewerWindow*> old = group();
    for (ViewerWindow* w : old)
        disconnect(w->collection(), nullptr, this, nullptr);
    m_group.clear();
    m_views->setViews({});
    for (ViewerWindow* w : old) {
        w->refreshStatus();
        m_app.windowChanged(w);
    }
    emit changed();
}

void CouplingManager::prune() {
    bool gone = false;
    for (const QPointer<ViewerWindow>& w : std::as_const(m_group))
        if (!w || !m_app.windows().contains(w.data()))
            gone = true;
    if (!gone)
        return;
    QList<ViewerWindow*> rest;
    for (ViewerWindow* w : group())
        if (m_app.windows().contains(w))
            rest << w;
    if (rest.size() >= 2) {
        for (ViewerWindow* w : rest)
            disconnect(w->collection(), nullptr, this, nullptr);
        m_group.clear();
        couple(rest);
    } else {
        for (ViewerWindow* w : rest)
            disconnect(w->collection(), nullptr, this, nullptr);
        m_group.clear();
        m_views->setViews({});
        for (ViewerWindow* w : rest)
            w->refreshStatus();
        emit changed();
    }
}

void CouplingManager::notifyWindows() {
    for (ViewerWindow* w : group()) {
        w->refreshStatus();
        m_app.windowChanged(w);
    }
}

void CouplingManager::onCurrentChanged(ViewerWindow* source) {
    if (m_flipping || !isCoupled(source))
        return;
    const Collection* sc = source->collection();
    const int index = sc->currentIndex();
    const QString path = sc->currentPath();
    if (index < 0)
        return;
    const CoupleBy by = coupleBy();
    m_flipping = true;
    for (ViewerWindow* w : group()) {
        if (w == source)
            continue;
        Collection* c = w->collection();
        QStringList paths;
        paths.reserve(c->count());
        for (const FileEntry& e : c->entries())
            paths << e.path;
        const int to = coupling::followerIndex(by, index, path, paths);
        if (to < 0) {
            if (!paths.isEmpty())
                w->showMessage(tr("Kein passendes Bild zu „%1“").arg(QFileInfo(path).fileName()), 1500);
            continue;
        }
        if (to != c->currentIndex())
            c->setCurrentIndex(to);
    }
    m_flipping = false;
}

void CouplingManager::showDialog(ViewerWindow* front) {
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    const QList<ViewerWindow*> windows = m_app.windows();
    auto* dlg = new QDialog(front);
    dlg->setObjectName(QStringLiteral("lfCoupleDialog"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setWindowTitle(tr("Fenster koppeln"));
    auto* v = new QVBoxLayout(dlg);
    auto* intro = new QLabel(tr("Gekoppelte Fenster blättern gemeinsam und teilen Zoom und Ausschnitt. "
                                "Mit ⌥ lässt sich ein Fenster einzeln verschieben."),
                             dlg);
    intro->setWordWrap(true);
    v->addWidget(intro);
    QList<QCheckBox*> boxes;
    for (int i = 0; i < windows.size(); ++i) {
        ViewerWindow* w = windows.at(i);
        auto* box = new QCheckBox(w->windowTitle(), dlg);
        box->setObjectName(QStringLiteral("lfCoupleWindow%1").arg(i));
        box->setChecked(isCoupled(w) || (m_group.isEmpty() && (w == front || windows.size() == 2)));
        v->addWidget(box);
        boxes << box;
    }
    auto* form = new QFormLayout;
    auto* by = new QComboBox(dlg);
    by->setObjectName(QStringLiteral("lfCoupleBy"));
    by->addItem(tr("Dateiname (ohne Endung)"), QStringLiteral("name"));
    by->addItem(tr("Position in der Sammlung"), QStringLiteral("index"));
    by->setCurrentIndex(by->findData(coupling::toId(coupleBy())));
    form->addRow(tr("Gemeinsam blättern nach:"), by);
    v->addLayout(form);
    auto* buttons = new QDialogButtonBox(dlg);
    QPushButton* ok = buttons->addButton(tr("Koppeln"), QDialogButtonBox::AcceptRole);
    ok->setObjectName(QStringLiteral("lfCoupleOk"));
    QPushButton* un = buttons->addButton(tr("Entkoppeln"), QDialogButtonBox::DestructiveRole);
    un->setObjectName(QStringLiteral("lfUncouple"));
    un->setVisible(!m_group.isEmpty());
    buttons->addButton(tr("Abbrechen"), QDialogButtonBox::RejectRole);
    v->addWidget(buttons);
    auto updateOk = [ok, boxes] {
        int n = 0;
        for (QCheckBox* b : boxes)
            n += b->isChecked() ? 1 : 0;
        ok->setEnabled(n >= 2);
    };
    for (QCheckBox* b : boxes)
        connect(b, &QCheckBox::toggled, dlg, updateOk);
    updateOk();
    QList<QPointer<ViewerWindow>> targets;
    for (ViewerWindow* w : windows)
        targets << QPointer<ViewerWindow>(w);
    connect(ok, &QPushButton::clicked, dlg, [this, dlg, boxes, targets, by] {
        Settings::instance().setValue(key::CoupleBy, by->currentData());
        QList<ViewerWindow*> chosen;
        for (int i = 0; i < boxes.size(); ++i)
            if (boxes.at(i)->isChecked() && targets.value(i))
                chosen << targets.value(i).data();
        couple(chosen);
        dlg->accept();
    });
    connect(un, &QPushButton::clicked, dlg, [this, dlg] {
        uncouple();
        dlg->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    m_dialog = dlg;
    dlg->open();
}

} // namespace lf
