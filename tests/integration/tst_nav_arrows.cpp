// SPDX-License-Identifier: GPL-3.0-or-later
// F-NAV-09 in real windows: the previous/next buttons at the image edges fade in on a mouse
// move and out after the idle time, a click flips like ←/→ (a double click never reaches the
// image), and they stay away at the ends without wrap, in the grid, in the slideshow and
// with the setting off. Images only in temporary folders.

#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/NavArrows.h"
#include "app/OverviewController.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "app/WindowExtras.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <cmath>
#include <memory>

using namespace lf;

namespace {

class NullTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString&, QString* error) override {
        if (error)
            *error = QStringLiteral("not in tests");
        return std::nullopt;
    }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return false; }
};

class NullShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

QString makeImage(const QString& path, QColor color) {
    QImage img(QSize(640, 480), QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

} // namespace

class TstNavArrows : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NullTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NullShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    void makeFolder() {
        QVERIFY(!makeImage(path("alpha.jpg"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("beta.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("gamma.jpg"), Qt::blue).isEmpty());
    }
    ViewerWindow* openAndWait(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        if (!QTest::qWaitFor([w] { return !w->collection()->isLoading() && w->collection()->count() == 3; },
                             5000))
            return nullptr;
        if (!QTest::qWaitFor([&] { return w->shownPath() == file; }, 5000))
            return nullptr;
        return w;
    }
    // Moves the mouse over the window the way the system does (through its QWindow).
    static void moveMouse(ViewerWindow* w, QPoint pos) { QTest::mouseMove(w->windowHandle(), pos); }
    static QPoint middle(ViewerWindow* w) { return w->view()->mapTo(w, w->view()->rect().center()); }
    static QPoint centerOf(ViewerWindow* w, QWidget* button) {
        return button->mapTo(w, button->rect().center());
    }
    static void click(ViewerWindow* w, QWidget* button) {
        QTest::mouseClick(w->windowHandle(), Qt::LeftButton, {}, centerOf(w, button));
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
        trash = std::make_unique<NullTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<NullShell>();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
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

    // Covers: F-NAV-09 (hidden at rest, fade in on a mouse move, out after the idle time,
    // accessible names and tooltips with the key)
    void F_NAV_09_fadeInOnMouseMoveAndOutWhenIdle() {
        makeFolder();
        QVERIFY(Settings::instance().value(key::ShowNavArrows).toBool()); // default on
        ViewerWindow* w = openAndWait(path("beta.jpg"));
        QVERIFY(w);
        NavArrows* n = w->navArrows();
        QVERIFY(n);
        NavArrowButton* left = n->leftButton();
        NavArrowButton* right = n->rightButton();
        QCOMPARE(left->parentWidget(), w->view()); // above the canvas (and grabbed for EDR)
        // At rest: nothing over the photo.
        QVERIFY(!left->isVisible());
        QVERIFY(!right->isVisible());
        QCOMPARE(left->focusPolicy(), Qt::NoFocus);
        QCOMPARE(left->accessibleName(), actionLabel(Action::PreviousImage));
        QCOMPARE(right->accessibleName(), actionLabel(Action::NextImage));
        QVERIFY(left->toolTip().contains(QStringLiteral("←")));
        QVERIFY(right->toolTip().contains(QStringLiteral("→")));
        // A mouse move over the image fades both in (beta has a neighbour on each side).
        moveMouse(w, middle(w));
        QVERIFY(left->shownTarget());
        QVERIFY(right->shownTarget());
        QVERIFY(left->isVisible() && right->isVisible());
        QVERIFY(!right->takesMouse()); // a click right after the move still reaches the image
        QTRY_VERIFY(left->takesMouse() && right->takesMouse());
        QCOMPARE(right->opacity(), 1.0);
        // At the edges, vertically centred, at least 44 px to hit.
        QVERIFY(left->geometry().left() < w->view()->width() / 4);
        QVERIFY(right->geometry().right() > w->view()->width() * 3 / 4);
        QVERIFY(left->width() >= NavArrows::kDiameter && left->height() >= NavArrows::kDiameter);
        QVERIFY(std::abs(left->geometry().center().y() - right->geometry().center().y()) <= 1);
        // Further moves keep them; after the idle time they fade out.
        QTest::qWait(NavArrows::kIdleMs / 2);
        moveMouse(w, middle(w) + QPoint(10, 0));
        QTest::qWait(NavArrows::kIdleMs / 2 + 200);
        QVERIFY(right->shownTarget());
        QTRY_VERIFY_WITH_TIMEOUT(!left->isVisible() && !right->isVisible(), 6000);
        // Over a button they stay.
        moveMouse(w, middle(w));
        QTRY_VERIFY(right->takesMouse());
        moveMouse(w, centerOf(w, right));
        QTRY_VERIFY(right->underMouse());
        QTest::qWait(NavArrows::kIdleMs + 500);
        QVERIFY(right->isVisible());
        QVERIFY(right->shownTarget());
    }

    // Covers: F-NAV-09 (click = ←/→, the first/last image hides its button without wrap, a
    // double click flips twice and never toggles full screen)
    void F_NAV_09_clickFlipsLikeTheArrowKeys() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("alpha.jpg"));
        QVERIFY(w);
        NavArrowButton* left = w->navArrows()->leftButton();
        NavArrowButton* right = w->navArrows()->rightButton();
        moveMouse(w, middle(w));
        // First image, "anhalten" at the ends (default): no previous button.
        QVERIFY(!left->shownTarget());
        QVERIFY(!left->isVisible());
        QTRY_VERIFY(right->takesMouse());
        click(w, right);
        QCOMPARE(w->currentPath(), path("beta.jpg"));
        QVERIFY(left->shownTarget()); // now there is a previous image
        QTRY_VERIFY(left->takesMouse());
        click(w, left);
        QCOMPARE(w->currentPath(), path("alpha.jpg"));
        QTRY_VERIFY(!left->isVisible()); // back at the first image
        // A double click on the button: two clicks, the image's double click (full screen,
        // F-IN-10) is not triggered.
        QTRY_VERIFY(right->takesMouse());
        QTest::mouseDClick(w->windowHandle(), Qt::LeftButton, {}, centerOf(w, right));
        QCOMPARE(w->currentPath(), path("gamma.jpg"));
        QVERIFY(!w->fullScreenTarget());
        QVERIFY(!w->isFullScreen());
        // Last image: no next button any more.
        QVERIFY(!right->shownTarget());
        QVERIFY(!right->isVisible());
        QVERIFY(left->shownTarget());
        // With wrap both directions always have a target.
        Settings::instance().setValue(key::WrapMode, QStringLiteral("wrap"));
        moveMouse(w, middle(w));
        QVERIFY(left->shownTarget() && right->shownTarget());
        QTRY_VERIFY(right->takesMouse());
        click(w, right);
        QCOMPARE(w->currentPath(), path("alpha.jpg"));
        QVERIFY(left->shownTarget());
        // Keyboard users: the buttons never take the focus.
        QVERIFY(!left->hasFocus() && !right->hasFocus());
    }

    // Covers: F-NAV-09 (setting off: never shown; not in the grid and the slideshow)
    void F_NAV_09_hiddenWhenOffInTheGridAndTheSlideshow() {
        makeFolder();
        ViewerWindow* w = openAndWait(path("beta.jpg"));
        QVERIFY(w);
        NavArrowButton* left = w->navArrows()->leftButton();
        NavArrowButton* right = w->navArrows()->rightButton();
        moveMouse(w, middle(w));
        QVERIFY(right->shownTarget());
        // Setting off: gone at once, a mouse move does not bring them back.
        Settings::instance().setValue(key::ShowNavArrows, false);
        QVERIFY(!left->isVisible() && !right->isVisible());
        moveMouse(w, middle(w) + QPoint(5, 5));
        QVERIFY(!left->shownTarget() && !right->shownTarget());
        QTest::qWait(200);
        QVERIFY(!left->isVisible() && !right->isVisible());
        Settings::instance().setValue(key::ShowNavArrows, true);
        moveMouse(w, middle(w));
        QVERIFY(left->shownTarget() && right->shownTarget());
        // Grid (F-GRID-02): no buttons.
        QVERIFY(w->perform(Action::ToggleGrid));
        QVERIFY(w->extras()->overview()->isGridShown());
        QVERIFY(!left->shownTarget() && !right->shownTarget());
        moveMouse(w, middle(w) + QPoint(-5, 5));
        QVERIFY(!left->shownTarget() && !right->shownTarget());
        QVERIFY(w->perform(Action::ToggleGrid));
        QVERIFY(!w->extras()->overview()->isGridShown());
        QTRY_VERIFY(w->view()->isVisible());
        moveMouse(w, middle(w));
        QVERIFY(left->shownTarget() && right->shownTarget());
        // Slideshow (full screen, F-SLIDE-01): no buttons, also after a mouse move.
        w->startSlideshow(7u);
        QVERIFY(w->isSlideshowRunning());
        QVERIFY(!left->shownTarget() && !right->shownTarget());
        moveMouse(w, middle(w) + QPoint(3, -3));
        QVERIFY(!left->shownTarget() && !right->shownTarget());
        QVERIFY(!left->isVisible() && !right->isVisible());
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
    TstNavArrows tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_nav_arrows.moc"
