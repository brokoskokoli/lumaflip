// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end tests of the application shell (SPEC 3, 4.1-4.3, 4.6-4.8, 4.12-4.15,
// 10.2, 12.1): Application + ViewerWindow + MainMenu with the real Collection,
// ImageProvider, decoders and trash. Real windows: on macOS the test forces the
// cocoa platform (the image canvas needs Metal) and skips only without a GPU.
//
// Shortcuts with ⌘ are menu shortcuts. Keyboard focus/activation of windows is not
// reliable while other GUI tests run in parallel, so those are verified by their
// QAction (shortcut list + trigger); plain keys are sent to the view.

#include "app/Application.h"
#include "app/DisplayText.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QWheelEvent>
#include <QtTest>

#include <memory>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lf;

namespace {

// Real trash, but remembers every location so exactly those files can be removed
// from the user's trash afterwards (same pattern as tests/unit/test_fileops.mm).
class RecordingTrash : public TrashService {
public:
    RecordingTrash() : m_real(createTrashService()) {}
    ~RecordingTrash() override { cleanup(); }
    void cleanup() {
        for (const QString& p : std::as_const(locations))
            if (!p.isEmpty() && QFileInfo(p).isFile())
                QFile::remove(p);
        locations.clear();
    }
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        auto loc = m_real->moveToTrash(path, error);
        if (loc)
            locations << *loc;
        return loc;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        return m_real->restoreFromTrash(trashed, original, error);
    }
    QStringList locations;

private:
    std::unique_ptr<TrashService> m_real;
};

