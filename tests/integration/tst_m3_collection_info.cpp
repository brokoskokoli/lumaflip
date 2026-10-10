// SPDX-License-Identifier: GPL-3.0-or-later
// M3 collection and info features in real windows: "Unterordner einbeziehen" (F-OPEN-08),
// sibling folders via the menu (F-OPEN-09), RAW+JPEG pairs with badge, "RAW anzeigen" and
// trash of both files (F-FMT-06), the GPS box with the click-gated map and "In Karten
// öffnen" (F-INFO-04) and the raw metadata window with search and copy (F-INFO-05).
// Files only in temporary folders; the trash is a temporary folder; no network (the map
// test uses a fake renderer, the real panel's map button is never clicked).

#include "app/Application.h"
#include "app/CollectionExtras.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/InfoGpsBox.h"
#include "app/InfoPanel.h"
#include "app/MainMenu.h"
#include "app/MetadataDialog.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "app/WindowExtras.h"
#include "app/WindowPanels.h"
#include "core/Collection.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/RatingStore.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTreeWidget>
#include <QUuid>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

class FolderTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& path, QString* error) override {
        const QString target =
            QDir(dir.path())
                .filePath(QUuid::createUuid().toString(QUuid::Id128) + QFileInfo(path).fileName());
        if (!QFile::rename(path, target)) {
            if (error)
                *error = QStringLiteral("trash failed");
            return std::nullopt;
        }
        return target;
    }
    bool restoreFromTrash(const QString& trashed, const QString& original, QString* error) override {
        if (QFileInfo::exists(original) || !QFile::rename(trashed, original)) {
            if (error)
                *error = QStringLiteral("restore failed");
            return false;
        }
        return true;
    }
    QTemporaryDir dir;
};

class NullShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

class FakeMapProvider : public MapSnapshotProvider {
public:
    void request(const GeoPoint&, QSize size, qreal, QObject*, Done done) override {
        ++requests;
        QImage img(size, QImage::Format_RGB32);
        img.fill(Qt::darkGreen);
        done(img, QString());
    }
    void cancel() override {}
    int requests = 0;
};

QString makeImage(const QString& path, QColor color, QSize size = QSize(320, 240)) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QImage img(size, QImage::Format_RGB32);
    img.fill(color);
    return img.save(path, "JPEG") ? QDir::cleanPath(path) : QString();
}

} // namespace

