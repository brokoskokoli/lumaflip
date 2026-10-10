// SPDX-License-Identifier: GPL-3.0-or-later
// M4 part 2 in real windows: the crop flow (F-EDIT-03: overlay, Enter saves undoably,
// "Speichern unter" keeps the original, Esc cancels), the export dialog and its run
// (F-EDIT-04), the photo overlay toggles reaching the shader and the AF points overlay
// (F-INFO-07) and the difference/blend compare (F-GRID-04). Images are generated in
// temporary folders.

#include "app/Application.h"
#include "app/CompareBlendOverlay.h"
#include "app/CompareView.h"
#include "app/CropOverlay.h"
#include "app/ExportDialog.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/MainMenu.h"
#include "app/OverviewController.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "app/WindowEditing.h"
#include "app/WindowExtras.h"
#include "core/Collection.h"
#include "core/CropSave.h"
#include "core/FileOps.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <memory>

using namespace lf;

namespace {

class NoTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString& p, QString*) override {
        return p + QStringLiteral(".trashed");
    }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return true; }
};

QByteArray readBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QImage pattern(QSize size, QColor c = Qt::darkCyan) {
    QImage img(size, QImage::Format_RGB32);
    img.fill(c);
    QPainter p(&img);
    p.fillRect(QRect(0, 0, size.width() / 2, size.height() / 2), Qt::yellow);
    p.fillRect(QRect(size.width() / 2, size.height() / 2, size.width() / 4, size.height() / 4), Qt::black);
    return img;
}

} // namespace

class TstM4Edit : public QObject {
    Q_OBJECT

    QTemporaryDir m_settingsDir;
    int m_settingsCounter = 0;
    std::unique_ptr<QTemporaryDir> dir;
    std::unique_ptr<NoTrash> trash;
    std::unique_ptr<FileOps> ops;
    std::unique_ptr<Application> app;

