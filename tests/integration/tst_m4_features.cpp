// SPDX-License-Identifier: GPL-3.0-or-later
// M4 macOS features in real windows: archives and PDF browsed like a folder (F-OPEN-10),
// the Live Text toggle (F-MAC-06), Quick Look (F-MAC-07), the desktop picture question
// (F-FILE-09, never confirmed here: the test must not change the desktop) and custom actions
// from the menu (F-FILE-10). Archives/PDFs are generated in temporary folders.

#include "app/Application.h"
#include "app/CustomActionsUi.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MacIntegration.h"
#include "app/MainMenu.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "app/WindowIntegration.h"
#include "core/Archive.h"
#include "core/Collection.h"
#include "core/ContainerPolicy.h"
#include "core/CustomActions.h"
#include "core/FileOps.h"
#include "core/LibArchive.h"
#include "core/LiveText.h"
#include "core/PdfDocument.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/VirtualPath.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

class RecordingTrash : public TrashService {
public:
    QStringList trashed;
    std::optional<QString> moveToTrash(const QString& p, QString*) override {
        trashed << p;
        return p + QStringLiteral(".trashed");
    }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return true; }
};

class RecordingShell : public Shell {
public:
    QStringList revealed;
    void revealInFileManager(const QString& p) override { revealed << p; }
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

QByteArray png(int w, int h, QColor c) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(c);
    QByteArray out;
    QBuffer b(&out);
    b.open(QIODevice::WriteOnly);
    img.save(&b, "PNG");
    return out;
}

} // namespace

class TstM4Features : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<RecordingTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<RecordingShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    ViewerWindow* openAndWait(const QString& p) {
        app->openPaths({p});
        // An archive or PDF opens after a check on a worker thread; until then the front
        // window may still be the one opened before: wait for the window showing `p`.
        const QString wanted = QDir::cleanPath(p);
        auto shows = [this, wanted] {
            ViewerWindow* f = app->frontWindow();
            if (!f)
                return false;
            const QString cur = f->currentPath();
            return cur == wanted || vpath::containerOf(cur) == wanted ||
                   f->collection()->containerPath() == wanted;
        };
        if (!QTest::qWaitFor(shows, 10000))
            return nullptr;
        ViewerWindow* w = app->frontWindow();
        if (!QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        if (!QTest::qWaitFor([w] { return !w->collection()->isLoading() && w->collection()->count() > 0; },
                             10000))
            return nullptr;
        return w;
    }
    QString comic() {
        QString error;
        const QString cbz = path(QStringLiteral("Band 1.cbz"));
        const bool ok = libarchive::writeArchive(cbz, QStringLiteral("zip"),
                                                 {{QStringLiteral("10.png"), png(64, 48, Qt::blue)},
                                                  {QStringLiteral("2.png"), png(64, 48, Qt::green)},
                                                  {QStringLiteral("1.png"), png(64, 48, Qt::red)}},
                                                 &error);
        return ok ? cbz : QString();
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        RecentItems::setSystemRegistrationEnabled(false);
        Settings::useTestingBackend(m_settingsDir.filePath(QStringLiteral("init.ini")));
        ImageView probe;
        probe.resize(64, 64);
        probe.show();
        QVERIFY(QTest::qWaitForWindowExposed(&probe));
        auto* c = probe.findChild<ImageCanvas*>();
        QVERIFY(c);
        if (c->grabFramebuffer().isNull() || !c->isRhiAvailable())
            QSKIP("No QRhi (GPU) available on this platform");
    }

    void init() {
        Settings::useTestingBackend(
            m_settingsDir.filePath(QStringLiteral("s%1.ini").arg(++m_settingsCounter)));
        dir = std::make_unique<QTemporaryDir>();
        QVERIFY(dir->isValid());
        trash = std::make_unique<RecordingTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<RecordingShell>();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
    }

    void cleanup() {
        macint::quickLookClose();
        app.reset();
        QTest::qWait(10);
        QThreadPool::globalInstance()->waitForDone(5000);
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
    }

    // Covers: F-OPEN-10 (archive opened like a folder: Finder order, status "a.cbz › 1.png")
    void F_OPEN_10_archiveOpensLikeAFolder() {
        const QString cbz = comic();
        QVERIFY(!cbz.isEmpty());
        ViewerWindow* w = openAndWait(cbz);
        QVERIFY(w);
        QVERIFY(w->collection()->isContainerCollection());
        QCOMPARE(w->collection()->count(), 3);
        QCOMPARE(w->currentPath(), vpath::join(cbz, QStringLiteral("1.png")));
        QTRY_COMPARE(w->shownPath(), w->currentPath());
        QVERIFY(w->view()->image());
        QCOMPARE(w->view()->image()->fullSize, QSize(64, 48));
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Band 1.cbz › 1.png")));
        QVERIFY(w->windowTitle().contains(QStringLiteral("›")));
        QVERIFY(w->perform(Action::NextImage));
        QCOMPARE(w->currentPath(), vpath::join(cbz, QStringLiteral("2.png")));
        QVERIFY(w->perform(Action::NextImage));
        QCOMPARE(w->currentPath(), vpath::join(cbz, QStringLiteral("10.png")));
        // "Im Finder zeigen" shows the archive.
        QVERIFY(w->perform(Action::RevealInFinder));
        QCOMPARE(shell->revealed, QStringList{cbz});
        // The window comes back with the archive (F-WIN-06 session).
        const QVariantMap state = w->sessionState();
        QCOMPARE(state.value(QStringLiteral("container")).toString(), cbz);
        ViewerWindow* again = app->newWindow();
        QVERIFY(again->restoreSessionState(state, {cbz}));
        QTRY_VERIFY(!again->collection()->isLoading());
        QCOMPARE(again->currentPath(), vpath::join(cbz, QStringLiteral("10.png")));
    }

