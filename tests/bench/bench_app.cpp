// SPDX-License-Identifier: GPL-3.0-or-later
// App-level benchmarks (SPEC 7): the whole path Application::openPaths -> window ->
// Collection -> ImageProvider -> ImageView -> rendered frame, in-process.
//
// P-01 "Kaltstart bis erstes Bild": measured from creating the shell and opening a
//      24 MP JPEG that was never decoded in this process until the first frame with
//      the screen-sized image (Quality::Screen, not the embedded preview) is
//      rendered. Process launch + dyld (~100 ms on the reference Mac) are not
//      included; P_01_ProcessLaunchToFirstFrame24MP covers them when the app
//      supports LF_EXIT_AFTER_FIRST_FRAME (see there).
//      P_01_ColdOpenFirstPreview24MP is the same path up to the first preview stage
//      (informational, same limit).
// P-02 "Öffnen bei laufender App": another (undecoded) 24 MP JPEG in a new window.
// P-06 "Halten von →": key auto-repeat at 30 Hz for 2 s over a folder of 24 MP
//      JPEGs; result = ms per displayed image (limit 66.7 ms = 15 images/s).
//
// Test images: copies of tests/corpus/generated/big_24mp.jpg (or a generated 24 MP
// JPEG) in $TMPDIR/lumaflip-bench-24mp-v1, created once and reused.

#include "Bench.h"

#include "app/Application.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/ViewerWindow.h"
#include "core/Collection.h"
#include "core/Settings.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <cstdio>
#include <memory>

namespace {

constexpr int kFolderImages = 80;

bool gpuPlatform() {
    const QString p = QGuiApplication::platformName();
    return p != QLatin1String("offscreen") && p != QLatin1String("minimal");
}

QString make24MPSource(const QString& folder) {
    const QString corpus = QStringLiteral(LF_CORPUS_DIR "/generated/big_24mp.jpg");
    if (QFileInfo(corpus).size() > 0)
        return corpus;
    const QString generated = folder + QStringLiteral("/source.jpg");
    if (QFileInfo(generated).size() > 0)
        return generated;
    QImage img(6000, 4000, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient g(0, 0, 6000, 4000);
    g.setColorAt(0, QColor(20, 60, 140));
    g.setColorAt(1, QColor(230, 180, 60));
    p.fillRect(img.rect(), g);
    QRandomGenerator rng(42);
    for (int i = 0; i < 4000; ++i)
        p.fillRect(QRect(rng.bounded(6000), rng.bounded(4000), 40, 40), QColor::fromRgb(rng.generate()));
    p.end();
    return img.save(generated, "JPG", 90) ? generated : QString();
}

// Folder with `kFolderImages` distinct files (same pixels, separate cache keys).
QString benchFolder(const QString& name, int count) {
    const QString base = QDir::tempPath() + QStringLiteral("/lumaflip-bench-24mp-v1");
    const QString folder = base + QLatin1Char('/') + name;
    if (!QDir().mkpath(folder))
        return {};
    const QString source = make24MPSource(base);
    if (source.isEmpty())
        return {};
    for (int i = 0; i < count; ++i) {
        const QString target = folder + QStringLiteral("/img_%1.jpg").arg(i, 3, 10, QLatin1Char('0'));
        if (!QFileInfo::exists(target) && !QFile::copy(source, target))
            return {};
    }
    return folder;
}

struct Shell {
    QTemporaryDir settingsDir;
    std::unique_ptr<lf::Application> app;
    Shell() {
        lf::Settings::useTestingBackend(settingsDir.filePath(QStringLiteral("bench.ini")));
        app = std::make_unique<lf::Application>();
    }
};

enum class Stage { AnyImage, Screen };

// Result of openAndRender(): >= 0 ms, kSkip (no GPU / no test data) or kFailed.
constexpr double kSkip = -1;
constexpr double kFailed = 1e9;

// Time to the first rendered frame showing `file` in a new window. Stage::Screen
// waits for the screen-sized decode (Quality::Screen or better), not the preview.
double openAndRender(lf::Application& app, const QString& file, Stage stage = Stage::Screen) {
    const QString path = QDir::cleanPath(file);
    QElapsedTimer t;
    t.start();
    lf::ViewerWindow* w = app.openPaths({file});
    if (!w)
        return kFailed;
    bool reached = false;
    // Decoding is asynchronous, so no stage can have been shown before this connect.
    QObject::connect(w, &lf::ViewerWindow::imageShown, w, [&](const QString& p, lf::Quality q) {
        if (p == path && (stage == Stage::AnyImage || q != lf::Quality::Preview))
            reached = true;
    });
    if (!QTest::qWaitFor([&] { return reached && w->view()->image(); }, 10000))
        return kFailed;
    auto* canvas = w->view()->findChild<lf::ImageCanvas*>();
    if (!canvas || !canvas->isRhiAvailable())
        return kSkip;
    canvas->grabFramebuffer(); // render + present the frame (upper bound)
    return t.nsecsElapsed() / 1e6;
}

double coldOpen(const QString& name, Stage stage) {
    if (!gpuPlatform())
        return kSkip;
    const QString folder = benchFolder(QStringLiteral("p01"), 12);
    if (folder.isEmpty())
        return kSkip;
    QElapsedTimer t;
    t.start();
    Shell shell; // includes creating the shell (menu bar, services)
    const double open = openAndRender(*shell.app, folder + QLatin1Char('/') + name, stage);
    return open < 0 || open >= kFailed ? open : t.nsecsElapsed() / 1e6;
}

} // namespace

LF_BENCH(P_01_ColdOpenFirstImage24MP, 400.0) {
    return coldOpen(QStringLiteral("img_005.jpg"), Stage::Screen);
}

LF_BENCH(P_01_ColdOpenFirstPreview24MP, 400.0) {
    return coldOpen(QStringLiteral("img_009.jpg"), Stage::AnyImage);
}

// End-to-end cold start: spawns the built app with a 24 MP JPEG and
// LF_EXIT_AFTER_FIRST_FRAME=1. The app (hook in main/ViewerWindow) must print one
// line "LF_FIRST_FRAME_MS=<ms since process start>" to stdout as soon as the first
// frame with the screen-stage image is presented, then quit with exit code 0.
// Skipped while the app binary does not contain that hook.
LF_BENCH(P_01_ProcessLaunchToFirstFrame24MP, 400.0) {
#ifdef LF_APP_BINARY
    const QString binary = QStringLiteral(LF_APP_BINARY);
    QFile bin(binary);
    if (!bin.open(QIODevice::ReadOnly) || !bin.readAll().contains("LF_EXIT_AFTER_FIRST_FRAME"))
        return -1; // hook not implemented in the app yet
    const QString folder = benchFolder(QStringLiteral("p01launch"), 12);
    if (folder.isEmpty())
        return -1;
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LF_EXIT_AFTER_FIRST_FRAME"), QStringLiteral("1"));
    env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    QElapsedTimer t;
    t.start();
    proc.start(binary, {folder + QStringLiteral("/img_007.jpg")});
    if (!proc.waitForStarted(5000))
        return 1e9;
    QByteArray out;
    double appMs = -1;
    double wallMs = -1;
    while (t.elapsed() < 10000 && appMs < 0) {
        if (!proc.waitForReadyRead(100) && proc.state() == QProcess::NotRunning)
            break;
        out += proc.readAllStandardOutput();
        const int at = out.indexOf("LF_FIRST_FRAME_MS=");
        if (at >= 0 && out.indexOf('\n', at) > at) {
            wallMs = t.nsecsElapsed() / 1e6;
            appMs = out.mid(at + 18, out.indexOf('\n', at) - at - 18).trimmed().toDouble();
        }
    }
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(1000);
    }
    if (appMs < 0)
        return 1e9; // hook present but no frame reported: failure
    std::printf("      P-01 launch: app reports %.1f ms since process start, %.1f ms wall clock\n", appMs,
                wallMs);
    return wallMs;
#else
    return -1;
#endif
}

