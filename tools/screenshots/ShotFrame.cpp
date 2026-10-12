// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShotFrame.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lf::shots {

QString appearanceId(Appearance appearance) {
    return appearance == Appearance::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

std::optional<Appearance> appearanceFromId(const QString& id) {
    if (id == QLatin1String("light"))
        return Appearance::Light;
    if (id == QLatin1String("dark"))
        return Appearance::Dark;
    return std::nullopt;
}

QStringList knownShots() {
    return {QStringLiteral("viewer"),      QStringLiteral("info"),    QStringLiteral("culling"),
            QStringLiteral("grid"),        QStringLiteral("compare"), QStringLiteral("map"),
            QStringLiteral("preferences"), QStringLiteral("demo")};
}

std::optional<QStringList> parseShotList(const QString& spec, QString* error) {
    const QStringList known = knownShots();
    if (spec.trimmed() == QLatin1String("all"))
        return known;
    QStringList wanted;
    for (const QString& part : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString name = part.trimmed();
        if (!known.contains(name)) {
            if (error)
                *error =
                    QStringLiteral("unknown shot \"%1\" (known: %2)").arg(name, known.join(QLatin1Char(',')));
            return std::nullopt;
        }
        if (!wanted.contains(name))
            wanted << name;
    }
    if (wanted.isEmpty()) {
        if (error)
            *error = QStringLiteral("no shot given");
        return std::nullopt;
    }
    // Always in the order of knownShots() (ratings set by one shot show in the next ones).
    QStringList ordered;
    for (const QString& name : known)
        if (wanted.contains(name))
            ordered << name;
    return ordered;
}

QString shotFileName(const QString& shot, const QString& language, Appearance appearance) {
    const QString base = QStringLiteral("%1-%2-%3").arg(shot, language, appearanceId(appearance));
    return shot == QLatin1String("demo") ? base + QLatin1Char('/') : base + QStringLiteral(".png");
}

QList<DemoFrame> demoTimeline(int images, int firstMs, int holdMs, int keyMs) {
    QList<DemoFrame> frames;
    if (images <= 0)
        return frames;
    frames.append({0, false, firstMs});
    for (int i = 1; i < images; ++i) {
        frames.append({i, true, keyMs});
        frames.append({i, false, std::max(0, holdMs - keyMs)});
    }
    return frames;
}

QString demoFrameList(const QList<DemoFrame>& frames) {
    QString out;
    for (qsizetype i = 0; i < frames.size(); ++i)
        out += QStringLiteral("frame-%1.png %2\n").arg(i, 3, 10, QLatin1Char('0')).arg(frames[i].durationMs);
    return out;
}

QList<QRectF> trafficLightRects(int titleHeight) {
    // Standard NSWindow buttons: 12 pt circles, 20 pt apart, the first centred 20 pt from
    // the left edge, vertically centred in the title bar.
    constexpr double kDiameter = 12.0;
    QList<QRectF> rects;
    for (int i = 0; i < 3; ++i) {
        const QPointF c(20.0 + 20.0 * i, titleHeight / 2.0);
        rects << QRectF(c.x() - kDiameter / 2, c.y() - kDiameter / 2, kDiameter, kDiameter);
    }
    return rects;
}

QColor trafficLightColor(int button) {
    switch (button) {
    case 0:
        return QColor(0xff, 0x5f, 0x57);
    case 1:
        return QColor(0xfe, 0xbc, 0x2e);
    default:
        return QColor(0x28, 0xc8, 0x40);
    }
}

namespace {

// One horizontal or vertical box blur pass over premultiplied ARGB values.
void boxPass(std::vector<float>& data, int w, int h, int radius, bool horizontal) {
    const int len = horizontal ? w : h;
    const int lines = horizontal ? h : w;
    std::vector<float> line(size_t(len) * 4);
    const float norm = 1.0f / float(2 * radius + 1);
    for (int l = 0; l < lines; ++l) {
        auto at = [&](int i) -> size_t {
            return horizontal ? (size_t(l) * w + i) * 4 : (size_t(i) * w + l) * 4;
        };
        for (int i = 0; i < len; ++i)
            for (int c = 0; c < 4; ++c)
                line[size_t(i) * 4 + c] = data[at(i) + c];
        float sum[4] = {0, 0, 0, 0};
        for (int i = -radius; i <= radius; ++i) {
            const int k = std::clamp(i, 0, len - 1);
            for (int c = 0; c < 4; ++c)
                sum[c] += (i < 0 || i >= len) ? 0.0f : line[size_t(k) * 4 + c];
        }
        for (int i = 0; i < len; ++i) {
            for (int c = 0; c < 4; ++c)
                data[at(i) + c] = sum[c] * norm;
            const int out = i - radius;
            const int in = i + radius + 1;
            for (int c = 0; c < 4; ++c) {
                if (out >= 0)
                    sum[c] -= line[size_t(out) * 4 + c];
                if (in < len)
                    sum[c] += line[size_t(in) * 4 + c];
            }
        }
    }
}

} // namespace

QImage blurred(const QImage& image, int radius) {
    QImage src = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (radius <= 0 || src.isNull())
        return src;
    const int w = src.width();
    const int h = src.height();
    std::vector<float> data(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            data[i] = float(qAlpha(row[x]));
            data[i + 1] = float(qRed(row[x]));
            data[i + 2] = float(qGreen(row[x]));
            data[i + 3] = float(qBlue(row[x]));
        }
    }
    for (int pass = 0; pass < 3; ++pass) {
        boxPass(data, w, h, radius, true);
        boxPass(data, w, h, radius, false);
    }
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            auto c = [](float v) { return std::clamp(int(std::lround(v)), 0, 255); };
            const int a = c(data[i]);
            row[x] = qRgba(std::min(c(data[i + 1]), a), std::min(c(data[i + 2]), a),
                           std::min(c(data[i + 3]), a), a);
        }
    }
    return out;
}

