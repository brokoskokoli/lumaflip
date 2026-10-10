// SPDX-License-Identifier: GPL-3.0-or-later
// Multilingual UI, GUI part (SPEC 4.16): live language switch (F-I18N-03) and layout in
// every language (F-I18N-06): menus, the preferences window and the info panel are built
// in each shipped language; no label, button or check box may be cut off.
//
// Opens windows: run only with permission on the developer's laptop (CLAUDE.md).

#include "app/Application.h"
#include "app/InfoPanel.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/Translations.h"
#include "core/ImageProvider.h"
#include "core/Language.h"
#include "core/Settings.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>

using namespace lf;

namespace {

// A visible widget whose text does not fit: its preferred size exceeds what it got.
QString clippedText(QWidget* w) {
    if (!w->isVisible() || w->width() <= 0)
        return {};
    QString text;
    if (auto* l = qobject_cast<QLabel*>(w)) {
        if (l->wordWrap() || l->text().isEmpty() || l->textFormat() == Qt::RichText)
            return {};
        text = l->text();
    } else if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        if (b->text().isEmpty())
            return {};
        text = b->text();
    } else {
        return {};
    }
    const QSize hint = w->sizeHint();
    if (hint.width() > w->width() + 1)
        return QStringLiteral("%1 \"%2\" (%3 > %4 px)")
            .arg(QString::fromLatin1(w->metaObject()->className()), text)
            .arg(hint.width())
            .arg(w->width());
    return {};
}

QStringList clippedTexts(QWidget* root) {
    QStringList out;
    for (QWidget* w : root->findChildren<QWidget*>())
        if (const QString c = clippedText(w); !c.isEmpty())
            out << c;
    return out;
}

} // namespace

class TstI18n : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_counter = 0;

    static void languages_data() {
        QTest::addColumn<QString>("lang");
        for (const QString& code : uiLanguageCodes())
            QTest::newRow(qPrintable(code)) << code;
    }

