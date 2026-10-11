// SPDX-License-Identifier: GPL-3.0-or-later
// M2 side panels in a real window (SPEC 5.1): info panel with metadata, histogram and
// Finder tags (F-INFO-02/03/06) and the filmstrip (F-GRID-01), incl. fullscreen
// behaviour (F-VIEW-10). Real windows on macOS (the image canvas needs Metal).

#include "app/Application.h"
#include "app/Filmstrip.h"
#include "app/HistogramWidget.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/InfoPanel.h"
#include "app/MainMenu.h"
#include "app/PreferencesDialog.h"
#include "app/ViewerWindow.h"
#include "app/WindowPanels.h"
#include "core/Collection.h"
#include "core/Decoder.h"
#include "core/FileOps.h"
#include "core/ImageProvider.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/ThumbnailCache.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

#include <atomic>
#include <memory>

using namespace lf;

namespace {

class FakeShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(64, 48)) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path) ? QDir::cleanPath(path) : QString();
}

// Thumbnails of anything, counted (F-GRID-01: only visible items are requested).
class CountingDecoder : public Decoder {
public:
    QString name() const override { return QStringLiteral("counting"); }
    int priority() const override { return 1000; }
    bool canDecode(const QString&, const QByteArray&) const override { return true; }
    std::optional<ImageInfo> info(const QString&, QString*) override { return ImageInfo{}; }
    std::optional<DecodedImage> decode(const QString&, const DecodeRequest& req, QString*) override {
        ++decodes;
        DecodedImage d;
        d.fullSize = QSize(400, 300);
        d.image = QImage(d.fullSize.scaled(req.maxPixelSize, req.maxPixelSize, Qt::KeepAspectRatio),
                         QImage::Format_RGB32);
        d.image.fill(Qt::darkCyan);
        return d;
    }
    Metadata metadata(const QString&) override { return {}; }
    std::atomic<int> decodes{0};
};

} // namespace

