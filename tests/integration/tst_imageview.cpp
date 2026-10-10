// SPDX-License-Identifier: GPL-3.0-or-later
// Rendering tests of the GPU image canvas (SPEC 4.3, 4.5, 7, 12 "Farbtests").
// They read back the QRhiWidget framebuffer, so they need a real GPU platform:
// on macOS the test forces the cocoa platform even if QT_QPA_PLATFORM=offscreen
// (tools/check.sh default) and skips only if no QRhi can be created.

#include "app/ImageCanvas.h"
#include "app/ImageView.h"

#include <QApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QSignalSpy>
#include <QtTest>

#include <memory>

using namespace lf;

Q_DECLARE_METATYPE(lf::InputEvent)

namespace {

DecodedImagePtr makeDecoded(const QImage& img, QSize fullSize = {}, bool preview = false) {
    auto d = std::make_shared<DecodedImage>();
    d->image = img;
    d->fullSize = fullSize.isEmpty() ? img.size() : fullSize;
    d->isPreview = preview;
    return d;
}

QImage solid(QSize size, QColor c, const QColorSpace& cs, QImage::Format fmt = QImage::Format_RGBA8888) {
    QImage img(size, fmt);
    img.fill(c);
    img.setColorSpace(cs);
    return img;
}

// Expected value of `c` (in `src`) after conversion to `dst`, by Qt's own CPU path.
QColor expected(QColor c, const QColorSpace& src, const QColorSpace& dst) {
    QImage img(1, 1, QImage::Format_RGBA8888);
    img.fill(c);
    img.setColorSpace(src);
    return img.convertedToColorSpace(dst).pixelColor(0, 0);
}

bool close(QColor a, QColor b, int tol) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
           std::abs(a.blue() - b.blue()) <= tol;
}

QString str(QColor c) {
    return QStringLiteral("(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
}

} // namespace

class TstImageView : public QObject {
    Q_OBJECT

    std::unique_ptr<ImageView> view;
    ImageCanvas* canvas = nullptr;

    // Renders until all uploads/conversions are done and returns the framebuffer.
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

private slots:
    void initTestCase() {
        qRegisterMetaType<lf::InputEvent>();
        view = std::make_unique<ImageView>();
        view->resize(200, 150);
        view->show();
        QVERIFY(QTest::qWaitForWindowExposed(view.get()));
        canvas = view->findChild<ImageCanvas*>();
        QVERIFY(canvas);
        const QImage probe = canvas->grabFramebuffer();
        if (probe.isNull() || !canvas->isRhiAvailable())
            QSKIP("No QRhi (GPU) available on this platform");
        QVERIFY(canvas->maxTextureSize() >= 4096);
    }

    void init() {
        view->clear();
        view->setRotation(0);
        view->setFlip(false, false);
        view->setBackground(QStringLiteral("dark"));
        view->setUpscaleNearest(false);
        view->setZoomLocked(false);
        view->setOutputColorSpace(QColorSpace(QColorSpace::SRgb));
        view->setZoomMode(ZoomMode::Fit);
        canvas->setMaxTileSize(0);
    }

    void F_COL_01_srgbSolidColor() {
        view->setImage(makeDecoded(solid({64, 64}, QColor(200, 100, 50), QColorSpace::SRgb)), false);
        const QColor c = centerPixel();
        QVERIFY2(close(c, QColor(200, 100, 50), 1), qPrintable(str(c)));
        // Untagged images are treated as sRGB.
        view->setImage(makeDecoded(solid({64, 64}, QColor(10, 220, 130), QColorSpace())), false);
        const QColor u = centerPixel();
        QVERIFY2(close(u, QColor(10, 220, 130), 1), qPrintable(str(u)));
        // ARGB32 premultiplied (BGRA upload path).
        view->setImage(makeDecoded(solid({64, 64}, QColor(30, 60, 240), QColorSpace::SRgb,
                                         QImage::Format_ARGB32_Premultiplied)),
                       false);
        const QColor b = centerPixel();
        QVERIFY2(close(b, QColor(30, 60, 240), 1), qPrintable(str(b)));
    }

