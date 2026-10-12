// SPDX-License-Identifier: GPL-3.0-or-later
// Pure helpers of the screenshot tool (tools/screenshots/ShotFrame.h): shot names, the demo
// timeline and the window frame around a captured window.

#include "QtTestApp.h"
#include "screenshots/ShotFrame.h"

#include <QGuiApplication>
#include <QPainter>

#include <gtest/gtest.h>

using namespace lf::shots;

namespace {

// Text is drawn with the font database of a QGuiApplication.
bool haveGuiApp() {
    lf::test::ensureQtApp();
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}

} // namespace

TEST(ScreenshotPlan, ParsesShotListsInCanonicalOrder) {
    EXPECT_EQ(parseShotList(QStringLiteral("all")), knownShots());
    const auto some = parseShotList(QStringLiteral("grid, viewer,grid"));
    ASSERT_TRUE(some.has_value());
    EXPECT_EQ(*some, (QStringList{QStringLiteral("viewer"), QStringLiteral("grid")}));
    QString error;
    EXPECT_FALSE(parseShotList(QStringLiteral("viewer,nope"), &error).has_value());
    EXPECT_TRUE(error.contains(QStringLiteral("nope")));
    EXPECT_FALSE(parseShotList(QStringLiteral(" , "), &error).has_value());
}

TEST(ScreenshotPlan, FileNamesAndAppearances) {
    EXPECT_EQ(shotFileName(QStringLiteral("viewer"), QStringLiteral("de"), Appearance::Dark),
              QStringLiteral("viewer-de-dark.png"));
    EXPECT_EQ(shotFileName(QStringLiteral("demo"), QStringLiteral("en"), Appearance::Light),
              QStringLiteral("demo-en-light/"));
    EXPECT_EQ(appearanceFromId(QStringLiteral("dark")), Appearance::Dark);
    EXPECT_EQ(appearanceFromId(appearanceId(Appearance::Light)), Appearance::Light);
    EXPECT_FALSE(appearanceFromId(QStringLiteral("auto")).has_value());
}

TEST(ScreenshotPlan, DemoTimelineHoldsEveryImage) {
    const QList<DemoFrame> frames = demoTimeline(3, 2000, 1400, 250);
    const QList<DemoFrame> expected{
        {0, false, 2000}, {1, true, 250}, {1, false, 1150}, {2, true, 250}, {2, false, 1150}};
    EXPECT_EQ(frames, expected);
    EXPECT_TRUE(demoTimeline(0, 1, 1, 1).isEmpty());
    EXPECT_EQ(demoFrameList(frames.mid(0, 2)), QStringLiteral("frame-000.png 2000\nframe-001.png 250\n"));
}

TEST(ScreenshotFrame, TrafficLightsAreCentredInTheTitle) {
    const QList<QRectF> rects = trafficLightRects(28);
    ASSERT_EQ(rects.size(), 3);
    EXPECT_DOUBLE_EQ(rects[0].center().x(), 20.0);
    EXPECT_DOUBLE_EQ(rects[2].center().x(), 60.0);
    for (const QRectF& r : rects) {
        EXPECT_DOUBLE_EQ(r.center().y(), 14.0);
        EXPECT_DOUBLE_EQ(r.width(), 12.0);
    }
    EXPECT_EQ(trafficLightColor(0), QColor(0xff, 0x5f, 0x57));
}

TEST(ScreenshotFrame, FramesContentWithCornersShadowAndButtons) {
    QImage content(200, 100, QImage::Format_ARGB32_Premultiplied);
    content.fill(QColor(10, 120, 200));
    FrameStyle style;
    style.dpr = 2.0;
    style.margin = 10;
    style.cornerRadius = 8;
    style.shadowBlur = 4;
    style.shadowOffsetY = 3;
    style.trafficLights = true;
    style.trafficLightTitleHeight = 28;
    const QImage out = frameWindow(content, style);
    EXPECT_EQ(out.size(), QSize(240, 140)); // content + 2 x 10 logical px at 2x
    EXPECT_DOUBLE_EQ(out.devicePixelRatio(), 2.0);
    // Outside the window: transparent far away, shadow below the window.
    EXPECT_EQ(qAlpha(out.pixel(0, 0)), 0);
    EXPECT_GT(qAlpha(out.pixel(120, 125)), 0);
    EXPECT_LT(qAlpha(out.pixel(120, 125)), 255);
    // The rounded corner is not filled, the middle is the content.
    EXPECT_LT(qAlpha(out.pixel(21, 21)), 255);
    EXPECT_EQ(out.pixelColor(120, 70), QColor(10, 120, 200));
    // The close button: centre (20, 14) logical inside the window = (20 + 40, 20 + 28) px.
    const QColor close = out.pixelColor(60, 48);
    EXPECT_EQ(close, trafficLightColor(0));
}

TEST(ScreenshotFrame, TitleBarAddsHeightAndText) {
    if (!haveGuiApp())
        GTEST_SKIP() << "no QGuiApplication";
    QImage content(100, 50, QImage::Format_ARGB32_Premultiplied);
    content.fill(Qt::white);
    FrameStyle style;
    style.dpr = 1.0;
    style.margin = 5;
    style.shadowAlpha = 0;
    style.titleBarHeight = 28;
    style.titleBarColor = QColor(200, 200, 200);
    const QImage out = frameWindow(content, style);
    EXPECT_EQ(out.size(), QSize(110, 88));
    EXPECT_EQ(out.pixelColor(15, 10), QColor(200, 200, 200)); // title bar
    EXPECT_EQ(out.pixelColor(50, 60), QColor(Qt::white));     // content below it
    EXPECT_EQ(qAlpha(out.pixel(1, 1)), 0);                    // no shadow
}

TEST(ScreenshotFrame, BlurSpreadsAlphaSymmetrically) {
    QImage dot(21, 21, QImage::Format_ARGB32_Premultiplied);
    dot.fill(Qt::transparent);
    dot.setPixelColor(10, 10, QColor(0, 0, 0, 255));
    const QImage b = blurred(dot, 2);
    EXPECT_GT(qAlpha(b.pixel(10, 10)), qAlpha(b.pixel(13, 10)));
    EXPECT_EQ(qAlpha(b.pixel(7, 10)), qAlpha(b.pixel(13, 10)));
    EXPECT_EQ(qAlpha(b.pixel(10, 7)), qAlpha(b.pixel(10, 13)));
    EXPECT_EQ(qAlpha(b.pixel(0, 0)), 0);
    EXPECT_EQ(blurred(dot, 0).pixelColor(10, 10), QColor(0, 0, 0, 255));
}

TEST(ScreenshotFrame, UniformRegionDetection) {
    QImage img(10, 10, QImage::Format_ARGB32);
    img.fill(Qt::black);
    EXPECT_TRUE(isUniform(img, QRect(0, 0, 10, 10)));
    img.setPixelColor(9, 9, Qt::red);
    EXPECT_FALSE(isUniform(img, QRect(0, 0, 10, 10)));
    EXPECT_TRUE(isUniform(img, QRect(0, 0, 9, 9)));
    EXPECT_TRUE(isUniform(img, QRect(20, 20, 5, 5))); // outside: nothing to compare
}

TEST(ScreenshotFrame, KeyCapDrawsSomething) {
    if (!haveGuiApp())
        GTEST_SKIP() << "no QGuiApplication";
    QImage img(80, 80, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    drawKeyCap(p, QRectF(10, 10, 60, 60), QStringLiteral("→"), true);
    p.end();
    EXPECT_FALSE(isUniform(img, img.rect()));
    EXPECT_EQ(qAlpha(img.pixel(40, 40)), 255);
}
