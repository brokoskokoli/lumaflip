// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-VIEW-11 magnifier ("Lupe") geometry, without widgets. The loupe is a circle centered
// on the cursor that shows the image around the cursor at 100 % or 200 % (zoom factor as
// in the view: 1.0 = one image pixel per device pixel, or per point with that setting). It
// keeps the view's rotation and mirroring: the view's image -> widget transform is
// magnified around the cursor.

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QTransform>

namespace lf::loupe {

// Loupe diameters (logical px) for the setting key::LoupeSize.
int diameterFor(const QString& sizeId); // "small" 160, "medium" 240 (default), "large" 360
double magnificationFor(const QString& zoomId); // "100" 1.0, "200" 2.0 (default 2.0)

struct Geometry {
    QRectF target;          // the loupe's square (circle bounds) in widget coordinates
    QTransform transform;   // full-size image px -> widget coordinates inside the loupe
    QRectF source;          // part of the full-size image the loupe shows (clipped to the image)
    double deviceScale = 0; // device px per full-size image px inside the loupe
    bool overImage = false; // the cursor is over the image
};

// `imageToWidget`: the view's transform (full-size image px -> widget logical px);
// `viewFactor`: the view's current zoom factor; `magnification`: wanted factor in the loupe;
// `dpr`: device pixel ratio of the widget.
Geometry compute(const QTransform& imageToWidget, QSize imageSize, QPointF cursor, double diameter,
                 double viewFactor, double magnification, double dpr);

// The loaded stage (`stageSize` px of an image with `fullSize` px) is too coarse for the
// loupe: it would be magnified on screen while the full image has more pixels.
bool needsFullResolution(QSize stageSize, QSize fullSize, double deviceScale);

} // namespace lf::loupe