    QString path(const QString& name) const { return QDir::cleanPath(dir->path()) + QLatin1Char('/') + name; }
    ViewerWindow* openAndWait(const QString& p) {
        app->openPaths({p});
        if (!QTest::qWaitFor([this] { return app->frontWindow() != nullptr; }, 5000))
            return nullptr;
        ViewerWindow* w = app->frontWindow();
        if (!QTest::qWaitForWindowExposed(w, 5000))
            return nullptr;
        if (!QTest::qWaitFor(
                [w, p] { return w->shownPath() == QFileInfo(p).absoluteFilePath() && w->view()->image(); },
                10000))
            return nullptr;
        return w;
    }
    QRgb centerPixel(ViewerWindow* w) {
        auto* canvas = w->view()->findChild<ImageCanvas*>();
        const QImage fb = canvas ? canvas->grabFramebuffer() : QImage();
        return fb.isNull() ? 0 : fb.pixel(fb.width() / 2, fb.height() / 2);
    }

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
        trash = std::make_unique<NoTrash>();
        ops = std::make_unique<FileOps>(*trash);
        AppServices services;
        services.fileOps = ops.get();
        services.openUrl = [](const QUrl&) { return true; };
        app = std::make_unique<Application>(services);
    }

    void cleanup() {
        app.reset();
        QTest::qWait(10);
        QThreadPool::globalInstance()->waitForDone(5000);
        ops.reset();
        trash.reset();
        dir.reset();
    }

    void F_EDIT_03_cropOverlayEnterSavesAndUndo() {
        const QString file = path(QStringLiteral("crop.jpg"));
        QVERIFY(pattern(QSize(640, 480)).save(file, "JPEG", 92));
        const QByteArray original = readBytes(file);
        ViewerWindow* w = openAndWait(file);
        QVERIFY(w);
        WindowEditing* editing = WindowEditing::of(w);
        QVERIFY(editing);
        QVERIFY(w->perform(Action::CropImage));
        QVERIFY(editing->cropActive());
        CropOverlay* overlay = editing->cropOverlay();
        QVERIFY(overlay && overlay->isVisible());
        QCOMPARE(overlay->cropRect(), QRect(0, 0, 640, 480)); // starts with the whole image
        QVERIFY(app->menu()->action(Action::CropImage));
        // The overlay follows zoom: the selection stays on the same image pixels.
        overlay->setImageRect(QRectF(96, 64, 320, 240));
        const QRectF before = w->view()->imageToWidget().mapRect(overlay->imageRect());
        w->view()->zoomBy(1.5);
        QVERIFY(w->view()->imageToWidget().mapRect(overlay->imageRect()) != before);
        QCOMPARE(overlay->cropRect(), QRect(96, 64, 320, 240));
        // Aspect presets keep their ratio.
        overlay->setAspect(crop::Aspect::Square);
        QCOMPARE(overlay->cropRect().width(), overlay->cropRect().height());
        overlay->setAspect(crop::Aspect::Free);
        overlay->setImageRect(QRectF(96, 64, 320, 240));
        // Enter saves into the file (undoable); the crop mode ends.
        w->activateWindow();
        QTest::keyClick(w->view(), Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(!editing->cropActive(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(jpeg::frameSize(readBytes(file)).width() >= 320, true, 10000);
        QVERIFY(jpeg::frameSize(readBytes(file)).width() < 640);
        QTRY_VERIFY(w->view()->image() && w->view()->image()->fullSize.width() < 640);
        ops->undo();
        QTRY_COMPARE_WITH_TIMEOUT(readBytes(file), original, 10000);
    }

    void F_EDIT_03_cropSaveAsKeepsOriginalAndEscCancels() {
        const QString file = path(QStringLiteral("orig.png"));
        QVERIFY(pattern(QSize(200, 150)).save(file, "PNG"));
        const QByteArray original = readBytes(file);
        ViewerWindow* w = openAndWait(file);
        QVERIFY(w);
        WindowEditing* editing = WindowEditing::of(w);
        QVERIFY(w->perform(Action::CropImage));
        editing->cropOverlay()->setImageRect(QRectF(10, 20, 100, 50));
        const QString copy = path(QStringLiteral("copy.png"));
        editing->saveCropAs(copy);
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(copy), 10000);
        QCOMPARE(QImage(copy).size(), QSize(100, 50));
        QCOMPARE(readBytes(file), original);
        QTRY_VERIFY(!editing->cropActive());
        // Esc ends the crop without saving.
        QVERIFY(w->perform(Action::CropImage));
        QVERIFY(editing->cropActive());
        w->activateWindow();
        QTest::keyClick(w->view(), Qt::Key_Escape);
        QTRY_VERIFY(!editing->cropActive());
        QCOMPARE(readBytes(file), original);
    }

    void F_EDIT_04_exportDialogRunsAndKeepsSource() {
        const QString file = path(QStringLiteral("export.png"));
        QVERIFY(pattern(QSize(300, 200)).save(file, "PNG"));
        const QByteArray original = readBytes(file);
        ViewerWindow* w = openAndWait(file);
        QVERIFY(w);
        WindowEditing* editing = WindowEditing::of(w);
        QVERIFY(w->perform(Action::ExportImage));
        QTRY_VERIFY(editing->exportDialog());
        ExportDialog* dlg = editing->exportDialog();
        auto* format = dlg->findChild<QComboBox*>(QStringLiteral("lfExportFormat"));
        QVERIFY(format && format->findData(QStringLiteral("jpeg")) >= 0);
        exporting::Options o = dlg->options();
        o.formatId = QStringLiteral("jpeg");
        o.maxLongEdge = 150;
        o.folder = path(QStringLiteral("out"));
        o.pattern = QStringLiteral("{name}-web");
        dlg->setOptions(o);
        QCOMPARE(dlg->previewName(), QStringLiteral("export-web.jpg"));
        dlg->accept();
        const QString out = path(QStringLiteral("out/export-web.jpg"));
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(out), 10000);
        QCOMPARE(QImage(out).size(), QSize(150, 100));
        QCOMPARE(readBytes(file), original);
        // Remembered for the next export.
        QCOMPARE(ExportDialog::optionsFromSettings().pattern, QStringLiteral("{name}-web"));
    }

    void F_INFO_07_overlayTogglesReachTheShader() {
        const QString file = path(QStringLiteral("white.png"));
        QImage white(400, 300, QImage::Format_RGB32);
        white.fill(Qt::white);
        QVERIFY(white.save(file, "PNG"));
        ViewerWindow* w = openAndWait(file);
        QVERIFY(w);
        QTRY_COMPARE(qRed(centerPixel(w)), 255);
        const QRgb plain = centerPixel(w);
        QVERIFY(w->perform(Action::ToggleClipping));
        QVERIFY(w->view()->photoOverlays().clipping);
        app->menu()->refreshState();
        QVERIFY(app->menu()->action(Action::ToggleClipping)->isChecked());
        // Pure white is clipped: drawn red.
        QTRY_VERIFY(qGreen(centerPixel(w)) < 40 && qRed(centerPixel(w)) > 200);
        QVERIFY(w->perform(Action::ToggleClipping));
        QTRY_COMPARE(centerPixel(w), plain); // off: exactly as before
        QVERIFY(w->perform(Action::ToggleFocusPeaking));
        QVERIFY(w->view()->photoOverlays().peaking);
        QVERIFY(w->perform(Action::ToggleFocusPeaking));
        QVERIFY(!w->view()->photoOverlays().active());
    }

    void F_INFO_07_afPointsOverlayFromExifSubjectArea() {
        // A JPEG whose EXIF has a SubjectArea (as iPhones write it): center 100, 75, 40 x 30.
        const QString file = path(QStringLiteral("af.jpg"));
        QByteArray jpeg;
        {
            QBuffer b(&jpeg);
            b.open(QIODevice::WriteOnly);
            pattern(QSize(200, 150)).save(&b, "JPEG");
        }
        QByteArray tiff("MM\0*\0\0\0\x08", 8);
        auto u16 = [&](int v) { tiff.append(char(v >> 8)).append(char(v & 0xFF)); };
        auto u32 = [&](quint32 v) {
            for (int k = 3; k >= 0; --k)
                tiff.append(char((v >> (8 * k)) & 0xFF));
        };
        u16(1); // IFD0: ExifIFD pointer
        u16(0x8769);
        u16(4);
        u32(1);
        u32(26);
        u32(0);
        u16(1); // Exif IFD at 26: SubjectArea (4 SHORT, data at 44)
        u16(0x9214);
        u16(3);
        u32(4);
        u32(44);
        u32(0);
        for (int v : {100, 75, 40, 30})
            u16(v);
        QByteArray app1("\xFF\xE1", 2);
        const int len = int(tiff.size()) + 8;
        app1.append(char(len >> 8)).append(char(len & 0xFF)).append("Exif\0\0", 6).append(tiff);
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(jpeg.left(2) + app1 + jpeg.mid(2));
        f.close();
        ViewerWindow* w = openAndWait(file);
        QVERIFY(w);
        QVERIFY(w->perform(Action::ToggleAfPoints));
        WindowEditing* editing = WindowEditing::of(w);
        QTRY_VERIFY(!editing->afPointsOverlay()->points().isEmpty());
        QCOMPARE(editing->afPointsOverlay()->source(), QStringLiteral("EXIF"));
        QVERIFY(editing->afPointsOverlay()->isVisible());
        QVERIFY(w->statusText().contains(QStringLiteral("EXIF")));
    }

    void F_GRID_04_differenceAndBlendInCompare() {
        const QString a = path(QStringLiteral("a.png")), b = path(QStringLiteral("b.png"));
        QVERIFY(pattern(QSize(320, 240)).save(a, "PNG"));
        QImage changed = pattern(QSize(320, 240));
        changed.setPixel(10, 10, qRgb(255, 0, 255));
        QVERIFY(changed.save(b, "PNG"));
        ViewerWindow* w = openAndWait(a);
        QVERIFY(w);
        OverviewController* overview = w->extras()->overview();
        QVERIFY(overview->compare({a, b}));
        CompareView* cmp = overview->compareView();
        QVERIFY(cmp);
        QTRY_VERIFY(cmp->paneView(0)->image() && cmp->paneView(1)->image());
        QVERIFY(cmp->blendBar()->isVisible());
        cmp->setBlendMode(compare::BlendMode::Difference);
        CompareBlendOverlay* overlay = cmp->blendOverlay();
        QVERIFY(overlay && overlay->isVisible());
        QTRY_VERIFY(overlay->recomputeCount() > 0);
        // Identical regions are black in the difference image.
        const QImage d = overlay->composed();
        QCOMPARE(qRed(d.pixel(d.width() / 2, d.height() / 2)), 0);
        app->menu()->refreshState();
        QVERIFY(app->menu()->command(QStringLiteral("compare:difference"))->isChecked());
        // Blend and wipe follow the slider; zooming the first pane recomputes.
        cmp->setBlendMode(compare::BlendMode::Wipe);
        cmp->setBlendValue(0.25);
        const int n = overlay->recomputeCount();
        cmp->paneView(0)->zoomBy(1.4);
        QTRY_VERIFY(overlay->recomputeCount() > n);
        cmp->setBlendMode(compare::BlendMode::Off);
        QVERIFY(!overlay->isVisible());
        QVERIFY(cmp->paneView(1)->isVisible());
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
    TstM4Edit tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_m4_edit.moc"
