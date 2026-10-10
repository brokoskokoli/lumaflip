// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-06 (M4): the VisionKit side of Live Text for one image view (core/LiveText.h has
// the request logic).
//
// The overlay (ImageAnalysisOverlayView, via app/LiveTextBridge.swift) is a native NSView. It
// is added to the window's content view above the image view's area, so AppKit draws it on
// top of the Qt content and it gets the mouse for selecting text / lifting subjects. It is
// not a Qt widget: the EDR surface (EdrSurface.h) composites Qt overlays into its extended
// pass, but a native view above that surface would be outside of it - so while the EDR
// surface presents the image, Live Text is unavailable (LiveTextSession::setBlocked, with
// a message: switching HDR off with ⇧⌘H makes it available).
//
// The image analysed is the shown stage with the view's rotation/flip applied; the overlay's
// contents rectangle follows zoom and pan (ImageView::imageToWidget()).

#include "core/LiveText.h"

#include <QObject>
#include <QPointer>

#include <memory>

namespace lf {

class ImageView;

class LiveTextOverlay : public QObject, public ImageAnalysisBackend {
    Q_OBJECT
public:
    explicit LiveTextOverlay(ImageView* view, QObject* parent = nullptr);
    ~LiveTextOverlay() override;

    // VisionKit available (built with the Swift bridge, ImageAnalyzer.isSupported).
    static bool isAvailable();

    bool isSupported() const override;
    void analyze(quint64 token, const QImage& image) override;
    void cancel() override;

    // Session to report results to (set by the owner).
    void setSession(LiveTextSession* session) { m_session = session; }
    // Shows/hides the native overlay and places it over the image view.
    void setVisible(bool visible);
    void updateGeometry();

private:
    struct Native;
    std::unique_ptr<Native> m_native;
    QPointer<ImageView> m_view;
    LiveTextSession* m_session = nullptr;
    bool m_visible = false;
};

} // namespace lf
