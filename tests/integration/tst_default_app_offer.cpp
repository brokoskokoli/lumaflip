// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-09: the "not the default app" bar in real windows, with a fake shell that reports
// another app as the default (the real system is never changed): the bar appears once per
// type above the image, its buttons work, it does not come back for an answered type (also
// after a restart), "Nicht mehr fragen" ends it, and nothing appears when the app is not
// the installed one or the answer is unknown.

#include "app/Application.h"
#include "app/DefaultAppBar.h"
#include "app/DefaultAppDialog.h"
#include "app/ImageView.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "core/DefaultAppOffer.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMutex>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QToolButton>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace lf;
using State = DefaultViewerStatus::State;

namespace {

class NullTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& p, QString*) override { return p; }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return true; }
};

// Reports another app as the default for JPEG and PNG; records what it is asked to set.
class FakeShell : public Shell {
public:
    FakeShell() {
        add(QStringLiteral("jpg"), QStringLiteral("public.jpeg"), QStringLiteral("JPEG-Bild"));
        add(QStringLiteral("png"), QStringLiteral("public.png"), QStringLiteral("PNG-Bild"));
        add(QStringLiteral("gif"), QStringLiteral("com.compuserve.gif"), QStringLiteral("GIF-Bild"));
    }
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg", "public.png"}; }
    bool makeDefaultViewer(const QStringList& types, QString* error) override {
        QMutexLocker lock(&mutex);
        madeDefault << types;
        if (!fail)
            return true;
        if (error)
            *error = QStringLiteral("Systemfehler 42");
        return false;
    }
    DefaultViewerStatus defaultViewerStatus(const QString& path) override {
        ++queries;
        return bySuffix.value(QFileInfo(path).suffix().toLower());
    }
    bool isInstalledApp() override { return installed; }

    QStringList made() {
        QMutexLocker lock(&mutex);
        return madeDefault;
    }

    QHash<QString, DefaultViewerStatus> bySuffix; // set before the queries start
    std::atomic<bool> installed{true};
    std::atomic<bool> fail{false};
    std::atomic<int> queries{0};

private:
    void add(const QString& suffix, const QString& type, const QString& name) {
        DefaultViewerStatus s;
        s.state = State::NotDefault;
        s.type = type;
        s.typeName = name;
        bySuffix.insert(suffix, s);
    }
    QMutex mutex;
    QStringList madeDefault;
};

} // namespace

