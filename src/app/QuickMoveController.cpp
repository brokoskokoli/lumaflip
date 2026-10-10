// SPDX-License-Identifier: GPL-3.0-or-later
// F-FILE-08 quick move/copy and F-FILE-11 name conflicts, see QuickMoveController.h.

#include "QuickMoveController.h"

#include "Application.h"
#include "ViewerWindow.h"
#include "core/FileOps.h"
#include "core/InputBindings.h"

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

namespace lf {

namespace {
constexpr int kChordTimeoutMs = 2500;
} // namespace

QuickMoveController::QuickMoveController(ViewerWindow& window, Application& app)
    : QObject(&window), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfQuickMoveController"));
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(kChordTimeoutMs);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (m_chord.isPending()) {
            m_chord.cancel();
            m_window.hideMessage();
        }
    });
}

QuickMoveController::~QuickMoveController() = default;

void QuickMoveController::start(QuickOp op) {
    if (m_window.currentPath().isEmpty())
        return;
    m_chord.start(op);
    m_timeout->start();
    m_window.showMessage(op == QuickOp::Move ? tr("Verschieben nach … (1–9, M = wie zuletzt)")
                                             : tr("Kopieren nach … (1–9, C = wie zuletzt)"),
                         kChordTimeoutMs);
}

bool QuickMoveController::interceptKey(const InputEvent& ev) {
    if (!m_chord.isPending() || ev.kind != InputKind::Key)
        return false;
    const QuickChord::Result r = m_chord.feed(ev.key, ev.modifiers);
    switch (r.kind) {
    case QuickChord::Kind::NotHandled:
        return false;
    case QuickChord::Kind::Pending:
        return true;
    case QuickChord::Kind::Execute:
        m_timeout->stop();
        transferCurrent(r.op, r.slot);
        return true;
    case QuickChord::Kind::Cancelled:
        m_timeout->stop();
        if (r.noLastTarget)
            m_window.showMessage(tr("Noch kein Ziel benutzt – erst eine Ziffer 1–9 wählen."), 3000);
        else
            m_window.hideMessage();
        return !r.passThrough;
    }
    return false;
}

void QuickMoveController::transferCurrent(QuickOp op, int slot) {
    const QString folder = quickmove::target(slot);
    if (folder.isEmpty()) {
        m_window.showMessage(
            tr("Ziel %1 ist nicht festgelegt (Einstellungen → Bewertung → Schnellablage).").arg(slot), 4000);
        return;
    }
    QStringList paths = m_subjects ? m_subjects() : QStringList();
    if (paths.isEmpty() && !m_window.currentPath().isEmpty())
        paths << m_window.currentPath();
    if (paths.isEmpty())
        return;
    paths = m_window.collection()->withPairs(paths); // F-FMT-06: RAW+JPEG pairs move together
    QuickChord::setLastSlot(slot);
    const QString name = QDir(folder).dirName();
    const bool move = op == QuickOp::Move;
    QVector<TransferItem> items;
    for (const QString& p : std::as_const(paths))
        items.append({p, folder, ConflictChoice::Skip});
    const bool several = paths.size() > 1;
    transfer(move ? TransferKind::Move : TransferKind::Copy, items, move ? tr("Verschieben") : tr("Kopieren"),
             [move, name, several](int done) {
                 if (done == 0)
                     return tr("Übersprungen – nichts geändert.");
                 if (several)
                     return move ? tr("%n Bild(er) nach „%1“ verschoben", nullptr, done).arg(name)
                                 : tr("%n Bild(er) nach „%1“ kopiert", nullptr, done).arg(name);
                 return move ? tr("Nach „%1“ verschoben").arg(name) : tr("Nach „%1“ kopiert").arg(name);
             });
}

void QuickMoveController::transfer(TransferKind kind, QVector<TransferItem> items, const QString& undoText,
                                   std::function<QString(int)> doneMessage) {
    if (items.isEmpty())
        return;
    // Which targets are taken is looked up on a worker (network volumes).
    QPointer<QuickMoveController> self(this);
    auto* w = new QFutureWatcher<QStringList>(this);
    connect(w, &QFutureWatcher<QStringList>::finished, this, [self, w, kind, items, undoText, doneMessage] {
        const QStringList conflicts = w->result();
        w->deleteLater();
        if (self)
            self->runWithChoices(kind, items, conflicts, 0, false, ConflictChoice::Skip, undoText,
                                 doneMessage);
    });
    w->setFuture(QtConcurrent::run([items, kind] { return filebatch::conflicts(items, kind); }));
}

