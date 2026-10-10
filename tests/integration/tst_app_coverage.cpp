// SPDX-License-Identifier: GPL-3.0-or-later
// Application-level tests for requirements that had no test of their own
// (SPEC 15.1 traceability): F-WIN-03 (⌘N/⌥⌘N/⌘W/⌥⌘W), F-MAC-03 (any number of
// independent windows), F-PRIV-02 (never asks for Full Disk Access) and the
// "UI is not blocked" half of P-05 (the timing half is the benchmark
// P_05_EnumerateAndSort10k). Real windows like tst_app: cocoa on macOS.

#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lf;

namespace {

class FakeShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

// Every user-visible text of a widget tree (labels, buttons, tooltips, window titles).
QStringList visibleTexts(const QWidget* root) {
    QStringList out{root->windowTitle(), root->toolTip()};
    for (const QLabel* l : root->findChildren<QLabel*>())
        out << l->text() << l->toolTip();
    for (const QAbstractButton* b : root->findChildren<QAbstractButton*>())
        out << b->text() << b->toolTip();
    return out;
}

// "Festplattenvollzugriff" may only be *explained* (SPEC 10.2), i.e. appear in
// a sentence saying it is not needed / optional.
bool demandsFullDiskAccess(const QString& text) {
    if (!text.contains(QStringLiteral("Festplattenvollzugriff"), Qt::CaseInsensitive) &&
        !text.contains(QStringLiteral("Full Disk Access"), Qt::CaseInsensitive))
        return false;
    return !text.contains(QStringLiteral("nicht nötig")) &&
           !text.contains(QStringLiteral("optional"), Qt::CaseInsensitive);
}

} // namespace

