// SPDX-License-Identifier: GPL-3.0-or-later
// HDR in real windows (SPEC 4.5, 8.4): SDR output unchanged by the HDR mapping, HDR tone
// mapping in the widget (SDR) pass against the CPU reference (core/HdrMapping), exposure,
// the extended range (EDR) surface with its overlay compositing (EDR capable Mac only), the
// HDR toggle with its status bar badge and the exposure HUD.
// Needs the GPU (cocoa platform, like tst_imageview); LF_NO_EDR=0 (tests/CMakeLists.txt).

#include "app/Application.h"
#include "app/EdrDisplay.h"
#include "app/HdrController.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "app/Loupe.h"
#include "app/RecentItems.h"
#include "app/ViewerWindow.h"
#include "core/FileOps.h"
#include "core/HdrMapping.h"
#include "core/InputBindings.h"
#include "core/Platform.h"
#include "core/Settings.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorSpace>
#include <QColorTransform>
#include <QDir>
#include <QFile>
#include <QFloat16>
#include <QFrame>
#include <QLabel>
#include <QRgbaFloat>
#include <QSlider>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>

#include <cmath>
#include <memory>

using namespace lf;

namespace {

DecodedImagePtr makeDecoded(const QImage& img, float headroom = 1.0f) {
    auto d = std::make_shared<DecodedImage>();
    d->image = img;
    d->fullSize = img.size();
    d->headroom = headroom;
    return d;
}

QColorSpace linearP3() {
    return QColorSpace(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Linear);
}

// Linear Display P3 half float image of one color (the HDR decode format).
QImage hdrSolid(QSize size, float r, float g, float b) {
    QImage img(size, QImage::Format_RGBA16FPx4);
    for (int y = 0; y < img.height(); ++y) {
        auto* p = reinterpret_cast<qfloat16*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x, p += 4) {
            p[0] = qfloat16(r);
            p[1] = qfloat16(g);
            p[2] = qfloat16(b);
            p[3] = qfloat16(1.0f);
        }
    }
    img.setColorSpace(linearP3());
    return img;
}

// CPU reference of the widget pass for an HDR pixel: linear P3 -> linear sRGB, exposure,
// tone curve to SDR white, clamp, sRGB encoding.
QColor referenceSdr(float r, float g, float b, float contentPeak, float exposure) {
    const QColorTransform toLinearSrgb = linearP3().transformationToColorSpace(
        QColorSpace(QColorSpace::SRgb).withTransferFunction(QColorSpace::TransferFunction::Linear));
    const QRgbaFloat32 lin = toLinearSrgb.map(QRgbaFloat32{r, g, b, 1.0f});
    const hdr::ToneCurve curve = hdr::makeToneCurve(contentPeak * exposure, 1.0f);
    auto rgb = hdr::toneMapRgb(curve, {lin.r * exposure, lin.g * exposure, lin.b * exposure});
    auto enc = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    };
    return QColor::fromRgbF(enc(rgb[0]), enc(rgb[1]), enc(rgb[2]));
}

bool close(QColor a, QColor b, int tol) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
           std::abs(a.blue() - b.blue()) <= tol;
}

