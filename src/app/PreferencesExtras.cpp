// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreferencesExtras.h"

#include "OpenWith.h"
#include "core/Settings.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>

namespace lf::prefs {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("lf::PreferencesDialog", text);
}

Settings& st() {
    return Settings::instance();
}

} // namespace

QString editorDisplayName(const QString& appPath) {
    if (appPath.isEmpty())
        return tr("Standard-App des Systems für den Bildtyp (nie Lumaflip)");
    QString name = QFileInfo(appPath).fileName();
    if (name.endsWith(QLatin1String(".app"), Qt::CaseInsensitive))
        name.chop(4);
    return name;
}

bool setEditorApp(const QString& appPath, QString* error) {
    if (!appPath.isEmpty()) {
        AppEntry app;
        app.path = appPath;
        if (isOwnApplication(app)) {
            if (error)
                *error =
                    tr("Lumaflip kann nicht sein eigener Editor sein. Bitte wähle ein anderes Programm.");
            return false;
        }
    }
    st().setValue(key::EditorApp, appPath);
    return true;
}

QWidget* createEditorChooser(QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("setting:") + key::EditorApp);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(row);
    label->setObjectName(QStringLiteral("editorName"));
    auto* choose = new QPushButton(tr("Auswählen …"), row);
    choose->setObjectName(QStringLiteral("chooseEditor"));
    choose->setAutoDefault(false);
    auto* reset = new QPushButton(tr("Standard"), row);
    reset->setObjectName(QStringLiteral("defaultEditor"));
    reset->setAutoDefault(false);
    layout->addWidget(label, 1);
    layout->addWidget(choose);
    layout->addWidget(reset);
    auto refresh = [label, reset] {
        const QString path = st().value(key::EditorApp).toString();
        label->setText(editorDisplayName(path));
        label->setToolTip(path);
        reset->setEnabled(!path.isEmpty());
    };
    refresh();
    QObject::connect(&st(), &Settings::changed, row, [refresh](const QString& k) {
        if (k == QLatin1String(key::EditorApp))
            refresh();
    });
    QObject::connect(choose, &QPushButton::clicked, row, [row] {
        const QString app = QFileDialog::getOpenFileName(
            row->window(), tr("Editor wählen"), QStringLiteral("/Applications"), tr("Programme (*.app)"));
        if (app.isEmpty())
            return;
        QString error;
        if (!setEditorApp(app, &error))
            QMessageBox::information(row->window(), tr("Editor wählen"), error);
    });
    QObject::connect(reset, &QPushButton::clicked, row, [] { setEditorApp({}); });
    return row;
}

QCheckBox* createLiveQueryCheck(QWidget* parent) {
    auto* cb = new QCheckBox(tr("Sortierung live vom Finder-Fenster übernehmen"), parent);
    cb->setObjectName(QStringLiteral("setting:") + key::FinderLiveQuery);
    auto refresh = [cb] {
        const QSignalBlocker block(cb);
        cb->setChecked(st().value(key::FinderLiveQuery).toBool());
    };
    refresh();
    QObject::connect(&st(), &Settings::changed, cb, [refresh](const QString& k) {
        if (k == QLatin1String(key::FinderLiveQuery))
            refresh();
    });
    QObject::connect(cb, &QCheckBox::toggled, cb, [cb, refresh](bool on) {
        if (!on) {
            st().setValue(key::FinderLiveQuery, false);
            return;
        }
        if (st().value(key::FinderLiveQuery).toBool())
            return;
        // F-SORT-04: explain the Automation permission before macOS asks for it (at the
        // next opened folder). Only "Aktivieren" turns the query on.
        refresh(); // unchecked until confirmed
        if (cb->window()->findChild<QMessageBox*>(QStringLiteral("lfLiveQueryExplanation")))
            return;
        auto* box = new QMessageBox(
            QMessageBox::Information, tr("Sortierung vom Finder-Fenster übernehmen"),
            tr("Lumaflip fragt dann beim Öffnen eines Ordners das vorderste Finder-Fenster nach seiner "
               "Sortierung – aber nur, wenn es genau diesen Ordner zeigt."),
            QMessageBox::NoButton, cb->window());
        box->setObjectName(QStringLiteral("lfLiveQueryExplanation"));
        box->setInformativeText(tr(
            "Dafür fragt macOS einmalig, ob Lumaflip den Finder steuern darf („Automation“). Lumaflip liest "
            "nur Ordner, Ansicht und Sortierung des Fensters und ändert nichts. Die Erlaubnis lässt sich "
            "jederzeit unter Systemeinstellungen → Datenschutz & Sicherheit → Automation widerrufen."));
        QPushButton* enable = box->addButton(tr("Aktivieren"), QMessageBox::AcceptRole);
        enable->setObjectName(QStringLiteral("lfLiveQueryEnable"));
        box->addButton(tr("Abbrechen"), QMessageBox::RejectRole);
        box->setDefaultButton(enable);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(cb->window()->isVisible() ? Qt::WindowModal : Qt::NonModal); // sheet
        QObject::connect(box, &QMessageBox::buttonClicked, cb, [enable](QAbstractButton* b) {
            if (b == enable)
                st().setValue(key::FinderLiveQuery, true);
        });
        box->open();
    });
    return cb;
}

} // namespace lf::prefs
