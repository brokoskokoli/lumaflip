// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Pure view geometry of the image canvas (SPEC 4.3, F-VIEW-01..07, F-EDIT-01).
// No GPU, no widgets: everything is computed in *device pixels* of the viewport
// and *full-resolution image pixels* (DecodedImage::fullSize, i.e. after EXIF
// orientation). Lower resolution stages map onto the same full-size coordinate
// system, so swapping stages never changes the geometry.
//
// Display transform: image -> rotate clockwise by quarter turns -> optional
// mirror in display space -> scale -> translate (offset of the top-left corner
// of the displayed image in the viewport).

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QTransform>

namespace lf {

// Zoom modes of F-VIEW-01. Custom = free factor (⌘+/−, pinch, wheel zoom).
enum class ZoomMode { Fit, FitUpscale, Actual, FillWidth, FillHeight, Fill, Custom };

class ZoomModel {
public:
    ZoomModel();

    // ---- inputs ----
    void setViewportSize(QSize devicePixels);
    QSize viewportSize() const { return m_viewport; }
    void setDevicePixelRatio(double dpr);
    double devicePixelRatio() const { return m_dpr; }
    // 100 % = 1 image pixel per device pixel (true, default) or per point (false).
    void setActualSizeInDevicePixels(bool devicePixels);
    bool actualSizeInDevicePixels() const { return m_actualDevicePixels; }

    // A new image (different file). Applies the current mode; with zoom lock the
    // scale and the relative view center are kept (F-VIEW-05).
    void setImageSize(QSize fullSize);
    QSize imageSize() const { return m_image; }
    void setZoomLocked(bool locked) { m_locked = locked; }
    bool zoomLocked() const { return m_locked; }

    // ---- zoom ----
    void setMode(ZoomMode mode); // re-centers; also becomes the mode for new images
    ZoomMode mode() const { return m_mode; }
    // Scale in device pixels per image pixel.
    double scale() const { return m_scale; }
    // User visible factor: 1.0 = 100 % (device pixels or points, see setting).
    double factor() const;
    double scaleForFactor(double factor) const;
    double scaleForMode(ZoomMode mode) const;
    double minScale() const;
    double maxScale() const;
    // Sets a custom scale keeping the image point under `anchor` (viewport device px) fixed.
    void setScale(double scale, QPointF anchor);
    void setFactor(double factor, QPointF anchor) { setScale(scaleForFactor(factor), anchor); }
    void zoomBy(double multiplier, QPointF anchor) { setScale(m_scale * multiplier, anchor); }
    // ⌘+ / ⌘−: next step on a fixed zoom ladder (contains 100 %).
    void zoomStep(int steps, QPointF anchor);
    static double nextLadderFactor(double factor, int direction);
    // F-VIEW-03: Fit <-> 100 % at `anchor`.
    void toggleFitActual(QPointF anchor);

    // ---- pan ----
    void panBy(QPointF deltaDevicePx); // moves the image by delta (clamped)
    void scrollToTop();
    void scrollToBottom();
    // Offset of the displayed image's top-left in the viewport (device px).
    QPointF offset() const { return m_offset; }
    void setOffset(QPointF offset);   // clamped
    bool isZoomed() const;            // displayed image larger than viewport
    bool atEdge(Qt::Edge edge) const; // that edge of the image is visible

    // ---- rotation / flip (F-EDIT-01) ----
    void setRotation(int quarterTurnsClockwise); // keeps the center point fixed
    int rotation() const { return m_rotation; }
    void setFlip(bool horizontal, bool vertical);
    bool flipHorizontal() const { return m_flipH; }
    bool flipVertical() const { return m_flipV; }

    // ---- derived ----
    QSizeF displayedImageSize() const;  // after rotation, unscaled (image px)
    QRectF imageRectInViewport() const; // device px
    QTransform imageToViewport() const; // full-size image px -> viewport device px
    QPointF viewportToImage(QPointF viewportPoint) const;
    // Size (in oriented image px, not view-rotated) to decode for the current
    // zoom: the on-screen size, never larger than the full size.
    QSize desiredDecodeSize() const;
    // True when the loaded stage (`loadedSize` pixels) is magnified (> 1 device
    // px per loaded pixel) while the full image has more pixels.
    bool needsHigherResolution(QSize loadedSize) const;

private:
    double unityScale() const;
    bool isFitLike(ZoomMode mode) const;
    void applyMode(bool keepRelativeCenter);
    void clampOffset();
    QPointF centerInDisplay() const;                  // viewport center in display coords (unscaled)
    QPointF imageToDisplay(QPointF imagePoint) const; // unscaled, no offset
    QPointF snappedOffset() const;
    void setCenterInDisplay(QPointF displayPoint);

    QSize m_viewport{1, 1};
    QSize m_image;
    double m_dpr = 1.0;
    bool m_actualDevicePixels = true;
    bool m_locked = false;
    ZoomMode m_mode = ZoomMode::Fit;
    ZoomMode m_baseMode = ZoomMode::Fit; // mode applied to new images when not locked
    double m_scale = 1.0;
    QPointF m_offset;
    int m_rotation = 0;
    bool m_flipH = false;
    bool m_flipV = false;
};

} // namespace lf
