// SPDX-License-Identifier: GPL-3.0-or-later
// Review round 4 (UI): stage marker gates (histogram of large images, animations larger
// than the decode target), animation player threading and memory bound, drag-out work off
// the GUI thread, the share menu filled asynchronously.

#include "GifTestUtil.h"
#include "app/AnimationPlayer.h"
#include "app/Application.h"
#include "app/DragOut.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/InfoPanel.h"
#include "app/Share.h"
#include "app/ViewerWindow.h"
#include "app/WindowPanels.h"
#include "core/Collection.h"
#include "core/Decoder.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/ThumbnailCache.h"

#include <QApplication>
#include <QColorSpace>
#include <QDir>
#include <QDrag>
#include <QElapsedTimer>
#include <QFile>
#include <QMenu>
#include <QPointer>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace lf;

namespace {

class NoShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

// 24 MP JPEG: left half red, right half blue.
QString makeLargeJpeg(const QString& path) {
    QImage img(6000, 4000, QImage::Format_RGB32);
    img.fill(Qt::red);
    for (int y = 0; y < img.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        std::fill(line + img.width() / 2, line + img.width(), qRgb(0, 0, 255));
    }
    return img.save(path, "JPEG", 80) ? QDir::cleanPath(path) : QString();
}

} // namespace