class TstPanels : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    QTemporaryDir m_thumbDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<FakeShell> shell;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }

    void makeFolder() {
        QVERIFY(!makeImage(path("a.png"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("b.png"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("c.png"), Qt::blue).isEmpty());
    }

    ViewerWindow* open(const QString& file) {
        ViewerWindow* w = app->openPaths({file});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        if (!QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, 5000))
            return nullptr;
        return w;
    }
    static bool waitShown(ViewerWindow* w, const QString& p) {
        return QTest::qWaitFor([&] { return w->shownPath() == p && w->view()->image() != nullptr; }, 5000);
    }
    static InfoPanel* info(ViewerWindow* w) { return w->panels()->infoPanel(); }
    static bool waitInfo(ViewerWindow* w, const QString& p) {
        return QTest::qWaitFor([&] { return info(w) && info(w)->path() == p && !info(w)->isLoading(); },
                               5000);
    }
    static void key(ViewerWindow* w, Qt::Key k) { QTest::keyClick(w->view(), k); }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        // Never write into the user's ~/Library/Caches during tests.
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
        shell = std::make_unique<FakeShell>();
        AppServices services;
        services.shell = shell.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
    }

    void cleanup() {
        if (auto* dlg = PreferencesDialog::existingInstance())
            dlg->close();
        app.reset();
        QTest::qWait(10);
        shell.reset();
        dir.reset();
    }

    // ---- info panel (F-INFO-02) ----

    void F_INFO_02_panelStaysOpenWhileFlipping() {
        makeFolder();
        ViewerWindow* w = open(path("a.png"));
        QVERIFY(w);
        QVERIFY(!w->panels()->infoPanelShown()); // minimal standard view (SPEC 5.1)
        key(w, Qt::Key_I);
        QVERIFY(w->panels()->infoPanelShown());
        QVERIFY(w->panels()->infoPanelEnabled());
        QCOMPARE(app->menu()->action(Action::ToggleInfoPanel)->shortcuts().first(), QKeySequence("Ctrl+I"));
        QVERIFY(waitInfo(w, path("a.png")));
        QCOMPARE(info(w)->value("Datei", "Name"), QStringLiteral("a.png"));
        // The info panel sits right of the image.
        auto* dock = w->findChild<QDockWidget*>(QStringLiteral("lfInfoDock"));
        QVERIFY(dock);
        QCOMPARE(w->dockWidgetArea(dock), Qt::RightDockWidgetArea);
        QVERIFY(dock->geometry().left() >= w->view()->mapTo(w, QPoint(w->view()->width(), 0)).x() - 1);
        for (const char* name : {"b.png", "c.png"}) {
            key(w, Qt::Key_Right);
            QVERIFY(w->panels()->infoPanelShown()); // stays open
            QVERIFY(waitInfo(w, path(QString::fromLatin1(name))));
            QCOMPARE(info(w)->value("Datei", "Name"), QString::fromLatin1(name));
            QCOMPARE(info(w)->value("Bild", "Pixelmaße"), QStringLiteral("64 × 48"));
        }
        // ⌘I closes it again (menu action).
        app->menu()->action(Action::ToggleInfoPanel)->trigger();
        QVERIFY(!w->panels()->infoPanelShown());
        QVERIFY(!Settings::instance().value(key::ShowInfoPanel).toBool());
    }

    void F_INFO_02_asyncLoadShowsPlaceholder() {
        makeFolder();
        ImageProvider provider;
        InfoPanel panel(provider, std::shared_ptr<TagService>(createTagService()));
        panel.resize(300, 500);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        // Event-loop gap probe: a fast timer keeps ticking while the metadata loads; reading
        // on the GUI thread would show up as a long gap. Generous bound: this is about
        // gross blocking, not speed (a loaded machine delays ticks as well); that the load is
        // asynchronous at all is the isLoading() check right after the call.
        const int maxGapMs = std::max(2000, qEnvironmentVariableIntValue("LF_UI_GAP_LIMIT_MS"));
        QElapsedTimer clock;
        qint64 last = 0, maxGap = 0;
        QTimer ticker;
        ticker.setInterval(1);
        connect(&ticker, &QTimer::timeout, this, [&] {
            const qint64 now = clock.elapsed();
            maxGap = std::max(maxGap, now - last);
            last = now;
        });
        clock.start();
        ticker.start();
        panel.setFile(path("b.png"));
        maxGap = std::max(maxGap, clock.elapsed()); // the call itself
        // Returns at once with name/path and a placeholder; metadata comes from a worker.
        QVERIFY(panel.isLoading());
        QCOMPARE(panel.value("Datei", "Name"), QStringLiteral("b.png"));
        QVERIFY(panel.value("Bild", "Pixelmaße").isEmpty());
        QCOMPARE(panel.sectionTitles().value(1), QStringLiteral("Bild"));
        QVERIFY(QTest::qWaitFor([&] { return !panel.isLoading(); }, 30000));
        ticker.stop();
        QVERIFY2(maxGap <= maxGapMs, qPrintable(QStringLiteral("GUI thread blocked for %1 ms").arg(maxGap)));
        QCOMPARE(panel.value("Bild", "Pixelmaße"), QStringLiteral("64 × 48"));
        QVERIFY(panel.value("Datei", "Größe").contains(QStringLiteral("Byte")));
        QVERIFY(!panel.value("Datei", "Geändert").isEmpty());
        // Flipping quickly through many files: only the newest one ends up shown.
        for (const char* n : {"a.png", "c.png", "b.png", "a.png"})
            panel.setFile(path(QString::fromLatin1(n)));
        QVERIFY(QTest::qWaitFor([&] { return !panel.isLoading(); }, 30000));
        QCOMPARE(panel.value("Datei", "Name"), QStringLiteral("a.png"));
        // A hidden panel does not read anything.
        panel.hide();
        panel.setFile(path("c.png"));
        QTest::qWait(100);
        QVERIFY(panel.isLoading());
        panel.show();
        QVERIFY(QTest::qWaitFor([&] { return !panel.isLoading(); }, 30000));
        QCOMPARE(panel.value("Datei", "Name"), QStringLiteral("c.png"));
    }

    void F_INFO_02_exifValuesOfCorpusJpeg() {
        const QString src = QStringLiteral(LF_CORPUS_DIR "/generated/exif.jpg");
        if (!QFileInfo::exists(src))
            QSKIP("format corpus missing - run tests/corpus/make_corpus.sh");
        QVERIFY(QFile::copy(src, path("exif.jpg")));
        Settings::instance().setValue(key::ShowInfoPanel, true); // remembered: shown at once
        ViewerWindow* w = open(path("exif.jpg"));
        QVERIFY(w);
        QVERIFY(w->panels()->infoPanelShown());
        QVERIFY(waitInfo(w, path("exif.jpg")));
        InfoPanel* p = info(w);
        QCOMPARE(p->sectionTitles(), (QStringList{"Datei", "Bild", "Kamera", "Aufnahme", "GPS"}));
        QCOMPARE(p->value("Bild", "Format"), QStringLiteral("JPEG"));
        QCOMPARE(p->value("Bild", "Pixelmaße"), QStringLiteral("640 × 480"));
        QCOMPARE(p->value("Bild", "HDR"), QStringLiteral("Nein"));
        QCOMPARE(p->value("Kamera", "Modell"), QStringLiteral("Synthetic Camera X1"));
        QCOMPARE(p->value("Kamera", "Objektiv"), QStringLiteral("Synthetic 35mm F1.4"));
        QCOMPARE(p->value("Aufnahme", "Blende"), QStringLiteral("f/2,8"));
        QCOMPARE(p->value("Aufnahme", "Belichtungszeit"), QStringLiteral("1/250 s"));
        QCOMPARE(p->value("Aufnahme", "ISO"), QStringLiteral("400"));
        QVERIFY(p->value("Aufnahme", "Aufnahmedatum").startsWith(QStringLiteral("17.05.2024 14:30:05")));
        QVERIFY(p->value("GPS", "Breitengrad").endsWith(QStringLiteral("N")));
        QVERIFY(p->value("GPS", "Längengrad").endsWith(QStringLiteral("W")));
        // Rows keep a meaningful order (not alphabetical).
        const QTreeWidgetItem* shot = p->tree()->topLevelItem(3);
        QCOMPARE(shot->child(0)->text(0), QStringLiteral("Aufnahmedatum"));
        QCOMPARE(shot->child(1)->text(0), QStringLiteral("Brennweite"));
    }

    void F_INFO_02_copyValue() {
        makeFolder();
        ViewerWindow* w = open(path("b.png"));
        QVERIFY(w);
        key(w, Qt::Key_I);
        QVERIFY(waitInfo(w, path("b.png")));
        QTreeWidget* tree = info(w)->tree();
        const QTreeWidgetItem* file = tree->topLevelItem(0);
        const QTreeWidgetItem* name = file->child(0);
        QCOMPARE(InfoPanel::copyText(name, false), QStringLiteral("b.png"));
        QCOMPARE(InfoPanel::copyText(name, true), QStringLiteral("Name: b.png"));
        QVERIFY(InfoPanel::copyText(file, false).startsWith(QStringLiteral("Datei\nName: b.png\n")));
        // The panel never takes the keyboard from the image.
        QCOMPARE(tree->focusPolicy(), Qt::NoFocus);
        key(w, Qt::Key_Right);
        QTRY_COMPARE(w->currentPath(), path("c.png"));
    }

    void F_INFO_02_visibilityRememberedAndPreferences() {
        makeFolder();
        ViewerWindow* w = open(path("a.png"));
        QVERIFY(w);
        key(w, Qt::Key_I);
        QVERIFY(Settings::instance().value(key::ShowInfoPanel).toBool());
        app->menu()->refreshState();
        QVERIFY(app->menu()->action(Action::ToggleInfoPanel)->isChecked());
        ViewerWindow* second = app->newWindow();
        second->openPaths({path("b.png")});
        QVERIFY(QTest::qWaitForWindowExposed(second, 5000));
        QVERIFY(second->panels()->infoPanelShown()); // new windows start like the last one
        // The settings window offers both panels (no longer "in einer späteren Version").
        app->showPreferences();
        PreferencesDialog* prefs = PreferencesDialog::existingInstance();
        QVERIFY(prefs);
        for (const char* k : {key::ShowInfoPanel, key::ShowFilmstrip}) {
            auto* cb = prefs->findChild<QCheckBox*>(QStringLiteral("setting:") + QLatin1String(k));
            QVERIFY2(cb && cb->isEnabled() && !cb->text().contains(QStringLiteral("Version")), k);
        }
    }

    // ---- histogram (F-INFO-03) ----

    void F_INFO_03_histogramOfShownImage() {
        makeFolder();
        ViewerWindow* w = open(path("a.png"));
        QVERIFY(w);
        QVERIFY(waitShown(w, path("a.png")));
        key(w, Qt::Key_I);
        QTRY_VERIFY_WITH_TIMEOUT(!info(w)->isHistogramPending(), 5000);
        const Histogram& h = info(w)->histogram();
        QVERIFY(h.samples > 0);
        QCOMPARE(h.red[255], quint32(h.samples)); // pure red
        QCOMPARE(h.green[0], quint32(h.samples));
        QVERIFY(info(w)->histogramWidget()->isVisible());
        // Next image: blue.
        key(w, Qt::Key_Right);
        QVERIFY(waitShown(w, path("b.png")));
        QTRY_VERIFY_WITH_TIMEOUT(info(w)->path() == path("b.png") && !info(w)->isHistogramPending(), 5000);
        QCOMPARE(info(w)->histogram().green[255], quint32(info(w)->histogram().samples));
        // Drawn in both appearances without errors.
        HistogramWidget* hw = info(w)->histogramWidget();
        for (QColor bg : {QColor(30, 30, 30), QColor(240, 240, 240)}) {
            QPalette pal = hw->palette();
            pal.setColor(QPalette::Window, bg);
            hw->setPalette(pal);
            const QImage shot = hw->grab().toImage();
            QVERIFY(!shot.isNull());
        }
    }

    // ---- Finder tags (F-INFO-06) ----

    void F_INFO_06_addAndRemoveTagsInPanel() {
        makeFolder();
        auto service = createTagService();
        if (!service->isSupported())
            QSKIP("no file tags on this platform");
        ViewerWindow* w = open(path("c.png"));
        QVERIFY(w);
        key(w, Qt::Key_I);
        QVERIFY(waitInfo(w, path("c.png")));
        InfoPanel* p = info(w);
        QVERIFY(p->tagsSupported());
        QVERIFY(p->tags().isEmpty());
        auto* edit = p->findChild<QLineEdit*>(QStringLiteral("lfTagEdit"));
        QVERIFY(edit && edit->isEnabled());
        QSignalSpy written(p, &InfoPanel::tagsWritten);
        edit->setText(QStringLiteral("Lumaflip Test"));
        QMetaObject::invokeMethod(edit, "returnPressed");
        QVERIFY(written.wait(30000));
        QCOMPARE(written.last().at(1).toBool(), true);
        QString error;
        QCOMPARE(service->tags(path("c.png"), &error).value_or(QStringList()), QStringList{"Lumaflip Test"});
        QCOMPARE(p->tags(), QStringList{"Lumaflip Test"});
        p->addTag(QStringLiteral("Grün"));
        QVERIFY(written.wait(30000));
        QCOMPARE(service->tags(path("c.png"), &error).value_or(QStringList()),
                 (QStringList{"Lumaflip Test", "Grün"}));
        auto* list = p->findChild<QListWidget*>(QStringLiteral("lfTagList"));
        QCOMPARE(list->count(), 2);
        p->removeTag(QStringLiteral("Lumaflip Test"));
        QVERIFY(written.wait(30000));
        QCOMPARE(service->tags(path("c.png"), &error).value_or(QStringList()), QStringList{"Grün"});
        // Shown again for the file after flipping away and back.
        key(w, Qt::Key_Left);
        QVERIFY(waitInfo(w, path("b.png")));
        QVERIFY(p->tags().isEmpty());
        key(w, Qt::Key_Right);
        QVERIFY(waitInfo(w, path("c.png")));
        QTRY_COMPARE(p->tags(), QStringList{"Grün"});
    }

    // ---- filmstrip (F-GRID-01) ----

    void F_GRID_01_filmstripCountClickAndFollow() {
        makeFolder();
        ViewerWindow* w = open(path("a.png"));
        QVERIFY(w);
        QVERIFY(!w->panels()->filmstripShown());
        key(w, Qt::Key_T);
        QVERIFY(w->panels()->filmstripShown());
        QVERIFY(Settings::instance().value(key::ShowFilmstrip).toBool());
        Filmstrip* film = w->panels()->filmstrip();
        QVERIFY(film);
        auto* dock = w->findChild<QDockWidget*>(QStringLiteral("lfFilmDock"));
        QCOMPARE(w->dockWidgetArea(dock), Qt::BottomDockWidgetArea);
        QCOMPARE(film->filmModel()->rowCount(), 3);
        QCOMPARE(film->filmModel()->currentRow(), 0);
        QCOMPARE(film->focusPolicy(), Qt::NoFocus);
        // Thumbnails arrive.
        QTRY_VERIFY_WITH_TIMEOUT(
            !film->filmModel()->index(2).data(FilmstripModel::ThumbnailRole).value<QImage>().isNull(), 60000);
        // Current follows navigation.
        key(w, Qt::Key_Right);
        QCOMPARE(film->filmModel()->currentRow(), 1);
        QVERIFY(film->filmModel()->index(1).data(FilmstripModel::CurrentRole).toBool());
        QVERIFY(!film->filmModel()->index(0).data(FilmstripModel::CurrentRole).toBool());
        // Click jumps.
        const QRect r = film->visualRect(film->filmModel()->index(2));
        QTest::mouseClick(film->viewport(), Qt::LeftButton, {}, r.center());
        QCOMPARE(w->currentPath(), path("c.png"));
        QVERIFY(waitShown(w, path("c.png")));
        QCOMPARE(film->filmModel()->currentRow(), 2);
        // Collection changes (remove/insert) update the strip.
        QVERIFY(QFile::remove(path("a.png"))); // gone on disk too (the folder watcher may reload)
        w->fileRemoved(path("a.png"));
        QTRY_COMPARE(film->filmModel()->rowCount(), 2);
        QCOMPARE(film->filmModel()->currentRow(), w->collection()->currentIndex());
        QVERIFY(!makeImage(path("d.png"), Qt::yellow).isEmpty());
        w->fileAdded(path("d.png"), false);
        QTRY_COMPARE(film->filmModel()->rowCount(), 3);
        QCOMPARE(film->filmModel()
                     ->index(film->filmModel()->currentRow())
                     .data(FilmstripModel::PathRole)
                     .toString(),
                 path("c.png"));
        // T hides it again.
        key(w, Qt::Key_T);
        QVERIFY(!w->panels()->filmstripShown());
    }

    void F_GRID_01_onlyVisibleThumbnailsRequested() {
        for (int i = 0; i < 150; ++i)
            QVERIFY(!makeImage(path(QStringLiteral("img_%1.png").arg(i, 3, 10, QLatin1Char('0'))), Qt::gray,
                               QSize(8, 6))
                         .isEmpty());
        Collection collection;
        collection.openFolder(QDir::cleanPath(dir->path()));
        QVERIFY(QTest::qWaitFor([&] { return !collection.isLoading() && collection.count() == 150; }, 5000));
        auto decoder = std::make_shared<CountingDecoder>();
        DecoderRegistry registry;
        registry.add(decoder);
        ThumbnailCacheOptions o;
        o.directory = m_thumbDir.path() + QStringLiteral("/visible");
        ThumbnailCache cache(registry, o);
        Filmstrip film(collection, cache);
        film.resize(640, film.sizeHint().height());
        film.show();
        QVERIFY(QTest::qWaitForWindowExposed(&film));
        for (int r : film.visibleRows()) // as painting does (see below)
            film.filmModel()->index(r).data(FilmstripModel::ThumbnailRole);
        QTRY_VERIFY_WITH_TIMEOUT(cache.pendingJobs() == 0 && decoder->decodes.load() > 0, 60000);
        QTest::qWait(100);
        const int visible = int(film.visibleRows().size());
        QVERIFY(visible > 0 && visible < 20);
        QVERIFY2(
            decoder->decodes.load() <= visible + 2,
            qPrintable(QStringLiteral("%1 decodes, %2 visible").arg(decoder->decodes.load()).arg(visible)));
        QCOMPARE(film.filmModel()->rowCount(), 150);
        // Jumping to the end: the strip scrolls there and makes only those thumbnails.
        const int before = decoder->decodes.load();
        collection.setCurrentIndex(149);
        QTRY_VERIFY_WITH_TIMEOUT(film.visibleRows().contains(149), 15000);
        // What painting the visible rows does - also when the window is covered by others
        // (parallel GUI test runs) and the system does not let it paint right now.
        for (int r : film.visibleRows())
            film.filmModel()->index(r).data(FilmstripModel::ThumbnailRole);
        QTRY_VERIFY_WITH_TIMEOUT(cache.pendingJobs() == 0 && decoder->decodes.load() > before, 60000);
        QTest::qWait(100);
        QVERIFY(decoder->decodes.load() - before <= int(film.visibleRows().size()) + 2);
        QVERIFY(decoder->decodes.load() < 40);
    }

    // ---- fullscreen (F-VIEW-10 with panels, SPEC 5.1) ----

    void F_VIEW_10_panelsHideInFullscreenAndReturnAtEdge() {
        makeFolder();
        Settings::instance().setValue(key::ShowInfoPanel, true);
        Settings::instance().setValue(key::ShowFilmstrip, true);
        ViewerWindow* w = open(path("a.png"));
        QVERIFY(w);
        QVERIFY(w->panels()->infoPanelShown());
        QVERIFY(w->panels()->filmstripShown());
        key(w, Qt::Key_F);
        QVERIFY(w->isFullScreen());
        QVERIFY(!w->panels()->infoPanelShown());
        QVERIFY(!w->panels()->filmstripShown());
        QVERIFY(w->panels()->infoPanelEnabled()); // still wanted
        (void)QTest::qWaitFor([w] { return w->windowHandle()->windowStates() & Qt::WindowFullScreen; }, 3000);
        QTest::qWait(800); // native fullscreen animation
        auto* canvas = w->view()->findChild<ImageCanvas*>();
        const QPoint right(canvas->width() - 3, canvas->height() / 2);
        QMouseEvent move(QEvent::MouseMove, QPointF(right), canvas->mapToGlobal(QPointF(right)), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &move);
        QVERIFY(w->panels()->infoPanelShown());
        QVERIFY(w->panels()->filmstripShown());
        QTRY_VERIFY_WITH_TIMEOUT(!w->panels()->infoPanelShown(), 4000); // hides after 1.5 s idle
        key(w, Qt::Key_Escape);
        QVERIFY(!w->isFullScreen());
        QVERIFY(w->panels()->infoPanelShown());
        QVERIFY(w->panels()->filmstripShown());
        (void)QTest::qWaitFor([w] { return !(w->windowHandle()->windowStates() & Qt::WindowFullScreen); },
                              3000);
        QTest::qWait(800);
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
    TstPanels tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_panels.moc"