class FakeShell : public Shell {
public:
    void revealInFileManager(const QString& path) override { revealed << path; }
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override {
        ++makeDefaultCalls; // never touches the system defaults
        return true;
    }
    QStringList revealed;
    int makeDefaultCalls = 0;
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

QString writeBytes(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    return QDir::cleanPath(path);
}

QStringList names(const Collection* c) {
    QStringList out;
    for (const auto& e : c->entries())
        out << e.name;
    return out;
}

QByteArray sha(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

} // namespace

class TstApp : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<RecordingTrash> trash;
    std::unique_ptr<FileOps> ops;
    FakeShell* shell = nullptr;
    std::unique_ptr<FakeShell> shellOwner;
    QList<QUrl> openedUrls;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }

    // Folder used by most tests: three images, plus files that must be skipped (F-OPEN-06).
    void makeStandardFolder() {
        QVERIFY(!makeImage(path("IMG_10.jpg"), Qt::blue).isEmpty());
        QVERIFY(!makeImage(path("IMG_2.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("IMG_1.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path(".hidden.png"), Qt::yellow).isEmpty());
        QVERIFY(!makeImage(path("._IMG_1.png"), Qt::yellow).isEmpty());
        QVERIFY(!writeBytes(path("notes.txt"), "hello").isEmpty());
        QVERIFY(QDir().mkpath(path("Sub.png"))); // a folder with an image suffix
    }

    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 5000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    static bool waitWindowShown(ViewerWindow* w) { return QTest::qWaitForWindowExposed(w, 5000); }

    QAction* action(Action a) const { return app->menu()->action(a); }
    QAction* command(const char* id) const { return app->menu()->command(QString::fromLatin1(id)); }
    static void key(ViewerWindow* w, Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QTest::keyClick(w->view(), k, mods);
    }
    // ⌘ shortcut: must be a menu shortcut; trigger it the way the menu does.
    void menuShortcut(Action a, const QKeySequence& seq) {
        QAction* act = action(a);
        QVERIFY2(act, qPrintable(actionId(a)));
        QVERIFY2(act->shortcuts().contains(seq), qPrintable(actionId(a) + " " + seq.toString()));
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
        trash = std::make_unique<RecordingTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shellOwner = std::make_unique<FakeShell>();
        shell = shellOwner.get();
        openedUrls.clear();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell;
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
        ops.reset();
        trash.reset(); // removes exactly the files this test put into the trash
        shellOwner.reset();
        dir.reset();
    }

    // ---- opening ----

    // Covers: F-OPEN-02, F-OPEN-03, F-SORT-03
    void F_OPEN_02_singleFileLoadsFolder() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(w);
        // F-OPEN-03: the opened file is the current image at once, before the folder is read.
        QCOMPARE(w->collection()->count(), 1);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("IMG_2.jpg")));
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->collection()->count(), 3);
        QCOMPARE(w->currentPath(), path("IMG_2.jpg"));
        QVERIFY(w->collection()->isFolderCollection());
        const SortSpec spec = w->collection()->sortSpec();
        QVERIFY(spec.source != SortSource::Manual);      // "Wie Finder" is the default (F-SORT-03)
        if (spec.key == SortKey::Name && spec.ascending) // no Finder info for temp folders: name order
            QCOMPARE(names(w->collection()), (QStringList{"IMG_1.png", "IMG_2.jpg", "IMG_10.jpg"}));
    }

    // Covers: F-OPEN-06
    void F_OPEN_06_hiddenAndNonImagesSkipped() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_1.png")});
        QVERIFY(waitLoaded(w));
        QStringList n = names(w->collection());
        n.sort();
        QCOMPARE(n, (QStringList{"IMG_1.png", "IMG_10.jpg", "IMG_2.jpg"}));
    }

    // Covers: F-OPEN-04, F-OPEN-01 (QFileOpenEvent = Finder double click / Dock drop)
    void F_OPEN_04_multiFileOpenGroupedWithin150ms() {
        makeStandardFolder();
        QVERIFY(!makeImage(path("IMG_4.png"), Qt::gray).isEmpty()); // in the folder, not opened
        // Deterministic timing: the interval cannot run out between the events however slow
        // the machine is; the end of the interval is triggered explicitly below.
        app->setOpenGroupingIntervalForTesting(60 * 60 * 1000);
        for (const char* n : {"IMG_10.jpg", "IMG_1.png"}) {
            QFileOpenEvent ev(path(QString::fromLatin1(n)));
            QCoreApplication::sendEvent(qApp, &ev);
            QTest::qWait(30);
        }
        QFileOpenEvent ev(path("IMG_2.jpg"));
        QCoreApplication::sendEvent(qApp, &ev);
        // The first file is shown immediately, the group follows after the interval.
        QCOMPARE(app->windows().size(), 1);
        ViewerWindow* w = app->windows().first();
        QVERIFY(app->isOpenBurstPending());
        app->flushOpenBurstForTesting(); // the interval ends
        QVERIFY(!app->isOpenBurstPending());
        QTRY_VERIFY_WITH_TIMEOUT(!w->collection()->isFolderCollection(), 10000); // exactly these files
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->collection()->count(), 3);
        QCOMPARE(app->windows().size(), 1);
        QCOMPARE(w->currentPath(), path("IMG_10.jpg")); // first requested stays current

        // Requests in separate intervals are separate (new window each, F-WIN-02).
        QFileOpenEvent a(path("IMG_1.png"));
        QCoreApplication::sendEvent(qApp, &a);
        app->flushOpenBurstForTesting();
        QFileOpenEvent b(path("IMG_2.jpg"));
        QCoreApplication::sendEvent(qApp, &b);
        app->flushOpenBurstForTesting();
        QTRY_COMPARE(app->windows().size(), 3);
        // The real interval is the specified one, and it ends by itself.
        app->setOpenGroupingIntervalForTesting(0);
        QFileOpenEvent c(path("IMG_4.png"));
        QCoreApplication::sendEvent(qApp, &c);
        QVERIFY(app->isOpenBurstPending());
        QTRY_VERIFY_WITH_TIMEOUT(!app->isOpenBurstPending(), Application::kOpenGroupingMs * 20);
        QTRY_COMPARE(app->windows().size(), 4);
        for (ViewerWindow* x : app->windows().mid(1)) {
            QVERIFY(waitLoaded(x));
            QVERIFY(x->collection()->isFolderCollection());
            QCOMPARE(x->collection()->count(), 4);
        }
    }

    // Covers: F-OPEN-05
    void F_OPEN_05_folderOpensFirstImage() {
        makeStandardFolder();
        QFileOpenEvent ev(dir->path()); // folder dropped on the Dock icon
        QCoreApplication::sendEvent(qApp, &ev);
        // Folders are classified on a worker thread first (no file system access on the GUI thread).
        QTRY_COMPARE_WITH_TIMEOUT(app->windows().size(), 1, 3000);
        ViewerWindow* w = app->windows().first();
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->collection()->count(), 3);
        QCOMPARE(w->collection()->currentIndex(), 0);
        QCOMPARE(w->currentPath(), w->collection()->at(0).path);
        QVERIFY(waitShown(w, w->collection()->at(0).path));
    }

    // Covers: F-OPEN-01 (command line, start window, drop on the window), F-OPEN-12 (simple)
    void F_OPEN_01_commandLineStartWindowAndDrop() {
        makeStandardFolder();
        app->start({QStringLiteral("Lumaflip"), QStringLiteral("-psn_0_123"), path("IMG_2.jpg")});
        QCOMPARE(app->windows().size(), 1);
        QCOMPARE(app->windows().first()->currentPath(), path("IMG_2.jpg"));
        app->closeAllWindows();
        QCOMPARE(app->windows().size(), 0);

        // Without arguments an empty start window appears (drop area + "Öffnen …").
        app->start({QStringLiteral("Lumaflip")});
        QTRY_COMPARE_WITH_TIMEOUT(app->windows().size(), 1, 2000);
        ViewerWindow* w = app->windows().first();
        QVERIFY(w->isEmpty());
        QVERIFY(waitWindowShown(w));
        auto* empty = w->findChild<QWidget*>(QStringLiteral("lfEmptyState"));
        QVERIFY(empty && empty->isVisible());
        QVERIFY(w->findChild<QPushButton*>(QStringLiteral("lfOpenButton")));
        QCOMPARE(w->windowTitle(), QStringLiteral("Lumaflip"));
        // An open request reuses the empty start window.
        QFileOpenEvent ev(path("IMG_1.png"));
        QCoreApplication::sendEvent(qApp, &ev);
        QCOMPARE(app->windows().size(), 1);
        QCOMPARE(w->currentPath(), path("IMG_1.png"));
        QVERIFY(!empty->isVisible());

        // Drop onto the window opens there.
        auto* mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(path("IMG_10.jpg"))});
        const QPoint pos = w->view()->geometry().center();
        QDragEnterEvent enter(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &enter);
        QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &drop);
        QCOMPARE(w->currentPath(), path("IMG_10.jpg"));
        QCOMPARE(app->windows().size(), 1);
        delete mime;
    }

    // ---- navigation ----

    // Covers: F-NAV-01, F-NAV-02, F-NAV-03, F-NAV-05
    void F_NAV_01_keyboardNavigation() {
        for (int i = 1; i <= 5; ++i)
            QVERIFY(!makeImage(path(QStringLiteral("p%1.png").arg(i)), QColor(40 * i, 0, 0)).isEmpty());
        ViewerWindow* w = app->openPaths({path("p1.png")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitLoaded(w));
        Collection* c = w->collection();
        if (c->sortSpec().key != SortKey::Name || !c->sortSpec().ascending) {
            w->setSortKey(SortKey::Name);
            w->setSortAscending(true);
            QVERIFY(waitLoaded(w));
        }
        const QString first = c->at(0).path, last = c->at(4).path;
        c->setCurrentPath(first);

        key(w, Qt::Key_Right); // F-NAV-01
        QCOMPARE(c->currentIndex(), 1);
        key(w, Qt::Key_Left);
        QCOMPARE(c->currentIndex(), 0);
        // F-NAV-03: stop at the start with a short visual hint
        key(w, Qt::Key_Left);
        QCOMPARE(c->currentIndex(), 0);
        QCOMPARE(w->toastText(), QStringLiteral("Erstes Bild"));
        // F-NAV-02: End/Home, ⌘↓/⌘↑
        key(w, Qt::Key_End);
        QCOMPARE(c->currentPath(), last);
        key(w, Qt::Key_Right);
        QCOMPARE(c->currentPath(), last);
        QCOMPARE(w->toastText(), QStringLiteral("Letztes Bild"));
        key(w, Qt::Key_Home);
        QCOMPARE(c->currentPath(), first);
        menuShortcut(Action::LastImage, QKeySequence(Qt::ControlModifier | Qt::Key_Down));
        QCOMPARE(c->currentPath(), last);
        menuShortcut(Action::FirstImage, QKeySequence(Qt::ControlModifier | Qt::Key_Up));
        QCOMPARE(c->currentPath(), first);
        // F-NAV-05 default inputs: Page keys, space, ↓
        key(w, Qt::Key_PageDown);
        QCOMPARE(c->currentIndex(), 1);
        key(w, Qt::Key_Space);
        QCOMPARE(c->currentIndex(), 2);
        key(w, Qt::Key_Space, Qt::ShiftModifier);
        QCOMPARE(c->currentIndex(), 1);
        key(w, Qt::Key_PageUp);
        QCOMPARE(c->currentIndex(), 0);
        key(w, Qt::Key_Down);
        QCOMPARE(c->currentIndex(), 1);
        // F-NAV-01: holding the key (auto repeat) keeps flipping
        for (int i = 0; i < 3; ++i) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, QString(), true);
            QCoreApplication::sendEvent(w->view(), &press);
        }
        QCOMPARE(c->currentIndex(), 4);
        // F-NAV-03: wrap around when configured
        Settings::instance().setValue(key::WrapMode, QStringLiteral("wrap"));
        key(w, Qt::Key_Right);
        QCOMPARE(c->currentPath(), first);
        key(w, Qt::Key_Left);
        QCOMPARE(c->currentPath(), last);
        QVERIFY(waitShown(w, last));
    }

    // Covers: F-IN-01, F-IN-03, F-NAV-05 (mouse wheel)
    void F_IN_01_wheelSetting() {
        for (int i = 1; i <= 4; ++i)
            QVERIFY(!makeImage(path(QStringLiteral("w%1.png").arg(i)), QColor(0, 50 * i, 0)).isEmpty());
        ViewerWindow* w = app->openPaths({path("w1.png")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitLoaded(w));
        w->setSortKey(SortKey::Name);
        QVERIFY(waitLoaded(w));
        w->collection()->setCurrentIndex(0);
        auto* canvas = w->view()->findChild<ImageCanvas*>();
        QVERIFY(canvas);
        auto wheel = [&](int dy, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            const QPointF p(canvas->width() / 2.0, canvas->height() / 2.0);
            QWheelEvent e(p, canvas->mapToGlobal(p), QPoint(), QPoint(0, dy), Qt::NoButton, mods,
                          Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(canvas, &e);
        };
        // Default "Bild wechseln": down = next.
        wheel(-120);
        QCOMPARE(w->collection()->currentIndex(), 1);
        // F-IN-03: minimum interval (120 ms) swallows a second notch right after.
        wheel(-120);
        QCOMPARE(w->collection()->currentIndex(), 1);
        QTest::qWait(150);
        wheel(120);
        QCOMPARE(w->collection()->currentIndex(), 0);
        // Setting "Zoomen": the wheel zooms instead of flipping.
        QVERIFY(waitShown(w, w->collection()->at(0).path));
        Settings::instance().setValue(QStringLiteral("input/wheel"), QStringLiteral("zoom"));
        const double before = w->view()->zoomFactor();
        wheel(120);
        QCOMPARE(w->collection()->currentIndex(), 0);
        QVERIFY2(w->view()->zoomFactor() > before * 1.1,
                 qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(w->view()->zoomFactor())));
    }

    // ---- title, status, sorting ----

    // Covers: F-NAV-07, F-INFO-01, F-SORT-05, F-SORT-02, F-WIN-10 (title)
    void F_NAV_07_titleAndStatus() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitLoaded(w));
        w->setSortFollowFinder();
        QVERIFY(waitLoaded(w));
        QVERIFY(waitShown(w, path("IMG_2.jpg")));
        const int pos = w->collection()->currentIndex() + 1;
        QCOMPARE(w->windowTitle(), QStringLiteral("IMG_2.jpg — %1 / 3").arg(pos));
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("64 × 48")));
        const QString status = w->statusText();
        QVERIFY2(status.contains(QStringLiteral("IMG_2.jpg")), qPrintable(status));
        QVERIFY2(status.contains(QStringLiteral("%1 / 3").arg(pos)), qPrintable(status));
        QVERIFY2(status.contains(QStringLiteral(" %")), qPrintable(status));       // zoom
        const QString bytes = formatByteSize(QFileInfo(path("IMG_2.jpg")).size()); // German units
        QVERIFY2(status.contains(bytes), qPrintable(status));                      // file size
        // F-SORT-05: detected order is shown
        const QString sort = w->sortText();
        QCOMPARE(sort, describe(w->collection()->sortSpec()));
        QVERIFY2(sort.startsWith(QStringLiteral("Wie Finder: ")) ||
                     sort.contains(QStringLiteral("Finder-Info")),
                 qPrintable(sort));
        QVERIFY(status.contains(sort));

        // Manual sort via the menu (F-SORT-02): name descending.
        app->windowActivated(w);
        command("sort:name")->trigger();
        QVERIFY(waitLoaded(w));
        command("sort:desc")->trigger();
        QVERIFY(waitLoaded(w));
        QCOMPARE(names(w->collection()), (QStringList{"IMG_10.jpg", "IMG_2.jpg", "IMG_1.png"}));
        QCOMPARE(w->currentPath(), path("IMG_2.jpg")); // the current image stays
        QCOMPARE(w->sortText(), QStringLiteral("Name ↓"));
        QCOMPARE(w->windowTitle(), QStringLiteral("IMG_2.jpg — 2 / 3"));
        app->menu()->refreshState();
        QVERIFY(command("sort:name")->isChecked());
        QVERIFY(!command("sort:finder")->isChecked());
        command("sort:size")->trigger();
        QVERIFY(waitLoaded(w));
        QCOMPARE(w->sortText(), QStringLiteral("Größe ↓"));
        command("sort:finder")->trigger();
        QVERIFY(waitLoaded(w));
        QVERIFY(w->collection()->followsFinder());
        // Status bar can be switched off (F-INFO-01).
        command("statusBar")->trigger();
        QVERIFY(w->statusBar()->isHidden());
        command("statusBar")->trigger();
        QVERIFY(!w->statusBar()->isHidden());
    }

    // ---- view ----

    // Covers: F-VIEW-01, F-VIEW-02, F-VIEW-09
    void F_VIEW_02_zoomShortcuts() {
        QVERIFY(!makeImage(path("big.png"), Qt::darkCyan, QSize(3000, 2000)).isEmpty());
        ViewerWindow* w = app->openPaths({path("big.png")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("big.png")));
        ImageView* v = w->view();
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        key(w, Qt::Key_3);
        QCOMPARE(v->zoomMode(), ZoomMode::Actual);
        QCOMPARE(v->zoomFactor(), 1.0);
        key(w, Qt::Key_1);
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        key(w, Qt::Key_2);
        QCOMPARE(v->zoomMode(), ZoomMode::FillWidth);
        menuShortcut(Action::ZoomActual, QKeySequence(Qt::ControlModifier | Qt::Key_0));
        QCOMPARE(v->zoomMode(), ZoomMode::Actual);
        menuShortcut(Action::ZoomFit, QKeySequence(Qt::ControlModifier | Qt::Key_9));
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        const double fit = v->zoomFactor();
        menuShortcut(Action::ZoomIn, QKeySequence(Qt::ControlModifier | Qt::Key_Plus));
        QVERIFY(v->zoomFactor() > fit);
        menuShortcut(Action::ZoomOut, QKeySequence(Qt::ControlModifier | Qt::Key_Minus));
        QVERIFY(qAbs(v->zoomFactor() - fit) < fit * 0.5);
        key(w, Qt::Key_Plus);
        QVERIFY(v->zoomFactor() > fit);
        // Modes without a default key are in the menu (F-VIEW-01).
        command("zoom:fillHeight")->trigger();
        QCOMPARE(v->zoomMode(), ZoomMode::FillHeight);
        command("zoom:fitUpscale")->trigger();
        QCOMPARE(v->zoomMode(), ZoomMode::FitUpscale);
        command("zoom:fill")->trigger();
        QCOMPARE(v->zoomMode(), ZoomMode::Fill);
        // F-VIEW-04: ⌥← pans when zoomed instead of flipping
        key(w, Qt::Key_3);
        QVERIFY(v->isZoomed());
        QVERIFY(v->atEdge(Qt::LeftEdge) == false || v->atEdge(Qt::RightEdge) == false);
        // F-VIEW-09: background from the menu
        command("background:checker")->trigger();
        QCOMPARE(Settings::instance().value(key::Background).toString(), QStringLiteral("checker"));
        command("background:dark")->trigger();
        QCOMPARE(Settings::instance().value(key::Background).toString(), QStringLiteral("dark"));
    }

    // Covers: F-VIEW-10
    void F_VIEW_10_fullscreenToggle() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(w->chromeVisible());
        key(w, Qt::Key_F);
        QVERIFY(w->isFullScreen());
        QVERIFY(!w->chromeVisible()); // pure image
        (void)QTest::qWaitFor([w] { return w->windowHandle()->windowStates() & Qt::WindowFullScreen; }, 3000);
        QTest::qWait(800); // native fullscreen animation
        // Mouse at the bottom edge reveals the chrome, it hides after 1.5 s idle.
        auto* canvas = w->view()->findChild<ImageCanvas*>();
        const QPoint edge(canvas->width() / 2, canvas->height() - 5);
        QMouseEvent move(QEvent::MouseMove, QPointF(edge), canvas->mapToGlobal(QPointF(edge)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &move);
        QVERIFY(w->chromeVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!w->chromeVisible(), 3000);
        // Esc leaves fullscreen.
        key(w, Qt::Key_Escape);
        QVERIFY(!w->isFullScreen());
        QVERIFY(w->chromeVisible());
        (void)QTest::qWaitFor([w] { return !(w->windowHandle()->windowStates() & Qt::WindowFullScreen); },
                              3000);
        QTest::qWait(800);
        // Double click toggles too (default of F-IN-10).
        QTest::mouseDClick(canvas, Qt::LeftButton, {}, QPoint(20, 20));
        QVERIFY(w->isFullScreen());
        // Let the native transition finish first (slow machines, CI): macOS would apply
        // its end after the toggle below and leave the window in fullscreen.
        (void)QTest::qWaitFor([w] { return w->windowHandle()->windowStates() & Qt::WindowFullScreen; }, 3000);
        QTest::qWait(800);
        w->toggleFullScreen();
        QVERIFY(!w->isFullScreen());
        (void)QTest::qWaitFor([w] { return !(w->windowHandle()->windowStates() & Qt::WindowFullScreen); },
                              3000);
        QTest::qWait(800);
    }

    // Covers: F-EDIT-01
    void F_EDIT_01_rotateAndFlipViewOnly() {
        makeStandardFolder();
        const QByteArray before = sha(path("IMG_2.jpg"));
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("IMG_2.jpg")));
        QVERIFY(waitLoaded(w));
        menuShortcut(Action::RotateRight, QKeySequence(Qt::ControlModifier | Qt::Key_R));
        QCOMPARE(w->view()->rotation(), 1);
        menuShortcut(Action::RotateLeft, QKeySequence(Qt::ControlModifier | Qt::Key_L));
        menuShortcut(Action::RotateLeft, QKeySequence(Qt::ControlModifier | Qt::Key_L));
        QCOMPARE(w->view()->rotation(), 3);
        key(w, Qt::Key_H);                                            // flip horizontally
        QVERIFY(w->statusText().contains(QStringLiteral("64 × 48"))); // pixel size is the file's
        QCOMPARE(sha(path("IMG_2.jpg")), before);                     // view only, the file is untouched
        // The next image starts unrotated.
        const QString next = w->collection()->at((w->collection()->currentIndex() + 1) % 3).path;
        w->collection()->setCurrentPath(next);
        QVERIFY(waitShown(w, next));
        QCOMPARE(w->view()->rotation(), 0);
    }

    // ---- file operations ----

    // Covers: F-FILE-01, F-WIN-09 (all windows of the folder update)
    void F_FILE_01_trashAndUndoInTwoWindows() {
        makeStandardFolder();
        ViewerWindow* w1 = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w1));
        QVERIFY(waitLoaded(w1));
        w1->setSortKey(SortKey::Name);
        QVERIFY(waitLoaded(w1));
        ViewerWindow* w2 = app->duplicateWindow(w1);
        QVERIFY(waitWindowShown(w2));
        QVERIFY(waitLoaded(w2));
        QCOMPARE(w2->collection()->count(), 3);
        w2->collection()->setCurrentPath(path("IMG_1.png"));
        QCOMPARE(w1->currentPath(), path("IMG_2.jpg"));

        InputEvent del;
        del.key = Qt::Key_Delete;
        QCOMPARE(app->bindings().resolve(del), Action::Trash);
        QSignalSpy inputs(w1->view(), &ImageView::userInput);
        key(w1, Qt::Key_Delete); // "Entf"
        QCOMPARE(inputs.size(), 1);
        // The trash runs on a worker thread (network volumes must not block the GUI).
        QTRY_VERIFY2(!QFileInfo::exists(path("IMG_2.jpg")), qPrintable(w1->toastText()));
        QTRY_COMPARE(w1->collection()->count(), 2);
        QTRY_COMPARE(trash->locations.size(), 1); // recorded on the worker after the move
        QTRY_COMPARE(w2->collection()->count(), 2);
        QTRY_COMPARE(w1->currentPath(), path("IMG_10.jpg")); // next image
        QTRY_COMPARE(w2->currentPath(), path("IMG_1.png"));
        QVERIFY(waitShown(w1, path("IMG_10.jpg")));

        // Undo (⌘Z) restores the file and shows it again in the window that trashed it.
        app->windowActivated(w1);
        menuShortcut(Action::Undo, QKeySequence(Qt::ControlModifier | Qt::Key_Z));
        QTRY_VERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        QTRY_COMPARE(w1->collection()->count(), 3);
        QTRY_COMPARE(w2->collection()->count(), 3);
        QTRY_COMPARE(w1->currentPath(), path("IMG_2.jpg"));
        QTRY_COMPARE(w2->currentPath(), path("IMG_1.png"));
        QVERIFY(waitShown(w1, path("IMG_2.jpg")));

        // ⌘⌫ is the menu shortcut; redo trashes again, undo restores.
        menuShortcut(Action::Trash, QKeySequence(Qt::ControlModifier | Qt::Key_Backspace));
        QTRY_VERIFY(!QFileInfo::exists(path("IMG_2.jpg")));
        menuShortcut(Action::Undo, QKeySequence(Qt::ControlModifier | Qt::Key_Z));
        QTRY_VERIFY(QFileInfo::exists(path("IMG_2.jpg")));
        // The folder watcher may refresh as well; the state must stay consistent.
        QTest::qWait(300);
        QVERIFY(waitLoaded(w1));
        QTRY_COMPARE(w1->collection()->count(), 3);
        QTRY_COMPARE(w2->collection()->count(), 3);
    }

    // Covers: F-FILE-02
    void F_FILE_02_revealInFinder() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        menuShortcut(Action::RevealInFinder,
                     QKeySequence(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_R));
        QCOMPARE(shell->revealed, QStringList{path("IMG_2.jpg")});
    }

    // Covers: F-FILE-03
    void F_FILE_03_copyImageAndPath() {
        makeStandardFolder();
        QClipboard* cb = QGuiApplication::clipboard();
        const QString savedText = cb->text();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("IMG_2.jpg")));
        menuShortcut(Action::CopyImage, QKeySequence(Qt::ControlModifier | Qt::Key_C));
        // Read and decoded on a worker thread; the clipboard follows.
        QTRY_VERIFY(w->toastText() == QStringLiteral("Bild kopiert"));
        const QMimeData* md = cb->mimeData();
        QVERIFY(md);
        QVERIFY(md->hasUrls());
        QCOMPARE(md->urls().value(0).toLocalFile(), path("IMG_2.jpg"));
        QVERIFY(md->hasImage());
        const QImage img = qvariant_cast<QImage>(md->imageData());
        QCOMPARE(img.size(), QSize(64, 48));
        menuShortcut(Action::CopyPath, QKeySequence(Qt::AltModifier | Qt::ControlModifier | Qt::Key_C));
        QCOMPARE(cb->text(), path("IMG_2.jpg"));
        cb->setText(savedText);
    }

    // ---- errors and privacy ----

    // Covers: F-FMT-02
    void F_FMT_02_errorCardForBrokenFile() {
        QVERIFY(!makeImage(path("a.png"), Qt::red).isEmpty());
        QVERIFY(!writeBytes(path("b_broken.jpg"), QByteArray("this is not a jpeg").repeated(20)).isEmpty());
        QVERIFY(!makeImage(path("c.png"), Qt::blue).isEmpty());
        ViewerWindow* w = app->openPaths({path("b_broken.jpg")});
        QVERIFY(waitWindowShown(w));
        QTRY_VERIFY_WITH_TIMEOUT(w->view()->isShowingError(), 5000);
        auto* title = w->view()->findChild<QLabel*>(QStringLiteral("lfErrorTitle"));
        QVERIFY(title && title->text().contains(QStringLiteral("b_broken.jpg")));
        auto* reveal = w->view()->findChild<QPushButton*>(QStringLiteral("lfErrorAction"));
        QVERIFY(reveal && reveal->isVisible());
        QCOMPARE(reveal->text(), QStringLiteral("Im Finder zeigen"));
        reveal->click();
        QCOMPARE(shell->revealed, QStringList{path("b_broken.jpg")});
        // Flipping continues.
        QVERIFY(waitLoaded(w));
        w->setSortKey(SortKey::Name);
        QVERIFY(waitLoaded(w));
        key(w, Qt::Key_Right);
        QVERIFY(waitShown(w, path("c.png")));
        QVERIFY(!w->view()->isShowingError());
        key(w, Qt::Key_Left);
        QTRY_VERIFY(w->view()->isShowingError());
    }

    // Covers: F-PRIV-01, F-PRIV-02
    void F_PRIV_01_accessDeniedHint() {
#ifdef Q_OS_UNIX
        if (::geteuid() == 0)
            QSKIP("root ignores permissions");
        const QString locked = path("locked");
        QVERIFY(QDir().mkpath(locked));
        QVERIFY(!makeImage(locked + "/a.png", Qt::red).isEmpty());
        QVERIFY(!makeImage(locked + "/b.png", Qt::green).isEmpty());
        // Listable: no; files reachable by path: yes (like a TCC-protected folder after
        // the user opened one file in Finder).
        QCOMPARE(::chmod(QFile::encodeName(locked).constData(), 0100), 0);
        ViewerWindow* w = app->openPaths({locked + "/b.png"});
        const bool loaded = w && waitLoaded(w);
        const bool shown = w && waitShown(w, locked + "/b.png");
        ::chmod(QFile::encodeName(locked).constData(), 0755);
        if (!w) // not QVERIFY: the static analyzer does not see that it returns
            QFAIL("no window");
        QVERIFY(loaded);
        QVERIFY(shown); // no error state: the opened file is shown
        QCOMPARE(w->collection()->count(), 1);
        QVERIFY(waitWindowShown(w));
        QVERIFY(w->accessHint()->isVisible());
        auto* text = w->accessHint()->findChild<QLabel*>(QStringLiteral("lfAccessText"));
        QVERIFY(text);
        QVERIFY(text->text().contains(QStringLiteral("locked")));
        auto* allow = w->accessHint()->findChild<QPushButton*>(QStringLiteral("lfAllowAccess"));
        QVERIFY(allow);
        // chmod = POSIX permissions (EACCES): System Settings cannot help, no button.
        QVERIFY(!w->accessHintIsPrivacy());
        QVERIFY(!allow->isVisible());
        QVERIFY2(text->text().contains(QStringLiteral("Zugriffsrechte")), qPrintable(text->text()));
        QVERIFY(!text->text().contains(QStringLiteral("Datenschutz")));
        // macOS privacy protection (TCC answers EPERM): hint with the settings button.
        emit w->collection()->accessDenied(locked, true);
        QVERIFY(w->accessHintIsPrivacy());
        QVERIFY(allow->isVisible());
        QVERIFY(text->text().contains(
            QStringLiteral("Festplattenvollzugriff ist dafür nicht nötig"))); // F-PRIV-02
        QCOMPARE(allow->text(), QStringLiteral("Zugriff erlauben …"));
        allow->click();
        QCOMPARE(openedUrls.size(), 1);
        QCOMPARE(openedUrls.first().scheme(), QStringLiteral("x-apple.systempreferences"));
        QVERIFY(openedUrls.first().toString().contains(QStringLiteral("Privacy_FilesAndFolders")));
        QVERIFY(!openedUrls.first().toString().contains(QStringLiteral("AllFiles"))); // no Full Disk Access
        w->accessHint()->findChild<QPushButton*>(QStringLiteral("lfDismissAccess"))->click();
        QVERIFY(!w->accessHint()->isVisible());
#endif
    }

    // ---- windows ----

    // Covers: F-WIN-01, F-WIN-03, F-WIN-08, F-MAC-03
    void F_WIN_01_independentWindowsDuplicateAndCascade() {
        makeStandardFolder();
        QTemporaryDir other;
        QVERIFY(!makeImage(other.filePath("x1.png"), Qt::white).isEmpty());
        QVERIFY(!makeImage(other.filePath("x2.png"), Qt::black).isEmpty());
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        ViewerWindow* b = app->openPaths({other.filePath("x1.png")});
        QVERIFY(a != b);
        QVERIFY(waitWindowShown(a) && waitWindowShown(b));
        QVERIFY(waitLoaded(a) && waitLoaded(b));
        QVERIFY(a->pos() != b->pos()); // F-WIN-08
        const QString aPath = a->currentPath();
        key(b, Qt::Key_Right);
        QCOMPARE(a->currentPath(), aPath); // independent collections
        key(b, Qt::Key_3);
        QCOMPARE(b->view()->zoomMode(), ZoomMode::Actual);
        QCOMPARE(a->view()->zoomMode(), ZoomMode::Fit); // independent zoom
        b->setSortKey(SortKey::Size);
        QVERIFY(a->collection()->followsFinder()); // independent sorting

        // ⌥⌘N duplicates folder, image and zoom.
        app->windowActivated(b);
        menuShortcut(Action::DuplicateWindow,
                     QKeySequence(Qt::AltModifier | Qt::ControlModifier | Qt::Key_N));
        QCOMPARE(app->windows().size(), 3);
        ViewerWindow* c = app->frontWindow();
        QVERIFY(c != a && c != b);
        QVERIFY(waitWindowShown(c));
        QCOMPARE(c->currentPath(), b->currentPath());
        QCOMPARE(c->collection()->count(), b->collection()->count());
        QCOMPARE(c->view()->zoomMode(), ZoomMode::Actual);
        QVERIFY(!c->collection()->isLoading()); // no second enumeration (F-WIN-09)
        // macOS may still move a new window into the screen right after showing it.
        QTRY_VERIFY(c->pos() != a->pos() && c->pos() != b->pos());

        // ⌘N: empty window, cascaded as well.
        menuShortcut(Action::NewWindow, QKeySequence(Qt::ControlModifier | Qt::Key_N));
        QCOMPARE(app->windows().size(), 4);
        ViewerWindow* d = app->frontWindow();
        QVERIFY(d->isEmpty());
        QVERIFY(waitWindowShown(d));
        // As above: macOS may still move the new window right after showing it.
        auto distinctPositions = [&] {
            QSet<QPoint> positions;
            for (ViewerWindow* x : app->windows())
                positions.insert(x->pos());
            return positions.size();
        };
        QTRY_COMPARE(distinctPositions(), 4);

        // ⌘W closes the front window, ⌥⌘W all.
        menuShortcut(Action::CloseWindow, QKeySequence(Qt::ControlModifier | Qt::Key_W));
        QCOMPARE(app->windows().size(), 3);
        menuShortcut(Action::CloseAllWindows,
                     QKeySequence(Qt::AltModifier | Qt::ControlModifier | Qt::Key_W));
        QCOMPARE(app->windows().size(), 0);
        // The menu still works without windows.
        menuShortcut(Action::NewWindow, QKeySequence(Qt::ControlModifier | Qt::Key_N));
        QCOMPARE(app->windows().size(), 1);
    }

    // Covers: F-WIN-02
    void F_WIN_02_openBehavior() {
        makeStandardFolder();
        QTemporaryDir other;
        QVERIFY(!makeImage(other.filePath("o.png"), Qt::white).isEmpty());
        Settings& st = Settings::instance();
        // Default: always a new window.
        QCOMPARE(st.value(key::OpenBehavior).toString(), QStringLiteral("newWindow"));
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        ViewerWindow* b = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(a != b);
        QCOMPARE(app->windows().size(), 2);
        b->close();
        QCOMPARE(app->windows().size(), 1);

        // New window unless the file is already open: that window comes to the front.
        st.setValue(key::OpenBehavior, QStringLiteral("newUnlessOpen"));
        QVERIFY(waitLoaded(a));
        ViewerWindow* again = app->openPaths({path("IMG_10.jpg")});
        QCOMPARE(again, a);
        QCOMPARE(a->currentPath(), path("IMG_10.jpg"));
        QCOMPARE(app->frontWindow(), a);
        ViewerWindow* c = app->openPaths({other.filePath("o.png")});
        QVERIFY(c != a);
        QCOMPARE(app->windows().size(), 2);

        // Replace the front window.
        st.setValue(key::OpenBehavior, QStringLiteral("replaceFront"));
        app->windowActivated(c);
        ViewerWindow* r = app->openPaths({path("IMG_1.png")});
        QCOMPARE(r, c);
        QCOMPARE(app->windows().size(), 2);
        QCOMPARE(c->currentPath(), path("IMG_1.png"));
    }

    // Covers: F-WIN-08 (option "Fenstergröße an Bild anpassen")
    void F_WIN_08_fitWindowToImageOption() {
        // 600 x 400 points: fits at 100 % even on small screens (CI runners: 1024 x 768),
        // where fitWindowToImage() would otherwise cap the window at 90 % of the screen.
        QScreen* primary = QGuiApplication::primaryScreen();
        QVERIFY(primary);
        const QSize pixels = (QSizeF(600, 400) * primary->devicePixelRatio()).toSize();
        QVERIFY(!makeImage(path("wide.png"), Qt::darkGreen, pixels).isEmpty());
        ViewerWindow* plain = app->openPaths({path("wide.png")});
        QVERIFY(waitWindowShown(plain));
        QVERIFY(waitShown(plain, path("wide.png")));
        // The new window cascades with this size; on small screens (CI: 1024 x 768) the
        // default size fills the screen and macOS treats the window as zoomed.
        plain->resize(500, 350);
        const QSize normal = plain->size();
        Settings::instance().setValue(key::FitWindowToImage, true);
        ViewerWindow* w = app->openPaths({path("wide.png")});
        QVERIFY(w != plain);
        QVERIFY(waitWindowShown(w));
        QVERIFY(waitShown(w, path("wide.png")));
        const qreal dpr = w->screen()->devicePixelRatio();
        const QSize expected = (QSizeF(pixels) / dpr).toSize();
        QTRY_VERIFY2(qAbs(w->view()->width() - expected.width()) <= 2 &&
                         qAbs(w->view()->height() - expected.height()) <= 2,
                     qPrintable(QStringLiteral("%1x%2 vs %3x%4 (normal %5x%6)")
                                    .arg(w->view()->width())
                                    .arg(w->view()->height())
                                    .arg(expected.width())
                                    .arg(expected.height())
                                    .arg(normal.width())
                                    .arg(normal.height())));
    }

    // Covers: F-WIN-09, F-OPEN-07
    void F_WIN_09_sharedResourcesAndFolderWatching() {
        makeStandardFolder();
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        QVERIFY(waitWindowShown(a));
        // The same folder in a second window is not enumerated again: the window takes the
        // other one's entries at once. (It loads only if the first window is reloading right
        // now - a late watcher event for the files just created - and then shares that load.)
        const bool aLoading = a->collection()->isLoading();
        ViewerWindow* b = app->openPaths({path("IMG_10.jpg")});
        QVERIFY(a != b);
        QCOMPARE(b->collection()->isLoading(), aLoading);
        QCOMPARE(b->collection()->count(), aLoading ? 1 : 3);
        QVERIFY(waitLoaded(b));
        QCOMPARE(b->collection()->count(), 3);
        QCOMPARE(b->currentPath(), path("IMG_10.jpg"));
        QCOMPARE(a->currentPath(), path("IMG_2.jpg"));
        // One provider for all windows: an image decoded for one window is cached for the other.
        QVERIFY(waitShown(a, path("IMG_2.jpg")));
        QVERIFY(app->provider().cached(path("IMG_2.jpg")) != nullptr);
        QVERIFY(&app->provider() == &ImageProvider::instance());
        // F-OPEN-07: a new file appears in both windows, current images stay.
        QVERIFY(!makeImage(path("IMG_3.png"), Qt::magenta).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(a->collection()->count(), 4, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(b->collection()->count(), 4, 8000);
        QCOMPARE(a->currentPath(), path("IMG_2.jpg"));
        QCOMPARE(b->currentPath(), path("IMG_10.jpg"));
    }

    // Covers: F-WIN-10
    void F_WIN_10_windowAndDockMenuList() {
        makeStandardFolder();
        ViewerWindow* a = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitLoaded(a));
        ViewerWindow* b = app->newWindow();
        app->menu()->refreshWindowList();
        const QStringList titles = app->menu()->windowMenuTitles();
        QCOMPARE(titles.size(), 2);
        QVERIFY(titles.contains(a->windowTitle()));
        QVERIFY(titles.contains(b->windowTitle()));
        QVERIFY(a->windowTitle().startsWith(QStringLiteral("IMG_2.jpg — ")));
        QCOMPARE(app->menu()->dockMenuTitles(), titles);
        // Titles follow navigation.
        key(a, Qt::Key_Right);
        QTRY_VERIFY(app->menu()->windowMenuTitles().contains(a->windowTitle()));
        // Choosing an entry brings the window to the front.
        auto* menu = app->menu()->menus().at(6); // after "Aussortieren" (M3)
        QCOMPARE(menu->title(), QStringLiteral("Fenster"));
        for (QAction* act : menu->actions())
            if (act->text() == a->windowTitle())
                act->trigger();
        QCOMPARE(app->frontWindow(), a);
    }

    // ---- macOS integration ----

    // Covers: F-MAC-01, F-SET-01, F-SET-02 (menu follows changed shortcuts)
    void F_MAC_01_menuBarContentsAndShortcuts() {
        const QList<QMenu*> menus = app->menu()->menus();
        QStringList titles;
        for (QMenu* m : menus)
            titles << m->title();
        QCOMPARE(titles, (QStringList{"Ablage", "Bearbeiten", "Darstellung", "Navigation", "Bild",
                                      "Aussortieren", "Fenster", "Hilfe"}));
        auto has = [&](Action a, const QKeySequence& seq) {
            QAction* act = action(a);
            return act && act->shortcuts().contains(seq);
        };
        using K = QKeySequence;
        QVERIFY(has(Action::NewWindow, K(Qt::ControlModifier | Qt::Key_N)));
        QVERIFY(has(Action::DuplicateWindow, K(Qt::AltModifier | Qt::ControlModifier | Qt::Key_N)));
        QVERIFY(has(Action::CloseWindow, K(Qt::ControlModifier | Qt::Key_W)));
        QVERIFY(has(Action::Trash, K(Qt::ControlModifier | Qt::Key_Backspace)));
        QVERIFY(has(Action::Trash, K(Qt::Key_Delete)));
        QVERIFY(has(Action::RevealInFinder, K(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_R)));
        QVERIFY(has(Action::Undo, K(Qt::ControlModifier | Qt::Key_Z)));
        QVERIFY(has(Action::Redo, K(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_Z)));
        QVERIFY(has(Action::CopyImage, K(Qt::ControlModifier | Qt::Key_C)));
        QVERIFY(has(Action::CopyPath, K(Qt::AltModifier | Qt::ControlModifier | Qt::Key_C)));
        QVERIFY(has(Action::ZoomActual, K(Qt::ControlModifier | Qt::Key_0)));
        QVERIFY(has(Action::ZoomFit, K(Qt::ControlModifier | Qt::Key_9)));
        QVERIFY(has(Action::ToggleFullScreen, K(Qt::MetaModifier | Qt::ControlModifier | Qt::Key_F)));
        QVERIFY(has(Action::RotateLeft, K(Qt::ControlModifier | Qt::Key_L)));
        QVERIFY(has(Action::RotateRight, K(Qt::ControlModifier | Qt::Key_R)));
        QVERIFY(has(Action::FlipHorizontal, K(Qt::Key_H)));
        QVERIFY(has(Action::NextImage, K(Qt::Key_Right)));
        QVERIFY(has(Action::FirstImage, K(Qt::ControlModifier | Qt::Key_Up)));
        QVERIFY(has(Action::ShowShortcuts, K(Qt::Key_Question)));
        QVERIFY(has(Action::OpenPreferences, K(Qt::ControlModifier | Qt::Key_Comma)));
        QCOMPARE(action(Action::OpenPreferences)->menuRole(), QAction::PreferencesRole);
        QCOMPARE(command("about")->menuRole(), QAction::AboutRole);
        QCOMPARE(command("quit")->menuRole(), QAction::QuitRole);
        QCOMPARE(command("open")->shortcut(), K(QKeySequence::Open));
        QVERIFY(command("makeDefault"));
        QVERIFY(command("sort:finder") && command("sort:asc") && command("sort:desc"));
        for (const char* id : {"sort:name", "sort:dateModified", "sort:dateCreated", "sort:dateAdded",
                               "sort:dateCaptured", "sort:size", "sort:kind"})
            QVERIFY2(command(id), id);
        for (const char* id : {"minimize", "zoomWindow", "bringAllToFront", "statusBar", "background:system"})
            QVERIFY2(command(id), id);
        // Every menu item that has an Action shows the current shortcut; changes follow.
        InputBindings b;
        b.setShortcuts(Action::ZoomActual, {K(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_A)});
        QCOMPARE(action(Action::ZoomActual)->shortcuts(),
                 QList<K>{K(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_A)});
        // "Als Standard-App festlegen" asks first; never called automatically.
        QCOMPARE(shell->makeDefaultCalls, 0);
        // ⌘, opens the preferences window (F-SET-01).
        action(Action::OpenPreferences)->trigger();
        QVERIFY(PreferencesDialog::existingInstance());
        QVERIFY(PreferencesDialog::existingInstance()->isVisible());
        // ? shows the shortcut overview.
        action(Action::ShowShortcuts)->trigger();
        bool found = false;
        for (QWidget* tl : QApplication::topLevelWidgets())
            found = found || (tl->objectName() == QLatin1String("lfShortcutsDialog") && tl->isVisible());
        QVERIFY(found);
    }

    // Covers: F-MAC-05, F-SET-04
    void F_MAC_05_geometryAndZoomModeRemembered() {
        makeStandardFolder();
        ViewerWindow* w = app->openPaths({path("IMG_2.jpg")});
        QVERIFY(waitWindowShown(w));
        w->resize(640, 420);
        w->move(140, 160);
        QTest::qWait(50);
        const QRect geo = w->geometry();
        key(w, Qt::Key_3); // 100 %
        QCOMPARE(Settings::instance().value(key::ZoomMode).toString(), QStringLiteral("actual"));
        w->close();
        QCOMPARE(app->windows().size(), 0);
        QVERIFY(!Settings::instance().value(QStringLiteral("window/geometry")).toByteArray().isEmpty());
        ViewerWindow* n = app->openPaths({path("IMG_1.png")});
        QVERIFY(waitWindowShown(n));
        QCOMPARE(n->size(), geo.size());
        QCOMPARE(n->geometry().topLeft(), geo.topLeft());
        QCOMPARE(n->view()->zoomMode(), ZoomMode::Actual);
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
    TstApp tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_app.moc"
