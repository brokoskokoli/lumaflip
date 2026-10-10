// SPDX-License-Identifier: GPL-3.0-or-later
#include "ImagePatch.h"

#include <cstring>

namespace lf {

QRect copyImagePatch(QImage& dst, const QImage& patch, QPoint at) {
    if (dst.isNull() || patch.isNull())
        return {};
    const QRect target = QRect(at, patch.size()).intersected(dst.rect());
    if (target.isEmpty())
        return {};
    const QImage src = patch.format() == dst.format() ? patch : patch.convertToFormat(dst.format());
    if (src.isNull() || src.depth() != dst.depth() || dst.depth() % 8 != 0)
        return {};
    const qsizetype bpp = dst.depth() / 8;
    const QPoint from = target.topLeft() - at; // first source pixel
    const qsizetype bytes = qsizetype(target.width()) * bpp;
    for (int y = 0; y < target.height(); ++y) {
        const uchar* s = src.constScanLine(from.y() + y) + qsizetype(from.x()) * bpp;
        uchar* d = dst.scanLine(target.y() + y) + qsizetype(target.x()) * bpp;
        std::memcpy(d, s, size_t(bytes));
    }
    return target;
}

} // namespace lf
