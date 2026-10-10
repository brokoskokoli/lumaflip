// SPDX-License-Identifier: GPL-3.0-or-later
// Regression tests for the third review round: launch and session restore, open bursts,
// file operations at shutdown and while the undo stack changes, Dock reopen, the default
// app registration, restored multi-file windows, the busy message, per-folder sorting,
// unfinished features in the shortcut overview and preferences, folder snapshots on
// volumes without change notifications. Real windows like tst_app (cocoa on macOS).

#include "app/Application.h"
#include "app/DockReopen.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/FolderCache.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include <atomic>
#include <memory>
#ifdef Q_OS_UNIX
#include <sys/time.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <utime.h>
#endif

using namespace lf;

namespace {

// Trash in a private folder (not the user's trash) that can be slowed down like a
// network volume.
class RecordingTrash : public TrashService {
public:
    RecordingTrash() { QDir().mkpath(m_dir.path()); }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QThread::msleep(ulong(delayMs.load()));
        if (QSemaphore* g = gate.load()) // deterministic: blocks until the test releases it
            g->acquire();
        const QString target =
            m_dir.filePath(QStringLiteral("%1-%2").arg(++m_counter).arg(QFileInfo(path).fileName()));
        if (!QFileInfo::exists(path) || !QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("„%1“ konnte nicht in den Papierkorb gelegt werden.")
                             .arg(QFileInfo(path).fileName());
            return std::nullopt;
        }
        return target;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        QThread::msleep(ulong(delayMs.load()));
        if (QFileInfo::exists(original) || !QFile::rename(trashed, original)) {
            if (error)
                *error = QStringLiteral("„%1“ konnte nicht wiederhergestellt werden.")
                             .arg(QFileInfo(original).fileName());
            return false;
        }
        return true;
    }
    std::atomic<int> delayMs{0};
    std::atomic<QSemaphore*> gate{nullptr};

private:
    QTemporaryDir m_dir;
    std::atomic<int> m_counter{0};
};

// Never touches the system's default apps; the blocking call can be slowed down.
class FakeShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override {
        QThread::msleep(ulong(delayMs.load()));
        ++makeDefaultCalls;
        return true;
    }
    std::atomic<int> makeDefaultCalls{0};
    std::atomic<int> delayMs{0};
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

// Moves the modification time of a folder (a change the folder watcher did not report).
bool touchFolder(const QString& folder, int secondsAhead) {
#ifdef Q_OS_UNIX
    struct timeval times[2];
    gettimeofday(&times[0], nullptr);
    times[0].tv_sec += secondsAhead;
    times[1] = times[0];
    return ::utimes(QFile::encodeName(folder).constData(), times) == 0;
#elif defined(Q_OS_WIN)
    // SetFileTime on the folder itself (QFile cannot open folders on Windows).
    const QString native = QDir::toNativeSeparators(folder);
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t{};
    t.LowPart = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart += quint64(qint64(secondsAhead) * 10000000);
    FILETIME ft{t.LowPart, t.HighPart};
    const bool ok = SetFileTime(h, nullptr, &ft, &ft) != 0;
    CloseHandle(h);
    return ok;
#else
    Q_UNUSED(folder);
    Q_UNUSED(secondsAhead);
    return false;
#endif
}

} // namespace