    // Covers: F-OPEN-10 (trash/rename/move refused with a clear message)
    void F_OPEN_10_fileOperationsAreRefusedInArchives() {
        const QString cbz = comic();
        ViewerWindow* w = openAndWait(cbz);
        QVERIFY(w);
        for (Action a :
             {Action::Trash, Action::Rename, Action::QuickMove, Action::RatePick, Action::SetWallpaper}) {
            w->hideMessage();
            QVERIFY(w->perform(a));
            QCOMPARE(w->toastText(), containers::blockedMessage());
        }
        QTest::qWait(200);
        QVERIFY(trash->trashed.isEmpty());
        QVERIFY(QFile::exists(cbz));
        QCOMPARE(w->collection()->count(), 3);
    }

    // Covers: F-OPEN-10 (comics right to left: ← goes forward)
    void F_OPEN_10_comicsRightToLeft() {
        const QString cbz = comic();
        ViewerWindow* w = openAndWait(cbz);
        QVERIFY(w);
        QAction* rtl = app->menu()->action(Action::ToggleRightToLeft);
        QVERIFY(rtl);
        rtl->trigger();
        QVERIFY(Settings::instance().value(key::ArchiveRightToLeft).toBool());
        QVERIFY(w->integration()->rightToLeftActive());
        InputEvent left;
        left.kind = InputKind::Key;
        left.key = Qt::Key_Left;
        w->handleInput(left, QPointF(-1, -1));
        QCOMPARE(w->currentPath(), vpath::join(cbz, QStringLiteral("2.png")));
        InputEvent right = left;
        right.key = Qt::Key_Right;
        w->handleInput(right, QPointF(-1, -1));
        QCOMPARE(w->currentPath(), vpath::join(cbz, QStringLiteral("1.png")));
    }

    // Covers: F-OPEN-10 (PDF pages like images)
    void F_OPEN_10_pdfPagesInAWindow() {
        if (!pdf::available())
            QSKIP("no PDF renderer in this build (QtPdf missing)");
        const QString pdf = path(QStringLiteral("Plan.pdf"));
        {
            QPdfWriter writer(pdf);
            QPainter p(&writer);
            p.fillRect(QRect(0, 0, 500, 500), Qt::black);
            writer.newPage();
            p.fillRect(QRect(0, 0, 500, 500), Qt::gray);
        }
        ViewerWindow* w = openAndWait(pdf);
        QVERIFY(w);
        QCOMPARE(w->collection()->count(), 2);
        QTRY_VERIFY(w->view()->image());
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Plan.pdf › ")));
        QVERIFY(w->perform(Action::NextImage));
        QCOMPARE(w->currentPath(), vpath::join(pdf, QStringLiteral("0002")));
    }

