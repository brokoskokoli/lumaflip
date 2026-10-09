// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GPU image canvas (SPEC 4.3, 8.4): zoom, pan, rotation, high quality scaling,
// color managed output, EDR on macOS (M3). Implemented with Qt RHI (Metal on macOS).
// The view handles pan/pinch/drag itself and forwards everything else (keys, wheel,
// clicks) as InputEvent to its owner via userInput().

#include "app/ZoomModel.h"  // ZoomMode (moved there so the pure geometry is testable)
#include "core/ImageProvider.h"
#include "core/InputBindings.h"

#include <QColor>
#include <QColorSpace>
#include <QList>
#include <QPointF>
#include <QWidget>

namespace lf {

class ImageView : public QWidget {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);
    ~ImageView() override;

    // Shows an image. If `sameFile` is true (higher quality stage of the current
    // file), zoom/pan are kept and the swap is seamless.
    void setImage(DecodedImagePtr image, bool sameFile);
    void clear();
    // Error card (F-FMT-02). A non-empty `actionText` adds a button that emits
    // errorActionTriggered() (the window uses it for "Im Finder zeigen").
    void showError(const QString& title, const QString& detail, const QString& actionText = {});
    bool isShowingError() const;
    DecodedImagePtr image() const;
    // Keep GPU textures for these images (neighbours from the prefetch window,
    // SPEC 7.1) so that switching to them takes one frame (P-03). Images not in
    // the list (and not current) release their textures.
    void setPreloadImages(const QList<DecodedImagePtr>& images);

    void setZoomMode(ZoomMode mode);
    ZoomMode zoomMode() const;
    double zoomFactor() const;   // 1.0 = 100 % (device pixels or points, see setting)
    void setZoomFactor(double factor, QPointF anchorInWidget);
    void zoomBy(double factor, QPointF anchorInWidget);
    void zoomBy(double factor);  // anchor = center
    // ⌘+ / ⌘− (F-VIEW-02): steps on a zoom ladder that contains 100 %.
    void zoomStep(int steps, QPointF anchorInWidget);
    void zoomStep(int steps);    // anchor = center
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
    // Output color space override (F-COL-01). Invalid (default) = the color space
    // of the window's screen, which is what macOS composites the window in.
    void setOutputColorSpace(const QColorSpace& colorSpace);
    QColorSpace outputColorSpace() const;  // effective

    // Size in device pixels the image should be decoded to for the current zoom.
    QSize desiredDecodeSize() const;

signals:
    void userInput(const lf::InputEvent& event, QPointF positionInWidget);
    void zoomChanged(double factor, lf::ZoomMode mode);
    // Current zoom needs more resolution than the loaded image has (Quality::Full).
    void needsHigherResolution();
    void dragOutRequested();
    void errorActionTriggered();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct Private;
    Private* d;
};

} // namespace lf
