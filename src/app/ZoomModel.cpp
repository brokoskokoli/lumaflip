// SPDX-License-Identifier: GPL-3.0-or-later
#include "ZoomModel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lf {

namespace {
constexpr double kEps = 1e-9;
constexpr double kEdgeTolerance = 0.5; // device px

constexpr std::array<double, 25> kLadder = {
    0.01, 0.02, 0.03, 0.05, 1.0 / 16, 1.0 / 12, 0.125, 1.0 / 6, 0.25, 1.0 / 3, 0.5,  2.0 / 3, 1.0,
    1.5,  2.0,  3.0,  4.0,  6.0,      8.0,      12.0,  16.0,    24.0, 32.0,    48.0, 64.0};
} // namespace

ZoomModel::ZoomModel() = default;

void ZoomModel::setViewportSize(QSize devicePixels) {
    devicePixels = devicePixels.expandedTo(QSize(1, 1));
    if (devicePixels == m_viewport)
        return;
    const QPointF c = centerInDisplay();
    m_viewport = devicePixels;
    if (m_mode != ZoomMode::Custom)
        m_scale = scaleForMode(m_mode);
    setCenterInDisplay(c);
}

void ZoomModel::setDevicePixelRatio(double dpr) {
    if (dpr <= 0 || std::abs(dpr - m_dpr) < kEps)
        return;
    m_dpr = dpr;
    if (m_mode != ZoomMode::Custom) {
        const QPointF c = centerInDisplay();
        m_scale = scaleForMode(m_mode);
        setCenterInDisplay(c);
    }
}

void ZoomModel::setActualSizeInDevicePixels(bool devicePixels) {
    if (devicePixels == m_actualDevicePixels)
        return;
    m_actualDevicePixels = devicePixels;
    if (m_mode != ZoomMode::Custom) {
        const QPointF c = centerInDisplay();
        m_scale = scaleForMode(m_mode);
        setCenterInDisplay(c);
    }
}

void ZoomModel::setImageSize(QSize fullSize) {
    const QSizeF oldDisplay = displayedImageSize();
    const QPointF oldCenter = centerInDisplay();
    const bool keep = m_locked && !m_image.isEmpty() && !oldDisplay.isEmpty();
    QPointF relCenter(0.5, 0.5);
    if (keep)
        relCenter = QPointF(oldCenter.x() / oldDisplay.width(), oldCenter.y() / oldDisplay.height());
    m_image = fullSize;
    if (m_image.isEmpty()) {
        m_offset = {};
        return;
    }
    if (keep) {
        // F-VIEW-05: same factor (Custom/Actual) or same mode, same relative center.
        if (m_mode != ZoomMode::Custom)
            m_scale = scaleForMode(m_mode);
        const QSizeF d = displayedImageSize();
        setCenterInDisplay(QPointF(relCenter.x() * d.width(), relCenter.y() * d.height()));
        return;
    }
    m_mode = m_baseMode;
    applyMode(false);
}

double ZoomModel::unityScale() const {
    return m_actualDevicePixels ? 1.0 : m_dpr;
}

double ZoomModel::factor() const {
    return m_scale / unityScale();
}

double ZoomModel::scaleForFactor(double factor) const {
    return factor * unityScale();
}

bool ZoomModel::isFitLike(ZoomMode mode) const {
    return mode != ZoomMode::Custom && mode != ZoomMode::Actual;
}

double ZoomModel::scaleForMode(ZoomMode mode) const {
    const QSizeF d = displayedImageSize();
    if (d.isEmpty())
        return unityScale();
    const double sx = m_viewport.width() / d.width();
    const double sy = m_viewport.height() / d.height();
    switch (mode) {
    case ZoomMode::Fit:
        return std::min({sx, sy, unityScale()});
    case ZoomMode::FitUpscale:
        return std::min(sx, sy);
    case ZoomMode::Actual:
        return unityScale();
    case ZoomMode::FillWidth:
        return sx;
    case ZoomMode::FillHeight:
        return sy;
    case ZoomMode::Fill:
        return std::max(sx, sy);
    case ZoomMode::Custom:
        break;
    }
    return m_scale;
}

double ZoomModel::minScale() const {
    return std::max(1e-4, std::min(scaleForMode(ZoomMode::FitUpscale), unityScale()) * 0.5);
}

double ZoomModel::maxScale() const {
    return std::max(unityScale() * kLadder.back(), scaleForMode(ZoomMode::FitUpscale));
}

void ZoomModel::setMode(ZoomMode mode) {
    m_mode = mode;
    if (mode != ZoomMode::Custom)
        m_baseMode = mode;
    applyMode(false);
}