QImage frameWindow(const QImage& content, const FrameStyle& style) {
    const double dpr = style.dpr > 0 ? style.dpr : 1.0;
    auto px = [dpr](double logical) { return int(std::lround(logical * dpr)); };
    const int titleH = px(style.titleBarHeight);
    const QSize window(content.width(), content.height() + titleH);
    const int margin = px(style.margin);
    const QSize canvasSize(window.width() + 2 * margin, window.height() + 2 * margin);
    const QRectF windowRect(margin, margin, window.width(), window.height());
    const double radius = style.cornerRadius * dpr;

    QPainterPath shape;
    shape.addRoundedRect(windowRect, radius, radius);

    // Shadow: the window shape, offset and blurred, on a transparent canvas.
    QImage shadow(canvasSize, QImage::Format_ARGB32_Premultiplied);
    shadow.fill(Qt::transparent);
    if (style.shadowAlpha > 0) {
        QPainter p(&shadow);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(0, px(style.shadowOffsetY));
        p.fillPath(shape, QColor(0, 0, 0, style.shadowAlpha));
        p.end();
        shadow = blurred(shadow, std::max(1, px(style.shadowBlur) / 2));
    }

    QImage out = shadow;
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    // The window: clear the shadow below it (a translucent window would show it), then the
    // title bar and the content, clipped to the rounded shape.
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillPath(shape, Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setClipPath(shape);
    if (titleH > 0) {
        const QRectF bar(windowRect.left(), windowRect.top(), window.width(), titleH);
        p.fillRect(bar, style.titleBarColor.isValid() ? style.titleBarColor : QColor(236, 236, 236));
        if (style.titleSeparatorColor.isValid())
            p.fillRect(QRectF(bar.left(), bar.bottom() - dpr * 0.5, bar.width(), dpr * 0.5),
                       style.titleSeparatorColor);
        if (!style.title.isEmpty()) {
            QFont font = p.font();
            font.setPixelSize(px(13));
            font.setBold(true);
            p.setFont(font);
            p.setPen(style.titleTextColor.isValid() ? style.titleTextColor : QColor(40, 40, 40));
            p.drawText(bar, Qt::AlignCenter, style.title);
        }
    }
    p.drawImage(QPointF(windowRect.left(), windowRect.top() + titleH), content);
    if (style.trafficLights) {
        const int stripH = titleH > 0 ? style.titleBarHeight : style.trafficLightTitleHeight;
        const QList<QRectF> rects = trafficLightRects(stripH);
        for (int i = 0; i < rects.size(); ++i) {
            const QRectF r(windowRect.left() + rects[i].left() * dpr, windowRect.top() + rects[i].top() * dpr,
                           rects[i].width() * dpr, rects[i].height() * dpr);
            const QColor fill = trafficLightColor(i);
            p.setPen(QPen(fill.darker(135), 0.5 * dpr));
            p.setBrush(fill);
            p.drawEllipse(r.adjusted(0.25 * dpr, 0.25 * dpr, -0.25 * dpr, -0.25 * dpr));
        }
    }
    p.setClipping(false);
    if (style.border.isValid()) {
        p.setPen(QPen(style.border, dpr));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(windowRect.adjusted(0.5 * dpr, 0.5 * dpr, -0.5 * dpr, -0.5 * dpr), radius, radius);
    }
    p.end();
    out.setDevicePixelRatio(dpr);
    return out;
}

bool isUniform(const QImage& image, const QRect& rect) {
    const QRect r = rect.intersected(image.rect());
    if (r.isEmpty())
        return true;
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb first = img.pixel(r.topLeft());
    for (int y = r.top(); y <= r.bottom(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = r.left(); x <= r.right(); ++x)
            if (row[x] != first)
                return false;
    }
    return true;
}

void drawKeyCap(QPainter& painter, const QRectF& rect, const QString& label, bool pressed) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const double r = rect.height() * 0.18;
    const double depth = rect.height() * (pressed ? 0.03 : 0.08);
    const QRectF cap = rect.adjusted(0, pressed ? rect.height() * 0.05 : 0, 0, -depth);
    // Soft shadow and the key's side.
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 70));
    painter.drawRoundedRect(rect.translated(0, rect.height() * 0.04), r, r);
    painter.setBrush(pressed ? QColor(150, 205, 210) : QColor(196, 214, 217));
    painter.drawRoundedRect(rect, r, r);
    QLinearGradient top(cap.topLeft(), cap.bottomLeft());
    top.setColorAt(0, pressed ? QColor(214, 244, 246) : QColor(255, 255, 255));
    top.setColorAt(1, pressed ? QColor(190, 232, 236) : QColor(240, 245, 246));
    painter.setBrush(top);
    painter.drawRoundedRect(cap, r, r);
    QFont font = painter.font();
    font.setPixelSize(int(std::lround(rect.height() * 0.5)));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor(0x0f, 0x2a, 0x33));
    painter.drawText(cap, Qt::AlignCenter, label);
    painter.restore();
}

} // namespace lf::shots
