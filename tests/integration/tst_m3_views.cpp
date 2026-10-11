// SPDX-License-Identifier: GPL-3.0-or-later
// M3 views in real windows: grid (F-GRID-02), compare (F-GRID-03), loupe (F-VIEW-11), always
// on top and the minimal window (F-VIEW-12/13, native NSWindow state), coupled windows
// (F-WIN-07), go to / shuffle (F-NAV-06), type-to-filter (F-NAV-08) and rating during the
// slideshow (F-SLIDE-03). Files only in temporary folders; the trash is a temporary folder.

#include "app/Application.h"
#include "app/CompareView.h"
#include "app/CouplingManager.h"
#include "app/CullingController.h"
#include "app/GridView.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/Loupe.h"
#include "app/MainMenu.h"
#include "app/NavigationExtras.h"
#include "app/OverviewController.h"
#include "app/QuickFilterBar.h"
#include "app/RecentItems.h"
#include "app/SlideshowController.h"
#include "app/TitleStrip.h"
#include "app/ViewSyncBinding.h"
#include "app/ViewerWindow.h"
#include "app/WindowExtras.h"
#include "app/WindowModes.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/RatingStore.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUuid>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

class FolderTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        QString target =
            QDir(dir.path())
                .filePath(QUuid::createUuid().toString(QUuid::Id128) + QFileInfo(path).fileName());
        if (!QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("trash failed");
            return std::nullopt;
        }
        return target;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        if (QFileInfo::exists(original) || !QFile::rename(trashed, original)) {
            if (error)
                *error = QStringLiteral("restore failed");
            return false;
        }
        return true;
    }
    QTemporaryDir dir;
};

class NullShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(640, 480)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

Rating stars(int n, PickFlag flag = PickFlag::None) {
    Rating r;
    r.stars = n;
    r.flag = flag;
    return r;
}

} // namespace

