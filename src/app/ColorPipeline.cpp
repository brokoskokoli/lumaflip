// SPDX-License-Identifier: GPL-3.0-or-later
#include "ColorPipeline.h"

#include <QColorTransform>
#include <QRgbaFloat>

#include <cmath>

namespace lf {

namespace {

bool isMatrixRgb(const QColorSpace& cs) {
    return cs.isValid() && cs.colorModel() == QColorSpace::ColorModel::Rgb &&
           cs.transformModel() == QColorSpace::TransformModel::ThreeComponentMatrix;
}

TransferKind kindOf(const QColorSpace& cs, float* gamma) {
    switch (cs.transferFunction()) {
    case QColorSpace::TransferFunction::Linear:
        return TransferKind::Linear;
    case QColorSpace::TransferFunction::SRgb:
        return TransferKind::SRgb;
    case QColorSpace::TransferFunction::Gamma:
        *gamma = cs.gamma();
        return TransferKind::Gamma;
    default:
        return TransferKind::Lut;
    }
}

QColorSpace linearOf(const QColorSpace& cs) {
    return cs.withTransferFunction(QColorSpace::TransferFunction::Linear);
}

} // namespace

QColorSpace effectiveSourceColorSpace(const QImage& image) {
    const QColorSpace cs = image.colorSpace();
    return cs.isValid() ? cs : QColorSpace(QColorSpace::SRgb);
}

bool needsCpuConversion(const QColorSpace& src) {
    return !isMatrixRgb(src);
}

QColorSpace sanitizeOutputColorSpace(const QColorSpace& dst) {
    if (isMatrixRgb(dst) && dst.isValidTarget())
        return dst;
    if (dst.isValid() && dst.colorModel() == QColorSpace::ColorModel::Rgb) {
        const auto pp = dst.primaryPoints();
        if (pp.isValid())
            return QColorSpace(pp, QColorSpace::TransferFunction::SRgb);
    }
    return QColorSpace(QColorSpace::SRgb);
}

void fillDestinationTransfer(ColorPipeline& p, const QColorSpace& dst) {
    p.dstKind = kindOf(dst, &p.dstGamma);
    p.dstLut.clear();
    if (p.dstKind != TransferKind::Lut)
        return;
    const QColorTransform t = linearOf(dst).transformationToColorSpace(dst);
    p.dstLut.resize(ColorPipeline::kLutSize * 4);
    for (int i = 0; i < ColorPipeline::kLutSize; ++i) {
        const float s = float(i) / float(ColorPipeline::kLutSize - 1);
        const float lin = s * s;
        const QRgbaFloat32 o = t.map(QRgbaFloat32{lin, lin, lin, 1.0f});
        p.dstLut[i * 4 + 0] = o.r;
        p.dstLut[i * 4 + 1] = o.g;
        p.dstLut[i * 4 + 2] = o.b;
        p.dstLut[i * 4 + 3] = 1.0f;
    }
}

ColorPipeline makeColorPipeline(const QColorSpace& srcIn, const QColorSpace& dstIn) {
    ColorPipeline p;
    const QColorSpace src = srcIn.isValid() ? srcIn : QColorSpace(QColorSpace::SRgb);
    const QColorSpace dst = sanitizeOutputColorSpace(dstIn);
    fillDestinationTransfer(p, dst);
    if (src == dst || !isMatrixRgb(src)) {
        p.identity = true; // equal, or already converted on the CPU
        return p;
    }
    p.identity = false;
    p.srcKind = kindOf(src, &p.srcGamma);
    if (p.srcKind == TransferKind::Lut) {
        const QColorTransform t = src.transformationToColorSpace(linearOf(src));
        p.srcLut.resize(ColorPipeline::kLutSize * 4);
        for (int i = 0; i < ColorPipeline::kLutSize; ++i) {
            const float v = float(i) / float(ColorPipeline::kLutSize - 1);
            const QRgbaFloat32 o = t.map(QRgbaFloat32{v, v, v, 1.0f});
            p.srcLut[i * 4 + 0] = o.r;
            p.srcLut[i * 4 + 1] = o.g;
            p.srcLut[i * 4 + 2] = o.b;
            p.srcLut[i * 4 + 3] = 1.0f;
        }
    }
    // Columns of the linear-to-linear matrix = images of the unit vectors. Float
    // mapping in Qt is unclamped, so out-of-gamut results (negative) are exact.
    const QColorTransform m = linearOf(src).transformationToColorSpace(linearOf(dst));
    const QRgbaFloat32 units[3] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    for (int c = 0; c < 3; ++c) {
        const QRgbaFloat32 o = m.map(units[c]);
        p.matrix[0 * 3 + c] = o.r;
        p.matrix[1 * 3 + c] = o.g;
        p.matrix[2 * 3 + c] = o.b;
    }
    return p;
}

} // namespace lf
