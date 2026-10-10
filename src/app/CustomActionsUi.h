// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-10 (M4): custom actions in the app (core/CustomActions.h has the model and the
// safe process start).
// - CustomActionsManager (one per app): runs an action on the current file of a window -
//   asynchronously, with the timeout key::CustomActionTimeoutSec; the first run of an action
//   (and the first run after its command changed) asks for confirmation, showing the command
//   and the file. Results go to a toast and to the log (Ablage → Eigene Aktionen →
//   Protokoll …: a small tool window, "lfActionLog").
// - prefs::createCustomActionsEditor(): the list in the preferences (Erweitert): add, edit,
//   remove; name, shell command or shortcut name, key binding (conflicts with the app's
//   shortcuts are reported).
// The menu entries (with their keys) are built by MainMenu (MainMenuM4.cpp).

#include "core/CustomActions.h"

#include <QDialog>
#include <QList>
#include <QObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QStringList>

#include <functional>

class QWidget;

namespace lf {

class ViewerWindow;

class CustomActionsManager : public QObject {
    Q_OBJECT
public:
    static CustomActionsManager& instance();

    QList<CustomAction> actions() const { return m_actions; }
    // Runs action `id` on the current file of `window` (asks first on the first run).
    void run(const QString& id, ViewerWindow* window);
    bool isRunning() const { return !m_runs.isEmpty(); }
    // Keys without ⌘/⌃ never reach the menu (Application filters them for InputBindings): the
    // viewer window asks here first. Runs the action bound to `key`/`modifiers` on the current
    // file of `window`; false if no action has this key.
    bool runForKey(int key, Qt::KeyboardModifiers modifiers, ViewerWindow* window);
    // An action has this key without ⌘/⌃ (typing it must not open the name filter).
    bool hasKey(int key, Qt::KeyboardModifiers modifiers) const;

    void showLog(QWidget* parent);
    QStringList logLines() const { return m_log; }
    QDialog* logWindow() const { return m_logWindow; }

    // Tests: the confirmation answer instead of a dialog; the Shortcuts tool.
    void setConfirmForTesting(std::function<bool(const CustomAction&, const QStringList& files)> confirm);
    void setShortcutsToolForTesting(const QString& tool) { m_tool = tool; }

signals:
    void actionsChanged();
    void logChanged();
    void runFinished(const QString& id, bool ok);

private:
    CustomActionsManager();
    void reload();
    void start(const CustomAction& action, const QStringList& files, QPointer<ViewerWindow> window);
    void appendLog(const QString& text);

    QList<CustomAction> m_actions;
    QList<CustomActionRun*> m_runs;
    QStringList m_log;
    QPointer<QDialog> m_logWindow;
    QPointer<QPlainTextEdit> m_logView;
    std::function<bool(const CustomAction&, const QStringList&)> m_confirm;
    QString m_tool = QStringLiteral("/usr/bin/shortcuts");
};

namespace prefs {
// The list of custom actions with add/edit/remove (object "lfCustomActions").
QWidget* createCustomActionsEditor(QWidget* parent);
} // namespace prefs

} // namespace lf