class TstM3Views : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<FolderTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NullShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    void makeFolder(const QString& sub = {}) {
        if (!sub.isEmpty())
            QVERIFY(QDir(dir->path()).mkpath(sub));
        const QString prefix = sub.isEmpty() ? QString() : sub + QLatin1Char('/');
        QVERIFY(!makeImage(path(prefix + "alpha.jpg"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path(prefix + "beta.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path(prefix + "gamma.jpg"), Qt::blue).isEmpty());
    }
    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    // Check mark of a menu entry as the user sees it: the menus holding it are opened
    // (aboutToShow refreshes the state for the front window).
    bool menuShowsChecked(Action a) {
        QAction* act = app->menu()->action(a);
        if (!act)
            return false;
        for (QObject* o : act->associatedObjects())
            if (auto* m = qobject_cast<QMenu*>(o))
                emit m->aboutToShow();
        return act->isChecked();
    }
    ViewerWindow* openAndWait(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000) || !waitLoaded(w))
            return nullptr;
        if (!QTest::qWaitFor([&] { return w->shownPath() == file; }, 5000))
            return nullptr;
        return w;
    }
    static Rating sidecar(const QString& image) { return ratingio::readSidecar(image).value_or(Rating()); }

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
        trash = std::make_unique<FolderTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<NullShell>();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
        RatingStore::instance().setOptions(RatingOptions()); // sidecar only
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

    // Covers: F-GRID-02 (G, selection, size, Enter opens, rating badges via the selection)
    void F_GRID_02_gridSelectionSizeAndOpen() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        QTest::keyClick(w->view(), Qt::Key_G);
        OverviewController* o = w->extras()->overview();
        QVERIFY(o->isGridShown());
        GridView* g = o->grid();
        QVERIFY(g && g->isVisible());
        QVERIFY(!w->view()->isVisible());
        QCOMPARE(g->gridModel()->rowCount(), 3);
        QCOMPARE(g->cursorPath(), path("alpha.jpg"));
        QVERIFY(menuShowsChecked(Action::ToggleGrid));
        // ⇧→ extends, ⌘-click toggles.
        QTest::keyClick(g, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(g->selectedPaths(), (QStringList{path("alpha.jpg"), path("beta.jpg")}));
        g->clickRow(2, Qt::ControlModifier);
        QCOMPARE(g->selection().selectedCount(), 3);
        QVERIFY(o->headerText().contains(QStringLiteral("3")));
        // Rating keys rate the whole selection (digits are rating keys in the grid).
        QTest::keyClick(g, Qt::Key_3);
        QTRY_COMPARE(sidecar(path("alpha.jpg")), stars(3));
        QTRY_COMPARE(sidecar(path("gamma.jpg")), stars(3));
        // ⌘+ / ⌘− change the thumbnail size.
        const int size = g->thumbSize();
        w->perform(Action::ZoomIn);
        QVERIFY(g->thumbSize() > size);
        w->perform(Action::ZoomOut);
        QCOMPARE(g->thumbSize(), size);
        // Enter opens the image under the cursor.
        g->clickRow(1, Qt::NoModifier);
        QTest::keyClick(g, Qt::Key_Return);
        QVERIFY(!o->isGridShown());
        QVERIFY(w->view()->isVisible());
        QTRY_COMPARE(w->currentPath(), path("beta.jpg"));
    }

    // Covers: F-GRID-02 (actions on the selection: trash as one undo step)
    void F_GRID_02_trashSelectionUndo() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        w->perform(Action::ToggleGrid);
        GridView* g = w->extras()->overview()->grid();
        QVERIFY(g);
        g->clickRow(0, Qt::NoModifier);
        g->clickRow(1, Qt::ShiftModifier);
        w->perform(Action::Trash);
        QTRY_VERIFY(!QFileInfo::exists(path("alpha.jpg")) && !QFileInfo::exists(path("beta.jpg")));
        QTRY_COMPARE(w->collection()->count(), 1);
        QTRY_VERIFY(!ops->isBusy());
        w->perform(Action::Undo);
        QTRY_VERIFY(QFileInfo::exists(path("alpha.jpg")) && QFileInfo::exists(path("beta.jpg")));
    }

