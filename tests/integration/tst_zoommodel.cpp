// SPDX-License-Identifier: GPL-3.0-or-later
// Pure geometry of the image canvas (SPEC 4.3): zoom modes, steps, anchor zoom,
// pan clamping, zoom lock, Retina scale, rotation/flip, decode size.

#include "app/ZoomModel.h"

#include <QtTest>

using namespace lf;

namespace {

bool fuzzy(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}
bool fuzzyPt(QPointF a, QPointF b, double eps = 1e-6) {
    return fuzzy(a.x(), b.x(), eps) && fuzzy(a.y(), b.y(), eps);
}

ZoomModel makeModel(QSize viewport, QSize image, double dpr = 1.0) {
    ZoomModel m;
    m.setDevicePixelRatio(dpr);
    m.setViewportSize(viewport);
    m.setImageSize(image);
    return m;
}

} // namespace

class TstZoomModel : public QObject {
    Q_OBJECT
private slots:
    void F_VIEW_01_fitDoesNotUpscale() {
        ZoomModel big = makeModel({1000, 800}, {4000, 2000});
        QCOMPARE(big.mode(), ZoomMode::Fit);
        QVERIFY(fuzzy(big.scale(), 0.25));
        QCOMPARE(big.imageRectInViewport(), QRectF(0, 150, 1000, 500)); // centered

        ZoomModel small = makeModel({1000, 800}, {200, 100});
        QVERIFY(fuzzy(small.scale(), 1.0)); // no upscaling of small images
        QCOMPARE(small.imageRectInViewport(), QRectF(400, 350, 200, 100));
        QVERIFY(!small.isZoomed());
    }

    void F_VIEW_01_allModes() {
        ZoomModel m = makeModel({1000, 800}, {200, 100});
        m.setMode(ZoomMode::FitUpscale);
        QVERIFY(fuzzy(m.scale(), 5.0));
        m.setMode(ZoomMode::Actual);
        QVERIFY(fuzzy(m.scale(), 1.0));
        QVERIFY(fuzzy(m.factor(), 1.0));
        m.setMode(ZoomMode::FillWidth);
        QVERIFY(fuzzy(m.scale(), 5.0));
        m.setMode(ZoomMode::FillHeight);
        QVERIFY(fuzzy(m.scale(), 8.0));
        m.setMode(ZoomMode::Fill);
        QVERIFY(fuzzy(m.scale(), 8.0));
        QVERIFY(m.isZoomed()); // fill crops the wider dimension

        ZoomModel big = makeModel({1000, 800}, {4000, 2000});
        big.setMode(ZoomMode::FillWidth);
        QVERIFY(fuzzy(big.scale(), 0.25));
        big.setMode(ZoomMode::FillHeight);
        QVERIFY(fuzzy(big.scale(), 0.4));
        big.setMode(ZoomMode::Fill);
        QVERIFY(fuzzy(big.scale(), 0.4));
        QCOMPARE(big.imageRectInViewport().center(), QPointF(500, 400));

        // Fill width of a tall image starts at the top.
        ZoomModel tall = makeModel({1000, 800}, {1000, 4000});
        tall.setMode(ZoomMode::FillWidth);
        QVERIFY(fuzzy(tall.scale(), 1.0));
        QCOMPARE(tall.offset().y(), 0.0);
        QVERIFY(tall.atEdge(Qt::TopEdge));
        QVERIFY(!tall.atEdge(Qt::BottomEdge));
    }

