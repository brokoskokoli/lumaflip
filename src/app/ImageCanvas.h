// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GPU part of ImageView (SPEC 8.4): a QRhiWidget (Metal on macOS) that draws the
// current image as mipmapped, possibly tiled textures (P-08) with color
// conversion in the shader (F-COL-01, ColorPipeline.h).
//
// Threading: QRhiWidget renders on the GUI thread. Texture uploads happen inside
// render() (a per-frame byte budget spreads huge images over several frames).
// Format or color conversions that the GPU cannot do (RGBA64, Indexed, Gray,
// CMYK, LUT-based ICC profiles) run on a worker thread (QtConcurrent) and are
// uploaded when ready, so the GUI thread never converts large images.
//
// GPU texture cache (P-03, SPEC 7.1): the current image and every image passed
// to setPreload() keep their textures, so switching to a preloaded neighbour
// only swaps uniforms and draws.

#include "core/ImageProvider.h"

#include <QColor>
#include <QColorSpace>
#include <QRhiWidget>
#include <QSize>
#include <QTransform>

namespace lf {

class ImageCanvas : public QRhiWidget {
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget* parent = nullptr);
    ~ImageCanvas() override;

    // `sameFile`: keep drawing the previous stage until the new one is uploaded.
    void setCurrent(DecodedImagePtr image, bool sameFile);
    DecodedImagePtr current() const;
    void setPreload(const QList<DecodedImagePtr>& images);

    // Full-resolution image px -> viewport device px.
    void setViewTransform(const QTransform& imageToViewport);
    void setBackground(const QColor& base, bool checker, const QColor& checker1, const QColor& checker2,
                       double checkerCellDevicePx);
    void setUpscaleNearest(bool nearest);
    // Output color space (see DisplayColorSpace.h). Invalid = sRGB.
    void setOutputColorSpace(const QColorSpace& cs);
    QColorSpace outputColorSpace() const;
    // Linear exposure multiplier and max linear value (EDR headroom, M3). SDR: 1, 1.
    void setExposure(float multiplier, float maxValue);

    // For tests: limit the tile size (0 = use the GPU's max texture size).
    void setMaxTileSize(int px);
    int maxTextureSize() const;      // 0 until the RHI is initialized
    bool isRhiAvailable() const;     // false if no QRhi could be created
    // True when the current image is fully uploaded (or there is none) and no
    // conversion is pending.
    bool isIdle() const;
    int currentTileCount() const;
    qint64 gpuBytes() const;         // texture memory held by the cache

signals:
    void conversionFinished();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    struct Private;
    Private* d;
};

} // namespace lf
