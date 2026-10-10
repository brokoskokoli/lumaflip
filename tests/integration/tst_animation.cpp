// SPDX-License-Identifier: GPL-3.0-or-later
// F-FMT-03 playback as the user sees it (backlog B-2): an animated file opened like from the
// Finder or reached in its folder plays by itself, and the picture on the canvas changes -
// checked on the rendered framebuffer, not only on the player's frame index. Also the loop
// count of the file, the browser minimum for very short frame delays and long animations
// that do not fit the frame cache (ring of decoded frames).

#include "GifTestUtil.h"
#include "app/AnimationPlayer.h"
#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

QString corpus(const QString& name) {
    return QStringLiteral(LF_CORPUS_DIR "/generated/") + name;
}

class NoShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

// Dominant channel of a color: 0 red, 1 green, 2 blue, -1 none (gray, background).
int dominant(QColor c) {
    const int r = c.red(), g = c.green(), b = c.blue();
    if (r > g + 60 && r > b + 60)
        return 0;
    if (g > r + 60 && g > b + 60)
        return 1;
    if (b > r + 60 && b > g + 60)
        return 2;
    return -1;
}

bool near(QColor a, QColor b) {
    return std::abs(a.red() - b.red()) < 40 && std::abs(a.green() - b.green()) < 40 &&
           std::abs(a.blue() - b.blue()) < 40;
}

} // namespace

