// SPDX-License-Identifier: GPL-3.0-or-later
// App-level benchmarks (SPEC 7): the whole path Application::openPaths -> window ->
// Collection -> ImageProvider -> ImageView -> rendered frame, in-process.
//
// P-01 "Kaltstart bis erstes Bild": measured from creating the shell and opening a
//      24 MP JPEG that was never decoded in this process until the first frame with
//      the screen-sized image (Quality::Screen, not the embedded preview) is
//      rendered. Process launch + dyld (~100 ms on the reference Mac) are not
//      included; P_01_ProcessLaunchToFirstFrame24MP (median of 5 real launches) covers
//      them when the app supports LF_EXIT_AFTER_FIRST_FRAME (see there).
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

#include <algorithm>
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

// End-to-end cold start (SPEC P-01, clarified): spawns the built app with a 24 MP JPEG and
// LF_EXIT_AFTER_FIRST_FRAME=1. The app (hook in main.cpp) prints
//   stderr: "LF_MAIN_MS=<ms since process start>" when main() is entered (dyld, static init)
//   stdout: "LF_FIRST_FRAME_MS=<ms since process start>" once the first frame with the
//           screen-stage image is presented, then exits with code 0.
// Measured value = LF_FIRST_FRAME_MS (kernel process start -> presented frame).
// Protocol: one warm-up launch (discarded: it pays the one-time code signature /
// Gatekeeper check of a freshly built binary and warms the file cache), then the median of
// 5 launches with warm system caches; min/median/max are printed. The warm-up launch is
// reported separately as P_01_FirstLaunchAfterBuild (limit 1 s).
// Assumptions (main.cpp belongs to the app, the benchmark only reads its output):
//   * A launch with a file argument never restores the previous session (F-WIN-06: an
//     open request has priority), and the hook additionally keeps the session untouched
//     while LF_EXIT_AFTER_FIRST_FRAME=1. The benchmark therefore passes no option to turn
//     session restore off; it measures the normal "double-click on a file" start path.
//   * The app runs with the user's normal settings (window size, zoom mode etc.).
// Skipped while the app binary does not contain the hook.
namespace {

struct Launch {
    double frameMs = -1; // LF_FIRST_FRAME_MS (process start -> frame)
    double mainMs = -1;  // LF_MAIN_MS (process start -> main)
    double wallMs = -1;  // QProcess::start -> line received (includes fork/exec + pipe)
};

double parseValue(const QByteArray& out, const char* key) {
    const qsizetype at = out.indexOf(key);
    if (at < 0)
        return -1;
    const qsizetype from = at + qsizetype(qstrlen(key));
    const qsizetype nl = out.indexOf('\n', from);
    if (nl < 0)
        return -1;
    bool ok = false;
    const double v = out.mid(from, nl - from).trimmed().toDouble(&ok);
    return ok ? v : -1;
}

// One launch; frameMs < 0 if the app did not report a frame within 10 s.
Launch launchOnce(const QString& binary, const QString& file) {
    Launch r;
    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LF_EXIT_AFTER_FIRST_FRAME"), QStringLiteral("1"));
    env.remove(QStringLiteral("QT_QPA_PLATFORM"));
    proc.setProcessEnvironment(env);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    QElapsedTimer t;
    t.start();
    proc.start(binary, {file});
    if (!proc.waitForStarted(5000))
        return r;
    QByteArray out;
    while (t.elapsed() < 10000 && r.frameMs < 0) {
        if (!proc.waitForReadyRead(100) && proc.state() == QProcess::NotRunning)
            break;
        out += proc.readAllStandardOutput();
        if (const double v = parseValue(out, "LF_FIRST_FRAME_MS="); v >= 0) {
            r.wallMs = t.nsecsElapsed() / 1e6;
            r.frameMs = v;
        }
    }
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(1000);
    }
    r.mainMs = parseValue(proc.readAllStandardError(), "LF_MAIN_MS=");
    return r;
}

double median(QVector<double> v) {
    std::sort(v.begin(), v.end());
    const qsizetype n = v.size();
    if (n == 0)
        return -1;
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

constexpr int kLaunchRuns = 5;

struct LaunchSeries {
    bool skipped = true;
    bool failed = false;
    bool firstAfterBuild = false; // no earlier launch of this build was recorded
    Launch warmup;
    QVector<Launch> runs;
};

// Warm-up + 5 launches, run once per benchmark process and shared by both launch
// benchmarks (whatever their registration order).
const LaunchSeries& launchSeries() {
    static const LaunchSeries series = [] {
        LaunchSeries s;
#ifdef LF_APP_BINARY
        const QString binary = QStringLiteral(LF_APP_BINARY);
        QFile bin(binary);
        if (!bin.open(QIODevice::ReadOnly) || !bin.readAll().contains("LF_EXIT_AFTER_FIRST_FRAME"))
            return s; // hook not implemented in the app yet
        const QString folder = benchFolder(QStringLiteral("p01launch"), 12);
        if (folder.isEmpty())
            return s;
        s.skipped = false;
        // "After build": the stamp remembers the last launched build (mtime, size, path).
        const QFileInfo bi(binary);
        const QByteArray build = QByteArray::number(bi.lastModified().toMSecsSinceEpoch()) + ' ' +
                                 QByteArray::number(bi.size()) + ' ' + binary.toUtf8();
        QFile stamp(QDir::tempPath() + QStringLiteral("/lumaflip-bench-24mp-v1/p01launch.stamp"));
        s.firstAfterBuild = !stamp.open(QIODevice::ReadOnly) || stamp.readAll() != build;
        stamp.close();
        const QString file = folder + QStringLiteral("/img_007.jpg");
        s.warmup = launchOnce(binary, file);
        if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate))
            stamp.write(build);
        for (int i = 0; i < kLaunchRuns; ++i) {
            const Launch l = launchOnce(binary, file);
            if (l.frameMs < 0) {
                s.failed = true; // hook present but no frame reported
                break;
            }
            s.runs.append(l);
        }
#endif
        return s;
    }();
    return series;
}

} // namespace

// Informational upper bound for the very first launch (SPEC P-01: < 1 s after
// installation/update). Only a real "first launch after build" when the line says so.
LF_BENCH(P_01_FirstLaunchAfterBuild, 1000.0) {
    const LaunchSeries& s = launchSeries();
    if (s.skipped)
        return -1;
    if (s.warmup.frameMs < 0)
        return 1e9;
    std::printf("      P-01 first launch%s: %.1f ms (main entered after %.1f ms, wall clock %.1f ms)\n",
                s.firstAfterBuild ? " after build" : " (this build was launched before)", s.warmup.frameMs,
                s.warmup.mainMs, s.warmup.wallMs);
    return s.warmup.frameMs;
}

LF_BENCH(P_01_ProcessLaunchToFirstFrame24MP, 400.0) {
    const LaunchSeries& s = launchSeries();
    if (s.skipped)
        return -1;
    if (s.failed || s.runs.size() != kLaunchRuns)
        return 1e9;
    QVector<double> frame, mainMs, wall;
    for (const Launch& l : s.runs) {
        frame.append(l.frameMs);
        mainMs.append(l.mainMs);
        wall.append(l.wallMs);
    }
    const double med = median(frame);
    std::printf("      P-01 launch, %d runs after 1 warm-up: min %.1f / median %.1f / max %.1f ms "
                "(main entered after %.1f ms, wall clock %.1f ms; medians)\n",
                kLaunchRuns, *std::min_element(frame.cbegin(), frame.cend()), med,
                *std::max_element(frame.cbegin(), frame.cend()), median(mainMs), median(wall));
    return med;
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