    void F_COL_01_srgbOnP3Output() {
        // A P3 display: sRGB red must be converted into P3 code values (not passed through).
        const QColorSpace p3(QColorSpace::DisplayP3);
        view->setOutputColorSpace(p3);
        view->setImage(makeDecoded(solid({64, 64}, QColor(255, 0, 0), QColorSpace::SRgb)), false);
        const QColor want = expected(QColor(255, 0, 0), QColorSpace::SRgb, p3); // ~ (234, 51, 35)
        const QColor c = centerPixel();
        QVERIFY2(close(c, want, 2), qPrintable(str(c) + " vs " + str(want)));
        QVERIFY(c.red() < 245 && c.green() > 30);
    }

    void F_COL_01_p3OnP3OutputNotFlattened() {
        const QColorSpace p3(QColorSpace::DisplayP3);
        view->setOutputColorSpace(p3);
        view->setImage(makeDecoded(solid({64, 64}, QColor(255, 0, 0), p3)), false);
        const QColor c = centerPixel();
        QVERIFY2(close(c, QColor(255, 0, 0), 1), qPrintable(str(c))); // full P3 red
        view->setImage(makeDecoded(solid({64, 64}, QColor(40, 200, 90), p3)), false);
        const QColor g = centerPixel();
        QVERIFY2(close(g, QColor(40, 200, 90), 1), qPrintable(str(g)));
    }

    void F_COL_01_p3OnSrgbOutput() {
        const QColorSpace p3(QColorSpace::DisplayP3);
        view->setImage(makeDecoded(solid({64, 64}, QColor(200, 100, 50), p3)), false);
        const QColor want = expected(QColor(200, 100, 50), p3, QColorSpace::SRgb);
        const QColor c = centerPixel();
        QVERIFY2(close(c, want, 2), qPrintable(str(c) + " vs " + str(want)));
        QVERIFY(c != QColor(200, 100, 50));
    }

    void F_COL_01_otherSpaces_data() {
        QTest::addColumn<int>("space");
        QTest::addColumn<int>("output");
        QTest::newRow("AdobeRGB->sRGB") << int(QColorSpace::AdobeRgb) << int(QColorSpace::SRgb);
        QTest::newRow("ProPhoto->P3") << int(QColorSpace::ProPhotoRgb) << int(QColorSpace::DisplayP3);
        QTest::newRow("Bt2020->sRGB") << int(QColorSpace::Bt2020) << int(QColorSpace::SRgb);
        QTest::newRow("sRGBLinear->P3") << int(QColorSpace::SRgbLinear) << int(QColorSpace::DisplayP3);
    }
    void F_COL_01_otherSpaces() {
        QFETCH(int, space);
        QFETCH(int, output);
        const QColorSpace src(static_cast<QColorSpace::NamedColorSpace>(space));
        const QColorSpace dst(static_cast<QColorSpace::NamedColorSpace>(output));
        view->setOutputColorSpace(dst);
        view->setImage(makeDecoded(solid({64, 64}, QColor(180, 120, 60), src)), false);
        const QColor want = expected(QColor(180, 120, 60), src, dst);
        const QColor c = centerPixel();
        QVERIFY2(close(c, want, 2), qPrintable(str(c) + " vs " + str(want)));
    }

    void F_COL_01_customOutputProfile() {
        // Display profile with a curve the shader has no closed form for: lookup texture path.
        const QColorSpace display(QColorSpace::Primaries::DciP3D65, QColorSpace::TransferFunction::Bt2020);
        QVERIFY(display.isValidTarget());
        view->setOutputColorSpace(display);
        view->setImage(makeDecoded(solid({64, 64}, QColor(180, 120, 60), QColorSpace::SRgb)), false);
        const QColor want = expected(QColor(180, 120, 60), QColorSpace::SRgb, display);
        const QColor c = centerPixel();
        QVERIFY2(close(c, want, 3), qPrintable(str(c) + " vs " + str(want)));
    }

