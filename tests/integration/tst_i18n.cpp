// SPDX-License-Identifier: GPL-3.0-or-later
// Multilingual UI, GUI part (SPEC 4.16): live language switch (F-I18N-03) and layout in
// every language (F-I18N-06): menus, the preferences window, the info panel and the other
// windows and dialogs are built in each shipped language; no label, button or check box may
// be cut off.
//
// Opens windows: run only with permission on the developer's laptop (CLAUDE.md).

#include "app/Application.h"
#include "app/CompareView.h"
#include "app/CouplingManager.h"
#include "app/CullingController.h"
#include "app/DefaultAppDialog.h"
#include "app/ImageView.h"
#include "app/InfoPanel.h"
#include "app/MainMenu.h"
#include "app/MetadataDialog.h"
#include "app/NavigationExtras.h"
#include "app/PreferencesDialog.h"
#include "app/QuickFilterBar.h"
#include "app/QuickMoveController.h"
#include "app/Translations.h"
#include "app/ViewerWindow.h"
#include "app/WindowExtras.h"
#include "core/ImageProvider.h"
#include "core/InputBindings.h"
#include "core/Language.h"
#include "core/Settings.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
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

    // F-I18N-03 (review 5): a metadata load that runs while the language changes is dropped
    // and read again - the panel never keeps rows in the old language.
    void F_I18N_03_infoPanelLoadDuringSwitchIsRedone() {
        QVERIFY(installTranslations(QStringLiteral("de")));
        const QString file = m_dir.filePath(QStringLiteral("switch.png"));
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QVERIFY(img.save(file));
        InfoPanel panel(ImageProvider::instance(), nullptr);
        panel.resize(240, 600);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.setFile(file);                                // the load starts now (German) ...
        QVERIFY(installTranslations(QStringLiteral("en"))); // ... and the language changes
        QTRY_VERIFY(!panel.isLoading());
        QTest::qWait(200); // a dropped load restarts; wait for the final result
        QTRY_VERIFY(!panel.isLoading());
        QTreeWidget* tree = panel.tree();
        QVERIFY(tree && tree->topLevelItemCount() > 0);
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("File"));
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            for (int r = 0; r < tree->topLevelItem(i)->childCount(); ++r)
                QVERIFY2(tree->topLevelItem(i)->child(r)->text(0) != QStringLiteral("Größe"),
                         "row label still German");
    }

    // F-I18N-03 (review 5): texts set once - the quick filter's placeholder and tooltip, the
    // accessible names of grid and compare view - and decoder errors on the error card follow
    // a live switch.
    void F_I18N_03_persistentTextsFollowTheSwitch() {
        QVERIFY(installTranslations(QStringLiteral("de")));
        const QString broken = m_dir.filePath(QStringLiteral("zz_switch_broken.jpg"));
        {
            QFile f(broken);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not a jpeg");
        }
        Application app;
        ViewerWindow* w = app.openPaths({broken});
        QVERIFY(w && QTest::qWaitForWindowExposed(w));
        QTRY_VERIFY(w->view()->isShowingError());
        auto cardTexts = [w] {
            QStringList out;
            if (auto* card = w->view()->findChild<QWidget*>(QStringLiteral("lfErrorCard")))
                for (QLabel* l : card->findChildren<QLabel*>())
                    out << l->text();
            return out.join(QLatin1Char('|'));
        };
        const QString germanCard = cardTexts();
        QuickFilterBar* filter = w->extras()->quickFilter();
        QCOMPARE(filter->field()->placeholderText(), QStringLiteral("Dateiname filtern"));
        CompareView compare(ImageProvider::instance());
        QCOMPARE(compare.accessibleName(), QStringLiteral("Vergleich"));

        QVERIFY(installTranslations(QStringLiteral("en")));
        QTRY_COMPARE(filter->field()->placeholderText(), QStringLiteral("Filter file names"));
        QTRY_COMPARE(compare.accessibleName(), QStringLiteral("Compare"));
        // The file is decoded again; the new failure brings the English text.
        // 15 s: slow machines (sanitizers on CI); the texts are printed if it fails.
        QTRY_VERIFY2_WITH_TIMEOUT(cardTexts() != germanCard && w->view()->isShowingError(),
                                  qPrintable(QStringLiteral("card: %1 | before: %2 | error shown: %3")
                                                 .arg(cardTexts(), germanCard)
                                                 .arg(w->view()->isShowingError())),
                                  15000);
    }

    // F-I18N-03 (review 5): an explicit language also goes to AppleLanguages (AppKit's own
    // texts follow after a restart); the preferences say so.
    void F_I18N_03_preferencesNoteSystemMenusAfterRestart() {
        QVERIFY(installTranslations(QStringLiteral("de")));
        PreferencesDialog* prefs = PreferencesDialog::showPreferences();
        bool found = false;
        for (QLabel* l : prefs->findChildren<QLabel*>())
            found = found || l->text().contains(QStringLiteral("nach einem Neustart"));
        QVERIFY(found);
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

    // F-I18N-06: the other windows and dialogs in every language - name conflict, raw
    // metadata, "Gehe zu Bild", default app, start screen, error card, shortcut overview,
    // culling key help and "Fenster koppeln": no label or button cut off.
    void F_I18N_06_dialogsTextsFit_data() { languages_data(); }
    void F_I18N_06_dialogsTextsFit() {
        QFETCH(QString, lang);
        QVERIFY(installTranslations(lang));
        QStringList problems;
        auto collect = [&](const QString& what, QWidget* root) {
            QVERIFY2(root, qPrintable(what));
            for (const QString& c : clippedTexts(root))
                problems << what + QStringLiteral(": ") + c;
        };
        auto topLevel = [](const QString& name) -> QWidget* {
            QWidget* found = nullptr;
            (void)QTest::qWaitFor(
                [&] {
                    for (QWidget* w : QApplication::topLevelWidgets())
                        if (w->objectName() == name && w->isVisible())
                            found = w;
                    return found != nullptr;
                },
                3000);
            return found;
        };
        const QString image = m_dir.filePath(QStringLiteral("probe.png"));
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::darkCyan);
        QVERIFY(img.save(image));
        const QString broken = m_dir.filePath(QStringLiteral("zz_broken.jpg"));
        {
            QFile f(broken);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not a jpeg");
        }

        // Name conflict (F-FILE-11), for a RAW+JPEG pair (the longest texts).
        QuickMoveController::askConflict(nullptr,
                                         {QStringLiteral("IMG_0001.CR3"), QStringLiteral("IMG_0001.JPG")},
                                         m_dir.path(), 3, [](std::optional<ConflictChoice>, bool) {});
        QWidget* conflict = topLevel(QStringLiteral("lfConflictDialog"));
        collect(QStringLiteral("conflict"), conflict);
        if (conflict)
            conflict->close();

        // Raw metadata window (F-INFO-05).
        // MetadataDialog deletes itself on close (WA_DeleteOnClose): heap only.
        QPointer<MetadataDialog> meta = new MetadataDialog;
        meta->setPath(image);
        meta->show();
        QVERIFY(QTest::qWaitForWindowExposed(meta.data()));
        QTRY_VERIFY(!meta->isLoading());
        collect(QStringLiteral("metadata"), meta.data());
        meta->close();
        QTRY_VERIFY(meta.isNull());

        // Default app (F-MAC-04).
        DefaultAppDialog defaults({QStringLiteral("public.jpeg"), QStringLiteral("public.png"),
                                   QStringLiteral("public.heic"), QStringLiteral("com.adobe.raw-image")});
        defaults.show();
        QVERIFY(QTest::qWaitForWindowExposed(&defaults));
        collect(QStringLiteral("default app"), &defaults);
        defaults.close();

        Application app;
        // Start screen (empty window).
        ViewerWindow* empty = app.newWindow();
        QVERIFY(empty && QTest::qWaitForWindowExposed(empty));
        collect(QStringLiteral("start screen"), empty);

        // "Gehe zu Bild" (F-NAV-06) and the culling key help (F-CULL-06).
        ViewerWindow* w = app.openPaths({image});
        QVERIFY(w && QTest::qWaitForWindowExposed(w));
        QTRY_COMPARE(w->shownPath(), image);
        w->resize(900, 640);
        w->perform(Action::GoToImage);
        QDialog* goTo = w->extras()->navigation()->goToDialog();
        QVERIFY(goTo && QTest::qWaitForWindowExposed(goTo));
        collect(QStringLiteral("go to"), goTo);
        goTo->close();
        w->culling()->setActive(true);
        QTRY_VERIFY(w->culling()->overlay() && w->culling()->overlay()->isVisible());
        {
            // The overlay paints its help lines; each must fit the overlay (same font and
            // margins as CullingOverlay::paintEvent).
            QWidget* overlay = w->culling()->overlay();
            QFont hf = overlay->font();
            hf.setPointSizeF(hf.pointSizeF() * 1.25);
            const QFontMetrics fh(hf);
            for (const char* line :
                 {"0–5  Sterne     P  Behalten     X  Verwerfen     U  Markierung weg",
                  "← →  Blättern     M/C + 1–9  Verschieben/Kopieren     ⌘Z  Rückgängig     Esc  Beenden"}) {
                const QString text = QCoreApplication::translate("lf::CullingController", line);
                if (fh.horizontalAdvance(text) + 2 * 10 + 8 > overlay->width() - 32)
                    problems << QStringLiteral("culling help \"%1\" (%2 > %3 px)")
                                    .arg(text)
                                    .arg(fh.horizontalAdvance(text) + 28)
                                    .arg(overlay->width() - 32);
            }
        }
        w->culling()->setActive(false);

        // Error card (F-FMT-02).
        ViewerWindow* bad = app.openPaths({broken});
        QVERIFY(bad && QTest::qWaitForWindowExposed(bad));
        bad->resize(900, 640);
        auto visibleCard = [bad] {
            auto* c = bad->findChild<QWidget*>(QStringLiteral("lfErrorCard"));
            return c && c->isVisible() ? c : nullptr;
        };
        QTRY_VERIFY(visibleCard());
        QWidget* card = visibleCard();
        collect(QStringLiteral("error card"), card);

        // Shortcut overview (F-IN-*).
        app.showShortcuts();
        collect(QStringLiteral("shortcuts"), topLevel(QStringLiteral("lfShortcutsDialog")));

        // "Fenster koppeln" (F-WIN-*): needs two windows.
        CouplingManager::of(app).showDialog(w);
        QWidget* couple = topLevel(QStringLiteral("lfCoupleDialog"));
        collect(QStringLiteral("couple windows"), couple);
        if (couple)
            couple->close();

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