void ZoomModel::resetToBaseMode() {
    m_mode = m_baseMode;
    applyMode(false);
}

void ZoomModel::restoreView(ZoomMode mode, double factor, QPointF imageCenter) {
    if (m_image.isEmpty())
        return;
    m_mode = mode;
    m_scale = mode == ZoomMode::Custom ? std::clamp(scaleForFactor(factor), minScale(), maxScale())
                                       : scaleForMode(mode);
    setCenterInDisplay(imageToDisplay(imageCenter));
}

void ZoomModel::applyMode(bool keepRelativeCenter) {
    if (m_image.isEmpty())
        return;
    const QPointF c = centerInDisplay();
    if (m_mode != ZoomMode::Custom)
        m_scale = scaleForMode(m_mode);
    const QSizeF d = displayedImageSize();
    if (keepRelativeCenter) {
        setCenterInDisplay(c);
        return;
    }
    // New placement: centered; "fill width" of a tall image starts at the top,
    // "fill height" of a wide image starts at the left.
    const double w = d.width() * m_scale, h = d.height() * m_scale;
    m_offset = QPointF((m_viewport.width() - w) / 2.0, (m_viewport.height() - h) / 2.0);
    if (m_mode == ZoomMode::FillWidth && h > m_viewport.height())
        m_offset.setY(0);
    if (m_mode == ZoomMode::FillHeight && w > m_viewport.width())
        m_offset.setX(0);
    clampOffset();
}

void ZoomModel::setScale(double scale, QPointF anchor) {
    if (m_image.isEmpty())
        return;
    scale = std::clamp(scale, minScale(), maxScale());
    const QPointF p = (anchor - m_offset) / m_scale; // display point under anchor
    m_scale = scale;
    m_mode = ZoomMode::Custom;
    m_offset = anchor - p * m_scale;
    clampOffset();
}

double ZoomModel::nextLadderFactor(double factor, int direction) {
    if (direction > 0) {
        for (double f : kLadder)
            if (f > factor * (1 + 1e-3))
                return f;
        return kLadder.back();
    }
    for (auto it = kLadder.rbegin(); it != kLadder.rend(); ++it)
        if (*it < factor * (1 - 1e-3))
            return *it;
    return kLadder.front();
}

void ZoomModel::zoomStep(int steps, QPointF anchor) {
    if (steps == 0 || m_image.isEmpty())
        return;
    double f = factor();
    for (int i = 0; i < std::abs(steps); ++i)
        f = nextLadderFactor(f, steps > 0 ? 1 : -1);
    const bool isUnity = std::abs(f - 1.0) < kEps;
    setScale(scaleForFactor(f), anchor);
    if (isUnity)
        m_mode = ZoomMode::Actual;
}

void ZoomModel::toggleFitActual(QPointF anchor) {
    if (m_image.isEmpty())
        return;
    const bool atActual =
        m_mode == ZoomMode::Actual || std::abs(m_scale - unityScale()) < 1e-6 * unityScale();
    if (atActual) {
        m_mode = ZoomMode::Fit;
        applyMode(false);
        return;
    }
    setScale(unityScale(), anchor);
    m_mode = ZoomMode::Actual;
}

void ZoomModel::panBy(QPointF delta) {
    m_offset += delta;
    clampOffset();
}

void ZoomModel::scrollToTop() {
    m_offset.setY(0);
    clampOffset();
}

void ZoomModel::scrollToBottom() {
    m_offset.setY(m_viewport.height() - displayedImageSize().height() * m_scale);
    clampOffset();
}

void ZoomModel::setOffset(QPointF offset) {
    m_offset = offset;
    clampOffset();
}

void ZoomModel::clampOffset() {
    const QSizeF d = displayedImageSize();
    auto clampAxis = [](double off, double size, double view) {
        if (size <= view + kEps)
            return (view - size) / 2.0;
        return std::clamp(off, view - size, 0.0);
    };
    m_offset.setX(clampAxis(m_offset.x(), d.width() * m_scale, m_viewport.width()));
    m_offset.setY(clampAxis(m_offset.y(), d.height() * m_scale, m_viewport.height()));
}

bool ZoomModel::isZoomed() const {
    const QSizeF d = displayedImageSize();
    return d.width() * m_scale > m_viewport.width() + kEdgeTolerance ||
           d.height() * m_scale > m_viewport.height() + kEdgeTolerance;
}

bool ZoomModel::atEdge(Qt::Edge edge) const {
    const QRectF r = imageRectInViewport();
    switch (edge) {
    case Qt::LeftEdge:
        return r.left() >= -kEdgeTolerance;
    case Qt::RightEdge:
        return r.right() <= m_viewport.width() + kEdgeTolerance;
    case Qt::TopEdge:
        return r.top() >= -kEdgeTolerance;
    case Qt::BottomEdge:
        return r.bottom() <= m_viewport.height() + kEdgeTolerance;
    }
    return true;
}