    // Covers: F-GRID-03 (side by side, synchronized zoom/pan, ⌥ = one pane, Tab, rating, Esc)
    void F_GRID_03_compareSynchronizedPanes() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        w->resize(1000, 700);
        w->perform(Action::CompareWithNext);
        OverviewController* o = w->extras()->overview();
        QVERIFY(o->isCompareShown());
        CompareView* cv = o->compareView();
        QCOMPARE(cv->paneCount(), 2);
        QCOMPARE(cv->paths(), (QStringList{path("alpha.jpg"), path("beta.jpg")}));
        ImageView* a = cv->paneView(0);
        ImageView* b = cv->paneView(1);
        QTRY_VERIFY(a->image() && b->image());
        QVERIFY(cv->paneCaption(0).contains(QStringLiteral("alpha.jpg")));
        // Zoom in one pane: the other follows (same factor, same relative center).
        a->setZoomFactor(2.0, QPointF(a->width() / 2.0, a->height() / 2.0));
        QTRY_VERIFY(std::abs(b->zoomFactor() - 2.0) < 1e-6);
        a->panBy(QPointF(40, 20));
        QTRY_VERIFY((a->viewCenter() - b->viewCenter()).manhattanLength() < 1e-3);
        // ⌥: pane b moves alone.
        cv->sync()->setIndependentTest([] { return true; });
        const QPointF before = a->viewCenter();
        b->panBy(QPointF(-60, 0));
        QVERIFY((a->viewCenter() - before).manhattanLength() < 1e-6);
        cv->sync()->setIndependentTest([] { return false; });
        // Tab moves the focus; rating keys rate the focused pane.
        QCOMPARE(cv->focusedPane(), 0);
        QTest::keyClick(cv, Qt::Key_Tab);
        QCOMPARE(cv->focusedPane(), 1);
        QTest::keyClick(cv, Qt::Key_2);
        QTRY_COMPARE(sidecar(path("beta.jpg")), stars(2));
        QCOMPARE(sidecar(path("alpha.jpg")), Rating());
        QTRY_VERIFY(cv->paneCaption(1).contains(QStringLiteral("★★")));
        // Esc closes the compare mode.
        QTest::keyClick(cv, Qt::Key_Escape);
        QVERIFY(!o->isCompareShown());
        QVERIFY(w->view()->isVisible());
        // From the grid: the selection.
        w->perform(Action::ToggleGrid);
        GridView* g = o->grid();
        g->selectAllItems();
        QVERIFY(o->canCompareSelection());
        w->perform(Action::CompareWithNext);
        QCOMPARE(cv->paneCount(), 3);
        w->perform(Action::ExitModeOrNothing);
        QVERIFY(o->isGridShown()); // back to where it started
    }

    // Covers: F-VIEW-11 (L held shows the loupe at 200 %; release hides it)
    void F_VIEW_11_loupeWhileKeyHeld() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        LoupeController* l = w->extras()->loupe();
        const QPoint at(w->view()->width() / 2, w->view()->height() / 2);
        QTest::keyPress(w->view(), Qt::Key_L);
        QVERIFY(l->isActive());
        QVERIFY(l->widget() && l->widget()->isVisible());
        // The loupe starts at the real cursor (wherever it is); move it to the center with a
        // synthesized mouse move (the real cursor is never touched).
        QMouseEvent move(QEvent::MouseMove, QPointF(at), QPointF(w->view()->mapToGlobal(at)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w->view(), &move);
        QCOMPARE(l->widget()->cursorPos(), QPointF(at));
        const loupe::Geometry g = l->widget()->loupeGeometry();
        QVERIFY(g.overImage);
        QCOMPARE(l->widget()->magnification(), 2.0); // default "200"
        QVERIFY(std::abs(g.deviceScale - 2.0) < 1e-6);
        QTest::keyRelease(w->view(), Qt::Key_L);
        QVERIFY(!l->isActive());
        QVERIFY(!l->widget()->isVisible());
        // Middle button with the input choice "Lupe".
        app->bindings().setChoice(QStringLiteral("middleClick"), QStringLiteral("magnifier"));
        QWidget* canvas = w->view()->findChild<ImageCanvas*>();
        QTest::mousePress(canvas, Qt::MiddleButton, {}, at);
        QVERIFY(l->isActive());
        QTest::mouseRelease(canvas, Qt::MiddleButton, {}, at);
        QVERIFY(!l->isActive());
    }

    // Covers: F-VIEW-12 (floating window level), F-VIEW-13 (borderless window on/off per window)
    void F_VIEW_12_F_VIEW_13_alwaysOnTopAndMinimalWindow() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        WindowExtras* x = w->extras();
        w->perform(Action::ToggleAlwaysOnTop);
        QVERIFY(x->alwaysOnTop());
        NativeWindowState s = nativeWindowState(w);
        QVERIFY(s.valid);
        QVERIFY(s.floating);
        QVERIFY(menuShowsChecked(Action::ToggleAlwaysOnTop));
        w->perform(Action::ToggleAlwaysOnTop);
        QVERIFY(!nativeWindowState(w).floating);
        QVERIFY(!menuShowsChecked(Action::ToggleAlwaysOnTop));

        // F-VIEW-13 = the borderless window of F-VIEW-15, the default for new windows: the
        // menu entry switches this window to the system's title bar and back.
        QVERIFY(x->minimalWindow());
        QVERIFY(menuShowsChecked(Action::ToggleMinimalWindow));
        s = nativeWindowState(w);
        QVERIFY(s.fullSizeContent);
        QVERIFY(s.titlebarTransparent);
        QVERIFY(s.titleHidden);
        TitleStripController* t = x->titleStrip();
        QVERIFY(t->active());
        w->perform(Action::ToggleMinimalWindow);
        QVERIFY(!x->minimalWindow());
        QVERIFY(!menuShowsChecked(Action::ToggleMinimalWindow));
        s = nativeWindowState(w);
        QVERIFY(!s.fullSizeContent && !s.titlebarTransparent && !s.titleHidden);
        QVERIFY(!t->active());
        QVERIFY(!t->strip()->isVisible());
        w->perform(Action::ToggleMinimalWindow);
        s = nativeWindowState(w);
        QVERIFY(s.fullSizeContent && s.titlebarTransparent && s.titleHidden);
        QVERIFY(t->active());
        QVERIFY(t->strip()->shownTarget()); // shows where the title went
    }

    // Moves the mouse over the window the way the system does (through its QWindow).
    static void moveMouse(ViewerWindow* w, QPoint pos) { QTest::mouseMove(w->windowHandle(), pos); }

    // Covers: F-VIEW-15 (borderless by default: image up to the top edge, the strip with the
    // title fades in on a mouse move and out after the idle time, system buttons with it)
    void F_VIEW_15_titleStripFadesInAndOut() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        TitleStripController* t = w->extras()->titleStrip();
        TitleStrip* strip = t->strip();
        QVERIFY(t->borderless());
        QVERIFY(t->active());
        const NativeWindowState native = nativeWindowState(w);
        QVERIFY(native.fullSizeContent && native.titleHidden);
        // The image view starts at the top edge of the window (no title bar, no menu bar).
        QTRY_COMPARE(w->view()->mapTo(w, QPoint(0, 0)).y(), 0);
        QCOMPARE(strip->mapTo(w, QPoint(0, 0)), QPoint(0, 0));
        QCOMPARE(strip->width(), w->centralWidget()->width());
        QVERIFY(strip->height() >= 20);
        // Shown briefly when the window became active (keyboard users), then idle: gone.
        QTRY_VERIFY_WITH_TIMEOUT(!strip->isVisible(), 6000);
        QVERIFY(!strip->shownTarget());
        if (titleStripUsesSystemButtons())
            QTRY_VERIFY(nativeWindowState(w).buttonsHidden);
        // A mouse move anywhere in the window fades it in.
        moveMouse(w, QPoint(w->width() / 2, w->height() / 2));
        QVERIFY(strip->shownTarget());
        QVERIFY(strip->isVisible());
        QVERIFY(!strip->takesMouse()); // a click right after the move still reaches the image
        QTRY_VERIFY(strip->takesMouse());
        QCOMPARE(strip->opacity(), 1.0);
        if (titleStripUsesSystemButtons())
            QVERIFY(!nativeWindowState(w).buttonsHidden);
        // The window title in the strip, also after flipping.
        QCOMPARE(strip->title(), w->windowTitle());
        QVERIFY(strip->title().contains(QStringLiteral("alpha.jpg")));
        w->collection()->setCurrentPath(path("beta.jpg"));
        QTRY_VERIFY(strip->title().contains(QStringLiteral("beta.jpg")));
        QCOMPARE(strip->title(), w->windowTitle());
        // Further moves keep it; after the idle time it fades out.
        QTest::qWait(TitleStripController::kIdleMs / 2);
        moveMouse(w, QPoint(w->width() / 2 + 10, w->height() / 2));
        QTest::qWait(TitleStripController::kIdleMs / 2 + 200);
        QVERIFY(strip->shownTarget());
        QTRY_VERIFY_WITH_TIMEOUT(!strip->isVisible(), 6000);
        if (titleStripUsesSystemButtons())
            QTRY_VERIFY(nativeWindowState(w).buttonsHidden);
    }

    // Covers: F-VIEW-15 (drag area and buttons of the strip, setting "immer zeigen")
    void F_VIEW_15_dragAreaButtonsAndAlwaysShowSetting() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        TitleStripController* t = w->extras()->titleStrip();
        TitleStrip* strip = t->strip();
        QTRY_VERIFY_WITH_TIMEOUT(!strip->isVisible(), 6000);
        const QPoint middle(strip->width() / 2, strip->height() / 2);
        QCOMPARE(strip->nativeHit(middle), TitleStripHit::Client); // hidden: the image's
        moveMouse(w, QPoint(w->width() / 2, w->height() / 2));
        QTRY_VERIFY(strip->takesMouse());
        // Between the buttons the strip moves the window (native caption on Windows).
        QCOMPARE(strip->buttonAt(middle), TitleButton::None);
        QCOMPARE(strip->nativeHit(middle), TitleStripHit::Caption);
        const TitleStripLayout l = strip->stripLayout();
        if (titleStripUsesSystemButtons()) {
            // macOS: the system's buttons on the left, the title stays clear of them.
            QVERIFY(l.close.isEmpty() && l.maximize.isEmpty());
            QVERIFY(l.titleArea.left() >= titleStripReservedLeft(w));
        } else {
            QVERIFY(!l.close.isEmpty() && !l.minimize.isEmpty() && !l.maximize.isEmpty());
            QCOMPARE(strip->nativeHit(l.maximize.center()), TitleStripHit::Maximize); // Snap Layouts
            QCOMPARE(strip->nativeHit(l.close.center()), TitleStripHit::Client);
            QCOMPARE(strip->buttonAt(l.minimize.center()), TitleButton::Minimize);
#ifdef Q_OS_WIN
            // Windows: buttons on the right, the menus behind a button on the left.
            QCOMPARE(l.close.right(), strip->width() - 1);
            QVERIFY(!l.menu.isEmpty() && l.menu.left() == 0);
            QVERIFY(t->holdsMenuBar());
            QCOMPARE(w->menuWidget()->maximumHeight(), 0);
#endif
        }
        // Setting "Titelleiste: immer zeigen": the system's title bar, no strip.
        Settings::instance().setValue(QString::fromLatin1(key::TitleBar), QStringLiteral("always"));
        QVERIFY(!t->borderless());
        QVERIFY(!t->active());
        QVERIFY(!w->extras()->minimalWindow());
        NativeWindowState s = nativeWindowState(w);
        QVERIFY(!s.fullSizeContent && !s.titleHidden);
        QVERIFY(!s.buttonsHidden);
        QVERIFY(!strip->isVisible());
        moveMouse(w, QPoint(w->width() / 2 + 5, w->height() / 2 + 5));
        QVERIFY(!strip->shownTarget());
