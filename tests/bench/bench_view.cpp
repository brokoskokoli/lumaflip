// SPDX-License-Identifier: GPL-3.0-or-later
// P-03: switching to a preloaded neighbour (decoded + GPU texture cached) must
// show the new image within one frame (16 ms).
//
// Measured: ImageView::setImage() + one complete rendered frame of the canvas.
// The frame is forced with QRhiWidget::grabFramebuffer(), which renders, submits
// and waits for the GPU (plus a framebuffer readback of the window size), so the
// number is an upper bound of the real on-screen swap. Median of several swaps.

#include "Bench.h"

#include "app/ImageCanvas.h"
#include "app/ImageView.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QtTest/QTest>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

lf::DecodedImagePtr make24MP(QColor color) {
    auto d = std::make_shared<lf::DecodedImage>();
    d->image = QImage(6000, 4000, QImage::Format_ARGB32_Premultiplied);
    d->image.fill(color);
    d->image.setColorSpace(QColorSpace::SRgb);
    d->fullSize = d->image.size();
    return d;
}

} // namespace

LF_BENCH(P_03_SwapPreloaded, 16.0) {
    if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
        QGuiApplication::platformName() == QLatin1String("minimal"))
        return -1;  // no GPU platform
    lf::ImageView view;
    view.resize(1200, 800);
    view.show();
    if (!QTest::qWaitForWindowExposed(&view))
        return -1;
    auto* canvas = view.findChild<lf::ImageCanvas*>();
    if (!canvas || canvas->grabFramebuffer().isNull() || !canvas->isRhiAvailable())
        return -1;

    const auto a = make24MP(Qt::red);
    const auto b = make24MP(Qt::blue);
    view.setImage(a, false);
    view.setPreloadImages({a, b});
    for (int i = 0; i < 10; ++i)
        canvas->grabFramebuffer();  // upload current + preloaded neighbour

    std::vector<double> samples;
    for (int i = 0; i < 9; ++i) {
        QElapsedTimer t;
        t.start();
        view.setImage(i % 2 ? a : b, false);
        canvas->grabFramebuffer();
        samples.push_back(t.nsecsElapsed() / 1e6);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
