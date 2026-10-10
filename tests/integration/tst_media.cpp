// SPDX-License-Identifier: GPL-3.0-or-later
// M2 media end to end: animation playback (F-FMT-03), multi-page files (F-FMT-04),
// slideshow (F-SLIDE-01/02) and saving the rotation with undo (F-EDIT-02), with the real
// Application, windows, decoders and GPU canvas (cocoa platform, see tst_app.cpp).

#include "GifTestUtil.h"
#include "app/AnimationPlayer.h"
#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/SlideshowController.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/LosslessTransform.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

QString corpus(const QString& name) {
    return QStringLiteral(LF_CORPUS_DIR "/generated/") + name;
}

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

QByteArray sha(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256)
                                       : QByteArray();
}

class NoShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

} // namespace

class TstMedia : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<TrashService> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NoShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    QString copyCorpus(const QString& name, const QString& as = {}) {
        const QString target = path(as.isEmpty() ? name : as);
        return QFile::copy(corpus(name), target) ? target : QString();
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 5000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    ViewerWindow* open(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000) || !waitShown(w, file))
            return nullptr;
        if (!QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 5000))
            return nullptr;
        return w;
    }
    QAction* action(Action a) const { return app->menu()->action(a); }
    static void key(ViewerWindow* w, Qt::Key k, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QTest::keyClick(w->view(), k, mods);
    }
    QStringList folderOfPngs(int n) {
        QStringList out;
        for (int i = 0; i < n; ++i)
            out << makeImage(path(QStringLiteral("img_%1.png").arg(i, 2, 10, QLatin1Char('0'))),
                             QColor::fromHsv(i * 40 % 360, 200, 200));
        return out;
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
        if (!QFileInfo::exists(corpus(QStringLiteral("anim.gif"))))
            QSKIP("format corpus missing - run tests/corpus/make_corpus.sh");
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
    }

    void cleanup() {
        for (QWidget* w : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(w))
                box->close();
        app.reset();
        QTest::qWait(10);
        ops.reset();
        trash.reset();
        shell.reset();
        dir.reset();
    }

    // ---- F-FMT-03 animation ----

    // Covers: F-FMT-03 (frame timing of the corpus GIF: 100/200/300 ms, status "Bild n/3")
    void F_FMT_03_frameTimingCorpusGif() {
        const QString gif = copyCorpus(QStringLiteral("anim.gif"));
        ViewerWindow* w = open(gif);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY_WITH_TIMEOUT(anim->isActive() && anim->isPlaying(), 3000);
        QCOMPARE(anim->frameCount(), 3);
        QVERIFY(anim->cachesAllFrames()); // small animation: every frame is kept
        QVERIFY(w->statusText().contains(QStringLiteral("Bild ")));

        QElapsedTimer clock;
        clock.start();
        QList<QPair<int, qint64>> shown;
        connect(anim, &AnimationPlayer::frameChanged, this,
                [&](int index, const DecodedImagePtr&) { shown.append({index, clock.elapsed()}); });
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() >= 8, 5000);
        const int durations[3] = {100, 200, 300};
        // Frames are due at absolute times (first + sum of the durations): a frame shown late
        // (busy machine) shortens the next one instead of shifting all later frames. So the
        // offsets from that schedule stay within a bounded band (the first timestamp may be
        // late itself, so the band is relative); a drift would make them grow frame by frame.
        int checked = 0;
        qint64 due = shown[0].second;
        qint64 minOffset = 0, maxOffset = 0;
        for (int i = 1; i < shown.size(); ++i) {
            const int prev = shown[i - 1].first;
            QCOMPARE(shown[i].first, (prev + 1) % 3); // in order, nothing skipped
            due += durations[prev];
            const qint64 offset = shown[i].second - due;
            minOffset = std::min(minOffset, offset);
            maxOffset = std::max(maxOffset, offset);
            ++checked;
        }
        QVERIFY2(
            maxOffset - minOffset <= 250,
            qPrintable(QStringLiteral("offsets from the schedule %1..%2 ms").arg(minOffset).arg(maxOffset)));
        QVERIFY(checked >= 7);
        // The view shows the frames (texture reuse in the canvas, same size).
        auto* canvas = w->view()->findChild<ImageCanvas*>();
        QVERIFY(canvas);
        QTRY_VERIFY(canvas->framesReusingTextures() > 0);
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Bild %1/3").arg(anim->currentFrame() + 1)));
    }

    // Covers: F-FMT-03, F-IN-06 (Space plays/pauses when animated, ',' / '.' step, speed)
    void F_FMT_03_playPauseStepAndSpeed() {
        const QString gif = copyCorpus(QStringLiteral("anim.gif"));
        QVERIFY(!makeImage(path("z_after.png"), Qt::gray).isEmpty());
        ViewerWindow* w = open(gif);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY_WITH_TIMEOUT(anim->isPlaying(), 3000);
        key(w, Qt::Key_Space); // animated: pause instead of next image
        QVERIFY(!anim->isPlaying());
        QCOMPARE(w->currentPath(), gif);
        const int f = anim->currentFrame();
        key(w, Qt::Key_Period);
        QTRY_COMPARE(anim->currentFrame(), (f + 1) % 3);
        QVERIFY(!anim->isPlaying());
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("Bild %1/3").arg((f + 1) % 3 + 1)));
        QCOMPARE(w->view()->image(), anim->currentImage());
        key(w, Qt::Key_Comma);
        key(w, Qt::Key_Comma);
        QTRY_COMPARE(anim->currentFrame(), (f + 2) % 3);
        QTest::qWait(400);
        QCOMPARE(anim->currentFrame(), (f + 2) % 3); // paused: stays
        key(w, Qt::Key_Space);
        QVERIFY(anim->isPlaying());

        // Speed (menu Bild → Animation → Geschwindigkeit): 0.25× … 4×.
        QAction* fast = app->menu()->command(QStringLiteral("speed:2"));
        QVERIFY(fast);
        fast->trigger();
        QCOMPARE(anim->speed(), 2.0);
        w->setAnimationSpeed(10.0);
        QCOMPARE(anim->speed(), AnimationPlayer::kMaxSpeed);
        w->setAnimationSpeed(0.01);
        QCOMPARE(anim->speed(), AnimationPlayer::kMinSpeed);
        QVERIFY(w->frameStatusText().contains(QStringLiteral("×")));
        // Menu actions exist with their keys.
        QVERIFY(action(Action::NextFrame)->shortcuts().contains(QKeySequence(Qt::Key_Period)));
        QVERIFY(action(Action::PreviousFrame)->shortcuts().contains(QKeySequence(Qt::Key_Comma)));

        // Not animated: Space goes to the next image (F-IN-06 default).
        w->collection()->setCurrentPath(path("z_after.png"));
        QVERIFY(waitShown(w, path("z_after.png")));
        QVERIFY(!anim->isActive());
        key(w, Qt::Key_Space, Qt::ShiftModifier);
        QTRY_COMPARE(w->currentPath(), gif);
    }

    // Covers: F-FMT-03 (playback stops when another image is shown or the window hides)
    void F_FMT_03_stopsWhenSwitchingImageOrHidden() {
        const QString gif = copyCorpus(QStringLiteral("anim.gif"), QStringLiteral("a.gif"));
        const QString png = makeImage(path("b.png"), Qt::darkCyan);
        ViewerWindow* w = open(gif);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY_WITH_TIMEOUT(anim->isPlaying(), 3000);

        w->showMinimized();
        QTRY_VERIFY_WITH_TIMEOUT(!anim->isPlaying(), 3000);
        QTest::qWait(800); // let the minimize animation finish (macOS reports it late)
        QVERIFY(!anim->isPlaying());
        w->showNormal();
        w->raise();
        QTRY_VERIFY_WITH_TIMEOUT(!w->isMinimized() && anim->isPlaying(), 5000);
        QTest::qWait(800);
        QTRY_VERIFY_WITH_TIMEOUT(!w->isMinimized() && anim->isPlaying(), 5000);

        int framesAfterSwitch = 0;
        key(w, Qt::Key_Right);
        QCOMPARE(w->currentPath(), png);
        QVERIFY(!anim->isActive());
        connect(anim, &AnimationPlayer::frameChanged, this, [&] { ++framesAfterSwitch; });
        QVERIFY(waitShown(w, png));
        QTest::qWait(450);
        QCOMPARE(framesAfterSwitch, 0);
        QCOMPARE(w->shownPath(), png);
        QVERIFY(w->frameStatusText().isEmpty());
        // Back to the animation: it plays again.
        key(w, Qt::Key_Left);
        QTRY_VERIFY_WITH_TIMEOUT(anim->isPlaying() && anim->path() == gif, 3000);
    }

    // Covers: F-FMT-03, P-09 (a long animation keeps only a bounded ring of decoded frames)
    void F_FMT_03_memoryBoundLongAnimation() {
        const QString gif = path("long.gif");
        QVERIFY(test::writeAnimatedGif(gif, QSize(400, 300), 200, 2)); // 200 frames, 20 ms
        ViewerWindow* w = open(gif);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY_WITH_TIMEOUT(anim->isPlaying(), 20000);
        QCOMPARE(anim->frameCount(), 200);
        QVERIFY(!anim->cachesAllFrames()); // 200 × 480 KB > 64 MB
        w->setAnimationSpeed(4.0);
        qint64 peak = 0;
        int peakFrames = 0;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 2000) {
            QTest::qWait(10);
            peak = std::max(peak, anim->heldBytes());
            peakFrames = std::max(peakFrames, anim->heldFrames());
        }
        QVERIFY2(peak <= anim->maxHeldBytes(),
                 qPrintable(QStringLiteral("%1 > %2").arg(peak).arg(anim->maxHeldBytes())));
        QVERIFY(peak <= AnimationPlayer::kRingBytes + 3 * 400 * 300 * 4);
        QVERIFY(peakFrames < 200);
        // It plays (on a loaded machine the decoder may need longer than the 2 s above).
        QTRY_VERIFY2_WITH_TIMEOUT(anim->framesShown() > 20, qPrintable(QString::number(anim->framesShown())),
                                  30000);
        QVERIFY(anim->heldBytes() <= anim->maxHeldBytes());
        // Frame colors follow the file.
        const DecodedImagePtr cur = anim->currentImage();
        QVERIFY(cur);
        const QColor got = cur->image.pixelColor(10, 10);
        const QColor want = test::gifPaletteColor(anim->currentFrame());
        QVERIFY(std::abs(got.red() - want.red()) <= 3 && std::abs(got.green() - want.green()) <= 3);
    }

    // ---- F-FMT-04 pages ----

    // Covers: F-FMT-04, F-IN-04 (⌥↓/⌥↑ and Bild↓/Bild↑ with "page", status "Seite 2/3", cache per page)
    void F_FMT_04_multipageNavigation() {
        const QString tiff = copyCorpus(QStringLiteral("multipage.tiff"));
        QVERIFY(!makeImage(path("z.png"), Qt::gray).isEmpty());
        ViewerWindow* w = open(tiff);
        QVERIFY(w);
        QTRY_COMPARE(w->pageCount(), 3);
        QVERIFY(w->statusText().contains(QStringLiteral("Seite 1/3")));
        QVERIFY(action(Action::NextPage)->shortcuts().contains(QKeySequence(Qt::AltModifier | Qt::Key_Down)));
        QVERIFY(
            action(Action::PreviousPage)->shortcuts().contains(QKeySequence(Qt::AltModifier | Qt::Key_Up)));

        action(Action::NextPage)->trigger();
        QCOMPARE(w->currentPage(), 1);
        QTRY_COMPARE(w->view()->image()->image.size(), QSize(32, 24));
        QVERIFY(w->statusText().contains(QStringLiteral("Seite 2/3")));
        QVERIFY(w->statusText().contains(QStringLiteral("32 × 24")));
        QCOMPARE(w->currentPath(), tiff); // same file

        // Bild↓ with the "page" choice (F-IN-04): next page, at the end the next image.
        app->bindings().setChoice(QStringLiteral("pageUpDown"), QStringLiteral("page"));
        key(w, Qt::Key_PageDown);
        QCOMPARE(w->currentPage(), 2);
        QTRY_COMPARE(w->view()->image()->image.size(), QSize(16, 12));
        QVERIFY(w->statusText().contains(QStringLiteral("Seite 3/3")));
        action(Action::NextPage)->trigger(); // beyond the last page: hint, stays
        QCOMPARE(w->currentPage(), 2);
        QCOMPARE(w->toastText(), QStringLiteral("Letzte Seite"));
        action(Action::PreviousPage)->trigger();
        QTRY_COMPARE(w->view()->image()->image.size(), QSize(32, 24));
        // Each page is cached separately.
        ImageProvider& provider = app->provider();
        QVERIFY(provider.cached(tiff, Quality::Preview, {}, 2));
        QCOMPARE(provider.cached(tiff, Quality::Preview, {}, 2)->image.size(), QSize(16, 12));
        QVERIFY(provider.cached(tiff, Quality::Preview, {}, 0));
        QCOMPARE(provider.cached(tiff, Quality::Preview, {}, 0)->image.size(), QSize(64, 48));

        // Another file starts at page 1 again; single-page files show no page status.
        key(w, Qt::Key_Right);
        QVERIFY(waitShown(w, path("z.png")));
        QCOMPARE(w->currentPage(), 0);
        QVERIFY(w->frameStatusText().isEmpty());
        key(w, Qt::Key_Left);
        QVERIFY(waitShown(w, tiff));
        QCOMPARE(w->currentPage(), 0);
        QTRY_COMPARE(w->view()->image()->image.size(), QSize(64, 48));
    }

    // ---- F-SLIDE-01/02 slideshow ----

    // Covers: F-SLIDE-01 (⇧⌘F/S, fullscreen, 1–9 set the interval, Space pauses, ←/→, Esc ends)
    void F_SLIDE_01_keysIntervalPauseEsc() {
        const QStringList files = folderOfPngs(4);
        ViewerWindow* w = open(files.first());
        QVERIFY(w);
        QVERIFY(action(Action::ToggleSlideshow)->shortcuts().contains(QKeySequence(Qt::Key_S)));
        QVERIFY(action(Action::ToggleSlideshow)
                    ->shortcuts()
                    .contains(QKeySequence(Qt::ShiftModifier | Qt::ControlModifier | Qt::Key_F)));
        QVERIFY(InputBindings::isActionAvailable(Action::ToggleSlideshow));
        action(Action::ToggleSlideshow)->trigger();
        QVERIFY(w->isSlideshowRunning());
        QVERIFY(w->isFullScreen());
        QCOMPARE(w->slideshow()->intervalMs(), 3000); // default (Preferences)

        const ZoomMode zoom = w->view()->zoomMode();
        key(w, Qt::Key_2); // interval 2 s, not "Breite füllen"
        QCOMPARE(w->slideshow()->intervalMs(), 2000);
        QCOMPARE(Settings::instance().value(key::SlideshowIntervalSec).toInt(), 2);
        QCOMPARE(w->view()->zoomMode(), zoom);
        key(w, Qt::Key_7);
        QCOMPARE(w->slideshow()->intervalMs(), 7000);

        key(w, Qt::Key_Space);
        QVERIFY(w->slideshow()->isPaused());
        key(w, Qt::Key_Space);
        QVERIFY(!w->slideshow()->isPaused());

        key(w, Qt::Key_Right);
        QCOMPARE(w->currentPath(), files.at(1));
        key(w, Qt::Key_Left);
        QCOMPARE(w->currentPath(), files.at(0));

        key(w, Qt::Key_Escape);
        QVERIFY(!w->isSlideshowRunning());
        QTRY_VERIFY_WITH_TIMEOUT(!w->isFullScreen(), 5000);
        // Digits are zoom keys again.
        key(w, Qt::Key_2);
        QCOMPARE(w->slideshow()->intervalMs(), 7000);
    }

    // Covers: F-SLIDE-01 (images advance by the interval, prefetch follows the slideshow order)
    void F_SLIDE_01_advancesAndPrefetchesInOrder() {
        Settings::instance().setValue(key::SlideshowShuffle, true);
        const QStringList files = folderOfPngs(8);
        ViewerWindow* w = open(files.first());
        QVERIFY(w);
        // A seed whose next images are not the collection neighbours.
        quint32 seed = 1;
        for (; seed < 200; ++seed) {
            w->startSlideshow(seed);
            const QStringList next = w->slideshow()->upcoming(3);
            w->stopSlideshow();
            if (!next.contains(files.at(1)) && !next.contains(files.at(2)) && !next.contains(files.at(3)))
                break;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!w->isFullScreen(), 5000);
        w->startSlideshow(seed);
        const QStringList upcoming = w->slideshow()->upcoming(3);
        QCOMPARE(upcoming.size(), 3);
        for (const QString& p : upcoming)
            QTRY_VERIFY2_WITH_TIMEOUT(app->provider().cached(p) != nullptr, qPrintable(p), 5000);
        w->slideshow()->setIntervalMs(250);
        QTRY_COMPARE_WITH_TIMEOUT(w->currentPath(), upcoming.at(0), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(w->currentPath(), upcoming.at(1), 3000);
        w->stopSlideshow();
    }

    // Covers: F-SLIDE-02 (shuffle order is deterministic for a seed and starts with the current image)
    void F_SLIDE_02_shuffleDeterministicWithSeed() {
        QStringList paths;
        for (int i = 0; i < 10; ++i)
            paths << QStringLiteral("/x/%1.jpg").arg(i);
        SlideshowController a, b, c;
        SlideshowController::Options o;
        o.shuffle = true;
        o.seed = 42;
        a.start(paths, paths.at(3), o);
        b.start(paths, paths.at(3), o);
        o.seed = 43;
        c.start(paths, paths.at(3), o);
        QCOMPARE(a.order(), b.order());
        QVERIFY(a.order() != c.order());
        QCOMPARE(a.order().first(), paths.at(3));
        QStringList sorted = a.order();
        sorted.sort();
        QStringList expected = paths;
        expected.sort();
        QCOMPARE(sorted, expected); // a permutation: every image once
        // Without shuffle: collection order from the current image on.
        o.shuffle = false;
        SlideshowController d;
        d.start(paths, paths.at(3), o);
        QCOMPARE(d.currentPath(), paths.at(3));
        QCOMPARE(d.upcoming(2), (QStringList{paths.at(4), paths.at(5)}));
    }

    // Covers: F-SLIDE-02 (repeat on: wraps around; off: the slideshow ends after the last image)
    void F_SLIDE_02_repeatAndEnd() {
        const QStringList files = folderOfPngs(3);
        ViewerWindow* w = open(files.at(1));
        QVERIFY(w);
        Settings::instance().setValue(key::SlideshowRepeat, true);
        w->startSlideshow(1);
        w->slideshow()->setIntervalMs(200);
        QTRY_COMPARE_WITH_TIMEOUT(w->currentPath(), files.at(2), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(w->currentPath(), files.at(0), 3000); // wrapped
        w->stopSlideshow();

        Settings::instance().setValue(key::SlideshowRepeat, false);
        w->collection()->setCurrentPath(files.at(1));
        w->startSlideshow(1);
        w->slideshow()->setIntervalMs(200);
        QTRY_COMPARE_WITH_TIMEOUT(w->currentPath(), files.at(2), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!w->isSlideshowRunning(), 3000); // ended at the last image
        QCOMPARE(w->currentPath(), files.at(2));
        QCOMPARE(w->toastText(), QStringLiteral("Diashow beendet"));
        QTRY_VERIFY_WITH_TIMEOUT(!w->isFullScreen(), 5000);
    }

    // Covers: F-SLIDE-02 (subtle crossfade between images; off: hard cut)
    void F_SLIDE_02_crossfadeHappens() {
        const QStringList files = folderOfPngs(3);
        ViewerWindow* w = open(files.first());
        QVERIFY(w);
        auto* canvas = w->view()->findChild<ImageCanvas*>();
        QVERIFY(canvas);
        Settings::instance().setValue(key::SlideshowCrossfade, true);
        w->startSlideshow(1);
        QTRY_VERIFY_WITH_TIMEOUT(w->isFullScreen(), 5000);
        QTest::qWait(300);
        const int before = canvas->crossfadeFramesRendered();
        key(w, Qt::Key_Right);
        QVERIFY(waitShown(w, files.at(1)));
        QTRY_VERIFY_WITH_TIMEOUT(canvas->crossfadeFramesRendered() > before, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas->isCrossfading(),
                                 15000); // ends after ~300 ms (loaded machine: later)

        Settings::instance().setValue(key::SlideshowCrossfade, false);
        const int afterOn = canvas->crossfadeFramesRendered();
        key(w, Qt::Key_Right);
        QVERIFY(waitShown(w, files.at(2)));
        QTest::qWait(400);
        QCOMPARE(canvas->crossfadeFramesRendered(), afterOn);
        w->stopSlideshow();
    }

    // ---- F-EDIT-02 saving the rotation ----

    // Covers: F-EDIT-02 (⌘S writes the EXIF orientation, view stays upright, ⌘Z restores the bytes)
    void F_EDIT_02_saveRotationAndUndo() {
        const QString jpg = copyCorpus(QStringLiteral("orient_1.jpg"), QStringLiteral("photo.jpg"));
        ViewerWindow* w = open(jpg);
        QVERIFY(w);
        QVERIFY(action(Action::SaveRotation)
                    ->shortcuts()
                    .contains(QKeySequence(Qt::ControlModifier | Qt::Key_S)));
        const QByteArray original = sha(jpg);
        action(Action::SaveRotation)->trigger(); // nothing rotated yet
        QVERIFY(w->toastText().contains(QStringLiteral("Keine Drehung")));

        action(Action::RotateRight)->trigger();
        QCOMPARE(w->view()->rotation(), 1);
        action(Action::SaveRotation)->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(sha(jpg) != original, 5000);
        QFile saved(jpg);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(jpeg::exifOrientation(saved.readAll()), 6);
        saved.close();
        // The file is reloaded upright, the view rotation is gone (no double rotation).
        QTRY_COMPARE_WITH_TIMEOUT(w->view()->image()->fullSize, QSize(48, 64), 5000);
        QCOMPARE(w->view()->rotation(), 0);
        QTRY_VERIFY(w->toastText().contains(QStringLiteral("Drehung gespeichert")));
        const QByteArray savedSha = sha(jpg);

        ops->undo();
        QTRY_COMPARE_WITH_TIMEOUT(sha(jpg), original, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(w->view()->image()->fullSize, QSize(64, 48), 5000);
        ops->redo();
        QTRY_COMPARE_WITH_TIMEOUT(sha(jpg), savedSha, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(w->view()->image()->fullSize, QSize(48, 64), 5000);
    }

    // Covers: F-EDIT-02 (RAW files are refused with a message; the file stays untouched)
    void F_EDIT_02_rawRefused() {
        // A DNG named file (TIFF data): refused by its RAW suffix even if the type looks like TIFF.
        const QString tif = path("raw.png");
        QVERIFY(!makeImage(tif, Qt::darkRed).isEmpty());
        const QString dng = path("raw.dng");
        QVERIFY(QFile::rename(tif, dng));
        ViewerWindow* w = open(dng);
        QVERIFY(w);
        const QByteArray before = sha(dng);
        action(Action::RotateLeft)->trigger();
        action(Action::SaveRotation)->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(w->toastText().contains(QStringLiteral("RAW")), 5000);
        QCOMPARE(sha(dng), before);
        QCOMPARE(w->view()->rotation(), 3); // view rotation stays
    }

    // Covers: F-EDIT-02 (lossless JPEG strategy: partial edge blocks -> question; trimming is lossless)
    void F_EDIT_02_losslessStrategyAsksBeforeTrim() {
        if (!jpeg::dctTransformAvailable())
            QSKIP("built without libjpeg-turbo");
        Settings::instance().setValue(key::RotationSaveStrategy, QStringLiteral("lossless"));
        const QString jpg = makeImage(path("odd.jpg"), Qt::darkGreen, QSize(70, 50));
        QVERIFY(!jpg.isEmpty());
        ViewerWindow* w = open(jpg);
        QVERIFY(w);
        const QByteArray before = sha(jpg);
        action(Action::RotateRight)->trigger();
        action(Action::SaveRotation)->trigger();
        QMessageBox* box = nullptr;
        QTRY_VERIFY_WITH_TIMEOUT((box = w->findChild<QMessageBox*>(QStringLiteral("lfRotationTrim"))), 5000);
        QCOMPARE(sha(jpg), before); // nothing written while asking
        auto* trim = box->findChild<QPushButton*>(QStringLiteral("lfRotationPrimary"));
        QVERIFY(trim);
        trim->click();
        QTRY_VERIFY_WITH_TIMEOUT(sha(jpg) != before, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(w->view()->image()->fullSize, QSize(48, 70), 5000);
        QCOMPARE(w->view()->rotation(), 0);
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
    TstMedia tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_media.moc"
