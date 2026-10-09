// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GPU image canvas (SPEC 4.3, 8.4): zoom, pan, rotation, high quality scaling,
// color managed output, EDR on macOS (M3). Implemented with Qt RHI (Metal on macOS).
// The view handles pan/pinch/drag itself and forwards everything else (keys, wheel,
// clicks) as InputEvent to its owner via userInput().

#include "core/ImageProvider.h"
#include "core/InputBindings.h"

#include <QColor>
#include <QPointF>
#include <QWidget>

namespace lf {

enum class ZoomMode { Fit, FitUpscale, Actual, FillWidth, FillHeight, Fill, Custom };

class ImageView : public QWidget {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);
    ~ImageView() override;

    // Shows an image. If `sameFile` is true (higher quality stage of the current
    // file), zoom/pan are kept and the swap is seamless.
    void setImage(DecodedImagePtr image, bool sameFile);
    void clear();
    // Error card (F-FMT-02).
    void showError(const QString& title, const QString& detail);
    DecodedImagePtr image() const;

    void setZoomMode(ZoomMode mode);
    ZoomMode zoomMode() const;
    double zoomFactor() const;   // 1.0 = 100 % (device pixels or points, see setting)
    void setZoomFactor(double factor, QPointF anchorInWidget);
    void zoomBy(double factor, QPointF anchorInWidget);
    void zoomBy(double factor);  // anchor = center
    void toggleFitActual(QPointF anchorInWidget);  // F-VIEW-03
    void setZoomLocked(bool locked);               // F-VIEW-05: keep zoom/pan across images
    bool zoomLocked() const;

    void panBy(QPointF deltaInWidget);
    void scrollToTop();
    void scrollToBottom();
    bool isZoomed() const;               // image larger than viewport
    bool atEdge(Qt::Edge edge) const;    // for "scroll then flip"

    void setRotation(int quarterTurnsClockwise);   // view-only rotation (F-EDIT-01)
    int rotation() const;
    void setFlip(bool horizontal, bool vertical);

    void setBackground(const QString& setting);    // see Settings key::Background
    void setUpscaleNearest(bool nearest);
    void setActualSizeInDevicePixels(bool devicePixels);

    // Size in device pixels the image should be decoded to for the current zoom.
    QSize desiredDecodeSize() const;

signals:
    void userInput(const lf::InputEvent& event, QPointF positionInWidget);
    void zoomChanged(double factor, lf::ZoomMode mode);
    // Current zoom needs more resolution than the loaded image has (Quality::Full).
    void needsHigherResolution();
    void dragOutRequested();

private:
    struct Private;
    Private* d;
};

} // namespace lf