LF_BENCH(P_02_WarmOpen24MP, 150.0) {
    if (!gpuPlatform())
        return -1;
    const QString folder = benchFolder(QStringLiteral("p02"), 12);
    if (folder.isEmpty())
        return -1;
    Shell shell;
    // Warm up: app running, one window showing an image.
    if (const double warm = openAndRender(*shell.app, folder + QStringLiteral("/img_000.jpg"));
        warm < 0 || warm >= kFailed)
        return warm;
    QTest::qWait(300);
    // A file far from the prefetch window of the first one (not decoded yet).
    return openAndRender(*shell.app, folder + QStringLiteral("/img_008.jpg"));
}

LF_BENCH(P_06_HoldArrowKey24MP, 1000.0 / 15.0) {
    if (!gpuPlatform())
        return -1;
    const QString folder = benchFolder(QStringLiteral("p06"), kFolderImages);
    if (folder.isEmpty())
        return -1;
    Shell shell;
    const QString first = folder + QStringLiteral("/img_000.jpg");
    if (const double warm = openAndRender(*shell.app, first); warm < 0 || warm >= kFailed)
        return warm;
    lf::ViewerWindow* w = shell.app->frontWindow();
    if (!QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 10000) ||
        w->collection()->count() < kFolderImages)
        return -1;
    w->collection()->setCurrentPath(QDir::cleanPath(first));
    QTest::qWait(300); // prefetch of the first neighbours, like a user looking at the first image
    const int before = w->displayedImageCount();
    QElapsedTimer t;
    t.start();
    // macOS key repeat at the fastest system setting: one event every ~33 ms.
    while (t.elapsed() < 2000) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, QString(), true);
        QCoreApplication::sendEvent(w->view(), &press);
        QTest::qWait(33);
    }
    const int shown = w->displayedImageCount() - before;
    std::printf("      P-06: %d images displayed in %.0f ms (%.1f/s), position %d\n", shown,
                double(t.elapsed()), shown * 1000.0 / double(t.elapsed()), w->collection()->currentIndex());
    return shown > 0 ? double(t.elapsed()) / shown : 1e9;
}
