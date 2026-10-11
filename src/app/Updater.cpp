// SPDX-License-Identifier: GPL-3.0-or-later
#include "Updater.h"

#include "core/UpdateConfig.h"

#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#if LF_HAVE_SPARKLE
#include "platform/mac/SparkleUpdater.h"
#else
namespace lf {
class SparkleUpdater {}; // never instantiated without Sparkle
} // namespace lf
#endif

#ifdef Q_OS_WIN
#include "Translations.h"
#include "WinSparkleUpdater_win.h"
#else
namespace lf {
class WinSparkleUpdater {}; // Windows only
} // namespace lf
#endif

namespace lf {

Q_LOGGING_CATEGORY(lcUpdater, "lumaflip.updater")

namespace {

QPointer<Updater> g_testInstance;
QString g_winSparkleLibrary; // tests: WinSparkle.dll outside the executable's folder

// Keeps the preference check boxes in sync with Sparkle (its permission prompt or
// another window may change the settings): refreshes whenever the page is shown or its
// window becomes active.
class PreferencesSync : public QObject {
public:
    PreferencesSync(QWidget* page, QCheckBox* checks, QCheckBox* downloads)
        : QObject(page), m_page(page), m_checks(checks), m_downloads(downloads) {
        page->installEventFilter(this);
        refresh();
    }
    void refresh() {
        Updater& u = Updater::instance();
        const QSignalBlocker b1(m_checks);
        const QSignalBlocker b2(m_downloads);
        m_checks->setChecked(u.automaticallyChecks());
        m_downloads->setChecked(u.automaticallyChecks() && u.automaticallyDownloads());
        m_downloads->setEnabled(u.automaticallyChecks() && u.allowsAutomaticDownloads());
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_page && (event->type() == QEvent::Show || event->type() == QEvent::WindowActivate))
            refresh();
        return false;
    }

private:
    QWidget* m_page;
    QCheckBox* m_checks;
    QCheckBox* m_downloads;
};

} // namespace

Updater::Updater(const QString& bundlePath) {
#if LF_HAVE_SPARKLE
    const SparkleBundleInfo info = readSparkleBundleInfo(bundlePath);
    m_bundleId = info.bundleId;
    if (!info.isBundle)
        m_reason = QStringLiteral("not running from an app bundle");
    else if (info.feedUrl.isEmpty() && info.publicKey.isEmpty())
        m_reason =
            QStringLiteral("no SUFeedURL/SUPublicEDKey in Info.plist (built without LF_SPARKLE_PUBLIC_KEY)");
    else
        m_reason = configurationProblem(info.feedUrl, info.publicKey);
    if (m_reason.isEmpty())
        m_sparkle = std::make_unique<SparkleUpdater>(bundlePath);
#elif LF_HAVE_WINSPARKLE
    Q_UNUSED(bundlePath);
    const QString key = QStringLiteral(LF_WINSPARKLE_PUBLIC_KEY);
    if (key.isEmpty())
        m_reason = QStringLiteral("no EdDSA public key compiled in (built without LF_SPARKLE_PUBLIC_KEY)");
    else
        m_reason = configurationProblem(QStringLiteral(LF_WINSPARKLE_FEED_URL), key);
    if (m_reason.isEmpty()) {
        // The app's own updater: settings in the registry, WinSparkle.dll next to Lumaflip.exe.
        WinSparkleUpdater::Config config;
        config.feedUrl = QStringLiteral(LF_WINSPARKLE_FEED_URL);
        config.publicKey = key;
        config.appName = QStringLiteral("Lumaflip");
        config.appVersion = QStringLiteral(LF_APP_VERSION);
        config.libraryPath = g_winSparkleLibrary;
        m_winSparkle = WinSparkleUpdater::create(config, &m_reason);
    }
#else
    Q_UNUSED(bundlePath);
    m_reason = QStringLiteral("built without Sparkle (LF_ENABLE_SPARKLE=OFF)");
#endif
}

Updater::Updater(const QString& feedUrl, const QString& publicKey, const QString& settingsFile) {
#if LF_HAVE_WINSPARKLE
    m_reason = configurationProblem(feedUrl, publicKey);
    if (m_reason.isEmpty()) {
        WinSparkleUpdater::Config config;
        config.feedUrl = feedUrl;
        config.publicKey = publicKey;
        config.appName = QStringLiteral("Lumaflip");
        config.appVersion = QStringLiteral(LF_APP_VERSION);
        config.settingsFile = settingsFile;
        config.libraryPath = g_winSparkleLibrary;
        m_winSparkle = WinSparkleUpdater::create(config, &m_reason);
    }
#else
    Q_UNUSED(feedUrl);
    Q_UNUSED(publicKey);
    Q_UNUSED(settingsFile);
    m_reason = QStringLiteral("WinSparkle is not part of this build (Windows only, LF_ENABLE_WINSPARKLE)");
#endif
}

Updater::~Updater() = default;

Updater& Updater::instance() {
    if (g_testInstance)
        return *g_testInstance;
    static QPointer<Updater> app;
    if (!app) {
        app = new Updater(QString());
        if (auto* qapp = QCoreApplication::instance()) {
            app->setParent(qapp);
            if (app->isAvailable()) {
                QTimer::singleShot(kStartDelayMs, app, [u = app.data()] { u->start(); });
            }
        }
        if (!app->isAvailable())
            qCInfo(lcUpdater) << "automatic updates disabled:" << app->unavailableReason();
    }
    return *app;
}

void Updater::setInstanceForTesting(Updater* updater) {
    g_testInstance = updater;
}

std::unique_ptr<Updater> Updater::createForBundle(const QString& bundlePath) {
    return std::unique_ptr<Updater>(new Updater(bundlePath));
}

