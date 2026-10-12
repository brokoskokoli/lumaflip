// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShotDriver.h"

#include "NativeCapture.h"

#include "app/Application.h"
#include "app/CompareView.h"
#include "app/CullingController.h"
#include "app/FolderMapPanel.h"
#include "app/GridView.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/InfoPanel.h"
#include "app/OverviewController.h"
#include "app/PreferencesDialog.h"
#include "app/RecentItems.h"
#include "app/TitleStrip.h"
#include "app/Translations.h"
#include "app/ViewerWindow.h"
#include "app/WindowExtras.h"
#include "app/WindowPanels.h"
#include "core/FileOps.h"
#include "core/Rating.h"
#include "core/RatingStore.h"
#include "core/Settings.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QStyleHints>
#include <QTest>
#include <QThreadPool>

#include <cstdio>

namespace lf::shots {

namespace {

void log(const QString& text) {
    std::fprintf(stderr, "lf_screenshots: %s\n", qPrintable(text));
    std::fflush(stderr);
}

// Photos of the shots (file names of tools/screenshots/sample-photos.json).
const QString kHero = QStringLiteral("Aegina harbour at sunset.jpg");
const QString kInfo = QStringLiteral("Athens - Temple of Zeus.jpg");
const QString kCulling = QStringLiteral("Athens - Evzones.jpg");
const QString kMap = QStringLiteral("Athens - Holy Apostles.jpg");
const QStringList kCompare = {QStringLiteral("Acropolis - Erechtheion.jpg"),
                              QStringLiteral("Acropolis - evening moon.jpg"),
                              QStringLiteral("Athens - Temple of Zeus.jpg")};
constexpr int kDemoImages = 6; // the hero and the next five in name order

// Ratings already in the folder (XMP sidecars), so the grid, filmstrip and captions show some.
struct PresetRating {
    const char* name;
    int stars;
    PickFlag flag;
};
constexpr PresetRating kRatings[] = {
    {"Aegina harbour at sunset.jpg", 5, PickFlag::Pick}, {"Athens - Temple of Zeus.jpg", 4, PickFlag::None},
    {"Acropolis - evening moon.jpg", 4, PickFlag::Pick}, {"Crete - Komos bay.jpg", 3, PickFlag::None},
    {"Euboea - shipwreck.jpg", 5, PickFlag::None},       {"Rhodes - church at night.jpg", 4, PickFlag::None},
    {"Euboea - fishing net.jpg", 2, PickFlag::None},     {"Acropolis - Erechtheion.jpg", 3, PickFlag::None},
};

// Cheap equality of two grabs: the same size and the same pixels at 1/4 size.
bool sameImage(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull())
        return false;
    const QSize small = a.size() / 4;
    return a.scaled(small, Qt::IgnoreAspectRatio, Qt::FastTransformation) ==
           b.scaled(small, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

} // namespace

ShotDriver::ShotDriver(Options options) : m_o(std::move(options)) {}

ShotDriver::~ShotDriver() {
    closeWindows();
    m_app.reset();
    QThreadPool::globalInstance()->waitForDone(10000);
    if (!m_ownedFolder.isEmpty())
        QDir(m_ownedFolder).removeRecursively();
}

QString ShotDriver::path(const QString& name) const {
    return QDir::cleanPath(m_folder + QLatin1Char('/') + name);
}

bool ShotDriver::prepareFolder() {
    if (!m_work.isValid()) {
        log(QStringLiteral("no temporary folder"));
        return false;
    }
    // The folder name appears in the folder map header and the info panel.
    // The folder's path shows in the info panel: LF_SHOT_FOLDER names a tidy one (CI:
    // ~/Pictures/Greece); it must not exist yet. Default: a temporary folder.
    m_folder = qEnvironmentVariable("LF_SHOT_FOLDER");
    if (!m_folder.isEmpty() && QFileInfo::exists(m_folder)) {
        log(QStringLiteral("LF_SHOT_FOLDER %1 exists already").arg(m_folder));
        return false;
    }
    if (m_folder.isEmpty())
        m_folder = QDir(m_work.path()).filePath(QStringLiteral("Greece"));
    else
        m_ownedFolder = m_folder;
    QDir().mkpath(m_folder);
    const QFileInfoList photos =
        QDir(m_o.photos).entryInfoList({QStringLiteral("*.jpg")}, QDir::Files, QDir::Name | QDir::IgnoreCase);
    if (photos.size() < kDemoImages) {
        log(QStringLiteral("too few photos in %1 (run tools/fetch_sample_photos.py)").arg(m_o.photos));
        return false;
    }
    for (const QFileInfo& fi : photos) {
        const QString dest = path(fi.fileName());
        if (!QFile::copy(fi.absoluteFilePath(), dest)) {
            log(QStringLiteral("cannot copy %1").arg(fi.fileName()));
            return false;
        }
        QFile f(dest);
        if (f.open(QIODevice::ReadWrite)) {
            f.setFileTime(fi.lastModified(), QFileDevice::FileModificationTime);
            f.close();
        }
    }
    for (const QString& needed : {kHero, kInfo, kCulling, kMap}) {
        if (!QFileInfo::exists(path(needed))) {
            log(QStringLiteral("sample photo missing: %1").arg(needed));
            return false;
        }
    }
    return true;
}

void ShotDriver::prepareRatings() {
    int pending = 0;
    for (const PresetRating& r : kRatings) {
        const QString file = path(QString::fromUtf8(r.name));
        if (!QFileInfo::exists(file))
            continue;
        Rating rating;
        rating.stars = r.stars;
        rating.flag = r.flag;
        ++pending;
        RatingStore::instance().setRating(FileOps::instance(), file, rating,
                                          [&pending](bool, const QString&, const QString&) { --pending; });
    }
    if (!QTest::qWaitFor([&pending] { return pending == 0; }, 10000))
        log(QStringLiteral("ratings not written in time"));
}

int ShotDriver::run() {
    if (!prepareFolder())
        return 1;
    m_dpr = qApp->devicePixelRatio();
    if (QScreen* s = QGuiApplication::primaryScreen())
        log(QStringLiteral("screen %1x%2 logical, device pixel ratio %3")
                .arg(s->geometry().width())
                .arg(s->geometry().height())
                .arg(s->devicePixelRatio()));
    QDir().mkpath(m_o.out);

    AppServices services;
    services.quitApp = [] {}; // closing the last window must not end the tool
    m_app = std::make_unique<Application>(services);
    prepareRatings();

    int failed = 0;
    for (const QString& shot : m_o.shots) {
        log(QStringLiteral("shot %1 (%2, %3)").arg(shot, m_o.language, appearanceId(m_o.appearance)));
        bool ok = false;
        if (shot == QLatin1String("viewer"))
            ok = shotViewer();
        else if (shot == QLatin1String("info"))
            ok = shotInfo();
        else if (shot == QLatin1String("culling"))
            ok = shotCulling();
        else if (shot == QLatin1String("grid"))
            ok = shotGrid();
        else if (shot == QLatin1String("compare"))
            ok = shotCompare();
        else if (shot == QLatin1String("map"))
            ok = shotMap();
        else if (shot == QLatin1String("preferences"))
            ok = shotPreferences();
        else if (shot == QLatin1String("demo"))
            ok = shotDemo();
        closeWindows();
        if (!ok) {
            log(QStringLiteral("shot %1 FAILED").arg(shot));
            ++failed;
        }
    }
    return failed == 0 ? 0 : 1;
}

// ---- windows ----

bool ShotDriver::ensureSize(QWidget* window, QSize size) {
    // macOS fits a window that is larger than the screen into it when it is shown; a resize
    // after that is kept (the canvas renders off screen parts all the same).
    for (int attempt = 0; attempt < 5 && window->size() != size; ++attempt) {
        window->resize(size);
        QTest::qWait(150);
    }
    if (window->size() != size) {
        log(QStringLiteral("window is %1x%2 instead of %3x%4")
                .arg(window->width())
                .arg(window->height())
                .arg(size.width())
                .arg(size.height()));
        return false;
    }
    return true;
}

ViewerWindow* ShotDriver::openImage(const QString& name) {
    const QString file = path(name);
    ViewerWindow* w = m_app->openPaths({file});
    if (!w) {
        log(QStringLiteral("no window for %1").arg(name));
        return nullptr;
    }
    QObject::connect(w, &ViewerWindow::imageShown, w, [this](const QString& p, Quality q) {
        if (!m_quality.contains(p) || int(q) > int(m_quality.value(p)))
            m_quality.insert(p, q);
    });
    if (!QTest::qWaitForWindowExposed(w, 10000)) {
        log(QStringLiteral("window not exposed"));
        return nullptr;
    }
    w->move(0, 0);
    if (!ensureSize(w, m_o.windowSize))
        return nullptr;
    if (!QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 15000) ||
        !QTest::qWaitFor([w, file] { return w->shownPath() == file; }, 15000)) {
        log(QStringLiteral("%1 not shown").arg(name));
        return nullptr;
    }
    if (!waitImageReady(w, file))
        return nullptr;
    return w;
}

bool ShotDriver::waitImageReady(ViewerWindow* w, const QString& file) {
    auto* canvas = w->view()->findChild<ImageCanvas*>();
    const bool ok = QTest::qWaitFor(
        [&] {
            // The stage of the image on the canvas (a cached image is shown before the
            // imageShown signal could be connected) or the best stage reported.
            const DecodedImagePtr current = canvas ? canvas->current() : DecodedImagePtr();
            const bool good = (current && current->quality != Quality::Preview) ||
                              m_quality.value(file, Quality::Preview) != Quality::Preview;
            return w->shownPath() == file && good && (!canvas || canvas->isIdle());
        },
        20000);
    if (!ok)
        log(QStringLiteral("%1 not ready (stage %2)")
                .arg(file)
                .arg(int(m_quality.value(file, Quality::Preview))));
    return ok;
}

void ShotDriver::closeWindows() {
    if (auto* prefs = PreferencesDialog::existingInstance())
        prefs->close();
    if (!m_app)
        return;
    for (ViewerWindow* w : m_app->windows())
        w->close();
    QTest::qWait(300);
}

// ---- capture ----

QImage ShotDriver::grabWindow(QWidget* window) {
    QImage image = window->grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // The GPU canvases are part of the grab (QRhiWidget renders them into it). Should a
    // canvas come out empty, its framebuffer is drawn in and the widgets above it again.
    const double dpr = image.devicePixelRatio();
    for (ImageCanvas* canvas : window->findChildren<ImageCanvas*>()) {
        if (!canvas->isVisible() || !canvas->current())
            continue;
        const QRect r(canvas->mapTo(window, QPoint(0, 0)), canvas->size());
        const QRect device(QPoint(int(r.x() * dpr), int(r.y() * dpr)), r.size() * dpr);
        if (!isUniform(image, device))
            continue;
        log(QStringLiteral("canvas empty in the window grab: drawing its framebuffer"));
        QPainter p(&image);
        p.drawImage(QRectF(r), canvas->grabFramebuffer());
        QWidget* host = canvas->parentWidget();
        for (QObject* o : host->children()) {
            auto* child = qobject_cast<QWidget*>(o);
            if (child && child != canvas && child->isVisible())
                child->render(&p, child->mapTo(window, QPoint(0, 0)), QRegion(), QWidget::DrawChildren);
        }
    }
    return image;
}

QImage ShotDriver::grabStable(QWidget* window, const std::function<void()>& prepare, int timeoutMs) {
    QImage last;
    QElapsedTimer timer;
    timer.start();
    while (true) {
        if (prepare)
            prepare();
        QTest::qWait(500);
        QImage image = grabWindow(window);
        if (sameImage(image, last))
            return image;
        if (timer.elapsed() > timeoutMs) {
            log(QStringLiteral("window did not settle within %1 ms - using the last grab").arg(timeoutMs));
            return image;
        }
        last = image;
    }
}

void ShotDriver::revealTitle(ViewerWindow* window) {
    window->hideMessage();
    if (TitleStripController* ts = window->extras()->titleStrip())
        ts->reveal();
}

void ShotDriver::hideTitle(ViewerWindow* window) {
    window->hideMessage();
    if (TitleStripController* ts = window->extras()->titleStrip())
        ts->strip()->fadeTo(false, 0);
}

FrameStyle ShotDriver::mainWindowStyle(ViewerWindow* window) const {
    FrameStyle style;
    style.dpr = m_dpr;
    style.border = m_o.appearance == Appearance::Dark ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 40);
    if (TitleStripController* ts = window->extras()->titleStrip();
        ts && ts->active() && ts->strip()->isVisible()) {
        style.trafficLights = true;
        style.trafficLightTitleHeight = ts->strip()->height();
    }
    return style;
}