    void F_VIEW_01_resizeKeepsMode() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        m.setViewportSize({2000, 800});
        QVERIFY(fuzzy(m.scale(), 0.4));
        QCOMPARE(m.mode(), ZoomMode::Fit);
    }

    void F_VIEW_02_stepZoom() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        QVERIFY(fuzzy(m.factor(), 0.25));
        m.zoomStep(1, {500, 400});
        QVERIFY(fuzzy(m.factor(), 1.0 / 3));
        QCOMPARE(m.mode(), ZoomMode::Custom);
        m.zoomStep(3, {500, 400}); // 1/2, 2/3, 1
        QVERIFY(fuzzy(m.factor(), 1.0));
        QCOMPARE(m.mode(), ZoomMode::Actual);
        m.zoomStep(1, {500, 400});
        QVERIFY(fuzzy(m.factor(), 1.5));
        m.zoomStep(-2, {500, 400});
        QVERIFY(fuzzy(m.factor(), 2.0 / 3));
        // Ladder clamps at the ends.
        QCOMPARE(ZoomModel::nextLadderFactor(64.0, 1), 64.0);
        QCOMPARE(ZoomModel::nextLadderFactor(0.01, -1), 0.01);
        // Between ladder values: next value in the direction.
        QCOMPARE(ZoomModel::nextLadderFactor(0.9, 1), 1.0);
        QCOMPARE(ZoomModel::nextLadderFactor(0.9, -1), 2.0 / 3);
        // Zoom at an anchor keeps the image point under the anchor.
        ZoomModel a = makeModel({1000, 800}, {4000, 2000});
        a.setMode(ZoomMode::Actual);
        const QPointF anchor(300, 200);
        const QPointF before = a.viewportToImage(anchor);
        a.zoomBy(1.7, anchor);
        QVERIFY(fuzzyPt(a.viewportToImage(anchor), before, 1e-6));
    }

    void F_VIEW_03_toggleAtCursorKeepsPoint() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        const QPointF cursor(700, 300);
        const QPointF before = m.viewportToImage(cursor);
        m.toggleFitActual(cursor);
        QCOMPARE(m.mode(), ZoomMode::Actual);
        QVERIFY(fuzzy(m.scale(), 1.0));
        // Offsets snap to whole device pixels at 100 %: allow half a pixel.
        QVERIFY(fuzzyPt(m.viewportToImage(cursor), before, 0.5));
        m.toggleFitActual(cursor);
        QCOMPARE(m.mode(), ZoomMode::Fit);
        QVERIFY(fuzzy(m.scale(), 0.25));
    }

    void F_VIEW_04_panClamping() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        m.setMode(ZoomMode::Actual);
        QVERIFY(m.isZoomed());
        m.panBy({1e6, 1e6});
        QCOMPARE(m.offset(), QPointF(0, 0));
        QVERIFY(m.atEdge(Qt::LeftEdge));
        QVERIFY(m.atEdge(Qt::TopEdge));
        QVERIFY(!m.atEdge(Qt::RightEdge));
        QVERIFY(!m.atEdge(Qt::BottomEdge));
        m.panBy({-1e6, -1e6});
        QCOMPARE(m.offset(), QPointF(-3000, -1200));
        QVERIFY(m.atEdge(Qt::RightEdge));
        QVERIFY(m.atEdge(Qt::BottomEdge));
        m.scrollToTop();
        QCOMPARE(m.offset().y(), 0.0);
        m.scrollToBottom();
        QCOMPARE(m.offset().y(), -1200.0);

        // Smaller than the viewport: stays centered, cannot be dragged.
        ZoomModel s = makeModel({1000, 800}, {200, 100});
        s.panBy({50, -70});
        QCOMPARE(s.offset(), QPointF(400, 350));
        QVERIFY(s.atEdge(Qt::LeftEdge) && s.atEdge(Qt::RightEdge));

        // Wide image: horizontal pan only, vertically centered.
        ZoomModel w = makeModel({1000, 800}, {4000, 400});
        w.setMode(ZoomMode::Actual);
        w.panBy({-100, -100});
        QCOMPARE(w.offset().y(), 200.0);
    }

    void F_VIEW_05_zoomLockKeepsFactorAndCenter() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        m.setFactor(0.5, {500, 400});
        m.panBy({600, 0}); // look further left
        const QSizeF d = m.displayedImageSize();
        const QPointF rel = m.viewportToImage({500, 400});
        const QPointF relN(rel.x() / d.width(), rel.y() / d.height());
        m.setZoomLocked(true);
        m.setImageSize({6000, 3000});
        QVERIFY(fuzzy(m.factor(), 0.5));
        QCOMPARE(m.mode(), ZoomMode::Custom);
        const QPointF c = m.viewportToImage({500, 400});
        QVERIFY(fuzzyPt(QPointF(c.x() / 6000, c.y() / 3000), relN, 1e-6));

        // Unlocked: back to the base mode (Fit).
        m.setZoomLocked(false);
        m.setImageSize({4000, 2000});
        QCOMPARE(m.mode(), ZoomMode::Fit);
        QVERIFY(fuzzy(m.scale(), 0.25));
    }

    void F_VIEW_07_devicePixelRatio2() {
        // 1000x800 points on a Retina display = 2000x1600 device px.
        ZoomModel m = makeModel({2000, 1600}, {1000, 500}, 2.0);
        m.setMode(ZoomMode::Actual);
        QVERIFY(fuzzy(m.scale(), 1.0)); // 1 image px = 1 device px
        QCOMPARE(m.imageRectInViewport().size(), QSizeF(1000, 500));
        QVERIFY(fuzzy(m.factor(), 1.0));
        // Fit does not upscale beyond 100 % (device pixels).
        m.setMode(ZoomMode::Fit);
        QVERIFY(fuzzy(m.scale(), 1.0));

        m.setActualSizeInDevicePixels(false); // 100 % = 1 image px per point
        m.setMode(ZoomMode::Actual);
        QVERIFY(fuzzy(m.scale(), 2.0));
        QVERIFY(fuzzy(m.factor(), 1.0));
        m.setMode(ZoomMode::Fit);
        QVERIFY(fuzzy(m.scale(), 2.0)); // fits (2000/1000 = 2) and 100 % in points = 2
        QVERIFY(fuzzy(m.factor(), 1.0));
    }

    void F_EDIT_01_rotationAndFlipGeometry() {
        ZoomModel m = makeModel({1000, 1000}, {400, 200});
        m.setMode(ZoomMode::Actual);
        QCOMPARE(m.imageRectInViewport(), QRectF(300, 400, 400, 200));
        const QTransform t0 = m.imageToViewport();
        QCOMPARE(t0.map(QPointF(0, 0)), QPointF(300, 400));

        m.setRotation(1); // 90° clockwise
        QCOMPARE(m.displayedImageSize(), QSizeF(200, 400));
        QCOMPARE(m.imageRectInViewport(), QRectF(400, 300, 200, 400));
        QCOMPARE(m.imageToViewport().map(QPointF(0, 0)), QPointF(600, 300));   // top-left -> top-right
        QCOMPARE(m.imageToViewport().map(QPointF(400, 0)), QPointF(600, 700)); // top-right -> bottom-right
        QCOMPARE(m.imageToViewport().map(QPointF(0, 200)), QPointF(400, 300)); // bottom-left -> top-left

        m.setRotation(2);
        QCOMPARE(m.imageToViewport().map(QPointF(0, 0)), QPointF(700, 600));
        m.setRotation(3);
        QCOMPARE(m.imageToViewport().map(QPointF(0, 0)), QPointF(400, 700));
        m.setRotation(-3); // normalised: == 1
        QCOMPARE(m.rotation(), 1);
        m.setRotation(4);
        QCOMPARE(m.rotation(), 0);

        m.setFlip(true, false);
        QCOMPARE(m.imageToViewport().map(QPointF(0, 0)), QPointF(700, 400));
        m.setFlip(false, true);
        QCOMPARE(m.imageToViewport().map(QPointF(0, 0)), QPointF(300, 600));
        m.setFlip(true, true);
        QCOMPARE(m.imageToViewport().map(QPointF(0, 0)), QPointF(700, 600));
        // Round trip
        QVERIFY(fuzzyPt(m.viewportToImage(m.imageToViewport().map(QPointF(123, 45))), QPointF(123, 45)));

        // Fit is recomputed for the rotated size.
        ZoomModel f = makeModel({1000, 500}, {2000, 500});
        QVERIFY(fuzzy(f.scale(), 0.5));
        f.setRotation(1);
        QVERIFY(fuzzy(f.scale(), 0.25));
        QCOMPARE(f.imageRectInViewport(), QRectF(437.5, 0, 125, 500));
    }

    void F_VIEW_04_rotationKeepsCenterWhenZoomed() {
        ZoomModel m = makeModel({1000, 800}, {4000, 3000});
        m.setMode(ZoomMode::Actual);
        m.panBy({500, 300});
        const QPointF before = m.viewportToImage({500, 400});
        m.setRotation(1);
        QVERIFY(fuzzyPt(m.viewportToImage({500, 400}), before, 0.5));
    }

    void desiredDecodeSize() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        QCOMPARE(m.desiredDecodeSize(), QSize(1000, 500));
        m.setMode(ZoomMode::Actual);
        QCOMPARE(m.desiredDecodeSize(), QSize(4000, 2000));
        m.setFactor(3.0, {500, 400});
        QCOMPARE(m.desiredDecodeSize(), QSize(4000, 2000)); // never above full size
        m.setRotation(1); // view rotation does not change the decode orientation
        QCOMPARE(m.desiredDecodeSize(), QSize(4000, 2000));
        ZoomModel empty;
        empty.setViewportSize({640, 480});
        QCOMPARE(empty.desiredDecodeSize(), QSize(640, 480));
    }

    void needsHigherResolution() {
        ZoomModel m = makeModel({1000, 800}, {4000, 2000});
        QVERIFY(!m.needsHigherResolution({1000, 500})); // fit: 1 device px per loaded px
        m.setFactor(0.5, {500, 400});
        QVERIFY(m.needsHigherResolution({1000, 500}));
        QVERIFY(!m.needsHigherResolution({4000, 2000}));
    }
};

QTEST_GUILESS_MAIN(TstZoomModel)
#include "tst_zoommodel.moc"
