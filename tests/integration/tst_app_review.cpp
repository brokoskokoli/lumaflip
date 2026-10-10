// SPDX-License-Identifier: GPL-3.0-or-later
// Regression tests for the second review round (acceptance tests and spec review):
// menu commands and the key window, undo/restore bookkeeping, open bursts, the GUI
// thread never waiting for the file system (trash, copy, default app), window
// placement, German texts, unfinished settings, folder snapshots shared by windows,
// window restoration and smaller UX points. Real windows like tst_app (cocoa on macOS).

#include "app/Application.h"
#include "app/DefaultAppDialog.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/Translations.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/FolderCache.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include <atomic>
#include <cmath>
#include <memory>

using namespace lf;

namespace {

// Trash in a private folder (not the user's trash: tst_app uses the real one with the
// same file names in parallel) that can be slowed down like a network volume.
class RecordingTrash : public TrashService {
public:
    RecordingTrash() { QDir().mkpath(m_dir.path()); }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QThread::msleep(ulong(delayMs.load()));
        const QString target =
            m_dir.filePath(QStringLiteral("%1-%2").arg(++m_counter).arg(QFileInfo(path).fileName()));
        if (!QFileInfo::exists(path) || !QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("„%1“ konnte nicht in den Papierkorb gelegt werden.")
                             .arg(QFileInfo(path).fileName());
            return std::nullopt;
        }
        locations << target;
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
    QStringList locations; // written on the file operation worker, read after completion
    std::atomic<int> delayMs{0};

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

QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QStringList names(const Collection* c) {
    QStringList out;
    for (const auto& e : c->entries())
        out << e.name;
    return out;
}

// WCAG contrast ratio of two colors.
double contrast(const QColor& a, const QColor& b) {
    auto lum = [](const QColor& c) {
        auto ch = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * ch(c.redF()) + 0.7152 * ch(c.greenF()) + 0.0722 * ch(c.blueF());
    };
    const double la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QAbstractButton* buttonWithText(QMessageBox* box, const QString& text) {
    for (QAbstractButton* b : box->buttons())
        if (b->text() == text)
            return b;
    return nullptr;
}

} // namespace

class TstAppReview : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<RecordingTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<FakeShell> shell;
    QList<QUrl> openedUrls;
    QPointer<QWidget> active; // the "key window" seen by the app (null: the front viewer)
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
        services.openUrl = [this](const QUrl& u) {
            openedUrls << u;
            return true;
        };
        services.activeWindow = [this]() -> QWidget* { return active.data(); };
        app = std::make_unique<Application>(services);
    }
    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return w && QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 5000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    static bool waitWindowShown(ViewerWindow* w) { return QTest::qWaitForWindowExposed(w, 5000); }
    QAction* action(Action a) const { return app->menu()->action(a); }
    QAction* command(const char* id) const { return app->menu()->command(QString::fromLatin1(id)); }
    ViewerWindow* otherWindow(ViewerWindow* not1) const {
        for (ViewerWindow* w : app->windows())
            if (w != not1)
                return w;
        return nullptr;
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
        trash = std::make_unique<RecordingTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<FakeShell>();
        openedUrls.clear();
        active = nullptr;
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
    }

    // ---- A1: menu shortcuts and the key window ----