bool ShotDriver::save(const QImage& content, const FrameStyle& style, const QString& file) {
    QImage framed = frameWindow(content, style);
    const QString target = QDir(m_o.out).filePath(file);
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!framed.save(target)) {
        log(QStringLiteral("cannot write %1").arg(target));
        return false;
    }
    log(QStringLiteral("wrote %1 (%2x%3)").arg(target).arg(framed.width()).arg(framed.height()));
    return true;
}

// ---- shots ----

bool ShotDriver::shotViewer() {
    ViewerWindow* w = openImage(kHero);
    if (!w)
        return false;
    const QImage image = grabStable(w, [w] { revealTitle(w); });
    return save(image, mainWindowStyle(w),
                shotFileName(QStringLiteral("viewer"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotInfo() {
    ViewerWindow* w = openImage(kInfo);
    if (!w)
        return false;
    w->panels()->setInfoPanelEnabled(true);
    w->panels()->setFilmstripEnabled(true);
    QTest::qWait(300);
    if (auto* dock = w->findChild<QDockWidget*>(QStringLiteral("lfInfoDock")))
        w->resizeDocks({dock}, {m_o.windowSize.width() * 36 / 100}, Qt::Horizontal);
    InfoPanel* info = nullptr;
    const bool loaded = QTest::qWaitFor(
        [&] {
            info = w->panels()->infoPanel();
            return info && info->path() == path(kInfo) && !info->isLoading() && !info->isHistogramPending();
        },
        20000);
    if (!loaded) {
        log(QStringLiteral("info panel not loaded"));
        return false;
    }
    if (info->tagsSupported() && info->tags().isEmpty()) {
        info->addTag(QStringLiteral("Greece"));
        if (!QTest::qWaitFor([info] { return !info->isTagWritePending(); }, 10000))
            log(QStringLiteral("Finder tag not written in time"));
    }
    const QImage image = grabStable(w, [w] { revealTitle(w); });
    return save(image, mainWindowStyle(w),
                shotFileName(QStringLiteral("info"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotCulling() {
    ViewerWindow* w = openImage(kCulling);
    if (!w)
        return false;
    w->culling()->setActive(true);
    w->perform(Action::Rate4);
    w->perform(Action::RatePick);
    QTest::qWait(500);
    if (!w->culling()->overlay() || !w->culling()->overlay()->isVisible())
        log(QStringLiteral("culling overlay not visible"));
    // The culling badge sits at the top left: no title strip over it, no toast.
    const QImage image = grabStable(w, [w] { hideTitle(w); });
    return save(image, mainWindowStyle(w),
                shotFileName(QStringLiteral("culling"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotGrid() {
    ViewerWindow* w = openImage(kHero);
    if (!w)
        return false;
    w->panels()->setFilmstripEnabled(false);
    w->perform(Action::ToggleGrid);
    OverviewController* o = w->extras()->overview();
    GridView* g = o->grid();
    if (!o->isGridShown() || !g) {
        log(QStringLiteral("grid not shown"));
        return false;
    }
    // A selection of three: click, then ⌘-click (Qt's Control is the Command key on macOS).
    g->clickRow(2, Qt::NoModifier);
    g->clickRow(5, Qt::ControlModifier);
    g->clickRow(9, Qt::ControlModifier);
    g->setThumbSize(200);
    // The grid's header is at the top: no title strip over it (it fades out in the app too).
    const QImage image = grabStable(w, [w] { hideTitle(w); });
    return save(image, mainWindowStyle(w),
                shotFileName(QStringLiteral("grid"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotCompare() {
    ViewerWindow* w = openImage(kCompare.first());
    if (!w)
        return false;
    QStringList paths;
    for (const QString& name : kCompare)
        paths << path(name);
    OverviewController* o = w->extras()->overview();
    if (!o->compare(paths, 0) || !o->compareView()) {
        log(QStringLiteral("compare mode not shown"));
        return false;
    }
    CompareView* cv = o->compareView();
    const bool loaded = QTest::qWaitFor(
        [cv] {
            for (int i = 0; i < cv->paneCount(); ++i)
                if (!cv->paneView(i)->image())
                    return false;
            return true;
        },
        20000);
    if (!loaded) {
        log(QStringLiteral("compare panes not loaded"));
        return false;
    }
    QTest::qWait(1500); // panes laid out at the window size
    // Synchronised zoom: zoom into the first pane, the others follow.
    ImageView* first = cv->paneView(0);
    const double fit = first->zoomFactor();
    first->setZoomFactor(fit * 1.6, QPointF(first->width() * 0.5, first->height() * 0.45));
    QTest::qWait(500);
    QStringList factors;
    for (int i = 0; i < cv->paneCount(); ++i)
        factors << QString::number(cv->paneView(i)->zoomFactor(), 'f', 3);
    log(QStringLiteral("compare zoom: fit %1, panes %2").arg(fit).arg(factors.join(QLatin1Char(' '))));
    const QImage image = grabStable(w, [w] { revealTitle(w); });
    return save(image, mainWindowStyle(w),
                shotFileName(QStringLiteral("compare"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotMap() {
    ViewerWindow* w = openImage(kMap);
    if (!w)
        return false;
    w->extras()->setFolderMapShown(true);
    if (auto* dock = w->findChild<QDockWidget*>(QStringLiteral("lfFolderMapDock")))
        w->resizeDocks({dock}, {m_o.windowSize.width() * 45 / 100}, Qt::Horizontal);
    auto* host = w->findChild<QWidget*>(QStringLiteral("lfFolderMapHost"));
    if (!host || !host->isVisible()) {
        log(QStringLiteral("folder map not shown"));
        return false;
    }
    // MapKit loads its tiles from the network: give it time, then take the map from the
    // window server's image (a widget grab cannot see the native map view).
    // The window is larger than the runner's screen: move it so the map is on screen.
    const QPoint hostInWindow = host->mapTo(w, QPoint(0, 0));
    w->move(-hostInWindow.x() + 8, -hostInWindow.y() + 40);
    QTest::qWait(12000);
    QImage image = grabStable(w, [w] { revealTitle(w); });
    QImage native = captureNativeWindow(w);
    if (!native.isNull())
        native.save(QDir(m_o.out).filePath(QStringLiteral("debug-native-%1.png").arg(m_o.language)));
    if (native.isNull()) {
        log(QStringLiteral("native window capture failed (Screen Recording permission?) - no map shot"));
        return false;
    }
    const double scale = double(native.width()) / w->width();
    const QRect logical(host->mapTo(w, QPoint(0, 0)), host->size());
    const QRect nativeRect(int(logical.x() * scale), int(logical.y() * scale), int(logical.width() * scale),
                           int(logical.height() * scale));
    const QImage map = native.copy(nativeRect);
    if (map.isNull() || isUniform(map, map.rect())) {
        log(QStringLiteral("the map is empty in the native capture - no map shot"));
        return false;
    }
    {
        QPainter p(&image);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(logical), map);
    }
    return save(image, mainWindowStyle(w), shotFileName(QStringLiteral("map"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotPreferences() {
    // A viewer window behind it, like in the app (the dialog belongs to no window).
    PreferencesDialog* dlg = PreferencesDialog::showPreferences(nullptr);
    if (!dlg || !QTest::qWaitForWindowExposed(dlg, 10000)) {
        log(QStringLiteral("preferences not shown"));
        return false;
    }
    dlg->showSection(PreferencesDialog::Input);
    dlg->move(0, 0);
    ensureSize(dlg, QSize(std::max(dlg->width(), 820), 640));
    const QImage image = grabStable(dlg);
    FrameStyle style;
    style.dpr = m_dpr;
    style.titleBarHeight = 28;
    style.title = dlg->windowTitle();
    style.titleBarColor = dlg->palette().color(QPalette::Window);
    style.titleTextColor = dlg->palette().color(QPalette::WindowText);
    style.trafficLights = true;
    style.border = m_o.appearance == Appearance::Dark ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 40);
    return save(image, style, shotFileName(QStringLiteral("preferences"), m_o.language, m_o.appearance));
}

bool ShotDriver::shotDemo() {
    ViewerWindow* w = openImage(kHero);
    if (!w)
        return false;
    w->panels()->setFilmstripEnabled(true);
    const QString dir = shotFileName(QStringLiteral("demo"), m_o.language, m_o.appearance);
    const QList<DemoFrame> frames = demoTimeline(kDemoImages, 2200, 1600, 260);
    // Where the key caps go: bottom right of the image view.
    auto drawKeys = [&](QImage& image, bool down) {
        const QRect view(w->view()->mapTo(w, QPoint(0, 0)), w->view()->size());
        const double unit = 40.0; // key size, logical px
        QPainter p(&image);       // logical coordinates (the image carries the device pixel ratio)
        const QRectF right(view.right() - unit - 22, view.bottom() - unit - 22, unit, unit);
        const QRectF left = right.translated(-unit - 10, 0);
        drawKeyCap(p, left, QStringLiteral("←"), false);
        drawKeyCap(p, right, QStringLiteral("→"), down);
    };
    int index = -1;
    QImage shown;
    for (qsizetype i = 0; i < frames.size(); ++i) {
        const DemoFrame& f = frames[i];
        if (f.index != index) {
            if (index >= 0) {
                w->perform(Action::NextImage);
                const QString file = w->currentPath();
                if (!QTest::qWaitFor([w, file] { return w->shownPath() == file; }, 10000) ||
                    !waitImageReady(w, file))
                    return false;
            }
            index = f.index;
            shown = grabStable(w, [w] { revealTitle(w); });
        }
        QImage image = shown;
        image.setDevicePixelRatio(m_dpr);
        drawKeys(image, f.keyDown);
        if (!save(image, mainWindowStyle(w),
                  dir + QStringLiteral("frame-%1.png").arg(i, 3, 10, QLatin1Char('0'))))
            return false;
    }
    QFile list(QDir(m_o.out).filePath(dir + QStringLiteral("frames.txt")));
    if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    list.write(demoFrameList(frames).toUtf8());
    return true;
}

} // namespace lf::shots
