// SPDX-License-Identifier: GPL-3.0-or-later
// Difference overlay and blending (F-GRID-04), see CompareBlend.h.

#include "CompareBlend.h"

#include <QColorSpace>
#include <QPainter>

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
    const int alpha = std::max(qAlpha(a), qAlpha(b));
    // Premultiplied: an amplified channel must not exceed alpha (invalid pixel otherwise).
    auto ch = [amount, alpha](int x, int y) {
        return std::clamp(int(std::lround(std::abs(x - y) * std::max(1.0, amount))), 0, alpha);
    };
    return qRgba(ch(qRed(a), qRed(b)), ch(qGreen(a), qGreen(b)), ch(qBlue(a), qBlue(b)), alpha);
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

QTransform partnerPlacement(QSizeF fullA, QSizeF fullB) {
    if (fullA.isEmpty() || fullB.isEmpty())
        return {};
    const double s = std::min(fullA.width() / fullB.width(), fullA.height() / fullB.height());
    const QPointF off((fullA.width() - fullB.width() * s) / 2.0, (fullA.height() - fullB.height() * s) / 2.0);
    return QTransform::fromScale(s, s) * QTransform::fromTranslate(off.x(), off.y());
}

QTransform alignPartner(const QTransform& bToA, const QTransform& aToOut, QSizeF fullB, QPointF outCenter,
                        QPointF centerDelta, double zoomRatio) {
    const QTransform m = bToA * aToOut;
    if (!m.isInvertible() || fullB.isEmpty())
        return m;
    if (centerDelta.isNull() && std::abs(zoomRatio - 1.0) < 1e-9)
        return m;
    // m(p) = L(p - pc) + C with pc = m^-1(C); the pane offset moves pc, the ratio scales L.
    const QPointF pc = m.inverted().map(outCenter) +
                       QPointF(centerDelta.x() * fullB.width(), centerDelta.y() * fullB.height());
    const QTransform linear(m.m11(), m.m12(), m.m21(), m.m22(), 0, 0);
    const double r = zoomRatio > 0 ? zoomRatio : 1.0;
    return QTransform::fromTranslate(-pc.x(), -pc.y()) * linear * QTransform::fromScale(r, r) *
           QTransform::fromTranslate(outCenter.x(), outCenter.y());
}

QImage renderLayer(const QImage& src, const QTransform& toOut, QSize outSize, const QColor& background) {
    QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull())
        return {};
    out.fill(background);
    if (src.isNull())
        return out;
    // Drawn in the source's own color space (float stays float until the conversion), then
    // converted at output size: both images end up in one color space (sRGB), so differences
    // never come from profiles, and nothing full-size is converted.
    const bool deep = src.depth() > 32;
    QImage layer(outSize,
                 deep ? QImage::Format_RGBA32FPx4_Premultiplied : QImage::Format_ARGB32_Premultiplied);
    if (layer.isNull())
        return out;
    layer.fill(Qt::transparent);
    {
        QPainter p(&layer);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setTransform(toOut);
        p.drawImage(0, 0, src);
    }
    const QColorSpace srgb(QColorSpace::SRgb);
    if (src.colorSpace().isValid() && src.colorSpace() != srgb) {
        layer.setColorSpace(src.colorSpace());
        layer = layer.convertedToColorSpace(srgb, QImage::Format_ARGB32_Premultiplied);
    } else if (deep) {
        layer = layer.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    QPainter p(&out);
    p.drawImage(0, 0, layer);
    return out;
}

} // namespace lf::compare
