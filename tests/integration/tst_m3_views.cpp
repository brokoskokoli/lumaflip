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
        const QString target =
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

    // Covers: F-VIEW-12 (floating window level), F-VIEW-13 (title bar chrome hidden, handle)
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

        w->perform(Action::ToggleMinimalWindow);
        s = nativeWindowState(w);
        QVERIFY(s.fullSizeContent);
        QVERIFY(s.titlebarTransparent);
        QVERIFY(s.titleHidden);
        QVERIFY(x->moveHandle()->isVisible());
        w->perform(Action::ToggleMinimalWindow);
        s = nativeWindowState(w);
        QVERIFY(!s.fullSizeContent && !s.titlebarTransparent && !s.titleHidden);
        QVERIFY(!x->moveHandle()->isVisible());
    }

    // Covers: F-WIN-07 (couple two windows: flip by file name, zoom/pan together, indicator)
    void F_WIN_07_coupledWindows() {
        makeFolder(QStringLiteral("one"));
        makeFolder(QStringLiteral("two"));
        QFile::remove(path("two/beta.jpg")); // "two" has no beta: no match, the follower stays
        ViewerWindow* a = openAndWait(path("one/alpha.jpg"));
        ViewerWindow* b = openAndWait(path("two/alpha.jpg"));
        QVERIFY(a && b && a != b);
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
