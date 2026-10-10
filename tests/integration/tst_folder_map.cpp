// SPDX-License-Identifier: GPL-3.0-or-later
// F-INFO-08 folder map in real windows (with a fake map view - no MapKit, no network):
// opening from the menu and from the info panel, privacy (nothing before opening, closing
// stops the map), counts in the header, marker click jumps to the image, the current marker
// follows ←/→. Plus the M3 formats in a window: SVG via resvg, XCF and PSB (F-FMT-01).
// Files only in temporary folders and the generated corpus.

#include "app/Application.h"
#include "app/FolderMapPanel.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/InfoGpsBox.h"
#include "app/InfoPanel.h"
#include "app/MainMenu.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "app/WindowExtras.h"
#include "app/WindowPanels.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/FolderMap.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QToolButton>
#include <QtTest>

#include <cmath>
#include <memory>

using namespace lf;

namespace {

class NullTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString&, QString* error) override {
        if (error)
            *error = QStringLiteral("no trash in this test");
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

// Records what the folder map asks of the view.
class FakeMapView final : public MapViewBackend {
public:
    static inline FakeMapView* last = nullptr;
    static inline int created = 0;
    FakeMapView() {
        last = this;
        ++created;
    }
    ~FakeMapView() override {
        if (last == this)
            last = nullptr;
    }
    void start() override {
        started = true;
        ++starts;
    }
    void stop() override {
        started = false;
        ++stops;
        markers.clear();
    }
    bool isStarted() const override { return started; }
    void setMarkers(const QVector<MapMarker>& m) override { markers = m; }
    void addMarkers(const QVector<MapMarker>& m) override { markers += m; }
    void setCurrent(const QString& path) override { current = path; }
    void showRegion(const GeoRegion& r, bool) override {
        visible = r;
        ++regionChanges;
    }
    std::optional<GeoRegion> visibleRegion() const override { return visible; }
    void setThumbnail(const QString& path, const QImage& img) override {
        thumbPath = path;
        thumb = img;
    }

    bool started = false;
    int starts = 0, stops = 0, regionChanges = 0;
    QVector<MapMarker> markers;
    QString current, thumbPath;
    QImage thumb;
    std::optional<GeoRegion> visible;
};

void be16(QByteArray& a, quint16 v) {
    a.append(char(v >> 8)).append(char(v & 0xFF));
}
void be32(QByteArray& a, quint32 v) {
    for (int s = 24; s >= 0; s -= 8)
        a.append(char((v >> s) & 0xFF));
}

// JPEG with an EXIF GPS block (see tests/unit/test_folder_map.cpp).
QByteArray gpsJpeg(std::optional<GeoPoint> p, QColor color) {
    QImage img(320, 240, QImage::Format_RGB32);
    img.fill(color);
    QByteArray jpg;
    QBuffer buf(&jpg);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 90);
    if (!p)
        return jpg;
    auto rationals = [](QByteArray& t, double v) {
        v = std::abs(v);
        const int d = int(v), m = int((v - d) * 60);
        const double s = ((v - d) * 60 - m) * 60;
        be32(t, quint32(d)), be32(t, 1), be32(t, quint32(m)), be32(t, 1);
        be32(t, quint32(std::llround(s * 10000))), be32(t, 10000);
    };
    QByteArray t("MM\0*", 4);
    be32(t, 8);
    be16(t, 1);
    be16(t, 0x8825), be16(t, 4), be32(t, 1), be32(t, 26);
    be32(t, 0);
    be16(t, 5);
    be16(t, 0), be16(t, 1), be32(t, 4), t.append(QByteArray("\x02\x02\x00\x00", 4));
    be16(t, 1), be16(t, 2), be32(t, 2), t.append(QByteArray(p->latitude < 0 ? "S\0\0\0" : "N\0\0\0", 4));
    be16(t, 2), be16(t, 5), be32(t, 3), be32(t, 92);
    be16(t, 3), be16(t, 2), be32(t, 2), t.append(QByteArray(p->longitude < 0 ? "W\0\0\0" : "E\0\0\0", 4));
    be16(t, 4), be16(t, 5), be32(t, 3), be32(t, 116);
    be32(t, 0);
    rationals(t, p->latitude);
    rationals(t, p->longitude);
    QByteArray app1("\xFF\xE1", 2);
    be16(app1, quint16(2 + 6 + t.size()));
    app1.append(QByteArray("Exif\0\0", 6)).append(t);
    return jpg.left(2) + app1 + jpg.mid(2);
}

} // namespace

