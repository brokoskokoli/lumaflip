// SPDX-License-Identifier: GPL-3.0-or-later
#include "CustomActionsUi.h"

#include "Application.h"
#include "ViewerWindow.h"
#include "core/ContainerPolicy.h"
#include "core/LocaleFormat.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace lf {

namespace {

constexpr int kMaxLogLines = 400;

QString kindText(CustomAction::Kind k) {
    return k == CustomAction::Kind::Shortcut
               ? QCoreApplication::translate("lf::CustomActionsManager", "Kurzbefehl")
               : QCoreApplication::translate("lf::CustomActionsManager", "Shell-Befehl");
}

} // namespace

CustomActionsManager& CustomActionsManager::instance() {
    static CustomActionsManager* m = new CustomActionsManager; // lives as long as the app
    return *m;
}

CustomActionsManager::CustomActionsManager() {
    setObjectName(QStringLiteral("lfCustomActionsManager"));
    reload();
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString& k) {
        if (k == QLatin1String(key::CustomActions))
            reload();
    });
}

void CustomActionsManager::reload() {
    const QList<CustomAction> fresh = customactions::load();
    if (fresh == m_actions)
        return;
    m_actions = fresh;
    emit actionsChanged();
}

void CustomActionsManager::setConfirmForTesting(
    std::function<bool(const CustomAction&, const QStringList&)> c) {
    m_confirm = std::move(c);
}

bool CustomActionsManager::runForKey(int key, Qt::KeyboardModifiers modifiers, ViewerWindow* window) {
    if (!window || (modifiers & (Qt::ControlModifier | Qt::MetaModifier)))
        return false; // ⌘/⌃: the menu entry's shortcut (MainMenu::rebuildCustomActions)
    for (const CustomAction& a : std::as_const(m_actions))
        if (customactions::matchesKey(a, key, modifiers)) {
            run(a.id, window);
            return true;
        }
    return false;
}

void CustomActionsManager::run(const QString& id, ViewerWindow* window) {
    const auto it =
        std::find_if(m_actions.cbegin(), m_actions.cend(), [&](const CustomAction& a) { return a.id == id; });
    if (it == m_actions.cend() || !window)
        return;
    const CustomAction action = *it;
    const QString cur = window->currentPath();
    if (cur.isEmpty()) {
        window->showMessage(tr("Es ist kein Bild geöffnet."));
        return;
    }
    if (vpath::isMember(cur)) {
        window->showMessage(containers::blockedMessage(), 3500);
        return;
    }
    if (const QString problem = customactions::validate(action); !problem.isEmpty()) {
        window->showMessage(problem, 3500);
        return;
    }
    const QStringList files{cur};
    QPointer<ViewerWindow> w(window);
    if (action.confirmed) {
        start(action, files, w);
        return;
    }
    // First run: show what will run with which file.
    // Confirms exactly the command shown: if the action was edited meanwhile (preferences,
    // import), the stored one stays unconfirmed; what runs is always the shown command.
    auto accept = [this, action, files, w] {
        QList<CustomAction> all = customactions::load();
        bool changed = false;
        for (CustomAction& a : all)
            if (a.id == action.id && a.kind == action.kind && a.command == action.command && !a.confirmed) {
                a.confirmed = true;
                changed = true;
            }
        if (changed)
            customactions::save(all);
        start(action, files, w);
    };
    if (m_confirm) {
        if (m_confirm(action, files))
            accept();
        return;
    }
    auto* box =
        new QMessageBox(QMessageBox::Question, tr("Eigene Aktion ausführen"),
                        tr("„%1“ zum ersten Mal ausführen?").arg(action.name), QMessageBox::NoButton, window);
    box->setObjectName(QStringLiteral("lfConfirmCustomAction"));
    const QString what = action.kind == CustomAction::Kind::Shortcut
                             ? tr("Kurzbefehl: %1").arg(action.command)
                             : tr("Befehl: %1").arg(action.command);
    box->setInformativeText(what + QLatin1Char('\n') + tr("Datei: %1").arg(cur));
    QPushButton* yes = box->addButton(tr("Ausführen"), QMessageBox::AcceptRole);
    box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
    box->setDefaultButton(yes);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::WindowModal);
    connect(box, &QMessageBox::buttonClicked, this, [yes, accept](QAbstractButton* b) {
        if (b == yes)
            accept();
    });
    box->open();
}