class TstM3CollectionInfo : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<FolderTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<NullShell> shell;
    std::unique_ptr<Application> app;
    QList<QUrl> openedUrls;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    static bool waitLoaded(ViewerWindow* w, int ms = 5000) {
        return QTest::qWaitFor([w] { return !w->collection()->isLoading(); }, ms);
    }
    ViewerWindow* openAndWait(const QString& p) {
        ViewerWindow* w = app->openPaths({p});
        if (!w || !QTest::qWaitForWindowExposed(w, 5000) || !waitLoaded(w))
            return nullptr;
        return w;
    }
    static QString corpus(const QString& name) { return QStringLiteral(LF_CORPUS_DIR "/generated/") + name; }

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::Quality>("lf::Quality");
        qRegisterMetaType<lf::DecodedImagePtr>("lf::DecodedImagePtr");
        RecentItems::setSystemRegistrationEnabled(false);
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
        trash = std::make_unique<FolderTrash>();
        ops = std::make_unique<FileOps>(*trash);
        shell = std::make_unique<NullShell>();
        openedUrls.clear();
        AppServices services;
        services.fileOps = ops.get();
        services.shell = shell.get();
        services.openUrl = [this](const QUrl& u) {
            openedUrls << u;
            return true;
        };
        app = std::make_unique<Application>(services);
        RatingStore::instance().setOptions(RatingOptions()); // sidecar only
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

    // Covers: F-OPEN-08 (menu toggle per window, subfolder in the status bar, session)
    void F_OPEN_08_subfolderToggleInWindow() {
        QVERIFY(!makeImage(path("a.jpg"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("Reise/b.jpg"), Qt::green).isEmpty());
        QVERIFY(!makeImage(path("Reise/Tag 2/c.jpg"), Qt::blue).isEmpty());
        ViewerWindow* w = openAndWait(path("a.jpg"));
        QVERIFY(w);
        QCOMPARE(w->collection()->count(), 1);
        QAction* toggle = app->menu()->action(Action::ToggleSubfolders);
        QVERIFY(toggle && toggle->isCheckable());
        toggle->trigger();
        QVERIFY(w->collection()->includesSubfolders());
        QTRY_COMPARE(w->collection()->count(), 3);
        QVERIFY(waitLoaded(w));
        app->menu()->refreshState();
        QVERIFY(toggle->isChecked());
        QCOMPARE(w->currentPath(), path("a.jpg")); // the image stays
        w->perform(Action::NextImage);
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("„Reise“")));
        w->perform(Action::NextImage);
        QTRY_VERIFY(w->statusText().contains(QStringLiteral("„Reise/Tag 2“")));
        // A new image in a subfolder appears (tree watching).
        QVERIFY(!makeImage(path("Reise/Tag 2/d.jpg"), Qt::yellow).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(w->collection()->count(), 4, 5000);
        // Per window: a second window keeps its own setting; the session remembers it.
        QVERIFY(w->sessionState().value(QStringLiteral("subfolders")).toBool());
        ViewerWindow* other = app->openPaths({path("Reise")});
        QVERIFY(other);
        QVERIFY(waitLoaded(other));
        QVERIFY(!other->collection()->includesSubfolders());
        toggle->trigger(); // acts on the front window
        app->menu()->refreshState();
    }

    // Covers: F-OPEN-09 (menu items with ⌥→ / ⌥←, next/previous sibling folder)
    void F_OPEN_09_siblingFolderMenu() {
        QVERIFY(!makeImage(path("A/1.jpg"), Qt::red).isEmpty());
        QVERIFY(!makeImage(path("B/2.jpg"), Qt::green).isEmpty());
        ViewerWindow* w = openAndWait(path("A/1.jpg"));
        QVERIFY(w);
        QAction* next = app->menu()->action(Action::NextFolder);
        QAction* prev = app->menu()->action(Action::PreviousFolder);
        QVERIFY(next && prev);
        QVERIFY(next->shortcuts().contains(QKeySequence(Qt::AltModifier | Qt::Key_Right)));
        next->trigger();
        QTRY_COMPARE(w->collection()->folderPath(), path("B"));
        QTRY_COMPARE(w->currentPath(), path("B/2.jpg"));
        prev->trigger();
        QTRY_COMPARE(w->collection()->folderPath(), path("A"));
    }

    // Covers: F-FMT-06 (badge, info panel shows both, "RAW anzeigen", trash of both)
    void F_FMT_06_pairBadgeToggleAndTrash() {
        Settings::instance().setValue(key::PairRawJpeg, true);
        QVERIFY(!makeImage(path("IMG_1.JPG"), Qt::red).isEmpty());
        QVERIFY(QFile::copy(path("IMG_1.JPG"), path("IMG_1.DNG"))); // stands in for the RAW
        QVERIFY(!makeImage(path("IMG_2.JPG"), Qt::green).isEmpty());
        ViewerWindow* w = openAndWait(path("IMG_1.JPG"));
        QVERIFY(w);
        QTRY_COMPARE(w->collection()->count(), 2);
        CollectionExtras* x = w->extras()->collectionExtras();
        QTRY_VERIFY(x->currentIsPair());
        QVERIFY(w->statusText().contains(QStringLiteral("RAW+JPEG")));
        auto* badge = w->findChild<QLabel*>(QStringLiteral("lfStatusRawJpeg"));
        QVERIFY(badge && badge->isVisible());
        // The info panel describes both files.
        w->perform(Action::ToggleInfoPanel);
        InfoPanel* info = w->panels()->infoPanel();
        QVERIFY(info);
        QTRY_VERIFY(!info->isLoading());
        QCOMPARE(info->value(QStringLiteral("RAW+JPEG-Paar"), QStringLiteral("Zweite Datei")),
                 QStringLiteral("IMG_1.DNG"));
        // "RAW anzeigen" switches to the RAW at the same position.
        QAction* raw = app->menu()->action(Action::ToggleRawJpeg);
        app->menu()->refreshState();
        QVERIFY(raw && raw->isEnabled());
        raw->trigger();
        QTRY_COMPARE(w->currentPath(), path("IMG_1.DNG"));
        QVERIFY(x->currentShowsRaw());
        raw->trigger();
        QTRY_COMPARE(w->currentPath(), path("IMG_1.JPG"));
        // Trash takes both files in one step; undo brings both back as one pair.
        w->perform(Action::Trash);
        QTRY_VERIFY(!QFileInfo::exists(path("IMG_1.JPG")) && !QFileInfo::exists(path("IMG_1.DNG")));
        QTRY_COMPARE(w->collection()->count(), 1);
        QCOMPARE(w->currentPath(), path("IMG_2.JPG"));
        w->perform(Action::Undo);
        QTRY_VERIFY(QFileInfo::exists(path("IMG_1.JPG")) && QFileInfo::exists(path("IMG_1.DNG")));
        QTRY_COMPARE(w->collection()->count(), 2);
        // Off (default): both files are separate images again.
        Settings::instance().setValue(key::PairRawJpeg, false);
        QTRY_COMPARE(w->collection()->count(), 3);
    }

    // Covers: F-INFO-04 (GPS box in the panel without network; map after the click with a
    // fake renderer; "In Karten öffnen")
    void F_INFO_04_gpsBoxMapAfterClick() {
        if (!QFileInfo::exists(corpus(QStringLiteral("exif.jpg"))))
            QSKIP("format corpus missing - run tests/corpus/make_corpus.sh");
        QVERIFY(QFile::copy(corpus(QStringLiteral("exif.jpg")), path("gps.jpg")));
        ViewerWindow* w = openAndWait(path("gps.jpg"));
        QVERIFY(w);
        w->perform(Action::ToggleInfoPanel);
        InfoPanel* info = w->panels()->infoPanel();
        QVERIFY(info);
        QTRY_VERIFY(info->gpsBox()->isVisible());
        MapPreviewController* map = info->gpsBox()->controller();
        QCOMPARE(map->state(), MapPreviewController::State::Idle); // nothing loaded (SPEC 10.3)
        QCOMPARE(map->requestCount(), 0);
        QVERIFY(info->gpsBox()->coordinatesLabel()->text().contains(QStringLiteral("48°")));
        QVERIFY(info->gpsBox()->showMapButton()->isVisible());

        // The same box with a fake renderer: the click loads the map.
        auto fake = std::make_unique<FakeMapProvider>();
        FakeMapProvider* provider = fake.get();
        InfoGpsBox box(std::move(fake));
        QList<QUrl> urls;
        box.setUrlOpener([&](const QUrl& u) { urls << u; });
        box.setLocation(GeoPoint{48.1371, -122.4194}, QStringLiteral("gps.jpg"));
        box.show();
        QCOMPARE(provider->requests, 0);
        QTest::mouseClick(box.showMapButton(), Qt::LeftButton);
        QCOMPARE(provider->requests, 1);
        QTRY_VERIFY(box.mapLabel()->isVisible());
        QTest::mouseClick(box.openMapsButton(), Qt::LeftButton);
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.first().scheme(), QStringLiteral("maps"));
    }

    // Covers: F-INFO-05 (raw metadata window: tree, search, copy, follows the image)
    void F_INFO_05_metadataWindowSearchCopy() {
        if (!QFileInfo::exists(corpus(QStringLiteral("exif.jpg"))))
            QSKIP("format corpus missing - run tests/corpus/make_corpus.sh");
        QVERIFY(QFile::copy(corpus(QStringLiteral("exif.jpg")), path("a_exif.jpg")));
        QVERIFY(!makeImage(path("b_plain.jpg"), Qt::blue).isEmpty());
        ViewerWindow* w = openAndWait(path("a_exif.jpg"));
        QVERIFY(w);
        w->perform(Action::ToggleInfoPanel);
        InfoPanel* info = w->panels()->infoPanel();
        QVERIFY(info);
        QTRY_VERIFY(info->allMetadataButton()->isEnabled());
        QTest::mouseClick(info->allMetadataButton(), Qt::LeftButton);
        MetadataDialog* dlg = info->metadataDialog();
        QVERIFY(dlg);
        QTRY_VERIFY(!dlg->isLoading());
        QVERIFY(dlg->view()->topLevelItemCount() >= 3); // e.g. Exif, GPS, TIFF, XMP
        dlg->searchField()->setText(QStringLiteral("synthetic camera"));
        QTRY_VERIFY(dlg->allText().contains(QStringLiteral("Model: Synthetic Camera X1")));
        QVERIFY(!dlg->allText().contains(QStringLiteral("FNumber")));
        dlg->copyAll();
        QVERIFY(QGuiApplication::clipboard()->text().contains(QStringLiteral("Synthetic Camera X1")));
        // Flipping to the next image: the window follows.
        w->perform(Action::NextImage);
        QTRY_COMPARE(dlg->path(), path("b_plain.jpg"));
        QTRY_VERIFY(!dlg->isLoading());
        dlg->close();
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
    TstM3CollectionInfo tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_m3_collection_info.moc"
