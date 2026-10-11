// SPDX-License-Identifier: GPL-3.0-or-later
#include "QuickFilterBar.h"

#include "Application.h"
#include "CustomActionsUi.h"
#include "ViewerWindow.h"
#include "core/Collection.h"
#include "core/InputBindings.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>

namespace lf {

QuickFilterBar::QuickFilterBar(ViewerWindow& window, Application& app, QWidget* parent)
    : QFrame(parent), m_window(window), m_app(app) {
    setObjectName(QStringLiteral("lfQuickFilter"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral("#lfQuickFilter { background: rgba(28, 28, 28, 230); border-radius: 8px; "
                                 "border: 1px solid rgba(255, 255, 255, 50); }"
                                 "#lfQuickFilter QLabel { color: #d8d8d8; }"
                                 "#lfQuickFilter QToolButton { color: #d8d8d8; border: none; }"));
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(8, 4, 4, 4);
    h->setSpacing(6);
    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("lfQuickFilterField"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setMinimumWidth(200);
    m_edit->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_edit->installEventFilter(this);
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("lfQuickFilterCount"));
    auto* closeButton = new QToolButton(this);
    closeButton->setObjectName(QStringLiteral("lfQuickFilterClose"));
    closeButton->setText(QStringLiteral("✕"));
    closeButton->setFocusPolicy(Qt::NoFocus);
    h->addWidget(m_edit, 1);
    h->addWidget(m_count);
    h->addWidget(closeButton);
    connect(closeButton, &QToolButton::clicked, this, &QuickFilterBar::clearAndClose);
    // Live (F-NAV-08), but not per keystroke: typing "urlaub" filters once (code review M3
    // #17); Enter, Esc and closing apply at once.
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &QuickFilterBar::applyFilter);
    connect(m_edit, &QLineEdit::textChanged, this, [this] { m_debounce->start(); });
    connect(m_window.collection(), &Collection::entriesChanged, this, &QuickFilterBar::updateCount);
    retranslateUi();
    hide();
}

void QuickFilterBar::retranslateUi() {
    m_edit->setPlaceholderText(tr("Dateiname filtern"));
    if (auto* closeButton = findChild<QToolButton*>(QStringLiteral("lfQuickFilterClose")))
        closeButton->setToolTip(tr("Filter aufheben (Esc)"));
    if (isVisible())
        place(); // the width follows the placeholder
}

void QuickFilterBar::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
        retranslateUi(); // F-I18N-03
}

QString QuickFilterBar::text() const {
    return m_edit->text();
}

void QuickFilterBar::applyFilter() {
    m_debounce->stop();
    if (m_window.collection()->nameFilter() != m_edit->text())
        m_window.collection()->setNameFilter(m_edit->text());
    updateCount();
}

void QuickFilterBar::updateCount() {
    const Collection* c = m_window.collection();
    m_count->setText(c->nameFilter().isEmpty()
                         ? QString()
                         : QStringLiteral("%1 / %2").arg(c->count()).arg(c->totalCount()));
    m_count->setVisible(!m_count->text().isEmpty());
    m_window.refreshStatus();
}

void QuickFilterBar::place() {
    QWidget* p = parentWidget();
    if (!p)
        return;
    adjustSize();
    const int w = std::min(std::max(sizeHint().width(), 280), std::max(120, p->width() - 24));
    resize(w, sizeHint().height());
    move(p->width() - w - 12, 12);
}

void QuickFilterBar::open(const QString& initialText) {
    if (m_edit->text() != m_window.collection()->nameFilter())
        m_edit->setText(m_window.collection()->nameFilter());
    show();
    place();
    raise();
    m_edit->setFocus(Qt::ShortcutFocusReason);
    if (!initialText.isEmpty())
        m_edit->insert(initialText);
    else
        m_edit->selectAll();
    updateCount();
}

void QuickFilterBar::closeField() {
    if (m_debounce->isActive())
        applyFilter();
    const bool hadFocus = m_edit->hasFocus();
    hide();
    if (hadFocus)
        m_window.focusContent();
}

void QuickFilterBar::clearAndClose() {
    m_edit->clear();
    m_debounce->stop();
    m_window.collection()->setNameFilter(QString());
    closeField();
    m_window.focusContent();
}

bool QuickFilterBar::maybeStart(const QKeyEvent* event, bool ratingKeysActive) {
    const QString t = event->text();
    if (t.isEmpty() || !t.at(0).isPrint() || t.at(0).isSpace())
        return false;
    if (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))
        return false; // command keys
    InputEvent ev;
    ev.kind = InputKind::Key;
    ev.key = event->key();
    ev.modifiers = event->modifiers();
    ev.inCullingMode = ratingKeysActive;
    if (m_app.bindings().resolve(ev) != Action::None)
        return false; // the key has a meaning (shortcut or input slot)
    if (CustomActionsManager::instance().hasKey(ev.key, ev.modifiers))
        return false; // F-FILE-10: a custom action's key (⇧K …)
    open(t);
    return true;
}

bool QuickFilterBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_edit && event->type() == QEvent::ShortcutOverride) {
        // Typing goes into the field (letters are image shortcuts elsewhere).
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!(ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) || ke->key() == Qt::Key_A) {
            event->accept();
            return true;
        }
    }
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            clearAndClose();
            return true;
        }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter || ke->key() == Qt::Key_Down) {
            closeField(); // keep the filter
            m_window.focusContent();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}

} // namespace lf
