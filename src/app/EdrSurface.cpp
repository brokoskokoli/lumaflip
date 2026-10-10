// SPDX-License-Identifier: GPL-3.0-or-later
#include "EdrSurface.h"

#include "ImageCanvas.h"

#include <QEvent>
#include <QExposeEvent>
#include <QLoggingCategory>
#include <QPlatformSurfaceEvent>
#include <QTimer>
#include <rhi/qrhi.h>

Q_LOGGING_CATEGORY(lcEdr, "lf.view.edr")

namespace lf {

EdrSurface::EdrSurface(ImageCanvas* canvas) : m_canvas(canvas) {
    setSurfaceType(QSurface::MetalSurface);
    setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
    connect(canvas, &ImageCanvas::rhiAboutToBeReleased, this, &EdrSurface::releaseSwapChain);
}

EdrSurface::~EdrSurface() {
    releaseSwapChain();
}

void EdrSurface::requestFrame() {
    if (m_pending || m_failed)
        return;
    m_pending = true;
    requestUpdate();
}

void EdrSurface::releaseSwapChain() {
    m_swapChain.reset();
    m_renderPass.reset();
}

bool EdrSurface::ensureSwapChain() {
    QRhi* rhi = m_canvas ? m_canvas->sharedRhi() : nullptr;
    if (!rhi || !isExposed() || m_failed)
        return false;
    if (m_swapChain && m_swapChain->rhi() != rhi)
        releaseSwapChain();
    if (!m_swapChain) {
        m_swapChain.reset(rhi->newSwapChain());
        m_swapChain->setWindow(this);
        if (!m_swapChain->isFormatSupported(QRhiSwapChain::HDRExtendedDisplayP3Linear)) {
            qCInfo(lcEdr) << "extended linear Display P3 swapchain not supported on this screen";
            releaseSwapChain();
            m_failed = true;
            QTimer::singleShot(0, this, &EdrSurface::failed);
            return false;
        }
        m_swapChain->setFormat(QRhiSwapChain::HDRExtendedDisplayP3Linear);
        m_renderPass.reset(m_swapChain->newCompatibleRenderPassDescriptor());
        m_swapChain->setRenderPassDescriptor(m_renderPass.get());
        if (!m_swapChain->createOrResize()) {
            qCWarning(lcEdr) << "creating the EDR swapchain failed";
            releaseSwapChain();
            m_failed = true;
            QTimer::singleShot(0, this, &EdrSurface::failed);
            return false;
        }
        return true;
    }
    if (m_swapChain->currentPixelSize() != m_swapChain->surfacePixelSize())
        return m_swapChain->createOrResize();
    return true;
}

void EdrSurface::renderFrame() {
    if (!ensureSwapChain())
        return;
    QRhi* rhi = m_canvas->sharedRhi();
    QRhi::FrameOpResult r = rhi->beginFrame(m_swapChain.get());
    if (r == QRhi::FrameOpSwapChainOutOfDate) {
        if (!m_swapChain->createOrResize())
            return;
        r = rhi->beginFrame(m_swapChain.get());
    }
    if (r != QRhi::FrameOpSuccess)
        return;
    m_canvas->renderExtended(m_swapChain->currentFrameCommandBuffer(),
                             m_swapChain->currentFrameRenderTarget());
    rhi->endFrame(m_swapChain.get());
}

bool EdrSurface::event(QEvent* e) {
    switch (e->type()) {
    case QEvent::UpdateRequest:
        m_pending = false;
        renderFrame();
        return true;
    case QEvent::PlatformSurface:
        if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            releaseSwapChain();
        break;
    default:
        break;
    }
    return QWindow::event(e);
}

void EdrSurface::exposeEvent(QExposeEvent*) {
    if (isExposed())
        renderFrame();
}

} // namespace lf