QString str(QColor c) {
    return QStringLiteral("(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

class NullShell : public Shell {
public:
    void revealInFileManager(const QString&) override {}
    void openWith(const QString&, const QString&) override {}
    QStringList imageDocumentTypes() const override { return {"public.jpeg"}; }
    bool makeDefaultViewer(const QStringList&, QString*) override { return true; }
};

class NoTrash : public TrashService {
public:
    std::optional<QString> moveToTrash(const QString&, QString* error) override {
        if (error)
            *error = QStringLiteral("no trash in this test");
        return std::nullopt;
    }
    bool restoreFromTrash(const QString&, const QString&, QString*) override { return false; }
};

} // namespace

class TstHdr : public QObject {
    Q_OBJECT

    std::unique_ptr<ImageView> view;
    ImageCanvas* canvas = nullptr;
    QTemporaryDir m_settingsDir;

    QImage grab() {
        QImage fb;
        for (int i = 0; i < 400; ++i) {
            fb = canvas->grabFramebuffer();
            if (canvas->isIdle() && !fb.isNull())
                return canvas->grabFramebuffer();
            QTest::qWait(5);
        }
        return fb;
    }
    QColor centerPixel() {
        const QImage fb = grab();
        return fb.pixelColor(fb.width() / 2, fb.height() / 2);
    }
    bool edrDisplay() const {
        return edrPresentationAvailable() &&
               edrDisplayState(view.get()).potentialHeadroom > hdr::kHdrThreshold;
    }

private slots:
    void initTestCase() {
        Settings::useTestingBackend(m_settingsDir.filePath(QStringLiteral("hdr.ini")));
        RecentItems::setSystemRegistrationEnabled(false);
        view = std::make_unique<ImageView>();
        view->resize(200, 150);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view.get()));
        canvas = view->findChild<ImageCanvas*>();
        QVERIFY(canvas);
        const QImage probe = canvas->grabFramebuffer();
        if (probe.isNull() || !canvas->isRhiAvailable())
            QSKIP("No QRhi (GPU) available on this platform");
    }

    void init() {
        ImageView::setDisplayStateForTesting({});
        view->clear();
        view->setHdrEnabled(true);
        view->setExposureLocked(false);
        view->setExposureEv(0.0);
        view->setBackground(QStringLiteral("dark"));
        view->setOutputColorSpace(QColorSpace(QColorSpace::SRgb));
        view->setZoomMode(ZoomMode::Fit);
    }

    void cleanup() { ImageView::setDisplayStateForTesting({}); }

    // SDR content: the HDR mapping (exposure, extended headroom, toggle) never changes the
    // widget's pixels - the output equals the previous renderer (Qt's CPU conversion).
    void F_COL_03_sdrPixelEqualityVsPreviousRenderer() {
        ImageView::setDisplayStateForTesting([](const QWidget*) { return hdr::DisplayState{3.0f, 8.0f}; });
        for (const QColorSpace& cs : {QColorSpace(QColorSpace::SRgb), QColorSpace(QColorSpace::DisplayP3),
                                      QColorSpace(QColorSpace::AdobeRgb)}) {
            QImage img(64, 64, QImage::Format_RGBA8888);
            img.fill(QColor(200, 100, 50));
            img.setColorSpace(cs);
            view->setImage(makeDecoded(img), false);
            QVERIFY(!view->isExtendedPresentationActive());
            QVERIFY(!view->hdrDecision().badge);
            const QImage before = grab();
            view->setExposureEv(2.0); // has no effect on SDR images
            view->setHdrEnabled(false);
            canvas->setHdrMapping(1.0f, 8.0f);
            const QImage after = grab();
            QCOMPARE(after, before);
            // Same as Qt's CPU conversion into the output space (the pre-HDR renderer).
            const QColor want = img.convertedToColorSpace(QColorSpace(QColorSpace::SRgb)).pixelColor(32, 32);
            const QColor got = after.pixelColor(after.width() / 2, after.height() / 2);
            QVERIFY2(close(got, want, 1), qPrintable(str(got) + " vs " + str(want)));
            view->setHdrEnabled(true);
            view->setExposureEv(0.0);
        }
        // Identity fast path: an sRGB image on an sRGB output is passed through unchanged.
        QImage flat(64, 64, QImage::Format_RGBA8888);
        flat.fill(QColor(201, 99, 13));
        flat.setColorSpace(QColorSpace::SRgb);
        view->setImage(makeDecoded(flat), false);
        const QColor p = centerPixel();
        QVERIFY2(p == QColor(201, 99, 13), qPrintable(str(p)));
    }

    // HDR content in the widget pass (SDR preview / no EDR): BT.2390 tone curve as in
    // core/HdrMapping, highlights compressed below SDR white instead of clipped.
    void F_COL_03_hdrToneMappedInSdrPass() {
        ImageView::setDisplayStateForTesting([](const QWidget*) { return hdr::DisplayState{1.0f, 1.0f}; });
        struct Case {
            float r, g, b, peak;
        };
        for (const Case& c :
             {Case{4.0f, 2.0f, 1.0f, 4.0f}, Case{0.5f, 0.25f, 0.1f, 4.0f}, Case{8.0f, 8.0f, 8.0f, 8.0f}}) {
            view->setImage(makeDecoded(hdrSolid({64, 64}, c.r, c.g, c.b), c.peak), false);
            QCOMPARE(view->hdrDecision().presentation, hdr::Presentation::ToneMapped);
            QVERIFY(view->hdrDecision().badge);
            QVERIFY(!view->isExtendedPresentationActive());
            const QColor got = centerPixel();
            const QColor want = referenceSdr(c.r, c.g, c.b, c.peak, 1.0f);
            QVERIFY2(close(got, want, 3), qPrintable(str(got) + " vs " + str(want)));
        }
        // Brighter content stays brighter (monotonic), nothing hard-clips to the same value.
        view->setImage(makeDecoded(hdrSolid({64, 64}, 2.0f, 2.0f, 2.0f), 8.0f), false);
        const QColor mid = centerPixel();
        view->setImage(makeDecoded(hdrSolid({64, 64}, 6.0f, 6.0f, 6.0f), 8.0f), false);
        const QColor high = centerPixel();
        QVERIFY2(high.red() > mid.red(), qPrintable(str(mid) + " " + str(high)));
    }

    // F-COL-05: the exposure scales HDR content (display only) and resets per image.
    void F_COL_05_exposureScalesHdrContent() {
        ImageView::setDisplayStateForTesting([](const QWidget*) { return hdr::DisplayState{1.0f, 1.0f}; });
        view->setImage(makeDecoded(hdrSolid({64, 64}, 0.2f, 0.1f, 0.05f), 2.0f), false);
        QVERIFY(view->exposureApplies());
        view->setExposureEv(1.0);
        const QColor got = centerPixel();
        const QColor want = referenceSdr(0.2f, 0.1f, 0.05f, 2.0f, 2.0f);
        QVERIFY2(close(got, want, 3), qPrintable(str(got) + " vs " + str(want)));
        // Another file: back to 0 EV; locked: kept.
        view->setImage(makeDecoded(hdrSolid({64, 64}, 0.2f, 0.1f, 0.05f), 2.0f), false);
        QCOMPARE(view->exposureEv(), 0.0);
        view->setExposureEv(-1.0);
        view->setExposureLocked(true);
        view->setImage(makeDecoded(hdrSolid({64, 64}, 0.3f, 0.1f, 0.05f), 2.0f), false);
        QCOMPARE(view->exposureEv(), -1.0);
        // A higher quality stage of the same file keeps the exposure in any case.
        view->setExposureLocked(false);
        view->setImage(makeDecoded(hdrSolid({64, 64}, 0.3f, 0.1f, 0.05f), 2.0f), true);
        QCOMPARE(view->exposureEv(), -1.0);
    }

    // F-COL-03: on an EDR capable display HDR images go through the extended range surface
    // (extended linear Display P3 swapchain); widgets above the image are composited into it.
    void F_COL_03_edrSwapchainSetupAndOverlay() {
        if (!edrDisplay())
            QSKIP("No EDR capable display (or not the cocoa platform)");
        view->setImage(makeDecoded(hdrSolid({64, 64}, 4.0f, 2.0f, 1.0f), 4.0f), false);
        QCOMPARE(view->hdrDecision().presentation, hdr::Presentation::Extended);
        QTRY_VERIFY(view->isExtendedPresentationActive());
        auto* surface = view->findChild<QWidget*>(QStringLiteral("lfEdrSurface"));
        QVERIFY(surface);
        QTRY_VERIFY(surface->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(canvas->extendedFramesRendered() > 0, 5000);
        QVERIFY(canvas->extendedPresentation());
        QVERIFY(canvas->extendedHeadroom() >= 1.0f);
        // An overlay widget (toast-like) appears in the extended pass.
        auto* label = new QLabel(QStringLiteral("Overlay"), view.get());
        label->setStyleSheet(QStringLiteral("background: rgba(0, 0, 0, 160); color: white;"));
        label->move(10, 10);
        label->show();
        QTRY_VERIFY(canvas->hasExtendedOverlay());
        const int frames = canvas->extendedFramesRendered();
        label->setText(QStringLiteral("Changed"));
        QTRY_VERIFY(canvas->extendedFramesRendered() > frames);
        label->hide();
        QTRY_VERIFY(!canvas->hasExtendedOverlay());
        delete label;
        // Toggle off: SDR preview through the widget again.
        view->setHdrEnabled(false);
        QVERIFY(!view->isExtendedPresentationActive());
        QTRY_VERIFY(!surface->isVisible());
        const QColor got = centerPixel();
        const QColor want = referenceSdr(4.0f, 2.0f, 1.0f, 4.0f, 1.0f);
        QVERIFY2(close(got, want, 3), qPrintable(str(got) + " vs " + str(want)));
        // An SDR image never uses the surface.
        view->setHdrEnabled(true);
        QImage sdr(64, 64, QImage::Format_RGBA8888);
        sdr.fill(Qt::gray);
        view->setImage(makeDecoded(sdr), false);
        QVERIFY(!view->isExtendedPresentationActive());
    }

    // Review 5: the overlay travels as patches (no full-view copy per change, the painting
    // buffers stay unshared) and widgets of the window stacked above the view without being
    // its children (quick filter bar on the central widget, F-NAV-08) are composited too.
    void F_COL_03_edrOverlayPatchesAndWidgetsAboveView() {
        if (!edrDisplay())
            QSKIP("No EDR capable display (or not the cocoa platform)");
        QWidget host;
        host.resize(240, 180);
        auto* v = new ImageView(&host);
        v->setGeometry(0, 0, 240, 180);
        auto* bar = new QLabel(QStringLiteral("Filter"), &host); // sibling above the view
        bar->setStyleSheet(QStringLiteral("background: rgba(28, 28, 28, 230); color: white;"));
        bar->setGeometry(100, 10, 120, 24);
        bar->hide();
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        auto* c = v->findChild<ImageCanvas*>();
        QVERIFY(c);
        v->setImage(makeDecoded(hdrSolid({64, 64}, 4.0f, 2.0f, 1.0f), 4.0f), false);
        QTRY_VERIFY(v->isExtendedPresentationActive());
        QTRY_VERIFY_WITH_TIMEOUT(c->extendedFramesRendered() > 0, 5000);
        QVERIFY(!c->hasExtendedOverlay());
        bar->show();
        bar->raise();
        QTRY_VERIFY(c->hasExtendedOverlay()); // not a child of the view, still grabbed
        const int patches = c->extendedOverlayPatches();
        for (int i = 0; i < 5; ++i) {
            bar->move(100 - 10 * i, 10 + 5 * i);
            QTRY_VERIFY(c->extendedOverlayPatches() > patches + i);
            QVERIFY(!c->overlayBufferShared());
        }
        // Moved off the view: nothing above the image any more.
        bar->move(300, 300);
        bar->hide();
        QTRY_VERIFY(!c->hasExtendedOverlay());
    }

    // F-VIEW-11 with F-COL-03/05: the loupe shows the image with the exposure and tone
    // mapping of the view (widget pass reference), not the unmapped stage.
    void F_VIEW_11_loupeAppliesExposureAndToneMapping() {
        ImageView::setDisplayStateForTesting([](const QWidget*) { return hdr::DisplayState{1.0f, 1.0f}; });
        view->setImage(makeDecoded(hdrSolid({64, 64}, 3.0f, 1.5f, 0.5f), 4.0f), false);
        view->setExposureEv(-1.0);
        LoupeWidget loupe(view.get(), view.get());
        loupe.setGeometry(view->rect());
        loupe.setCursorPos(QPointF(view->width() / 2.0, view->height() / 2.0));
        loupe.show();
        const QImage shot = loupe.grab().toImage();
        const QPointF center = loupe.loupeGeometry().target.center() * shot.devicePixelRatio();
        const QColor got = shot.pixelColor(center.toPoint());
        const QColor want = referenceSdr(3.0f, 1.5f, 0.5f, 4.0f, 0.5f);
        QVERIFY2(close(got, want, 3), qPrintable(str(got) + " vs " + str(want)));
        // The same as the view's own pixels around the loupe.
        loupe.hide();
        const QColor around = centerPixel();
        QVERIFY2(close(got, around, 3), qPrintable(str(got) + " vs " + str(around)));
    }

    // The decision follows the display: no EDR -> SDR preview, EDR -> extended.
    void F_COL_03_headroomFollowsDisplay() {
        float current = 1.0f;
        ImageView::setDisplayStateForTesting(
            [&current](const QWidget*) { return hdr::DisplayState{current, 8.0f}; });
        view->setImage(makeDecoded(hdrSolid({64, 64}, 4.0f, 4.0f, 4.0f), 4.0f), false);
        QCOMPARE(view->hdrDecision().targetHeadroom, 1.0f);
        current = 3.0f; // brightness changed: picked up by the poll
        QTRY_COMPARE(view->hdrDecision().targetHeadroom, 3.0f);
        QCOMPARE(canvas->extendedHeadroom(), 3.0f);
        ImageView::setDisplayStateForTesting([](const QWidget*) { return hdr::DisplayState{1.0f, 1.0f}; });
        view->setImage(makeDecoded(hdrSolid({64, 64}, 4.0f, 4.0f, 4.0f), 4.0f), false);
        QCOMPARE(view->hdrDecision().presentation, hdr::Presentation::ToneMapped);
        QVERIFY(!view->isExtendedPresentationActive());
    }

    // F-COL-04: the toggle (⇧⌘H) switches HDR <-> SDR preview in every window and is
    // remembered; HDR images carry a badge in the status bar. F-COL-05: exposure HUD.
    void F_COL_04_toggleBadgeAndExposureHud() {
        const QString exr = QStringLiteral(LF_CORPUS_DIR "/generated/hdr.exr");
        if (!QFileInfo::exists(exr))
            QSKIP("hdr.exr not in the corpus (tests/corpus/make_corpus.sh)");
        QTemporaryDir dir;
        QVERIFY(QFile::copy(exr, dir.filePath(QStringLiteral("a.exr"))));
        QVERIFY(QFile::copy(exr, dir.filePath(QStringLiteral("b.exr"))));
        NoTrash trash;
        FileOps ops(trash);
        NullShell shell;
        AppServices services;
        services.fileOps = &ops;
        services.shell = &shell;
        services.openUrl = [](const QUrl&) { return true; };
        auto app = std::make_unique<Application>(services);
        const QString a = QDir::cleanPath(dir.filePath(QStringLiteral("a.exr")));
        ViewerWindow* w = app->openPaths({a});
        QVERIFY(w);
        QVERIFY(QTest::qWaitForWindowExposed(w));
        QTRY_COMPARE_WITH_TIMEOUT(w->shownPath(), a, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(w->view()->image() && w->view()->hdrDecision().badge, 5000);
        HdrController* hdrc = w->hdr();
        QVERIFY(hdrc);
        auto* badge = w->findChild<QLabel*>(QStringLiteral("lfStatusHdr"));
        QVERIFY(badge);
        QTRY_VERIFY(!badge->text().isEmpty());
        QVERIFY(badge->text().startsWith(QStringLiteral("HDR")));

        // Toggle: setting, view and badge follow.
        const bool before = Settings::instance().value(key::HdrEnabled).toBool();
        app->perform(Action::ToggleHdr);
        QCOMPARE(Settings::instance().value(key::HdrEnabled).toBool(), !before);
        QCOMPARE(w->view()->hdrEnabled(), !before);
        if (!w->view()->hdrEnabled()) {
            QCOMPARE(w->view()->hdrDecision().presentation, hdr::Presentation::ToneMapped);
            QVERIFY(!w->view()->isExtendedPresentationActive());
            QCOMPARE(badge->text(), hdrc->badgeText());
            QVERIFY(badge->text() != QStringLiteral("HDR"));
        }
        app->perform(Action::ToggleHdr);
        QCOMPARE(w->view()->hdrEnabled(), before);

        // Exposure keys and HUD.
        app->perform(Action::ExposureIncrease);
        QVERIFY(std::abs(w->view()->exposureEv() - 1.0 / 3.0) < 0.01);
        app->perform(Action::ExposureReset);
        QCOMPARE(w->view()->exposureEv(), 0.0);
        app->perform(Action::ToggleExposureHud);
        QVERIFY(hdrc->hudRequested());
        QTRY_VERIFY(hdrc->isHudVisible());
        auto* slider = hdrc->hud()->findChild<QSlider*>(QStringLiteral("lfExposureSlider"));
        QVERIFY(slider);
        slider->setValue(6); // +2 EV
        QCOMPARE(w->view()->exposureEv(), 2.0);
        QVERIFY(badge->text().contains(QStringLiteral("EV")));
        // Next image: reset (not locked).
        app->perform(Action::NextImage);
        QTRY_VERIFY_WITH_TIMEOUT(w->shownPath().endsWith(QStringLiteral("b.exr")), 5000);
        QTRY_COMPARE(w->view()->exposureEv(), 0.0);
        QCOMPARE(slider->value(), 0);
        // Locked via the HUD: kept on the next image.
        auto* lock = hdrc->hud()->findChild<QCheckBox*>(QStringLiteral("lfExposureLock"));
        QVERIFY(lock);
        slider->setValue(-3);
        lock->setChecked(true);
        app->perform(Action::PreviousImage);
        QTRY_VERIFY_WITH_TIMEOUT(w->shownPath().endsWith(QStringLiteral("a.exr")), 5000);
        QCOMPARE(w->view()->exposureEv(), -1.0);
        app->perform(Action::ToggleExposureHud);
        QVERIFY(!hdrc->isHudVisible());
        app.reset();
        QThreadPool::globalInstance()->waitForDone(5000);
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
    TstHdr tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_hdr.moc"