private slots:
    void init() { Settings::useTestingBackend(m_dir.filePath(QStringLiteral("s%1.ini").arg(++m_counter))); }
    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            delete dlg;
        installTranslations(QStringLiteral("de"));
    }

    // Every catalogue is found by the app (bundle or build tree) together with Qt's own.
    void F_I18N_02_cataloguesInstall_data() { languages_data(); }
    void F_I18N_02_cataloguesInstall() {
        QFETCH(QString, lang);
        QVERIFY2(installTranslations(lang), qPrintable(appTranslationDirs().join(QLatin1Char(' '))));
        QCOMPARE(installedUiLanguage(), lang);
        QVERIFY(!installedQtCatalog().isEmpty());
        if (lang != QLatin1String("de"))
            QVERIFY(!installedAppCatalog().isEmpty());
    }

    // F-I18N-03: choosing a language in the preferences switches menus, the preferences
    // window and the window texts without a restart.
    void F_I18N_03_liveSwitchFromPreferences() {
        installTranslations(QStringLiteral("de"));
        Application app;
        QCOMPARE(app.menu()->menus().first()->title(), QStringLiteral("Ablage"));
        PreferencesDialog* prefs = PreferencesDialog::showPreferences();
        auto* combo = prefs->findChild<QComboBox*>(QStringLiteral("setting:") + key::Language);
        QVERIFY(combo);
        QCOMPARE(combo->itemData(0).toString(), QStringLiteral("system"));
        QVERIFY(combo->findText(QStringLiteral("日本語")) >= 0);
        const int en = combo->findData(QStringLiteral("en"));
        QVERIFY(en >= 0);
        combo->setCurrentIndex(en);
        emit combo->activated(en);
        QTRY_COMPARE(installedUiLanguage(), QStringLiteral("en"));
        QTRY_COMPARE(app.menu()->menus().first()->title(), QStringLiteral("File"));
        QTRY_VERIFY(PreferencesDialog::existingInstance() && PreferencesDialog::existingInstance() != prefs);
        QCOMPARE(PreferencesDialog::existingInstance()->sectionTitle(PreferencesDialog::General),
                 QStringLiteral("General"));
        // Back to the system language: whatever it resolves to is installed.
        Settings::instance().setValue(key::Language, QStringLiteral("system"));
        QTRY_COMPARE(installedUiLanguage(), resolvedUiLanguage());
    }

    // F-I18N-06: no cut-off texts in the preferences (every section) in any language.
    void F_I18N_06_preferencesTextsFit_data() { languages_data(); }
    void F_I18N_06_preferencesTextsFit() {
        QFETCH(QString, lang);
        QVERIFY(installTranslations(lang));
        PreferencesDialog dlg;
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QStringList problems;
        for (int s = 0; s < dlg.sectionCount(); ++s) {
            dlg.showSection(s);
            QTest::qWait(20);
            for (const QString& c : clippedTexts(&dlg))
                problems << dlg.sectionTitle(s) + QStringLiteral(": ") + c;
        }
        // The section toolbar: every title fully visible.
        auto* toolbar = dlg.findChild<QToolBar*>(QStringLiteral("sectionToolbar"));
        QVERIFY(toolbar);
        QVERIFY2(
            toolbar->sizeHint().width() <= dlg.width() + 1,
            qPrintable(QStringLiteral("toolbar %1 > %2").arg(toolbar->sizeHint().width()).arg(dlg.width())));
        QVERIFY2(problems.isEmpty(),
                 qPrintable(lang + QStringLiteral(":\n") + problems.join(QLatin1Char('\n'))));
    }

    // F-I18N-06: menu titles and items are translated and not empty in every language.
    void F_I18N_06_menusTranslated_data() { languages_data(); }
    void F_I18N_06_menusTranslated() {
        QFETCH(QString, lang);
        QVERIFY(installTranslations(lang));
        Application app;
        const QList<QMenu*> menus = app.menu()->menus();
        QVERIFY(menus.size() >= 6);
        for (QMenu* m : menus) {
            QVERIFY(!m->title().trimmed().isEmpty());
            for (QAction* a : m->actions())
                if (!a->isSeparator())
                    QVERIFY2(!a->text().trimmed().isEmpty(), qPrintable(m->title()));
        }
        if (lang != QLatin1String("de"))
            QVERIFY2(menus.first()->title() != QLatin1String("Ablage"), qPrintable(lang));
        // Menus are native on macOS (no clipping); a menu's own width must still be sane.
        for (QMenu* m : menus)
            QVERIFY2(m->sizeHint().width() < 700, qPrintable(m->title()));
    }

    // F-I18N-06: the info panel's labels fit its minimum width in every language.
    void F_I18N_06_infoPanelTextsFit_data() { languages_data(); }
    void F_I18N_06_infoPanelTextsFit() {
        QFETCH(QString, lang);
        QVERIFY(installTranslations(lang));
        const QString file = m_dir.filePath(QStringLiteral("probe.png"));
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QVERIFY(img.save(file));
        InfoPanel panel(ImageProvider::instance(), nullptr);
        panel.resize(240, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.setFile(file);
        QTRY_VERIFY(!panel.isLoading());
        QStringList problems = clippedTexts(&panel);
        // Row labels (first column) are shown completely, values may elide.
        auto* tree = panel.findChild<QTreeWidget*>(QStringLiteral("lfInfoTree"));
        QVERIFY(tree);
        const QFontMetrics fm(tree->font());
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* top = tree->topLevelItem(i);
            for (int r = 0; r < top->childCount(); ++r) {
                const QString label = top->child(r)->text(0);
                if (fm.horizontalAdvance(label) > tree->columnWidth(0))
                    problems << QStringLiteral("row label \"%1\"").arg(label);
            }
        }
        QVERIFY2(problems.isEmpty(),
                 qPrintable(lang + QStringLiteral(":\n") + problems.join(QLatin1Char('\n'))));
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LumaflipTest"));
    TstI18n tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_i18n.moc"
