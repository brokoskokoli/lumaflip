// SPDX-License-Identifier: GPL-3.0-or-later
#include "DefaultAppDialog.h"

#include "DisplayText.h"
#include "core/Settings.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace lf {

namespace {

QStringList excludedTypes() {
    return Settings::instance().value(QString::fromLatin1(DefaultAppDialog::kExcludedKey)).toStringList();
}

} // namespace

DefaultAppDialog::DefaultAppDialog(const QStringList& types, QWidget* parent)
    : QDialog(parent), m_types(types) {
    setObjectName(QStringLiteral("lfMakeDefaultDialog"));
    setWindowTitle(tr("Als Standard-App festlegen"));
    auto* layout = new QVBoxLayout(this);
    auto* text = new QLabel(tr("Lumaflip wird die Standard-App für die ausgewählten Bildformate. "
                               "Nicht ausgewählte Formate bleiben beim bisherigen Programm."),
                            this);
    text->setWordWrap(true);
    layout->addWidget(text);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("lfMakeDefaultTypes"));
    const QStringList excluded = excludedTypes();
    for (const QString& id : types) {
        const ImageTypeDescription d = describeImageType(id);
        QString label = d.name;
        if (!d.extensions.isEmpty())
            label += QStringLiteral(" (%1)").arg(d.extensions.join(QStringLiteral(", ")));
        auto* item = new QListWidgetItem(label, m_list);
        item->setData(Qt::UserRole, id);
        item->setToolTip(id);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(excluded.contains(id) ? Qt::Unchecked : Qt::Checked);
    }
    m_list->setMinimumHeight(m_list->sizeHintForRow(0) * std::min(12, int(types.size())) + 8);
    layout->addWidget(m_list, 1);

    auto* row = new QHBoxLayout;
    auto* all = new QPushButton(tr("Alle auswählen"), this);
    all->setObjectName(QStringLiteral("lfMakeDefaultAll"));
    auto* none = new QPushButton(tr("Keine auswählen"), this);
    none->setObjectName(QStringLiteral("lfMakeDefaultNone"));
    row->addWidget(all);
    row->addWidget(none);
    row->addStretch();
    layout->addLayout(row);
    auto setAll = [this](Qt::CheckState state) {
        for (int i = 0; i < m_list->count(); ++i)
            m_list->item(i)->setCheckState(state);
    };
    connect(all, &QPushButton::clicked, this, [setAll] { setAll(Qt::Checked); });
    connect(none, &QPushButton::clicked, this, [setAll] { setAll(Qt::Unchecked); });

    auto* buttons = new QDialogButtonBox(this);
    m_confirm = buttons->addButton(tr("Festlegen"), QDialogButtonBox::AcceptRole);
    m_confirm->setObjectName(QStringLiteral("lfMakeDefaultConfirm"));
    m_confirm->setDefault(true);
    QPushButton* cancel = buttons->addButton(tr("Abbrechen"), QDialogButtonBox::RejectRole);
    cancel->setObjectName(QStringLiteral("lfMakeDefaultCancel"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        store();
        const QStringList selected = selectedTypes();
        accept();
        emit confirmed(selected);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::itemChanged, this, &DefaultAppDialog::updateButton);
    updateButton();
}

QStringList DefaultAppDialog::selectedTypes() const {
    QStringList out;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->checkState() == Qt::Checked)
            out << m_list->item(i)->data(Qt::UserRole).toString();
    return out;
}

QStringList DefaultAppDialog::rememberedSelection(const QStringList& types) {
    const QStringList excluded = excludedTypes();
    QStringList out;
    for (const QString& t : types)
        if (!excluded.contains(t))
            out << t;
    return out;
}

void DefaultAppDialog::updateButton() {
    m_confirm->setEnabled(!selectedTypes().isEmpty());
}

void DefaultAppDialog::store() {
    // Types offered now but not selected; earlier exclusions of types no longer offered stay.
    QStringList excluded;
    for (const QString& t : excludedTypes())
        if (!m_types.contains(t))
            excluded << t;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->checkState() != Qt::Checked)
            excluded << m_list->item(i)->data(Qt::UserRole).toString();
    Settings::instance().setValue(QString::fromLatin1(kExcludedKey), excluded);
}

} // namespace lf