    void F_COL_01_highBitDepthFormats() {
        const QColorSpace p3(QColorSpace::DisplayP3);
        // 16-bit integer (converted to half float on the CPU).
        view->setImage(
            makeDecoded(solid({64, 64}, QColor(200, 100, 50), QColorSpace::SRgb, QImage::Format_RGBA64)),
            false);
        QColor c = centerPixel();
        QVERIFY2(close(c, QColor(200, 100, 50), 1), qPrintable(str(c)));
        // Linear half float in P3 (HDR decode format, SDR range).
        QImage h(64, 64, QImage::Format_RGBA16FPx4);
        h.fill(QColor::fromRgbF(0.5f, 0.25f, 0.1f));
        const QColorSpace p3lin = p3.withTransferFunction(QColorSpace::TransferFunction::Linear);
        h.setColorSpace(p3lin);
        view->setImage(makeDecoded(h), false);
        const QColor want = expected(QColor::fromRgbF(0.5f, 0.25f, 0.1f), p3lin, QColorSpace::SRgb);
        c = centerPixel();
        QVERIFY2(close(c, want, 2), qPrintable(str(c) + " vs " + str(want)));
        // Indexed / grayscale formats are converted.
        QImage g(64, 64, QImage::Format_Grayscale8);
        g.fill(QColor(128, 128, 128));
        view->setImage(makeDecoded(g), false);
        c = centerPixel();
        QVERIFY2(close(c, QColor(128, 128, 128), 1), qPrintable(str(c)));
    }

    void F_COL_01_largeImageConvertedOffThread() {
        // > sync limit: the conversion runs on a worker thread, the GUI stays free.
        QImage big = solid({1200, 1000}, QColor(200, 100, 50), QColorSpace::SRgb, QImage::Format_RGBA64);
        QElapsedTimer t;
        t.start();
        view->setImage(makeDecoded(big), false);
        // Deterministic: the result is applied via the event loop, so right after
        // setImage() the conversion is still pending. The time limit is only a sanity bound.
        QVERIFY(!canvas->isIdle());
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
        const QColor c = centerPixel();
        QVERIFY2(close(c, QColor(200, 100, 50), 1), qPrintable(str(c)));
    }

    // Review finding: changing the output color space released the textures of CPU-converted
    // images before the new conversion was ready, so frames in between were blank.
    void F_COL_02_outputChangeKeepsImageVisible() {
        // Gray color space -> converted on the CPU; > sync limit -> converted off-thread.
        QImage g(1000, 1000, QImage::Format_Grayscale8);
        g.fill(200);
        g.setColorSpace(
            QColorSpace(QColorSpace(QColorSpace::SRgb).whitePoint(), QColorSpace::TransferFunction::SRgb));
        view->setImage(makeDecoded(g), false);
        const QColor before = centerPixel();
        QVERIFY2(close(before, QColor(200, 200, 200), 2), qPrintable(str(before)));
        view->setOutputColorSpace(QColorSpace(QColorSpace::DisplayP3));
        QVERIFY(!canvas->isIdle()); // re-conversion pending (applied via the event loop)
        const QImage fb = canvas->grabFramebuffer();
        const QColor during = fb.pixelColor(fb.width() / 2, fb.height() / 2);
        QVERIFY2(close(during, before, 3), qPrintable(str(during)));
        const QColor after = centerPixel();
        QVERIFY2(close(after, QColor(200, 200, 200), 2), qPrintable(str(after)));
    }

    void F_COL_01_displayColorSpaceAuto() {
        view->setOutputColorSpace(QColorSpace());
        const QColorSpace out = view->outputColorSpace();
        QVERIFY(out.isValid());
        QVERIFY(out.isValidTarget());
        qInfo() << "Display color space:" << out.description() << out.primaries() << out.transferFunction();
    }

    void F_VIEW_09_checkerBehindAlpha() {
        view->setBackground(QStringLiteral("checker"));
        QImage clear(64, 64, QImage::Format_RGBA8888_Premultiplied);
        clear.fill(Qt::transparent);
        view->setImage(makeDecoded(clear), false);
        const QImage fb = grab();
        const double dpr = canvas->devicePixelRatioF();
        // Inside the image both checker colors appear.
        QSet<QRgb> seen;
        const QPoint c(fb.width() / 2, fb.height() / 2);
        for (int y = -int(30 * dpr); y < int(30 * dpr); y += 2)
            for (int x = -int(30 * dpr); x < int(30 * dpr); x += 2)
                seen.insert(fb.pixel(c + QPoint(x, y)) & 0xffffff);
        QVERIFY2(seen.contains(0xffffff) && seen.contains(0xcccccc),
                 qPrintable(QString::number(seen.size())));
        // Outside the image: the plain dark background.
        QCOMPARE(fb.pixelColor(2, 2), QColor(30, 30, 30));

        // Half transparent red over the checker.
        view->setImage(makeDecoded(solid({64, 64}, QColor(255, 0, 0, 128), QColorSpace::SRgb)), false);
        const QImage fb2 = grab();
        const QColor p = fb2.pixelColor(fb2.width() / 2, fb2.height() / 2);
        QVERIFY2(p.red() > 220 && p.green() > 90 && p.green() < 135, qPrintable(str(p)));
    }

