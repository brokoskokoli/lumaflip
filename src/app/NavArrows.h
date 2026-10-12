// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-NAV-09: round previous/next buttons at the left and right edge of the image, like the
// arrows of common gallery viewers.
//
// Hidden at rest; a mouse move over the image fades them in, after kIdleMs without a move
// they fade out again (the same timing as the title strip F-VIEW-15 and the full screen
// chrome F-VIEW-10, revealed by the same mouse moves) - not while the mouse is over one of
// them or a button is held. A button only appears when its direction has a target (the
// first/last image without wrap has none, core/NavArrows.h) and never in the grid, compare,
// crop, slideshow, with the loupe or Live Text (a native view above the image that takes the
// mouse). A click runs the same action as ←/→ (right-to-left comics swap them); the click
// and a double click stay with the button (no pan, no full screen). No keyboard focus.
//
// The buttons are children of the image view: they are drawn above the canvas, and the EDR
// surface grabs them into its extended pass like the toasts (EdrSurface.h).

#include "core/InputBindings.h"

#include <QAbstractButton>
#include <QPointer>

class QTimer;
class QVariantAnimation;

namespace lf {

class ViewerWindow;

class NavArrowButton : public QAbstractButton {
    Q_OBJECT
public:
    enum class Side { Left, Right };
    NavArrowButton(Side side, QWidget* parent);

    Side side() const { return m_side; }
    // Fades in (true) or out; `ms` 0 = at once. Hidden when faded out.
    void fadeTo(bool shown, int ms);
    bool shownTarget() const { return m_target; }
    qreal opacity() const { return m_opacity; }
    // Fully faded in: only then the button takes the mouse (a click right after the first
    // mouse move still reaches the image, like the title strip).
    bool takesMouse() const { return isVisible() && m_target && m_opacity >= 1.0; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& pos) const override;
    bool event(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void setOpacity(qreal value);

    Side m_side;
    bool m_target = false;
    qreal m_opacity = 0.0;
    QVariantAnimation* m_fade = nullptr;
};

class NavArrows : public QObject {
    Q_OBJECT
public:
    static constexpr int kIdleMs = 1500; // like the title strip and the full screen chrome
    static constexpr int kFadeInMs = 150;
    static constexpr int kFadeOutMs = 350;
    static constexpr int kDiameter = 44; // the circle (hit target at least this)
    static constexpr int kEdgeMargin = 12;

    explicit NavArrows(ViewerWindow& window);

    NavArrowButton* leftButton() const { return m_left; }
    NavArrowButton* rightButton() const { return m_right; }
    Action leftAction() const { return m_leftAction; }
    Action rightAction() const { return m_rightAction; }

    void reveal(); // fade in what has a target, fade out after kIdleMs without a mouse move
    // The window's state changed (image, mode, setting): buttons without a target or in a
    // mode without buttons go at once.
    void sync();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool allowedNow() const;
    void place();
    void onIdle();
    void activate(NavArrowButton* button);
    void retranslate();

    ViewerWindow& m_window;
    NavArrowButton* m_left = nullptr;
    NavArrowButton* m_right = nullptr;
    QTimer* m_idle = nullptr;
    Action m_leftAction = Action::PreviousImage;
    Action m_rightAction = Action::NextImage;
    bool m_leftWanted = false;
    bool m_rightWanted = false;
};

} // namespace lf
