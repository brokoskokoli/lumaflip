// SPDX-License-Identifier: GPL-3.0-or-later
// MainMenu, M3 menu "Aussortieren": culling mode (F-CULL-06), rating (F-CULL-01), auto
// advance (F-CULL-03), filter (F-CULL-04), rejects (F-CULL-05), quick move/copy (F-FILE-08).

#include "Application.h"
#include "CullingController.h"
#include "MainMenu.h"
#include "PreferencesDialog.h"
#include "QuickMoveController.h"
#include "ViewerWindow.h"
#include "core/QuickMove.h"
#include "core/Settings.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <QMenuBar>

namespace lf {

namespace {

struct RateItem {
    const char* id;
    Action action;
};
const RateItem kRateItems[] = {
    {"rate:0", Action::Rate0},       {"rate:1", Action::Rate1},           {"rate:2", Action::Rate2},
    {"rate:3", Action::Rate3},       {"rate:4", Action::Rate4},           {"rate:5", Action::Rate5},
    {"rate:pick", Action::RatePick}, {"rate:reject", Action::RateReject}, {"rate:clear", Action::RateClear},
};

const char* const kFilterIds[] = {"all",  "picks", "min1",    "min2",   "min3",
                                  "min4", "min5",  "unrated", "rejects"};

QString filterText(const QString& id) {
    if (id == QLatin1String("all"))
        return MainMenu::tr("Alle Bilder");
    if (id == QLatin1String("picks"))
        return MainMenu::tr("Nur markierte");
    if (id == QLatin1String("unrated"))
        return MainMenu::tr("Nur unbewertete");
    if (id == QLatin1String("rejects"))
        return MainMenu::tr("Nur abgelehnte");
    return MainMenu::tr("Mindestens %1 ★").arg(id.mid(3));
}

} // namespace

void MainMenu::buildCulling() {
    auto front = [this] { return m_app.frontWindow(); };
    auto culling = [front]() -> CullingController* {
        ViewerWindow* w = front();
        return w ? w->culling() : nullptr;
    };

    QMenu* menu = m_bar->addMenu(tr("Aussortieren"));
    menu->setObjectName(QStringLiteral("menu:culling"));
    add(menu, Action::ToggleCullingMode, tr("Aussortiermodus"))->setCheckable(true);
    menu->addSeparator();

    // ---- rating (no key equivalents here: the digits are resolved by the window, 5.2) ----
    QMenu* rate = menu->addMenu(tr("Bewerten"));
    rate->setObjectName(QStringLiteral("menu:rate"));
    for (const RateItem& item : kRateItems) {
        if (item.action == Action::RatePick)
            rate->addSeparator();
        QAction* act = addCommand(rate, QString::fromLatin1(item.id), actionLabel(item.action));
        const Action a = item.action;
        connect(act, &QAction::triggered, this, [this, a] { m_app.perform(a); });
    }
    QAction* advance =
        addCommand(menu, QStringLiteral("culling:autoAdvance"), tr("Nach Bewertung automatisch weiter"));
    advance->setCheckable(true);
    connect(advance, &QAction::triggered, this,
            [](bool on) { Settings::instance().setValue(key::CullAutoAdvance, on); });

    // ---- filter ----
    QMenu* filter = menu->addMenu(tr("Filter"));
    filter->setObjectName(QStringLiteral("menu:ratingFilter"));
    auto* group = new QActionGroup(this);
    for (const char* raw : kFilterIds) {
        const QString id = QString::fromLatin1(raw);
        QAction* act = addCommand(filter, QStringLiteral("filter:") + id, filterText(id));
        act->setCheckable(true);
        group->addAction(act);
        if (id == QLatin1String("all") || id == QLatin1String("min5"))
            filter->addSeparator();
        connect(act, &QAction::triggered, this, [this, culling, id] {
            if (CullingController* c = culling())
                c->setFilter(RatingFilter::fromId(id));
            refreshState();
        });
    }
    connect(filter, &QMenu::aboutToShow, this, &MainMenu::refreshState);

    // ---- rejects (F-CULL-05) ----
    menu->addSeparator();
    connect(addCommand(menu, QStringLiteral("culling:trashRejects"),
                       tr("Alle abgelehnten in den Papierkorb legen …")),
            &QAction::triggered, this, [culling] {
                if (CullingController* c = culling())
                    c->bulkRejects(true);
            });
    connect(addCommand(menu, QStringLiteral("culling:moveRejects"),
                       tr("Alle abgelehnten nach „_Rejected“ verschieben …")),
            &QAction::triggered, this, [culling] {
                if (CullingController* c = culling())
                    c->bulkRejects(false);
            });

    // ---- quick move/copy (F-FILE-08) ----
    menu->addSeparator();
    add(menu, Action::QuickMove, tr("Schnellablage: verschieben (M, dann 1–9)"));
    add(menu, Action::QuickCopy, tr("Schnellablage: kopieren (C, dann 1–9)"));
    for (const bool move : {true, false}) {
        QMenu* sub = menu->addMenu(move ? tr("Verschieben nach") : tr("Kopieren nach"));
        sub->setObjectName(move ? QStringLiteral("menu:quickMove") : QStringLiteral("menu:quickCopy"));
        for (int n = 1; n <= quickmove::kSlots; ++n) {
            QAction* act = addCommand(sub,
                                      (move ? QStringLiteral("quickMove:") : QStringLiteral("quickCopy:")) +
                                          QString::number(n),
                                      quickmove::label(n, {}));
            connect(act, &QAction::triggered, this, [culling, move, n] {
                if (CullingController* c = culling())
                    c->quickMove()->transferCurrent(move ? QuickOp::Move : QuickOp::Copy, n);
            });
        }
        connect(sub, &QMenu::aboutToShow, this, [this, sub] {
            for (QAction* act : sub->actions()) {
                const qsizetype colon = act->objectName().lastIndexOf(QLatin1Char(':'));
                bool ok = false;
                const int n = act->objectName().mid(colon + 1).toInt(&ok);
                if (!ok)
                    continue;
                const QString target = quickmove::target(n);
                act->setText(quickmove::label(n, target));
                act->setEnabled(!target.isEmpty() && m_app.frontWindow());
            }
        });
    }
    connect(
        addCommand(menu, QStringLiteral("culling:quickTargets"), tr("Ziele der Schnellablage festlegen …")),
        &QAction::triggered, this, [] {
            if (PreferencesDialog* p = PreferencesDialog::showPreferences())
                p->showSection(PreferencesDialog::Rating);
        });
    connect(menu, &QMenu::aboutToShow, this, &MainMenu::refreshState);
}

void MainMenu::refreshCullingState() {
    ViewerWindow* w = m_app.frontWindow();
    CullingController* c = w ? w->culling() : nullptr;
    for (Action a : {Action::ToggleCullingMode, Action::QuickMove, Action::QuickCopy})
        if (QAction* act = action(a))
            act->setEnabled(c != nullptr);
    if (QAction* act = action(Action::ToggleCullingMode))
        act->setChecked(c && c->isActive());
    for (const RateItem& item : kRateItems)
        if (QAction* act = command(QString::fromLatin1(item.id)))
            act->setEnabled(c != nullptr);
    command(QStringLiteral("culling:autoAdvance"))
        ->setChecked(Settings::instance().value(key::CullAutoAdvance).toBool());
    const QString filterId = c ? c->filter().id() : QStringLiteral("all");
    for (const char* raw : kFilterIds) {
        QAction* act = command(QStringLiteral("filter:") + QString::fromLatin1(raw));
        act->setEnabled(c != nullptr);
        act->setChecked(filterId == QLatin1String(raw));
    }
    for (const char* id : {"culling:trashRejects", "culling:moveRejects"})
        command(QString::fromLatin1(id))->setEnabled(c != nullptr);
}

} // namespace lf
