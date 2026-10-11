// SPDX-License-Identifier: GPL-3.0-or-later
#include "NavigationExtras.h"

#include "Application.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/Settings.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace lf {

NavigationExtras::NavigationExtras(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfNavigationExtras"));
    connect(window.collection(), &Collection::entriesChanged, this, [this] {
        if (m_shuffle)
            updatePaths();
    });
}

void NavigationExtras::showGoToDialog() {
    const Collection* c = m_window.collection();
    if (c->count() == 0)
        return;
    if (m_dialog) {
        m_dialog->raise();
        return;
    }
    auto* dlg = new QDialog(&m_window);
    dlg->setObjectName(QStringLiteral("lfGoToDialog"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setWindowTitle(tr("Gehe zu Bild"));
    auto* v = new QVBoxLayout(dlg);
    auto* label = new QLabel(tr("Bildnummer (1–%1), oder +n / −n vom aktuellen Bild:").arg(c->count()), dlg);
    label->setObjectName(QStringLiteral("lfGoToLabel"));
    auto* edit = new QLineEdit(dlg);
    edit->setObjectName(QStringLiteral("lfGoToField"));
    edit->setPlaceholderText(QString::number(c->currentIndex() + 1));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Gehe zu"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Abbrechen"));
    v->addWidget(label);
    v->addWidget(edit);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, edit] {
        if (goTo(edit->text()))
            dlg->accept();
        else
            edit->selectAll();
    });
    m_dialog = dlg;
    dlg->open();
    edit->setFocus();
}

bool NavigationExtras::goTo(const QString& text) {
    Collection* c = m_window.collection();
    const std::optional<int> index = nav::parseGoTo(text, c->currentIndex(), c->count());
    if (!index) {
        m_window.showMessage(tr("Gib eine Bildnummer zwischen 1 und %1 ein.").arg(c->count()), 2500);
        return false;
    }
    c->setCurrentIndex(*index);
    return true;
}

void NavigationExtras::updatePaths() {
    QStringList paths;
    for (const FileEntry& e : m_window.collection()->entries())
        paths << e.path;
    m_order.setPaths(paths);
}

void NavigationExtras::setShuffle(bool on) {
    if (on == m_shuffle)
        return;
    m_shuffle = on;
    if (on)
        updatePaths();
    m_window.showMessage(on ? tr("Zufällige Reihenfolge an") : tr("Zufällige Reihenfolge aus"), 1200);
    emit shuffleChanged(on);
    m_window.refreshStatus();
    m_app.windowChanged(&m_window);
}

bool NavigationExtras::navigate(int delta) {
    if (!m_shuffle || delta == 0)
        return false;
    Collection* c = m_window.collection();
    if (c->count() == 0)
        return true;
    const bool wrap = Settings::instance().value(key::WrapMode).toString() == QLatin1String("wrap");
    bool hitEnd = false;
    const QString cur = c->currentPath();
    const QString to = m_order.step(cur, delta, wrap, &hitEnd);
    if (hitEnd && to == cur) {
        m_window.showMessage(
            delta > 0 ? tr("Ende der zufälligen Reihenfolge") : tr("Anfang der zufälligen Reihenfolge"), 900);
        return true;
    }
    c->setCurrentPath(to);
    return true;
}

bool NavigationExtras::goFirst(bool last) {
    if (!m_shuffle)
        return false;
    const QString to = last ? m_order.last() : m_order.first();
    if (!to.isEmpty())
        m_window.collection()->setCurrentPath(to);
    return true;
}

} // namespace lf