class TstReview4 : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    QTemporaryDir m_thumbDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<TrashService> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NoShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 10000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    ViewerWindow* open(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000) || !waitShown(w, file))
            return nullptr;
        if (!QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 10000))
            return nullptr;
        return w;
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        ThumbnailCache::setDefaultDirectory(m_thumbDir.path() + QStringLiteral("/thumbs"));
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
        trash = createTrashService();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<NoShell>();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
        setDragExecHookForTesting({});
        ShareServiceCache::instance().setQueryForTesting({});
    }

    void cleanup() {
        app.reset();
        QTest::qWait(10);
        setDragExecHookForTesting({});
        ShareServiceCache::instance().setQueryForTesting({});
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
    }

    // ---- A: the stage marker, not isPreview, gates quality-dependent features ----

    // Covers: F-INFO-03 (the Screen stage of a 24 MP JPEG is downscaled - isPreview is set
    // for it - and still gets its histogram)
    void F_INFO_03_histogramOfLargeJpegScreenStage() {
        const QString jpg = makeLargeJpeg(path("big.jpg"));
        QVERIFY(!jpg.isEmpty());
        ViewerWindow* w = open(jpg);
        QVERIFY(w);
        QTRY_VERIFY_WITH_TIMEOUT(w->view()->image() && w->view()->image()->quality == Quality::Screen, 15000);
        const DecodedImagePtr shown = w->view()->image();
        QVERIFY(shown->image.width() < 6000);
        QVERIFY(shown->isPreview); // what the old gate looked at
        QTest::keyClick(w->view(), Qt::Key_I);
        InfoPanel* info = w->panels()->infoPanel();
        QVERIFY(info);
        QTRY_VERIFY_WITH_TIMEOUT(info->path() == jpg && !info->isHistogramPending(), 15000);
        const Histogram& h = info->histogram();
        QVERIFY(h.samples > 0);
        // Half red, half blue (JPEG: close to the extremes).
        quint64 redHigh = 0, blueHigh = 0;
        for (int i = 200; i < 256; ++i) {
            redHigh += h.red[i];
            blueHigh += h.blue[i];
        }
        QVERIFY(redHigh > quint64(h.samples) / 3);
        QVERIFY(blueHigh > quint64(h.samples) / 3);
    }

    // Covers: F-FMT-03 (an animation larger than the decode target - its frames are
    // downscaled - plays)
    void F_FMT_03_animationLargerThanDecodeTargetPlays() {
        const QString gif = path("large.gif");
        QVERIFY(test::writeAnimatedGif(gif, QSize(3000, 2000), 4, 10));
        ViewerWindow* w = app->openPaths({gif});
        QVERIFY(w);
        w->resize(500, 400);
        QVERIFY(QTest::qWaitForWindowExposed(w, 5000));
        QVERIFY(waitShown(w, gif, 15000));
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY_WITH_TIMEOUT(anim->isActive() && anim->isPlaying(), 15000);
        QCOMPARE(anim->frameCount(), 4);
        const DecodedImagePtr first = anim->currentImage();
        QVERIFY(first);
        QVERIFY(first->image.width() < 3000); // downscaled to the window
        QTRY_VERIFY_WITH_TIMEOUT(anim->framesShown() >= 3, 15000);
    }

    // ---- B: animation player ----

    // Covers: F-FMT-03 (windows closed while frames decode: no access to the destroyed
    // player - run under ASan by `tools/check.sh --quick --sanitize`)
    void F_FMT_03_destroyWindowDuringPlayback() {
        const QString gif = path("long.gif");
        QVERIFY(test::writeAnimatedGif(gif, QSize(600, 400), 60, 2));
        for (int round = 0; round < 4; ++round) {
            ViewerWindow* w = app->openPaths({gif});
            QVERIFY(w);
            QVERIFY(QTest::qWaitForWindowExposed(w, 5000));
            QVERIFY(waitShown(w, gif));
            QTRY_VERIFY_WITH_TIMEOUT(w->animation()->isPlaying(), 10000);
            QTest::qWait(30 * round); // at different points of the decode
            QPointer<ViewerWindow> guard(w);
            w->close();
            delete guard.data(); // the player goes now, its decode task may still run
            QTest::qWait(20);
        }
        // The players alone, destroyed right after scheduling a decode.
        const DecodedImagePtr first = app->provider().cached(gif, Quality::Preview);
        QVERIFY(first);
        for (int i = 0; i < 40; ++i) {
            auto* p = new AnimationPlayer;
            p->open(gif, first, 60, true);
            p->step(1 + i % 5);
            delete p;
            if (i % 8 == 0)
                QTest::qWait(5);
        }
        QTest::qWait(200); // decode tasks finish; nothing is delivered to deleted players
    }

    // Covers: F-FMT-03, P-09 (the frame ring follows the image cache budget with a hard
    // cap; frames keep the provider's per-image byte cap, P-08)
    void P_09_animationRingFollowsBudget() {
        const QString gif = path("ring.gif");
        const QSize size(400, 300);
        QVERIFY(test::writeAnimatedGif(gif, size, 40, 2));
        QString error;
        auto decoded = DecoderRegistry::instance().decode(gif, DecodeRequest{}, &error);
        QVERIFY2(decoded, qPrintable(error));
        const qint64 frameBytes = decoded->image.sizeInBytes();
        auto first = std::make_shared<const DecodedImage>(*decoded);

        // A budget of 12 frames: ring = a quarter = 3 frames (not the old minimum of 5).
        AnimationPlayer p;
        p.setMemoryBudget(frameBytes * 12);
        p.open(gif, first, 40, true);
        QVERIFY(!p.cachesAllFrames());
        QCOMPARE(p.ringBytes(), frameBytes * 3);
        QVERIFY(p.maxHeldBytes() <= p.ringBytes());
        qint64 peak = 0;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 1200) {
            QTest::qWait(5);
            peak = std::max(peak, p.heldBytes());
        }
        QVERIFY2(peak <= p.maxHeldBytes(),
                 qPrintable(QStringLiteral("%1 > %2").arg(peak).arg(p.maxHeldBytes())));
        QTRY_VERIFY_WITH_TIMEOUT(p.framesShown() > 5, 10000); // still plays
        // Tiny budget: current + next frame only.
        p.stop();
        p.setMemoryBudget(frameBytes);
        p.open(gif, first, 40, true);
        QCOMPARE(p.maxHeldBytes(), frameBytes * 2);
        t.restart();
        while (t.elapsed() < 600) {
            QTest::qWait(5);
            QVERIFY(p.heldFrames() <= 2);
        }
        QTRY_VERIFY_WITH_TIMEOUT(p.framesShown() > 3, 10000);
        // The per-image byte cap reaches the frame decodes.
        p.stop();
        p.setMemoryBudget(0);
        p.setMaxDecodeBytes(frameBytes / 4);
        p.open(gif, first, 40, false);
        p.step(1);
        QTRY_VERIFY_WITH_TIMEOUT(p.currentFrame() == 1, 10000);
        QVERIFY(p.currentImage());
        QVERIFY(p.currentImage()->image.sizeInBytes() <= frameBytes / 4);
    }

    // ---- D: drag-out ----

    // Covers: F-FILE-07 (starting a drag of a full-resolution image costs the GUI thread
    // little: no smoothing of the full image there; the image data comes from a worker)
    void F_FILE_07_dragOutLightOnGuiThread() {
        // The full stage of a 24 MP HDR image (float buffer in linear Display P3).
        QImage full(6000, 4000, QImage::Format_RGBA16FPx4_Premultiplied);
        full.fill(QColor(40, 160, 200));
        full.setColorSpace(
            QColorSpace(QColorSpace::DisplayP3).withTransferFunction(QColorSpace::TransferFunction::Linear));
        QWidget source;
        source.resize(100, 100);
        int withPixmap = 0;
        setDragExecHookForTesting([&](QDrag* drag) {
            withPixmap += drag->pixmap().isNull() ? 0 : 1;
            return Qt::CopyAction;
        });
        // Best of a few runs (scheduling noise on a loaded machine).
        qint64 best = std::numeric_limits<qint64>::max();
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer t;
            t.start();
            QCOMPARE(startDragOut(&source, path("big.jpg"), full), Qt::CopyAction);
            best = std::min(best, t.elapsed());
        }
        QCOMPARE(withPixmap, 3);
        // Reference, measured the same way (a loaded machine slows both): the GUI-thread work
        // of the previous implementation - smooth scaling of the full image to the drag image
        // and to the thumbnail, each converted to 8-bit sRGB.
        qint64 old = std::numeric_limits<qint64>::max();
        for (int i = 0; i < 2; ++i) {
            QElapsedTimer t;
            t.start();
            for (int px : {kDragImageMaxPx, kDragThumbnailMaxPt * 2}) {
                const QImage s = full.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                     .convertedToColorSpace(QColorSpace::SRgb, QImage::Format_ARGB32);
                QVERIFY(!s.isNull());
            }
            old = std::min(old, t.elapsed());
        }
        qInfo("drag start %lld ms, previous GUI-thread work %lld ms", best, old);
        QVERIFY2(best * 4 < old,
                 qPrintable(
                     QStringLiteral("drag start %1 ms, previous GUI-thread work %2 ms").arg(best).arg(old)));
        // The image data still arrives (from the worker) with the bounded size, 8-bit.
        std::unique_ptr<QMimeData> mime(createDragMimeData(path("big.jpg"), full));
        auto* dm = qobject_cast<DragMimeData*>(mime.get());
        QVERIFY(dm);
        QVERIFY(mime->hasImage());
        QTRY_VERIFY_WITH_TIMEOUT(dm->isImageReady(), 20000);
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        QCOMPARE(std::max(img.width(), img.height()), kDragImageMaxPx);
        QCOMPARE(img.format(), QImage::Format_ARGB32);
        QCOMPARE(mime->urls(), QList<QUrl>{QUrl::fromLocalFile(path("big.jpg"))});
    }

    // ---- E: share menu ----

    // Covers: F-FILE-06 (the services are looked up on a worker, once per file type; the
    // menu shows a placeholder until they arrive and never blocks)
    void F_FILE_06_shareMenuFilledAsynchronously() {
#ifdef LF_GENERIC_PLATFORM
        QSKIP("no share sheet outside macOS (docs/LINUX.md)");
#endif
        QSemaphore gate;
        std::atomic<int> queries{0};
        std::atomic<bool> onGuiThread{false};
        QThread* gui = QThread::currentThread();
        ShareServiceCache::instance().setQueryForTesting([&](const QStringList& paths) {
            ++queries;
            if (QThread::currentThread() == gui)
                onGuiThread = true;
            gate.acquire();
            QList<ShareService> out;
            if (!paths.isEmpty())
                out << ShareService{QStringLiteral("AirDrop"), QImage()}
                    << ShareService{QStringLiteral("Mail"), QImage()};
            return out;
        });
        QMenu menu;
        const QStringList jpg{path("a.jpg")};
        QElapsedTimer t;
        t.start();
        populateShareMenu(&menu, jpg, {});
        QVERIFY(t.elapsed() < 1000); // returns with the query blocked on the gate
        QVERIFY(menu.findChild<QAction*>(QStringLiteral("share:pending")));
        QVERIFY(!menu.findChild<QAction*>(QStringLiteral("share:pending"))->isEnabled());
        QCOMPARE(menu.findChild<QAction*>(QStringLiteral("share:pending"))->text(),
                 QStringLiteral("Wird geladen …"));
        QVERIFY(menu.findChild<QAction*>(QStringLiteral("share:picker"))->isEnabled());
        // Opening it again while the query runs starts no second one.
        populateShareMenu(&menu, jpg, {});
        gate.release();
        QTRY_VERIFY(menu.findChild<QAction*>(QStringLiteral("share:Mail")));
        QVERIFY(!menu.findChild<QAction*>(QStringLiteral("share:pending")));
        QCOMPARE(queries.load(), 1);
        QVERIFY(!onGuiThread.load());
        // Same type (other file): from the cache at once.
        populateShareMenu(&menu, {path("b.jpeg")}, {});
        QVERIFY(menu.findChild<QAction*>(QStringLiteral("share:AirDrop")));
        QCOMPARE(queries.load(), 1);
        QCOMPARE(shareTypeKey(path("b.jpeg")), shareTypeKey(path("a.jpg"))); // public.jpeg
        // Another type: looked up once more.
        gate.release();
        populateShareMenu(&menu, {path("c.png")}, {});
        QTRY_VERIFY(menu.findChild<QAction*>(QStringLiteral("share:AirDrop")));
        QCOMPARE(queries.load(), 2);
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
    TstReview4 tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_review4.moc"