void ZoomModel::setRotation(int quarterTurnsClockwise) {
    const int q = ((quarterTurnsClockwise % 4) + 4) % 4;
    if (q == m_rotation)
        return;
    const QPointF c = viewportToImage(QPointF(m_viewport.width() / 2.0, m_viewport.height() / 2.0));
    m_rotation = q;
    if (m_mode != ZoomMode::Custom)
        m_scale = scaleForMode(m_mode);
    setCenterInDisplay(imageToDisplay(c));
}

void ZoomModel::setFlip(bool horizontal, bool vertical) {
    if (horizontal == m_flipH && vertical == m_flipV)
        return;
    const QPointF c = viewportToImage(QPointF(m_viewport.width() / 2.0, m_viewport.height() / 2.0));
    m_flipH = horizontal;
    m_flipV = vertical;
    setCenterInDisplay(imageToDisplay(c));
}

QSizeF ZoomModel::displayedImageSize() const {
    if (m_image.isEmpty())
        return {};
    return (m_rotation % 2) ? QSizeF(m_image.height(), m_image.width()) : QSizeF(m_image);
}

QPointF ZoomModel::snappedOffset() const {
    // At integral scales (100 %, 200 % ...) align image pixels with device pixels.
    if (std::abs(m_scale - std::round(m_scale)) < 1e-6 && m_scale >= 1.0)
        return QPointF(std::round(m_offset.x()), std::round(m_offset.y()));
    return m_offset;
}

QRectF ZoomModel::imageRectInViewport() const {
    const QSizeF d = displayedImageSize();
    return QRectF(snappedOffset(), d * m_scale);
}

QPointF ZoomModel::imageToDisplay(QPointF p) const {
    const double W = m_image.width(), H = m_image.height();
    QPointF d;
    switch (m_rotation) {
    case 1:
        d = QPointF(H - p.y(), p.x());
        break;
    case 2:
        d = QPointF(W - p.x(), H - p.y());
        break;
    case 3:
        d = QPointF(p.y(), W - p.x());
        break;
    default:
        d = p;
        break;
    }
    const QSizeF ds = displayedImageSize();
    if (m_flipH)
        d.setX(ds.width() - d.x());
    if (m_flipV)
        d.setY(ds.height() - d.y());
    return d;
}

QTransform ZoomModel::imageToViewport() const {
    // Affine map from image to display coords, derived from the images of the
    // origin and the unit vectors.
    const QPointF o = imageToDisplay(QPointF(0, 0));
    const QPointF ex = imageToDisplay(QPointF(1, 0)) - o;
    const QPointF ey = imageToDisplay(QPointF(0, 1)) - o;
    const QPointF off = snappedOffset();
    const double s = m_scale;
    // QTransform: x' = m11*x + m21*y + dx ; y' = m12*x + m22*y + dy
    return QTransform(ex.x() * s, ex.y() * s, ey.x() * s, ey.y() * s, o.x() * s + off.x(),
                      o.y() * s + off.y());
}

QPointF ZoomModel::viewportToImage(QPointF viewportPoint) const {
    bool invertible = false;
    const QTransform inv = imageToViewport().inverted(&invertible);
    return invertible ? inv.map(viewportPoint) : QPointF();
}

QPointF ZoomModel::centerInDisplay() const {
    if (m_scale <= 0)
        return {};
    return (QPointF(m_viewport.width() / 2.0, m_viewport.height() / 2.0) - m_offset) / m_scale;
}

void ZoomModel::setCenterInDisplay(QPointF displayPoint) {
    m_offset = QPointF(m_viewport.width() / 2.0, m_viewport.height() / 2.0) - displayPoint * m_scale;
    clampOffset();
}

QSize ZoomModel::desiredDecodeSize() const {
    if (m_image.isEmpty())
        return m_viewport;
    const double f = std::min(m_scale, 1.0);
    return QSize(std::max(1, int(std::ceil(m_image.width() * f - 1e-6))),
                 std::max(1, int(std::ceil(m_image.height() * f - 1e-6))));
}

bool ZoomModel::needsHigherResolution(QSize loadedSize) const {
    if (m_image.isEmpty() || loadedSize.isEmpty() || loadedSize.width() >= m_image.width())
        return false;
    const double devicePxPerLoadedPx = m_scale * m_image.width() / double(loadedSize.width());
    return devicePxPerLoadedPx > 1.0 + 1e-3;
}

} // namespace lf