class TstReview3 : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<RecordingTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<FakeShell> shell;
    int quitCalls = 0;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }

    void makeStandardFolder() {
        QVERIFY(!makeImage(path("IMG_10.jpg"), Qt::blue).isEmpty());
        QVERIFY(!makeImage(path("IMG_2.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("IMG_1.png"), Qt::red).isEmpty());
    }
    void makeApp() {
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        services.activeWindow = []() -> QWidget* { return nullptr; };
        services.quitApp = [this] { ++quitCalls; };
        app = std::make_unique<Application>(services);
    }
    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return w && QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 5000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    QStringList names(const Collection* c) const {
        QStringList out;
        for (const auto& e : c->entries())
            out << e.name;
        return out;
    }
    // Saves a session with the window(s) and starts a new app instance (not started yet).
    void saveSessionAndRestart() {
        app->saveSession();
        QVERIFY(app->hasSession());
        app.reset();
        makeApp();
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
        Settings::setSystemRestoreWindowsForTesting(-1);
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        trash = std::make_unique<RecordingTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<FakeShell>();
        quitCalls = 0;
        makeApp();
    }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        app.reset();
        QTest::qWait(10);
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
        Settings::setSystemRestoreWindowsForTesting(-1);
    }

    // ---- F-WIN-06: launch, restore and open requests (review items 1, 2, B1) ----

    // Covers: F-WIN-06 (the default follows "Fenster beim Beenden schließen")
    void F_WIN_06_restoreDefaultFollowsSystemSetting() {
        Settings::setSystemRestoreWindowsForTesting(0); // macOS default: windows close
        QVERIFY(!Settings::instance().value(key::RestoreWindows).toBool());
        QVERIFY(!Settings::instance().defaultValue(key::RestoreWindows).toBool());
        Settings::setSystemRestoreWindowsForTesting(1);
        QVERIFY(Settings::instance().value(key::RestoreWindows).toBool());
        Settings::instance().setValue(key::RestoreWindows, false); // the user's choice wins
        QVERIFY(!Settings::instance().value(key::RestoreWindows).toBool());
        Settings::setSystemRestoreWindowsForTesting(-1);
        Settings::instance().resetToDefaults(key::RestoreWindows);
        QCOMPARE(Settings::instance().value(key::RestoreWindows).toBool(), systemRestoresWindows());
    }

    // Covers: F-WIN-06 (a plain launch restores the windows)
    void F_WIN_06_plainLaunchRestoresSession() {
        makeStandardFolder();
        Settings::setSystemRestoreWindowsForTesting(1);
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        saveSessionAndRestart();
        QSignalSpy restored(app.get(), &Application::sessionRestored);
        app->start({QStringLiteral("Lumaflip")});
        QVERIFY(app->isRestorePending());
        QTRY_COMPARE(restored.size(), 1);
        QCOMPARE(app->windows().size(), 1);
        QCOMPARE(app->frontWindow()->currentPath(), path("IMG_2.jpg"));
    }

    // Covers: F-WIN-06, P-01 (launched by an open request: no restore, the opened image is in front)
    void F_WIN_06_openRequestAtLaunchSkipsRestore() {
        makeStandardFolder();
        QVERIFY(QDir().mkpath(path("other")));
        QVERIFY(!makeImage(path("other/x.png"), Qt::red).isEmpty());
        Settings::instance().setValue(key::RestoreWindows, true);
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        saveSessionAndRestart();
        QSignalSpy restored(app.get(), &Application::sessionRestored);
        app->start({QStringLiteral("Lumaflip")});
        app->queueOpen(path("other/x.png")); // the Finder double click that launched the app
        QTest::qWait(Application::kEmptyStartDelayMs + 400);
        QCOMPARE(restored.size(), 0);
        QVERIFY(!app->isRestorePending());
        QCOMPARE(app->windows().size(), 1);
        QCOMPARE(app->frontWindow()->currentPath(), path("other/x.png"));
        // The saved session is still there until the next save.
        QVERIFY(app->hasSession());
    }

    // Covers: F-WIN-06 (⌘Q before the restore finished keeps the saved session)
    void F_WIN_06_saveDuringPendingRestoreKeepsSession() {
        makeStandardFolder();
        Settings::instance().setValue(key::RestoreWindows, true);
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        saveSessionAndRestart();
        app->start({QStringLiteral("Lumaflip")});
        QVERIFY(app->isRestorePending());
        app->saveSession(); // e.g. ⌘Q or logout right after the launch
        QVERIFY(app->hasSession());
        app->quit();
        QVERIFY(app->hasSession());
        QCOMPARE(quitCalls, 1);
    }

    // Covers: F-WIN-06 (a restored multi-file window with one file left stays a multi-file window)
    void F_WIN_06_restoredFileSetWithOneFileStaysFileSet() {
        makeStandardFolder();
        Settings::instance().setValue(key::RestoreWindows, true);
        ViewerWindow* a = app->openPaths({path("IMG_10.jpg"), path("IMG_1.png")});
        QVERIFY(waitLoaded(a));
        QVERIFY(!a->collection()->isFolderCollection());
        saveSessionAndRestart();
        QVERIFY(QFile::remove(path("IMG_1.png")));
        QSignalSpy restored(app.get(), &Application::sessionRestored);
        app->start({QStringLiteral("Lumaflip")});
        QTRY_COMPARE(restored.size(), 1);
        QCOMPARE(app->windows().size(), 1);
        ViewerWindow* r = app->windows().first();
        QVERIFY(waitLoaded(r));
        QVERIFY(!r->collection()->isFolderCollection()); // not the whole folder (3 images)
        QCOMPARE(names(r->collection()), QStringList{"IMG_10.jpg"});
    }

    // ---- F-OPEN-04: open bursts (review item 3) ----

    // Covers: F-OPEN-04 (a burst whose first path has no image suffix opens every file once)
    void F_OPEN_04_burstWithNonImageFirstOpensEachOnce() {
        makeStandardFolder();
        QVERIFY(QFile::copy(path("IMG_2.jpg"), path("photo")));
        app->queueOpen(path("photo"));
        app->queueOpen(path("IMG_10.jpg"));
        QTRY_VERIFY(!app->windows().isEmpty());
        QTest::qWait(Application::kOpenGroupingMs + 300);
        int withPhoto = 0;
        ViewerWindow* w = nullptr;
        for (ViewerWindow* x : app->windows())
            if (x->collection()->indexOf(path("photo")) >= 0) {
                ++withPhoto;
                w = x;
            }
        QCOMPARE(withPhoto, 1);
        QCOMPARE(app->windows().size(), 1);
        QVERIFY(waitLoaded(w));
        QVERIFY(!w->collection()->isFolderCollection()); // exactly the two files
        QCOMPARE(w->collection()->count(), 2);
    }

    // ---- F-NAV-03: a newer open wins over a pending folder switch (review item 4) ----

    // Covers: F-NAV-03, F-IN-16 (a pending "next folder" lookup does not override a newer open)
    void F_NAV_03_newerOpenWinsOverPendingFolderSwitch() {
        QVERIFY(QDir().mkpath(path("A")) && QDir().mkpath(path("B")) && QDir().mkpath(path("X/sub")));
        QVERIFY(!makeImage(path("A/a1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("B/b1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("X/sub/x1.png"), Qt::red).isEmpty());
        ViewerWindow* w = app->openPaths({path("A/a1.png")});
        QVERIFY(waitLoaded(w));
        w->perform(Action::NextFolder);    // looked up on a worker
        w->openFile(path("X/sub/x1.png")); // the user drops another file right away
        QTest::qWait(600);
        QCOMPARE(w->collection()->folderPath(), path("X/sub"));
        // The same for the other open paths: openFolder, openFiles, drop (openPaths).
        w->perform(Action::NextFolder);
        w->openFolder(path("A"));
        QTest::qWait(600);
        QCOMPARE(w->collection()->folderPath(), path("A"));
        w->perform(Action::NextFolder);
        w->openPaths({path("X/sub")});
        QTest::qWait(600);
        QCOMPARE(w->collection()->folderPath(), path("X/sub"));
    }

    // ---- F-FILE-01: file operations (review items 6, 7, 15) ----

    // Covers: F-FILE-01 (synchronous operations are refused while asynchronous ones run)
    void F_FILE_01_syncRefusedWhileAsyncUndoRuns() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(w));
        QString err;
        QVERIFY(ops->moveToTrash(path("IMG_2.jpg"), &err));
        QTRY_VERIFY(w->collection()->indexOf(path("IMG_2.jpg")) < 0);
        trash->delayMs = 300;
        ops->undo(); // the restore runs on a worker
        QVERIFY(ops->isBusy());
        QVERIFY(!ops->moveToTrash(path("IMG_10.jpg"), &err)); // must not interleave
        QVERIFY(err.contains(QStringLiteral("Dateivorgang")));
        QVERIFY(QFileInfo::exists(path("IMG_10.jpg")));
        QTRY_VERIFY(!ops->isBusy());
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_VERIFY(w->collection()->indexOf(path("IMG_2.jpg")) >= 0);
        QCOMPARE(ops->undoStack()->index(), 0);
        QCOMPARE(ops->undoStack()->count(), 1);
    }

    // Covers: F-FILE-01 (the undo stack changes during asynchronous work: no dangling command, windows
    // updated)
    void F_FILE_01_stackChangedDuringAsyncUndo() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(w));
        QString err;
        QVERIFY(ops->moveToTrash(path("IMG_2.jpg"), &err));
        QTRY_VERIFY(w->collection()->indexOf(path("IMG_2.jpg")) < 0);
        QSignalSpy failed(ops.get(), &FileOps::operationFailed);
        trash->delayMs = 300;
        ops->undo();
        QTest::qWait(50);
        ops->undoStack()->clear(); // deletes the command while its work runs
        QTRY_VERIFY(!ops->isBusy());
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));                 // the work was done
        QTRY_VERIFY(w->collection()->indexOf(path("IMG_2.jpg")) >= 0); // and reported
        QCOMPARE(failed.size(), 1);                                    // history reset noted
        QCOMPARE(ops->undoStack()->count(), 0);
    }

    // Covers: F-FILE-01 (⌘Q waits for queued file operations with a busy message, never drops them)
    void F_FILE_01_quitWaitsForQueuedFileOps() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_1.png")});
        QVERIFY(waitLoaded(w));
        trash->delayMs = 300;
        ops->moveToTrashAsync(path("IMG_2.jpg"));
        ops->moveToTrashAsync(path("IMG_10.jpg"));
        app->quit();
        QVERIFY(app->isQuitPending());
        QCOMPARE(quitCalls, 0);
        QVERIFY(w->toastText().contains(QStringLiteral("beendet")));
        QTRY_COMPARE_WITH_TIMEOUT(quitCalls, 1, 5000);
        QVERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QVERIFY(!QFileInfo::exists(path("IMG_10.jpg")));
    }

    // Covers: F-FILE-01 (operations still queued when FileOps goes away are carried out)
    void F_FILE_01_queuedOpsNotDroppedOnDestruction() {
        makeStandardFolder();
        trash->delayMs = 200;
        ops->moveToTrashAsync(path("IMG_2.jpg"));
        ops->moveToTrashAsync(path("IMG_10.jpg"));
        QTest::qWait(20);
        app.reset();
        ops.reset(); // the second one is still queued here
        QVERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QVERIFY(!QFileInfo::exists(path("IMG_10.jpg")));
        ops = std::make_unique<FileOps>(*trash);
    }

    // Covers: F-FILE-01 (the busy message disappears from the window that showed it)
    void F_FILE_01_busyMessageHiddenWhereShown() {
        makeStandardFolder();
        QVERIFY(QDir().mkpath(path("B")));
        QVERIFY(!makeImage(path("B/b.png"), Qt::red).isEmpty());
        ViewerWindow* a = app->openPaths({path("IMG_1.png")});
        QVERIFY(waitLoaded(a));
        ViewerWindow* b = app->openPaths({path("B/b.png")});
        QVERIFY(waitLoaded(b) && a != b);
        app->windowActivated(a);
        // The operation runs until the test lets it finish (no timing assumptions), and the
        // busy hint comes at once instead of after its delay.
        QSemaphore gate;
        trash->gate = &gate;
        // Whatever happens below, the worker is never left blocked (the test would hang).
        const auto releaseGate = qScopeGuard([&] {
            trash->gate = nullptr;
            gate.release(100);
        });
        app->setBusyHintDelayForTesting(0);
        // The busy message goes to the front window - normally `a`, but the system may still
        // be activating `b` (opened last) on a loaded machine. Recorded as shown, since
        // another message may replace the toast meanwhile.
        QSignalSpy messagesA(a, &ViewerWindow::messageShown);
        QSignalSpy messagesB(b, &ViewerWindow::messageShown);
        auto busyIn = [](const QSignalSpy& spy) {
            for (const QList<QVariant>& m : spy)
                if (m.first().toString().contains(QStringLiteral("Dateivorgang")))
                    return true;
            return false;
        };
        ops->moveToTrashAsync(path("IMG_2.jpg"));
        QTRY_VERIFY_WITH_TIMEOUT(busyIn(messagesA) || busyIn(messagesB), 10000);
        ViewerWindow* shownIn = busyIn(messagesA) ? a : b;
        ViewerWindow* other = shownIn == a ? b : a;
        QVERIFY(ops->isBusy());
        app->windowActivated(other); // another window comes to the front meanwhile
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(!ops->isBusy(), 10000);
        // The busy message is gone from the window that showed it.
        QVERIFY2(!shownIn->toastText().contains(QStringLiteral("Dateivorgang")),
                 qPrintable(shownIn->toastText()));
    }

    // ---- F-OPEN-12 / Dock (review item 10) ----

    // Covers: F-OPEN-12 (only a real Dock reopen opens an empty window, not every activation)
    void F_OPEN_12_dockReopenOnlyOnRealReopen() {
        app->start({QStringLiteral("Lumaflip")});
        QTRY_COMPARE(app->windows().size(), 1);
        app->closeAllWindows();
        QCOMPARE(app->windows().size(), 0);
        // ⌘-Tab back / activation: Qt reports a (forced) active state; no window appears.
        QWindowSystemInterface::handleApplicationStateChanged<QWindowSystemInterface::SynchronousDelivery>(
            Qt::ApplicationActive, true);
        QTest::qWait(100);
        QCOMPARE(app->windows().size(), 0);
        // The Dock icon: AppKit asks the application delegate.
        if (!simulateDockReopenForTesting())
            app->handleDockReopen(); // no Cocoa delegate (other platform plugin)
        QTRY_COMPARE(app->windows().size(), 1);
        QVERIFY(app->windows().first()->isEmpty());
    }

    // ---- F-MAC-04: default app registration (review item 11) ----

    // Covers: F-MAC-04 (the shell outlives its worker: the app waits before it goes away)
    void F_MAC_04_shellOutlivesDefaultViewerWorker() {
        shell->delayMs = 300;
        app->setDefaultViewer();
        QTest::qWait(20);
        app.reset(); // waits for the worker that uses the shell
        QCOMPARE(shell->makeDefaultCalls.load(), 1);
        shell.reset(); // safe now
        shell = std::make_unique<FakeShell>();
    }

    // ---- F-SET-02: shortcut overview (B4) ----

    // Covers: F-SET-02, SPEC 5.2 (the shortcut overview marks planned actions instead of listing them as
    // usable)
    void F_SET_02_shortcutOverviewMarksPlannedActions() {
        app->showShortcuts();
        auto* table = qApp->findChild<QTableWidget*>(QStringLiteral("lfShortcutsTable"));
        if (!table)
            for (QWidget* w : QApplication::topLevelWidgets())
                if ((table = w->findChild<QTableWidget*>(QStringLiteral("lfShortcutsTable"))))
                    break;
        QVERIFY(table);
        auto row = [&](Action a) {
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0)->text().startsWith(actionLabel(a)))
                    return r;
            return -1;
        };
        // M3: the grid (F-GRID-02) and the magnifier (F-VIEW-11) were the last planned actions.
        for (Action a : {Action::ToggleGrid, Action::Magnifier, Action::ZoomFit}) {
            const int r = row(a);
            QVERIFY2(r >= 0, qPrintable(actionLabel(a)));
            QVERIFY(!table->item(r, 0)->text().contains(QStringLiteral("späteren Version")));
            QVERIFY(table->item(r, 0)->flags() & Qt::ItemIsEnabled);
        }
        // M2: the slideshow is available now (F-SLIDE-01).
        const int show = row(Action::ToggleSlideshow);
        QVERIFY(show >= 0);
        QVERIFY(!table->item(show, 0)->text().contains(QStringLiteral("späteren Version")));
        table->window()->close();
    }

    // ---- F-FMT-03: the formats list tells whether animations play (B11) ----

    // Covers: F-FMT-03, F-SET-01 (the formats list offers animation playback since M2)
    void F_FMT_03_formatsListMarksPlayback() {
        app->showPreferences();
        PreferencesDialog* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs);
        auto* list = prefs->findChild<QTableWidget*>(QStringLiteral("formatList"));
        QVERIFY(list);
        bool found = false;
        for (int r = 0; r < list->rowCount(); ++r) {
            if (list->item(r, 0)->text() == QStringLiteral("Animiert")) {
                found = true;
                QVERIFY(!list->item(r, 1)->text().contains(QStringLiteral("in einer späteren Version")));
                QVERIFY(list->item(r, 1)->text().contains(QStringLiteral("Wiedergabe")));
            }
        }
        QVERIFY(found);
    }

    // ---- F-SORT-06: manual sort choice per folder (B5) ----

    // Covers: F-SORT-06 (a sort chosen in the menu is remembered for that folder only)
    void F_SORT_06_manualSortRememberedPerFolder() {
        QVERIFY(QDir().mkpath(path("A")) && QDir().mkpath(path("B")));
        QVERIFY(!makeImage(path("A/a1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("A/a2.png"), Qt::red, QSize(200, 200)).isEmpty());
        QVERIFY(!makeImage(path("B/b1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("B/b2.png"), Qt::red, QSize(200, 200)).isEmpty());
        Settings::instance().setValue(key::FollowFinder, false); // global: name ascending
        app->openPaths({path("A")}); // a folder: opened once it is checked on a worker
        QTRY_VERIFY(!app->windows().isEmpty());
        ViewerWindow* w = app->windows().first();
        QVERIFY(waitLoaded(w));
        QCOMPARE(names(w->collection()), (QStringList{"a1.png", "a2.png"}));
        w->setSortKey(SortKey::Size);
        w->setSortAscending(false);
        QVERIFY(waitLoaded(w));
        QCOMPARE(names(w->collection()), (QStringList{"a2.png", "a1.png"}));
        // Another folder in the same window: the global choice.
        w->openFolder(path("B"));
        QVERIFY(waitLoaded(w));
        QCOMPARE(names(w->collection()), (QStringList{"b1.png", "b2.png"}));
        QCOMPARE(w->collection()->sortSpec().key, SortKey::Name);
        // Back to A (also in a new window): its own choice comes back.
        w->openFolder(path("A"));
        QVERIFY(waitLoaded(w));
        QCOMPARE(names(w->collection()), (QStringList{"a2.png", "a1.png"}));
        QCOMPARE(w->collection()->sortSpec().key, SortKey::Size);
        app->windowActivated(w);
        app->menu()->refreshState();
        QVERIFY(app->menu()->command(QStringLiteral("sort:size"))->isChecked());
        QVERIFY(!app->menu()->command(QStringLiteral("sort:finder"))->isChecked());
    }

    // Covers: F-SORT-06 ("Für alle Ordner verwenden" makes the window's choice the default)
    void F_SORT_06_applyToAllFolders() {
        QVERIFY(QDir().mkpath(path("A")) && QDir().mkpath(path("B")));
        QVERIFY(!makeImage(path("A/a1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("A/a2.png"), Qt::red, QSize(200, 200)).isEmpty());
        QVERIFY(!makeImage(path("B/b1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("B/b2.png"), Qt::red, QSize(200, 200)).isEmpty());
        Settings::instance().setValue(key::FollowFinder, false);
        app->openPaths({path("A")});
        QTRY_VERIFY(!app->windows().isEmpty());
        ViewerWindow* w = app->windows().first();
        QVERIFY(waitLoaded(w));
        w->setSortKey(SortKey::Size);
        w->setSortAscending(false);
        QVERIFY(waitLoaded(w));
        app->windowActivated(w);
        app->menu()->command(QStringLiteral("sort:applyToAll"))->trigger();
        QCOMPARE(Settings::instance().value(key::ManualSortKey).toString(), QStringLiteral("size"));
        QVERIFY(!Settings::instance().value(key::ManualSortAscending).toBool());
        QVERIFY(!Settings::instance().value(key::FollowFinder).toBool());
        w->openFolder(path("B"));
        QVERIFY(waitLoaded(w));
        QCOMPARE(names(w->collection()), (QStringList{"b2.png", "b1.png"}));
    }

    // ---- F-WIN-09 / B12: folder snapshots without watcher events ----

    // Covers: F-WIN-09, F-OPEN-07 (an explicit open notices changes the folder watcher did not report)
    void F_WIN_09_explicitOpenRevalidatesSharedSnapshot() {
        makeStandardFolder();
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        QCOMPARE(a->collection()->count(), 3);
        // A change on a volume without FSEvents: no watcher event, the snapshot stays.
        QVERIFY(!makeImage(path("IMG_3.jpg"), Qt::yellow).isEmpty());
        QVERIFY(touchFolder(dir->path(), 5));
        ViewerWindow* b = app->openPaths({path("IMG_10.jpg")}); // copies a's entries at once
        QVERIFY(b && b != a);
        QTRY_COMPARE(b->collection()->count(), 4);
        QTRY_COMPARE(a->collection()->count(), 4); // the other window gets it too
        QCOMPARE(b->currentPath(), path("IMG_10.jpg"));
    }
};

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen")
        qputenv("QT_QPA_PLATFORM", "cocoa");
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LumaflipTest"));
    TstReview3 tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_review3.moc"
