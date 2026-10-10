// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Magnifier ("Lupe", F-VIEW-11): while `L` (Action::Magnifier) or - with the input choice
// "Lupe" - the middle mouse button is held, a round loupe at the cursor shows the image at
// 100 % or 200 % (Settings key::LoupeZoom, size key::LoupeSize). It is drawn over the
// image view from the best stage the window has; when that stage is too coarse for the
// loupe, the full resolution is requested like for zooming in (the loupe follows when it
// arrives). Geometry: core/LoupeGeometry.h.

#include "core/LoupeGeometry.h"

#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QWidget>

namespace lf {

class ImageView;
class ViewerWindow;

class LoupeWidget : public QWidget {
    Q_OBJECT
public:
    LoupeWidget(ImageView* view, QWidget* parent);
    void setCursorPos(QPointF pos);
    QPointF cursorPos() const { return m_pos; }
    loupe::Geometry loupeGeometry() const; // of the current state
    double magnification() const;
    int diameter() const;
    bool fullResolutionWanted() const; // the shown stage is too coarse (tests)

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPointer<ImageView> m_view;
    QPointF m_pos;
};

class LoupeController : public QObject {
    Q_OBJECT
public:
    explicit LoupeController(ViewerWindow& window);
    ~LoupeController() override;

    bool isActive() const { return m_active; }
    // Action::Magnifier at `pos` (view coordinates; negative = cursor position). The loupe
    // stays until the key or the middle mouse button that started it is released.
    void begin(QPointF pos);
    void end();
    LoupeWidget* widget() const { return m_widget; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void follow(QPointF pos);
    void requestFullIfNeeded();

    ViewerWindow& m_window;
    QPointer<LoupeWidget> m_widget;
    bool m_active = false;
    bool m_fullRequested = false;
};

} // namespace lf
