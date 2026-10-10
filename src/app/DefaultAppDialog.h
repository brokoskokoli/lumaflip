// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-04: "Als Standard-App festlegen" asks with a checklist of the image types the app
// can open (type name + extensions). All are preselected; the user's choice is remembered
// (the types left out are stored, so types added later start selected). Only the selected
// types are passed on; the others stay with their current app.

#include <QDialog>
#include <QStringList>

class QListWidget;
class QPushButton;

namespace lf {

class DefaultAppDialog : public QDialog {
    Q_OBJECT
public:
    // Settings key: UTTypes the user left out last time (QStringList).
    static constexpr auto kExcludedKey = "general/defaultAppExcludedTypes";

    explicit DefaultAppDialog(const QStringList& types, QWidget* parent = nullptr);

    QStringList selectedTypes() const; // in list order
    QListWidget* list() const { return m_list; }
    QPushButton* confirmButton() const { return m_confirm; }

    // The remembered selection out of `types` (all but the excluded ones).
    static QStringList rememberedSelection(const QStringList& types);

signals:
    void confirmed(const QStringList& types); // after the choice was stored

private:
    void updateButton();
    void store();

    QStringList m_types;
    QListWidget* m_list = nullptr;
    QPushButton* m_confirm = nullptr;
};

} // namespace lf