void CustomActionsManager::start(const CustomAction& action, const QStringList& files,
                                 QPointer<ViewerWindow> window) {
    const int timeoutMs = Settings::instance().value(key::CustomActionTimeoutSec).toInt() * 1000;
    auto* run = new CustomActionRun(action, files, timeoutMs, this, m_tool);
    m_runs << run;
    appendLog(tr("▶ %1: %2").arg(action.name, files.join(QStringLiteral(", "))));
    if (window)
        window->showMessage(tr("„%1“ läuft …").arg(action.name));
    connect(run, &CustomActionRun::finished, this, [this, run, window](const CustomActionRun::Result& r) {
        m_runs.removeAll(run);
        const QString name = run->action().name;
        if (!r.output.trimmed().isEmpty())
            appendLog(r.output.trimmed());
        appendLog(r.ok() ? tr("✓ %1: fertig").arg(name) : tr("✗ %1: %2").arg(name, r.error));
        if (window)
            window->showMessage(r.ok() ? tr("„%1“ ausgeführt").arg(name) : tr("„%1“: %2").arg(name, r.error),
                                r.ok() ? 1500 : 5000);
        emit runFinished(run->action().id, r.ok());
        run->deleteLater();
    });
    run->start();
}

void CustomActionsManager::appendLog(const QString& text) {
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    for (const QString& line : text.split(QLatin1Char('\n')))
        m_log << stamp + QLatin1Char(' ') + line;
    while (m_log.size() > kMaxLogLines)
        m_log.removeFirst();
    if (m_logView) {
        m_logView->setPlainText(m_log.join(QLatin1Char('\n')));
        m_logView->moveCursor(QTextCursor::End);
    }
    emit logChanged();
}

void CustomActionsManager::showLog(QWidget* parent) {
    if (!m_logWindow) {
        auto* dlg = new QDialog(parent, Qt::Tool);
        dlg->setObjectName(QStringLiteral("lfActionLog"));
        dlg->setWindowTitle(tr("Protokoll der Aktionen"));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        auto* v = new QVBoxLayout(dlg);
        auto* view = new QPlainTextEdit(dlg);
        view->setObjectName(QStringLiteral("lfActionLogText"));
        view->setReadOnly(true);
        view->setPlaceholderText(tr("Noch keine Aktion ausgeführt."));
        QFont mono = view->font();
        mono.setFamily(QStringLiteral("Menlo"));
        view->setFont(mono);
        v->addWidget(view);
        auto* clear = new QPushButton(tr("Leeren"), dlg);
        clear->setAutoDefault(false);
        connect(clear, &QPushButton::clicked, this, [this] {
            m_log.clear();
            if (m_logView)
                m_logView->clear();
            emit logChanged();
        });
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(clear);
        v->addLayout(row);
        dlg->resize(520, 300);
        m_logWindow = dlg;
        m_logView = view;
    }
    m_logView->setPlainText(m_log.join(QLatin1Char('\n')));
    m_logView->moveCursor(QTextCursor::End);
    m_logWindow->show();
    m_logWindow->raise();
}

// ---- preferences ----