    // Covers: F-MAC-01, F-WIN-03 (⌘W closes the key window, not the viewer behind it)
    void F_MAC_01_menuCommandsActOnKeyWindowOnly() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("IMG_2.jpg")));
        action(Action::OpenPreferences)->trigger();
        PreferencesDialog* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs && prefs->isVisible());
        active = prefs; // the preferences window is key
        const double zoom = w->view()->zoomFactor();
        action(Action::ZoomIn)->trigger();
        action(Action::RotateLeft)->trigger();
        action(Action::Trash)->trigger();
        action(Action::NextImage)->trigger();
        QTest::qWait(200);
        QCOMPARE(w->view()->zoomFactor(), zoom);
        QCOMPARE(w->view()->rotation(), 0);
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QCOMPARE(trash->locations.size(), 0);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        // ⌘W closes the preferences, the viewer stays.
        action(Action::CloseWindow)->trigger();
        QVERIFY(!prefs->isVisible());
        QCOMPARE(app->windows().size(), 1);
        QVERIFY(w->isVisible());
        // The viewer is key again: commands reach it.
        active = w;
        action(Action::RotateLeft)->trigger();
        QCOMPARE(w->view()->rotation(), 3);
        action(Action::CloseWindow)->trigger();
        QCOMPARE(app->windows().size(), 0);
    }

    // ---- A2/A4/A7: trash, undo, origin window ----

    // Covers: F-FILE-01, F-OPEN-04 (a restored file only returns where it was)
    void F_OPEN_04_restoredFileOnlyInWindowsThatHadIt() {
        makeStandardFolder();
        ViewerWindow* multi = app->openPaths({path("IMG_10.jpg"), path("IMG_1.png")});
        QVERIFY(waitLoaded(multi));
        QVERIFY(!multi->collection()->isFolderCollection());
        QCOMPARE(multi->collection()->count(), 2);
        ViewerWindow* folder = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(folder != multi);
        QVERIFY(waitLoaded(folder));
        QCOMPARE(folder->collection()->count(), 3);

        folder->perform(Action::Trash);
        QTRY_VERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_COMPARE(folder->collection()->count(), 2);
        app->fileOps().undo();
        QTRY_COMPARE(folder->collection()->count(), 3);
        QTRY_COMPARE(folder->currentPath(), path("IMG_2.jpg"));
        QTest::qWait(200);
        QCOMPARE(multi->collection()->count(), 2); // was never part of the multi-file window
        QCOMPARE(multi->collection()->indexOf(path("IMG_2.jpg")), -1);

        // A file of the multi-file window does come back there.
        multi->collection()->setCurrentPath(path("IMG_1.png"));
        multi->perform(Action::Trash);
        QTRY_COMPARE(multi->collection()->count(), 1);
        app->fileOps().undo();
        QTRY_COMPARE(multi->collection()->count(), 2);
        QTRY_COMPARE(multi->currentPath(), path("IMG_1.png"));
    }

    // Covers: F-FILE-01 (undo → redo → undo shows the file again in the window that trashed it)
    void F_FILE_01_undoRedoUndoReturnsToTrashingWindow() {
        makeStandardFolder();
        ViewerWindow* w1 = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(w1));
        w1->setSortKey(SortKey::Name);
        QVERIFY(waitLoaded(w1));
        ViewerWindow* w2 = app->duplicateWindow(w1);
        QVERIFY(waitLoaded(w2));
        w2->collection()->setCurrentPath(path("IMG_1.png"));

        w1->perform(Action::Trash);
        QTRY_VERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_COMPARE(w1->currentPath(), path("IMG_10.jpg"));
        app->fileOps().undo();
        QTRY_COMPARE(w1->currentPath(), path("IMG_2.jpg"));
        app->fileOps().redo();
        QTRY_VERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_COMPARE(w1->currentPath(), path("IMG_10.jpg"));
        app->windowActivated(w2); // another window is in front now
        app->fileOps().undo();
        QTRY_VERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_COMPARE(w1->currentPath(), path("IMG_2.jpg"));
        QCOMPARE(w2->currentPath(), path("IMG_1.png"));
        QTRY_COMPARE(w2->collection()->count(), 3);
    }

    // Covers: F-FILE-01 (a failed trash leaves nothing behind)
    void F_FILE_01_failedTrashLeavesNoUndoEntry() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitLoaded(w));
        // Fully shown first: a decode still running would fail on the removed file and
        // could put its own message on top.
        QVERIFY(waitShown(w, path("IMG_2.jpg"), 10000));
        QSignalSpy messages(w, &ViewerWindow::messageShown);
        QSignalSpy busy(&app->fileOps(), &FileOps::busyChanged);
        QVERIFY(QFile::remove(path("IMG_2.jpg"))); // gone before the trash runs
        w->perform(Action::Trash);
        // The operation completes (busy -> idle) and its error was shown - whatever other
        // message (watcher, decoder) may have replaced the toast since.
        QTRY_VERIFY_WITH_TIMEOUT(!busy.isEmpty() && !busy.last().first().toBool(), 10000);
        auto shownError = [&] {
            for (const QList<QVariant>& m : std::as_const(messages))
                if (m.first().toString().contains(QStringLiteral("existiert nicht")))
                    return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(shownError(), 10000);
        QCOMPARE(app->fileOps().undoStack()->count(), 0);
        QCOMPARE(trash->locations.size(), 0);
    }

    // Covers: F-FILE-01 (optional confirmation, default off)
    void F_FILE_01_confirmTrashSetting() {
        makeStandardFolder();
        QCOMPARE(Settings::instance().value(key::ConfirmTrash).toBool(), false);
        Settings::instance().setValue(key::ConfirmTrash, true);
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitLoaded(w));
        w->perform(Action::Trash);
        auto* box = w->findChild<QMessageBox*>(QStringLiteral("lfConfirmTrash"));
        QVERIFY(box);
        QVERIFY(box->text().contains(QStringLiteral("IMG_2.jpg")));
        QTest::qWait(100);
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QAbstractButton* cancel = buttonWithText(box, QStringLiteral("Abbrechen"));
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY(!w->findChild<QMessageBox*>(QStringLiteral("lfConfirmTrash")));
        QVERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        w->perform(Action::Trash);
        box = w->findChild<QMessageBox*>(QStringLiteral("lfConfirmTrash"));
        QVERIFY(box);
        QAbstractButton* yes = buttonWithText(box, QStringLiteral("In den Papierkorb legen"));
        QVERIFY(yes);
        yes->click();
        QTRY_VERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_COMPARE(app->fileOps().undoStack()->count(), 1);
    }

    // ---- A3: open bursts ----

    // Covers: F-WIN-02, F-OPEN-04 (a burst never replaces an existing window's folder)
    void F_WIN_02_burstKeepsExistingWindow() {
        makeStandardFolder();
        Settings::instance().setValue(key::OpenBehavior, QStringLiteral("newUnlessOpen"));
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        QTest::qWait(Application::kOpenGroupingMs + 50);
        QFileOpenEvent e1(path("IMG_2.jpg")); // already open in `a`
        QCoreApplication::sendEvent(qApp, &e1);
        QTest::qWait(30);
        QFileOpenEvent e2(path("IMG_1.png"));
        QCoreApplication::sendEvent(qApp, &e2);
        QTRY_COMPARE_WITH_TIMEOUT(app->windows().size(), 2, 3000);
        ViewerWindow* b = otherWindow(a);
        QVERIFY(waitLoaded(b));
        QVERIFY(a->collection()->isFolderCollection());
        QCOMPARE(a->collection()->count(), 3);
        QVERIFY(!b->collection()->isFolderCollection());
        QStringList n = names(b->collection());
        n.sort();
        QCOMPARE(n, (QStringList{"IMG_1.png", "IMG_2.jpg"}));
    }

    // ---- A5: ⌘C ----

    // Covers: F-FILE-03 (full resolution + original bytes, prepared off the GUI thread)
    void F_FILE_03_copyImageFullResolutionAsync() {
        const QString big = makeImage(path("big.png"), Qt::darkCyan, QSize(3000, 2000));
        QVERIFY(!big.isEmpty());
        ViewerWindow* w = app->openPaths({big});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, big));
        QElapsedTimer t;
        t.start();
        w->perform(Action::CopyImage);
        QVERIFY2(t.elapsed() < 100, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(w->toastText(), QStringLiteral("Bild wird kopiert …"));
        QTRY_COMPARE_WITH_TIMEOUT(w->toastText(), QStringLiteral("Bild kopiert"), 8000);
        const QMimeData* md = QGuiApplication::clipboard()->mimeData();
        QVERIFY(md);
        QCOMPARE(md->urls().value(0).toLocalFile(), big);
        QVERIFY(md->hasImage());
        QCOMPARE(qvariant_cast<QImage>(md->imageData()).size(), QSize(3000, 2000)); // not the screen stage
        QCOMPARE(md->data(QStringLiteral("image/png")), readAll(big));
    }

    // ---- A6: placement ----

    // Covers: F-WIN-08 (cascade in x and y, never (nearly) on top of each other, on screen)
    void F_WIN_08_cascadeKeepsWindowsApart() {
        for (int i = 0; i < 12; ++i)
            app->newWindow();
        const QList<ViewerWindow*> list = app->windows();
        QCOMPARE(list.size(), 12);
        QSet<int> ys;
        for (int i = 0; i < list.size(); ++i) {
            ys.insert(list[i]->pos().y());
            for (int j = i + 1; j < list.size(); ++j) {
                const QPoint d = list[i]->pos() - list[j]->pos();
                QVERIFY2(std::abs(d.x()) >= 10 || std::abs(d.y()) >= 10,
                         qPrintable(QStringLiteral("%1 vs %2").arg(i).arg(j)));
            }
            const QRect avail = list[i]->screen()->availableGeometry();
            QVERIFY2(avail.contains(list[i]->pos()),
                     qPrintable(QStringLiteral("window %1 off screen").arg(i)));
        }
        QVERIFY(ys.size() > 1); // cascades in y as well
    }

    // ---- A7: "Nicht gefunden" ----

    // Covers: F-OPEN-01 (missing paths are reported, also without any window)
    void F_OPEN_01_notFoundShownWithoutWindow() {
        QCOMPARE(app->windows().size(), 0);
        app->openPaths({path("does-not-exist")});
        QTRY_COMPARE(app->windows().size(), 1);
        QVERIFY2(
            app->windows().first()->toastText().contains(QStringLiteral("Nicht gefunden: does-not-exist")),
            qPrintable(app->windows().first()->toastText()));
    }

    // ---- A8: start screen ----

    // Covers: F-OPEN-12, F-MAC-02 (start screen readable, no empty status bar strip)
    void F_OPEN_12_startScreenReadable() {
        makeStandardFolder();
        ViewerWindow* w = app->newWindow();
        QVERIFY(waitWindowShown(w));
        QVERIFY(w->isEmpty());
        QVERIFY(w->statusBar()->isHidden());
        auto* open = w->findChild<QPushButton*>(QStringLiteral("lfOpenButton"));
        QVERIFY(open && open->isVisible());
        for (const char* bg : {"dark", "light", "checker"}) {
            Settings::instance().setValue(key::Background, QString::fromLatin1(bg));
            open->ensurePolished();
            const double c =
                contrast(open->palette().color(QPalette::ButtonText), w->view()->backgroundColor());
            QVERIFY2(c >= 4.5, qPrintable(QStringLiteral("%1: contrast %2").arg(QLatin1String(bg)).arg(c)));
        }
        app->openPaths({path("IMG_2.jpg")}); // reuses the start window
        QTRY_VERIFY(!w->statusBar()->isHidden());
    }

    // ---- B9: German ----

    // Covers: F-MAC-01 (Qt's own texts are German: application menu, buttons)
    void F_MAC_01_germanQtTranslations() {
        // German UI chosen explicitly (the default follows the system language, F-I18N-03).
        installTranslations(QStringLiteral("de"));
        const QString file = installedQtCatalog();
        QVERIFY2(!file.isEmpty(), qPrintable(qtTranslationDirs().join(QLatin1Char(' '))));
        QCOMPARE(QCoreApplication::translate("MAC_APPLICATION_MENU", "Quit %1"),
                 QStringLiteral("%1 beenden"));
        QCOMPARE(QCoreApplication::translate("MAC_APPLICATION_MENU", "Services"), QStringLiteral("Dienste"));
        QCOMPARE(QCoreApplication::translate("MAC_APPLICATION_MENU", "Show All"),
                 QStringLiteral("Alle anzeigen"));
        QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Abbrechen"));
        QMessageBox box(QMessageBox::Question, QStringLiteral("t"), QStringLiteral("t"),
                        QMessageBox::Yes | QMessageBox::No);
        QCOMPARE(box.button(QMessageBox::No)->text().remove(QLatin1Char('&')), QStringLiteral("Nein"));
    }

    // ---- B11: one enumeration per folder change ----

    // Covers: F-WIN-09, F-OPEN-07
    void F_WIN_09_folderEnumeratedOncePerChange() {
        makeStandardFolder();
        QTest::qWait(1500); // FSEvents of the files just written must not count as a change
        // Two windows at once (the first load still running), a third one later.
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        ViewerWindow* b = app->openPaths({path("IMG_10.jpg")});
        QVERIFY(a != b);
        QVERIFY(waitLoaded(a) && waitLoaded(b));
        const QString folder = a->collection()->folderPath();
        app->openPaths({dir->path()});
        QTRY_COMPARE(app->windows().size(), 3);
        ViewerWindow* c = app->windows().last();
        QVERIFY(waitLoaded(c));
        for (ViewerWindow* w : app->windows())
            QCOMPARE(w->collection()->count(), 3);
        QCOMPARE(FolderCache::instance().enumerationCount(folder), 1);
        QCOMPARE(FolderCache::instance().leaseCount(folder), 3);

        // One change (atomic rename into the folder): one enumeration for all three windows.
        QTemporaryDir staging;
        const QString img = makeImage(staging.filePath(QStringLiteral("IMG_3.png")), Qt::magenta);
        QVERIFY(QFile::rename(img, path("IMG_3.png")));
        for (ViewerWindow* w : app->windows())
            QTRY_COMPARE_WITH_TIMEOUT(w->collection()->count(), 4, 8000);
        QTest::qWait(300);
        const int after = FolderCache::instance().enumerationCount(folder);
        QVERIFY2(after >= 2 && after <= 3, qPrintable(QString::number(after))); // not one per window
        app->closeAllWindows();
        QTest::qWait(10);
        QCOMPARE(FolderCache::instance().leaseCount(folder), 0);
    }

    // ---- B12: nothing waits for the file system on the GUI thread ----

    // Covers: F-FILE-01 (trash/undo on a slow volume: the GUI stays responsive)
    void F_FILE_01_slowTrashDoesNotBlockGui() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(w));
        trash->delayMs = 800;
        QElapsedTimer t;
        t.start();
        w->perform(Action::Trash);
        QVERIFY2(t.elapsed() < 150, qPrintable(QString::number(t.elapsed())));
        QVERIFY(app->fileOps().isBusy());
        QTRY_VERIFY_WITH_TIMEOUT(QGuiApplication::overrideCursor() != nullptr, 700); // busy state
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(path("IMG_2.jpg")), 3000);
        QTRY_VERIFY(!app->fileOps().isBusy());
        QTRY_VERIFY(QGuiApplication::overrideCursor() == nullptr);
        t.restart();
        app->fileOps().undo();
        QVERIFY2(t.elapsed() < 150, qPrintable(QString::number(t.elapsed())));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path("IMG_2.jpg")), 3000);
        trash->delayMs = 0;
    }

    // Covers: F-MAC-04 (the preferences button registers without blocking, German result)
    void F_MAC_04_makeDefaultViewerAsync() {
        shell->delayMs = 1500; // LaunchServices answering slowly
        action(Action::OpenPreferences)->trigger();
        PreferencesDialog* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs);
        auto* button = prefs->findChild<QPushButton*>(QStringLiteral("makeDefault"));
        QVERIFY(button);
        button->click();
        auto* question = prefs->findChild<DefaultAppDialog*>(QStringLiteral("lfMakeDefaultDialog"));
        QVERIFY(question); // the type checklist (tst_m2_fixes_ui: F_MAC_04_*)
        QSignalSpy finished(app.get(), &Application::defaultViewerFinished);
        QAbstractButton* yes = question->confirmButton();
        QVERIFY(yes);
        QCOMPARE(yes->text(), QStringLiteral("Festlegen"));
        QElapsedTimer t;
        t.start();
        yes->click();
        QVERIFY2(t.elapsed() < 700, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(shell->makeDefaultCalls.load(), 0); // still running on a worker
        QCOMPARE(finished.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000);
        QCOMPARE(shell->makeDefaultCalls.load(), 1);
        QVERIFY(finished.first().at(0).toBool());
        const QString message = finished.first().at(1).toString();
        QCOMPARE(message, QStringLiteral("Lumaflip ist jetzt die Standard-App für Bilder."));
        QCOMPARE(prefs->statusText(), message);
        if (auto* result = prefs->findChild<QMessageBox*>(QStringLiteral("lfMakeDefaultResult")))
            result->close();
    }

    // ---- B13/B14: title and sort menu ----

    // Covers: F-WIN-10 (option: folder name in the title)
    void F_WIN_10_folderNameInTitle() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(w));
        const int pos = w->collection()->currentIndex() + 1;
        QCOMPARE(w->windowTitle(), QStringLiteral("IMG_2.jpg — %1 / 3").arg(pos));
        Settings::instance().setValue(key::TitleShowFolder, true);
        QCOMPARE(w->windowTitle(),
                 QStringLiteral("IMG_2.jpg — %1 / 3 — %2").arg(pos).arg(QDir(dir->path()).dirName()));
        action(Action::OpenPreferences)->trigger();
        auto* box = PreferencesDialog::existingInstance()->findChild<QCheckBox*>(
            QStringLiteral("setting:") + QLatin1String(key::TitleShowFolder));
        QVERIFY(box && box->isChecked());
        box->setChecked(false);
        QCOMPARE(w->windowTitle(), QStringLiteral("IMG_2.jpg — %1 / 3").arg(pos));
    }

    // Covers: F-SORT-02 (Pixelzahl and Bewertung in Darstellung → Sortieren nach)
    void F_SORT_02_pixelCountAndRatingInMenu() {
        QVERIFY(!makeImage(path("small.png"), Qt::red, QSize(10, 10)).isEmpty());
        QVERIFY(!makeImage(path("large.png"), Qt::blue, QSize(40, 40)).isEmpty());
        ViewerWindow* w = app->openPaths({path("small.png")});
        QVERIFY(waitLoaded(w));
        app->windowActivated(w);
        QVERIFY(command("sort:pixelCount"));
        QCOMPARE(command("sort:pixelCount")->text(), QStringLiteral("Pixelzahl"));
        QVERIFY(command("sort:rating"));
        QCOMPARE(command("sort:rating")->text(), QStringLiteral("Bewertung"));
        command("sort:pixelCount")->trigger();
        QVERIFY(waitLoaded(w));
        command("sort:desc")->trigger();
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->collection()->sortSpec().key, SortKey::PixelCount);
        QCOMPARE(names(w->collection()), (QStringList{"large.png", "small.png"}));
        command("sort:rating")->trigger();
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->collection()->sortSpec().key, SortKey::Rating);
        app->menu()->refreshState();
        QVERIFY(command("sort:rating")->isChecked());
    }

    // ---- B15: unfinished settings, end of collection ----

    // Covers: F-SET-01 (settings without effect are disabled with "in einer späteren Version")
    void F_SET_01_unfinishedSettingsDisabled() {
        action(Action::OpenPreferences)->trigger();
        PreferencesDialog* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs);
        auto check = [&](const char* k) {
            return prefs->findChild<QCheckBox*>(QStringLiteral("setting:") + QLatin1String(k));
        };
        for (const char* k :
             {key::RatingStoreSidecar, key::RatingStoreEmbedded, key::RatingStoreFinderTags}) {
            QVERIFY2(check(k) && check(k)->isEnabled(), k); // M3: ratings are implemented
        }
        for (const char* k :
             {key::RestoreWindows, key::RawEmbeddedOnly, key::ConfirmTrash, key::TitleShowFolder,
              key::ShowInfoPanel, key::ShowFilmstrip}) // M2: info panel, filmstrip
            QVERIFY2(check(k) && check(k)->isEnabled(), k);
        auto choiceEnabled = [&](const char* slot, const char* choice) {
            auto* combo = prefs->findChild<QComboBox*>(QStringLiteral("slot:") + QLatin1String(slot));
            auto* model = combo ? qobject_cast<QStandardItemModel*>(combo->model()) : nullptr;
            const int i = combo ? combo->findData(QString::fromLatin1(choice)) : -1;
            return model && i >= 0 ? model->item(i)->isEnabled() : true;
        };
        QVERIFY(!choiceEnabled("middleClick", "magnifier"));
        QVERIFY(choiceEnabled("drag", "dragOut"));      // F-FILE-07 (M2)
        QVERIFY(choiceEnabled("space", "slideshow"));   // M2: slideshow (F-SLIDE-01)
        QVERIFY(choiceEnabled("pageUpDown", "page"));   // M2: pages (F-FMT-04)
        QVERIFY(choiceEnabled("pageUpDown", "folder")); // implemented
        QVERIFY(choiceEnabled("middleClick", "fitActual"));
        auto* wrap = prefs->findChild<QComboBox*>(QStringLiteral("setting:") + QLatin1String(key::WrapMode));
        QVERIFY(wrap && wrap->findData(QStringLiteral("nextFolder")) >= 0);
    }

    // Covers: F-NAV-03, F-IN-16 ("Zum nächsten Ordner"; also Action::NextFolder/PreviousFolder)
    void F_NAV_03_endOfCollectionNextFolder() {
        QVERIFY(QDir().mkpath(path("A")) && QDir().mkpath(path("B")) && QDir().mkpath(path("C")));
        QVERIFY(!makeImage(path("A/a1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("A/a2.png"), Qt::red).isEmpty());
        QFile txt(path("B/readme.txt")); // no images: skipped
        QVERIFY(txt.open(QIODevice::WriteOnly));
        txt.close();
        QVERIFY(!makeImage(path("C/c1.png"), Qt::blue).isEmpty());
        QVERIFY(!makeImage(path("C/c2.png"), Qt::blue).isEmpty());
        Settings::instance().setValue(key::FollowFinder, false); // name order
        Settings::instance().setValue(key::WrapMode, QStringLiteral("nextFolder"));
        ViewerWindow* w = app->openPaths({path("A/a2.png")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->currentPath(), path("A/a2.png"));
        QTest::keyClick(w->view(), Qt::Key_Right);
        QTRY_COMPARE(w->collection()->folderPath(), path("C"));
        QVERIFY(waitLoaded(w));
        QTRY_COMPARE(w->currentPath(), path("C/c1.png"));
        QTest::keyClick(w->view(), Qt::Key_Left); // back: the last image of the previous folder
        QTRY_COMPARE(w->collection()->folderPath(), path("A"));
        QVERIFY(waitLoaded(w));
        QTRY_COMPARE(w->currentPath(), path("A/a2.png"));
        w->perform(Action::NextFolder);
        QTRY_COMPARE(w->collection()->folderPath(), path("C"));
        w->perform(Action::NextFolder); // nothing after C
        QTRY_VERIFY(w->toastText().contains(QStringLiteral("Kein weiterer Ordner")));
    }

    // Covers: F-WIN-06 (windows come back with folder, image, zoom and position)
    void F_WIN_06_restoreWindows() {
        makeStandardFolder();
        Settings::instance().setValue(key::RestoreWindows, true); // default: off like macOS (F-WIN-06)
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        a->setZoomMode(ZoomMode::Actual);
        ViewerWindow* b = app->openPaths({path("IMG_10.jpg"), path("IMG_1.png")});
        QVERIFY(waitLoaded(b));
        b->collection()->setCurrentPath(path("IMG_1.png"));
        app->newWindow(); // empty windows are not remembered
        app->saveSession();
        QVERIFY(app->hasSession());
        app.reset();
        makeApp();
        QSignalSpy restored(app.get(), &Application::sessionRestored);
        app->start({QStringLiteral("Lumaflip")});
        QTRY_COMPARE(restored.size(), 1);
        QCOMPARE(app->windows().size(), 2);
        ViewerWindow* ra = app->windows().at(0);
        ViewerWindow* rb = app->windows().at(1);
        QVERIFY(waitLoaded(ra) && waitLoaded(rb));
        QVERIFY(ra->collection()->isFolderCollection());
        QCOMPARE(ra->currentPath(), path("IMG_2.jpg"));
        QCOMPARE(ra->collection()->count(), 3);
        QCOMPARE(ra->view()->zoomMode(), ZoomMode::Actual);
        QVERIFY(!rb->collection()->isFolderCollection());
        QCOMPARE(rb->collection()->count(), 2);
        QCOMPARE(rb->currentPath(), path("IMG_1.png"));

        // Switched off: an empty start window instead.
        Settings::instance().setValue(key::RestoreWindows, false);
        app->saveSession();
        app.reset();
        makeApp();
        app->start({QStringLiteral("Lumaflip")});
        QTRY_COMPARE(app->windows().size(), 1);
        QVERIFY(app->windows().first()->isEmpty());
    }

    // ---- B17: duplicate viewport, Dock click, failing full stage ----

    // Covers: F-WIN-03 (⌥⌘N copies zoom and the visible part of the image)
    void F_WIN_03_duplicateCopiesZoomAndViewport() {
        const QString big = makeImage(path("big.png"), Qt::darkGreen, QSize(3000, 2000));
        ViewerWindow* w = app->openPaths({big});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, big));
        w->view()->setZoomFactor(0.8, QPointF(w->view()->width() / 2.0, w->view()->height() / 2.0));
        w->view()->panBy(QPointF(-150, -90));
        const QPointF center = w->view()->viewCenter();
        QVERIFY(center.x() > 0.5 && center.y() > 0.5);
        app->windowActivated(w);
        action(Action::DuplicateWindow)->trigger();
        ViewerWindow* d = app->frontWindow();
        QVERIFY(d != w);
        QVERIFY(waitWindowShown(d));
        QCOMPARE(d->view()->zoomMode(), ZoomMode::Custom);
        QVERIFY(std::abs(d->view()->zoomFactor() - w->view()->zoomFactor()) < 1e-6);
        const QPointF dc = d->view()->viewCenter();
        QVERIFY2(
            std::abs(dc.x() - center.x()) < 0.01 && std::abs(dc.y() - center.y()) < 0.01,
            qPrintable(
                QStringLiteral("%1,%2 vs %3,%4").arg(dc.x()).arg(dc.y()).arg(center.x()).arg(center.y())));
    }

    // Covers: F-MAC-03 (Dock click without windows opens an empty window)
    void F_MAC_03_dockClickWithoutWindowOpensOne() {
        app->start({QStringLiteral("Lumaflip")});
        QTRY_COMPARE(app->windows().size(), 1);
        app->closeAllWindows();
        QCOMPARE(app->windows().size(), 0);
        app->handleDockReopen(); // applicationShouldHandleReopen (see tst_review3 for the hook)
        QTRY_COMPARE(app->windows().size(), 1);
        QVERIFY(app->windows().first()->isEmpty());
        // With a window open nothing more happens.
        app->handleDockReopen();
        QTest::qWait(50);
        QCOMPARE(app->windows().size(), 1);
    }

    // Covers: F-FMT-02 (a failing full-resolution stage is a status message, not an error card)
    void F_FMT_02_fullStageFailureInStatusBar() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("IMG_2.jpg")));
        emit app->provider().stageFailed(path("IMG_2.jpg"), Quality::Full, QStringLiteral("ImageIO: kaputt"));
        QCOMPARE(w->statusBar()->currentMessage(), QStringLiteral("Volle Auflösung nicht verfügbar"));
        QVERIFY(!w->view()->isShowingError());
        QVERIFY(w->view()->image());
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
    TstAppReview tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_app_review.moc"