    // Covers: F-MAC-06 (Live Text toggle: menu, ⇧⌘L, the setting switches it off)
    void F_MAC_06_liveTextToggle() {
        {
            QFile f(path(QStringLiteral("a.png")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(png(200, 100, Qt::white));
        }
        ViewerWindow* w = openAndWait(path(QStringLiteral("a.png")));
        QVERIFY(w);
        QAction* toggle = app->menu()->action(Action::ToggleLiveText);
        QVERIFY(toggle);
        QVERIFY(toggle->isCheckable());
        QCOMPARE(toggle->shortcut(), QKeySequence(QStringLiteral("Shift+Ctrl+L")));
        LiveTextSession* s = w->integration()->liveText();
        QVERIFY(s);
        QVERIFY(!s->isActive()); // nothing is analysed until asked
        if (!WindowIntegration::liveTextAvailable()) {
            QVERIFY(w->perform(Action::ToggleLiveText));
            QVERIFY(!s->isActive());
            QVERIFY(!w->toastText().isEmpty());
            return;
        }
        QVERIFY(w->perform(Action::ToggleLiveText));
        QVERIFY(s->isActive());
        QTRY_VERIFY(s->state() != LiveTextSession::State::Analyzing);
        // Flipping cancels; switching off in the settings makes it unavailable.
        Settings::instance().setValue(key::LiveTextEnabled, false);
        QCOMPARE(s->state(), LiveTextSession::State::Unavailable);
        QVERIFY(w->perform(Action::ToggleLiveText));
        QVERIFY(!s->isActive());
    }

    // Covers: F-MAC-07 (Quick Look with ⌘Y; refused inside archives)
    void F_MAC_07_quickLook() {
#ifdef LF_GENERIC_PLATFORM
        QSKIP("Quick Look is macOS only (docs/LINUX.md)");
#endif
        {
            QFile f(path(QStringLiteral("a.png")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(png(80, 60, Qt::red));
        }
        ViewerWindow* w = openAndWait(path(QStringLiteral("a.png")));
        QVERIFY(w);
        QAction* ql = app->menu()->action(Action::QuickLook);
        QVERIFY(ql);
        QCOMPARE(ql->shortcut(), QKeySequence(QStringLiteral("Ctrl+Y")));
        QVERIFY(w->perform(Action::QuickLook));
        QTRY_VERIFY(macint::quickLookVisible());
        QVERIFY(w->perform(Action::QuickLook));
        QTRY_VERIFY(!macint::quickLookVisible());
        ViewerWindow* archive = openAndWait(comic());
        QVERIFY(archive);
        QVERIFY(archive->perform(Action::QuickLook));
        QCOMPARE(archive->toastText(), containers::blockedMessage());
        QVERIFY(!macint::quickLookVisible());
    }

    // Covers: F-FILE-09 (asks first; never confirmed here)
    void F_FILE_09_wallpaperAsksFirst() {
        {
            QFile f(path(QStringLiteral("a.png")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(png(80, 60, Qt::red));
        }
        ViewerWindow* w = openAndWait(path(QStringLiteral("a.png")));
        QVERIFY(w);
        QVERIFY(app->menu()->action(Action::SetWallpaper));
        QVERIFY(w->perform(Action::SetWallpaper));
        QTRY_VERIFY(w->integration()->wallpaperQuestion());
        QMessageBox* box = w->integration()->wallpaperQuestion();
        QVERIFY(box->checkBox());
        QCOMPARE(box->checkBox()->objectName(), QStringLiteral("lfWallpaperAllScreens"));
        box->reject(); // the desktop stays as it is
        QTRY_VERIFY(!w->integration()->wallpaperQuestion());
    }

    // Covers: F-FILE-10 (custom action in the menu with its key, confirmed first run, log)
    void F_FILE_10_customActionFromTheMenu() {
#ifdef Q_OS_WIN
        QSKIP("POSIX shell command and a file name with \" (Windows: test_windows_platform.cpp)");
#endif
        {
            QFile f(path(QStringLiteral("it's \"here\".png")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(png(80, 60, Qt::red));
        }
        CustomAction a;
        a.id = QStringLiteral("copyname");
        a.name = QStringLiteral("Name merken");
        a.command = QStringLiteral("printf '%s' \"$1\" > \"$(dirname \"$1\")/result.txt\"");
        a.keys = QKeySequence(QStringLiteral("Ctrl+Alt+Shift+9"));
        customactions::save({a});
        QTRY_COMPARE(CustomActionsManager::instance().actions().size(), 1);
        QAction* entry = app->menu()->command(QStringLiteral("customAction:copyname"));
        QVERIFY(entry);
        QCOMPARE(entry->shortcut(), a.keys);
        int asked = 0;
        CustomActionsManager::instance().setConfirmForTesting([&](const CustomAction&, const QStringList&) {
            ++asked;
            return true;
        });
        ViewerWindow* w = openAndWait(path(QStringLiteral("it's \"here\".png")));
        QVERIFY(w);
        entry->trigger();
        QTRY_VERIFY(QFile::exists(path(QStringLiteral("result.txt"))));
        QFile out(path(QStringLiteral("result.txt")));
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(out.readAll()), path(QStringLiteral("it's \"here\".png")));
        QCOMPARE(asked, 1);
        QTRY_VERIFY(customactions::load().first().confirmed);
        // Saving the confirmation rebuilds the menu entries (the old QAction is gone):
        // let the rebuild run and fetch the entry again.
        QTest::qWait(200);
        QAction* again = app->menu()->command(QStringLiteral("customAction:copyname"));
        if (!again)
            QFAIL("menu entry missing after the rebuild");
        again->trigger(); // confirmed: no question
        QTRY_VERIFY(!CustomActionsManager::instance().isRunning());
        QCOMPARE(asked, 1);
        QVERIFY(w->perform(Action::ShowActionLog));
        QTRY_VERIFY(CustomActionsManager::instance().logWindow());
        QVERIFY(
            CustomActionsManager::instance().logLines().join(u'\n').contains(QStringLiteral("Name merken")));
        CustomActionsManager::instance().logWindow()->close();
        CustomActionsManager::instance().setConfirmForTesting({});
        customactions::save({});
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
    TstM4Features tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_m4_features.moc"
