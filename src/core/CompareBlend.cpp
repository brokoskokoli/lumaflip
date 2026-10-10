// SPDX-License-Identifier: GPL-3.0-or-later
// Difference overlay and blending (F-GRID-04), see CompareBlend.h.

#include "CompareBlend.h"

#include <algorithm>
#include <cmath>

namespace lf::compare {

QString blendModeId(BlendMode m) {
    switch (m) {
    case BlendMode::Off:
        return QStringLiteral("off");
    case BlendMode::Difference:
        return QStringLiteral("difference");
    case BlendMode::Blend:
        return QStringLiteral("blend");
    case BlendMode::Wipe:
        return QStringLiteral("wipe");
    }
    return QStringLiteral("off");
}

BlendMode blendModeFromId(const QString& id) {
    for (BlendMode m : {BlendMode::Difference, BlendMode::Blend, BlendMode::Wipe})
        if (blendModeId(m) == id)
            return m;
    return BlendMode::Off;
}

QRgb differencePixel(QRgb a, QRgb b, double amount) {
    auto ch = [amount](int x, int y) {
        return std::clamp(int(std::lround(std::abs(x - y) * std::max(1.0, amount))), 0, 255);
    };
    return qRgba(ch(qRed(a), qRed(b)), ch(qGreen(a), qGreen(b)), ch(qBlue(a), qBlue(b)),
                 std::max(qAlpha(a), qAlpha(b)));
}

QRgb blendPixel(QRgb a, QRgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto ch = [t](int x, int y) { return std::clamp(int(std::lround(x * (1.0 - t) + y * t)), 0, 255); };
    return qRgba(ch(qRed(a), qRed(b)), ch(qGreen(a), qGreen(b)), ch(qBlue(a), qBlue(b)),
                 ch(qAlpha(a), qAlpha(b)));
}

QImage composite(const QImage& a0, const QImage& b0, BlendMode mode, double value) {
    if (a0.isNull())
        return {};
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (mode == BlendMode::Off || b0.isNull() || b0.size() != a0.size())
        return a;
    const QImage b = b0.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out(a.size(), QImage::Format_ARGB32_Premultiplied);
    if (out.isNull())
        return {};
    const int w = a.width(), h = a.height();
    const int split = int(std::lround(std::clamp(value, 0.0, 1.0) * w));
    for (int y = 0; y < h; ++y) {
        const auto* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        auto* po = reinterpret_cast<QRgb*>(out.scanLine(y));
        switch (mode) {
        case BlendMode::Difference:
            for (int x = 0; x < w; ++x)
                po[x] = differencePixel(pa[x], pb[x], value);
            break;
        case BlendMode::Blend:
            for (int x = 0; x < w; ++x)
                po[x] = blendPixel(pa[x], pb[x], value);
            break;
        case BlendMode::Wipe:
            for (int x = 0; x < w; ++x)
                po[x] = x < split ? pa[x] : pb[x];
            break;
        case BlendMode::Off:
            break;
        }
    }
    return out;
}

DiffStats differenceStats(const QImage& a0, const QImage& b0) {
    DiffStats s;
    if (a0.isNull() || a0.size() != b0.size())
        return s;
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32);
    const QImage b = b0.convertToFormat(QImage::Format_ARGB32);
    qint64 changed = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const int d =
                std::max({std::abs(qRed(pa[x]) - qRed(pb[x])), std::abs(qGreen(pa[x]) - qGreen(pb[x])),
                          std::abs(qBlue(pa[x]) - qBlue(pb[x]))});
            s.maxDelta = std::max(s.maxDelta, d);
            if (d > 0)
                ++changed;
        }
    }
    s.changedFraction = double(changed) / std::max<qint64>(1, qint64(a.width()) * a.height());
    return s;
}

} // namespace lf::compare
