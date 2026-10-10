// SPDX-License-Identifier: GPL-3.0-or-later
#include "LoupeGeometry.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace lf::loupe {

int diameterFor(const QString& sizeId) {
    if (sizeId == QLatin1String("small"))
        return 160;
    if (sizeId == QLatin1String("large"))
        return 360;
    return 240;
}

double magnificationFor(const QString& zoomId) {
    return zoomId == QLatin1String("100") ? 1.0 : 2.0;
}

Geometry compute(const QTransform& imageToWidget, QSize imageSize, QPointF cursor, double diameter,
                 double viewFactor, double magnification, double dpr) {
    Geometry g;
    const double r = diameter / 2.0;
    g.target = QRectF(cursor.x() - r, cursor.y() - r, diameter, diameter);
    if (imageSize.isEmpty() || !(viewFactor > 0) || !(magnification > 0) || !imageToWidget.isInvertible())
        return g;
    // Magnify the view around the cursor: the cursor keeps pointing at the same image pixel.
    const double m = magnification / viewFactor;
    QTransform around;
    around.translate(cursor.x(), cursor.y());
    around.scale(m, m);
    around.translate(-cursor.x(), -cursor.y());
    g.transform = imageToWidget * around;
    // Image px per widget px inside the loupe -> device px per image px.
    const double widgetPerImage = std::sqrt(std::abs(g.transform.determinant()));
    g.deviceScale = widgetPerImage * dpr;
    const QRectF imageRect(QPointF(0, 0), QSizeF(imageSize));
    const QRectF shown = g.transform.inverted().mapRect(g.target);
    g.source = shown.intersected(imageRect);
    const QPointF under = imageToWidget.inverted().map(cursor);
    g.overImage = imageRect.contains(under);
    return g;
}

bool needsFullResolution(QSize stageSize, QSize fullSize, double deviceScale) {
    if (stageSize.isEmpty() || fullSize.isEmpty())
        return false;
    if (stageSize.width() >= fullSize.width() && stageSize.height() >= fullSize.height())
        return false; // the full image is loaded
    // Device px per stage px: > 1 means the stage is magnified on screen.
    const double stagePerFull = double(stageSize.width()) / fullSize.width();
    return deviceScale / stagePerFull > 1.0 + 1e-6;
}

} // namespace lf::loupe
