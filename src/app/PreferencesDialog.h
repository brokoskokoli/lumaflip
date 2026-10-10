// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Preferences window (SPEC 4.13 F-SET-01..03, 4.14 F-IN-17/18). Non-modal, single
// instance (opened via ⌘, by the app shell), macOS style: a toolbar selects the
// section, every change is applied immediately (no OK/Cancel).

#include "core/InputBindings.h"

#include <QDialog>
#include <QHash>
#include <QMultiHash>
#include <QStringList>

#include <functional>

class QStackedWidget;
class QTableWidget;
class QLabel;
class QActionGroup;
class QKeySequenceEdit;

namespace lf {

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    enum Section { General, Window, Display, Input, Navigation, Rating, Formats, Advanced, SectionCount };
    // Item data role on the action column of the shortcut table: true if the row conflicts.
    static constexpr int ConflictRole = Qt::UserRole + 1;
    static constexpr int ActionRole = Qt::UserRole + 2;
    static constexpr int MaxShortcutsPerAction = 3;

    explicit PreferencesDialog(QWidget* parent = nullptr);
    ~PreferencesDialog() override;

    // Single instance: creates the dialog on first use, then raises it.
    static PreferencesDialog* showPreferences(QWidget* parent = nullptr);
    static PreferencesDialog* existingInstance();

    int sectionCount() const { return SectionCount; }
    QString sectionTitle(int section) const;
    int currentSection() const;
    void showSection(int section);
    // Settings keys (or prefixes ending in '/') reset by the section's reset button.
    QStringList sectionKeys(int section) const;
    void resetSection(int section);

    QTableWidget* shortcutTable() const { return m_shortcutTable; }
    // Message line at the bottom (e.g. the result of "Als Standard-App festlegen").
    void showStatus(const QString& text);
    QString statusText() const;
    // Choices of input slots (4.14) that are not implemented yet: shown disabled.
    static bool isChoiceAvailable(const QString& slotId, const QString& choiceId);
    int conflictCount() const { return m_conflicts; }

public slots:
    bool exportSettingsTo(const QString& filePath); // F-SET-03 (shows errors)
    bool importSettingsFrom(const QString& filePath);

signals:
    // "Als Standard-App festlegen" (F-MAC-04); handled by the app shell.
    void makeDefaultRequested();

private:
    QWidget* buildGeneral();
    QWidget* buildWindow();
    QWidget* buildDisplay();
    QWidget* buildInput();
    QWidget* buildNavigation();
    QWidget* buildRating();
    QWidget* buildFormats();
    QWidget* buildAdvanced();
    QWidget* page(QWidget* content, int section);
    QWidget* buildShortcutEditor();

    void onSettingChanged(const QString& key);
    void addRefresher(const QString& key, std::function<void()> fn);
    void refreshShortcutRow(int row);
    void applyShortcutRow(int row);
    void updateConflicts();

    QStackedWidget* m_stack = nullptr;
    QActionGroup* m_sectionActions = nullptr;
    QTableWidget* m_shortcutTable = nullptr;
    QLabel* m_conflictLabel = nullptr;
    QLabel* m_messageLabel = nullptr;
    InputBindings m_bindings;
    QMultiHash<QString, std::function<void()>> m_refreshers;
    QHash<int, int> m_rowOfAction;
    int m_conflicts = 0;
    bool m_updatingShortcuts = false;
};

} // namespace lf