class TstDefaultAppOffer : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NullTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<FakeShell> shell;
    std::unique_ptr<Application> app;

    QString image(const QString& name, Qt::GlobalColor color = Qt::darkCyan) const {
        const QString p = QDir::cleanPath(dir->path()) + QLatin1Char('/') + name;
        QImage img(48, 32, QImage::Format_RGB32);
        img.fill(color);
        const QByteArray format = QFileInfo(name).suffix() == QLatin1String("jpg") ? "JPEG" : "PNG";
        if (!img.save(p, QFileInfo(name).suffix() == QLatin1String("gif") ? "PNG" : format.constData()))
            return {};
        return p;
    }

    void startApp() {
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
        app->enableDefaultAppOffer(0, 0);
        QVERIFY(app->defaultAppOffer());
    }

    ViewerWindow* open(const QString& p) {
        ViewerWindow* w = app->openPaths({p});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        return w;
    }

    // The offer's query for the current image came back (whatever it said).
    bool waitForQueries(int count) {
        return QTest::qWaitFor([this, count] { return shell->queries.load() >= count; }, 10000) &&
               (QTest::qWait(150), true); // the answer reaches the GUI thread
    }

    DefaultAppBar* bar() const { return app->defaultAppOffer()->visibleBar(); }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        RecentItems::setSystemRegistrationEnabled(false);
    }

    void init() {
        qunsetenv(defaultapp::kDisableEnv); // the test harness sets it for all other tests
        Settings::useTestingBackend(
            m_settingsDir.filePath(QStringLiteral("s%1.ini").arg(++m_settingsCounter)));
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        trash = std::make_unique<NullTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<FakeShell>();
        startApp();
    }

    void cleanup() {
        app.reset();
        QTest::qWait(10);
        QThreadPool::globalInstance()->waitForDone(5000);
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
    }

    // Covers: F-MAC-09 (bar above the image, "Festlegen" sets this type, success toast,
    // asked once per type - also after a restart)
    void F_MAC_09_barAppearsOnceAndSetsTheType() {
        const QString a = image(QStringLiteral("a.jpg"));
        const QString b = image(QStringLiteral("b.jpg"), Qt::darkRed);
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        ViewerWindow* w = open(a);
        QVERIFY(w);
        QTRY_VERIFY_WITH_TIMEOUT(bar(), 10000);
        DefaultAppBar* offer = bar();
        QCOMPARE(offer->window(), static_cast<QWidget*>(w));
        QCOMPARE(offer->type(), QStringLiteral("public.jpeg"));
        QCOMPARE(offer->text(),
                 QStringLiteral("Lumaflip ist nicht die Standard-App für den Dateityp „JPEG-Bild“."));
        // At the top of the window, above the image - not over it.
        QVERIFY(offer->isVisible());
        QVERIFY(offer->geometry().bottom() < w->view()->geometry().top() + 1);
        QVERIFY(offer->findChild<QPushButton*>(QStringLiteral("lfDefaultAppAll")));
        QVERIFY(offer->findChild<QPushButton*>(QStringLiteral("lfDefaultAppNever")));
        QVERIFY(offer->findChild<QToolButton*>(QStringLiteral("lfDefaultAppClose")));

        auto* set = offer->findChild<QPushButton*>(QStringLiteral("lfDefaultAppSet"));
        QVERIFY(set);
        set->click();
        QTRY_COMPARE(shell->made(), QStringList{QStringLiteral("public.jpeg")});
        QVERIFY(!bar());
        QVERIFY(offer->isHidden());
        QTRY_COMPARE(w->toastText(), QStringLiteral("Lumaflip ist jetzt die Standard-App für „JPEG-Bild“"));
        QCOMPARE(defaultapp::askedTypes(), QStringList{QStringLiteral("public.jpeg")});

        // The next JPEG of the folder: no bar, not even a second query this session.
        const int queries = shell->queries.load();
        QVERIFY(w->perform(Action::NextImage));
        QTRY_COMPARE(w->shownPath(), b);
        QTest::qWait(300);
        QVERIFY(!bar());
        QCOMPARE(shell->queries.load(), queries);

        // After a restart the type is still answered: asked again, but no bar.
        app.reset();
        startApp();
        const int before = shell->queries.load();
        QVERIFY(open(a));
        QVERIFY(waitForQueries(before + 1));
        QVERIFY(!bar());
    }

    // Covers: F-MAC-09 (failure shown as a toast; the type counts as asked)
    void F_MAC_09_failureIsShownAsToast() {
        shell->fail = true;
        ViewerWindow* w = open(image(QStringLiteral("a.png")));
        QVERIFY(w);
        QTRY_VERIFY_WITH_TIMEOUT(bar(), 10000);
        bar()->findChild<QPushButton*>(QStringLiteral("lfDefaultAppSet"))->click();
        QTRY_VERIFY(w->toastText().contains(QStringLiteral("Systemfehler 42")));
        QVERIFY(w->toastText().startsWith(
            QStringLiteral("Lumaflip konnte nicht als Standard-App festgelegt werden")));
        QCOMPARE(defaultapp::askedTypes(), QStringList{QStringLiteral("public.png")});
    }

    // Covers: F-MAC-09 (close = "not now" for this type, other types still asked;
    // "Nicht mehr fragen" turns the offer off for good)
    void F_MAC_09_closeAndNeverAsk() {
        ViewerWindow* w = open(image(QStringLiteral("a.png")));
        QVERIFY(w);
        QTRY_VERIFY_WITH_TIMEOUT(bar(), 10000);
        QCOMPARE(bar()->type(), QStringLiteral("public.png"));
        bar()->findChild<QToolButton*>(QStringLiteral("lfDefaultAppClose"))->click();
        QVERIFY(!bar());
        QCOMPARE(defaultapp::askedTypes(), QStringList{QStringLiteral("public.png")});
        QVERIFY(defaultapp::isEnabled());

        // Another type in the same window: asked.
        const QString jpg = image(QStringLiteral("b.jpg"));
        w->openFile(jpg);
        QTRY_VERIFY_WITH_TIMEOUT(bar(), 10000);
        QCOMPARE(bar()->type(), QStringLiteral("public.jpeg"));
        bar()->findChild<QPushButton*>(QStringLiteral("lfDefaultAppNever"))->click();
        QVERIFY(!bar());
        QVERIFY(!defaultapp::isEnabled());
        QVERIFY(!Settings::instance().value(QString::fromLatin1(key::OfferDefaultApp)).toBool());
        QVERIFY(!w->toastText().isEmpty());
        QVERIFY(shell->made().isEmpty()); // nothing was set

        // A third type: not even asked any more.
        const int queries = shell->queries.load();
        w->openFile(image(QStringLiteral("c.gif")));
        QTest::qWait(400);
        QVERIFY(!bar());
        QCOMPARE(shell->queries.load(), queries);
    }

    // Covers: F-MAC-09 ("Alle Bildformate …" opens the F-MAC-04 checklist)
    void F_MAC_09_allFormatsOpensTheChecklist() {
        ViewerWindow* w = open(image(QStringLiteral("a.jpg")));
        QVERIFY(w);
        QTRY_VERIFY_WITH_TIMEOUT(bar(), 10000);
        bar()->findChild<QPushButton*>(QStringLiteral("lfDefaultAppAll"))->click();
        QVERIFY(!bar());
        DefaultAppDialog* dlg = nullptr;
        QTRY_VERIFY([&] {
            for (QWidget* top : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<DefaultAppDialog*>(top); d && d->isVisible())
                    dlg = d;
            return dlg != nullptr;
        }());
        QCOMPARE(defaultapp::askedTypes(), QStringList{QStringLiteral("public.jpeg")});
        dlg->reject();
        QVERIFY(shell->made().isEmpty());
    }

    // Covers: F-MAC-09 (not the installed app, unknown answer, preference off: no bar)
    void F_MAC_09_noBarWhenNotInstalledOrUnknown() {
        shell->installed = false;
        ViewerWindow* w = open(image(QStringLiteral("a.jpg")));
        QVERIFY(w);
        QVERIFY(waitForQueries(1));
        QVERIFY(!bar());

        shell->installed = true;
        shell->bySuffix[QStringLiteral("png")].state = State::Unknown; // e.g. LaunchServices failed
        w->openFile(image(QStringLiteral("b.png")));
        QVERIFY(waitForQueries(2));
        QVERIFY(!bar());

        Settings::instance().setValue(QString::fromLatin1(key::OfferDefaultApp), false); // preference
        const int queries = shell->queries.load();
        w->openFile(image(QStringLiteral("c.gif")));
        QTest::qWait(400);
        QVERIFY(!bar());
        QCOMPARE(shell->queries.load(), queries);
        QVERIFY(defaultapp::askedTypes().isEmpty()); // nothing was answered
    }

    // Covers: F-MAC-09 (the environment switch of the test harness turns it off)
    void F_MAC_09_environmentSwitchTurnsItOff() {
        qputenv(defaultapp::kDisableEnv, "1");
        ViewerWindow* w = open(image(QStringLiteral("a.jpg")));
        QVERIFY(w);
        QTest::qWait(400);
        QVERIFY(!bar());
        QCOMPARE(shell->queries.load(), 0);
        qunsetenv(defaultapp::kDisableEnv);
    }
};

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    // The canvas needs Metal; the offscreen platform has no QRhi.
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen")
        qputenv("QT_QPA_PLATFORM", "cocoa");
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LumaflipTest"));
    TstDefaultAppOffer tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_default_app_offer.moc"