    void F_VIEW_09_backgrounds() {
        view->setImage(makeDecoded(solid({16, 16}, QColor(255, 255, 255), QColorSpace::SRgb)), false);
        view->setBackground(QStringLiteral("light"));
        QCOMPARE(grab().pixelColor(1, 1), QColor(236, 236, 236));
        view->setBackground(QStringLiteral("#336699"));
        QCOMPARE(grab().pixelColor(1, 1), QColor(0x33, 0x66, 0x99));
        view->setBackground(QStringLiteral("dark"));
        QCOMPARE(grab().pixelColor(1, 1), QColor(30, 30, 30));
        view->setBackground(QStringLiteral("system"));
        const QColor sys = grab().pixelColor(1, 1);
        QVERIFY(sys == QColor(30, 30, 30) || sys == QColor(236, 236, 236));
    }

    void F_VIEW_06_nearestVsSmoothUpscale() {
        QImage img(8, 8, QImage::Format_RGBA8888);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                img.setPixelColor(x, y, ((x + y) % 2) ? Qt::white : Qt::black);
        img.setColorSpace(QColorSpace::SRgb);
        view->setImage(makeDecoded(img), false);
        view->setZoomFactor(12.0, QPointF(100, 75));
        auto countMid = [&] {
            const QImage fb = grab();
            int mid = 0;
            const QPoint c(fb.width() / 2, fb.height() / 2);
            for (int y = -40; y < 40; ++y)
                for (int x = -40; x < 40; ++x) {
                    const int v = qRed(fb.pixel(c + QPoint(x, y)));
                    mid += (v > 20 && v < 235) ? 1 : 0;
                }
            return mid;
        };
        view->setUpscaleNearest(true);
        const int nearestMid = countMid();
        view->setUpscaleNearest(false);
        const int smoothMid = countMid();
        QVERIFY2(nearestMid == 0, qPrintable(QString::number(nearestMid)));
        QVERIFY2(smoothMid > 1000, qPrintable(QString::number(smoothMid)));
    }

    void F_VIEW_06_minificationAveragesDetail() {
        // 1px black/white stripes shown at ~1/8: high quality minification yields
        // a uniform mid gray (no moiré / aliasing).
        QImage img(1600, 1200, QImage::Format_RGBA8888);
        for (int y = 0; y < img.height(); ++y) {
            auto* line = reinterpret_cast<quint32*>(img.scanLine(y));
            for (int x = 0; x < img.width(); ++x)
                line[x] = (x % 2) ? 0xffffffffu : 0xff000000u;
        }
        img.setColorSpace(QColorSpace::SRgb);
        view->setImage(makeDecoded(img), false);
        const QImage fb = grab();
        const QRect r = QRect(QPoint(0, 0), fb.size())
                            .adjusted(fb.width() / 4, fb.height() / 4, -fb.width() / 4, -fb.height() / 4);
        int lo = 255, hi = 0;
        for (int y = r.top(); y < r.bottom(); y += 3)
            for (int x = r.left(); x < r.right(); ++x) {
                lo = std::min(lo, qRed(fb.pixel(x, y)));
                hi = std::max(hi, qRed(fb.pixel(x, y)));
            }
        QVERIFY2(hi - lo <= 12 && lo > 100 && hi < 155, qPrintable(QStringLiteral("%1..%2").arg(lo).arg(hi)));
    }