namespace prefs {

namespace {

struct Tr {
    Q_DECLARE_TR_FUNCTIONS(lf::CustomActionsEditor)
};

// Actions of the app that `seq` already triggers in a viewer window (InputBindings: menu
// shortcuts and slot keys) and other custom actions with the same key (labels).
QStringList shortcutConflicts(const QKeySequence& seq, const QString& ownId) {
    if (seq.isEmpty())
        return {};
    QStringList out = customactions::bindingConflicts(seq, InputBindings());
    for (const CustomAction& c : customactions::load())
        if (c.id != ownId && c.keys == seq)
            out << c.name;
    return out;
}

QString conflictText(const QKeySequence& seq, const QStringList& conflicts) {
    return Tr::tr("%1 ist schon belegt (%2); im Bildfenster hat die eigene Aktion Vorrang.")
        .arg(seq.toString(QKeySequence::NativeText), conflicts.join(QStringLiteral(", ")));
}

// Edits `action` in a modal dialog; false if cancelled.
bool editAction(QWidget* parent, CustomAction& action) {
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("lfEditCustomAction"));
    dlg.setWindowTitle(Tr::tr("Eigene Aktion"));
    auto* form = new QFormLayout(&dlg);
    auto* name = new QLineEdit(action.name, &dlg);
    name->setObjectName(QStringLiteral("name"));
    auto* kind = new QComboBox(&dlg);
    kind->setObjectName(QStringLiteral("kind"));
    kind->addItem(kindText(CustomAction::Kind::Shell));
#ifdef Q_OS_MACOS
    const bool shortcutsApp = true;
#else
    const bool shortcutsApp = false; // the Shortcuts app is macOS only
#endif
    // Elsewhere only an imported shortcut action shows the type (validate() refuses it).
    if (shortcutsApp || action.kind == CustomAction::Kind::Shortcut)
        kind->addItem(kindText(CustomAction::Kind::Shortcut));
    kind->setCurrentIndex(action.kind == CustomAction::Kind::Shortcut ? 1 : 0);
    auto* command = new QLineEdit(action.command, &dlg);
    command->setObjectName(QStringLiteral("command"));
    command->setMinimumWidth(360);
    auto* help = new QLabel(&dlg);
    help->setWordWrap(true);
    auto updateHelp = [help, kind] {
        if (kind->currentIndex() == 1)
            help->setText(Tr::tr("Name eines Kurzbefehls aus der Kurzbefehle-App; er bekommt die Datei als "
                                 "Eingabe."));
        else
#ifdef Q_OS_WIN
            help->setText(Tr::tr("Programm mit Argumenten, z. B. \"C:\\Tools\\tool.exe\" --flag; die Datei "
                                 "wird als eigenes Argument angehängt und nie über cmd.exe gestartet. "
                                 "Batch-Dateien (.bat, .cmd) sind nicht erlaubt."));
#else
            help->setText(
                Tr::tr("Läuft mit /bin/sh; die Datei steht in \"$1\" (alle Dateien: \"$@\"), "
                       "z. B. open -a Preview \"$1\". Der Pfad wird nie in den Befehl eingesetzt."));
#endif
    };
    updateHelp();
    QObject::connect(kind, &QComboBox::currentIndexChanged, &dlg, updateHelp);
    auto* keys = new QKeySequenceEdit(action.keys, &dlg);
    keys->setObjectName(QStringLiteral("keys"));
    keys->setMaximumSequenceLength(1);
    form->addRow(Tr::tr("Name:"), name);
    if (kind->count() > 1)
        form->addRow(Tr::tr("Art:"), kind);
    else
        kind->hide();
    form->addRow(Tr::tr("Befehl:"), command);
    form->addRow(QString(), help);
    form->addRow(Tr::tr("Taste:"), keys);
    auto* keyWarning = new QLabel(&dlg);
    keyWarning->setObjectName(QStringLiteral("keyConflict"));
    keyWarning->setStyleSheet(QStringLiteral("color: #d03030;"));
    keyWarning->setWordWrap(true);
    auto updateKeyWarning = [keyWarning, keys, id = action.id] {
        const QStringList c = shortcutConflicts(keys->keySequence(), id);
        keyWarning->setText(c.isEmpty() ? QString() : conflictText(keys->keySequence(), c));
        keyWarning->setVisible(!c.isEmpty());
    };
    updateKeyWarning();
    QObject::connect(keys, &QKeySequenceEdit::keySequenceChanged, &dlg, updateKeyWarning);
    form->addRow(QString(), keyWarning);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        CustomAction probe = action;
        probe.name = name->text().trimmed();
        probe.kind = kind->currentIndex() == 1 ? CustomAction::Kind::Shortcut : CustomAction::Kind::Shell;
        probe.command = command->text();
        if (const QString problem = customactions::validate(probe); !problem.isEmpty()) {
            QMessageBox::warning(&dlg, Tr::tr("Eigene Aktion"), problem);
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    const CustomAction before = action;
    action.name = name->text().trimmed();
    action.kind = kind->currentIndex() == 1 ? CustomAction::Kind::Shortcut : CustomAction::Kind::Shell;
    action.command = command->text();
    action.keys = keys->keySequence();
    // A changed command must be confirmed again before it runs.
    if (action.command != before.command || action.kind != before.kind)
        action.confirmed = false;
    return true;
}

} // namespace

QWidget* createCustomActionsEditor(QWidget* parent) {
    auto* box = new QWidget(parent);
    box->setObjectName(QStringLiteral("lfCustomActions"));
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    auto* table = new QTableWidget(0, 4, box);
    table->setObjectName(QStringLiteral("lfCustomActionsTable"));
    table->setHorizontalHeaderLabels({Tr::tr("Name"), Tr::tr("Art"), Tr::tr("Befehl"), Tr::tr("Taste")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setMinimumHeight(140);
    auto* warning = new QLabel(box);
    warning->setObjectName(QStringLiteral("lfCustomActionsConflict"));
    warning->setStyleSheet(QStringLiteral("color: #d03030;"));
    warning->setWordWrap(true);
    auto fill = [table, warning] {
        const QList<CustomAction> all = customactions::load();
        table->setRowCount(int(all.size()));
        QStringList conflicts;
        for (int r = 0; r < all.size(); ++r) {
            const CustomAction& a = all[r];
            table->setItem(r, 0, new QTableWidgetItem(a.name));
            table->setItem(r, 1, new QTableWidgetItem(kindText(a.kind)));
            table->setItem(r, 2, new QTableWidgetItem(a.command));
            table->setItem(r, 3, new QTableWidgetItem(a.keys.toString(QKeySequence::NativeText)));
            table->item(r, 0)->setData(Qt::UserRole, a.id);
            if (const QStringList c = shortcutConflicts(a.keys, a.id); !c.isEmpty())
                conflicts << Tr::tr("„%1“: %2").arg(a.name, conflictText(a.keys, c));
        }
        warning->setText(conflicts.join(QLatin1Char('\n')));
        warning->setVisible(!conflicts.isEmpty());
    };
    fill();
    QObject::connect(&Settings::instance(), &Settings::changed, box, [fill](const QString& k) {
        if (k == QLatin1String(key::CustomActions) || k.startsWith(QLatin1String(key::ShortcutPrefix)))
            fill();
    });
    v->addWidget(table);
    v->addWidget(warning);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(Tr::tr("Hinzufügen …"), box);
    add->setObjectName(QStringLiteral("lfCustomActionAdd"));
    auto* edit = new QPushButton(Tr::tr("Bearbeiten …"), box);
    edit->setObjectName(QStringLiteral("lfCustomActionEdit"));
    auto* remove = new QPushButton(Tr::tr("Entfernen"), box);
    remove->setObjectName(QStringLiteral("lfCustomActionRemove"));
    for (QPushButton* b : {add, edit, remove}) {
        b->setAutoDefault(false);
        row->addWidget(b);
    }
    row->addStretch(1);
    v->addLayout(row);
    QObject::connect(add, &QPushButton::clicked, box, [box] {
        QList<CustomAction> all = customactions::load();
        if (all.size() >= customactions::kMaxActions)
            return;
        CustomAction a;
        a.id = customactions::newId();
        if (!editAction(box, a))
            return;
        all << a;
        customactions::save(all);
    });
    auto editRow = [box, table] {
        const int r = table->currentRow();
        if (r < 0 || !table->item(r, 0))
            return;
        const QString id = table->item(r, 0)->data(Qt::UserRole).toString();
        QList<CustomAction> all = customactions::load();
        for (CustomAction& a : all)
            if (a.id == id) {
                if (editAction(box, a))
                    customactions::save(all);
                return;
            }
    };
    QObject::connect(edit, &QPushButton::clicked, box, editRow);
    QObject::connect(table, &QTableWidget::cellDoubleClicked, box, editRow);
    QObject::connect(remove, &QPushButton::clicked, box, [table] {
        const int r = table->currentRow();
        if (r < 0 || !table->item(r, 0))
            return;
        const QString id = table->item(r, 0)->data(Qt::UserRole).toString();
        QList<CustomAction> all = customactions::load();
        all.erase(std::remove_if(all.begin(), all.end(), [&](const CustomAction& a) { return a.id == id; }),
                  all.end());
        customactions::save(all);
    });
    return box;
}

} // namespace prefs

} // namespace lf
