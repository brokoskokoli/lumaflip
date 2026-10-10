// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/PreferencesDialog.h"
#include "core/InputBindings.h"
#include "core/Settings.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>

using namespace lf;

class TestPreferences : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_counter = 0;

    static void choose(QComboBox* combo, const QString& value) {
        const int idx = combo->findData(value);
        QVERIFY(idx >= 0);
        combo->setCurrentIndex(idx);
        emit combo->activated(idx); // what a user selection emits
    }
    static void click(QWidget* w) {
        auto* b = qobject_cast<QAbstractButton*>(w);
        QVERIFY(b);
        b->click();
    }
    static int rowOf(QTableWidget* t, Action a) {
        for (int r = 0; r < t->rowCount(); ++r)
            if (t->item(r, 0)->data(PreferencesDialog::ActionRole).toInt() == int(a))
                return r;
        return -1;
    }
    static QKeySequenceEdit* edit(QTableWidget* t, Action a, int col) {
        return qobject_cast<QKeySequenceEdit*>(t->cellWidget(rowOf(t, a), 1 + col));
    }
    static bool conflictMarked(QTableWidget* t, Action a) {
        return t->item(rowOf(t, a), 0)->data(PreferencesDialog::ConflictRole).toBool();
    }

private slots:
    void init() { Settings::useTestingBackend(m_dir.filePath(QStringLiteral("p%1.ini").arg(++m_counter))); }

    // F-SET-01
    void F_SET_01_allSections() {
        PreferencesDialog dlg;
        QCOMPARE(dlg.sectionCount(), 8);
        const QStringList expected = {
            "Allgemein", "Fenster", "Anzeige",  "Maus & Tastatur", "Navigation & Sortierung",
            "Bewertung", "Formate", "Erweitert"};
        auto* toolbar = dlg.findChild<QToolBar*>("sectionToolbar");
        QVERIFY(toolbar);
        QCOMPARE(toolbar->actions().size(), 8);
        for (int i = 0; i < 8; ++i) {
            QCOMPARE(dlg.sectionTitle(i), expected[i]);
            QCOMPARE(toolbar->actions().at(i)->text().replace("&&", "&"), expected[i]);
            QVERIFY(dlg.findChild<QScrollArea*>(QStringLiteral("section:%1").arg(i)));
            QVERIFY(dlg.findChild<QPushButton*>(QStringLiteral("reset:%1").arg(i))); // F-IN-17 per section
        }
        toolbar->actions().at(PreferencesDialog::Rating)->trigger();
        QCOMPARE(dlg.currentSection(), int(PreferencesDialog::Rating));
        QVERIFY(!dlg.isModal());

        // every setting of Settings.h has an editor (input/ keys via slot combos/tuning/shortcuts)
        for (const QString& k : Settings::knownKeys()) {
            if (k.startsWith(key::ShortcutPrefix))
                continue;
            if (k.startsWith(key::InputPrefix) && !k.contains("Tuning") && k != key::JumpSize) {
                QVERIFY2(dlg.findChild<QComboBox*>("slot:" + k.mid(int(qstrlen(key::InputPrefix)))),
                         qPrintable(k));
                continue;
            }
            QVERIFY2(dlg.findChild<QWidget*>("setting:" + k), qPrintable(k));
        }
        // rating storage choices (settings only; the feature itself is M3)
        QVERIFY(dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::RatingStoreSidecar)->isChecked());
        QVERIFY(
            !dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::RatingStoreEmbedded)->isChecked());
        QVERIFY(
            !dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::RatingStoreFinderTags)->isChecked());
    }

    void singleInstance() {
        PreferencesDialog* a = PreferencesDialog::showPreferences();
        PreferencesDialog* b = PreferencesDialog::showPreferences();
        QCOMPARE(a, b);
        QCOMPARE(PreferencesDialog::existingInstance(), a);
        QVERIFY(a->isVisible());
        delete a;
        QVERIFY(!PreferencesDialog::existingInstance());
    }

    // F-IN-17
    void F_IN_17_comboPersistsAndShowsHelp() {
        PreferencesDialog dlg;
        for (const InputSlot& s : InputBindings::inputSlots()) {
            auto* combo = dlg.findChild<QComboBox*>("slot:" + s.id);
            QVERIFY2(combo, qPrintable(s.id));
            QCOMPARE(combo->count(), s.choices.size());
            QCOMPARE(combo->currentData().toString(), s.defaultChoice);
            QVERIFY(!dlg.findChild<QLabel*>("help:" + s.id)->text().isEmpty());
        }
        auto* wheel = dlg.findChild<QComboBox*>("slot:wheel");
        choose(wheel, "zoom");
        QCOMPARE(Settings::instance().value("input/wheel").toString(), QString("zoom"));
        QCOMPARE(dlg.findChild<QLabel*>("help:wheel")->text(), InputBindings::inputSlots()[0].choiceHelp[1]);
        InputEvent ev;
        ev.kind = InputKind::Wheel;
        ev.wheelSteps = -1;
        QCOMPARE(InputBindings().resolve(ev), Action::ZoomIn);

        // endOfCollection mirrors the navigation setting
        choose(dlg.findChild<QComboBox*>("slot:endOfCollection"), "wrap");
        QCOMPARE(Settings::instance().value(key::WrapMode).toString(), QString("wrap"));
        QCOMPARE(
            dlg.findChild<QComboBox*>(QStringLiteral("setting:") + key::WrapMode)->currentData().toString(),
            QString("wrap"));

        // external change updates the UI
        Settings::instance().setValue("input/esc", "exitModeOrClose");
        QCOMPARE(dlg.findChild<QComboBox*>("slot:esc")->currentData().toString(), QString("exitModeOrClose"));

        // other widget kinds persist too
        auto* spin = dlg.findChild<QSpinBox*>(QStringLiteral("setting:") + key::WheelMinIntervalMs);
        spin->setValue(200);
        QCOMPARE(Settings::instance().value(key::WheelMinIntervalMs).toInt(), 200);
        auto* check = dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::FinderLiveQuery);
        check->setChecked(true); // F-SORT-04: explained first, stored on "Aktivieren"
        auto* explanation = dlg.findChild<QMessageBox*>(QStringLiteral("lfLiveQueryExplanation"));
        QVERIFY(explanation);
        QVERIFY(!Settings::instance().value(key::FinderLiveQuery).toBool());
        click(explanation->findChild<QPushButton*>(QStringLiteral("lfLiveQueryEnable")));
        QVERIFY(Settings::instance().value(key::FinderLiveQuery).toBool());
        QVERIFY(check->isChecked());
        choose(dlg.findChild<QComboBox*>(QStringLiteral("setting:") + key::ZoomMode), "fillWidth");
        QCOMPARE(Settings::instance().value(key::ZoomMode).toString(), QString("fillWidth"));
    }

    // F-IN-18
    void F_IN_18_presetButtons() {
        PreferencesDialog dlg;
        click(dlg.findChild<QPushButton*>("preset:photographer"));
        QCOMPARE(dlg.findChild<QComboBox*>("slot:wheel")->currentData().toString(), QString("zoom"));
        // Not available before 1.0 (F-CULL-01): the preset skips it.
        QCOMPARE(dlg.findChild<QComboBox*>("slot:ratingKeys")->currentData().toString(),
                 QString("cullingOnly"));
        click(dlg.findChild<QPushButton*>("preset:windows"));
        QCOMPARE(dlg.findChild<QComboBox*>("slot:wheel")->currentData().toString(), QString("image"));
        QCOMPARE(dlg.findChild<QComboBox*>("slot:esc")->currentData().toString(), QString("exitModeOrClose"));
        auto* t = dlg.shortcutTable();
        bool hasReturn = false;
        for (int c = 0; c < PreferencesDialog::MaxShortcutsPerAction; ++c)
            hasReturn =
                hasReturn || edit(t, Action::ToggleFullScreen, c)->keySequence() == QKeySequence("Return");
        QVERIFY(hasReturn);
        click(dlg.findChild<QPushButton*>("preset:mac"));
        for (const InputSlot& s : InputBindings::inputSlots())
            if (s.id != "endOfCollection")
                QCOMPARE(dlg.findChild<QComboBox*>("slot:" + s.id)->currentData().toString(),
                         s.defaultChoice);
        QCOMPARE(dlg.conflictCount(), 0);
    }

    // F-SET-02
    void F_SET_02_shortcutEditAndConflicts() {
        PreferencesDialog dlg;
        auto* t = dlg.shortcutTable();
        QVERIFY(t);
        QVERIFY(rowOf(t, Action::ToggleGrid) >= 0);
        QCOMPARE(edit(t, Action::ZoomActual, 0)->keySequence(), QKeySequence("Ctrl+0"));
        QCOMPARE(edit(t, Action::ZoomActual, 1)->keySequence(), QKeySequence("3"));
        QCOMPARE(dlg.conflictCount(), 0);

        edit(t, Action::ToggleGrid, 0)->setKeySequence(QKeySequence("T")); // = filmstrip
        QCOMPARE(InputBindings().shortcut(Action::ToggleGrid), QKeySequence("T"));
        QVERIFY(conflictMarked(t, Action::ToggleGrid));
        QVERIFY(conflictMarked(t, Action::ToggleFilmstrip));
        QVERIFY(!conflictMarked(t, Action::ZoomFit));
        QCOMPARE(dlg.conflictCount(), 2);
        QVERIFY(dlg.findChild<QLabel*>("conflictLabel")->text().contains("doppelt"));
        QVERIFY(t->item(rowOf(t, Action::ToggleGrid), 0)
                    ->toolTip()
                    .contains(actionLabel(Action::ToggleFilmstrip)));

        // per-row reset restores the standard and clears the marking
        click(dlg.findChild<QWidget*>("resetShortcut:toggleGrid"));
        QCOMPARE(edit(t, Action::ToggleGrid, 0)->keySequence(), QKeySequence("G"));
        QVERIFY(!conflictMarked(t, Action::ToggleGrid));
        QCOMPARE(dlg.conflictCount(), 0);

        // clearing a shortcut
        edit(t, Action::OpenInEditor, 0)->clear();
        QVERIFY(InputBindings().shortcuts(Action::OpenInEditor).isEmpty());
        // reset all
        click(dlg.findChild<QPushButton*>("resetAllShortcuts"));
        QCOMPARE(edit(t, Action::OpenInEditor, 0)->keySequence(), QKeySequence("E"));
    }

    void resetSection() {
        PreferencesDialog dlg;
        Settings::instance().setValue(key::ZoomMode, "actual");
        Settings::instance().setValue(key::Background, "#112233");
        Settings::instance().setValue(key::PrefetchAhead, 9);
        Settings::instance().setValue("input/wheel", "zoom");
        Settings::instance().setValue(key::WheelStepsPerImage, 4);
        click(dlg.findChild<QPushButton*>(QStringLiteral("reset:%1").arg(PreferencesDialog::Display)));
        QCOMPARE(Settings::instance().value(key::ZoomMode).toString(), QString("fit"));
        QCOMPARE(Settings::instance().value(key::Background).toString(), QString("dark"));
        QCOMPARE(
            dlg.findChild<QComboBox*>(QStringLiteral("setting:") + key::ZoomMode)->currentData().toString(),
            QString("fit"));
        QCOMPARE(Settings::instance().value(key::PrefetchAhead).toInt(), 9); // other section untouched
        dlg.resetSection(PreferencesDialog::Input);
        QCOMPARE(Settings::instance().value("input/wheel").toString(), QString("image"));
        QCOMPARE(dlg.findChild<QComboBox*>("slot:wheel")->currentData().toString(), QString("image"));
        QCOMPARE(dlg.findChild<QSpinBox*>(QStringLiteral("setting:") + key::WheelStepsPerImage)->value(), 1);
    }

    // F-SET-03
    void F_SET_03_exportImport() {
        PreferencesDialog dlg;
        const QString file = m_dir.filePath("export.json");
        Settings::instance().setValue(key::ShowFilmstrip, true);
        QSignalSpy done(&dlg, &PreferencesDialog::settingsTransferFinished);
        dlg.exportSettingsTo(file);       // written on a worker thread
        QVERIFY(dlg.isTransferRunning()); // returned at once, the result follows
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!dlg.isTransferRunning());
        QVERIFY(done.takeFirst().at(0).toBool());
        Settings::instance().setValue(key::ShowFilmstrip, false);
        dlg.importSettingsFrom(file); // read on a worker thread, applied on the GUI thread
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.takeFirst().at(0).toBool());
        QVERIFY(dlg.findChild<QCheckBox*>(QStringLiteral("setting:") + key::ShowFilmstrip)->isChecked());
        dlg.importSettingsFrom(m_dir.filePath("nope.json"));
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(!done.takeFirst().at(0).toBool());
        QVERIFY(dlg.findChild<QLabel*>("messageLabel")->text().contains("fehlgeschlagen"));
    }

    void makeDefaultButtonEmitsSignal() {
        PreferencesDialog dlg;
        QSignalSpy spy(&dlg, &PreferencesDialog::makeDefaultRequested);
        click(dlg.findChild<QPushButton*>("makeDefault"));
        QCOMPARE(spy.count(), 1);
    }
};

QTEST_MAIN(TestPreferences)
#include "tst_preferences.moc"