    void F_VIEW_07_actualSizeIsPixelExact() {
        // 100 % = one image pixel per device pixel, with exact pixel values.
        QImage img(120, 90, QImage::Format_RGBA8888);
        quint32 seed = 12345;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                img.setPixel(x, y, 0xff000000u | (seed >> 8));
            }
        img.setColorSpace(QColorSpace::SRgb);
        for (bool nearest : {false, true}) {
            view->setUpscaleNearest(nearest);
            view->setImage(makeDecoded(img), false);
            view->setZoomMode(ZoomMode::Actual);
            const QImage fb = grab();
            const QPoint origin((fb.width() - img.width()) / 2, (fb.height() - img.height()) / 2);
            int wrong = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    wrong +=
                        (fb.pixel(origin + QPoint(x, y)) & 0xffffff) != (img.pixel(x, y) & 0xffffff) ? 1 : 0;
            QVERIFY2(wrong == 0, qPrintable(QString::number(wrong)));
        }
    }

    void F_EDIT_01_rotatedRendering() {
        // Left half red, right half blue; after 90° cw rotation red is on top.
        QImage img(64, 32, QImage::Format_RGBA8888);
        img.fill(Qt::blue);
        QPainter(&img).fillRect(0, 0, 32, 32, Qt::red);
        img.setColorSpace(QColorSpace::SRgb);
        view->setImage(makeDecoded(img), false);
        view->setRotation(1);
        QImage fb = grab();
        const QPoint c(fb.width() / 2, fb.height() / 2);
        // Fit without upscaling: 1 image px = 1 device px.
        QCOMPARE(fb.pixelColor(c + QPoint(0, -16)), QColor(255, 0, 0));
        QCOMPARE(fb.pixelColor(c + QPoint(0, 16)), QColor(0, 0, 255));
        view->setRotation(0);
        view->setFlip(true, false);
        fb = grab();
        QCOMPARE(fb.pixelColor(c + QPoint(-16, 0)), QColor(0, 0, 255));
        QCOMPARE(fb.pixelColor(c + QPoint(16, 0)), QColor(255, 0, 0));
    }

    void P_08_tilesMatchSingleTexture() {
        // Smooth gradient rendered with tiny forced tiles vs. one texture: no seams.
        QImage img(1000, 700, QImage::Format_RGBA8888);
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                img.setPixel(x, y, qRgb(x * 255 / 999, y * 255 / 699, (x + y) % 256 < 128 ? 40 : 200));
        img.setColorSpace(QColorSpace::SRgb);
        auto decoded = makeDecoded(img);
        view->setImage(decoded, false);
        const QImage single = grab();
        QCOMPARE(canvas->currentTileCount(), 1);
        canvas->setMaxTileSize(256);
        view->clear();
        view->setImage(decoded, false);
        const QImage tiled = grab();
        QVERIFY(canvas->currentTileCount() > 4);
        int maxDiff = 0;
        for (int y = 0; y < single.height(); ++y)
            for (int x = 0; x < single.width(); ++x) {
                const QRgb a = single.pixel(x, y), b = tiled.pixel(x, y);
                maxDiff = std::max({maxDiff, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
                                    std::abs(qBlue(a) - qBlue(b))});
            }
        QVERIFY2(maxDiff <= 2, qPrintable(QString::number(maxDiff)));
        // Also at 100 % (magnification path) with the view panned across tile borders.
        view->setZoomMode(ZoomMode::Actual);
        const QImage tiledActual = grab();
        canvas->setMaxTileSize(0);
        view->clear();
        view->setImage(decoded, false);
        view->setZoomMode(ZoomMode::Actual);
        const QImage singleActual = grab();
        int maxDiffActual = 0;
        for (int y = 0; y < single.height(); ++y)
            for (int x = 0; x < single.width(); ++x) {
                const QRgb a = singleActual.pixel(x, y), b = tiledActual.pixel(x, y);
                maxDiffActual = std::max({maxDiffActual, std::abs(qRed(a) - qRed(b)),
                                          std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
            }
        if (maxDiffActual > 1 && qEnvironmentVariableIsSet("LF_DUMP")) {
            tiledActual.save(qEnvironmentVariable("LF_DUMP") + "/tiled.png");
            singleActual.save(qEnvironmentVariable("LF_DUMP") + "/single.png");
        }
        QVERIFY2(maxDiffActual <= 1, qPrintable(QString::number(maxDiffActual)));
    }

    void P_08_hugeImageBeyondMaxTextureSize() {
        const int maxTex = canvas->maxTextureSize();
        const QSize size(std::max(20000, maxTex + 1000), 12000);
        QImage huge(size, QImage::Format_ARGB32_Premultiplied);
        if (huge.isNull())
            QSKIP("Not enough memory for the huge test image");
        // Four quadrants with distinct colors.
        const int hw = size.width() / 2, hh = size.height() / 2;
        for (int y = 0; y < size.height(); ++y) {
            auto* line = reinterpret_cast<quint32*>(huge.scanLine(y));
            const bool top = y < hh;
            std::fill(line, line + hw, top ? 0xffff0000u : 0xff00ff00u);
            std::fill(line + hw, line + size.width(), top ? 0xff0000ffu : 0xffffff00u);
        }
        huge.setColorSpace(QColorSpace::SRgb);
        QElapsedTimer t;
        t.start();
        view->setImage(makeDecoded(huge), false);
        // setImage itself does no heavy work: nothing is uploaded until the next frame.
        QVERIFY(!canvas->isIdle());
        QVERIFY2(t.elapsed() < 1000, qPrintable(QString::number(t.elapsed())));
        const QImage fb = grab();
        qInfo() << "huge image" << size << "rendered after" << t.elapsed() << "ms, tiles"
                << canvas->currentTileCount();
        QVERIFY(canvas->currentTileCount() >= 2);
        const QPoint c(fb.width() / 2, fb.height() / 2);
        const int dx = fb.width() / 4, dy = int(fb.width() / 2.0 * size.height() / size.width() / 2);
        QCOMPARE(fb.pixelColor(c + QPoint(-dx, -dy / 2)), QColor(255, 0, 0));
        QCOMPARE(fb.pixelColor(c + QPoint(dx, -dy / 2)), QColor(0, 0, 255));
        QCOMPARE(fb.pixelColor(c + QPoint(-dx, dy / 2)), QColor(0, 255, 0));
        QCOMPARE(fb.pixelColor(c + QPoint(dx, dy / 2)), QColor(255, 255, 0));
        // 100 % at the center: the tile border at maxTex - padding lies left of center.
        view->setZoomMode(ZoomMode::Actual);
        const QImage fb2 = grab();
        QCOMPARE(fb2.pixelColor(2, 2), QColor(255, 0, 0));
        QCOMPARE(fb2.pixelColor(fb2.width() - 3, fb2.height() - 3), QColor(255, 255, 0));
        view->clear();
        canvas->setPreload({});
    }

    void F_FMT_02_errorCardVisible() {
        view->resize(400, 300);
        view->showError(QStringLiteral("Datei kann nicht geöffnet werden"),
                        QStringLiteral("kaputt.jpg: Defekte Datei"));
        QVERIFY(!view->image());
        auto* card = view->findChild<QFrame*>(QStringLiteral("lfErrorCard"));
        QVERIFY(card);
        QVERIFY(card->isVisible());
        const QRect g = card->geometry();
        QVERIFY(qAbs(g.center().x() - view->width() / 2) <= 1);
        QVERIFY(qAbs(g.center().y() - view->height() / 2) <= 1);
        auto* title = card->findChild<QLabel*>(QStringLiteral("lfErrorTitle"));
        QVERIFY(title && title->text().contains(QStringLiteral("geöffnet")));
        // The card is painted over the RHI content: bright text pixels in the card.
        const QImage shot = view->grab().toImage();
        const double r = shot.devicePixelRatio();
        int bright = 0;
        for (int y = int(g.top() * r); y < int(g.bottom() * r); ++y)
            for (int x = int(g.left() * r); x < int(g.right() * r); ++x)
                bright += qGray(shot.pixel(x, y)) > 180 ? 1 : 0;
        QVERIFY2(bright > 50, qPrintable(QString::number(bright)));
        // A new image hides the card.
        view->setImage(makeDecoded(solid({16, 16}, Qt::white, QColorSpace::SRgb)), false);
        QVERIFY(!card->isVisible());
        view->resize(200, 150);
    }

    void F_VIEW_03_stageSwapIsSeamless() {
        // Preview stage (1000x500 of a 4000x2000 image), zoomed in -> needs more.
        QSignalSpy needs(view.get(), &ImageView::needsHigherResolution);
        view->setImage(makeDecoded(solid({1000, 500}, Qt::gray, QColorSpace::SRgb), {4000, 2000}, true),
                       false);
        view->setZoomFactor(0.5, QPointF(60, 40));
        view->panBy(QPointF(-30, 10));
        QCOMPARE(needs.count(), 1);
        const double f = view->zoomFactor();
        const bool zoomed = view->isZoomed();
        const bool left = view->atEdge(Qt::LeftEdge);
        const QImage before = grab();
        view->setImage(makeDecoded(solid({4000, 2000}, Qt::gray, QColorSpace::SRgb), {4000, 2000}), true);
        QCOMPARE(view->zoomFactor(), f);
        QCOMPARE(view->isZoomed(), zoomed);
        QCOMPARE(view->atEdge(Qt::LeftEdge), left);
        QCOMPARE(needs.count(), 1); // full resolution loaded: no further request
        QCOMPARE(grab(), before);   // same geometry, same content
    }

    void F_VIEW_05_zoomLockAcrossImages() {
        view->setImage(makeDecoded(solid({4000, 2000}, Qt::gray, QColorSpace::SRgb)), false);
        view->setZoomFactor(0.5, QPointF(100, 75));
        view->setZoomLocked(true);
        view->setImage(makeDecoded(solid({4000, 2000}, Qt::darkGray, QColorSpace::SRgb)), false);
        QCOMPARE(view->zoomFactor(), 0.5);
        view->setZoomLocked(false);
        view->setImage(makeDecoded(solid({4000, 2000}, Qt::gray, QColorSpace::SRgb)), false);
        QCOMPARE(view->zoomMode(), ZoomMode::Fit);
    }

    void P_03_preloadedSwapIsFast() {
        auto a = makeDecoded(solid({3000, 2000}, Qt::red, QColorSpace::SRgb));
        auto b = makeDecoded(solid({3000, 2000}, Qt::blue, QColorSpace::SRgb));
        view->setImage(a, false);
        view->setPreloadImages({a, b});
        grab();
        for (int i = 0; i < 5; ++i)
            canvas->grabFramebuffer(); // uploads the preloaded neighbour
        QElapsedTimer t;
        t.start();
        view->setImage(b, false);
        const QImage fb = canvas->grabFramebuffer();
        const qint64 ms = t.elapsed();
        QVERIFY(canvas->isIdle()); // nothing left to upload: texture came from the cache
        QCOMPARE(fb.pixelColor(fb.width() / 2, fb.height() / 2), QColor(0, 0, 255));
        qInfo() << "preloaded swap + frame + readback:" << ms << "ms";
        view->setPreloadImages({});
    }

    void F_IN_01_inputForwarding() {
        QSignalSpy spy(view.get(), &ImageView::userInput);
        view->setImage(makeDecoded(solid({100, 100}, Qt::gray, QColorSpace::SRgb)), false);
        // Mouse wheel (angle delta only): one notch down = +1 step.
        QWheelEvent wheel(QPointF(50, 50), view->mapToGlobal(QPointF(50, 50)), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &wheel);
        QCOMPARE(spy.count(), 1);
        auto ev = spy.at(0).at(0).value<lf::InputEvent>();
        QCOMPARE(ev.kind, InputKind::Wheel);
        QCOMPARE(ev.wheelSteps, 1);
        QVERIFY(!ev.zoomed);
        // High resolution wheel: two half notches = one step.
        QWheelEvent half(QPointF(50, 50), view->mapToGlobal(QPointF(50, 50)), QPoint(), QPoint(0, 60),
                         Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &half);
        QCOMPARE(spy.count(), 1);
        QApplication::sendEvent(canvas, &half);
        QCOMPARE(spy.count(), 2);
        ev = spy.at(1).at(0).value<lf::InputEvent>();
        QCOMPARE(ev.wheelSteps, -1);
        QCOMPARE(ev.modifiers, Qt::ControlModifier);
        // Key press
        QTest::keyClick(view.get(), Qt::Key_Right);
        QCOMPARE(spy.count(), 3);
        ev = spy.at(2).at(0).value<lf::InputEvent>();
        QCOMPARE(ev.kind, InputKind::Key);
        QCOMPARE(ev.key, int(Qt::Key_Right));
        QVERIFY(ev.atScrollEdge);
        // Click, double click, middle click
        QTest::mouseClick(canvas, Qt::LeftButton, {}, QPoint(20, 20));
        QCOMPARE(spy.last().at(0).value<lf::InputEvent>().kind, InputKind::Click);
        QTest::mouseDClick(canvas, Qt::LeftButton, {}, QPoint(20, 20));
        QCOMPARE(spy.last().at(0).value<lf::InputEvent>().kind, InputKind::DoubleClick);
        QTest::mouseClick(canvas, Qt::MiddleButton, {}, QPoint(20, 20));
        QCOMPARE(spy.last().at(0).value<lf::InputEvent>().kind, InputKind::MiddleClick);
        QTest::mouseClick(canvas, Qt::BackButton, {}, QPoint(20, 20));
        QCOMPARE(spy.last().at(0).value<lf::InputEvent>().kind, InputKind::BackButton);
        // Trackpad horizontal swipe while not zoomed.
        const int n = spy.count();
        for (int i = 0; i < 5; ++i) {
            QWheelEvent swipe(QPointF(50, 50), view->mapToGlobal(QPointF(50, 50)), QPoint(-20, 0),
                              QPoint(-40, 0), Qt::NoButton, Qt::NoModifier,
                              i == 0 ? Qt::ScrollBegin : Qt::ScrollUpdate, true);
            QApplication::sendEvent(canvas, &swipe);
        }
        QCOMPARE(spy.count(), n + 1);
        ev = spy.last().at(0).value<lf::InputEvent>();
        QCOMPARE(ev.kind, InputKind::Swipe);
        QCOMPARE(ev.swipeDirection, -1);
    }

    void F_VIEW_04_trackpadAndDragPan() {
        QSignalSpy spy(view.get(), &ImageView::userInput);
        QSignalSpy dragOut(view.get(), &ImageView::dragOutRequested);
        view->setImage(makeDecoded(solid({2000, 2000}, Qt::gray, QColorSpace::SRgb)), false);
        view->setZoomMode(ZoomMode::Actual);
        QVERIFY(view->isZoomed());
        QVERIFY(!view->atEdge(Qt::TopEdge));
        // Trackpad scroll pans (no InputEvent).
        for (int i = 0; i < 50; ++i) {
            QWheelEvent scroll(QPointF(50, 50), view->mapToGlobal(QPointF(50, 50)), QPoint(0, 100),
                               QPoint(0, 200), Qt::NoButton, Qt::NoModifier,
                               i == 0 ? Qt::ScrollBegin : Qt::ScrollUpdate, true);
            QApplication::sendEvent(canvas, &scroll);
        }
        QVERIFY(view->atEdge(Qt::TopEdge));
        QCOMPARE(spy.count(), 0);
        // Drag pans towards the bottom-right edge.
        QTest::mousePress(canvas, Qt::LeftButton, {}, QPoint(150, 120));
        for (int i = 1; i <= 20; ++i)
            QTest::mouseMove(canvas, QPoint(150 - i * 7, 120 - i * 6));
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, QPoint(10, 0));
        QVERIFY(!view->atEdge(Qt::TopEdge));
        QCOMPARE(spy.count(), 0); // a drag is not a click
        // ⌥-drag requests drag-out.
        QTest::mousePress(canvas, Qt::LeftButton, Qt::AltModifier, QPoint(50, 50));
        QTest::mouseMove(canvas, QPoint(90, 90));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::AltModifier, QPoint(90, 90));
        QCOMPARE(dragOut.count(), 1);
        // Pinch zoom
        const double f = view->zoomFactor();
        QNativeGestureEvent pinch(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                                  QPointF(50, 50), QPointF(50, 50), view->mapToGlobal(QPointF(50, 50)), 0.5,
                                  QPointF());
        QApplication::sendEvent(canvas, &pinch);
        QVERIFY2(qAbs(view->zoomFactor() - f * 1.5) < 1e-6,
                 qPrintable(QStringLiteral("%1 -> %2").arg(f).arg(view->zoomFactor())));
        QNativeGestureEvent smart(Qt::SmartZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                                  QPointF(50, 50), QPointF(50, 50), view->mapToGlobal(QPointF(50, 50)), 0,
                                  QPointF());
        QApplication::sendEvent(canvas, &smart);
        QCOMPARE(view->zoomMode(), ZoomMode::Actual);
    }
};

int main(int argc, char* argv[]) {
#ifdef Q_OS_MACOS
    // The canvas needs Metal; the offscreen platform has no QRhi.
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen")
        qputenv("QT_QPA_PLATFORM", "cocoa");
#endif
    QApplication app(argc, argv);
    TstImageView tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_imageview.moc"
