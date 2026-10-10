// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Extended dynamic range presentation of the image canvas (F-COL-03, SPEC 8.4).
//
// Why a separate native surface: Qt composites every widget - QRhiWidget included - into
// the window's own CAMetalLayer, which is an 8-bit SDR swapchain in the screen's color
// space. EDR needs a layer with an RGBA16F extended linear Display P3 drawable and
// wantsExtendedDynamicRangeContent; the window's layer cannot be switched (Qt owns its
// swapchain format and composites the UI into it). EdrSurface is a child QWindow (embedded
// with QWidget::createWindowContainer) with its own QRhiSwapChain in
// QRhiSwapChain::HDRExtendedDisplayP3Linear, created on the canvas' QRhi: textures and the
// GPU cache are shared, switching between the widget and this surface needs no upload.
//
// The surface ignores input (Qt::WindowTransparentForInput): clicks, wheel and gestures
// reach the canvas below, the keyboard focus stays with the view. Widgets above the image
// (error card, toasts, rename field, culling overlay, exposure HUD ...) and widgets of the
// window stacked above the view (quick filter bar) are painted by Qt into the window's layer
// *below* this native view; the view grabs them into an overlay image and hands the changed
// parts to the canvas, which composites them on top in the extended pass (ImageView.cpp).
//
// Used only while an HDR image is shown with HDR on, on an EDR capable display.

#include <QWindow>

#include <memory>

class QRhiSwapChain;
class QRhiRenderPassDescriptor;

namespace lf {

class ImageCanvas;

class EdrSurface : public QWindow {
    Q_OBJECT
public:
    explicit EdrSurface(ImageCanvas* canvas);
    ~EdrSurface() override;

    // Schedules a frame (vsync paced); before the surface is shown/exposed the request is
    // kept and drawn on the next expose.
    void requestFrame();
    // Allows another attempt after failed() (e.g. the window moved to another screen).
    void resetFailure();
    // False if the swapchain format is not supported (screen without EDR) or creating the
    // swapchain failed; failed() is emitted then.
    bool isUsable() const { return !m_failed; }
    // Drops the swapchain (the canvas' QRhi goes away); created again on the next frame.
    void releaseSwapChain();

signals:
    void failed(); // the extended presentation cannot be used: fall back to the widget

protected:
    bool event(QEvent* e) override;
    void exposeEvent(QExposeEvent* e) override;

private:
    bool ensureSwapChain();
    void renderFrame();

    ImageCanvas* m_canvas;
    std::unique_ptr<QRhiSwapChain> m_swapChain;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    bool m_failed = false;
    bool m_pending = false; // requestUpdate() issued, UpdateRequest outstanding
    bool m_dirty = false;   // a frame was requested and not drawn yet
};

} // namespace lf
