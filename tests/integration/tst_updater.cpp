// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-08: automatic updates with Sparkle 2. Plumbing only - these tests never start
// Sparkle and never contact the network: the test bundles point SUFeedURL at a local
// file URL, and the updater is only ever started by the app (after launch) or by the
// menu command, which the tests do not trigger.
#include "app/Application.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/Updater.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QTemporaryDir>
#include <QTest>

using namespace lf;

namespace {

// 32 bytes, base64: a syntactically valid ed25519 public key (never used to verify).
const char* kTestPublicKey = "dGVzdC1rZXktbHVtYWZsaXAtdXBkYXRlci10ZXN0cyE=";

} // namespace

class TestUpdater : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    int m_counter = 0;

    // A minimal .app bundle (Info.plist only) with a unique bundle identifier. Sparkle
    // keeps its settings in the domain SUDefaultsDomain names: a file in the temporary
    // directory, so the tests leave nothing in ~/Library/Preferences.
    QString makeBundle(bool withKey, const QString& feed = {}) {
        const QString id = QStringLiteral("io.github.brokoskokoli.lumaflip.tests.updater.%1.%2")
                               .arg(QCoreApplication::applicationPid())
                               .arg(++m_counter);
        const QString app = m_dir.filePath(QStringLiteral("Test%1.app").arg(m_counter));
        QDir().mkpath(app + QStringLiteral("/Contents/MacOS"));
        QString plist = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                       "<plist version=\"1.0\"><dict>\n"
                                       "<key>CFBundleIdentifier</key><string>%1</string>\n"
                                       "<key>CFBundleName</key><string>UpdaterTest</string>\n"
                                       "<key>CFBundlePackageType</key><string>APPL</string>\n"
                                       "<key>CFBundleVersion</key><string>1.0</string>\n"
                                       "<key>CFBundleShortVersionString</key><string>1.0</string>\n"
                                       "<key>SUDefaultsDomain</key><string>%2</string>\n")
                            .arg(id, m_dir.filePath(QStringLiteral("defaults%1").arg(m_counter)));
        if (withKey) {
            const QString url =
                feed.isEmpty() ? QUrl::fromLocalFile(m_dir.filePath("appcast.xml")).toString() : feed;
            plist += QStringLiteral("<key>SUFeedURL</key><string>%1</string>\n"
                                    "<key>SUPublicEDKey</key><string>%2</string>\n")
                         .arg(url, QLatin1String(kTestPublicKey));
        }
        plist += QStringLiteral("</dict></plist>\n");
        QFile f(app + QStringLiteral("/Contents/Info.plist"));
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(plist.toUtf8());
        return app;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void init() { Settings::useTestingBackend(m_dir.filePath(QStringLiteral("s%1.ini").arg(++m_counter))); }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        Updater::setInstanceForTesting(nullptr);
    }

    // The build option (LF_ENABLE_SPARKLE) decides whether Sparkle is linked; without it
    // the app builds and runs with the updater disabled.
    void F_MAC_08_buildOptionMatchesUpdater() {
        QCOMPARE(Updater::isCompiledIn(), bool(LF_HAVE_SPARKLE));
        // CI fetches Sparkle (tools/fetch_sparkle.sh): there it must be compiled in.
        if (qEnvironmentVariableIntValue("LF_REQUIRE_SPARKLE") == 1)
            QVERIFY2(Updater::isCompiledIn(), "LF_REQUIRE_SPARKLE=1, but built without Sparkle");
        // The test binary is no .app bundle with a feed: the app's updater is disabled,
        // says why, and is never started.
        Updater& app = Updater::instance();
        QVERIFY(!app.isAvailable());
        QVERIFY(!app.unavailableReason().isEmpty());
        QVERIFY(!app.isStarted());
        app.checkForUpdates(); // no-op
        QVERIFY(!app.isStarted());
        QVERIFY(!app.automaticallyChecks());
    }

    // Never ship without key: feed and EdDSA public key must both be usable.
    void F_MAC_08_configurationValidation() {
        const QString https = QStringLiteral("https://brokoskokoli.github.io/lumaflip/appcast.xml");
        QVERIFY(Updater::configurationProblem(https, QLatin1String(kTestPublicKey)).isEmpty());
        QVERIFY(Updater::configurationProblem(QStringLiteral("file:///tmp/appcast.xml"),
                                              QLatin1String(kTestPublicKey))
                    .isEmpty());
        QVERIFY(!Updater::configurationProblem(https, QString()).isEmpty());
        QVERIFY(!Updater::configurationProblem(https, QStringLiteral("not base64!")).isEmpty());
        QVERIFY(!Updater::configurationProblem(https, QStringLiteral("c2hvcnQ=")).isEmpty()); // 5 bytes
        QVERIFY(!Updater::configurationProblem(QStringLiteral("http://example.org/appcast.xml"),
                                               QLatin1String(kTestPublicKey))
                     .isEmpty());
        QVERIFY(!Updater::configurationProblem(QString(), QLatin1String(kTestPublicKey)).isEmpty());
    }

    void F_MAC_08_bundleWithoutKeyIsDisabled() {
        const QString app = makeBundle(false);
        QVERIFY(!app.isEmpty());
        auto u = Updater::createForBundle(app);
        QVERIFY(!u->isAvailable());
        QVERIFY(!u->unavailableReason().isEmpty());
        QVERIFY(!u->start());
        QVERIFY(!u->isStarted());

        // A plain http feed is refused as well.
        auto insecure =
            Updater::createForBundle(makeBundle(true, QStringLiteral("http://example.org/a.xml")));
        QVERIFY(!insecure->isAvailable());
    }

    // "Nach Updates suchen …" is in the menu only if the updater is available.
    void F_MAC_08_menuItemMatchesAvailability() {
        {
            Application shell;
            QAction* act = shell.menu()->command(QStringLiteral("checkForUpdates"));
            QVERIFY(act);
            QCOMPARE(act->text(), QStringLiteral("Nach Updates suchen …"));
            QCOMPARE(act->menuRole(), QAction::ApplicationSpecificRole);
            QCOMPARE(act->isVisible(), Updater::instance().isAvailable());
            QVERIFY(!act->isVisible()); // test binary: no feed/key
        }
        auto u = Updater::createForBundle(makeBundle(true));
        QCOMPARE(u->isAvailable(), Updater::isCompiledIn());
        Updater::setInstanceForTesting(u.get());
        {
            Application shell;
            QAction* act = shell.menu()->command(QStringLiteral("checkForUpdates"));
            QVERIFY(act);
            QCOMPARE(act->isVisible(), Updater::isCompiledIn());
        }
        QVERIFY(!u->isStarted()); // building the menu never starts Sparkle
    }

    // Preferences "Allgemein": the check boxes map to SPUUpdater's properties
    // (automaticallyChecksForUpdates, automaticallyDownloadsUpdates).
    void F_MAC_08_preferencesMapToSparkle() {
        {
            PreferencesDialog dlg; // updater unavailable: no rows
            QVERIFY(!dlg.findChild<QCheckBox*>(QStringLiteral("updater:automaticChecks")));
            QVERIFY(!dlg.findChild<QCheckBox*>(QStringLiteral("updater:automaticDownloads")));
        }
        if (!Updater::isCompiledIn())
            QSKIP("built without Sparkle (LF_ENABLE_SPARKLE=OFF)");

        const QString bundle = makeBundle(true);
        auto u = Updater::createForBundle(bundle);
        QVERIFY2(u->isAvailable(), qPrintable(u->unavailableReason()));
        Updater::setInstanceForTesting(u.get());
        // Default without a decision: no automatic checks (Sparkle asks on the 2nd launch).
        QVERIFY(!u->automaticallyChecks());

        PreferencesDialog dlg;
        dlg.showSection(PreferencesDialog::General);
        auto* checks = dlg.findChild<QCheckBox*>(QStringLiteral("updater:automaticChecks"));
        auto* downloads = dlg.findChild<QCheckBox*>(QStringLiteral("updater:automaticDownloads"));
        QVERIFY(checks);
        QVERIFY(downloads);
        QVERIFY(!checks->isChecked());
        QVERIFY(!downloads->isEnabled()); // only together with automatic checks

        checks->click();
        QVERIFY(u->automaticallyChecks());
        QVERIFY(downloads->isEnabled());
        QVERIFY(!downloads->isChecked());
        downloads->click();
        QVERIFY(u->automaticallyDownloads());

        // Stored in Sparkle's own defaults: a fresh updater for the bundle reads them back.
        {
            auto again = Updater::createForBundle(bundle);
            QVERIFY(again->automaticallyChecks());
            QVERIFY(again->automaticallyDownloads());
        }

        checks->click(); // off: no update traffic at all (SPEC 10.3)
        QVERIFY(!u->automaticallyChecks());
        QVERIFY(!downloads->isEnabled());
        QVERIFY(!downloads->isChecked());

        // Changes made elsewhere (Sparkle's permission prompt) show up when the page is shown again.
        u->setAutomaticallyChecks(true);
        dlg.showSection(PreferencesDialog::Window);
        dlg.show();
        dlg.showSection(PreferencesDialog::General);
        checks->window()->hide();
        checks->window()->show();
        QTRY_VERIFY(checks->isChecked());

        QVERIFY(!u->isStarted()); // never started: no network
    }
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LumaflipTest"));
    TestUpdater tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_updater.moc"