class TstAnimation : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<TrashService> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NoShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    QString copyCorpus(const QString& name) {
        const QString target = path(name);
        return QFile::copy(corpus(name), target) ? target : QString();
    }
    static bool waitShown(ViewerWindow* w, const QString& p, int ms = 8000) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, ms);
    }
    // Like a Finder double click (QFileOpenEvent -> Application::openPaths).
    ViewerWindow* open(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 8000) || !waitShown(w, file))
            return nullptr;
        return w;
    }
    static ImageCanvas* canvasOf(ViewerWindow* w) { return w->view()->findChild<ImageCanvas*>(); }
    // Color in the middle of the rendered canvas (the image is fitted and centered).
    static QColor centerColor(ImageCanvas* canvas) {
        const QImage fb = canvas->grabFramebuffer();
        return fb.isNull() ? QColor() : fb.pixelColor(fb.width() / 2, fb.height() / 2);
    }
    // Samples the rendered canvas for `ms` and returns the distinct colors seen, in order.
    static QList<QColor> sampleColors(ImageCanvas* canvas, int ms) {
        QList<QColor> seen;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            const QColor c = centerColor(canvas);
            if (c.isValid() && (seen.isEmpty() || !near(seen.last(), c)))
                seen << c;
            QTest::qWait(30);
        }
        return seen;
    }
    static QString describe(const QList<QColor>& colors) {
        QStringList l;
        for (const QColor& c : colors)
            l << c.name();
        return l.join(QStringLiteral(" "));
    }
    static QString state(ViewerWindow* w) {
        AnimationPlayer* a = w->animation();
        return QStringLiteral("active=%1 playing=%2 finished=%3 frame=%4/%5 shown=%6 status='%7'")
            .arg(a->isActive())
            .arg(a->isPlaying())
            .arg(a->isFinished())
            .arg(a->currentFrame())
            .arg(a->frameCount())
            .arg(a->framesShown())
            .arg(w->frameStatusText());
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

    // Covers: F-FMT-03 (corpus GIF, animated WebP and APNG play by themselves; the canvas
    // shows red, green and blue in turn - the frames of tests/corpus/generated/anim.*)
    void F_FMT_03_corpusAnimationsPlayOnCanvas_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("upscaleWithFilmstrip"); // view settings of the B-2 report
        for (bool user : {false, true}) {
            const char* suffix = user ? " upscaled+filmstrip" : "";
            QTest::addRow("gif%s", suffix) << QStringLiteral("anim.gif") << user;
            QTest::addRow("webp%s", suffix) << QStringLiteral("anim.webp") << user;
            QTest::addRow("apng%s", suffix) << QStringLiteral("anim.png") << user;
        }
    }
    void F_FMT_03_corpusAnimationsPlayOnCanvas() {
        QFETCH(QString, name);
        QFETCH(bool, upscaleWithFilmstrip);
        if (!QFileInfo::exists(corpus(name)))
            QSKIP("format corpus file missing - run tests/corpus/make_corpus.sh");
        if (upscaleWithFilmstrip) {
            Settings::instance().setValue(key::ZoomMode, QStringLiteral("fitUpscale"));
            Settings::instance().setValue(key::ShowFilmstrip, true);
        }
        const QString file = copyCorpus(name);
        ViewerWindow* w = open(file);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isActive() && anim->isPlaying(), qPrintable(state(w)), 5000);
        QCOMPARE(anim->frameCount(), 3);
        QCOMPARE(anim->loopCount(), 0); // the corpus files loop endlessly
        ImageCanvas* canvas = canvasOf(w);
        QVERIFY(canvas);
        // 100 + 200 + 300 ms per round: two rounds show every frame at least once.
        const QList<QColor> seen = sampleColors(canvas, 1500);
        QSet<int> channels;
        for (const QColor& c : seen)
            channels.insert(dominant(c));
        QVERIFY2(channels.contains(0) && channels.contains(1) && channels.contains(2),
                 qPrintable(QStringLiteral("colors on the canvas: %1; %2").arg(describe(seen), state(w))));
        QVERIFY2(anim->framesShown() >= 4, qPrintable(state(w)));
        QVERIFY(w->frameStatusText().startsWith(QStringLiteral("Bild ")));
    }

    // Covers: F-FMT-03, F-NAV-01 (an animated GIF reached with → in its folder - prefetched as
    // a neighbour before - plays, and the canvas changes)
    void F_FMT_03_playsWhenReachedInFolder() {
        QImage still(QSize(120, 90), QImage::Format_RGB32);
        still.fill(Qt::gray);
        QVERIFY(still.save(path(QStringLiteral("a_still.png"))));
        const QString gif = path(QStringLiteral("b_anim.gif"));
        QVERIFY(test::writeAnimatedGif(gif, QSize(120, 90), 4, 8));
        ViewerWindow* w = open(path(QStringLiteral("a_still.png")));
        QVERIFY(w);
        QTRY_VERIFY_WITH_TIMEOUT(!w->collection()->isLoading() && w->collection()->count() == 2, 8000);
        QTest::qWait(300); // let the neighbour be prefetched
        QVERIFY(!w->animation()->isActive());
        QTest::keyClick(w->view(), Qt::Key_Right);
        QVERIFY(waitShown(w, gif));
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isPlaying(), qPrintable(state(w)), 5000);
        const QList<QColor> seen = sampleColors(canvasOf(w), 1200);
        // Four differently colored frames of 80 ms: several colors within 1.2 s.
        QVERIFY2(seen.size() >= 3,
                 qPrintable(QStringLiteral("colors: %1; %2").arg(describe(seen), state(w))));
    }

    // Covers: F-FMT-03, P-09 (an animation larger than the frame cache plays through the
    // ring of decoded frames: it keeps moving and stays within its memory bound)
    void F_FMT_03_longAnimationPlaysThroughRing() {
        const QString gif = path(QStringLiteral("long.gif"));
        // 1000 x 800 x 4 bytes x 24 frames = 77 MB > the 64 MB cache-all bound.
        QVERIFY(test::writeAnimatedGif(gif, QSize(1000, 800), 24, 4));
        ViewerWindow* w = open(gif);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isPlaying(), qPrintable(state(w)), 8000);
        QTRY_VERIFY2_WITH_TIMEOUT(anim->framesShown() >= 10, qPrintable(state(w)), 15000);
        QVERIFY(anim->heldBytes() <= std::max(anim->maxHeldBytes(), anim->cacheAllBytes()));
        const QList<QColor> seen = sampleColors(canvasOf(w), 800);
        QVERIFY2(seen.size() >= 2,
                 qPrintable(QStringLiteral("colors: %1; %2").arg(describe(seen), state(w))));
    }

    // Covers: F-FMT-03 (the file's loop count: a GIF without loop extension plays once and
    // stays on its last frame; Space plays it again; delays of 10 ms play as 100 ms)
    void F_FMT_03_loopCountAndShortDelays() {
        const QString once = path(QStringLiteral("once.gif"));
        QVERIFY(test::writeAnimatedGif(once, QSize(80, 60), 3, 1, -1)); // 10 ms, no NETSCAPE loop
        ViewerWindow* w = open(once);
        QVERIFY(w);
        AnimationPlayer* anim = w->animation();
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isActive(), qPrintable(state(w)), 5000);
        QCOMPARE(anim->loopCount(), 1);
        QVERIFY(anim->currentImage());
        QCOMPARE(AnimationPlayer::durationMs(*anim->currentImage()), AnimationPlayer::kDefaultDurationMs);
        QElapsedTimer t;
        t.start();
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isFinished(), qPrintable(state(w)), 5000);
        // Three frames of 100 ms (browser minimum), not of 10 ms: about 200 ms until the end.
        QVERIFY2(t.elapsed() >= 120, qPrintable(QString::number(t.elapsed())));
        QVERIFY(!anim->isPlaying());
        QCOMPARE(anim->currentFrame(), 2);
        const int shown = anim->framesShown();
        QTest::qWait(400);
        QCOMPARE(anim->framesShown(), shown); // stays on the last frame
        const QColor last = centerColor(canvasOf(w));
        QTest::qWait(300);
        QVERIFY(near(centerColor(canvasOf(w)), last)); // the canvas stays on it too

        QTest::keyClick(w->view(), Qt::Key_Space); // plays again from the start
        QVERIFY(anim->isPlaying());
        QTRY_VERIFY_WITH_TIMEOUT(anim->framesShown() > shown, 3000);
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isFinished(), qPrintable(state(w)), 5000);
        QCOMPARE(anim->currentFrame(), 2);

        // NETSCAPE loop count 1 = one repetition: two plays, then it stops.
        const QString twice = path(QStringLiteral("twice.gif"));
        QVERIFY(test::writeAnimatedGif(twice, QSize(80, 60), 3, 2, 1));
        w->collection()->setCurrentPath(twice);
        QVERIFY(waitShown(w, twice));
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isActive() && anim->path() == twice, qPrintable(state(w)), 5000);
        QCOMPARE(anim->loopCount(), 2);
        QTRY_VERIFY2_WITH_TIMEOUT(anim->isFinished(), qPrintable(state(w)), 5000);
        QVERIFY2(anim->framesShown() >= 5, qPrintable(state(w))); // 0-1-2-0-1-2: five changes
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
    TstAnimation tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_animation.moc"
