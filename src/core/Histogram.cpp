// SPDX-License-Identifier: GPL-3.0-or-later
#include "Histogram.h"

#include <QColorSpace>

#include <algorithm>
#include <cmath>

namespace lf {

quint32 Histogram::maxCount(bool ignoreEnds) const {
    quint32 m = 0;
    const int from = ignoreEnds ? 1 : 0;
    const int to = ignoreEnds ? kBins - 1 : kBins;
    for (int i = from; i < to; ++i)
        m = std::max({m, red[i], green[i], blue[i], luma[i]});
    if (m == 0 && ignoreEnds)
        return maxCount(false);
    return m;
}

int lumaOf(int r, int g, int b) {
    return (2126 * r + 7152 * g + 722 * b + 5000) / 10000;
}

namespace {

bool isFloatOrWide(QImage::Format f) {
    switch (f) {
    case QImage::Format_RGBX16FPx4:
    case QImage::Format_RGBA16FPx4:
    case QImage::Format_RGBA16FPx4_Premultiplied:
    case QImage::Format_RGBX32FPx4:
    case QImage::Format_RGBA32FPx4:
    case QImage::Format_RGBA32FPx4_Premultiplied:
        return true;
    default:
        return false;
    }
}

} // namespace

Histogram computeHistogram(const QImage& source, qint64 maxSamples) {
    Histogram h;
    if (source.isNull())
        return h;
    QImage img = source;
    // Large images: a regular grid of samples (nearest neighbour reads only those pixels),
    // before any conversion touches the whole buffer.
    const qint64 pixels = qint64(img.width()) * img.height();
    if (maxSamples > 0 && pixels > maxSamples) {
        const double step = std::sqrt(double(pixels) / double(maxSamples));
        img = img.scaled(std::max(1, int(img.width() / step)), std::max(1, int(img.height() / step)),
                         Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    // Linear float buffers (HDR) are brought to an sRGB-like curve with their own primaries,
    // so the histogram looks like the one of an ordinary image.
    if (isFloatOrWide(img.format()) && img.colorSpace().isValid() &&
        img.colorSpace().transferFunction() != QColorSpace::TransferFunction::SRgb) {
        const QColorSpace target = img.colorSpace().withTransferFunction(QColorSpace::TransferFunction::SRgb);
        if (target.isValid())
            img = img.convertedToColorSpace(target, QImage::Format_RGBA8888);
    }
    if (img.format() != QImage::Format_ARGB32)
        img = img.convertToFormat(QImage::Format_ARGB32); // non-premultiplied: true channel values
    if (img.isNull())
        return h;

    for (int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = line[x];
            if (qAlpha(p) == 0)
                continue;
            const int r = qRed(p);
            const int g = qGreen(p);
            const int b = qBlue(p);
            ++h.red[r];
            ++h.green[g];
            ++h.blue[b];
            ++h.luma[lumaOf(r, g, b)];
            ++h.samples;
        }
    }
    return h;
}

} // namespace lf
