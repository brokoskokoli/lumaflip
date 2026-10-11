// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-08: automatic updates with Sparkle 2 (macOS) and WinSparkle (Windows). Plumbing
// only - these tests never start the updater and never contact the network: the macOS test
// bundles point SUFeedURL at a local file URL, the Windows updaters keep their settings in
// an INI file (not the registry), and the updater is only ever started by the app (after
// launch) or by the menu command, which the tests do not trigger.
#include "app/Application.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/Updater.h"
#include "core/Settings.h"
#include "core/UpdateConfig.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
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
    // An updater "installation": a .app bundle on macOS, a feed + key + settings file on
    // Windows (WinSparkle keeps its settings there instead of the registry).
    struct Target {
        QString bundle;
        QString feed;
        QString key;
        QString settingsFile;
    };
    Target makeTarget(bool withKey, const QString& feed = {}) {
#ifdef Q_OS_WIN
        Target t;
        // WinSparkle accepts http(s) feeds only; the URL is never contacted (never started).
        t.feed = feed.isEmpty() ? QLatin1String(update::kWindowsFeedUrl) : feed;
        t.key = withKey ? QLatin1String(kTestPublicKey) : QString();
        t.settingsFile = m_dir.filePath(QStringLiteral("winsparkle%1.ini").arg(++m_counter));
        return t;
#else
        return Target{makeBundle(withKey, feed), {}, {}, {}};
#endif
    }
    static std::unique_ptr<Updater> open(const Target& t) {
#ifdef Q_OS_WIN
        return Updater::createForConfiguration(t.feed, t.key, t.settingsFile);
#else
        return Updater::createForBundle(t.bundle);
#endif
    }

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
    void initTestCase() {
        QVERIFY(m_dir.isValid());
#ifdef LF_TEST_WINSPARKLE_DLL
        Updater::setWinSparkleLibraryForTesting(QStringLiteral(LF_TEST_WINSPARKLE_DLL));
#endif
    }

    void init() { Settings::useTestingBackend(m_dir.filePath(QStringLiteral("s%1.ini").arg(++m_counter))); }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        Updater::setInstanceForTesting(nullptr);
    }

    // The build options (LF_ENABLE_SPARKLE, LF_ENABLE_WINSPARKLE) decide whether an updater
    // is part of the build; without it the app builds and runs with the updater disabled.
    void F_MAC_08_buildOptionMatchesUpdater() {
        QCOMPARE(Updater::isCompiledIn(), bool(LF_HAVE_SPARKLE) || bool(LF_HAVE_WINSPARKLE));
        // CI fetches Sparkle (tools/fetch_sparkle.sh) and WinSparkle (tools/fetch_winsparkle.py):
        // there they must be part of the build.
        if (qEnvironmentVariableIntValue("LF_REQUIRE_SPARKLE") == 1)
            QVERIFY2(Updater::isCompiledIn(), "LF_REQUIRE_SPARKLE=1, but built without Sparkle");
        if (qEnvironmentVariableIntValue("LF_REQUIRE_WINSPARKLE") == 1)
            QVERIFY2(LF_HAVE_WINSPARKLE, "LF_REQUIRE_WINSPARKLE=1, but built without WinSparkle");
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
        auto u = open(makeTarget(false));
        QVERIFY(!u->isAvailable());
        QVERIFY(!u->unavailableReason().isEmpty());
        QVERIFY(!u->start());
        QVERIFY(!u->isStarted());

        // A plain http feed is refused as well.
        auto insecure = open(makeTarget(true, QStringLiteral("http://example.org/a.xml")));
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
        auto u = open(makeTarget(true));
        QCOMPARE(u->isAvailable(), Updater::isCompiledIn());
        if (Updater::isCompiledIn())
            QVERIFY2(u->isAvailable(), qPrintable(u->unavailableReason()));
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
    // (automaticallyChecksForUpdates, automaticallyDownloadsUpdates) on macOS and to
    // WinSparkle's automatic check on Windows (WinSparkle always asks before it installs:
    // "Updates automatisch installieren" stays disabled there).
    void F_MAC_08_preferencesMapToSparkle() {
        {
            PreferencesDialog dlg; // updater unavailable: no rows
            QVERIFY(!dlg.findChild<QCheckBox*>(QStringLiteral("updater:automaticChecks")));
            QVERIFY(!dlg.findChild<QCheckBox*>(QStringLiteral("updater:automaticDownloads")));
        }
        if (!Updater::isCompiledIn())
            QSKIP("built without Sparkle/WinSparkle (LF_ENABLE_SPARKLE/LF_ENABLE_WINSPARKLE=OFF)");

        const Target target = makeTarget(true);
        auto u = open(target);
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
        QCOMPARE(downloads->isEnabled(), u->allowsAutomaticDownloads());
        QVERIFY(!downloads->isChecked());
#ifdef Q_OS_WIN
        QVERIFY(!u->allowsAutomaticDownloads());
        QVERIFY(!u->automaticallyDownloads());
#else
        downloads->click();
        QVERIFY(u->automaticallyDownloads());
#endif

        // Stored in the updater's own settings: a fresh updater for the same installation
        // reads them back.
        {
            auto again = open(target);
            QVERIFY(again->automaticallyChecks());
            QCOMPARE(again->automaticallyDownloads(), u->automaticallyDownloads());
        }
#ifdef Q_OS_WIN
        // WinSparkle's own key, in the settings file instead of the registry.
        QCOMPARE(QSettings(target.settingsFile, QSettings::IniFormat)
                     .value(QStringLiteral("WinSparkle/CheckForUpdates"))
                     .toString(),
                 QStringLiteral("1"));
#endif

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
