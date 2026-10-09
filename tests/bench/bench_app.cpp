// SPDX-License-Identifier: GPL-3.0-or-later
// App-level benchmarks (SPEC 7): the whole path Application::openPaths -> window ->
// Collection -> ImageProvider -> ImageView -> rendered frame, in-process.
//
// P-01 "Kaltstart bis erstes Bild": measured from creating the shell and opening a
//      24 MP JPEG that was never decoded in this process until the first frame with
//      the image is rendered. Process launch + dyld (~100 ms on the reference Mac)
//      are not included; see docs/TESTING-M1.md for the end-to-end check.
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
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest/QTest>

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

bool waitShown(lf::ViewerWindow* w, const QString& path, int ms = 10000) {
    return QTest::qWaitFor([&] { return w->shownPath() == path && w->view()->image(); }, ms);
}

// Time to the first rendered frame showing `file` in a new window.
double openAndRender(lf::Application& app, const QString& file) {
    QElapsedTimer t;
    t.start();
    lf::ViewerWindow* w = app.openPaths({file});
    if (!w || !waitShown(w, QDir::cleanPath(file)))
        return -1;
    auto* canvas = w->view()->findChild<lf::ImageCanvas*>();
    if (!canvas || !canvas->isRhiAvailable())
        return -1;
    canvas->grabFramebuffer(); // render + present the frame (upper bound)
    return t.nsecsElapsed() / 1e6;
}

} // namespace

LF_BENCH(P_01_ColdOpenFirstImage24MP, 400.0) {
    if (!gpuPlatform())
        return -1;
    const QString folder = benchFolder(QStringLiteral("p01"), 12);
    if (folder.isEmpty())
        return -1;
    QElapsedTimer t;
    t.start();
    Shell shell; // includes creating the shell (menu bar, services)
    const double open = openAndRender(*shell.app, folder + QStringLiteral("/img_005.jpg"));
    return open < 0 ? -1 : t.nsecsElapsed() / 1e6;
}

LF_BENCH(P_02_WarmOpen24MP, 150.0) {
    if (!gpuPlatform())
        return -1;
    const QString folder = benchFolder(QStringLiteral("p02"), 12);
    if (folder.isEmpty())
        return -1;
    Shell shell;
    // Warm up: app running, one window showing an image.
    if (openAndRender(*shell.app, folder + QStringLiteral("/img_000.jpg")) < 0)
        return -1;
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
    if (openAndRender(*shell.app, first) < 0)
        return -1;
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
