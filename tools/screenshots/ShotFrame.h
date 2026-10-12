// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure helpers of the screenshot tool (tools/screenshots, docs/RELEASING.md "Screenshots"):
// which shots exist and how their files are named, the timeline of the demo animation and
// the window frame drawn around a captured window (rounded corners, shadow, the macOS window
// buttons the widget grab cannot see). Only QtCore/QtGui, unit-tested in
// tests/unit/test_screenshot_frame.cpp on every platform.

#include <QColor>
#include <QImage>
#include <QList>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <optional>

class QPainter;

namespace lf::shots {

enum class Appearance { Light, Dark };

QString appearanceId(Appearance appearance); // "light" / "dark"
std::optional<Appearance> appearanceFromId(const QString& id);

// Every shot the tool knows, in the order it takes them.
QStringList knownShots();
// "all" or a comma separated list of knownShots(); unknown names -> nullopt and `error`.
std::optional<QStringList> parseShotList(const QString& spec, QString* error = nullptr);
// "viewer-en-dark.png"; the demo frames go to "demo-en-dark/".
QString shotFileName(const QString& shot, const QString& language, Appearance appearance);

// One frame of the demo animation: the image `index` of the flip sequence is shown, with the
// arrow key drawn pressed (`keyDown`) for the first frame after a flip.
struct DemoFrame {
    int index = 0;
    bool keyDown = false;
    int durationMs = 0;
    bool operator==(const DemoFrame&) const = default;
};
// `images` images: the first held for `firstMs`, then per flip a short key-down frame
// (`keyMs`) and the rest of `holdMs` with the key up.
QList<DemoFrame> demoTimeline(int images, int firstMs, int holdMs, int keyMs);
// Lines "frame-000.png 1600" for the assembly script (tools/screenshots/export_images.sh).
QString demoFrameList(const QList<DemoFrame>& frames);

// The macOS window buttons (close, minimize, zoom) in window coordinates (logical px) for a
// title bar / title strip of `titleHeight`.
QList<QRectF> trafficLightRects(int titleHeight);
QColor trafficLightColor(int button); // 0 close, 1 minimize, 2 zoom

struct FrameStyle {
    double dpr = 2.0;       // device pixels per logical px of `content`
    int cornerRadius = 12;  // logical px
    int margin = 48;        // transparent room around the window for the shadow (logical px)
    int shadowBlur = 22;    // logical px
    int shadowOffsetY = 14; // logical px
    int shadowAlpha = 96;   // 0..255 at the darkest point
    QColor border;          // hairline around the window; invalid = none
    bool trafficLights = false;
    int trafficLightTitleHeight = 28; // height of the strip the buttons are centred in
    // A title bar drawn above the content (dialogs: the native one is not in the grab).
    int titleBarHeight = 0; // logical px; 0 = none
    QString title;
    QColor titleBarColor;
    QColor titleTextColor;
    QColor titleSeparatorColor;
};

// Blurs the alpha channel of `image` (ARGB32 premultiplied) with three box blurs of
// `radius` px (close to a Gaussian); colour channels are scaled along. Pure, deterministic.
QImage blurred(const QImage& image, int radius);
// The decorated window: `content` (device pixels, dpr from `style`) with rounded corners,
// optional title bar and buttons, border and shadow on a transparent canvas.
QImage frameWindow(const QImage& content, const FrameStyle& style);

// True if every pixel of `rect` (device px, clipped to the image) has the same colour -
// e.g. a canvas the widget grab left empty.
bool isUniform(const QImage& image, const QRect& rect);

// A keyboard key cap (demo animation), `center`/`size` in the painter's coordinates.
void drawKeyCap(QPainter& painter, const QRectF& rect, const QString& label, bool pressed);

} // namespace lf::shots
