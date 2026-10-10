// SPDX-License-Identifier: GPL-3.0-or-later
// P-07 / F-FMT-03: one animation frame step must fit into a display frame (16 ms).
//
// Measured: AnimationPlayer::step(+1) with the frames decoded ahead (frameChanged ->
// ImageView::showAnimationFrame, which reuses the GPU textures) plus one complete rendered
// frame of the canvas (QRhiWidget::grabFramebuffer: render, submit, wait, read back - an
// upper bound of the real swap). 1280x720 frames; median of 30 steps.

#include "Bench.h"

#include "../integration/GifTestUtil.h"
#include "app/AnimationPlayer.h"
#include "app/ImageCanvas.h"
#include "app/ImageView.h"
#include "core/Decoder.h"

#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <algorithm>
#include <vector>

LF_BENCH(P_07_AnimationFrameStep, 16.0) {
    if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
        QGuiApplication::platformName() == QLatin1String("minimal"))
        return -1; // no GPU platform
    QTemporaryDir dir;
    const QString gif = dir.filePath(QStringLiteral("bench.gif"));
    const int frames = 12;
    if (!lf::test::writeAnimatedGif(gif, QSize(1280, 720), frames, 4))
        return -1;

    lf::ImageView view;
    view.resize(1400, 900);
    view.show();
    if (!QTest::qWaitForWindowExposed(&view))
        return -1;
    auto* canvas = view.findChild<lf::ImageCanvas*>();
    if (!canvas || canvas->grabFramebuffer().isNull() || !canvas->isRhiAvailable())
        return -1;

    QString error;
    auto first = lf::DecoderRegistry::instance().decode(gif, lf::DecodeRequest{}, &error);
    if (!first)
        return -1;
    auto firstPtr = std::make_shared<const lf::DecodedImage>(std::move(*first));
    view.setImage(firstPtr, false);
    canvas->grabFramebuffer();

    lf::AnimationPlayer player;
    QObject::connect(&player, &lf::AnimationPlayer::frameChanged, &view,
                     [&view](int, const lf::DecodedImagePtr& img) { view.showAnimationFrame(img); });
    player.open(gif, firstPtr, frames, false);
    // All frames decoded ahead (small animation: cached completely).
    if (!QTest::qWaitFor([&] { return player.heldFrames() == frames; }, 20000))
        return -1;

    std::vector<double> samples;
    for (int i = 0; i < 31; ++i) {
        QElapsedTimer t;
        t.start();
        player.step(1);
        canvas->grabFramebuffer();
        if (i > 0) // first step: warm-up
            samples.push_back(t.nsecsElapsed() / 1e6);
    }
    if (canvas->framesReusingTextures() < 20)
        return 1e9; // texture reuse broken: fail loudly
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