void QuickMoveController::runWithChoices(TransferKind kind, QVector<TransferItem> items,
                                         QStringList conflicts, int next, bool forAll,
                                         ConflictChoice allChoice, const QString& undoText,
                                         std::function<QString(int)> doneMessage) {
    // Ask about the remaining conflicts one after the other (or apply "für alle").
    while (next < conflicts.size()) {
        const QString source = conflicts.at(next);
        auto it = std::find_if(items.begin(), items.end(),
                               [&](const TransferItem& i) { return i.source == source; });
        if (forAll) {
            if (it != items.end())
                it->onConflict = allChoice;
            ++next;
            continue;
        }
        const QString folder = it != items.end() ? it->targetFolder : QString();
        QPointer<QuickMoveController> self(this);
        askConflict(&m_window, QFileInfo(source).fileName(), folder, int(conflicts.size() - next),
                    [self, kind, items, conflicts, next, undoText,
                     doneMessage](std::optional<ConflictChoice> choice, bool all) mutable {
                        if (!self || !choice)
                            return; // cancelled: nothing happens
                        auto it2 = std::find_if(items.begin(), items.end(), [&](const TransferItem& i) {
                            return i.source == conflicts.at(next);
                        });
                        if (it2 != items.end())
                            it2->onConflict = *choice;
                        self->runWithChoices(kind, items, conflicts, next + 1, all, *choice, undoText,
                                             doneMessage);
                    });
        return;
    }
    auto batch = std::make_shared<FileBatch>(m_app.fileOps().trashService(), kind, items);
    QPointer<ViewerWindow> window(&m_window);
    runBatchAsync(
        m_app.fileOps(), undoText, batch, [window, batch, doneMessage](bool ok, const QString& error) {
            if (!window)
                return;
            if (!ok) {
                window->showMessage(error, 5000);
                return;
            }
            window->showMessage(doneMessage ? doneMessage(int(batch->steps().size())) : QString(), 2500);
        });
}

void QuickMoveController::askConflict(QWidget* parent, const QString& fileName, const QString& folder,
                                      int count,
                                      std::function<void(std::optional<ConflictChoice>, bool)> done) {
    auto* box = new QMessageBox(parent);
    box->setObjectName(QStringLiteral("lfConflictDialog"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(tr("Namenskonflikt"));
    box->setText(tr("„%1“ gibt es in „%2“ schon.").arg(fileName, QDir(folder).dirName()));
    box->setInformativeText(
        tr("Ersetzen legt die vorhandene Datei in den Papierkorb, „Beide behalten“ gibt der "
           "neuen einen Namen wie „%1“. ⌘Z macht alles rückgängig.")
            .arg(filebatch::keepBothName(folder, fileName, [&](const QString& p) {
                return QFileInfo(p).fileName() == fileName; // no disk access here
            })));
    QPushButton* replace = box->addButton(tr("Ersetzen"), QMessageBox::DestructiveRole);
    replace->setObjectName(QStringLiteral("lfConflictReplace"));
    QPushButton* keepBoth = box->addButton(tr("Beide behalten"), QMessageBox::AcceptRole);
    keepBoth->setObjectName(QStringLiteral("lfConflictKeepBoth"));
    QPushButton* skip = box->addButton(tr("Überspringen"), QMessageBox::RejectRole);
    skip->setObjectName(QStringLiteral("lfConflictSkip"));
    QPushButton* cancel = box->addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("lfConflictCancel"));
    box->setDefaultButton(keepBoth);
    box->setEscapeButton(cancel);
    QCheckBox* all = nullptr;
    if (count > 1) {
        all = new QCheckBox(tr("Für alle %1 Konflikte").arg(count), box);
        all->setObjectName(QStringLiteral("lfConflictForAll"));
        box->setCheckBox(all);
    }
    QObject::connect(box, &QMessageBox::finished, box, [box, replace, keepBoth, skip, all, done](int) {
        QAbstractButton* clicked = box->clickedButton();
        std::optional<ConflictChoice> choice;
        if (clicked == replace)
            choice = ConflictChoice::Replace;
        else if (clicked == keepBoth)
            choice = ConflictChoice::KeepBoth;
        else if (clicked == skip)
            choice = ConflictChoice::Skip;
        if (done)
            done(choice, all && all->isChecked());
    });
    box->open();
}

} // namespace lf
