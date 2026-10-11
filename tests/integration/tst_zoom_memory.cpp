// SPDX-License-Identifier: GPL-3.0-or-later
// F-VIEW-14 in a real window: the zoom and the visible part of a file come back when the
// window shows it again (session only), Enter returns to the standard zoom, "Zoom fixieren"
// (F-VIEW-05) wins, the setting switches it off, renamed files keep their view.

#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/ViewerWindow.h"
#include "app/ZoomMemoryController.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <cmath>
#include <memory>

using namespace lf;

namespace {

class NoTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& p, QString*) override { return p + ".trashed"; }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return true; }
};

class NoShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.png"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

bool closeTo(double a, double b, double eps = 1e-3) {
    return std::abs(a - b) <= eps;
}
bool closePt(QPointF a, QPointF b, double eps = 1e-3) {
    return closeTo(a.x(), b.x(), eps) && closeTo(a.y(), b.y(), eps);
}

} // namespace

class TstZoomMemory : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NoTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NoShell> shell;
    std::unique_ptr<Application> app;
    QList<Action> intercepted;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }

    void writeImage(const QString& name, QColor color) {
        QImage img(1600, 1200, QImage::Format_RGB32);
        img.fill(color);
        QVERIFY(img.save(path(name), "PNG"));
    }

    ViewerWindow* openAndWait(const QString& p) {
        app->openPaths({p});
        if (!QTest::qWaitFor(
                [this, p] { return app->frontWindow() && app->frontWindow()->currentPath() == p; }, 10000))
            return nullptr;
        ViewerWindow* w = app->frontWindow();
        w->resize(900, 700);
        if (!QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        if (!waitShown(w, p))
            return nullptr;
        return w;
    }

    static bool waitShown(ViewerWindow* w, const QString& p) {
        return QTest::qWaitFor([w, p] { return w->shownPath() == p && w->view()->image(); }, 10000);
    }

    static bool go(ViewerWindow* w, Action a, const QString& expected) {
        return w->perform(a) && waitShown(w, expected);
    }

    // Enter as the view hands it to the window (the window adds zoomChanged).
    static void pressEnter(ViewerWindow* w, int key = Qt::Key_Return) {
        InputEvent ev;
        ev.kind = InputKind::Key;
        ev.key = key;
        ev.modifiers = key == Qt::Key_Enter ? Qt::KeypadModifier : Qt::NoModifier;
        w->handleInput(ev, QPointF(-1, -1));
    }

    // Zooms in and pans: a view that differs from the standard zoom.
    static void zoomAndPan(ViewerWindow* w, int steps = 2, QPointF pan = QPointF(-80, 50)) {
        for (int i = 0; i < steps; ++i)
            w->perform(Action::ZoomIn);
        w->view()->panBy(pan);
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
        writeImage(QStringLiteral("a.png"), Qt::red);
        writeImage(QStringLiteral("b.png"), Qt::green);
        writeImage(QStringLiteral("c.png"), Qt::blue);
        trash = std::make_unique<NoTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<NoShell>();
        intercepted.clear();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        services.interceptAction = [this](ViewerWindow*, Action a) {
            if (a != Action::Rename)
                return false;
            intercepted << a; // no inline rename field in these tests
            return true;
        };
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

    void F_VIEW_14_zoomComesBackPerFile() {
        const QString a = path(QStringLiteral("a.png")), b = path(QStringLiteral("b.png"));
        ViewerWindow* w = openAndWait(a);
        QVERIFY(w);
        ImageView* v = w->view();
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        QVERIFY(!w->zoomMemory()->zoomChanged());
        zoomAndPan(w);
        QVERIFY(v->isZoomed());
        QVERIFY(w->zoomMemory()->zoomChanged());
        const double factor = v->zoomFactor();
        const QPointF center = v->viewCenter();
        QVERIFY(!closePt(center, QPointF(0.5, 0.5))); // panned

        // b gets the standard zoom.
        QVERIFY(go(w, Action::NextImage, b));
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        QVERIFY(!v->isZoomed());
        QVERIFY(!w->zoomMemory()->zoomChanged());

        // Back to a: same zoom and visible part, also after better stages arrived.
        QVERIFY(go(w, Action::PreviousImage, a));
        QCOMPARE(v->zoomMode(), ZoomMode::Custom);
        QVERIFY(closeTo(v->zoomFactor(), factor, 1e-9));
        QVERIFY(closePt(v->viewCenter(), center));
        QTest::qWait(300);
        QVERIFY(closeTo(v->zoomFactor(), factor, 1e-9));
        QVERIFY(closePt(v->viewCenter(), center));
        QVERIFY(w->zoomMemory()->zoomChanged());

        // The window size changes (full screen, resize): same factor, same image point.
        w->resize(1100, 800);
        QTRY_VERIFY(closePt(v->viewCenter(), center));
        QVERIFY(closeTo(v->zoomFactor(), factor, 1e-9));
    }

    void F_VIEW_14_enterResetsToStandardZoom() {
        const QString a = path(QStringLiteral("a.png")), b = path(QStringLiteral("b.png"));
        ViewerWindow* w = openAndWait(a);
        QVERIFY(w);
        ImageView* v = w->view();
        zoomAndPan(w);
        QVERIFY(w->zoomMemory()->zoomChanged());
        pressEnter(w);
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        QVERIFY(!v->isZoomed());
        QVERIFY(!w->zoomMemory()->zoomChanged());
        QVERIFY(!w->zoomMemory()->memory().contains(a, 0));
        QVERIFY(intercepted.isEmpty()); // no rename
        // Forgotten: a comes back in the standard zoom.
        QVERIFY(go(w, Action::NextImage, b));
        QVERIFY(go(w, Action::PreviousImage, a));
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        // Enter without a changed zoom keeps renaming (F-FILE-04).
        pressEnter(w);
        QCOMPARE(intercepted, QList<Action>{Action::Rename});
        // Keypad Enter resets as well.
        zoomAndPan(w);
        pressEnter(w, Qt::Key_Enter);
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        // The menu item does the same.
        zoomAndPan(w);
        QVERIFY(w->perform(Action::ResetZoom));
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        QVERIFY(!w->zoomMemory()->memory().contains(a, 0));
        // A chosen zoom mode becomes the standard: nothing remembered, Enter renames.
        zoomAndPan(w);
        QVERIFY(w->perform(Action::ZoomActual));
        QCOMPARE(v->standardZoomMode(), ZoomMode::Actual);
        QVERIFY(!w->zoomMemory()->memory().contains(a, 0));
        QVERIFY(!w->zoomMemory()->zoomChanged());
    }

    void F_VIEW_14_zoomLockWins() {
        const QString a = path(QStringLiteral("a.png")), b = path(QStringLiteral("b.png"));
        ViewerWindow* w = openAndWait(a);
        QVERIFY(w);
        ImageView* v = w->view();
        zoomAndPan(w, 1);
        const double remembered = v->zoomFactor();
        QVERIFY(go(w, Action::NextImage, b));
        // F-VIEW-05 on: the locked zoom goes to every image, a's remembered one does not win.
        QVERIFY(w->perform(Action::ToggleZoomLock));
        QVERIFY(v->zoomLocked());
        zoomAndPan(w, 3, QPointF(30, -20));
        const double locked = v->zoomFactor();
        QVERIFY(!closeTo(locked, remembered));
        QVERIFY(go(w, Action::PreviousImage, a));
        QVERIFY(closeTo(v->zoomFactor(), locked, 1e-9));
        QVERIFY(w->perform(Action::ToggleZoomLock));
        QVERIFY(!v->zoomLocked());
        // Unlocked again: the memory applies (b was not remembered while locked).
        QVERIFY(go(w, Action::NextImage, b));
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
    }

    void F_VIEW_14_settingOffAndFileChanges() {
        const QString a = path(QStringLiteral("a.png")), b = path(QStringLiteral("b.png"));
        ViewerWindow* w = openAndWait(a);
        QVERIFY(w);
        ImageView* v = w->view();
        QVERIFY(Settings::instance().value(key::RememberZoom).toBool()); // default on

        // Renamed: the view follows the file.
        zoomAndPan(w);
        const double factor = v->zoomFactor();
        const QString renamed = path(QStringLiteral("a2.png"));
        QVERIFY(QFile::rename(a, renamed));
        w->fileRenamed(a, renamed);
        QVERIFY(w->zoomMemory()->memory().contains(renamed, 0));
        QVERIFY(!w->zoomMemory()->memory().contains(a, 0));
        QTRY_COMPARE(w->currentPath(), renamed);
        QVERIFY(waitShown(w, renamed));
        QVERIFY(closeTo(v->zoomFactor(), factor, 1e-9));

        // Deleted: forgotten.
        w->fileRemoved(renamed);
        QVERIFY(!w->zoomMemory()->memory().contains(renamed, 0));

        // Setting off: nothing is remembered.
        QVERIFY(waitShown(w, b));
        zoomAndPan(w);
        QVERIFY(w->zoomMemory()->memory().contains(b, 0));
        Settings::instance().setValue(key::RememberZoom, false);
        QCOMPARE(w->zoomMemory()->memory().size(), 0);
        zoomAndPan(w);
        QVERIFY(go(w, Action::NextImage, path(QStringLiteral("c.png"))));
        QVERIFY(go(w, Action::PreviousImage, b));
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
        // Enter still resets a changed zoom.
        zoomAndPan(w);
        pressEnter(w);
        QCOMPARE(v->zoomMode(), ZoomMode::Fit);
    }
};

QTEST_MAIN(TstZoomMemory)
#include "tst_zoom_memory.moc"
