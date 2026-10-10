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
//
// HDR (F-COL-03/05, core/HdrMapping.h): every image is tone-mapped from its peak
// (DecodedImage::headroom x exposure) to the target of the pass - 1 (SDR) for the widget
// itself, the display's headroom for renderExtended(). SDR content at exposure 1 takes
// exactly the previous code path (bit-identical output). The extended range presentation
// (EdrSurface.h) renders with the same QRhi and texture cache into an extended linear
// Display P3 swapchain; while it is active the widget only clears (it is covered).

#include "core/ImageProvider.h"

#include <QColor>
#include <QColorSpace>
#include <QRhiWidget>
#include <QSize>
#include <QTransform>

#include <functional>

class QRhi;
class QRhiRenderTarget;

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
    // F-FMT-03: next frame of the current animation. Reuses the current GPU textures when
    // the frame has the same size and format (one texture upload per frame, no new
    // allocations); otherwise behaves like setCurrent(frame, true).
    void replaceFrame(DecodedImagePtr frame);
    // F-SLIDE-02: the next setCurrent() fades in over the image shown now (with its view
    // transform) within `ms`, starting when the new image is on the GPU.
    void startCrossfade(int ms);
    bool isCrossfading() const;
    int crossfadeFramesRendered() const; // frames drawn with both images (tests)
    int framesReusingTextures() const;   // replaceFrame() calls that reused textures (tests)

    // Full-resolution image px -> viewport device px.
    void setViewTransform(const QTransform& imageToViewport);
    void setBackground(const QColor& base, bool checker, const QColor& checker1, const QColor& checker2,
                       double checkerCellDevicePx);
    void setUpscaleNearest(bool nearest);
    // Output color space (see DisplayColorSpace.h). Invalid = sRGB.
    void setOutputColorSpace(const QColorSpace& cs);
    QColorSpace outputColorSpace() const;
    // F-COL-05 exposure (linear multiplier, display only) and F-COL-03 headroom of the
    // extended range presentation (>= 1).
    void setHdrMapping(float exposureMultiplier, float extendedHeadroom);
    float exposureMultiplier() const;
    float extendedHeadroom() const;
    // Extended range presentation on/off. While on, frame requests go to `requestFrame`
    // (the EdrSurface) and renderExtended() draws the image.
    void setExtendedPresentation(bool on, std::function<void()> requestFrame = {});
    bool extendedPresentation() const;
    // Records one frame into `rt` (extended linear Display P3, e.g. a swapchain with
    // QRhiSwapChain::HDRExtendedDisplayP3Linear) with `cb`, inside a frame the caller began on
    // sharedRhi(). Returns false if the RHI is not ready.
    bool renderExtended(QRhiCommandBuffer* cb, QRhiRenderTarget* rt);
    // UI overlay for renderExtended() (widgets above the image are hidden behind the native
    // EDR surface): premultiplied ARGB32 in the canvas' device pixels and the display's color
    // space; `dirty` = changed rectangle (device px). A null image removes the overlay.
    void setExtendedOverlay(const QImage& overlay, const QRect& dirty);
    QRhi* sharedRhi() const;            // the QRhi of this widget (null until initialized)
    int extendedFramesRendered() const; // tests
    bool hasExtendedOverlay() const;    // tests

    // For tests: limit the tile size (0 = use the GPU's max texture size).
    void setMaxTileSize(int px);
    int maxTextureSize() const;  // 0 until the RHI is initialized
    bool isRhiAvailable() const; // false if no QRhi could be created
    // True when the current image is fully uploaded (or there is none) and no
    // conversion is pending.
    bool isIdle() const;
    int currentTileCount() const;
    qint64 gpuBytes() const; // texture memory held by the cache

signals:
    void conversionFinished();
    // The QRhi goes away (window change, shutdown): users of sharedRhi() must drop
    // everything they created with it now.
    void rhiAboutToBeReleased();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

private:
    struct Private;
    Private* d;
};

} // namespace lf