class TstAppCoverage : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<FakeShell> shell;
    QList<QUrl> openedUrls;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }

    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 5000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    static bool waitExposed(ViewerWindow* w) { return QTest::qWaitForWindowExposed(w, 5000); }

    // Triggers a menu command the way its ⌘ shortcut does, after checking the shortcut.
    void menuShortcut(Action a, const QKeySequence& seq) {
        QAction* act = app->menu()->action(a);
        if (!act)
            QFAIL(qPrintable(actionId(a)));
        QVERIFY2(act->shortcuts().contains(seq), qPrintable(actionId(a) + " " + seq.toString()));
        QVERIFY(act->isEnabled());
        act->trigger();
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        Settings::useTestingBackend(m_settingsDir.filePath(QStringLiteral("init.ini")));
        ImageView probe;
        probe.resize(64, 64);
        probe.show();
        QVERIFY(QTest::qWaitForWindowExposed(&probe));
        auto* canvas = probe.findChild<ImageCanvas*>();
        QVERIFY(canvas);
        if (canvas->grabFramebuffer().isNull() || !canvas->isRhiAvailable())
            QSKIP("No QRhi (GPU) available on this platform");
    }

    void init() {
        Settings::useTestingBackend(
            m_settingsDir.filePath(QStringLiteral("s%1.ini").arg(++m_settingsCounter)));
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        shell = std::make_unique<FakeShell>();
        openedUrls.clear();
        AppServices services;
        services.shell = shell.get();
        services.openUrl = [this](const QUrl& u) {
            openedUrls << u; // never opens System Settings during tests
            return true;
        };
        app = std::make_unique<Application>(services);
    }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        app.reset();
        QTest::qWait(10); // deferred deletes
        shell.reset();
        dir.reset();
    }

    // F-WIN-03: ⌘N opens a new empty window, ⌥⌘N duplicates the current window with
    // the same folder, image and zoom, ⌘W closes the window, ⌥⌘W all windows.
    void F_WIN_03_newDuplicateAndCloseWindows() {
        QVERIFY(!makeImage(path("a.png"), Qt::red, QSize(800, 600)).isEmpty());
        QVERIFY(!makeImage(path("b.png"), Qt::green, QSize(800, 600)).isEmpty());
        QVERIFY(!makeImage(path("c.png"), Qt::blue, QSize(800, 600)).isEmpty());

        ViewerWindow* src = app->openPaths({path("b.png")});
        QVERIFY(src);
        QVERIFY(waitExposed(src));
        QVERIFY(waitLoaded(src));
        QVERIFY(waitShown(src, path("b.png")));

        // ⌘N: an additional, empty window.
        menuShortcut(Action::NewWindow, QKeySequence(Qt::ControlModifier | Qt::Key_N));
        QCOMPARE(app->windows().size(), 2);
        ViewerWindow* empty = app->frontWindow();
        QVERIFY(empty != src);
        QVERIFY(empty->isEmpty());
        QVERIFY(waitExposed(empty));
        QCOMPARE(empty->collection()->count(), 0);
        QCOMPARE(src->currentPath(), path("b.png"));

        // A custom zoom factor (not just a mode) must survive the duplication.
        src->view()->setZoomFactor(2.5, QPointF(src->view()->width() / 2.0, src->view()->height() / 2.0));
        QCOMPARE(src->view()->zoomMode(), ZoomMode::Custom);
        app->windowActivated(src);

        // ⌥⌘N
        menuShortcut(Action::DuplicateWindow,
                     QKeySequence(Qt::AltModifier | Qt::ControlModifier | Qt::Key_N));
        QCOMPARE(app->windows().size(), 3);
        ViewerWindow* dup = app->frontWindow();
        QVERIFY(dup != src && dup != empty);
        QVERIFY(waitExposed(dup));
        QVERIFY(!dup->isEmpty());
        QCOMPARE(dup->collection()->folderPath(), src->collection()->folderPath());
        QCOMPARE(dup->collection()->count(), 3);
        QCOMPARE(dup->currentPath(), path("b.png"));
        QVERIFY(waitShown(dup, path("b.png")));
        QCOMPARE(dup->view()->zoomMode(), ZoomMode::Custom);
        QVERIFY(qAbs(dup->view()->zoomFactor() - 2.5) < 1e-6);
        // The duplicate is a separate window: navigating it leaves the source alone.
        QTest::keyClick(dup->view(), Qt::Key_Right);
        QCOMPARE(dup->currentPath(), path("c.png"));
        QCOMPARE(src->currentPath(), path("b.png"));

        // ⌘W closes only the front window.
        app->windowActivated(dup);
        menuShortcut(Action::CloseWindow, QKeySequence(Qt::ControlModifier | Qt::Key_W));
        QCOMPARE(app->windows().size(), 2);
        QVERIFY(!app->windows().contains(dup));
        QVERIFY(app->windows().contains(src) && app->windows().contains(empty));

        // ⌥⌘W closes all windows.
        menuShortcut(Action::CloseAllWindows,
                     QKeySequence(Qt::AltModifier | Qt::ControlModifier | Qt::Key_W));
        QCOMPARE(app->windows().size(), 0);
    }

    // F-MAC-03 (-> 4.15): any number of windows, each with its own collection,
    // current image and zoom; closing some leaves the others intact.
    void F_MAC_03_manyIndependentWindows() {
        constexpr int kWindows = 8;
        QList<ViewerWindow*> wins;
        QStringList folders;
        for (int i = 0; i < kWindows; ++i) {
            const QString folder = path(QStringLiteral("folder%1").arg(i));
            QVERIFY(QDir().mkpath(folder));
            for (int j = 0; j <= i % 3 + 1; ++j)
                QVERIFY(!makeImage(folder + QStringLiteral("/img%1.png").arg(j),
                                   QColor::fromHsv(i * 40, 200, 200))
                             .isEmpty());
            folders << folder;
            ViewerWindow* w = app->openPaths({folder + QStringLiteral("/img0.png")});
            QVERIFY(w);
            QVERIFY(!wins.contains(w));
            wins << w;
        }
        QCOMPARE(app->windows().size(), kWindows);
        QSet<QPoint> positions;
        for (int i = 0; i < kWindows; ++i) {
            ViewerWindow* w = wins[i];
            QVERIFY(waitExposed(w));
            QVERIFY(waitLoaded(w));
            QCOMPARE(w->collection()->folderPath(), folders[i]);
            QCOMPARE(w->collection()->count(), i % 3 + 2);
            positions.insert(w->pos());
        }
        QCOMPARE(positions.size(), kWindows); // never exactly on top of each other

        // Change every second window; the others keep their state.
        for (int i = 0; i < kWindows; i += 2) {
            QTest::keyClick(wins[i]->view(), Qt::Key_Right);
            QTest::keyClick(wins[i]->view(), Qt::Key_3); // 100 %
        }
        for (int i = 0; i < kWindows; ++i) {
            const bool changed = i % 2 == 0;
            QCOMPARE(wins[i]->currentPath(),
                     folders[i] + (changed ? QStringLiteral("/img1.png") : QStringLiteral("/img0.png")));
            QCOMPARE(wins[i]->view()->zoomMode(), changed ? ZoomMode::Actual : ZoomMode::Fit);
        }

        // Close windows in arbitrary order; the rest stay usable.
        for (int i : {3, 0, 6}) {
            wins[i]->close();
            QVERIFY(!app->windows().contains(wins[i]));
        }
        QTest::qWait(10);
        QCOMPARE(app->windows().size(), kWindows - 3);
        ViewerWindow* last = wins[7];
        QTest::keyClick(last->view(), Qt::Key_Right);
        QCOMPARE(last->currentPath(), folders[7] + QStringLiteral("/img1.png"));
        QVERIFY(waitShown(last, folders[7] + QStringLiteral("/img1.png")));
    }

    // F-PRIV-02: the app never demands Full Disk Access. Opening an image from a
    // folder that cannot be listed (what a TCC denial looks like) shows the hint,
    // which links to "Dateien und Ordner" only; no modal dialog, no other System
    // Settings page, and no UI text asks for Full Disk Access.
    void F_PRIV_02_neverDemandsFullDiskAccess() {
#ifdef Q_OS_UNIX
        if (::geteuid() == 0)
            QSKIP("root ignores permissions");
        QVERIFY(!Application::privacySettingsUrl().toString().contains(QStringLiteral("AllFiles")));
        QVERIFY(
            Application::privacySettingsUrl().toString().contains(QStringLiteral("Privacy_FilesAndFolders")));

        const QString locked = path("locked");
        QVERIFY(QDir().mkpath(locked));
        QVERIFY(!makeImage(locked + "/a.png", Qt::red).isEmpty());
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0100), 0);
        ViewerWindow* w = app->openPaths({locked + "/a.png"});
        const bool loaded = w && waitLoaded(w);
        const bool shown = w && waitShown(w, locked + "/a.png");
        ::chmod(QFile::encodeName(locked).constData(), 0755);
        if (!w) // not QVERIFY: the static analyzer does not see that it returns
            QFAIL("no window");
        QVERIFY(loaded && shown);
        QVERIFY(waitExposed(w));
        QVERIFY(w->accessHint()->isVisible());

        // Nothing was opened or asked for by itself.
        QVERIFY(openedUrls.isEmpty());
        QCOMPARE(QApplication::activeModalWidget(), nullptr);

        // Texts of the window (incl. the hint) and of all menus.
        for (const QString& t : visibleTexts(w))
            QVERIFY2(!demandsFullDiskAccess(t), qPrintable(t));
        for (QAction* a : app->menu()->findChildren<QAction*>())
            QVERIFY2(!demandsFullDiskAccess(a->text()) && !demandsFullDiskAccess(a->toolTip()),
                     qPrintable(a->text()));
        // Preferences: may explain, never demand.
        app->showPreferences();
        auto* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs);
        for (const QString& t : visibleTexts(prefs))
            QVERIFY2(!demandsFullDiskAccess(t), qPrintable(t));
        prefs->close();

        // Every button of the hint: only the Files & Folders page is ever opened.
        for (QPushButton* b : w->accessHint()->findChildren<QPushButton*>())
            if (b->objectName() != QLatin1String("lfDismissAccess"))
                b->click();
        QVERIFY(!openedUrls.isEmpty());
        for (const QUrl& u : std::as_const(openedUrls)) {
            QCOMPARE(u.scheme(), QStringLiteral("x-apple.systempreferences"));
            QVERIFY2(u.toString().contains(QStringLiteral("Privacy_FilesAndFolders")),
                     qPrintable(u.toString()));
            QVERIFY2(!u.toString().contains(QStringLiteral("AllFiles")), qPrintable(u.toString()));
        }