#ifdef Q_OS_WIN
        QVERIFY(w->menuWidget()->maximumHeight() > 0);
#endif
        // New windows follow the setting as well.
        ViewerWindow* w2 = openAndWait(path("gamma.jpg"));
        QVERIFY(w2 && w2 != w);
        QVERIFY(!w2->extras()->titleStrip()->borderless());
        QVERIFY(!nativeWindowState(w2).titleHidden);
        Settings::instance().setValue(QString::fromLatin1(key::TitleBar), QStringLiteral("autoHide"));
        QVERIFY(t->borderless() && t->active());
        QVERIFY(nativeWindowState(w).fullSizeContent);
        QVERIFY(nativeWindowState(w2).titleHidden);
        if (!titleStripUsesSystemButtons()) {
            // The strip's own close button closes the window.
            moveMouse(w2, QPoint(w2->width() / 2, w2->height() / 2));
            TitleStrip* strip2 = w2->extras()->titleStrip()->strip();
            QTRY_VERIFY(strip2->takesMouse());
            QPointer<ViewerWindow> closing(w2);
            QTest::mouseClick(strip2, Qt::LeftButton, {}, strip2->stripLayout().close.center());
            QTRY_VERIFY(!closing || !closing->isVisible());
        }
    }

    // Covers: F-VIEW-15 (no strip in full screen - F-VIEW-10 has its own chrome; the
    // borderless window comes back after leaving full screen)
    void F_VIEW_15_noStripInFullScreen() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        TitleStripController* t = w->extras()->titleStrip();
        QTRY_VERIFY_WITH_TIMEOUT(!w->fullScreenTransitionPending(), 5000);
        w->setFullScreen(true);
        QTRY_VERIFY_WITH_TIMEOUT(!w->fullScreenTransitionPending() && w->isFullScreen(), 10000);
        QVERIFY(!t->active());
        moveMouse(w, QPoint(w->width() / 2, w->height() / 2));
        QVERIFY(!t->strip()->isVisible());
        if (titleStripUsesSystemButtons())
            QVERIFY(!nativeWindowState(w).buttonsHidden); // the title bar comes with the menu bar
        QTest::qWait(800);
        w->setFullScreen(false);
        QTRY_VERIFY_WITH_TIMEOUT(!w->fullScreenTransitionPending() && !w->isFullScreen(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(t->active(), 5000);
        QVERIFY(nativeWindowState(w).fullSizeContent);
        moveMouse(w, QPoint(w->width() / 2 + 5, w->height() / 2));
        QVERIFY(t->strip()->shownTarget());
        QTest::qWait(800);
    }

    // Covers: F-WIN-07 (couple two windows: flip by file name, zoom/pan together, indicator)
    void F_WIN_07_coupledWindows() {
        makeFolder(QStringLiteral("one"));
        makeFolder(QStringLiteral("two"));
        QFile::remove(path("two/beta.jpg")); // "two" has no beta: no match, the follower stays
        ViewerWindow* a = openAndWait(path("one/alpha.jpg"));
        ViewerWindow* b = openAndWait(path("two/alpha.jpg"));
        if (!a || !b || a == b)
            QFAIL("two windows expected");
        CouplingManager& m = CouplingManager::of(*app);
        QVERIFY(m.couple({a, b}));
        QVERIFY(m.isCoupled(a) && m.isCoupled(b));
        QVERIFY(a->statusText().contains(QStringLiteral("Gekoppelt")));
        QCOMPARE(m.coupleBy(), CoupleBy::Name);
        a->collection()->setCurrentPath(path("one/gamma.jpg"));
        QTRY_COMPARE(b->currentPath(), path("two/gamma.jpg"));
        a->collection()->setCurrentPath(path("one/beta.jpg"));
        QCOMPARE(b->currentPath(), path("two/gamma.jpg"));
        // By position.
        Settings::instance().setValue(key::CoupleBy, QStringLiteral("index"));
        a->collection()->setCurrentIndex(0);
        QTRY_COMPARE(b->collection()->currentIndex(), 0);
        // Zoom and pan together.
        QTRY_VERIFY(a->view()->image() && b->view()->image());
        a->view()->setZoomFactor(1.5, QPointF(10, 10));
        QTRY_VERIFY(std::abs(b->view()->zoomFactor() - 1.5) < 1e-6);
        m.uncouple();
        QVERIFY(!a->statusText().contains(QStringLiteral("Gekoppelt")));
        // The dialog.
        a->perform(Action::CoupleWindows);
        QDialog* dlg = m.dialog();
        QVERIFY(dlg);
        auto* ok = dlg->findChild<QPushButton*>(QStringLiteral("lfCoupleOk"));
        QVERIFY(ok && ok->isEnabled()); // two windows: both preselected
        ok->click();
        QVERIFY(m.isCoupled(a) && m.isCoupled(b));
        b->close();
        QTRY_VERIFY(!m.isCoupled(a));
    }

    // Covers: F-NAV-06 (⌘G dialog, shuffled order with "Zufällig" in the status line)
    void F_NAV_06_goToAndShuffle() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        QVERIFY(app->menu()->action(Action::GoToImage)->shortcuts().contains(QKeySequence("Ctrl+G")));
        w->perform(Action::GoToImage);
        NavigationExtras* n = w->extras()->navigation();
        QDialog* dlg = n->goToDialog();
        QVERIFY(dlg && dlg->isVisible());
        auto* field = dlg->findChild<QLineEdit*>(QStringLiteral("lfGoToField"));
        QVERIFY(field);
        field->setText(QStringLiteral("3"));
        QTest::keyClick(field, Qt::Key_Return);
        QTRY_COMPARE(w->currentPath(), path("gamma.jpg"));
        QVERIFY(n->goTo(QStringLiteral("-1")));
        QCOMPARE(w->currentPath(), path("beta.jpg"));
        QVERIFY(!n->goTo(QStringLiteral("x")));

        w->perform(Action::ToggleShuffle);
        QVERIFY(n->shuffle());
        QVERIFY(w->statusText().contains(QStringLiteral("Zufällig")));
        const QStringList order = n->order().order();
        QCOMPARE(order.size(), 3);
        w->collection()->setCurrentPath(order.first());
        w->perform(Action::NextImage);
        QCOMPARE(w->currentPath(), order.at(1));
        w->perform(Action::LastImage);
        QCOMPARE(w->currentPath(), order.last());
        w->perform(Action::ToggleShuffle);
        QVERIFY(!w->statusText().contains(QStringLiteral("Zufällig")));
    }

    // Covers: F-NAV-08 (typing opens the filter, live filtering, Esc clears)
    void F_NAV_08_typeToFilter() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        QuickFilterBar* f = w->extras()->quickFilter();
        QVERIFY(!f->isOpen());
        QTest::keyClick(w->view(), 'b'); // "B" has no other meaning
        QVERIFY(f->isOpen());
        QCOMPARE(f->text(), QStringLiteral("b"));
        QTRY_COMPARE(w->collection()->count(), 1);
        QCOMPARE(w->currentPath(), path("beta.jpg"));
        QTest::keyClicks(f->field(), QStringLiteral("x"));
        QTRY_COMPARE(w->collection()->count(), 0); // after the typing pause (debounce)
        QTest::keyClick(f->field(), Qt::Key_Backspace);
        QTest::keyClick(f->field(), Qt::Key_Return); // keeps the filter
        QVERIFY(!f->isOpen());
        QVERIFY(w->statusText().contains(QStringLiteral("Name: „b“")));
        w->perform(Action::FilterByName);
        QVERIFY(f->isOpen());
        QTest::keyClick(f->field(), Qt::Key_Escape);
        QVERIFY(!f->isOpen());
        QCOMPARE(w->collection()->count(), 3);
        // Keys with a meaning do not open it ("I" = info panel is a shortcut).
        const QKeyEvent info(QEvent::KeyPress, Qt::Key_I, Qt::NoModifier, QStringLiteral("i"));
        QVERIFY(!f->maybeStart(&info, false));
    }

    // Covers: F-SLIDE-03 (P/X/U rate during the slideshow, digits keep the interval)
    void F_SLIDE_03_ratingDuringSlideshow() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        w->startSlideshow(7u);
        QVERIFY(w->isSlideshowRunning());
        const QString first = w->currentPath();
        QTest::keyClick(w->view(), Qt::Key_P);
        QTRY_COMPARE(sidecar(first), stars(0, PickFlag::Pick));
        QTest::keyClick(w->view(), Qt::Key_4); // interval, no rating (rating keys only in culling)
        QCOMPARE(w->slideshow()->intervalMs(), 4000);
        w->culling()->setActive(true);
        QTest::keyClick(w->view(), Qt::Key_4); // now a rating
        QTRY_COMPARE(sidecar(w->currentPath()).stars, 4);
        w->stopSlideshow();
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
    TstM3Views tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_m3_views.moc"