std::unique_ptr<Updater> Updater::createForConfiguration(const QString& feedUrl, const QString& publicKey,
                                                         const QString& settingsFile) {
    return std::unique_ptr<Updater>(new Updater(feedUrl, publicKey, settingsFile));
}

void Updater::setWinSparkleLibraryForTesting(const QString& path) {
    g_winSparkleLibrary = path;
}

bool Updater::isCompiledIn() {
    return LF_HAVE_SPARKLE != 0 || LF_HAVE_WINSPARKLE != 0;
}

QString Updater::configurationProblem(const QString& feedUrl, const QString& publicKey) {
    return update::configurationProblem(feedUrl, publicKey);
}

bool Updater::isAvailable() const {
    return m_sparkle != nullptr || m_winSparkle != nullptr;
}

QString Updater::unavailableReason() const {
    return m_reason;
}

QString Updater::bundleId() const {
    return m_bundleId;
}

bool Updater::isStarted() const {
#if LF_HAVE_SPARKLE
    return m_sparkle && m_sparkle->isStarted();
#elif LF_HAVE_WINSPARKLE
    return m_winSparkle && m_winSparkle->isStarted();
#else
    return false;
#endif
}

bool Updater::automaticallyChecks() const {
#if LF_HAVE_SPARKLE
    return m_sparkle && m_sparkle->automaticallyChecks();
#elif LF_HAVE_WINSPARKLE
    return m_winSparkle && m_winSparkle->automaticallyChecks();
#else
    return false;
#endif
}

void Updater::setAutomaticallyChecks(bool on) {
#if LF_HAVE_SPARKLE
    if (!m_sparkle || m_sparkle->automaticallyChecks() == on)
        return;
    m_sparkle->setAutomaticallyChecks(on);
    emit settingsChanged();
#elif LF_HAVE_WINSPARKLE
    if (!m_winSparkle || m_winSparkle->automaticallyChecks() == on)
        return;
    m_winSparkle->setAutomaticallyChecks(on);
    emit settingsChanged();
#else
    Q_UNUSED(on);
#endif
}

bool Updater::automaticallyDownloads() const {
#if LF_HAVE_SPARKLE
    return m_sparkle && m_sparkle->automaticallyDownloads();
#else
    return false; // WinSparkle always asks before it installs
#endif
}

void Updater::setAutomaticallyDownloads(bool on) {
#if LF_HAVE_SPARKLE
    if (!m_sparkle || m_sparkle->automaticallyDownloads() == on)
        return;
    m_sparkle->setAutomaticallyDownloads(on);
    emit settingsChanged();
#else
    Q_UNUSED(on);
#endif
}

bool Updater::allowsAutomaticDownloads() const {
#if LF_HAVE_SPARKLE
    return m_sparkle && m_sparkle->allowsAutomaticDownloads();
#else
    return false;
#endif
}

bool Updater::start() {
#if LF_HAVE_SPARKLE
    if (!m_sparkle)
        return false;
    if (m_sparkle->isStarted())
        return true;
    QString error;
    if (!m_sparkle->start(&error)) {
        qCWarning(lcUpdater) << "Sparkle did not start:" << error;
        return false;
    }
    return true;
#elif LF_HAVE_WINSPARKLE
    if (!m_winSparkle)
        return false;
    if (m_winSparkle->isStarted())
        return true;
    QString error;
    // WinSparkle's windows in Lumaflip's language (F-I18N-03), not only Windows' one.
    if (!m_winSparkle->start(update::winSparkleLanguage(installedUiLanguage()), &error)) {
        qCWarning(lcUpdater) << "WinSparkle did not start:" << error;
        return false;
    }
    return true;
#else
    return false;
#endif
}

void Updater::checkForUpdates() {
#if LF_HAVE_SPARKLE
    if (start())
        m_sparkle->checkForUpdates();
#elif LF_HAVE_WINSPARKLE
    if (start())
        m_winSparkle->checkForUpdates();
#endif
}

void Updater::setUpMenuAction(QAction* action) {
    // Application menu ("Lumaflip"), below "Über Lumaflip".
    action->setMenuRole(QAction::ApplicationSpecificRole);
    action->setVisible(instance().isAvailable());
    QObject::connect(action, &QAction::triggered, action, [] { instance().checkForUpdates(); });
}

void Updater::addPreferences(QFormLayout* form, QWidget* page) {
    Updater& u = instance();
    if (!u.isAvailable())
        return;
    auto* checks = new QCheckBox(tr("Automatisch nach Updates suchen"), page);
    checks->setObjectName(QStringLiteral("updater:automaticChecks"));
    auto* downloads = new QCheckBox(tr("Updates automatisch installieren"), page);
    downloads->setObjectName(QStringLiteral("updater:automaticDownloads"));
    auto* help = new QLabel(tr("Lumaflip fragt dazu einmal am Tag bei GitHub nach einer neuen Version. "
                               "Das ist die einzige Netzwerkverbindung der App."),
                            page);
    help->setWordWrap(true); // secondary text, styled like the other help labels
    QFont f = help->font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    help->setFont(f);
    help->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(tr("Updates:"), checks);
    form->addRow(QString(), downloads);
    form->addRow(QString(), help);
    auto* sync = new PreferencesSync(page, checks, downloads);
    connect(checks, &QCheckBox::toggled, sync, [sync](bool on) {
        Updater::instance().setAutomaticallyChecks(on);
        sync->refresh();
    });
    connect(downloads, &QCheckBox::toggled, sync, [sync](bool on) {
        Updater::instance().setAutomaticallyDownloads(on);
        sync->refresh();
    });
}

} // namespace lf