#else
        QSKIP("needs POSIX permissions");
#endif
    }

    // P-05 "ohne die UI zu blockieren": while a folder with 10 000 files is read and
    // sorted, the GUI event loop keeps running (no gap > kMaxGapMs) and the opened
    // image is shown before / independent of the enumeration.
    void P_05_tenThousandFilesDoNotBlockUi() {
        constexpr int kFiles = 10000;
        // Under sanitizers everything is several times slower; the bound is about
        // blocking (hundreds of ms or more), not about speed.
        const int kMaxGapMs = qEnvironmentVariableIntValue("LF_UI_GAP_LIMIT_MS") > 0
                                  ? qEnvironmentVariableIntValue("LF_UI_GAP_LIMIT_MS")
                                  : 150;
        const QString big = path("big");
        QVERIFY(QDir().mkpath(big));
        for (int i = 0; i < kFiles; ++i) {
            QFile f(big + QStringLiteral("/IMG_%1.jpg").arg(i));
            QVERIFY(f.open(QIODevice::WriteOnly)); // empty: enumeration only, never decoded
        }
        const QString opened = makeImage(big + QStringLiteral("/IMG_5000.png"), Qt::cyan);
        QVERIFY(!opened.isEmpty());

        // Warm-up window elsewhere so window/RHI creation is not measured.
        QVERIFY(!makeImage(path("warm.png"), Qt::gray).isEmpty());
        ViewerWindow* warm = app->openPaths({path("warm.png")});
        QVERIFY(waitExposed(warm));
        QVERIFY(waitShown(warm, path("warm.png")));

        QElapsedTimer clock;
        qint64 last = 0, maxGap = 0;
        QTimer ticker;
        ticker.setTimerType(Qt::PreciseTimer);
        ticker.setInterval(5);
        connect(&ticker, &QTimer::timeout, this, [&] {
            const qint64 now = clock.elapsed();
            maxGap = qMax(maxGap, now - last);
            last = now;
        });
        // Baseline: the gaps of the idle event loop on this machine right now (other
        // processes, parallel test runs). Blocking by the app comes on top of that.
        clock.start();
        ticker.start();
        QTest::qWait(500);
        const qint64 idleGap = maxGap;
        maxGap = 0;
        clock.restart();
        last = 0;
        ViewerWindow* w = app->openPaths({opened});
        const qint64 openCallMs = clock.elapsed();
        QVERIFY(w);
        QVERIFY(QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 20000));
        const qint64 loadMs = clock.elapsed();
        QTest::qWait(50); // the result is applied after isLoading() flips: include it
        ticker.stop();
        maxGap = qMax(maxGap, openCallMs);
        qInfo(
            "P-05: openPaths %lld ms, folder loaded after %lld ms, max event-loop gap %lld ms (idle %lld ms)",
            openCallMs, loadMs, maxGap, idleGap);
        QCOMPARE(w->collection()->count(), kFiles + 1);
        QCOMPARE(w->currentPath(), opened);
        QVERIFY(waitShown(w, opened));
        // On an idle machine the idle gaps stay near the 5 ms tick; when they are longer, the
        // machine is busy and GUI-thread work takes longer by about that factor as well.
        const qint64 limit = qint64(kMaxGapMs * std::max(1.0, double(idleGap) / 10.0)) + idleGap;
        QVERIFY2(maxGap <= limit,
                 qPrintable(QStringLiteral("GUI thread blocked for %1 ms (limit %2 ms, idle gaps %3 ms)")
                                .arg(maxGap)
                                .arg(limit)
                                .arg(idleGap)));
        // Independent of machine speed: reading the folder on the GUI thread would
        // make the longest gap about as long as the whole load. Not with software OpenGL
        // (Linux CI: Mesa llvmpipe), where creating the canvas' GL context and shaders at the
        // first expose is a GUI-thread gap of its own, comparable to this short load.
        if (qEnvironmentVariableIntValue("LIBGL_ALWAYS_SOFTWARE") == 1)
            return;
        QVERIFY2(
            maxGap <= qint64(50 * std::max(1.0, double(idleGap) / 10.0)) + idleGap ||
                maxGap * 10 <= loadMs * 6,
            qPrintable(
                QStringLiteral("GUI thread blocked for %1 of %2 ms of loading").arg(maxGap).arg(loadMs)));
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
    TstAppCoverage tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_app_coverage.moc"