class TstFolderMap : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NullTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NullShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    static QString corpus(const QString& name) { return QStringLiteral(LF_CORPUS_DIR "/generated/") + name; }
    bool write(const QString& p, const QByteArray& data) {
        QFile f(p);
        return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
    }
    ViewerWindow* openAndWait(const QString& p) {
        ViewerWindow* w = app->openPaths({p});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        if (!QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 5000))
            return nullptr;
        return w;
    }
    // Four images, three with a position (Berlin, Paris, Rome), sorted by name.
    void makeTrip() {
        QVERIFY(write(path("1_berlin.jpg"), gpsJpeg(GeoPoint{52.5163, 13.3777}, Qt::red)));
        QVERIFY(write(path("2_paris.jpg"), gpsJpeg(GeoPoint{48.8584, 2.2945}, Qt::green)));
        QVERIFY(write(path("3_scan.jpg"), gpsJpeg(std::nullopt, Qt::gray)));
        QVERIFY(write(path("4_rome.jpg"), gpsJpeg(GeoPoint{41.8902, 12.4922}, Qt::blue)));
    }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        RecentItems::setSystemRegistrationEnabled(false);
        Settings::useTestingBackend(m_settingsDir.filePath(QStringLiteral("init.ini")));
        FolderMapPanel::setBackendFactory([](QWidget*) { return std::make_unique<FakeMapView>(); });
        ImageView probe;
        probe.resize(64, 64);
        probe.show();
        QVERIFY(QTest::qWaitForWindowExposed(&probe));
        auto* c = probe.findChild<ImageCanvas*>();
        QVERIFY(c);
        if (c->grabFramebuffer().isNull() || !c->isRhiAvailable())
            QSKIP("No QRhi (GPU) available on this platform");
    }

    void cleanupTestCase() { FolderMapPanel::setBackendFactory({}); }

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
        GeoTagCache::instance().clear();
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

    // Covers: F-INFO-08 (menu Darstellung → Karte, counts, SPEC 10.3 privacy)
    void F_INFO_08_mapOpensFromMenuWithCounts() {
        makeTrip();
        ViewerWindow* w = openAndWait(path("1_berlin.jpg"));
        QVERIFY(w);
        QVERIFY(!w->extras()->folderMapShown());
        QVERIFY(!w->extras()->folderMap()); // nothing created before the user asks
        QAction* toggle = app->menu()->action(Action::ToggleFolderMap);
        QVERIFY(toggle);
        QVERIFY(toggle->isCheckable());
        QCOMPARE(toggle->shortcut(), QKeySequence(QStringLiteral("Shift+Ctrl+M")));
        toggle->trigger();
        QTRY_VERIFY(w->extras()->folderMapShown());
        FolderMapPanel* panel = w->extras()->folderMap();
        QVERIFY(panel);
        auto* fake = static_cast<FakeMapView*>(panel->backend());
        QVERIFY(fake->started);
        QTRY_VERIFY(!panel->model()->isScanning());
        QCOMPARE(panel->model()->totalCount(), 4);
        QCOMPARE(panel->model()->locatedCount(), 3);
        QCOMPARE(fake->markers.size(), 3);
        QVERIFY(panel->statusLabel()->text().contains(QStringLiteral("3")));
        QVERIFY(panel->statusLabel()->text().contains(QStringLiteral("4")));
        QCOMPARE(fake->current, path("1_berlin.jpg"));
        QTRY_VERIFY(toggle->isChecked());
        // Closing (close button) removes the map: no more loading.
        panel->closeButton()->click();
        QTRY_VERIFY(!w->extras()->folderMapShown());
        QVERIFY(!fake->started);
        QCOMPARE(fake->stops, 1);
        // Opening again (shortcut path through the window) starts it again.
        QVERIFY(w->perform(Action::ToggleFolderMap));
        QTRY_VERIFY(fake->started);
        QCOMPARE(fake->starts, 2);
    }

    // Covers: F-INFO-08 ("Ordnerkarte" in the info panel's GPS box)
    void F_INFO_08_mapOpensFromInfoPanel() {
#ifdef LF_GENERIC_PLATFORM
        QSKIP("no map snapshots outside macOS (MapKit) (docs/LINUX.md)");
#endif
        makeTrip();
        ViewerWindow* w = openAndWait(path("2_paris.jpg"));
        QVERIFY(w);
        w->panels()->setInfoPanelEnabled(true);
        QTRY_VERIFY(w->panels()->infoPanel());
        InfoGpsBox* gps = w->panels()->infoPanel()->gpsBox();
        QTRY_VERIFY(gps->isVisible());
        QVERIFY(gps->folderMapButton());
        gps->folderMapButton()->click();
        QTRY_VERIFY(w->extras()->folderMapShown());
        QTRY_VERIFY(w->extras()->folderMap()->isMapOpen());
    }

    // Covers: F-INFO-08 (click on a marker shows the image; the current marker follows ←/→)
    void F_INFO_08_markerClickJumpsAndFollowsNavigation() {
        makeTrip();
        ViewerWindow* w = openAndWait(path("1_berlin.jpg"));
        QVERIFY(w);
        w->extras()->setFolderMapShown(true);
        FolderMapPanel* panel = w->extras()->folderMap();
        QVERIFY(panel);
        auto* fake = static_cast<FakeMapView*>(panel->backend());
        QTRY_VERIFY(!panel->model()->isScanning());
        // Click on Rome.
        QVERIFY(fake->markerActivated);
        fake->markerActivated(path("4_rome.jpg"));
        QTRY_COMPARE(w->currentPath(), path("4_rome.jpg"));
        QCOMPARE(fake->current, path("4_rome.jpg"));
        // Zoomed in on Rome, then ← to the scan (no position) and ← again to Paris: recentred.
        fake->userMovedMap();
        fake->visible = GeoRegion{{41.8902, 12.4922}, 0.05, 0.05};
        const int before = fake->regionChanges;
        QVERIFY(w->perform(Action::PreviousImage));
        QTRY_COMPARE(w->currentPath(), path("3_scan.jpg"));
        QCOMPARE(fake->regionChanges, before); // no position: the map stays
        QVERIFY(w->perform(Action::PreviousImage));
        QTRY_COMPARE(w->currentPath(), path("2_paris.jpg"));
        QCOMPARE(fake->current, path("2_paris.jpg"));
        QCOMPARE(fake->regionChanges, before + 1);
        QVERIFY(fake->visible.has_value());
        QVERIFY(geo::regionContains(*fake->visible, GeoPoint{48.8584, 2.2945}));
        // Hover asks for the thumbnail.
        fake->markerHovered(path("1_berlin.jpg"));
        QTRY_COMPARE(fake->thumbPath, path("1_berlin.jpg"));
        QVERIFY(!fake->thumb.isNull());
    }

    // Covers: F-INFO-08 (the name filter limits the map)
    void F_INFO_08_mapRespectsNameFilter() {
        makeTrip();
        ViewerWindow* w = openAndWait(path("1_berlin.jpg"));
        QVERIFY(w);
        w->extras()->setFolderMapShown(true);
        FolderMapPanel* panel = w->extras()->folderMap();
        QTRY_VERIFY(!panel->model()->isScanning());
        w->collection()->setNameFilter(QStringLiteral("paris"));
        QTRY_COMPARE(panel->model()->totalCount(), 1);
        QTRY_COMPARE(panel->model()->locatedCount(), 1);
    }

    // Covers: F-FMT-01 (SVG via resvg, GIMP XCF and Photoshop PSB shown in a window)
    void F_FMT_01_newFormatsVisibleInWindow() {
        const QStringList files = {QStringLiteral("css.svg"), QStringLiteral("quad.xcf"),
                                   QStringLiteral("quad16.xcf"), QStringLiteral("quad.psb"),
                                   QStringLiteral("cmyk.psb")};
        for (const QString& name : files) {
            const QString file = corpus(name);
            if (!QFileInfo::exists(file))
                QSKIP("format corpus missing - run tests/corpus/make_corpus.sh");
            ViewerWindow* w = openAndWait(file);
            QVERIFY2(w, qPrintable(name));
            QTRY_COMPARE_WITH_TIMEOUT(w->shownPath(), QDir::cleanPath(file), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(bool(w->view()->image()), 10000);
            // The pixel size reaches the status bar with the image info, which may come
            // after the first image (slow machines, CI): wait for it.
            const QString size = name.endsWith(QStringLiteral(".svg")) ? QStringLiteral("200 × 100")
                                                                       : QStringLiteral("64 × 48");
            QTRY_VERIFY2_WITH_TIMEOUT(w->statusText().contains(size), qPrintable(w->statusText()), 10000);
            w->close();
            QTest::qWait(20);
        }
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
    TstFolderMap tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_folder_map.moc"
